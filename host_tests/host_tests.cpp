// Unit tests for the radar protocol and detector logic. They run on your computer, not the board.
// From the CYD-Human-Detector folder:
//
//   g++ -std=c++17 -Iinclude host_tests/host_tests.cpp src/radar_protocol.cpp src/tracking_protocol.cpp src/detector.cpp -o host_tests/run
//   host_tests/run
#include <stdio.h>
#include <string.h>

#include <vector>

#include "detector.h"
#include "radar_protocol.h"
#include "tracking_protocol.h"

static int failures = 0;
#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                            \
    }                                                        \
  } while (0)

using Bytes = std::vector<uint8_t>;

static bool sameBytes(const uint8_t* a, size_t n, const Bytes& b) { return n == b.size() && memcmp(a, b.data(), n) == 0; }

// A data frame as the radar sends it: header, length, payload, footer.
static Bytes dataFrame(const Bytes& payload) {
  Bytes f = {0xF4, 0xF3, 0xF2, 0xF1, (uint8_t)(payload.size() & 0xFF), (uint8_t)(payload.size() >> 8)};
  f.insert(f.end(), payload.begin(), payload.end());
  f.insert(f.end(), {0xF8, 0xF7, 0xF6, 0xF5});
  return f;
}

static Bytes basicPayload(uint8_t state, uint16_t moveCm, uint8_t moveE, uint16_t stillCm, uint8_t stillE, uint16_t detCm) {
  return {0x02, 0xAA, state, (uint8_t)moveCm, (uint8_t)(moveCm >> 8), moveE, (uint8_t)stillCm, (uint8_t)(stillCm >> 8),
          stillE, (uint8_t)detCm, (uint8_t)(detCm >> 8), 0x55, 0x00};
}

static Bytes engineeringPayload() {
  Bytes p = {0x01, 0xAA, 0x03, 0x78, 0x00, 0x40, 0xC8, 0x00, 0x22, 0x78, 0x00, 0x08, 0x08};
  for (int i = 0; i < 9; i++) p.push_back((uint8_t)(10 + i));  // moving energy per gate
  for (int i = 0; i < 9; i++) p.push_back((uint8_t)(90 - i));  // still energy per gate
  p.insert(p.end(), {0x00, 0x00, 0x55, 0x00});                  // 2 reserved, tail, check
  return p;
}

// Feeds bytes; returns every reading produced.
static std::vector<RadarReading> feedAll(RadarParser& parser, const Bytes& bytes) {
  std::vector<RadarReading> got;
  RadarReading r;
  for (uint8_t b : bytes) {
    if (parser.feed(b, r)) got.push_back(r);
  }
  return got;
}

static void testCommands() {
  uint8_t out[64];
  size_t n = radarCmdEnableConfig(out, sizeof(out));
  CHECK(sameBytes(out, n, {0xFD, 0xFC, 0xFB, 0xFA, 0x04, 0x00, 0xFF, 0x00, 0x01, 0x00, 0x04, 0x03, 0x02, 0x01}));
  n = radarCmdEndConfig(out, sizeof(out));
  CHECK(sameBytes(out, n, {0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0xFE, 0x00, 0x04, 0x03, 0x02, 0x01}));
  n = radarCmdEngineering(true, out, sizeof(out));
  CHECK(sameBytes(out, n, {0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x62, 0x00, 0x04, 0x03, 0x02, 0x01}));
  n = radarCmdEngineering(false, out, sizeof(out));
  CHECK(sameBytes(out, n, {0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x63, 0x00, 0x04, 0x03, 0x02, 0x01}));
  // The protocol document's example: gate 8 moving & still, 5 s hold
  n = radarCmdMaxGates(8, 8, 5, out, sizeof(out));
  CHECK(sameBytes(out, n, {0xFD, 0xFC, 0xFB, 0xFA, 0x14, 0x00, 0x60, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01,
                           0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01}));
  // The protocol document's example: gate 3, moving 40, still 40
  n = radarCmdGateSensitivity(3, 40, 40, out, sizeof(out));
  CHECK(sameBytes(out, n, {0xFD, 0xFC, 0xFB, 0xFA, 0x14, 0x00, 0x64, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01,
                           0x00, 0x28, 0x00, 0x00, 0x00, 0x02, 0x00, 0x28, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01}));
  CHECK(radarCmdEnableConfig(out, 10) == 0);  // buffer too small
  CHECK(radarAckFor(radarcmd::EnableConfig) == 0x01FF);
}

