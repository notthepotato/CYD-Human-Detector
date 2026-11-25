// Settings, presence tracking and text formatting. Plain C++ (no Arduino headers), unit-tested
// on a computer (host_tests/).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "radar_protocol.h"

// Defaults here; detector/config.txt on the SD card can override them.
struct Settings {
  bool useFeet = true;         // show distances in feet (false = meters)
  uint8_t rotation = 1;        // 1 = landscape, 3 = upside down
  uint16_t alertCm = 600;      // a person counts as "here" within this distance
  uint16_t clearSeconds = 3;   // nobody for this long -> clear again
  uint8_t beepCount = 2;       // beeps when someone arrives (0 = silent)
  uint16_t beepHz = 2600;
  bool logToSd = true;
  bool mirror = false;         // RD-03D / LD2450: swap left and right
  bool stillTracks = true;     // RD-03D / LD2450: show people whose speed is 0
  uint8_t ledBrightness = 100;   // the RGB LED on the back, 0..100 % (its color is picked by tapping)
  bool ledInvert = false;        // LED lights when its pins are HIGH instead of LOW
  // Written into the radar module only when set in config.txt (the module remembers them).
  int8_t maxGate = -1;         // 2..8 (gate = 0.75 m), -1 = leave the module as it is
  int32_t sensorHoldSeconds = -1;  // 0..65535, -1 = leave the module as it is
};

enum class ConfigLine { Blank, Applied, Error };

// The LED colors a tap cycles through: off -> red -> green -> blue -> yellow -> off.
constexpr int kLedChoices = 5;
uint32_t ledChoiceColor(int choice);  // 0xRRGGBB
const char* ledChoiceName(int choice);  // "OFF", "RED", ...

// How bright each of the LED's three colors should be (0..255) for `rgb` at `brightnessPercent`,
// corrected so that half values look half as bright (gamma 2.2).
void ledLevels(uint32_t rgb, uint8_t brightnessPercent, uint8_t& r, uint8_t& g, uint8_t& b);

// Applies one line of config.txt. On error, writes a short reason to `why`.
ConfigLine applyConfigLine(const char* line, Settings& settings, char* why, size_t whyLen);

// The commented config.txt written to a new SD card.
const char* defaultConfigText();

// "6.2 ft", "14 ft", "1.9 m"
void formatDistance(uint16_t cm, bool useFeet, char* out, size_t outLen);

// "Moving", "Standing still", "Moving + still", "Nobody"
const char* motionLabel(uint8_t state);

// Turns raw radar reports into "someone arrived" / "they left" events.
class PresenceTracker {
 public:
  enum class Event { None, Arrived, Left };

  // `inRange`: the radar sees someone within the alert distance right now.
  Event update(bool inRange, uint16_t distanceCm, uint32_t nowMs, uint32_t clearMs);

  bool present() const { return present_; }
  uint32_t visitStartMs() const { return startMs_; }
  uint32_t lastSeenMs() const { return lastSeenMs_; }
  uint16_t closestCm() const { return closestCm_; }  // closest during the current/last visit
  uint32_t visits() const { return visits_; }

 private:
  bool present_ = false;
  uint32_t startMs_ = 0;
  uint32_t lastSeenMs_ = 0;
  uint16_t closestCm_ = 0;
  uint32_t visits_ = 0;
};

// ------------------------------------------------------------------ which targets count

// What the radar sees with nobody in view, per 0.75 m distance gate ("learn the room"): metal,
// furniture, walls. Targets no stronger than this (plus a margin) are ignored.
struct Background {
  bool learned = false;
  uint8_t moving[kMaxGates] = {};
  uint8_t still[kMaxGates] = {};
};

constexpr uint8_t kLearnMargin = 10;  // how much stronger than the background a target must be

// While learning: keeps the strongest energy seen at each gate. Needs engineering-mode reports.
void addToBackground(Background& bg, const RadarReading& reading);

// Trigger levels to write into the radar after learning: background + margin, never below the
// factory setting, at most 100.
void learnedSensitivity(const Background& bg, uint8_t moving[kMaxGates], uint8_t still[kMaxGates]);

// The radar's targets after filtering.
struct Targets {
  bool moving = false;
  uint16_t movingCm = 0;
  bool still = false;
  uint16_t stillCm = 0;
  bool stillIgnored = false;  // the radar reports a still target, but "moving only" mode is on

  // Where the person is: the moving target when there is one (the radar measures movement much more
  // precisely), otherwise the still one. 0 = nobody.
  uint16_t distanceCm() const { return moving ? movingCm : (still ? stillCm : 0); }
  bool any() const { return moving || still; }
};

// `movingOnly`: ignore still targets (metal and furniture never move). `bg`: ignore targets no
// stronger than the learned background at their distance.
Targets pickTargets(const RadarReading& reading, bool movingOnly, const Background& bg);

// Smooths the distance: the middle value of the last 5 reports, so one odd report doesn't make the
// dot jump. Starts over when the target goes away.
class DistanceFilter {
 public:
  uint16_t add(uint16_t cm);  // one report; returns the smoothed distance (0 when cm is 0)

 private:
  static constexpr int kSize = 5;
  uint16_t samples_[kSize] = {};
  int count_ = 0;
  int next_ = 0;
};

// CSV for detector/log.csv. There's no clock on the board, so rows use seconds since power-on.
const char* logHeader();
int formatLogRow(char* out, size_t outLen, uint32_t uptimeSeconds, PresenceTracker::Event event, uint16_t distanceCm,
                 uint16_t closestCm, uint32_t durationSeconds, uint8_t state);
