#include "canvas8.h"

#include <stdlib.h>
#include <string.h>

namespace {
// Adafruit GFX's classic 5x7 font (`font[]`, 5 column bytes per character, bit 0 = top row).
#include <glcdfont.c>
}  // namespace

bool Canvas8::begin() {
  if (ready()) return true;
  for (int i = 0; i < kBands; i++) {
    bands_[i] = (uint8_t*)malloc(W * kBandRows);
    if (!bands_[i]) {
      for (int j = 0; j < i; j++) {
        free(bands_[j]);
        bands_[j] = nullptr;
      }
      return false;
    }
  }
  clear(0);
  return true;
}

void Canvas8::clear(uint8_t color) {
  for (int i = 0; i < kBands; i++) memset(bands_[i], color, W * kBandRows);
}

void Canvas8::hline(int x, int y, int w, uint8_t color) {
  if ((unsigned)y >= (unsigned)H) return;
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (x + w > W) w = W - x;
  if (w > 0) memset(row(y) + x, color, w);
}

void Canvas8::vline(int x, int y, int h, uint8_t color) {
  for (int i = 0; i < h; i++) pixel(x, y + i, color);
}

void Canvas8::fillRect(int x, int y, int w, int h, uint8_t color) {
  for (int i = 0; i < h; i++) hline(x, y + i, w, color);
}

void Canvas8::rect(int x, int y, int w, int h, uint8_t color, int thickness) {
  for (int t = 0; t < thickness; t++) {
    hline(x + t, y + t, w - 2 * t, color);
    hline(x + t, y + h - 1 - t, w - 2 * t, color);
    vline(x + t, y + t, h - 2 * t, color);
    vline(x + w - 1 - t, y + t, h - 2 * t, color);
  }
}

void Canvas8::line(int x0, int y0, int x1, int y1, uint8_t color, int thickness) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (thickness <= 1) {
      pixel(x0, y0, color);
    } else if (thickness == 2) {
      fillRect(x0, y0, 2, 2, color);
    } else {
      fillRect(x0 - 1, y0 - 1, 3, 3, color);
    }
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

void Canvas8::fillCircle(int cx, int cy, int r, uint8_t color) { ring(cx, cy, -1.0f, r + 0.4f, color); }

void Canvas8::ring(int cx, int cy, float inner, float outer, uint8_t color) {
  int reach = (int)outer + 1;
  float in2 = inner < 0 ? -1.0f : inner * inner, out2 = outer * outer;
  for (int dy = -reach; dy <= reach; dy++) {
    for (int dx = -reach; dx <= reach; dx++) {
      float d2 = (float)(dx * dx + dy * dy);
      if (d2 >= in2 && d2 <= out2) pixel(cx + dx, cy + dy, color);
    }
  }
}

void Canvas8::text(const char* s, int x, int baseline, const GFXfont* font, uint8_t color) {
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (c < font->first || c > font->last) continue;
    const GFXglyph& g = font->glyph[c - font->first];
    const uint8_t* bits = font->bitmap + g.bitmapOffset;
    int bit = 0;
    for (int yy = 0; yy < g.height; yy++) {
      for (int xx = 0; xx < g.width; xx++, bit++) {
        if (bits[bit >> 3] & (0x80 >> (bit & 7))) pixel(x + g.xOffset + xx, baseline + g.yOffset + yy, color);
      }
    }
    x += g.xAdvance;
  }
}

int Canvas8::textWidth(const char* s, const GFXfont* font) {
  int x = 0, right = 0;
  for (; *s; s++) {
    uint8_t c = (uint8_t)*s;
    if (c < font->first || c > font->last) continue;
    const GFXglyph& g = font->glyph[c - font->first];
    if (g.width > 0 && x + g.xOffset + g.width > right) right = x + g.xOffset + g.width;
    x += g.xAdvance;
  }
  return right;
}

void Canvas8::textCentered(const char* s, int centerX, int baseline, const GFXfont* font, uint8_t color) {
  text(s, centerX - textWidth(s, font) / 2, baseline, font, color);
}

void Canvas8::small(const char* s, int x, int top, uint8_t color) {
  for (; *s; s++, x += 6) {
    uint8_t c = (uint8_t)*s;
    for (int col = 0; col < 5; col++) {
      uint8_t bitsCol = font[c * 5 + col];
      for (int r = 0; r < 8; r++) {
        if (bitsCol & (1 << r)) pixel(x + col, top + r, color);
      }
    }
  }
}

int Canvas8::smallWidth(const char* s) {
  size_t n = strlen(s);
  return n ? (int)n * 6 - 1 : 0;
}