static void testParser() {
  // Basic frame: moving person at 81 cm, energy 59
  {
    RadarParser p;
    auto got = feedAll(p, dataFrame(basicPayload(0x01, 81, 59, 0, 0, 81)));
    CHECK(got.size() == 1);
    CHECK(got[0].state == 1 && got[0].movingCm == 81 && got[0].movingEnergy == 59 && !got[0].engineering);
    CHECK(got[0].nearestCm() == 81);
  }
  // Engineering frame with per-gate energies
  {
    RadarParser p;
    auto got = feedAll(p, dataFrame(engineeringPayload()));
    CHECK(got.size() == 1);
    const RadarReading& r = got[0];
    CHECK(r.engineering && r.gateCount == 9);
    CHECK(r.state == 3 && r.movingCm == 120 && r.movingEnergy == 64 && r.stillCm == 200 && r.stillEnergy == 34);
    CHECK(r.movingGates[0] == 10 && r.movingGates[8] == 18 && r.stillGates[0] == 90 && r.stillGates[8] == 82);
    CHECK(r.nearestCm() == 120);
  }
  // Garbage before, a frame split byte by byte, then a second frame
  {
    RadarParser p;
    Bytes stream = {0x00, 0xF4, 0x13, 0xFF, 0xF4, 0xF3, 0x99};
    Bytes a = dataFrame(basicPayload(0x02, 0, 0, 300, 40, 300));
    Bytes b = dataFrame(basicPayload(0x00, 0, 0, 0, 0, 0));
    stream.insert(stream.end(), a.begin(), a.end());
    stream.insert(stream.end(), b.begin(), b.end());
    auto got = feedAll(p, stream);
    CHECK(got.size() == 2);
    CHECK(got.size() == 2 && got[0].state == 2 && got[0].stillCm == 300 && got[1].state == 0);
    CHECK(got.size() == 2 && got[1].nearestCm() == 0);
  }
  // A frame missing its last byte, followed by a good frame: the good one still parses
  {
    RadarParser p;
    Bytes bad = dataFrame(basicPayload(0x01, 50, 50, 0, 0, 50));
    bad.pop_back();
    Bytes good = dataFrame(basicPayload(0x01, 222, 70, 0, 0, 222));
    Bytes stream = bad;
    stream.insert(stream.end(), good.begin(), good.end());
    auto got = feedAll(p, stream);
    CHECK(got.size() == 1 && got[0].movingCm == 222);
  }
  // Impossible length field doesn't wedge the parser
  {
    RadarParser p;
    Bytes stream = {0xF4, 0xF3, 0xF2, 0xF1, 0xFF, 0x7F};
    Bytes good = dataFrame(basicPayload(0x01, 99, 10, 0, 0, 99));
    stream.insert(stream.end(), good.begin(), good.end());
    auto got = feedAll(p, stream);
    CHECK(got.size() == 1 && got[0].movingCm == 99);
    CHECK(p.badFrames() >= 1);
  }
  // Wrong intra-frame head byte is rejected
  {
    RadarParser p;
    Bytes payload = basicPayload(0x01, 10, 10, 0, 0, 10);
    payload[1] = 0xAB;
    CHECK(feedAll(p, dataFrame(payload)).empty());
  }
  // Engineering frame claiming more gates than it carries is rejected
  {
    RadarParser p;
    Bytes payload = engineeringPayload();
    payload[11] = 0x0C;  // 12 gates: out of range
    CHECK(feedAll(p, dataFrame(payload)).empty());
  }
  // ACK frame (from the protocol document): enable configuration succeeded
  {
    RadarParser p;
    auto got = feedAll(p, {0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0xFF, 0x01, 0x00, 0x00, 0x01, 0x00, 0x40, 0x00, 0x04,
                           0x03, 0x02, 0x01});
    CHECK(got.empty());
    CHECK(p.ackCount() == 1 && p.lastAckCommand() == 0x01FF && p.lastAckStatus() == 0);
  }
}

