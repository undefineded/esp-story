#include "player_view.h"

#include <cstdio>
#include <cstring>

#include "app_context.h"
#include "app_theme.h"
#include "display.h"
#include "fonts.h"
#include "render_math.h"

// The shared player chrome, ported from the Arduino UI (src/ui.cpp). Both the
// music player and the audio-story player use this so the two pages cannot drift
// apart again.
namespace App {
namespace {

constexpr int kInfoX = 112;
constexpr int kInfoW = 198;
constexpr int kTitleHeight = 22;

}  // namespace

void PlayerView::reset(uint32_t now) {
    title_.clear();
    title_width_ = 0;
    scroll_ = 0;
    title_epoch_ = now;
    dirty_ = true;
}

void PlayerView::tick(const PlayerViewData& data, uint32_t now) {
    const char* title = data.title ? data.title : "";
    if (title_ != title) {
        auto& g = Display::canvas();
        title_ = title;
        g.setFont(Fonts::title());
        title_width_ = g.textWidth(title_.c_str());
        scroll_ = 0;
        title_epoch_ = now;
        dirty_ = true;
        return;
    }
    if (title_width_ > kInfoW) {
        const int offset = RenderMath::scrollOffset(title_width_, kInfoW, now - title_epoch_);
        if (offset != scroll_) {
            scroll_ = offset;
            dirty_ = true;
        }
    }
}

void PlayerView::draw(lgfx::LGFXBase& g, const PlayerViewData& data) {
    const auto& c = Theme::colors();
    g.fillScreen(c.paper);
    drawTopBar(g, data.volume);
    drawCover(g, data.cover_label);

    drawCategory(g, data.category);
    drawCount(g, data.index, data.count);
    drawTitle(g, data.title);

    // Codec + bitrate. Decoders only learn the bitrate after the first frame, so
    // this row changes once per track and must be repainted with the page.
    char subtitle[64];
    if (data.has_track) {
        std::snprintf(subtitle, sizeof(subtitle), "MP3·%ukbps",
                      static_cast<unsigned>(data.bitrate / 1000));
    } else {
        std::snprintf(subtitle, sizeof(subtitle), "请插入 TF 卡");
    }
    g.setFont(Fonts::body());
    g.setTextColor(c.text, c.paper);
    g.drawString(subtitle, kInfoX, 95);

    char current[24];
    char total[24];
    std::snprintf(current, sizeof(current), "%02u:%02u",
                  static_cast<unsigned>(data.seconds / 60), static_cast<unsigned>(data.seconds % 60));
    std::snprintf(total, sizeof(total), "%02u:%02u",
                  static_cast<unsigned>(data.duration / 60), static_cast<unsigned>(data.duration % 60));
    g.setFont(Fonts::small());
    g.setTextColor(c.text, c.paper);
    g.drawString(current, kInfoX, 118);
    g.drawRightString(total, 310, 118);

    // Progress groove: grey track, blue fill and the yellow end cap.
    g.fillRect(kInfoX - 2, 132, kInfoW + 4, 12, c.paper);
    g.fillRoundRect(kInfoX, 134, kInfoW, 8, 2, c.track);
    g.drawRoundRect(kInfoX, 134, kInfoW, 8, 2, c.track_border);
    int width = 0;
    if (data.duration > 0) {
        width = static_cast<int>(static_cast<uint64_t>(kInfoW - 2) * data.seconds / data.duration);
    }
    if (width > kInfoW - 2) width = kInfoW - 2;
    if (width > 1) {
        g.fillRect(kInfoX + 1, 135, width, 6, c.blue);
        if (width > 4) g.fillRect(kInfoX + 1 + width - 4, 135, 4, 6, c.yellow);
    }

    drawStatus(g, data);
    drawTransport(g);
    drawPlayButton(g, data.playing && !data.paused);
}

void PlayerView::drawTopBar(lgfx::LGFXBase& g, int volume) {
    const auto& c = Theme::colors();
    Theme::setTopBarColor(c.coral);
    g.fillRect(0, 0, 320, 30, c.coral);
    g.fillRect(0, 30, 320, 2, c.coral_dark);
    Theme::drawSmileFace(g, 19, 15, 10);
    g.setFont(Fonts::body());
    g.setTextColor(TFT_WHITE, c.coral);
    g.drawString("小小故事盒", 36, 7);
    Theme::drawHallIcon(g, ctx().hall, c.coral);
    Theme::drawChildLockBadge(g, ctx().child_locked, c.coral);

    const bool card = ctx().hall;
    g.fillRect(card ? 228 : 238, 0, card ? 66 : 82, 30, c.coral);
    g.setFont(Fonts::body());
    g.setTextColor(TFT_WHITE, c.coral);
    char text[24];
    std::snprintf(text, sizeof(text), "音量:%d", volume);
    g.drawRightString(text, card ? 292 : 310, 7);
}

void PlayerView::drawCover(lgfx::LGFXBase& g, const char* label) {
    const auto& c = Theme::colors();
    const uint16_t cover = Theme::rgb(0x7cc9dd);
    const uint16_t moon = Theme::rgb(0xfff4a8);
    const uint16_t bush = Theme::rgb(0x2f9b68);
    g.fillRoundRect(13, 45, 92, 112, 6, c.ink);
    g.fillRoundRect(10, 42, 92, 112, 6, c.ink);
    g.fillRoundRect(12, 44, 88, 108, 5, c.yellow);
    g.fillRoundRect(14, 47, 82, 84, 2, cover);
    g.fillCircle(71, 73, 16, moon);
    g.drawCircle(71, 73, 16, c.ink);
    g.drawCircle(71, 73, 15, c.ink);
    g.fillCircle(37, 112, 8, bush);
    g.fillCircle(70, 111, 8, bush);
    g.fillRoundRect(29, 101, 48, 20, 10, TFT_WHITE);
    g.fillCircle(44, 101, 10, TFT_WHITE);
    g.fillCircle(60, 99, 12, TFT_WHITE);
    g.setFont(Fonts::body());
    g.setTextColor(c.text, c.yellow);
    char text[24];
    std::snprintf(text, sizeof(text), "%s", label ? label : "音乐播放");
    g.drawCenterString(text, 56, 135);
}

void PlayerView::drawCategory(lgfx::LGFXBase& g, const char* label) {
    const auto& c = Theme::colors();
    const char* value = (label && label[0]) ? label : "音乐";
    constexpr int pad = 6;
    g.fillRect(kInfoX, 43, kInfoW, 18, c.paper);
    g.setFont(Fonts::body());
    const int width = g.textWidth(value) + pad * 2;
    g.fillRoundRect(kInfoX, 43, width, 18, 4, c.aqua);
    g.drawRoundRect(kInfoX, 43, width, 18, 4, c.aqua_dark);
    g.setTextColor(TFT_WHITE, c.aqua);
    g.drawString(value, kInfoX + pad, 44);
}

void PlayerView::drawCount(lgfx::LGFXBase& g, int index, int count) {
    const auto& c = Theme::colors();
    char text[32];
    if (count > 0) std::snprintf(text, sizeof(text), "%02d / %02d", index + 1, count);
    else std::snprintf(text, sizeof(text), "-- / --");
    g.fillRect(240, 43, 70, 18, c.paper);
    g.setFont(Fonts::small());
    g.setTextColor(c.text, c.paper);
    g.drawRightString(text, 310, 47);
}

void PlayerView::drawTitle(lgfx::LGFXBase& g, const char* title) {
    const auto& c = Theme::colors();
    const char* value = (title && title[0]) ? title : "";
    g.setFont(Fonts::title());
    g.setTextColor(c.text, c.paper);
    g.setClipRect(kInfoX, 63, kInfoW, kTitleHeight);
    g.fillRect(kInfoX, 63, kInfoW, kTitleHeight, c.paper);
    g.drawString(value, kInfoX - scroll_, 65);
    g.clearClipRect();
}

void PlayerView::drawStatus(lgfx::LGFXBase& g, const PlayerViewData& data) {
    const auto& c = Theme::colors();
    uint16_t dot = c.track;
    uint16_t border = c.track_border;
    const char* status = "无音乐";
    if (data.has_track && data.playing && !data.paused) {
        dot = c.green;
        border = c.green_dark;
        status = "正在播放";
    } else if (data.has_track && !data.playing && data.paused) {
        dot = c.yellow;
        border = Theme::rgb(0xa97700);
        status = "已经暂停";
    } else if (data.has_track) {
        dot = c.yellow;
        border = Theme::rgb(0xa97700);
        status = "已停止";
    }
    g.fillCircle(kInfoX + 4, 154, 4, border);
    g.fillCircle(kInfoX + 4, 154, 3, dot);
    g.setFont(Fonts::body());
    g.setTextColor(c.text, c.paper);
    g.drawString(status, kInfoX + 14, 146);
}

void PlayerView::drawTransport(lgfx::LGFXBase& g) {
    const auto& c = Theme::colors();
    g.fillRect(0, 188, 320, 52, c.ink);
    g.fillRect(0, 188, 320, 3, c.yellow);
    g.fillRoundRect(41, 197, 52, 34, 10, c.button_border);
    g.fillRoundRect(43, 199, 48, 30, 8, c.button);
    g.fillRect(61, 208, 3, 12, TFT_WHITE);
    g.fillTriangle(71, 208, 71, 220, 61, 214, TFT_WHITE);
    g.fillRoundRect(227, 197, 52, 34, 10, c.button_border);
    g.fillRoundRect(229, 199, 48, 30, 8, c.button);
    g.fillRect(258, 208, 3, 12, TFT_WHITE);
    g.fillTriangle(249, 208, 249, 220, 259, 214, TFT_WHITE);
}

void PlayerView::drawPlayButton(lgfx::LGFXBase& g, bool playing) {
    const auto& c = Theme::colors();
    g.fillRect(117, 192, 86, 46, c.ink);
    g.fillRoundRect(119, 194, 82, 40, 12, c.yellow_light);
    g.fillRoundRect(121, 196, 78, 36, 10, c.yellow);

    const int ix = 140;
    const int iy = 214;
    if (playing) {
        g.fillRect(ix - 7, iy - 9, 5, 18, c.ink);
        g.fillRect(ix + 2, iy - 9, 5, 18, c.ink);
    } else {
        g.fillTriangle(ix - 7, iy - 9, ix - 7, iy + 9, ix + 8, iy, c.ink);
    }
    g.setFont(Fonts::body());
    g.setTextColor(c.text, c.yellow);
    g.drawString(playing ? "暂停" : "播放", ix + 14, iy - 8);
}

}  // namespace App
