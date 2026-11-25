// The 2.8" 320x240 display: the radar-style screen from radar_view, sent to the ILI9341.
//
// Each frame is drawn in memory (Canvas8), then only the rows that changed since the last frame are
// sent to the display, so the moving ping stays smooth and nothing flickers.
#pragma once
#include <stdint.h>

#include "radar_view.h"

namespace screen {

using View = view::Info;
using Tone = view::Tone;

void begin(uint8_t rotation, bool invertColors, uint8_t brightness);
void setInvert(bool invertColors);
void setBrightness(uint8_t brightness);

void showSplash(const char* sdLine, Tone sdTone, const char* configLine, const char* buildLine);

// Draws a new frame (about 30 a second) when it's time. Call every loop.
void update(const View& view);
// Draws a frame now and resends the whole screen.
void redrawAll(const View& view);

// "Learn the room" screens instead of the radar. `now`: draw immediately (before a slow step).
void showLearn(const view::Learn& learn, bool now);

}  // namespace screen