static void testConfig() {
  Settings s;
  char why[64];
  CHECK(applyConfigLine("", s, why, sizeof(why)) == ConfigLine::Blank);
  CHECK(applyConfigLine("  # comment", s, why, sizeof(why)) == ConfigLine::Blank);
  CHECK(applyConfigLine("units = M", s, why, sizeof(why)) == ConfigLine::Applied && !s.useFeet);
  CHECK(applyConfigLine("units = ft", s, why, sizeof(why)) == ConfigLine::Applied && s.useFeet);
  CHECK(applyConfigLine("alert_distance = 10 ft", s, why, sizeof(why)) == ConfigLine::Applied && s.alertCm == 305);
  CHECK(applyConfigLine("alert_distance = 2.5m", s, why, sizeof(why)) == ConfigLine::Applied && s.alertCm == 250);
  CHECK(applyConfigLine("alert_distance = 150 cm  # close", s, why, sizeof(why)) == ConfigLine::Applied &&
        s.alertCm == 150);
  CHECK(applyConfigLine("alert_distance = 10", s, why, sizeof(why)) == ConfigLine::Error && s.alertCm == 150);
  CHECK(applyConfigLine("alert_distance = 40 ft", s, why, sizeof(why)) == ConfigLine::Error);
  CHECK(applyConfigLine("alert_distance = 8 m", s, why, sizeof(why)) == ConfigLine::Applied && s.alertCm == 800);
  CHECK(applyConfigLine("alert_distance = 9 m", s, why, sizeof(why)) == ConfigLine::Error && s.alertCm == 800);
  CHECK(!s.mirror && applyConfigLine("mirror = on", s, why, sizeof(why)) == ConfigLine::Applied && s.mirror);
  CHECK(applyConfigLine("mirror = off", s, why, sizeof(why)) == ConfigLine::Applied && !s.mirror && s.logToSd);
  CHECK(applyConfigLine("mirror = sideways", s, why, sizeof(why)) == ConfigLine::Error && strstr(why, "mirror"));
  CHECK(s.stillTracks && applyConfigLine("still_targets = off", s, why, sizeof(why)) == ConfigLine::Applied &&
        !s.stillTracks && !s.mirror && s.logToSd);
  CHECK(applyConfigLine("still_targets = maybe", s, why, sizeof(why)) == ConfigLine::Error && strstr(why, "still_targets"));

  // LED: brightness setting and the colors a tap cycles through
  CHECK(s.ledBrightness == 100);
  CHECK(applyConfigLine("led_brightness = 40", s, why, sizeof(why)) == ConfigLine::Applied && s.ledBrightness == 40);
  CHECK(applyConfigLine("led_brightness = 101", s, why, sizeof(why)) == ConfigLine::Error && s.ledBrightness == 40);
  CHECK(applyConfigLine("led_color = red", s, why, sizeof(why)) == ConfigLine::Error && strstr(why, "unknown"));
  CHECK(!s.ledInvert && applyConfigLine("led_invert = on", s, why, sizeof(why)) == ConfigLine::Applied && s.ledInvert);
  CHECK(s.logToSd && !s.mirror && !s.stillTracks);  // the other on/off settings untouched
  CHECK(applyConfigLine("led_invert = sometimes", s, why, sizeof(why)) == ConfigLine::Error && strstr(why, "led_invert"));
  CHECK(ledChoiceColor(0) == 0x000000 && strcmp(ledChoiceName(0), "OFF") == 0);
  CHECK(ledChoiceColor(1) == 0xFF0000 && ledChoiceColor(2) == 0x00FF00 && ledChoiceColor(3) == 0x0000FF);
  CHECK(strcmp(ledChoiceName(4), "YELLOW") == 0 && ledChoiceColor(5) == 0x000000);  // wraps back to off
  uint8_t r, g, b;
  ledLevels(0xFF0000, 100, r, g, b);
  CHECK(r == 255 && g == 0 && b == 0);
  ledLevels(0xFF8000, 100, r, g, b);  // half green looks half as bright: 255 * 0.5^2.2
  CHECK(r == 255 && g == 56 && b == 0);
  ledLevels(0xFFFFFF, 50, r, g, b);
  CHECK(r == 55 && g == 55 && b == 55);
  ledLevels(0x00FF00, 0, r, g, b);
  CHECK(r == 0 && g == 0 && b == 0);
  ledLevels(0x0000FF, 250, r, g, b);  // brightness over 100 counts as 100
  CHECK(b == 255);
  CHECK(applyConfigLine("clear_seconds = 10", s, why, sizeof(why)) == ConfigLine::Applied && s.clearSeconds == 10);
  CHECK(applyConfigLine("clear_seconds = 0", s, why, sizeof(why)) == ConfigLine::Error);
  CHECK(applyConfigLine("beeps = 0", s, why, sizeof(why)) == ConfigLine::Applied && s.beepCount == 0);
  CHECK(applyConfigLine("beep_hz = 9000", s, why, sizeof(why)) == ConfigLine::Error);
  CHECK(applyConfigLine("rotation = 3", s, why, sizeof(why)) == ConfigLine::Applied && s.rotation == 3);
  CHECK(applyConfigLine("rotation = 2", s, why, sizeof(why)) == ConfigLine::Error);
  CHECK(applyConfigLine("log = off", s, why, sizeof(why)) == ConfigLine::Applied && !s.logToSd);
  CHECK(applyConfigLine("sensor_range = 20 ft", s, why, sizeof(why)) == ConfigLine::Applied && s.maxGate == 8);
  CHECK(applyConfigLine("sensor_range = 4.5 m", s, why, sizeof(why)) == ConfigLine::Applied && s.maxGate == 6);
  CHECK(applyConfigLine("sensor_range = 1 m", s, why, sizeof(why)) == ConfigLine::Applied && s.maxGate == 2);
  CHECK(applyConfigLine("sensor_hold_seconds = 5", s, why, sizeof(why)) == ConfigLine::Applied &&
        s.sensorHoldSeconds == 5);
  CHECK(applyConfigLine("volume = 11", s, why, sizeof(why)) == ConfigLine::Error && strcmp(why, "unknown setting") == 0);
  CHECK(applyConfigLine("no equals", s, why, sizeof(why)) == ConfigLine::Error);

  // The default file parses cleanly and matches the built-in defaults
  Settings fromFile;
  int problems = 0;
  const char* text = defaultConfigText();
  char line[200];
  while (*text) {
    const char* nl = strchr(text, '\n');
    size_t len = nl ? (size_t)(nl - text) : strlen(text);
    memcpy(line, text, len);
    line[len] = '\0';
    if (applyConfigLine(line, fromFile, why, sizeof(why)) == ConfigLine::Error) {
      printf("default config line failed: '%s' (%s)\n", line, why);
      problems++;
    }
    text += len + (nl ? 1 : 0);
  }
  Settings d;
  CHECK(problems == 0);
  CHECK(fromFile.useFeet == d.useFeet && fromFile.clearSeconds == d.clearSeconds && fromFile.beepCount == d.beepCount);
  CHECK(fromFile.beepHz == d.beepHz && fromFile.rotation == d.rotation && fromFile.logToSd == d.logToSd);
  CHECK(fromFile.alertCm == 610 && d.alertCm == 600);  // "20 ft" in the file
  CHECK(fromFile.maxGate == -1 && fromFile.sensorHoldSeconds == -1);  // commented out
}

