#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "ble_service.h"
#include "display.h"
#include "fonts.h"

#include <cstdio>

#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

namespace App {
namespace {

// 11 = 显示绑定码, 12 = 资源占用.
constexpr int kItemCount = 12;
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

// 0 keeps the decoded frame as-is (the letterboxed 320x240 picture), 1 zooms so
// the picture touches the top and bottom edges and crops the sides.
const char* videoFitLabel(uint8_t fit) {
    return fit == 1 ? "高度铺满" : "宽度铺满";
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
                    drawRow(g, y, "视频画面", videoFitLabel(ctx().settings.video_fit), is_selected, -1);
                    break;
                case 4:
                    drawRow(g, y, "播放中熄屏", minutesLabel(ctx().settings.screen_off_min), is_selected, -1);
                    break;
                case 5:
                    drawRow(g, y, "无播放休眠", minutesLabel(ctx().settings.sleep_min), is_selected, -1);
                    break;
                case 6:
                    drawRow(g, y, "刷新列表", "单击OK", is_selected, -1);
                    break;
                case 7:
                    drawRow(g, y, "蓝牙广播", ctx().settings.ble ? "开" : "关", is_selected, -1);
                    break;
                case 8:
                    drawRow(g, y, "NFC 功能", ctx().settings.nfc ? "开" : "关", is_selected, -1);
                    break;
                case 9:
                    drawRow(g, y, "家长设置", parentalReady() ? "单击进入" : "不可用", is_selected, -1);
                    break;
                case 10:
                    drawRow(g, y, "显示绑定码", "单击查看", is_selected, -1);
                    break;
                default:
                    drawRow(g, y, "资源占用", "RAM PSRAM Flash", is_selected, -1);
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
                s.video_fit = s.video_fit ? 0 : 1;
                break;
            case 4:
                s.screen_off_min = cycleMinutes(s.screen_off_min, direction);
                break;
            case 5:
                s.sleep_min = cycleMinutes(s.sleep_min, direction);
                break;
            default:
                return;  // items 6..10 are actions or toggles, not values
        }
        if (!saveSettings() && ctx().fram_confirmed) notify("设置保存失败");
    }

