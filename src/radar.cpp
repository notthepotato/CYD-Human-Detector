#include "radar.h"

#include <Arduino.h>

#include "pins.h"

namespace radar {
namespace {

HardwareSerial& gPort = Serial2;
const Settings* gSettings = nullptr;
// Both radar families talk at 256000 baud; every byte goes to both parsers, and whichever finds
// valid frames says what's connected.
RadarParser gParser;            // LD2410B
TrackingParser gTrackingParser; // RD-03D / LD2450
RadarReading gLatest;
TrackingReading gTracks;
RadarKind gKind = RadarKind::Presence;
uint32_t gLastFrameMs = 0;
uint32_t gFrames = 0;
uint32_t gLastConfigAttemptMs = 0;
int gConfigAttempts = 0;
bool gSettingsPending = false;  // config.txt asks for range/hold, not written yet
int gMultiTargetSends = 0;      // RD-03D: "track 3 people" still to send
bool gMultiTargetDone = false;  // sent already: only once per power-up, so a radar that pauses or
                                // restarts after it can't end up in a found/lost loop
uint32_t gNextMultiTargetMs = 0;

// Link health, for the Serial Monitor and the NO RADAR screen.
bool gWasConnected = false;
uint32_t gDrops = 0;            // times the radar went quiet after it had been answering
uint32_t gLinkChangeMs = 0;
uint32_t gStatsMs = 0;
uint32_t gStatsFrames = 0;

constexpr uint32_t kLinkTimeoutMs = 3000;  // the radars report ~10 times a second

void pump() {
  while (gPort.available()) {
    uint8_t b = (uint8_t)gPort.read();
    RadarReading r;
    TrackingReading t;
    bool presence = gParser.feed(b, r);
    bool tracking = gTrackingParser.feed(b, t);
    if (!presence && !tracking) continue;
    uint32_t now = millis();
    if (tracking) {
      if (!gMultiTargetDone) {  // first time: ask for multi-target mode (twice, half a second apart)
        gMultiTargetDone = true;
        gMultiTargetSends = 2;
        gNextMultiTargetMs = now;
      }
      gTracks = t;
      gKind = RadarKind::Tracking;
    } else {
      gLatest = r;
      gKind = RadarKind::Presence;
    }
    gLastFrameMs = now ? now : 1;  // 0 means "never"
    gFrames++;
  }
}

// Sends a command and waits for its acknowledgement. True if the radar replied "success".
bool sendAndWait(const uint8_t* bytes, size_t length, uint16_t command, uint32_t timeoutMs) {
  if (length == 0) return false;
  uint32_t acksBefore = gParser.ackCount();
  gPort.write(bytes, length);
  gPort.flush();
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    pump();
    if (gParser.ackCount() != acksBefore && gParser.lastAckCommand() == radarAckFor(command)) {
      return gParser.lastAckStatus() == 0;
    }
    delay(2);
  }
  return false;
}

bool enterConfig() {
  uint8_t buf[16];
  for (int attempt = 0; attempt < 3; attempt++) {
    if (sendAndWait(buf, radarCmdEnableConfig(buf, sizeof(buf)), radarcmd::EnableConfig, 300)) return true;
  }
  return false;
}

void leaveConfig() {
  uint8_t buf[16];
  sendAndWait(buf, radarCmdEndConfig(buf, sizeof(buf)), radarcmd::EndConfig, 300);
}

// Enable config -> range/hold -> end config. Both values are stored in the radar's own flash, so
// this only runs when config.txt sets one of them. Setting one sends both; the other gets the
// factory value (8 gates, 5 s).
bool writeRangeAndHold() {
  if (!enterConfig()) return false;
  uint8_t buf[40];
  uint8_t gate = gSettings->maxGate > 0 ? (uint8_t)gSettings->maxGate : 8;
  uint16_t hold = gSettings->sensorHoldSeconds >= 0 ? (uint16_t)gSettings->sensorHoldSeconds : 5;
  bool ok = sendAndWait(buf, radarCmdMaxGates(gate, gate, hold, buf, sizeof(buf)), radarcmd::MaxGates, 300);
  leaveConfig();
  Serial.printf("Radar range %d gates (%.2f m), hold %u s: %s\n", gate, gate * 0.75, hold, ok ? "saved" : "FAILED");
  return ok;
}

const char* kindName() { return gKind == RadarKind::Tracking ? "RD-03D / LD2450" : "LD2410B"; }

// Reports the radar coming and going, and every 10 s how many reports arrived.
void watchLink(uint32_t nowMs) {
  bool c = connected(nowMs);
  if (c != gWasConnected) {
    if (c) {
      Serial.printf("Radar found: %s (%.1f s after %s)\n", kindName(), (nowMs - gLinkChangeMs) / 1000.0,
                    gDrops ? "it was lost" : "power-on");
    } else {
      gDrops++;
      Serial.printf("Radar LOST (%lu times now): no reports for %.1f s. Reports so far %lu, bad frames %lu\n",
                    (unsigned long)gDrops, (nowMs - gLastFrameMs) / 1000.0, (unsigned long)gFrames,
                    (unsigned long)badFrames());
    }
    gWasConnected = c;
    gLinkChangeMs = nowMs;
  }
  if (nowMs - gStatsMs >= 10000) {
    if (gFrames != gStatsFrames || c) {
      Serial.printf("Radar: %s, %lu reports in the last %.0f s, bad frames %lu, lost %lu times\n", kindName(),
                    (unsigned long)(gFrames - gStatsFrames), (nowMs - gStatsMs) / 1000.0,
                    (unsigned long)badFrames(), (unsigned long)gDrops);
    }
    gStatsMs = nowMs;
    gStatsFrames = gFrames;
  }
}

}  // namespace

