#include "app_theme.h"

#include <cstdio>
#include <cstring>

#include "display.h"
#include "fonts.h"

namespace App::Theme {
namespace {

Palette s_palette{};
bool s_ready = false;
uint16_t s_top_bar = 0;

}  // namespace

const Palette& colors() { return s_palette; }

void init() {
    if (s_ready) return;
    s_ready = true;
    auto& g = Display::canvas();
    s_palette.ink = g.color565(23, 50, 77);
    s_palette.text = g.color565(0, 0, 0);
    s_palette.paper = g.color565(255, 242, 176);
    s_palette.coral = g.color565(240, 90, 86);
    s_palette.coral_dark = g.color565(217, 67, 67);
    s_palette.yellow = g.color565(255, 200, 61);
    s_palette.yellow_light = g.color565(255, 231, 154);
    s_palette.aqua = g.color565(43, 183, 170);
    s_palette.aqua_dark = g.color565(22, 143, 133);
    s_palette.blue = g.color565(60, 120, 216);
    s_palette.track = g.color565(212, 221, 225);
    s_palette.track_border = g.color565(164, 178, 185);
    s_palette.button = g.color565(49, 90, 130);
    s_palette.button_border = g.color565(111, 149, 184);
    s_palette.green = g.color565(73, 185, 92);
    s_palette.green_dark = g.color565(35, 123, 49);
    s_palette.card_light = g.color565(191, 244, 186);
    s_palette.card_dark = g.color565(45, 206, 27);
    s_top_bar = s_palette.coral;
}

uint16_t rgb(uint32_t value) {
    return Display::canvas().color565((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
}

void setTopBarColor(uint16_t value) { s_top_bar = value; }
uint16_t topBarColor() { return s_top_bar; }

void fitText(lgfx::LGFXBase& g, const char* text, int max_width, char* out, std::size_t capacity) {
    if (capacity == 0) return;
    if (!text) text = "";
    std::snprintf(out, capacity, "%s", text);
    if (g.textWidth(out) <= max_width) return;

    // Drop whole UTF-8 sequences (not bytes) until "…" fits, so CJK titles are
    // never cut in the middle of a code point.
    char probe[192];
    std::size_t end = std::strlen(out);
    while (end > 0) {
        std::size_t cut = end - 1;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u) --cut;
        std::snprintf(probe, sizeof(probe), "%.*s…", static_cast<int>(cut), out);
        if (g.textWidth(probe) <= max_width) {
            std::snprintf(out, capacity, "%s", probe);
            return;
        }
        end = cut;
    }
    std::snprintf(out, capacity, "…");
}

void drawSmileFace(lgfx::LGFXBase& g, int cx, int cy, int radius) {
    const auto& c = colors();
    g.fillCircle(cx, cy, radius, c.yellow);
    g.drawCircle(cx, cy, radius, TFT_WHITE);
    g.drawCircle(cx, cy, radius - 1, TFT_WHITE);
    int eye = radius / 5;
    if (eye < 2) eye = 2;
    g.fillCircle(cx - radius / 2, cy - radius / 3, eye, c.ink);
    g.fillCircle(cx + radius / 2, cy - radius / 3, eye, c.ink);
    const int mouth = radius * 2 / 3;
    g.fillRect(cx - mouth / 2, cy + radius / 4, mouth, 2, c.ink);
    g.fillRect(cx - mouth / 2 - 1, cy + radius / 4 - 1, 2, 2, c.ink);
    g.fillRect(cx + mouth / 2 - 1, cy + radius / 4 - 1, 2, 2, c.ink);
}

void drawHallIcon(lgfx::LGFXBase& g, bool detected, uint16_t background) {
    const auto& c = colors();
    constexpr int x = 298, y = 10;
    g.fillRect(x - 2, y - 2, 22, 17, background);
    if (!detected) return;
    g.fillRoundRect(x, y, 18, 13, 3, c.card_light);
    g.fillRoundRect(x + 2, y + 3, 8, 8, 2, c.card_dark);
    g.fillCircle(x + 6, y + 7, 2, c.card_light);
    g.fillRoundRect(x + 11, y + 5, 5, 3, 1, c.card_dark);
}

void drawChildLockBadge(lgfx::LGFXBase& g, bool locked, uint16_t background) {
    const auto& c = colors();
    g.fillRect(128, 3, 98, 24, background);
    if (!locked) return;
    g.fillRoundRect(130, 5, 60, 20, 8, c.yellow);
    g.setFont(Fonts::small());
    g.setTextColor(c.ink, c.yellow);
    g.drawCenterString("童锁", 160, 8);
}

}  // namespace App::Theme