static void testFormatting() {
  char t[16];
  formatDistance(190, true, t, sizeof(t));
  CHECK(strcmp(t, "6.2 ft") == 0);
  formatDistance(300, true, t, sizeof(t));
  CHECK(strcmp(t, "9.8 ft") == 0);
  formatDistance(305, true, t, sizeof(t));
  CHECK(strcmp(t, "10 ft") == 0);
  formatDistance(610, true, t, sizeof(t));
  CHECK(strcmp(t, "20 ft") == 0);
  formatDistance(190, false, t, sizeof(t));
  CHECK(strcmp(t, "1.9 m") == 0);
  CHECK(strcmp(motionLabel(0), "Nobody") == 0 && strcmp(motionLabel(2), "Standing still") == 0);
}

static void testTracker() {
  PresenceTracker t;
  using E = PresenceTracker::Event;
  CHECK(t.update(false, 0, 0, 3000) == E::None && !t.present());
  CHECK(t.update(true, 400, 1000, 3000) == E::Arrived && t.present() && t.visits() == 1);
  CHECK(t.update(true, 150, 1100, 3000) == E::None && t.closestCm() == 150);
  CHECK(t.update(true, 300, 1200, 3000) == E::None && t.closestCm() == 150);
  CHECK(t.update(false, 0, 3000, 3000) == E::None && t.present());  // only 1.8 s gone
  CHECK(t.update(true, 200, 3500, 3000) == E::None && t.visits() == 1);  // came back: same visit
  CHECK(t.update(false, 0, 6600, 3000) == E::Left && !t.present());
  CHECK(t.update(true, 500, 7000, 3000) == E::Arrived && t.visits() == 2 && t.closestCm() == 500);

  char row[96];
  formatLogRow(row, sizeof(row), 42, E::Arrived, 400, 400, 0, 1);
  CHECK(strcmp(row, "42,arrived,400,400,0,Moving\n") == 0);
  formatLogRow(row, sizeof(row), 50, E::Left, 0, 150, 8, 0);
  CHECK(strcmp(row, "50,left,0,150,8,Nobody\n") == 0);
}

