#include "app_context.h"
#include "app_page.h"
#include "app_theme.h"
#include "display.h"
#include "fonts.h"

#include <cstdio>

namespace App {
namespace {

constexpr int kItemCount = 8;
constexpr int kVisibleRows = 4;
constexpr int kRowY = 34;
constexpr int kRowHeight = 44;
// Provisional response curves, mirrored from the audio component's presets so
// the settings page can draw the EQ shape without touching the decoder.
constexpr int kEqGains[4][3] = {{-2, 4, -1}, {2, 2, 1}, {5, 0, -1}, {0, 0, 0}};

const char* eqLabel(uint8_t preset) {
    switch (preset) {
        case 0: return "人声";
        case 1: return "流行";
        case 2: return "低音";
        default: return "平直";
    }
}

const char* rotationLabel(uint8_t rotation) {
    return rotation == 1 ? "横屏 0°" : "横屏 180°";
}

void drawBandMeter(lgfx::LGFXBase& g, int y, uint8_t preset) {
    const auto& c = Theme::colors();
    const auto index = preset > 3 ? 3 : preset;
    for (int i = 0; i < 3; ++i) {
        const int cx = 170 + i * 34;
        const int value = kEqGains[index][i];
        const int magnitude = value < 0 ? -value : value;
        int width = magnitude * 12 / 6;
        if (width > 12) width = 12;
        g.drawFastHLine(cx - 12, y + 31, 24, c.track_border);
        if (value >= 0) {
            g.fillRect(cx, y + 25, width, 6, c.blue);
        } else {
            g.fillRect(cx - width, y + 31, width, 6, c.yellow);
        }
    }
}

void drawRow(lgfx::LGFXBase& g, int y, const char* label, const char* value,
             bool selected, int bar_percent) {
    const auto& c = Theme::colors();
    const uint16_t background = selected ? c.yellow : TFT_WHITE;
    if (selected) g.fillRoundRect(13, y + 3, 300, 44, 6, c.ink);
    g.fillRoundRect(10, y, 300, 44, 6, selected ? c.ink : TFT_WHITE);
    g.fillRoundRect(12, y + 2, 296, 40, 5, background);
    if (!selected) g.drawRoundRect(10, y, 300, 44, 6, c.track_border);

    g.setFont(Fonts::body());
    g.setTextColor(c.text, background);
    g.drawString(label, 24, y + 7);
    g.drawRightString(value, 294, y + 7);

    if (bar_percent >= 0) {
        g.fillRoundRect(24, y + 26, 272, 8, 2, c.track);
        g.drawRoundRect(24, y + 26, 272, 8, 2, c.track_border);
        int width = 272 * bar_percent / 100;
        if (width > 1) {
            if (width > 272) width = 272;
            g.fillRect(25, y + 27, width - 2, 6, c.blue);
            if (width > 6) g.fillRect(25 + width - 6, y + 27, 4, 6, c.yellow);
        }
    }
}

class SettingsPage final : public Page {
public:
    const char* id() const override { return "settings"; }

    void enter() override {
        selected_ = 0;
        invalidate();
    }

