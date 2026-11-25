#include "detector.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

void copyText(char* dst, size_t dstLen, const char* src) {
  if (dstLen == 0) return;
  size_t n = strlen(src);
  if (n >= dstLen) n = dstLen - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
}

char* trim(char* s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char* end = s + strlen(s);
  while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
  return s;
}

void lower(char* s) {
  for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

bool parseLong(const char* text, long minValue, long maxValue, long& out) {
  errno = 0;
  char* end = nullptr;
  long v = strtol(text, &end, 10);
  if (errno != 0 || end == text) return false;
  while (*end && isspace((unsigned char)*end)) end++;
  if (*end != '\0' || v < minValue || v > maxValue) return false;
  out = v;
  return true;
}

// "10 ft", "3 m", "2.5m", "250 cm" -> centimeters. A unit is required.
bool parseDistanceCm(const char* text, long& cm) {
  errno = 0;
  char* end = nullptr;
  double v = strtod(text, &end);
  if (errno != 0 || end == text || !(v > 0)) return false;
  while (*end && isspace((unsigned char)*end)) end++;
  double result;
  if (strcmp(end, "ft") == 0 || strcmp(end, "feet") == 0) {
    result = v * 30.48;
  } else if (strcmp(end, "m") == 0) {
    result = v * 100.0;
  } else if (strcmp(end, "cm") == 0) {
    result = v;
  } else {
    return false;
  }
  if (result < 30 || result > 800) return false;  // LD2410B: about 6 m, RD-03D: about 8 m
  cm = (long)(result + 0.5);
  return true;
}

}  // namespace

namespace {
// Yellow is red + a little less green: on these LEDs green is the brightest of the three.
const uint32_t kLedColors[kLedChoices] = {0x000000, 0xFF0000, 0x00FF00, 0x0000FF, 0xFFC800};
const char* const kLedNames[kLedChoices] = {"OFF", "RED", "GREEN", "BLUE", "YELLOW"};
}  // namespace

uint32_t ledChoiceColor(int choice) { return kLedColors[(unsigned)choice % kLedChoices]; }

const char* ledChoiceName(int choice) { return kLedNames[(unsigned)choice % kLedChoices]; }

void ledLevels(uint32_t rgb, uint8_t brightnessPercent, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (brightnessPercent > 100) brightnessPercent = 100;
  auto level = [brightnessPercent](uint32_t channel) -> uint8_t {
    double linear = (channel / 255.0) * (brightnessPercent / 100.0);
    return (uint8_t)(pow(linear, 2.2) * 255.0 + 0.5);
  };
  r = level((rgb >> 16) & 0xFF);
  g = level((rgb >> 8) & 0xFF);
  b = level(rgb & 0xFF);
}

// Settings that are "on" or "off".
bool* onOffSetting(const char* key, Settings& s) {
  if (strcmp(key, "log") == 0) return &s.logToSd;
  if (strcmp(key, "mirror") == 0) return &s.mirror;
  if (strcmp(key, "still_targets") == 0) return &s.stillTracks;
  if (strcmp(key, "led_invert") == 0) return &s.ledInvert;
  return nullptr;
}

