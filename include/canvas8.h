// A 320x240 off-screen image with one byte (a palette index) per pixel. Each frame is drawn here
// first and then sent to the display, so the moving radar ping never flickers.
//
// Plain C++ (no Arduino headers): the same drawing code renders the README screenshots on a PC.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <gfxfont.h>  // from the Adafruit GFX library (font format only)

class Canvas8 {
 public:
  static constexpr int W = 320, H = 240;

  // Allocates the image (75 KB) in six 12.5 KB blocks, so it never needs one huge free block.
  bool begin();
  bool ready() const { return bands_[0] != nullptr; }

  uint8_t* row(int y) { return bands_[y / kBandRows] + (y % kBandRows) * W; }
  const uint8_t* row(int y) const { return bands_[y / kBandRows] + (y % kBandRows) * W; }

  void clear(uint8_t color);
  void pixel(int x, int y, uint8_t color) {
    if ((unsigned)x < (unsigned)W && (unsigned)y < (unsigned)H) row(y)[x] = color;
  }
  void hline(int x, int y, int w, uint8_t color);
  void vline(int x, int y, int h, uint8_t color);
  void fillRect(int x, int y, int w, int h, uint8_t color);
  void rect(int x, int y, int w, int h, uint8_t color, int thickness = 1);
  // `thickness` 1..3: each point of the line becomes a small square.
  void line(int x0, int y0, int x1, int y1, uint8_t color, int thickness = 1);
  void fillCircle(int cx, int cy, int r, uint8_t color);
  // A ring between radius `inner` and `outer` (pixel centers), centered on a pixel.
  void ring(int cx, int cy, float inner, float outer, uint8_t color);

  // Text in an Adafruit GFX font. `baseline` is the y of the text's baseline.
  void text(const char* s, int x, int baseline, const GFXfont* font, uint8_t color);
  static int textWidth(const char* s, const GFXfont* font);
  void textCentered(const char* s, int centerX, int baseline, const GFXfont* font, uint8_t color);

  // The classic 5x7 pixel font: 6 px per character, 8 px per line. `top` is the top of the text.
  void small(const char* s, int x, int top, uint8_t color);
  static int smallWidth(const char* s);

 private:
  static constexpr int kBandRows = 40;
  static constexpr int kBands = H / kBandRows;
  uint8_t* bands_[kBands] = {};
};