static RadarReading basicReading(uint8_t state, uint16_t movCm, uint8_t movE, uint16_t stillCm, uint8_t stillE) {
  RadarReading r;
  r.state = state;
  r.movingCm = movCm;
  r.movingEnergy = movE;
  r.stillCm = stillCm;
  r.stillEnergy = stillE;
  r.detectCm = movCm ? movCm : stillCm;
  return r;
}

static void testTargets() {
  Background none;
  // A metal object (still, 90 cm) nearer than a walking person (300 cm): the person's distance wins.
  RadarReading r = basicReading(3, 300, 60, 90, 70);
  Targets t = pickTargets(r, false, none);
  CHECK(t.moving && t.movingCm == 300 && t.still && t.stillCm == 90 && t.distanceCm() == 300);
  // Standing still: the still distance is used.
  t = pickTargets(basicReading(2, 0, 0, 240, 50), false, none);
  CHECK(!t.moving && t.still && t.distanceCm() == 240);
  // Moving-only mode ignores still targets, but says one was there.
  t = pickTargets(basicReading(2, 0, 0, 240, 50), true, none);
  CHECK(!t.any() && t.distanceCm() == 0 && t.stillIgnored);
  t = pickTargets(basicReading(3, 300, 60, 90, 70), true, none);
  CHECK(t.moving && !t.still && t.distanceCm() == 300);
  // Nobody
  t = pickTargets(basicReading(0, 0, 0, 0, 0), false, none);
  CHECK(!t.any() && !t.stillIgnored && t.distanceCm() == 0);

  // Learning: strongest energy per gate across engineering reports; basic reports are ignored.
  Background bg;
  RadarReading e;
  e.engineering = true;
  e.gateCount = 9;
  e.stillGates[1] = 65;  // metal at gate 1 (75-150 cm)
  e.movingGates[4] = 12;
  addToBackground(bg, e);
  e.stillGates[1] = 55;
  e.movingGates[4] = 18;
  addToBackground(bg, e);
  addToBackground(bg, basicReading(2, 0, 0, 100, 99));
  CHECK(bg.still[1] == 65 && bg.moving[4] == 18 && bg.still[0] == 0);
  bg.learned = true;

  uint8_t mov[kMaxGates], still[kMaxGates];
  learnedSensitivity(bg, mov, still);
  CHECK(still[1] == 75);                 // 65 + 10
  CHECK(mov[4] == 28);                   // 18 + 10 beats the factory 20
  CHECK(mov[0] == 50 && still[8] == 20);  // never below the factory levels
  bg.still[2] = 95;
  learnedSensitivity(bg, mov, still);
  CHECK(still[2] == 100);                // capped

  // The metal object at 90 cm (gate 1) is now ignored; a person still at 240 cm isn't.
  t = pickTargets(basicReading(2, 0, 0, 90, 70), false, bg);
  CHECK(!t.still);
  t = pickTargets(basicReading(2, 0, 0, 90, 80), false, bg);  // much stronger than the background
  CHECK(t.still && t.stillCm == 90);
  t = pickTargets(basicReading(2, 0, 0, 240, 50), false, bg);
  CHECK(t.still && t.stillCm == 240);
  // A weak movement at gate 4 (300-375 cm) is background now; a strong one counts.
  t = pickTargets(basicReading(1, 310, 25, 0, 0), false, bg);
  CHECK(!t.moving);
  t = pickTargets(basicReading(1, 310, 60, 0, 0), false, bg);
  CHECK(t.moving && t.distanceCm() == 310);
  // Far beyond the last gate still uses the last gate's background.
  t = pickTargets(basicReading(1, 900, 60, 0, 0), false, bg);
  CHECK(t.moving);
}