ConfigLine applyConfigLine(const char* rawLine, Settings& s, char* why, size_t whyLen) {
  char buf[128];
  copyText(buf, sizeof(buf), rawLine);
  char* hash = strchr(buf, '#');
  if (hash) *hash = '\0';
  char* line = trim(buf);
  if (*line == '\0') return ConfigLine::Blank;

  char* eq = strchr(line, '=');
  if (!eq) {
    copyText(why, whyLen, "expected key = value");
    return ConfigLine::Error;
  }
  *eq = '\0';
  char* key = trim(line);
  char* value = trim(eq + 1);
  lower(key);
  lower(value);
  long n = 0;

  if (strcmp(key, "units") == 0) {
    if (strcmp(value, "ft") == 0 || strcmp(value, "feet") == 0) {
      s.useFeet = true;
    } else if (strcmp(value, "m") == 0 || strcmp(value, "meters") == 0) {
      s.useFeet = false;
    } else {
      copyText(why, whyLen, "units must be ft or m");
      return ConfigLine::Error;
    }
  } else if (strcmp(key, "alert_distance") == 0) {
    if (!parseDistanceCm(value, n)) {
      copyText(why, whyLen, "alert_distance: e.g. 10 ft or 3 m (max 8 m)");
      return ConfigLine::Error;
    }
    s.alertCm = (uint16_t)n;
  } else if (strcmp(key, "clear_seconds") == 0) {
    if (!parseLong(value, 1, 600, n)) {
      copyText(why, whyLen, "clear_seconds must be 1-600");
      return ConfigLine::Error;
    }
    s.clearSeconds = (uint16_t)n;
  } else if (strcmp(key, "beeps") == 0) {
    if (!parseLong(value, 0, 10, n)) {
      copyText(why, whyLen, "beeps must be 0-10");
      return ConfigLine::Error;
    }
    s.beepCount = (uint8_t)n;
  } else if (strcmp(key, "beep_hz") == 0) {
    if (!parseLong(value, 200, 8000, n)) {
      copyText(why, whyLen, "beep_hz must be 200-8000");
      return ConfigLine::Error;
    }
    s.beepHz = (uint16_t)n;
  } else if (strcmp(key, "rotation") == 0) {
    if (!parseLong(value, 1, 3, n) || n == 2) {
      copyText(why, whyLen, "rotation must be 1 or 3");
      return ConfigLine::Error;
    }
    s.rotation = (uint8_t)n;
  } else if (bool* flag = onOffSetting(key, s)) {
    bool on;
    if (strcmp(value, "on") == 0 || strcmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
      on = true;
    } else if (strcmp(value, "off") == 0 || strcmp(value, "no") == 0 || strcmp(value, "0") == 0) {
      on = false;
    } else {
      char msg[48];
      snprintf(msg, sizeof(msg), "%s must be on or off", key);
      copyText(why, whyLen, msg);
      return ConfigLine::Error;
    }
    *flag = on;
  } else if (strcmp(key, "led_brightness") == 0) {
    if (!parseLong(value, 0, 100, n)) {
      copyText(why, whyLen, "led_brightness must be 0-100");
      return ConfigLine::Error;
    }
    s.ledBrightness = (uint8_t)n;
  } else if (strcmp(key, "sensor_range") == 0) {
    // Rounded up to the next 0.75 m gate, 2..8 gates (1.5 m .. 6 m)
    if (!parseDistanceCm(value, n)) {
      copyText(why, whyLen, "sensor_range: e.g. 15 ft or 4.5 m (max 6 m)");
      return ConfigLine::Error;
    }
    long gate = (n + 74) / 75;
    if (gate < 2) gate = 2;
    if (gate > 8) gate = 8;
    s.maxGate = (int8_t)gate;
  } else if (strcmp(key, "sensor_hold_seconds") == 0) {
    if (!parseLong(value, 0, 65535, n)) {
      copyText(why, whyLen, "sensor_hold_seconds must be 0-65535");
      return ConfigLine::Error;
    }
    s.sensorHoldSeconds = (int32_t)n;
  } else {
    copyText(why, whyLen, "unknown setting");
    return ConfigLine::Error;
  }
  return ConfigLine::Applied;
}

const char* defaultConfigText() {
  return "# Human detector settings. Edit on your computer, put the card back in, then\n"
         "# restart the board (press RST or unplug it). Lines starting with # are ignored.\n"
         "\n"
         "# Show distances in ft or m.\n"
         "units = ft\n"
         "\n"
         "# Someone counts as \"here\" within this distance. The LD2410B sees about 20 ft (6 m),\n"
         "# the RD-03D and LD2450 about 26 ft (8 m) and 20 ft (6 m).\n"
         "alert_distance = 20 ft\n"
         "\n"
         "# Seconds with nobody in range before it shows CLEAR again.\n"
         "clear_seconds = 3\n"
         "\n"
         "# Beeps when someone arrives (0 = silent), and their pitch in Hz.\n"
         "# Beeps need a small speaker on the 2-pin SPEAK connector.\n"
         "beeps = 2\n"
         "beep_hz = 2600\n"
         "\n"
         "# Screen orientation: 1 = landscape, 3 = upside down.\n"
         "rotation = 1\n"
         "\n"
         "# Save arrivals and departures to detector/log.csv on this card (on / off).\n"
         "log = on\n"
         "\n"
         "# RD-03D / LD2450: swap left and right, if people show up on the wrong side\n"
         "# (it depends on which way up the radar is mounted).\n"
         "mirror = off\n"
         "\n"
         "# RD-03D / LD2450: keep showing people who've stopped moving (on / off). Turn it off\n"
         "# if dots stay where nobody is.\n"
         "still_targets = on\n"
         "\n"
         "# Brightness of the LED on the back, 0-100 (%). Tap the screen to pick its color.\n"
         "led_brightness = 100\n"
         "# Is the LED lit when it should be off? Some boards wire it the other way round:\n"
         "# then set this to on.\n"
         "led_invert = off\n"
         "\n"
         "# LD2410B radar settings. These are saved inside the radar itself, so they're only\n"
         "# sent when you remove the # in front of them.\n"
         "# sensor_range: how far the radar looks (1.5 m to 6 m, in 0.75 m steps).\n"
         "# sensor_hold_seconds: how long the radar keeps saying \"person\" after they leave.\n"
         "# sensor_range = 20 ft\n"
         "# sensor_hold_seconds = 5\n";
}