    void activate() {
        switch (selected_) {
            case 6: {
                // Rebuild the catalog from the card. The stored index can be stale
                // (external edits, or an older firmware that could not read long
                // names); a rescan stops playback first because it needs every
                // file lease back.
                notify("正在刷新列表…", 4000);
                if (refreshCatalog()) {
                    notify("列表已刷新");
                } else {
                    notify("刷新失败：没有存储卡或扫描出错");
                }
                invalidate();
                break;
            }
            case 7:
                ctx().settings.ble = ctx().settings.ble ? 0 : 1;
                Ble::setAdvertisingEnabled(ctx().settings.ble != 0);
                notify(ctx().settings.ble ? "蓝牙广播已开启" : "蓝牙广播已关闭");
                saveSettings();
                break;
            case 8: {
                const bool next = ctx().settings.nfc == 0;
                if (!applyNfcEnabled(next)) {
                    notify("正在写卡，稍后再关闭 NFC");
                    break;
                }
                notify(next ? "NFC 功能已开启" : "NFC 功能已关闭");
                saveSettings();
                break;
            }
            case 9: openParentalSettings(); break;
            case 10: openBindCode(); break;
            case 11: pushPage(createResourcePage()); break;
            default: break;
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

    void openBindCode() { pushPage(createBindCodePage()); }

    int selected_ = 0;
};

// Six-digit pairing code shown to the parent. Binding itself happens over BLE;
// this page only displays the code and the current pairing state.
class BindCodePage final : public Page {
public:
    const char* id() const override { return "bind-code"; }

    void enter() override { invalidate(); }

    void handle(const Event& event) override {
        switch (event.key) {
            case Key::OkLong:
            case Key::OkDouble:
                popPage();
                break;
            case Key::SetLong:
                goHome();
                break;
            default:
                break;
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        Theme::setTopBarColor(c.coral);
        g.fillScreen(c.paper);
        g.fillRect(0, 0, 320, 30, c.coral);
        g.fillRect(0, 30, 320, 2, c.coral_dark);
        Theme::drawSmileFace(g, 19, 15, 10);
        g.setFont(Fonts::body());
        g.setTextColor(TFT_WHITE, c.coral);
        g.drawString("小小故事盒", 36, 7);
        g.drawRightString("绑定码", 310, 7);

        if (!parentalReady()) {
            g.setFont(Fonts::title());
            g.setTextColor(c.text, c.paper);
            g.drawCenterString("家长记录不可用", 160, 110);
            g.setFont(Fonts::small());
            g.drawCenterString("确认 FRAM 后可重新生成", 160, 150);
            return;
        }

        const char* code = parentalBindCode();
        g.setFont(Fonts::title());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString("在小程序中输入", 160, 62);

        // Digit boxes keep the code readable on the small panel.
        const int box_width = 38, gap = 6;
        const int total = 6 * box_width + 5 * gap;
        int x = (320 - total) / 2;
        for (int i = 0; i < 6; ++i) {
            g.fillRoundRect(x, 92, box_width, 54, 8, c.yellow_light);
            g.drawRoundRect(x, 92, box_width, 54, 8, c.ink);
            char digit[2] = {code[i] ? code[i] : '-', 0};
            g.setFont(Fonts::title());
            g.setTextColor(c.ink, c.yellow_light);
            g.drawCenterString(digit, x + box_width / 2, 110);
            x += box_width + gap;
        }

        g.setFont(Fonts::body());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString(parentalBound() ? "已绑定家长账号" : "尚未绑定", 160, 168);
        g.setFont(Fonts::small());
        g.drawCenterString("长按 OK 返回", 160, 214);
    }
};

// ---- resource page -------------------------------------------------------
//
// Read-only readout of the three memory pools this product actually fights
// over: internal RAM (I2S DMA + driver buffers), PSRAM (canvas, frame mailboxes,
// media catalog) and the app partition in flash. Everything here comes from
// O(1) heap counters plus one image-metadata read on entry, so the page itself
// costs a static object and no heap. The per-second refresh only redraws.

constexpr int kResCardY = 36;
constexpr int kResCardH = 58;
constexpr int kResCardGap = 64;

void formatKb(char* out, std::size_t size, uint32_t bytes) {
    std::snprintf(out, size, "%u KB", static_cast<unsigned>(bytes / 1024));
}

void formatMb(char* out, std::size_t size, uint64_t bytes) {
    const uint64_t tenths = bytes * 10 / (1024 * 1024);
    std::snprintf(out, size, "%u.%u MB", static_cast<unsigned>(tenths / 10),
                  static_cast<unsigned>(tenths % 10));
}

unsigned usedPercent(uint64_t total, uint64_t free) {
    if (!total || free >= total) return 0;
    return static_cast<unsigned>((total - free) * 100 / total);
}

void drawResourceCard(lgfx::LGFXBase& g, int y, const char* label, const char* value,
                      const char* detail, unsigned percent) {
    const auto& c = Theme::colors();
    g.fillRoundRect(10, y, 300, kResCardH, 6, TFT_WHITE);
    g.drawRoundRect(10, y, 300, kResCardH, 6, c.track_border);
    g.setFont(Fonts::body());
    g.setTextColor(c.text, TFT_WHITE);
    g.drawString(label, 22, y + 6);
    g.drawRightString(value, 296, y + 6);

    constexpr int bar_x = 22, bar_y = 26, bar_w = 274, bar_h = 8;
    g.fillRoundRect(bar_x, y + bar_y, bar_w, bar_h, 2, c.track);
    g.drawRoundRect(bar_x, y + bar_y, bar_w, bar_h, 2, c.track_border);
    int width = static_cast<int>(bar_w * (percent > 100 ? 100 : percent) / 100);
    if (width > 1) {
        g.fillRect(bar_x + 1, y + bar_y + 1, width - 2, bar_h - 2, c.blue);
        if (width > 6) g.fillRect(bar_x + 1 + width - 6, y + bar_y + 1, 4, bar_h - 2, c.yellow);
    }

    g.setFont(Fonts::small());
    g.setTextColor(Theme::rgb(0x647b79), TFT_WHITE);
    g.drawString(detail, 22, y + 40);
}

class ResourcePage final : public Page {
public:
    const char* id() const override { return "resources"; }

    void enter() override {
        readRom();
        last_ms_ = 0;
        invalidate();
    }

    void handle(const Event& event) override {
        switch (event.key) {
            case Key::Ok:
            case Key::Next:
            case Key::Prev:
                last_ms_ = 0;  // force a fresh read on the next tick
                break;
            case Key::OkLong:
            case Key::OkDouble:
                popPage();
                break;
            case Key::SetLong:
                goHome();
                break;
            default:
                break;
        }
    }

    void tick(uint32_t now) override {
        if (last_ms_ && now - last_ms_ < 1000) return;
        last_ms_ = now;
        invalidate();
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        Theme::setTopBarColor(c.coral);
        g.fillScreen(c.paper);
        g.fillRect(0, 0, 320, 30, c.coral);
        g.fillRect(0, 30, 320, 2, c.coral_dark);
        Theme::drawSmileFace(g, 19, 15, 10);
        g.setFont(Fonts::body());
        g.setTextColor(TFT_WHITE, c.coral);
        g.drawString("小小故事盒", 36, 7);
        g.drawRightString("资源占用", 310, 7);

        const uint32_t ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const uint32_t ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const uint32_t ram_low = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const uint32_t ram_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const uint32_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        const uint32_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        const uint32_t psram_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

        char value[48], detail[96], a[20], b[20], c2[20];
        int y = kResCardY;

        // Internal RAM: the pool the I2S DMA ring and every driver buffer share.
        formatKb(a, sizeof(a), ram_total - ram_free);
        formatKb(b, sizeof(b), ram_total);
        std::snprintf(value, sizeof(value), "%s / %s", a, b);
        formatKb(a, sizeof(a), ram_free);
        formatKb(b, sizeof(b), ram_block);
        formatKb(c2, sizeof(c2), ram_low);
        std::snprintf(detail, sizeof(detail), "余 %s · 最大块 %s · 最低 %s", a, b, c2);
        drawResourceCard(g, y, "内部 RAM", value, detail, usedPercent(ram_total, ram_free));
        y += kResCardGap;

        // PSRAM: canvas, frame mailboxes, media catalog and video buffers.
        formatMb(a, sizeof(a), psram_total - psram_free);
        formatMb(b, sizeof(b), psram_total);
        std::snprintf(value, sizeof(value), "%s / %s", a, b);
        formatMb(a, sizeof(a), psram_free);
        formatMb(b, sizeof(b), psram_block);
        std::snprintf(detail, sizeof(detail), "余 %s · 最大块 %s", a, b);
        drawResourceCard(g, y, "PSRAM", value, detail, usedPercent(psram_total, psram_free));
        y += kResCardGap;

        // Flash app partition: image bytes vs partition size.
        formatMb(a, sizeof(a), rom_image_);
        formatMb(b, sizeof(b), rom_total_);
        std::snprintf(value, sizeof(value), "%s / %s", a, b);
        formatKb(a, sizeof(a), rom_total_ > rom_image_ ? rom_total_ - rom_image_ : 0);
        std::snprintf(detail, sizeof(detail), "分区 %s · 余 %s", rom_label_ ? rom_label_ : "app", a);
        drawResourceCard(g, y, "Flash 固件", value, detail, usedPercent(rom_total_, rom_image_));

        g.setFont(Fonts::small());
        g.setTextColor(Theme::rgb(0x647b79), c.paper);
        g.drawCenterString("每秒刷新  长按 OK 返回", 160, 226);
    }

private:
    // Image length never changes at runtime; one metadata read per entry is
    // enough and keeps the 1 Hz refresh free of flash access.
    void readRom() {
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (!running) running = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
        if (!running) return;
        rom_label_ = running->label;
        rom_total_ = running->size;
        const esp_partition_pos_t position{running->address, running->size};
        esp_image_metadata_t metadata{};
        if (esp_image_get_metadata(&position, &metadata) == ESP_OK) rom_image_ = metadata.image_len;
    }

    const char* rom_label_ = nullptr;
    uint32_t rom_image_ = 0, rom_total_ = 0;
    uint32_t last_ms_ = 0;
};

SettingsPage& settingsInstance() {
    static SettingsPage instance;
    return instance;
}

BindCodePage& bindCodeInstance() {
    static BindCodePage instance;
    return instance;
}

ResourcePage& resourceInstance() {
    static ResourcePage instance;
    return instance;
}

}  // namespace

Page* createSettingsPage() { return &settingsInstance(); }
Page* createBindCodePage() { return &bindCodeInstance(); }
Page* createResourcePage() { return &resourceInstance(); }

}  // namespace App