void begin(const Settings& settings) {
  gSettings = &settings;
  gPort.setRxBufferSize(1024);
  gPort.begin(256000, SERIAL_8N1, Pin::RadarRx, Pin::RadarTx);
  gSettingsPending = settings.maxGate > 0 || settings.sensorHoldSeconds >= 0;
}

void poll(uint32_t nowMs) {
  pump();
  watchLink(nowMs);
  if (gMultiTargetSends > 0 && (int32_t)(nowMs - gNextMultiTargetMs) >= 0) {
    uint8_t buf[16];
    gPort.write(buf, trackingCmdMultiTarget(buf, sizeof(buf)));
    gMultiTargetSends--;
    gNextMultiTargetMs = nowMs + 500;
  }
  // config.txt asks for an LD2410B range/hold: send it once that radar is answering (not to the
  // other kind), retrying every 10 s, at most 3 times.
  if (gSettingsPending && gConfigAttempts < 3 && connected(nowMs) && gKind == RadarKind::Presence &&
      (gConfigAttempts == 0 || nowMs - gLastConfigAttemptMs > 10000)) {
    gLastConfigAttemptMs = nowMs;
    gConfigAttempts++;
    if (writeRangeAndHold()) gSettingsPending = false;
  }
}

bool setEngineering(bool on) {
  if (!enterConfig()) return false;
  uint8_t buf[16];
  bool ok = sendAndWait(buf, radarCmdEngineering(on, buf, sizeof(buf)), on ? radarcmd::EngineeringOn : radarcmd::EngineeringOff, 300);
  leaveConfig();
  return ok;
}

int writeSensitivity(const uint8_t moving[kMaxGates], const uint8_t still[kMaxGates]) {
  if (!enterConfig()) return 0;
  uint8_t buf[40];
  int saved = 0;
  for (int gate = 0; gate < kMaxGates; gate++) {
    bool ok = sendAndWait(buf, radarCmdGateSensitivity(gate, moving[gate], still[gate], buf, sizeof(buf)),
                          radarcmd::GateSensitivity, 300);
    Serial.printf("Gate %d (%.2f-%.2f m): moving %u, still %u: %s\n", gate, gate * 0.75, (gate + 1) * 0.75,
                  moving[gate], still[gate], ok ? "saved" : "FAILED");
    if (ok) saved++;
  }
  leaveConfig();
  return saved;
}

bool connected(uint32_t nowMs) {
  if (gLastFrameMs == 0) return false;
  // Signed: a report can finish arriving just after the caller read the clock, making it "newer"
  // than nowMs. Unsigned, that wrapped around to ~49 days old and flashed NO RADAR for a frame.
  int32_t ageMs = (int32_t)(nowMs - gLastFrameMs);
  return ageMs < (int32_t)kLinkTimeoutMs;
}

RadarKind kind() { return gKind; }

const RadarReading& latest() { return gLatest; }

const TrackingReading& latestTracks() { return gTracks; }

uint32_t frameCount() { return gFrames; }

uint32_t badFrames() { return gParser.badFrames() + gTrackingParser.badFrames(); }

uint32_t dropCount() { return gDrops; }

}  // namespace radar
