// Talks to the radar over UART2 (256000 baud) and keeps the latest reading. Works out which kind
// is connected from the frames it sends: LD2410B (distance only) or RD-03D / LD2450 (positions).
#pragma once
#include <stdint.h>

#include "detector.h"
#include "radar_protocol.h"
#include "tracking_protocol.h"

namespace radar {

// Starts the serial link. Writes sensor_range / sensor_hold_seconds into an LD2410B if config.txt
// sets them, once it answers. Asks an RD-03D to track 3 people when it connects.
void begin(const Settings& settings);

// Reads whatever has arrived. Call every loop.
void poll(uint32_t nowMs);

// True while reports keep arriving (the radar sends about 10 a second; 3 s without one = lost).
bool connected(uint32_t nowMs);

// LD2410B only. "Learn the room": per-distance reporting on/off (needed to measure the background), and writing
// the learned trigger levels into the radar. Each takes up to a second or two (the screen pauses).
bool setEngineering(bool on);
// Returns how many of the 9 gates the radar accepted.
int writeSensitivity(const uint8_t moving[kMaxGates], const uint8_t still[kMaxGates]);

// What's connected (LD2410B until a tracking radar's frames arrive).
RadarKind kind();
const RadarReading& latest();          // LD2410B
const TrackingReading& latestTracks(); // RD-03D / LD2450
uint32_t frameCount();
uint32_t badFrames();
// How many times the radar went quiet after it had been answering (loose wire, power dips...).
uint32_t dropCount();

}  // namespace radar
