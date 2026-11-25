#include "radar_protocol.h"

#include <string.h>

namespace {

const uint8_t kDataHeader[4] = {0xF4, 0xF3, 0xF2, 0xF1};
const uint8_t kDataFooter[4] = {0xF8, 0xF7, 0xF6, 0xF5};
const uint8_t kCmdHeader[4] = {0xFD, 0xFC, 0xFB, 0xFA};
const uint8_t kCmdFooter[4] = {0x04, 0x03, 0x02, 0x01};

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

size_t buildCommand(uint16_t command, const uint8_t* value, size_t valueLen, uint8_t* out, size_t outLen) {
  size_t payload = 2 + valueLen;
  size_t total = 4 + 2 + payload + 4;
  if (outLen < total) return 0;
  memcpy(out, kCmdHeader, 4);
  out[4] = (uint8_t)(payload & 0xFF);
  out[5] = (uint8_t)(payload >> 8);
  out[6] = (uint8_t)(command & 0xFF);
  out[7] = (uint8_t)(command >> 8);
  if (valueLen) memcpy(out + 8, value, valueLen);
  memcpy(out + 8 + valueLen, kCmdFooter, 4);
  return total;
}

}  // namespace

uint16_t RadarReading::nearestCm() const {
  uint16_t best = 0;
  if (moving()) best = movingCm;
  if (still() && (best == 0 || (stillCm > 0 && stillCm < best))) best = stillCm;
  if (present() && best == 0) best = detectCm;
  return best;
}

RadarParser::Header RadarParser::matchHeader() const {
  size_t n = len_ < 4 ? len_ : 4;
  bool data = true, ack = true;
  for (size_t i = 0; i < n; i++) {
    data = data && buf_[i] == kDataHeader[i];
    ack = ack && buf_[i] == kCmdHeader[i];
  }
  if (!data && !ack) return Header::Mismatch;
  if (n < 4) return Header::Partial;
  return data ? Header::Data : Header::Ack;
}

void RadarParser::drop(size_t n) {
  if (n >= len_) {
    len_ = 0;
    return;
  }
  memmove(buf_, buf_ + n, len_ - n);
  len_ -= n;
}

bool RadarParser::feed(uint8_t byte, RadarReading& out) {
  if (len_ == sizeof(buf_)) drop(1);
  buf_[len_++] = byte;

  while (len_ > 0) {
    Header header = matchHeader();
    if (header == Header::Mismatch) {
      drop(1);
      continue;
    }
    if (header == Header::Partial || len_ < 6) return false;

    uint16_t payload = le16(buf_ + 4);
    if (payload > kMaxPayload) {  // can't be a real frame: resync
      badFrames_++;
      drop(1);
      continue;
    }
    size_t total = 4 + 2 + payload + 4;
    if (len_ < total) return false;

    const uint8_t* footer = header == Header::Data ? kDataFooter : kCmdFooter;
    if (memcmp(buf_ + total - 4, footer, 4) != 0) {
      badFrames_++;
      drop(1);
      continue;
    }

    bool gotReading = false;
    if (header == Header::Data) {
      gotReading = parseData(buf_ + 6, payload, out);
      if (!gotReading) badFrames_++;
    } else {
      parseAck(buf_ + 6, payload);
    }
    drop(total);
    if (gotReading) return true;
  }
  return false;
}

bool RadarParser::parseData(const uint8_t* p, size_t n, RadarReading& out) {
  // [type][0xAA][state][moveCm x2][moveE][stillCm x2][stillE][detectCm x2] ... [0x55][0x00]
  if (n < 13) return false;
  uint8_t type = p[0];
  if ((type != 0x01 && type != 0x02) || p[1] != 0xAA) return false;
  if (p[n - 2] != 0x55 || p[n - 1] != 0x00) return false;

  RadarReading r;
  r.state = p[2] & 0x03;
  r.movingCm = le16(p + 3);
  r.movingEnergy = p[5];
  r.stillCm = le16(p + 6);
  r.stillEnergy = p[8];
  r.detectCm = le16(p + 9);

  if (type == 0x01) {
    // [maxMovingGate N][maxStillGate M][moving energy x (N+1)][still energy x (M+1)][reserved]
    if (n < 13 + 2) return false;
    uint8_t movingMax = p[11];
    uint8_t stillMax = p[12];
    if (movingMax >= kMaxGates || stillMax >= kMaxGates) return false;
    size_t need = 13 + (size_t)(movingMax + 1) + (size_t)(stillMax + 1) + 2;
    if (need > n) return false;
    const uint8_t* moving = p + 13;
    const uint8_t* still = moving + movingMax + 1;
    r.engineering = true;
    r.gateCount = (uint8_t)((movingMax > stillMax ? movingMax : stillMax) + 1);
    for (int i = 0; i <= movingMax; i++) r.movingGates[i] = moving[i];
    for (int i = 0; i <= stillMax; i++) r.stillGates[i] = still[i];
  }
  out = r;
  return true;
}

void RadarParser::parseAck(const uint8_t* p, size_t n) {
  if (n < 4) return;
  lastAckCommand_ = le16(p);
  lastAckStatus_ = le16(p + 2);
  ackCount_++;
}

size_t radarCmdEnableConfig(uint8_t* out, size_t outLen) {
  const uint8_t value[2] = {0x01, 0x00};
  return buildCommand(radarcmd::EnableConfig, value, 2, out, outLen);
}

size_t radarCmdEndConfig(uint8_t* out, size_t outLen) {
  return buildCommand(radarcmd::EndConfig, nullptr, 0, out, outLen);
}

size_t radarCmdEngineering(bool on, uint8_t* out, size_t outLen) {
  return buildCommand(on ? radarcmd::EngineeringOn : radarcmd::EngineeringOff, nullptr, 0, out, outLen);
}

namespace {
// Commands with three [parameter word (2 bytes)][value (4 bytes)] blocks, parameter words
// 0x0000, 0x0001, 0x0002, all little-endian.
size_t threeParameterCommand(uint16_t command, uint32_t a, uint32_t b, uint32_t c, uint8_t* out, size_t outLen) {
  uint8_t value[18] = {};
  const uint32_t values[3] = {a, b, c};
  for (int i = 0; i < 3; i++) {
    uint8_t* block = value + i * 6;
    block[0] = (uint8_t)i;
    block[1] = 0;
    block[2] = (uint8_t)(values[i] & 0xFF);
    block[3] = (uint8_t)((values[i] >> 8) & 0xFF);
    block[4] = (uint8_t)((values[i] >> 16) & 0xFF);
    block[5] = (uint8_t)((values[i] >> 24) & 0xFF);
  }
  return buildCommand(command, value, sizeof(value), out, outLen);
}
}  // namespace

size_t radarCmdMaxGates(uint8_t maxMovingGate, uint8_t maxStillGate, uint16_t holdSeconds, uint8_t* out,
                        size_t outLen) {
  return threeParameterCommand(radarcmd::MaxGates, maxMovingGate, maxStillGate, holdSeconds, out, outLen);
}

size_t radarCmdGateSensitivity(uint8_t gate, uint8_t moving, uint8_t still, uint8_t* out, size_t outLen) {
  return threeParameterCommand(radarcmd::GateSensitivity, gate, moving, still, out, outLen);
}