static void testDistanceFilter() {
  DistanceFilter f;
  CHECK(f.add(200) == 200);
  CHECK(f.add(210) == 210);              // middle of {200, 210}: upper middle
  CHECK(f.add(205) == 205);
  CHECK(f.add(900) == 210);              // one odd report doesn't move it far
  CHECK(f.add(207) == 207);
  CHECK(f.add(209) == 209);              // the oldest (200) drops out: 205 207 209 210 900
  CHECK(f.add(0) == 0);                  // target gone: start over
  CHECK(f.add(400) == 400);
}

// ------------------------------------------------------------------ RD-03D / LD2450

// One 8-byte slot with the radars' sign-flag encoding (top bit set = positive).
static void putSigned(Bytes& out, int value) {
  uint16_t raw = (uint16_t)((value < 0 ? -value : value) & 0x7FFF);
  if (value >= 0) raw |= 0x8000;
  out.push_back((uint8_t)(raw & 0xFF));
  out.push_back((uint8_t)(raw >> 8));
}

struct Slot {
  int x, y, speed, res;
};

static Bytes trackingFrame(const std::vector<Slot>& slots) {
  Bytes f = {0xAA, 0xFF, 0x03, 0x00};
  for (int i = 0; i < 3; i++) {
    if (i < (int)slots.size()) {
      putSigned(f, slots[i].x);
      putSigned(f, slots[i].y);
      putSigned(f, slots[i].speed);
      f.push_back((uint8_t)(slots[i].res & 0xFF));
      f.push_back((uint8_t)(slots[i].res >> 8));
    } else {
      f.insert(f.end(), 8, 0x00);
    }
  }
  f.insert(f.end(), {0x55, 0xCC});
  return f;
}

static std::vector<TrackingReading> feedTracking(TrackingParser& parser, const Bytes& bytes) {
  std::vector<TrackingReading> got;
  TrackingReading r;
  for (uint8_t b : bytes) {
    if (parser.feed(b, r)) got.push_back(r);
  }
  return got;
}

static bool near(float a, float b, float tolerance) { return a > b - tolerance && a < b + tolerance; }

