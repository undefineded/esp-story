#pragma once
#include <cstddef>
#include <cstdint>

#include "LovyanGFX.hpp"

namespace App::Theme {

// StoryBox palette ported from the Arduino UI (src/ui.cpp, taken from ui.html).
struct Palette {
    uint16_t ink;           // #17324d outlines, icons, shadows
    uint16_t text;          // #000000 body text
    uint16_t paper;         // #fff2b0 page background
    uint16_t coral;         // #f05a56 top bar
    uint16_t coral_dark;    // #d94343 top bar edge
    uint16_t yellow;        // #ffc83d accent
    uint16_t yellow_light;  // #ffe79a button rim
    uint16_t aqua;          // #2bb7aa category badge
    uint16_t aqua_dark;     // #168f85
    uint16_t blue;          // #3c78d8 progress
    uint16_t track;         // #d4dde1 progress groove
    uint16_t track_border;  // #a4b2b9
    uint16_t button;        // #315a82
    uint16_t button_border; // #6f95b8
    uint16_t card_light;    // #bff4ba hall card body
    uint16_t card_dark;     // #2dce1b hall card chip
};

const Palette& colors();

// Must be called after Display::begin(): LovyanGFX converts RGB888 per panel.
void init();

// 0xRRGGBB -> panel colour.
uint16_t rgb(uint32_t value);

// Colour of the bar currently drawn at y=0..30; the hall icon and the child
// lock badge repaint their own background, so they need to know it.
void setTopBarColor(uint16_t value);
uint16_t topBarColor();

// UTF-8 safe truncation with an ellipsis, sized by rendered width.
void fitText(lgfx::LGFXBase& g, const char* text, int max_width, char* out, std::size_t capacity);

// Brand smiley: yellow face on the current top bar.
void drawSmileFace(lgfx::LGFXBase& g, int cx, int cy, int radius);

// Card-inserted indicator (top right), background colour behind it.
void drawHallIcon(lgfx::LGFXBase& g, bool detected, uint16_t background);
// "童锁" pill between the brand and the volume readout.
void drawChildLockBadge(lgfx::LGFXBase& g, bool locked, uint16_t background);

}
