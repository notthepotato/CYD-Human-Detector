// Ai-Thinker RD-03D and HLK-LD2450 "tracking" radars: up to 3 people, each with a position.
// Both send the same report frame, about 10 times a second at 256000 baud:
//
//   AA FF 03 00 | target 1 (8 bytes) | target 2 | target 3 | 55 CC        (30 bytes)
//
// Each target: X (mm, left/right), Y (mm, straight ahead), speed (cm/s), distance resolution (mm),
// all little-endian. X, Y and speed use a sign flag, not two's complement: top bit set = positive,
// clear = negative, low 15 bits = size. An empty slot is all zeros.
//
// Plain C++ with no Arduino dependencies, so it's unit-tested on a computer (host_tests/).
#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr int kMaxTracks = 3;

// Which kind of radar is connected, told apart by the frames it sends.
enum class RadarKind : uint8_t {
  Presence,  // HLK-LD2410 / LD2410B / LD2410C: distance only
  Tracking,  // Ai-Thinker RD-03D or HLK-LD2450: up to 3 people with positions
};

struct Track {
  bool valid = false;
  int16_t xMm = 0;        // negative = left of the radar, positive = right (as the radar sees it)
  int16_t yMm = 0;        // distance straight ahead
  int16_t speedCmS = 0;   // RD-03D: positive = moving away, negative = coming closer
  uint16_t resolutionMm = 0;

  uint16_t distanceCm() const;  // straight-line distance
  float angleDeg() const;       // 0 = straight ahead, negative = left, positive = right
  bool moving() const { return speedCmS != 0; }
};

struct TrackingReading {
  Track tracks[kMaxTracks];
  int count() const;
  // The nearest valid track's index, or -1.
  int nearest() const;
};

// Turns the radar's byte stream into readings. Resynchronises after garbage or a dropped byte.
class TrackingParser {
 public:
  // Feed one received byte. Returns true when it completed a valid frame (written to `out`).
  bool feed(uint8_t byte, TrackingReading& out);
  uint32_t badFrames() const { return badFrames_; }

  static constexpr size_t kFrameLen = 30;

 private:
  uint8_t buf_[kFrameLen] = {};
  size_t len_ = 0;
  uint32_t badFrames_ = 0;
};

// Decodes one 8-byte target slot.
Track decodeTrack(const uint8_t* slot);

// "Track up to 3 targets": FD FC FB FA 02 00 90 00 04 03 02 01. The RD-03D may start in
// single-target mode; the LD2450 starts in multi-target mode.
size_t trackingCmdMultiTarget(uint8_t* out, size_t outLen);
