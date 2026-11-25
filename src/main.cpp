// Human Detector: a 24 GHz radar on the ESP32-2432S028R "Cheap Yellow Display".
//   Ai-Thinker RD-03D or HLK-LD2450: up to 3 people, each shown where they are, with trails.
//   HLK-LD2410B: one person's distance (it can't tell direction).
// The firmware works out which one is connected.
//
// A radar-style screen: a green fan showing the radar's field of view, an expanding ping, and red
// dots for the people it detects. Beeps when someone arrives and logs arrivals/departures to the
// SD card.
//
//   Tap screen          LED on the back: off > red > green > blue > yellow     (remembered)
//   Double-tap screen   RD-03D: trails on / off. LD2410B: "moving only" mode   (remembered)
//   Hold screen         brightness 100/50/20%                                  (remembered)
//   BOOT button         fix inverted colors                                    (remembered)
//   Hold BOOT 2 s       LD2410B: learn the room: ignore what's in view now     (remembered)
//   SD card             detector/config.txt settings, detector/log.csv log (optional)
#include <Arduino.h>
#include <Preferences.h>
#include <XPT2046_Bitbang.h>

#include "detector.h"
#include "pins.h"
#include "radar.h"
#include "screen.h"
#include "storage.h"

namespace {

// When this firmware was compiled, e.g. "Firmware built Nov 22 2026 20:31": shown on the start-up
// screen and in the Serial Monitor, so it's easy to see which version is on the board.
const char kBuildLine[] = "Firmware built " __DATE__ " " __TIME__;

Settings gSettings;
Preferences gPrefs;
PresenceTracker gTracker;
XPT2046_Bitbang gTouch(Pin::TouchMosi, Pin::TouchMiso, Pin::TouchClk, Pin::TouchCs);

const uint8_t kBrightness[] = {255, 128, 50};
constexpr uint16_t kTouchPressure = 300;  // XPT2046 pressure reading that counts as a touch

int gLedChoice = 0;        // the LED's color: 0 off, then red, green, blue, yellow
bool gInvert = false;
bool gMovingOnly = false;  // LD2410B: ignore still targets (metal and furniture never move)
bool gTrails = true;       // RD-03D / LD2450: trails behind people
uint8_t gBrightnessIndex = 0;
Background gBackground;    // learned with "learn the room"

DistanceFilter gFilter;
uint32_t gLastFrames = 0;
uint16_t gDistanceCm = 0;  // smoothed distance to the person (0 = nobody)
Targets gTargets;          // LD2410B
TrackingReading gTracks;   // RD-03D / LD2450 (mirrored if config.txt says so)

// A short message over the fan after a tap.
const char* gNoticeTitle = nullptr;
const char* gNoticeLine = nullptr;
uint32_t gNoticeUntilMs = 0;

void notice(const char* title, const char* line, uint32_t now) {
  gNoticeTitle = title;
  gNoticeLine = line;
  gNoticeUntilMs = now + 2500;
}

// ------------------------------------------------------------------ LED + beeps

// The RGB LED on the back: three LEDs (red IO4, green IO16, blue IO17) that light when their pin
// is LOW (HIGH with led_invert = on). A lit one is dimmed with PWM, so together they can mix
// colors; one that's off is held at a steady level instead, since PWM can leave a faint glow.
const int kLedPins[3] = {Pin::LedRed, Pin::LedGreen, Pin::LedBlue};
bool gLedPwm[3] = {false, false, false};
uint32_t gLedShown = 0xFFFFFFFF;  // the color showing now (none yet)

void setLedChannel(int i, uint8_t level) {
  const int pin = kLedPins[i];
  const bool lowIsOn = !gSettings.ledInvert;
  if (level == 0) {
    if (gLedPwm[i]) {
      ledcDetach(pin);
      gLedPwm[i] = false;
    }
    pinMode(pin, OUTPUT);
    digitalWrite(pin, lowIsOn ? HIGH : LOW);
  } else {
    if (!gLedPwm[i]) {
      ledcAttach(pin, 5000, 8);
      gLedPwm[i] = true;
    }
    ledcWrite(pin, lowIsOn ? 255 - level : level);
  }
}

void showLed(uint32_t rgb) {
  if (rgb == gLedShown) return;
  gLedShown = rgb;
  uint8_t level[3];
  ledLevels(rgb, gSettings.ledBrightness, level[0], level[1], level[2]);
  for (int i = 0; i < 3; i++) setLedChannel(i, level[i]);
}

void beginLed() {
  gLedShown = 0xFFFFFFFF;  // (again after config.txt: led_invert may have changed)
  showLed(0x000000);
}

int gBeepsLeft = 0;
uint32_t gNextBeepMs = 0;

void startBeeps() {
  if (gSettings.beepCount == 0) return;
  gBeepsLeft = gSettings.beepCount;
  gNextBeepMs = millis();
}

void serviceBeeps(uint32_t now) {
  if (gBeepsLeft <= 0 || (int32_t)(now - gNextBeepMs) < 0) return;
  tone(Pin::Speaker, gSettings.beepHz, 110);
  gBeepsLeft--;
  gNextBeepMs = now + 230;
}

// ------------------------------------------------------------------ learn the room
//
// 5 s to step away, 10 s measuring what the radar sees per distance (engineering mode), then the
// trigger levels go into the radar and the background is kept here too (some radars won't accept
// every level).

constexpr uint32_t kCountdownMs = 5000, kMeasureMs = 10000, kResultMs = 8000;
constexpr uint32_t kMinSamples = 20;  // the radar sends ~10 reports a second

struct Learning {
  bool active = false;
  view::Learn::Phase phase = view::Learn::Phase::Countdown;
  uint32_t phaseStartMs = 0;
  Background bg;
  uint32_t samples = 0;
  uint32_t lastFrames = 0;
  bool engineeringOn = false;
  bool ok = false;
  bool forgot = false;
  int gatesSaved = 0;
} gLearn;

void saveBackground() {
  gPrefs.putBool("bgok", gBackground.learned);
  gPrefs.putBytes("bgmov", gBackground.moving, kMaxGates);
  gPrefs.putBytes("bgstill", gBackground.still, kMaxGates);
}

void loadBackground() {
  gBackground.learned = gPrefs.getBool("bgok", false);
  if (!gBackground.learned) return;
  if (gPrefs.getBytes("bgmov", gBackground.moving, kMaxGates) != kMaxGates ||
      gPrefs.getBytes("bgstill", gBackground.still, kMaxGates) != kMaxGates) {
    gBackground = Background();
  }
}

// Time in the current phase. Signed, because the radar steps keep the board busy for a second or
// two, so a "now" read before them can be earlier than the phase start.
uint32_t phaseElapsed(uint32_t now) {
  int32_t elapsed = (int32_t)(now - gLearn.phaseStartMs);
  return elapsed > 0 ? (uint32_t)elapsed : 0;
}

view::Learn learnView(uint32_t now) {
  view::Learn v;
  v.phase = gLearn.phase;
  v.nowMs = now;
  v.ok = gLearn.ok;
  v.forgot = gLearn.forgot;
  v.gatesSaved = gLearn.gatesSaved;
  v.feet = gSettings.useFeet;
  uint32_t elapsed = phaseElapsed(now);
  if (gLearn.phase == view::Learn::Phase::Countdown) {
    v.secondsLeft = (int)((kCountdownMs - (elapsed < kCountdownMs ? elapsed : kCountdownMs) + 999) / 1000);
  } else if (gLearn.phase == view::Learn::Phase::Measuring) {
    uint32_t left = elapsed < kMeasureMs ? kMeasureMs - elapsed : 0;
    v.secondsLeft = (int)((left + 999) / 1000);
    v.progress = elapsed / (float)kMeasureMs;
  }
  return v;
}

void enterPhase(view::Learn::Phase phase) {
  gLearn.phase = phase;
  gLearn.phaseStartMs = millis();
}

// Shows "Saving..." before a step that keeps the board busy for a second or two.
void showSaving() {
  enterPhase(view::Learn::Phase::Saving);
  screen::showLearn(learnView(millis()), true);
}

void startLearning() {
  gLearn = Learning();
  gLearn.active = true;
  gBeepsLeft = 0;
  enterPhase(view::Learn::Phase::Countdown);
  Serial.println("Learn the room: step out of view");
}

void stopLearning() {
  if (gLearn.engineeringOn) radar::setEngineering(false);
  gLearn.active = false;
}

void finishMeasuring() {
  showSaving();
  if (gLearn.engineeringOn) {
    radar::setEngineering(false);
    gLearn.engineeringOn = false;
  }
  gLearn.ok = gLearn.samples >= kMinSamples;
  if (gLearn.ok) {
    gLearn.bg.learned = true;
    uint8_t moving[kMaxGates], still[kMaxGates];
    learnedSensitivity(gLearn.bg, moving, still);
    gLearn.gatesSaved = radar::writeSensitivity(moving, still);
    gBackground = gLearn.bg;
    saveBackground();
  }
  Serial.printf("Learn the room: %lu reports, %s, %d of 9 gates saved in the radar\n", (unsigned long)gLearn.samples,
                gLearn.ok ? "learned" : "FAILED (no per-distance data from the radar)", gLearn.gatesSaved);
  enterPhase(view::Learn::Phase::Done);
}

// Back to the radar's factory trigger levels, and nothing filtered here.
void forgetRoom() {
  showSaving();
  if (gLearn.engineeringOn) {
    radar::setEngineering(false);
    gLearn.engineeringOn = false;
  }
  Background none;
  uint8_t moving[kMaxGates], still[kMaxGates];
  learnedSensitivity(none, moving, still);  // = factory levels
  gLearn.gatesSaved = radar::writeSensitivity(moving, still);
  gBackground = Background();
  saveBackground();
  gLearn.forgot = true;
  gLearn.ok = true;
  Serial.println("Learn the room: forgotten, factory levels restored");
  enterPhase(view::Learn::Phase::Done);
}

void serviceLearning(uint32_t now) {
  uint32_t elapsed = phaseElapsed(now);
  switch (gLearn.phase) {
    case view::Learn::Phase::Countdown:
      if (elapsed >= kCountdownMs) {
        enterPhase(view::Learn::Phase::Measuring);
        screen::showLearn(learnView(millis()), true);         // before the radar switch (~1 s)
        gLearn.engineeringOn = radar::setEngineering(true);  // per-distance energy
        gLearn.lastFrames = radar::frameCount();
        enterPhase(view::Learn::Phase::Measuring);           // the 10 s start now
      }
      break;
    case view::Learn::Phase::Measuring: {
      uint32_t frames = radar::frameCount();
      if (frames != gLearn.lastFrames) {
        gLearn.lastFrames = frames;
        const RadarReading& r = radar::latest();
        if (r.engineering) {
          addToBackground(gLearn.bg, r);
          gLearn.samples++;
        }
      }
      if (elapsed >= kMeasureMs) finishMeasuring();
      break;
    }
    case view::Learn::Phase::Saving:
      break;
    case view::Learn::Phase::Done:
      if (elapsed >= kResultMs) gLearn.active = false;
      break;
  }
  if (gLearn.active) screen::showLearn(learnView(millis()), false);
}

// ------------------------------------------------------------------ touch + BOOT button

bool gTouchDown = false;
bool gHoldHandled = false;
uint32_t gTouchStartMs = 0;
uint32_t gLastTouchPollMs = 0;
bool gTapPending = false;  // waiting to see if a second tap follows
uint32_t gTapReleaseMs = 0;
constexpr uint32_t kDoubleTapMs = 400;

void onTap(uint32_t now) {
  if (gLearn.active) {
    if (gLearn.phase == view::Learn::Phase::Countdown || gLearn.phase == view::Learn::Phase::Measuring ||
        gLearn.phase == view::Learn::Phase::Done) {
      stopLearning();
    }
    return;
  }
  // Next LED color
  static const char* const kTitles[kLedChoices] = {"LED OFF", "LED RED", "LED GREEN", "LED BLUE", "LED YELLOW"};
  gLedChoice = (gLedChoice + 1) % kLedChoices;
  gPrefs.putUChar("led", (uint8_t)gLedChoice);
  notice(kTitles[gLedChoice], "Tap again for the next color", now);
}

void onDoubleTap(uint32_t now) {
  if (gLearn.active) {
    if (gLearn.phase == view::Learn::Phase::Countdown) forgetRoom();
    return;
  }
  if (radar::kind() == RadarKind::Tracking) {
    gTrails = !gTrails;
    gPrefs.putBool("trails", gTrails);
    notice(gTrails ? "TRAILS ON" : "TRAILS OFF", nullptr, now);
    return;
  }
  gMovingOnly = !gMovingOnly;
  gPrefs.putBool("moving", gMovingOnly);
  if (gMovingOnly) {
    notice("MOVING ONLY", "Still things are ignored", now);
  } else {
    notice("ALL TARGETS", "Still people count again", now);
  }
}

void handleTouch(uint32_t now) {
  if (gTapPending && now - gTapReleaseMs > kDoubleTapMs) {
    gTapPending = false;
    onTap(now);
  }
  if (now - gLastTouchPollMs < 25) return;  // bit-banged reads take ~1 ms, so poll gently
  gLastTouchPollMs = now;

  bool pressed = gTouch.getTouch().zRaw >= kTouchPressure;
  if (pressed && !gTouchDown) {
    gTouchDown = true;
    gHoldHandled = false;
    gTouchStartMs = now;
  } else if (pressed && gTouchDown) {
    if (!gHoldHandled && !gLearn.active && now - gTouchStartMs >= 800) {  // hold: next brightness level
      gHoldHandled = true;
      gTapPending = false;
      gBrightnessIndex = (gBrightnessIndex + 1) % sizeof(kBrightness);
      screen::setBrightness(kBrightness[gBrightnessIndex]);
      gPrefs.putUChar("bright", gBrightnessIndex);
    }
  } else if (!pressed && gTouchDown) {
    gTouchDown = false;
    if (!gHoldHandled && now - gTouchStartMs >= 40) {
      if (gTapPending && now - gTapReleaseMs <= kDoubleTapMs) {
        gTapPending = false;
        onDoubleTap(now);
      } else {
        gTapPending = true;
        gTapReleaseMs = now;
      }
    }
  }
}

bool gBootDown = false;
bool gBootHandled = false;
uint32_t gBootChangedMs = 0;

void handleBootButton(uint32_t now) {
  bool down = digitalRead(Pin::BootButton) == LOW;
  if (down && gBootDown && !gBootHandled && now - gBootChangedMs >= 2000) {  // held 2 s
    gBootHandled = true;
    if (radar::kind() == RadarKind::Tracking) {
      notice("NOT NEEDED", "Learn the room is for the LD2410B", now);
    } else if (!gLearn.active) {
      startLearning();
    }
  }
  if (down == gBootDown || now - gBootChangedMs < 50) return;  // debounce
  uint32_t heldMs = now - gBootChangedMs;
  gBootDown = down;
  gBootChangedMs = now;
  if (down) {
    gBootHandled = false;
  } else if (!gBootHandled && heldMs < 1000) {  // short press: fix inverted colors
    gInvert = !gInvert;
    screen::setInvert(gInvert);
    gPrefs.putBool("invert", gInvert);
  }
}

// ------------------------------------------------------------------ presence

void logEvent(PresenceTracker::Event event, uint32_t now, uint8_t state, uint16_t distanceCm) {
  uint32_t duration = event == PresenceTracker::Event::Left ? (gTracker.lastSeenMs() - gTracker.visitStartMs()) / 1000 : 0;
  char row[96];
  formatLogRow(row, sizeof(row), now / 1000, event, distanceCm, gTracker.closestCm(), duration, state);
  Serial.print(row);
  if (gSettings.logToSd) storage::appendLog(row);
}

screen::View buildView(uint32_t now) {
  screen::View v;
  v.radarConnected = radar::connected(now);
  v.kind = radar::kind();
  v.tracks = gTracks;
  v.trails = gTrails;
  v.ledChoice = (uint8_t)gLedChoice;
  switch (storage::status()) {
    case storage::Status::Error: v.sd = view::Sd::Error; break;
    case storage::Status::Ready: v.sd = gSettings.logToSd ? view::Sd::Logging : view::Sd::LogOff; break;
    default: v.sd = view::Sd::NoCard; break;
  }
  v.useFeet = gSettings.useFeet;
  v.alertCm = gSettings.alertCm;
  v.targets = gTargets;
  v.targetCm = gDistanceCm;
  v.movingOnly = gMovingOnly;
  v.roomLearned = gBackground.learned;
  v.present = gTracker.present();
  v.visitSeconds = gTracker.present() ? (now - gTracker.visitStartMs()) / 1000 : 0;
  v.visits = gTracker.visits();
  v.sinceLastSeenS = (now - gTracker.lastSeenMs()) / 1000;
  v.frames = radar::frameCount();
  v.linkDrops = radar::dropCount();
  if ((int32_t)(gNoticeUntilMs - now) > 0) {
    v.noticeTitle = gNoticeTitle;
    v.noticeLine = gNoticeLine;
  }
  return v;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  beginLed();
  pinMode(Pin::BootButton, INPUT_PULLUP);

  gPrefs.begin("detector", false);
  gLedChoice = gPrefs.getUChar("led", 0) % kLedChoices;
  gInvert = gPrefs.getBool("invert", false);
  gMovingOnly = gPrefs.getBool("moving", false);
  gTrails = gPrefs.getBool("trails", true);
  gBrightnessIndex = gPrefs.getUChar("bright", 0) % sizeof(kBrightness);
  loadBackground();

  // SD card first: config.txt can change the screen rotation.
  storage::Status sd = storage::begin();
  int configProblems = (sd == storage::Status::Ready) ? storage::loadConfig(gSettings) : 0;
  beginLed();  // config.txt may say led_invert

  screen::begin(gSettings.rotation, gInvert, kBrightness[gBrightnessIndex]);
  const char* sdLine = "No SD card (optional): not logging";
  screen::Tone sdTone = screen::Tone::Dim;
  if (sd == storage::Status::Error) {
    sdLine = "SD card error: not logging";
    sdTone = screen::Tone::Warning;
  } else if (sd == storage::Status::Ready) {
    sdLine = gSettings.logToSd ? "SD card: logging to detector/log.csv" : "SD card: logging is off in config";
    sdTone = gSettings.logToSd ? screen::Tone::Good : screen::Tone::Dim;
  }
  char configLine[48] = "";
  if (configProblems) {
    snprintf(configLine, sizeof(configLine), "config.txt: %d line%s ignored", configProblems,
             configProblems == 1 ? "" : "s");
  }
  screen::showSplash(sdLine, sdTone, configLine, kBuildLine);

  gTouch.begin();
  gTouch.setCalibration(200, 3700, 240, 3800);  // only pressure is used

  radar::begin(gSettings);  // writes sensor_range / sensor_hold_seconds if config.txt sets them
  Serial.println(kBuildLine);
  Serial.printf("Human detector ready. Alert distance %u cm, clear after %u s. LD2410B: %s, room %s. "
                "RD-03D: trails %s%s\n",
                gSettings.alertCm, gSettings.clearSeconds, gMovingOnly ? "moving only" : "all targets",
                gBackground.learned ? "learned" : "default", gTrails ? "on" : "off",
                gSettings.mirror ? ", mirrored" : "");

  delay(configProblems ? 5000 : 2000);  // show the splash

  screen::redrawAll(buildView(millis()));
}

void loop() {
  uint32_t now = millis();
  radar::poll(now);
  handleTouch(now);
  handleBootButton(now);
  serviceBeeps(now);

  if (gLearn.active) {
    showLed(ledChoiceColor(gLedChoice));
    serviceLearning(millis());
    delay(5);
    return;
  }

  bool connected = radar::connected(now);
  bool tracking = radar::kind() == RadarKind::Tracking;
  uint8_t state = 0;  // for the log: 1 moving, 2 still
  if (tracking) {
    // RD-03D / LD2450: the nearest person counts. Positions are already smooth enough.
    gTargets = Targets();
    gTracks = connected ? radar::latestTracks() : TrackingReading();
    for (Track& t : gTracks.tracks) {
      if (gSettings.mirror) t.xMm = (int16_t)-t.xMm;
      if (!gSettings.stillTracks && !t.moving()) t.valid = false;
    }
    int nearest = gTracks.nearest();
    gDistanceCm = nearest >= 0 ? gTracks.tracks[nearest].distanceCm() : 0;
    if (nearest >= 0) state = gTracks.tracks[nearest].moving() ? 1 : 2;
  } else {
    gTracks = TrackingReading();
    gTargets = connected ? pickTargets(radar::latest(), gMovingOnly, gBackground) : Targets();
    uint32_t frames = radar::frameCount();
    if (!connected) {
      gDistanceCm = gFilter.add(0);
    } else if (frames != gLastFrames) {  // smooth over the radar's reports (~10 a second)
      gLastFrames = frames;
      gDistanceCm = gFilter.add(gTargets.distanceCm());
    }
    state = (gTargets.moving ? 1 : 0) | (gTargets.still ? 2 : 0);
  }
  bool inRange = gDistanceCm > 0 && gDistanceCm <= gSettings.alertCm;

  PresenceTracker::Event event = gTracker.update(inRange, gDistanceCm, now, gSettings.clearSeconds * 1000UL);
  if (event == PresenceTracker::Event::Arrived) startBeeps();
  if (event != PresenceTracker::Event::None) logEvent(event, now, state, gDistanceCm);

  showLed(ledChoiceColor(gLedChoice));  // the color picked by tapping (off by default)

  screen::update(buildView(now));
  delay(5);
}