    void handle(const Event& event) override {
        if (event.delta) {
            adjust(event.delta > 0 ? 1 : -1);
            return;
        }
        switch (event.key) {
            case Key::Next:
                selected_ = (selected_ + 1) % kItemCount;
                break;
            case Key::Prev:
                selected_ = (selected_ + kItemCount - 1) % kItemCount;
                break;
            case Key::Ok:
                activate();
                break;
            case Key::OkDouble:
                close();
                return;
            case Key::SetLong:
                close(true);
                return;
            default:
                break;
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        Theme::setTopBarColor(c.coral);
        g.fillScreen(c.paper);
        drawTopBar(g);

        // Clear the window, then draw the four visible rows.
        g.fillRect(8, kRowY, 310, 183, c.paper);
        int top = selected_ - (kVisibleRows - 1);
        if (top > kItemCount - kVisibleRows) top = kItemCount - kVisibleRows;
        if (top < 0) top = 0;

        char value[24];
        for (int row = 0; row < kVisibleRows; ++row) {
            const int item = top + row;
            if (item >= kItemCount) break;
            const int y = kRowY + row * kRowHeight;
            const bool is_selected = item == selected_;
            switch (item) {
                case 0:
                    std::snprintf(value, sizeof(value), "%u%%", ctx().settings.brightness);
                    drawRow(g, y, "亮度", value, is_selected, ctx().settings.brightness);
                    break;
                case 1:
                    drawRow(g, y, "旋转方向", rotationLabel(ctx().settings.rotation), is_selected, -1);
                    break;
                case 2:
                    drawRow(g, y, "均衡器", eqLabel(ctx().settings.eq), is_selected, -1);
                    drawBandMeter(g, y, ctx().settings.eq);
                    break;
                case 3:
                    drawRow(g, y, "播放中熄屏", minutesLabel(ctx().settings.screen_off_min), is_selected, -1);
                    break;
                case 4:
                    drawRow(g, y, "无播放休眠", minutesLabel(ctx().settings.sleep_min), is_selected, -1);
                    break;
                case 5:
                    drawRow(g, y, "刷新列表", "单击OK", is_selected, -1);
                    break;
                case 6:
                    drawRow(g, y, "蓝牙广播", ctx().settings.ble ? "开" : "关", is_selected, -1);
                    break;
                default:
                    drawRow(g, y, "显示绑定码", "单击查看", is_selected, -1);
                    break;
            }
        }

        // Scroll bar for the 4-row window.
        constexpr int sx = 314, sy = kRowY, sh = kRowHeight * kVisibleRows;
        const int thumb_height = sh * kVisibleRows / kItemCount;
        const int thumb_y = sy + (sh - thumb_height) * top / (kItemCount - kVisibleRows);
        g.fillRoundRect(sx, sy, 4, sh, 2, c.track);
        g.fillRoundRect(sx, thumb_y, 4, thumb_height, 2, c.coral);

        g.setFont(Fonts::body());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString("旋钮调整  双击OK保存回主页", 160, 220);
    }

private:
    void drawTopBar(lgfx::LGFXBase& g) {
        const auto& c = Theme::colors();
        g.fillRect(0, 0, 320, 30, c.coral);
        g.fillRect(0, 30, 320, 2, c.coral_dark);
        Theme::drawSmileFace(g, 19, 15, 10);
        g.setFont(Fonts::body());
        g.setTextColor(TFT_WHITE, c.coral);
        g.drawString("小小故事盒", 36, 7);
        Theme::drawHallIcon(g, ctx().hall, c.coral);
        Theme::drawChildLockBadge(g, ctx().child_locked, c.coral);
    }

    void adjust(int direction) {
        auto& s = ctx().settings;
        switch (selected_) {
            case 0: {
                int brightness = static_cast<int>(s.brightness) + direction * 5;
                if (brightness < 10) brightness = 10;
                if (brightness > 100) brightness = 100;
                s.brightness = static_cast<uint8_t>(brightness);
                Display::setBrightness(s.brightness);
                break;
            }
            case 1:
                s.rotation = s.rotation == 1 ? 3 : 1;
                Display::setRotation(s.rotation);
                break;
            case 2: {
                const int next = (static_cast<int>(s.eq) + (direction > 0 ? 1 : 3)) % 4;
                applyEq(static_cast<uint8_t>(next));
                break;
            }
            case 3:
                s.screen_off_min = cycleMinutes(s.screen_off_min, direction);
                break;
            case 4:
                s.sleep_min = cycleMinutes(s.sleep_min, direction);
                break;
            default:
                return;  // items 5..7 are actions, not values
        }
        if (!saveSettings() && ctx().fram_confirmed) notify("设置保存失败");
    }

    void activate() {
        switch (selected_) {
            case 5: notify("刷新列表：P2 接入"); break;
            case 6:
                ctx().settings.ble = ctx().settings.ble ? 0 : 1;
                notify(ctx().settings.ble ? "蓝牙广播将在 P4 接入" : "蓝牙广播将在 P4 接入");
                saveSettings();
                break;
            default: notify("绑定码：P5 接入"); break;
        }
    }

    void close(bool to_home = false) {
        if (!saveSettings() && ctx().fram_confirmed) notify("设置保存失败");
        if (to_home) {
            goHome();
        } else {
            popPage();
        }
    }

    int selected_ = 0;
};

SettingsPage& settingsInstance() {
    static SettingsPage instance;
    return instance;
}

}  // namespace

Page* createSettingsPage() { return &settingsInstance(); }

}  // namespace App