static void testTracking() {
  // Hi-Link's LD2450 manual example: target 1 at x -782 mm, y 1713 mm, speed -16 cm/s,
  // resolution 320 mm; targets 2 and 3 empty.
  const Bytes manual = {0xAA, 0xFF, 0x03, 0x00, 0x0E, 0x03, 0xB1, 0x86, 0x10, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0xCC};
  TrackingParser p;
  auto got = feedTracking(p, manual);
  CHECK(got.size() == 1);
  if (got.size() == 1) {
    const Track& t = got[0].tracks[0];
    CHECK(t.valid && t.xMm == -782 && t.yMm == 1713 && t.speedCmS == -16 && t.resolutionMm == 320);
    CHECK(!got[0].tracks[1].valid && !got[0].tracks[2].valid && got[0].count() == 1 && got[0].nearest() == 0);
    CHECK(t.distanceCm() == 188);  // sqrt(782^2 + 1713^2) = 1883 mm
    CHECK(near(t.angleDeg(), -24.5f, 0.2f));  // left of center
    CHECK(t.moving());
  }
  // Our encoder matches the manual's bytes.
  CHECK(trackingFrame({{-782, 1713, -16, 320}}) == manual);

  // Three people; the nearest is found; angles on both sides.
  got = feedTracking(p, trackingFrame({{1000, 1000, 30, 320}, {0, 900, 0, 320}, {-2000, 3464, -50, 320}}));
  CHECK(got.size() == 1);
  if (got.size() == 1) {
    CHECK(got[0].count() == 3 && got[0].nearest() == 1);
    CHECK(near(got[0].tracks[0].angleDeg(), 45.0f, 0.1f) && near(got[0].tracks[2].angleDeg(), -30.0f, 0.1f));
    CHECK(!got[0].tracks[1].moving() && got[0].tracks[1].valid);  // standing still still counts
    CHECK(got[0].tracks[2].distanceCm() == 400);
  }

  // Not-real targets: the RD-03D's 248 / 256 cm/s markers, and a position behind the radar.
  got = feedTracking(p, trackingFrame({{500, 1500, 248, 320}, {500, 1500, -256, 320}, {500, -100, 20, 320}}));
  CHECK(got.size() == 1 && got[0].count() == 0 && got[0].nearest() == -1);

  // Garbage, a frame with a broken footer, then a good one: only the good one comes through.
  Bytes stream = {0x00, 0xAA, 0x13, 0xAA, 0xFF, 0x03};  // includes a false start
  Bytes broken = trackingFrame({{100, 1000, 10, 320}});
  broken[29] = 0x00;
  stream.insert(stream.end(), broken.begin(), broken.end());
  Bytes good = trackingFrame({{-300, 2000, 5, 320}});
  stream.insert(stream.end(), good.begin(), good.end());
  TrackingParser q;
  got = feedTracking(q, stream);
  CHECK(got.size() == 1 && q.badFrames() >= 1);
  if (got.size() == 1) CHECK(got[0].tracks[0].xMm == -300 && got[0].tracks[0].yMm == 2000);

  // Both parsers on one stream (how the firmware tells the radars apart): each finds only its own.
  Bytes mixed = dataFrame(basicPayload(1, 120, 60, 0, 0, 120));
  mixed.insert(mixed.end(), good.begin(), good.end());
  RadarParser presence;
  TrackingParser tracking;
  CHECK(feedAll(presence, mixed).size() == 1);
  CHECK(feedTracking(tracking, mixed).size() == 1);

  uint8_t out[16];
  size_t n = trackingCmdMultiTarget(out, sizeof(out));
  CHECK(sameBytes(out, n, {0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x90, 0x00, 0x04, 0x03, 0x02, 0x01}));
  CHECK(trackingCmdMultiTarget(out, 4) == 0);
}

int main() {
  testTracking();
  testCommands();
  testTargets();
  testDistanceFilter();
  testParser();
  testConfig();
  testFormatting();
  testTracker();
  if (failures) {
    printf("%d check(s) failed\n", failures);
    return 1;
  }
  printf("All host tests passed\n");
  return 0;
}
