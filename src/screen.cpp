#include "screen.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Arduino.h>
#include <SPI.h>

#include "canvas8.h"
#include "pins.h"

namespace screen {
namespace {

SPIClass gTftSpi(HSPI);
Adafruit_ILI9341 tft(&gTftSpi, Pin::TftDc, Pin::TftCs, -1);

Canvas8 gCanvas;
bool gCanvasOk = false;
uint32_t gRowHash[Canvas8::H];  // what each row looked like when it was last sent
uint16_t gLine[Canvas8::W];

constexpr uint32_t kFrameMs = 33;  // ~30 frames a second
uint32_t gLastFrameMs = 0;
float gDotCm = 0;  // LD2410B: where the dot is drawn: glides toward the latest distance
view::TrackAnim gTracks;  // RD-03D / LD2450: the dots and their trails
uint32_t gLastFrames = 0;  // radar reports seen at the last frame

uint32_t hashRow(const uint8_t* p) {
  uint32_t h = 2166136261u;  // FNV-1a
  for (int i = 0; i < Canvas8::W; i++) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

void sendRows(int y, int count) {
  const uint16_t* palette = view::palette565();
  tft.setAddrWindow(0, y, Canvas8::W, count);
  for (int i = 0; i < count; i++) {
    const uint8_t* src = gCanvas.row(y + i);
    for (int x = 0; x < Canvas8::W; x++) gLine[x] = palette[src[x]];
    tft.writePixels(gLine, Canvas8::W);
  }
}

// Sends the rows that changed since the last frame (or every row).
void push(bool all) {
  tft.startWrite();
  int runStart = -1;
  for (int y = 0; y <= Canvas8::H; y++) {
    bool changed = false;
    if (y < Canvas8::H) {
      uint32_t h = hashRow(gCanvas.row(y));
      changed = all || h != gRowHash[y];
      gRowHash[y] = h;
    }
    if (changed && runStart < 0) runStart = y;
    if (!changed && runStart >= 0) {
      sendRows(runStart, y - runStart);
      runStart = -1;
    }
  }
  tft.endWrite();
}

// Only if the 75 KB screen image couldn't be allocated (it should always fit).
void showMemoryError() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_YELLOW);
  tft.setTextSize(2);
  tft.setCursor(10, 100);
  tft.print("Not enough memory");
  tft.setCursor(10, 124);
  tft.print("for the screen");
}

}  // namespace

void begin(uint8_t rotation, bool invertColors, uint8_t brightness) {
  gTftSpi.begin(Pin::TftSck, Pin::TftMiso, Pin::TftMosi, Pin::TftCs);
  tft.begin();
  tft.setRotation(rotation);
  tft.invertDisplay(invertColors);
  ledcAttach(Pin::TftBacklight, 5000, 8);
  setBrightness(brightness);
  gCanvasOk = gCanvas.begin();
  if (!gCanvasOk) {
    Serial.println("Not enough memory for the screen image");
    showMemoryError();
  }
}

void setInvert(bool invertColors) { tft.invertDisplay(invertColors); }

void setBrightness(uint8_t brightness) { ledcWrite(Pin::TftBacklight, brightness); }

void showSplash(const char* sdLine, Tone sdTone, const char* configLine, const char* buildLine) {
  if (!gCanvasOk) return;
  view::drawSplash(gCanvas, sdLine, sdTone, configLine, buildLine);
  push(true);
}

void update(const View& v) {
  uint32_t now = millis();
  uint32_t dt = now - gLastFrameMs;
  if (dt < kFrameMs) return;
  gLastFrameMs = now;

  if (!v.radarConnected || v.targetCm == 0) {
    gDotCm = 0;
  } else if (gDotCm <= 0) {
    gDotCm = v.targetCm;
  } else {
    float k = dt / 150.0f;  // glide: the radar reports in small jumps ~10 times a second
    gDotCm += (v.targetCm - gDotCm) * (k > 1 ? 1 : k);
  }

  bool newReport = v.frames != gLastFrames;
  gLastFrames = v.frames;
  if (v.radarConnected && v.kind == RadarKind::Tracking) {
    view::stepTracks(gTracks, v.tracks, newReport, dt);
  } else {
    gTracks = view::TrackAnim();
  }

  if (!gCanvasOk) return;
  static view::Anim anim;  // static: keeps the trails off the stack
  anim.nowMs = now;
  anim.dotCm = gDotCm;
  anim.tracks = gTracks;
  view::drawRadar(gCanvas, v, anim);
  push(false);
}

void showLearn(const view::Learn& learn, bool now) {
  uint32_t t = millis();
  if (!now && t - gLastFrameMs < kFrameMs) return;
  gLastFrameMs = t;
  if (!gCanvasOk) return;
  view::drawLearn(gCanvas, learn);
  push(false);
}

void redrawAll(const View& v) {
  gLastFrameMs = millis() - kFrameMs;
  update(v);
  if (gCanvasOk) push(true);
}

}  // namespace screen
