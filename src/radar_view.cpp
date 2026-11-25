#include "radar_view.h"

#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace view {
namespace {

// ------------------------------------------------------------------ colors

struct Rgb {
  uint8_t r, g, b;
};
const Rgb kRgb[kColorCount] = {
    {8, 18, 26},     // Bg
    {20, 36, 46},    // Panel
    {62, 80, 92},    // Grid: thin range rings
    {44, 160, 60},   // GreenDim: thin lines, small text
    {74, 232, 84},   // Green: main lines
    {160, 255, 150}, // GreenHi: the ping's leading edge
    {64, 196, 84},   // Ping2
    {36, 112, 52},   // Ping3
    {226, 234, 236}, // Text
    {120, 140, 150}, // TextDim
    {255, 48, 40},   // Red
    {255, 180, 150}, // RedHi: the dot's center
    {140, 26, 26},   // RedDim: the dot's glow
    {120, 34, 34},   // RedArc: "somewhere at this distance"
    {255, 176, 32},  // Amber
    {255, 226, 150}, // AmberHi
    {130, 84, 16},   // AmberDim
    {64, 216, 72},   // BoxGreen
    {6, 40, 10},     // BoxDark: text on the green box
    {196, 18, 28},   // BoxRed
    {255, 70, 70},   // BoxRedHi
    {255, 255, 255}, // White
    {70, 18, 20},    // TrailOld
    {130, 26, 26},   // TrailMid
    {200, 38, 34},   // TrailNew
    {90, 150, 255},  // LedBlueText: "LED: BLUE"
    {255, 214, 40},  // LedYellowText: "LED: YELLOW"
};

uint16_t g565[kColorCount];
bool g565Ready = false;

// ------------------------------------------------------------------ geometry
//
// The fan's point is just below the screen's bottom edge, hidden in the distance box. 0 m sits on
// the box's top corners (radius kRBase), the far end of the range on the outer arc (radius kR).

constexpr float kAX = 160.0f, kAY = 238.0f;
constexpr float kR = 176.0f, kRBase = 48.0f;
constexpr float kS60 = 0.8660254f, kC60 = 0.5f;
constexpr float kHalfFan = 1.0471976f;  // 60 degrees
constexpr int kBoxTop = (int)(kAY - kRBase * kC60);  // 214
constexpr int kBoxLeft = (int)(kAX - kRBase * kS60 + 0.5f), kBoxRight = (int)(kAX + kRBase * kS60 + 0.5f);

// Bottom corners: targets (left) and status (right). Kept clear of the fan's edges and range labels.
// The tracking radars' list has 3 lines, so it's narrower and taller (still clear of the edge).
constexpr int kTargetsX = 3, kTargetsY = 209, kTargetsW = 100, kTargetsH = 30;
constexpr int kTracksX = 3, kTracksY = 200, kTracksW = 86, kTracksH = 39;
constexpr int kStatusX = 234, kStatusY = 213, kStatusW = 84, kStatusH = 26;
// Top corners: info (left) and radar link (right).
constexpr int kInfoX = 2, kInfoY = 2, kInfoW = 89, kInfoH = 39;
constexpr int kLinkX = 229, kLinkY = 2, kLinkW = 89, kLinkH = 39;

// Sine/cosine across the fan (-60..+60 degrees), so arcs are quick to draw every frame.
constexpr int kSteps = 1024;
constexpr float kStepAngle = 2 * kHalfFan / kSteps;
float gSin[kSteps + 1], gCos[kSteps + 1];
bool gTrigReady = false;

void initTables() {
  if (gTrigReady) return;
  for (int i = 0; i <= kSteps; i++) {
    float a = -kHalfFan + i * kStepAngle;
    gSin[i] = sinf(a);
    gCos[i] = cosf(a);
  }
  gTrigReady = true;
}

int iround(float v) { return (int)floorf(v + 0.5f); }

// The distance scale: what the outer arc stands for, and the spacing of the range rings.
struct Scale {
  float fullCm;
  float stepCm;
  bool feet;
  int steps() const { return (int)(fullCm / stepCm + 0.5f); }
};
// LD2410B: 6 m / 20 ft. RD-03D / LD2450 see further: 8 m / 25 ft.
Scale scaleFor(RadarKind kind, bool feet) {
  if (kind == RadarKind::Tracking) return feet ? Scale{762.0f, 152.4f, true} : Scale{800.0f, 200.0f, false};
  return feet ? Scale{609.6f, 152.4f, true} : Scale{600.0f, 100.0f, false};
}
Scale gScale = scaleFor(RadarKind::Presence, true);  // set at the start of each frame

float radiusFor(float cm) {
  if (cm > gScale.fullCm) cm = gScale.fullCm;
  if (cm < 0) cm = 0;
  return kRBase + cm / gScale.fullCm * (kR - kRBase);
}

// An arc across the whole fan. `dash` = 0 for solid, n for n pixels on / n off (measured along an
// arc of radius `dashRadius`, so neighbouring arcs can share one dash pattern).
void arc(Canvas8& c, float r, uint8_t color, int dash = 0, float dashRadius = 0) {
  if (r <= 0) return;
  int stride = (int)(0.45f / (r * kStepAngle));
  if (stride < 1) stride = 1;
  int lastX = -1, lastY = -1;
  for (int i = 0; i <= kSteps; i += stride) {
    int x = iround(kAX + r * gSin[i]), y = iround(kAY - r * gCos[i]);
    if (x == lastX && y == lastY) continue;
    lastX = x;
    lastY = y;
    if (dash > 0 && ((int)(i * kStepAngle * (dashRadius > 0 ? dashRadius : r)) / dash) & 1) continue;
    c.pixel(x, y, color);
  }
}

void thickArc(Canvas8& c, float r, int thickness, uint8_t color) {
  float half = (thickness - 1) * 0.5f;
  for (float o = -half; o <= half + 0.01f; o += 0.5f) arc(c, r + o, color);
}

// A point at `r` along the fan's edge or a radial, `angle` in radians (0 = straight ahead).
void polar(float r, float angle, float& x, float& y) {
  x = kAX + r * sinf(angle);
  y = kAY - r * cosf(angle);
}

void smallCentered(Canvas8& c, const char* s, int centerX, int top, uint8_t color) {
  c.small(s, centerX - Canvas8::smallWidth(s) / 2, top, color);
}

void smallRight(Canvas8& c, const char* s, int right, int top, uint8_t color) {
  c.small(s, right - Canvas8::smallWidth(s), top, color);
}

// ------------------------------------------------------------------ text helpers

void formatAgo(uint32_t seconds, char* out, int outLen) {
  if (seconds < 60) {
    snprintf(out, outLen, "SEEN JUST NOW");
  } else if (seconds < 3600) {
    snprintf(out, outLen, "SEEN %lu MIN AGO", (unsigned long)(seconds / 60));
  } else if (seconds < 100 * 3600UL) {
    snprintf(out, outLen, "SEEN %lu HR AGO", (unsigned long)(seconds / 3600));
  } else {
    snprintf(out, outLen, "SEEN DAYS AGO");
  }
}

void formatHere(uint32_t seconds, char* out, int outLen) {
  if (seconds < 3600) {
    snprintf(out, outLen, "HERE %lu:%02lu", (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
  } else {
    snprintf(out, outLen, "HERE %luh%02lu", (unsigned long)(seconds / 3600), (unsigned long)(seconds / 60 % 60));
  }
}

// ------------------------------------------------------------------ the fan

void drawGrid(Canvas8& c, const Info& info, bool live) {
  const uint8_t main = live ? Green : Grid;
  const uint8_t thin = live ? GreenDim : Grid;

  // Range rings (every 1 m / 2 m / 5 ft). The first is thick, like the base of a radar scope.
  for (int i = 1; i < gScale.steps(); i++) {
    float r = radiusFor(i * gScale.stepCm);
    if (i == 1) {
      thickArc(c, r, 3, main);
    } else {
      arc(c, r, Grid);
    }
  }
  // Outer edge of the range: dotted.
  arc(c, kR, main, 2, kR);
  arc(c, kR + 0.5f, main, 2, kR);

  // Alert distance, if it's noticeably shorter than the full range (the default is the full range).
  if (live && info.alertCm < gScale.fullCm - 20) arc(c, radiusFor(info.alertCm), Amber, 3);

  // Radials at -30, 0 and +30 degrees, from the distance box up to the outer arc.
  const float radials[] = {-0.5235988f, 0.0f, 0.5235988f};
  for (float a : radials) {
    float x0, y0, x1, y1;
    polar((kAY - kBoxTop) / cosf(a), a, x0, y0);
    polar(kR, a, x1, y1);
    c.line(iround(x0), iround(y0), iround(x1), iround(y1), thin);
  }
}

void drawEdgesAndBox(Canvas8& c, bool live) {
  const uint8_t main = live ? Green : Grid;
  for (int side = -1; side <= 1; side += 2) {
    float x0, y0, x1, y1;
    polar(kRBase, side * kHalfFan, x0, y0);
    polar(kR, side * kHalfFan, x1, y1);
    c.line(iround(x0), iround(y0), iround(x1), iround(y1), main, 2);
  }
  // Distance box under the fan's point.
  c.fillRect(kBoxLeft + 2, kBoxTop + 2, kBoxRight - kBoxLeft - 3, Canvas8::H - kBoxTop - 2, Bg);
  c.hline(kBoxLeft, kBoxTop, kBoxRight - kBoxLeft + 1, main);
  c.hline(kBoxLeft, kBoxTop + 1, kBoxRight - kBoxLeft + 1, main);
  c.vline(kBoxLeft, kBoxTop, Canvas8::H - kBoxTop, main);
  c.vline(kBoxLeft + 1, kBoxTop, Canvas8::H - kBoxTop, main);
  c.vline(kBoxRight, kBoxTop, Canvas8::H - kBoxTop, main);
  c.vline(kBoxRight - 1, kBoxTop, Canvas8::H - kBoxTop, main);
}

void drawLabels(Canvas8& c, const Info& info, bool live) {
  const uint8_t color = live ? GreenDim : Grid;
  // Angles around the outer arc.
  float x, y;
  polar(kR + 9, -0.5235988f, x, y);
  smallCentered(c, "-30", iround(x), iround(y) - 4, color);
  polar(kR + 9, 0.5235988f, x, y);
  smallCentered(c, "30", iround(x), iround(y) - 4, color);
  c.small("-60", 0, 158, color);
  smallRight(c, "60", Canvas8::W - 1, 158, color);
  c.fillRect(159, iround(kAY - kR) - 7, 3, 3, live ? Green : Grid);  // straight ahead

  // Distances along the outside of the right-hand edge. (Not the outer one: it would run off the
  // screen, and "60" sits there.)
  (void)info;
  for (int i = 1; i < gScale.steps(); i++) {
    char label[16];
    if (gScale.feet) {
      snprintf(label, sizeof(label), "%dft", i * 5);
    } else {
      snprintf(label, sizeof(label), "%dm", (int)(i * gScale.stepCm / 100 + 0.5f));
    }
    // Just below the edge, starting where the ring meets it.
    polar(radiusFor(i * gScale.stepCm), kHalfFan, x, y);
    c.small(label, iround(x) + 2, iround(y) + 4, color);
  }
}

void drawPing(Canvas8& c, uint32_t nowMs, float& pingR) {
  pingR = -100;
  uint32_t t = nowMs % kPingPeriodMs;
  if (t > kPingSweepMs) return;
  pingR = kRBase + (kR - kRBase) * t / (float)kPingSweepMs;
  const struct {
    float offset;
    uint8_t color;
    int thickness;
  } trail[] = {{-12, Ping3, 1}, {-6, Ping2, 2}, {0, GreenHi, 2}};
  for (const auto& p : trail) {
    float r = pingR + p.offset;
    if (r < kRBase + 3 || r > kR - 2) continue;
    thickArc(c, r, p.thickness, p.color);
  }
}

void drawTarget(Canvas8& c, const Info& info, const Anim& anim, float pingR) {
  if (anim.dotCm <= 0) return;
  bool inAlert = info.targetCm > 0 && info.targetCm <= info.alertCm;
  float r = radiusFor(anim.dotCm);
  arc(c, r, inAlert ? RedArc : AmberDim, 2);  // the LD2410B can't tell direction: anywhere on this arc

  int cx = iround(kAX), cy = iround(kAY - r);
  bool pinged = fabsf(pingR - r) < 7;
  float glow = 8.5f + 1.5f * sinf(anim.nowMs * 0.0063f) + (pinged ? 2.0f : 0.0f);
  c.ring(cx, cy, glow - 1.2f, glow, inAlert ? RedDim : AmberDim);
  c.ring(cx, cy, 4.6f, 6.0f, inAlert ? Red : Amber);
  c.fillCircle(cx, cy, 3, pinged ? White : (inAlert ? RedHi : AmberHi));
}

// RD-03D / LD2450: where a position (mm) lands on the fan. Angles past ±60° sit on the edge.
void trackPoint(float xMm, float yMm, float& px, float& py, float* radius = nullptr) {
  float angle = atan2f(xMm, yMm);
  if (angle > kHalfFan) angle = kHalfFan;
  if (angle < -kHalfFan) angle = -kHalfFan;
  float r = radiusFor(sqrtf(xMm * xMm + yMm * yMm) / 10.0f);
  polar(r, angle, px, py);
  if (radius) *radius = r;
}

void drawTracks(Canvas8& c, const Info& info, const Anim& anim, float pingR) {
  for (int i = 0; i < kMaxTracks; i++) {
    const TrackAnim::Dot& d = anim.tracks.dots[i];
    if (!d.active) continue;
    // Trail: line segments from the oldest point to the dot, darker the older they are.
    if (info.trails && d.trailLen > 0) {
      float px0, py0, px1, py1;
      trackPoint(d.trailX[0], d.trailY[0], px0, py0);
      for (int j = 1; j <= d.trailLen; j++) {
        bool last = j == d.trailLen;
        trackPoint(last ? d.xMm : d.trailX[j], last ? d.yMm : d.trailY[j], px1, py1);
        uint8_t color = j * 3 < d.trailLen ? TrailOld : (j * 3 < d.trailLen * 2 ? TrailMid : TrailNew);
        c.line(iround(px0), iround(py0), iround(px1), iround(py1), color, 2);
        px0 = px1;
        py0 = py1;
      }
    }
  }
  // Dots on top of every trail.
  for (int i = 0; i < kMaxTracks; i++) {
    const TrackAnim::Dot& d = anim.tracks.dots[i];
    if (!d.active) continue;
    float px, py, r;
    trackPoint(d.xMm, d.yMm, px, py, &r);
    const Track& t = info.tracks.tracks[i];
    bool inAlert = t.valid && t.distanceCm() <= info.alertCm;
    int cx = iround(px), cy = iround(py);
    bool pinged = fabsf(pingR - r) < 7;
    float glow = 8.0f + 1.5f * sinf(anim.nowMs * 0.0063f + i * 2.1f) + (pinged ? 2.0f : 0.0f);
    c.ring(cx, cy, glow - 1.2f, glow, inAlert ? RedDim : AmberDim);
    c.ring(cx, cy, 4.6f, 6.0f, inAlert ? Red : Amber);
    c.fillCircle(cx, cy, 3, pinged ? White : (inAlert ? RedHi : AmberHi));
    char label[2] = {(char)('1' + i), 0};
    c.small(label, cx + 8, cy - 13, Text);
  }
}

// ------------------------------------------------------------------ corners

void drawInfo(Canvas8& c, const Info& info) {
  c.fillRect(kInfoX, kInfoY, kInfoW, kInfoH, Panel);
  int x = kInfoX + 3, y = kInfoY + 3;
  char line[24];
  snprintf(line, sizeof(line), "VISITS %lu", (unsigned long)info.visits);
  c.small(line, x, y, GreenDim);
  if (info.present) {
    formatHere(info.visitSeconds, line, sizeof(line));
  } else if (info.visits > 0) {
    formatAgo(info.sinceLastSeenS, line, sizeof(line));
  } else {
    snprintf(line, sizeof(line), "NO VISITS YET");
  }
  c.small(line, x, y + 9, GreenDim);
  switch (info.sd) {
    case Sd::Logging: c.small("SD: LOGGING", x, y + 18, GreenDim); break;
    case Sd::LogOff: c.small("SD: LOG OFF", x, y + 18, TextDim); break;
    case Sd::Error: c.small("SD: ERROR", x, y + 18, Amber); break;
    default: c.small("SD: NO CARD", x, y + 18, TextDim); break;
  }
  // The LED's color, written in that color.
  const uint8_t ledInk[kLedChoices] = {TextDim, Red, Green, LedBlueText, LedYellowText};
  if (info.ledChoice % kLedChoices == 0) {
    c.small("TAP: LED COLOR", x, y + 27, TextDim);
  } else {
    snprintf(line, sizeof(line), "LED: %s", ledChoiceName(info.ledChoice));
    c.small(line, x, y + 27, ledInk[info.ledChoice % kLedChoices]);
  }
}

void drawLink(Canvas8& c, const Info& info, bool live) {
  c.fillRect(kLinkX, kLinkY, kLinkW, kLinkH, Panel);
  int x = kLinkX + 3, y = kLinkY + 3;
  char line[24], d[12];
  if (live && info.kind == RadarKind::Tracking) {
    c.small("RD-03D/LD2450", x, y, GreenDim);
    c.small(info.trails ? "TRAILS: ON" : "TRAILS: OFF", x, y + 9, info.trails ? GreenDim : TextDim);
  } else {
    c.small(info.movingOnly ? "MODE: MOVING" : "MODE: ALL", x, y, info.movingOnly ? Amber : GreenDim);
    c.small(info.roomLearned ? "ROOM: LEARNED" : "ROOM: DEFAULT", x, y + 9, info.roomLearned ? GreenDim : TextDim);
  }
  formatShort(info.alertCm, info.useFeet, d, sizeof(d), false);
  snprintf(line, sizeof(line), "ALERT %s", d);
  c.small(line, x, y + 18, GreenDim);
  if (live) {
    snprintf(line, sizeof(line), "RX: OK fr=%lu", (unsigned long)(info.frames % 100000));
    c.small(line, x, y + 27, Green);
  } else {
    c.small("RX: NO DATA", x, y + 27, Amber);
  }
}

void targetLine(char* out, int outLen, int number, const char* what, uint16_t cm, bool feet) {
  char d[12];
  formatShort(cm, feet, d, sizeof(d), false);
  snprintf(out, outLen, "T%d %s %s", number, what, d);
}

// RD-03D / LD2450: "T1 2.4m -15" = person 1, 2.4 m away, 15 degrees to the left.
void drawTrackList(Canvas8& c, const Info& info) {
  c.rect(kTracksX, kTracksY, kTracksW, kTracksH, GreenDim);
  int x = kTracksX + 3, y = kTracksY + 3;
  c.small("TARGETS", x, y, GreenDim);
  int n = 0;
  for (int i = 0; i < kMaxTracks; i++) {
    const Track& t = info.tracks.tracks[i];
    if (!t.valid) continue;
    char d[12], line[24];
    formatShort(t.distanceCm(), info.useFeet, d, sizeof(d), false);
    snprintf(line, sizeof(line), "T%d %s %d", i + 1, d, iround(t.angleDeg()));
    c.small(line, x, y + 9 + n * 9, t.distanceCm() <= info.alertCm ? Red : Amber);
    n++;
  }
  if (n == 0) c.small("NONE", x, y + 9, TextDim);
}

void drawTargets(Canvas8& c, const Info& info, bool live) {
  if (live && info.kind == RadarKind::Tracking) {
    drawTrackList(c, info);
    return;
  }
  c.rect(kTargetsX, kTargetsY, kTargetsW, kTargetsH, live ? GreenDim : Grid);
  int x = kTargetsX + 3, y = kTargetsY + 3;
  c.small("TARGETS", x, y, live ? GreenDim : Grid);
  if (!live) {
    c.small("--", x, y + 9, TextDim);
    return;
  }
  const Targets& t = info.targets;
  int n = 0;
  char line[24];
  if (t.moving) {
    targetLine(line, sizeof(line), n + 1, "MOVING", t.movingCm, info.useFeet);
    c.small(line, x, y + 9 + n * 8, t.movingCm <= info.alertCm ? Red : Amber);
    n++;
  }
  if (t.still) {
    targetLine(line, sizeof(line), n + 1, "STILL", t.stillCm, info.useFeet);
    c.small(line, x, y + 9 + n * 8, t.stillCm <= info.alertCm ? Red : Amber);
    n++;
  } else if (t.stillIgnored) {
    c.small("STILL: IGNORED", x, y + 9 + n * 8, TextDim);
    n++;
  }
  if (n == 0) c.small("NONE", x, y + 9, TextDim);
}

// A short message over the top of the fan, e.g. after switching modes.
void drawNotice(Canvas8& c, const char* title, const char* line) {
  const int w = 200, h = line ? 40 : 28, x = 160 - w / 2, y = 64;
  c.fillRect(x, y, w, h, Panel);
  c.rect(x, y, w, h, Green, 2);
  c.textCentered(title, 160, y + 19, &FreeSansBold9pt7b, Green);
  if (line) smallCentered(c, line, 160, y + 27, Text);
}

void drawStatus(Canvas8& c, const Info& info, const Anim& anim, bool live) {
  uint8_t fill = BoxGreen, ink = BoxDark;
  char word[24] = "CLEAR";
  if (!live) {
    fill = Amber;
    snprintf(word, sizeof(word), "OFFLINE");
  } else if (info.present) {
    bool flash = info.visitSeconds < 3 && (anim.nowMs / 200) % 2 == 0;  // flashes as they arrive
    fill = flash ? BoxRedHi : BoxRed;
    ink = White;
    int people = 1;
    if (info.kind == RadarKind::Tracking) {
      people = 0;
      for (const Track& t : info.tracks.tracks) people += (t.valid && t.distanceCm() <= info.alertCm) ? 1 : 0;
      if (people < 1) people = 1;  // still "here" during the clear delay
    }
    if (people == 1) {
      snprintf(word, sizeof(word), "PERSON");
    } else {
      snprintf(word, sizeof(word), "%d HERE", people);  // "3 PEOPLE" doesn't fit
    }
  }
  c.fillRect(kStatusX, kStatusY, kStatusW, kStatusH, fill);
  c.textCentered(word, kStatusX + kStatusW / 2, kStatusY + 19, &FreeSansBold9pt7b, ink);
}

void drawDistance(Canvas8& c, const Info& info, bool live) {
  const int cx = (kBoxLeft + kBoxRight) / 2;
  if (live && info.targetCm > 0) {
    char d[12];
    formatShort(info.targetCm, info.useFeet, d, sizeof(d), false);
    c.textCentered(d, cx + 1, 235, &FreeSansBold12pt7b, info.targetCm <= info.alertCm ? Red : Amber);
  } else {
    c.fillRect(cx - 10, 226, 21, 2, live ? Green : Grid);
  }
}

void drawNoRadarHelp(Canvas8& c, uint32_t linkDrops) {
  const int x = 66, y = 90, w = 188, h = 84;
  c.fillRect(x, y, w, h, Panel);
  c.rect(x, y, w, h, Amber, 2);
  c.textCentered("NO RADAR", 160, y + 24, &FreeSansBold12pt7b, Amber);
  smallCentered(c, "Radar TX -> IO27 (CN1)", 160, y + 34, Text);
  smallCentered(c, "Radar RX -> IO22 (CN1)", 160, y + 44, Text);
  smallCentered(c, "VCC -> VIN on P1 (5 V)", 160, y + 54, Text);
  if (linkDrops == 0) {
    smallCentered(c, "Still stuck? Swap TX/RX", 160, y + 68, TextDim);
  } else {  // it was working: wiring order is fine, the connection or power isn't
    char line[40];
    snprintf(line, sizeof(line), "Lost %lux: check wires and 5V", (unsigned long)linkDrops);
    smallCentered(c, line, 160, y + 68, Amber);
  }
}

}  // namespace

const uint16_t* palette565() {
  if (!g565Ready) {
    for (int i = 0; i < kColorCount; i++) {
      g565[i] = ((kRgb[i].r & 0xF8) << 8) | ((kRgb[i].g & 0xFC) << 3) | (kRgb[i].b >> 3);
    }
    g565Ready = true;
  }
  return g565;
}

void paletteRgb(uint8_t color, uint8_t& r, uint8_t& g, uint8_t& b) {
  const Rgb& c = kRgb[color < kColorCount ? color : 0];
  r = c.r;
  g = c.g;
  b = c.b;
}

void formatShort(uint16_t cm, bool useFeet, char* out, int outLen, bool twoDecimals) {
  if (useFeet) {
    float feet = cm / 30.48f;
    if (feet < 9.95f) {
      snprintf(out, outLen, "%.1fft", feet);
    } else {
      snprintf(out, outLen, "%dft", (int)(feet + 0.5f));
    }
  } else if (twoDecimals) {
    snprintf(out, outLen, "%d.%02dm", cm / 100, cm % 100);
  } else {
    int tenths = (cm + 5) / 10;
    snprintf(out, outLen, "%d.%dm", tenths / 10, tenths % 10);
  }
}

void stepTracks(TrackAnim& anim, const TrackingReading& reading, bool newReport, uint32_t dtMs) {
  float k = dtMs / 120.0f;  // glide: reports come in small jumps ~10 times a second
  if (k > 1) k = 1;
  for (int i = 0; i < kMaxTracks; i++) {
    TrackAnim::Dot& d = anim.dots[i];
    const Track& t = reading.tracks[i];
    if (!t.valid) {
      d.active = false;
      d.trailLen = 0;
      continue;
    }
    float dx = t.xMm - d.xMm, dy = t.yMm - d.yMm;
    if (!d.active || dx * dx + dy * dy > 1200.0f * 1200.0f) {
      // New, or jumped more than 1.2 m (the radar swapped who's in this slot): start fresh.
      d.active = true;
      d.xMm = t.xMm;
      d.yMm = t.yMm;
      d.trailLen = 0;
    } else {
      d.xMm += dx * k;
      d.yMm += dy * k;
    }
    if (newReport) {
      if (d.trailLen == kTrailLen) {
        for (int j = 1; j < kTrailLen; j++) {
          d.trailX[j - 1] = d.trailX[j];
          d.trailY[j - 1] = d.trailY[j];
        }
        d.trailLen--;
      }
      d.trailX[d.trailLen] = (int16_t)d.xMm;
      d.trailY[d.trailLen] = (int16_t)d.yMm;
      d.trailLen++;
    }
  }
}

void drawRadar(Canvas8& c, const Info& info, const Anim& anim) {
  initTables();
  const bool live = info.radarConnected;
  gScale = scaleFor(live ? info.kind : RadarKind::Presence, info.useFeet);
  c.clear(Bg);

  drawGrid(c, info, live);
  float pingR = -100;
  if (live) {
    drawPing(c, anim.nowMs, pingR);
    if (info.kind == RadarKind::Tracking) {
      drawTracks(c, info, anim, pingR);
    } else {
      drawTarget(c, info, anim, pingR);
    }
  }
  drawEdgesAndBox(c, live);
  drawLabels(c, info, live);

  drawInfo(c, info);
  drawLink(c, info, live);
  drawTargets(c, info, live);
  drawStatus(c, info, anim, live);
  drawDistance(c, info, live);
  if (!live) {
    drawNoRadarHelp(c, info.linkDrops);
  } else if (info.noticeTitle) {
    drawNotice(c, info.noticeTitle, info.noticeLine);
  }
}

void drawLearn(Canvas8& c, const Learn& learn) {
  initTables();
  gScale = scaleFor(RadarKind::Presence, learn.feet);
  c.clear(Bg);
  Info dim;
  dim.useFeet = learn.feet;
  drawGrid(c, dim, false);
  drawEdgesAndBox(c, false);

  const int x = 30, y = 48, w = 260, h = 132;
  bool failed = learn.phase == Learn::Phase::Done && !learn.ok;
  uint8_t accent = failed ? Amber : Green;
  c.fillRect(x, y, w, h, Panel);
  c.rect(x, y, w, h, accent, 2);
  char line[56];

  switch (learn.phase) {
    case Learn::Phase::Countdown:
      c.textCentered("LEARN THE ROOM", 160, y + 26, &FreeSansBold12pt7b, Green);
      c.textCentered("Step out of the radar's view", 160, y + 50, &FreeSans9pt7b, Text);
      snprintf(line, sizeof(line), "%d", learn.secondsLeft);
      c.textCentered(line, 160, y + 96, &FreeSansBold18pt7b, Green);
      smallCentered(c, "Tap: cancel   Double-tap: undo learning", 160, y + h - 16, TextDim);
      break;
    case Learn::Phase::Measuring: {
      c.textCentered("LEARN THE ROOM", 160, y + 26, &FreeSansBold12pt7b, Green);
      c.textCentered("Measuring: stay out of view", 160, y + 50, &FreeSans9pt7b, Text);
      const int bx = x + 20, by = y + 66, bw = w - 40, bh = 14;
      c.rect(bx, by, bw, bh, GreenDim);
      int fill = (int)((bw - 4) * (learn.progress < 0 ? 0 : (learn.progress > 1 ? 1 : learn.progress)));
      c.fillRect(bx + 2, by + 2, fill, bh - 4, Green);
      snprintf(line, sizeof(line), "%d s left", learn.secondsLeft);
      smallCentered(c, line, 160, by + bh + 6, Text);
      smallCentered(c, "Tap the screen to cancel", 160, y + h - 16, TextDim);
      break;
    }
    case Learn::Phase::Saving:
      c.textCentered("LEARN THE ROOM", 160, y + 26, &FreeSansBold12pt7b, Green);
      c.textCentered("Saving to the radar...", 160, y + 70, &FreeSans9pt7b, Text);
      break;
    case Learn::Phase::Done:
      if (learn.forgot) {
        c.textCentered("ROOM FORGOTTEN", 160, y + 26, &FreeSansBold12pt7b, Green);
        c.textCentered("Back to the radar's", 160, y + 54, &FreeSans9pt7b, Text);
        c.textCentered("factory sensitivity", 160, y + 74, &FreeSans9pt7b, Text);
        if (learn.gatesSaved < kMaxGates) {
          snprintf(line, sizeof(line), "The radar accepted %d of 9 steps", learn.gatesSaved);
          smallCentered(c, line, 160, y + 88, Amber);
        }
      } else if (learn.ok) {
        c.textCentered("ROOM LEARNED", 160, y + 26, &FreeSansBold12pt7b, Green);
        c.textCentered("What's in view now is ignored", 160, y + 54, &FreeSans9pt7b, Text);
        c.textCentered("(metal, furniture, walls)", 160, y + 74, &FreeSans9pt7b, TextDim);
        if (learn.gatesSaved < kMaxGates) {
          snprintf(line, sizeof(line), "Saved %d/9 in radar, rest filtered here", learn.gatesSaved);
          smallCentered(c, line, 160, y + 88, Amber);
        }
        smallCentered(c, "Moved it? Learn again: hold BOOT 2 s", 160, y + h - 28, TextDim);
      } else {
        c.textCentered("DIDN'T WORK", 160, y + 26, &FreeSansBold12pt7b, Amber);
        c.textCentered("The radar didn't answer", 160, y + 54, &FreeSans9pt7b, Text);
        smallCentered(c, "Check the wire to IO22 (radar RX),", 160, y + 72, TextDim);
        smallCentered(c, "then hold BOOT 2 s to try again", 160, y + 82, TextDim);
      }
      smallCentered(c, "Tap to continue", 160, y + h - 16, TextDim);
      break;
  }
}

void drawSplash(Canvas8& c, const char* sdLine, Tone sdTone, const char* configLine, const char* buildLine) {
  initTables();
  c.clear(Bg);
  c.textCentered("Human Detector", 160, 70, &FreeSansBold18pt7b, Green);
  c.textCentered("LD2410B, RD-03D or LD2450 radar", 160, 100, &FreeSans9pt7b, Text);
  uint8_t color = sdTone == Tone::Good ? Green : (sdTone == Tone::Warning ? Amber : TextDim);
  c.textCentered(sdLine, 160, 134, &FreeSans9pt7b, color);
  if (configLine && configLine[0]) c.textCentered(configLine, 160, 156, &FreeSans9pt7b, Amber);
  c.textCentered("Tap: LED color   Hold: brightness", 160, 196, &FreeSans9pt7b, TextDim);
  c.textCentered("Colors look wrong? Press BOOT", 160, 220, &FreeSans9pt7b, TextDim);
  if (buildLine && buildLine[0]) smallCentered(c, buildLine, 160, 230, GreenDim);
}

}  // namespace view