void formatDistance(uint16_t cm, bool useFeet, char* out, size_t outLen) {
  if (useFeet) {
    double feet = cm / 30.48;
    if (feet < 9.95) {
      snprintf(out, outLen, "%.1f ft", feet);
    } else {
      snprintf(out, outLen, "%d ft", (int)(feet + 0.5));
    }
  } else {
    snprintf(out, outLen, "%.1f m", cm / 100.0);
  }
}

const char* motionLabel(uint8_t state) {
  switch (state & 0x03) {
    case 1: return "Moving";
    case 2: return "Standing still";
    case 3: return "Moving + still";
    default: return "Nobody";
  }
}

PresenceTracker::Event PresenceTracker::update(bool inRange, uint16_t distanceCm, uint32_t nowMs, uint32_t clearMs) {
  if (inRange) {
    lastSeenMs_ = nowMs;
    if (!present_) {
      present_ = true;
      startMs_ = nowMs;
      closestCm_ = distanceCm;
      visits_++;
      return Event::Arrived;
    }
    if (distanceCm > 0 && (closestCm_ == 0 || distanceCm < closestCm_)) closestCm_ = distanceCm;
    return Event::None;
  }
  if (present_ && nowMs - lastSeenMs_ >= clearMs) {
    present_ = false;
    return Event::Left;
  }
  return Event::None;
}

const char* logHeader() { return "uptime_s,event,distance_cm,closest_cm,duration_s,motion\n"; }

int formatLogRow(char* out, size_t outLen, uint32_t uptimeSeconds, PresenceTracker::Event event, uint16_t distanceCm,
                 uint16_t closestCm, uint32_t durationSeconds, uint8_t state) {
  const char* name = event == PresenceTracker::Event::Arrived ? "arrived"
                     : event == PresenceTracker::Event::Left  ? "left"
                                                              : "none";
  return snprintf(out, outLen, "%lu,%s,%u,%u,%lu,%s\n", (unsigned long)uptimeSeconds, name, (unsigned)distanceCm,
                  (unsigned)closestCm, (unsigned long)durationSeconds, motionLabel(state));
}

// ------------------------------------------------------------------ which targets count

namespace {

// Hi-Link's factory trigger levels per gate. The still level of gates 0 and 1 can't be set in the
// radar; 40 (as gate 2) is used for the learned floor there.
const uint8_t kFactoryMoving[kMaxGates] = {50, 50, 40, 30, 20, 15, 15, 15, 15};
const uint8_t kFactoryStill[kMaxGates] = {40, 40, 40, 40, 30, 30, 20, 20, 20};

int gateOf(uint16_t cm) {
  int gate = cm / kGateCm;
  return gate >= kMaxGates ? kMaxGates - 1 : gate;
}

uint8_t level(uint8_t background, uint8_t factory) {
  int v = background + kLearnMargin;
  if (v < factory) v = factory;
  return (uint8_t)(v > 100 ? 100 : v);
}

}  // namespace

void addToBackground(Background& bg, const RadarReading& r) {
  if (!r.engineering) return;
  for (int i = 0; i < r.gateCount && i < kMaxGates; i++) {
    if (r.movingGates[i] > bg.moving[i]) bg.moving[i] = r.movingGates[i];
    if (r.stillGates[i] > bg.still[i]) bg.still[i] = r.stillGates[i];
  }
}

void learnedSensitivity(const Background& bg, uint8_t moving[kMaxGates], uint8_t still[kMaxGates]) {
  for (int i = 0; i < kMaxGates; i++) {
    moving[i] = level(bg.moving[i], kFactoryMoving[i]);
    still[i] = level(bg.still[i], kFactoryStill[i]);
  }
}

Targets pickTargets(const RadarReading& r, bool movingOnly, const Background& bg) {
  Targets t;
  if (r.moving() && r.movingCm > 0) {
    t.moving = !bg.learned || r.movingEnergy > bg.moving[gateOf(r.movingCm)] + kLearnMargin;
    if (t.moving) t.movingCm = r.movingCm;
  }
  if (r.still() && r.stillCm > 0) {
    bool strongEnough = !bg.learned || r.stillEnergy > bg.still[gateOf(r.stillCm)] + kLearnMargin;
    if (movingOnly) {
      t.stillIgnored = strongEnough;
    } else if (strongEnough) {
      t.still = true;
      t.stillCm = r.stillCm;
    }
  }
  return t;
}

uint16_t DistanceFilter::add(uint16_t cm) {
  if (cm == 0) {
    count_ = next_ = 0;
    return 0;
  }
  samples_[next_] = cm;
  next_ = (next_ + 1) % kSize;
  if (count_ < kSize) count_++;
  uint16_t sorted[kSize];
  for (int i = 0; i < count_; i++) sorted[i] = samples_[i];
  for (int i = 1; i < count_; i++) {  // insertion sort: at most 5 values
    uint16_t v = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > v) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = v;
  }
  return sorted[count_ / 2];
}
