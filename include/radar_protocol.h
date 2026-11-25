// HLK-LD2410 / LD2410B / LD2410C serial protocol (Hi-Link "Serial communication protocol v1.04").
// Plain C++ with no Arduino dependencies, so it's unit-tested on a computer (host_tests/).
#pragma once
#include <stddef.h>
#include <stdint.h>

constexpr int kMaxGates = 9;          // gates 0..8, 0.75 m each
constexpr uint16_t kGateCm = 75;

// One report from the radar.
struct RadarReading {
  uint8_t state = 0;          // 0 nobody, 1 moving, 2 still, 3 moving + still
  uint16_t movingCm = 0;
  uint8_t movingEnergy = 0;   // 0..100
  uint16_t stillCm = 0;
  uint8_t stillEnergy = 0;    // 0..100
  uint16_t detectCm = 0;

  // Engineering mode only: energy per 0.75 m distance gate.
  bool engineering = false;
  uint8_t gateCount = 0;      // how many entries below are valid (max gate + 1)
  uint8_t movingGates[kMaxGates] = {};
  uint8_t stillGates[kMaxGates] = {};

  bool present() const { return state != 0; }
  bool moving() const { return state & 1; }
  bool still() const { return state & 2; }

  // Distance to the nearest detected person, or 0 when nobody is detected.
  uint16_t nearestCm() const;
};

// Turns the radar's byte stream into readings. Resynchronises after garbage or a dropped byte.
class RadarParser {
 public:
  // Feed one received byte. Returns true when it completed a valid data frame (written to `out`).
  bool feed(uint8_t byte, RadarReading& out);

  // Most recent command acknowledgement from the radar.
  uint16_t lastAckCommand() const { return lastAckCommand_; }
  uint16_t lastAckStatus() const { return lastAckStatus_; }
  uint32_t ackCount() const { return ackCount_; }
  uint32_t badFrames() const { return badFrames_; }

 private:
  enum class Header { Mismatch, Partial, Data, Ack };
  Header matchHeader() const;
  void drop(size_t n);
  bool parseData(const uint8_t* p, size_t n, RadarReading& out);
  void parseAck(const uint8_t* p, size_t n);

  static constexpr size_t kMaxPayload = 64;
  uint8_t buf_[4 + 2 + kMaxPayload + 4] = {};
  size_t len_ = 0;
  uint16_t lastAckCommand_ = 0;
  uint16_t lastAckStatus_ = 0xFFFF;
  uint32_t ackCount_ = 0;
  uint32_t badFrames_ = 0;
};

// Command frames. Each writes into `out` and returns the length (0 if `outLen` is too small).
// Every command must be sent between enableConfig and endConfig.
size_t radarCmdEnableConfig(uint8_t* out, size_t outLen);
size_t radarCmdEndConfig(uint8_t* out, size_t outLen);
size_t radarCmdEngineering(bool on, uint8_t* out, size_t outLen);
// Max moving/still gate (2..8) and how long it keeps reporting "person" after they leave
// (seconds). Saved in the radar itself, so it survives power-off.
size_t radarCmdMaxGates(uint8_t maxMovingGate, uint8_t maxStillGate, uint16_t holdSeconds, uint8_t* out,
                        size_t outLen);

// Trigger levels (0..100) for one distance gate: energy must go above these to count as a moving /
// still target there. Saved in the radar. (Hi-Link: the still level of gates 0 and 1 can't be set.)
size_t radarCmdGateSensitivity(uint8_t gate, uint8_t moving, uint8_t still, uint8_t* out, size_t outLen);

// The ACK command word the radar replies with for `command`.
constexpr uint16_t radarAckFor(uint16_t command) { return command | 0x0100; }

namespace radarcmd {
constexpr uint16_t EnableConfig = 0x00FF;
constexpr uint16_t EndConfig = 0x00FE;
constexpr uint16_t MaxGates = 0x0060;
constexpr uint16_t GateSensitivity = 0x0064;
constexpr uint16_t EngineeringOn = 0x0062;
constexpr uint16_t EngineeringOff = 0x0063;
}  // namespace radarcmd
