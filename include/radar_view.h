// The radar-style screen: a green ±60° fan (the radar's field of view) with range rings, an
// expanding "ping", and red dots for the people it detects. Drawn into a Canvas8.
//
// RD-03D / LD2450: a dot per person (up to 3) at their real position, with a fading trail.
// LD2410B: distance only, not direction, so its one dot sits on the center line and a faint arc
// across the fan marks "somewhere at this distance".
//
// Plain C++ (no Arduino headers) so the README screenshots are rendered by this exact code on a PC.
#pragma once
#include <stdint.h>

#include "canvas8.h"
#include "detector.h"
#include "radar_protocol.h"
#include "tracking_protocol.h"

namespace view {

// Palette indices. palette565() gives each one's RGB565 color for the display.
enum Color : uint8_t {
  Bg,
  Panel,
  Grid,
  GreenDim,
  Green,
  GreenHi,
  Ping2,
  Ping3,
  Text,
  TextDim,
  Red,
  RedHi,
  RedDim,
  RedArc,
  Amber,
  AmberHi,
  AmberDim,
  BoxGreen,
  BoxDark,
  BoxRed,
  BoxRedHi,
  White,
  TrailOld,  // a tracked person's trail, oldest to newest
  TrailMid,
  TrailNew,
  LedBlueText,
  LedYellowText,
  kColorCount
};

const uint16_t* palette565();
void paletteRgb(uint8_t color, uint8_t& r, uint8_t& g, uint8_t& b);

enum class Sd : uint8_t { NoCard, Logging, LogOff, Error };

// Everything the main loop knows that the screen shows.
struct Info {
  bool radarConnected = false;
  RadarKind kind = RadarKind::Presence;
  TrackingReading tracks;       // RD-03D / LD2450: everyone it sees (already mirrored if set)
  bool trails = true;           // RD-03D / LD2450: draw trails behind people
  uint8_t ledChoice = 0;        // the LED on the back: 0 off, 1 red, 2 green, 3 blue, 4 yellow
  Sd sd = Sd::NoCard;
  bool useFeet = true;
  uint16_t alertCm = 600;
  Targets targets;              // the radar's targets after "moving only" and the learned background
  uint16_t targetCm = 0;        // the person's (smoothed) distance (0 = nobody)
  bool movingOnly = false;      // "moving only" mode: still targets are ignored
  bool roomLearned = false;
  bool present = false;         // someone within the alert distance (after the clear delay)
  uint32_t visitSeconds = 0;    // how long they've been here
  uint32_t visits = 0;
  uint32_t sinceLastSeenS = 0;
  uint32_t frames = 0;          // reports received from the radar
  uint32_t linkDrops = 0;       // times the radar went quiet after answering
  // A short notice over the fan (e.g. after switching modes). nullptr = none.
  const char* noticeTitle = nullptr;
  const char* noticeLine = nullptr;
};

// RD-03D / LD2450 dots: each glides toward its latest position and leaves a short trail.
constexpr int kTrailLen = 18;  // reports (~1.8 s)
struct TrackAnim {
  struct Dot {
    bool active = false;
    float xMm = 0, yMm = 0;  // where it's drawn
    int trailLen = 0;        // oldest first
    int16_t trailX[kTrailLen] = {}, trailY[kTrailLen] = {};
  } dots[kMaxTracks];
};
// Call every frame. `newReport`: a radar report arrived since the last call (adds a trail point).
void stepTracks(TrackAnim& anim, const TrackingReading& reading, bool newReport, uint32_t dtMs);

// Animation state, kept by the screen code.
struct Anim {
  uint32_t nowMs = 0;  // drives the ping and the dot's glow
  float dotCm = 0;     // LD2410B: the dot's (smoothed) distance, 0 = no dot
  TrackAnim tracks;    // RD-03D / LD2450
};

// Ping timing: it sweeps from the base to the outer ring in kPingSweepMs, once every kPingPeriodMs.
constexpr uint32_t kPingPeriodMs = 2200;
constexpr uint32_t kPingSweepMs = 1700;

void drawRadar(Canvas8& c, const Info& info, const Anim& anim);

// "Learn the room" screens.
struct Learn {
  enum class Phase : uint8_t { Countdown, Measuring, Saving, Done } phase = Phase::Countdown;
  uint32_t nowMs = 0;
  int secondsLeft = 0;
  float progress = 0;     // Measuring: 0..1
  bool ok = false;        // Done: saved
  bool forgot = false;    // Done: "forget the room" instead of learning
  int gatesSaved = 0;     // Done: how many of the 9 distance steps the radar accepted
  bool feet = true;
};
void drawLearn(Canvas8& c, const Learn& learn);

enum class Tone : uint8_t { Good, Dim, Warning };
// `buildLine`: when this firmware was built, so you can tell which version is on the board.
void drawSplash(Canvas8& c, const char* sdLine, Tone sdTone, const char* configLine, const char* buildLine);

// "1.27m", "4.2ft", "12ft"
void formatShort(uint16_t cm, bool useFeet, char* out, int outLen, bool twoDecimals);

}  // namespace view
