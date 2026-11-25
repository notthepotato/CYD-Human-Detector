#include "tracking_protocol.h"

#include <math.h>
#include <string.h>

namespace {

const uint8_t kHeader[4] = {0xAA, 0xFF, 0x03, 0x00};
const uint8_t kFooter[2] = {0x55, 0xCC};

int16_t signFlag(uint8_t low, uint8_t high) {
  int16_t value = (int16_t)(((high & 0x7F) << 8) | low);
  return (high & 0x80) ? value : (int16_t)-value;
}

}  // namespace

uint16_t Track::distanceCm() const {
  float mm = sqrtf((float)xMm * xMm + (float)yMm * yMm);
  return (uint16_t)(mm / 10.0f + 0.5f);
}

float Track::angleDeg() const { return atan2f((float)xMm, (float)yMm) * 57.29578f; }

int TrackingReading::count() const {
  int n = 0;
  for (const Track& t : tracks) n += t.valid ? 1 : 0;
  return n;
}

int TrackingReading::nearest() const {
  int best = -1;
  for (int i = 0; i < kMaxTracks; i++) {
    if (tracks[i].valid && (best < 0 || tracks[i].distanceCm() < tracks[best].distanceCm())) best = i;
  }
  return best;
}

Track decodeTrack(const uint8_t* s) {
  Track t;
  t.xMm = signFlag(s[0], s[1]);
  t.yMm = signFlag(s[2], s[3]);
  t.speedCmS = signFlag(s[4], s[5]);
  t.resolutionMm = (uint16_t)(s[6] | (s[7] << 8));
  // Empty slots are all zeros. The RD-03D also reports speeds of exactly 248 or 256 cm/s for
  // targets that aren't real (the ESPHome driver drops them too).
  int absSpeed = t.speedCmS < 0 ? -t.speedCmS : t.speedCmS;
  t.valid = (t.xMm != 0 || t.yMm != 0) && t.yMm > 0 && absSpeed != 248 && absSpeed != 256;
  return t;
}

bool TrackingParser::feed(uint8_t byte, TrackingReading& out) {
  // Still matching the header: restart on a mismatch (the byte may itself start a new header).
  if (len_ < sizeof(kHeader)) {
    if (byte == kHeader[len_]) {
      buf_[len_++] = byte;
    } else {
      len_ = (byte == kHeader[0]) ? 1 : 0;
      if (len_) buf_[0] = byte;
    }
    return false;
  }
  buf_[len_++] = byte;
  if (len_ < kFrameLen) return false;
  len_ = 0;
  if (buf_[kFrameLen - 2] != kFooter[0] || buf_[kFrameLen - 1] != kFooter[1]) {
    badFrames_++;
    return false;
  }
  for (int i = 0; i < kMaxTracks; i++) out.tracks[i] = decodeTrack(buf_ + 4 + i * 8);
  return true;
}

size_t trackingCmdMultiTarget(uint8_t* out, size_t outLen) {
  const uint8_t cmd[] = {0xFD, 0xFC, 0xFB, 0xFA, 0x02, 0x00, 0x90, 0x00, 0x04, 0x03, 0x02, 0x01};
  if (outLen < sizeof(cmd)) return 0;
  memcpy(out, cmd, sizeof(cmd));
  return sizeof(cmd);
}
