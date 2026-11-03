#include <cstdio>
#include <string>
#include <vector>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "display.h"
#include "esp_err.h"
#include "fonts.h"

namespace App {
namespace {

constexpr int kVisibleRows = 4;
constexpr int kListRowY = 36;
constexpr int kListRowHeight = 38;

int wrap(int value, int count) {
    if (count <= 0) return 0;
    value %= count;
    return value < 0 ? value + count : value;
}

void drawTopBar(lgfx::LGFXBase& g, const char* title) {
    const auto& c = Theme::colors();
    Theme::setTopBarColor(c.coral);
    g.fillRect(0, 0, 320, 30, c.coral);
    g.fillRect(0, 30, 320, 2, c.coral_dark);
    Theme::drawSmileFace(g, 19, 15, 10);
    g.setFont(Fonts::body());
    g.setTextColor(TFT_WHITE, c.coral);
    g.drawString("小小故事盒", 36, 7);
    g.drawRightString(title, 310, 7);
    Theme::drawHallIcon(g, ctx().hall, c.coral);
    Theme::drawChildLockBadge(g, ctx().child_locked, c.coral);
}

void drawCaption(lgfx::LGFXBase& g, const char* text) {
    const auto& c = Theme::colors();
    g.setFont(Fonts::small());
    g.setTextColor(c.text, c.paper);
    g.drawCenterString(text, 160, 224);
}

// Row card used by both lists: optional icon slot, title and right-hand value.
void drawListRow(lgfx::LGFXBase& g, int y, int height, const std::string& title,
                 const std::string& right, bool selected, bool marker) {
    const auto& c = Theme::colors();
    const uint16_t background = selected ? c.yellow : TFT_WHITE;
    if (selected) g.fillRoundRect(13, y + 3, 300, height, 6, c.ink);
    g.fillRoundRect(10, y, 300, height, 6, selected ? c.ink : TFT_WHITE);
    g.fillRoundRect(12, y + 2, 296, height - 4, 5, background);
    if (!selected) g.drawRoundRect(10, y, 300, height, 6, c.track_border);

    char text[96];
    Theme::fitText(g, title.c_str(), 236, text, sizeof(text));
    g.setFont(Fonts::body());
    g.setTextColor(c.text, background);
    g.drawString(text, 24, y + 8);

    if (!right.empty()) {
        g.setFont(Fonts::small());
        g.drawRightString(right.c_str(), 296, y + 11);
    }
    if (marker) {
        g.fillCircle(308, y + height / 2, 5, c.green);
    }
}

class MusicListPage final : public Page {
public:
    const char* id() const override { return "music-list"; }

    void enter() override {
        selected_ = 0;
        catalog_ = catalog();
        invalidate();
        if (!ctx().catalog_ready) requestScan();
    }

    void handle(const Event& event) override {
        const int count = entryCount();
        if (event.delta) {
            selected_ = wrap(selected_ + event.delta, count);
            return;
        }
        switch (event.key) {
            case Key::Next: selected_ = wrap(selected_ + 1, count); break;
            case Key::Prev: selected_ = wrap(selected_ - 1, count); break;
            case Key::Ok:
            case Key::Set: openSelection(); break;
            case Key::SetLong: goHome(); break;
            case Key::OkDouble: goHome(); break;
            default: break;
        }
    }

    void tick(uint32_t) override {
        auto latest = catalog();
        if (latest != catalog_) {
            catalog_ = latest;
            invalidate();
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g, "音乐");

        const int total = entryCount();
        int top = selected_ - (kVisibleRows - 1);
        if (top > total - kVisibleRows) top = total - kVisibleRows;
        if (top < 0) top = 0;

        g.fillRect(8, kListRowY, 310, 172, c.paper);
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) break;
            const int y = kListRowY + row * kListRowHeight;
            if (index == 0) {
                drawListRow(g, y, kListRowHeight, "全部歌曲",
                            std::to_string(catalog_ ? catalog_->tracks.size() : 0u),
                            index == selected_, false);
            } else if (catalog_ && index - 1 < static_cast<int>(catalog_->categories.size())) {
                const auto& name = catalog_->categories[index - 1];
                drawListRow(g, y, kListRowHeight, name, categoryCount(index),
                            index == selected_, false);
            }
        }

        drawCaption(g, "旋钮选择  OK 进入  SET 长按回主页");
    }

private:
    int entryCount() const {
        return 1 + (catalog_ ? static_cast<int>(catalog_->categories.size()) : 0);
    }

    std::string categoryCount(int entry) const {
        if (!catalog_) return "0";
        std::size_t count = 0;
        for (const auto& track : catalog_->tracks) {
            if (track.category == entry) ++count;
        }
        return std::to_string(count);
    }

    void openSelection() {
        if (!catalog_) {
            notify("列表为空，先刷新");
            return;
        }
        if (!playlist().select(catalog_, selected_)) {
            notify("该分类没有曲目");
            return;
        }
        if (!App::playCurrent(false)) {
            notify("播放器加载失败");
            return;
        }
        pushPage(createPlayerPage());
        pushPage(createTrackListPage());
    }

    void requestScan() {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g, "音乐");
        Theme::drawSmileFace(g, 160, 96, 16);
        g.setFont(Fonts::title());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString("正在扫描曲目", 160, 132);
        g.setFont(Fonts::small());
        g.drawCenterString("请勿拔出 TF 卡", 160, 160);
        Display::present(true);
        const bool ok = refreshCatalog();
        ctx().catalog_ready = ok;
        catalog_ = catalog();
        invalidate();
        if (!ok) notify("扫描失败，请检查 TF 卡");
    }

    int selected_ = 0;
    CatalogPtr catalog_;
};

class TrackListPage final : public Page {
public:
    const char* id() const override { return "track-list"; }

    void enter() override {
        selected_ = static_cast<int>(playlist().position());
        invalidate();
    }

    void handle(const Event& event) override {
        const int count = static_cast<int>(playlist().size());
        if (!count) return;
        if (event.delta) {
            moveBy(event.delta);
            return;
        }
        switch (event.key) {
            case Key::Next: moveBy(1); break;
            case Key::Prev: moveBy(-1); break;
            case Key::NextLong:
            case Key::Ok: {
                const int delta = selected_ - static_cast<int>(playlist().position());
                if (delta) playlist().move(delta);
                if (App::playCurrent(true)) pushPage(createPlayerPage());
                break;
            }
            case Key::SetLong: goHome(); break;
            case Key::OkDouble: popPage(); break;
            case Key::OkLong:
                toggleFavorite();
                break;
            default: break;
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g, "歌曲");

        const int total = static_cast<int>(playlist().size());
        int top = selected_ - (kVisibleRows - 1);
        if (top > total - kVisibleRows) top = total - kVisibleRows;
        if (top < 0) top = 0;

        g.fillRect(8, kListRowY, 310, 172, c.paper);
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) break;
            const int y = kListRowY + row * kListRowHeight;
            const int delta = index - static_cast<int>(playlist().position());
            const auto* track = trackAt(delta);
            if (!track) break;
            std::string right;
            if (App::favorite(globalIndex(delta))) right = "♥";
            drawListRow(g, y, kListRowHeight, track->name, right,
                        index == selected_, index == static_cast<int>(playlist().position()));
        }

        drawCaption(g, "OK 播放  长按OK收藏  SET 长按回主页");
    }

private:
    void moveBy(int delta) {
        const int count = static_cast<int>(playlist().size());
        selected_ = wrap(selected_ + delta, count);
        invalidate();
    }

    // Walk a playlist copy so the active playback position is never disturbed
    // by merely browsing the list.
    const Media::Track* trackAt(int delta) {
        scratch_ = playlist();
        if (!scratch_.move(delta)) return nullptr;
        return scratch_.current();
    }

    std::size_t globalIndex(int delta) {
        const auto* track = trackAt(delta);
        const auto snapshot = App::catalog();
        if (!track || !snapshot) return 0;
        for (std::size_t i = 0; i < snapshot->tracks.size(); ++i) {
            if (snapshot->tracks[i].id == track->id) return i;
        }
        return 0;
    }

    void toggleFavorite() {
        const auto index = globalIndex(0);
        const bool enabled = !App::favorite(index);
        if (!App::setFavorite(index, enabled)) notify("收藏保存失败");
        else notify(enabled ? "已收藏" : "已取消收藏");
        invalidate();
    }

    int selected_ = 0;
    Media::Playlist scratch_;
};

// Player page shows the current playlist entry and drives playback commands.
class PlayerPage final : public Page {
public:
    const char* id() const override { return "player"; }

    void enter() override {
        request_ = 0;
        last_generation_ = 0;
        drawn_second_ = 0xFFFFFFFF;
        last_state_ = -1;
        invalidate();
    }

    void handle(const Event& event) override {
        if (event.delta) {
            stepVolume(event.delta);
            return;
        }
        switch (event.key) {
            case Key::Ok: {
                const auto snapshot = playback().snapshot();
                const bool playing = snapshot.state == Audio::PlaybackState::Playing;
                pausePlayback(playing);
                break;
            }
            case Key::Next:
                if (playlist().move(1)) playCurrent(true);
                break;
            case Key::Prev:
                if (playlist().move(-1)) playCurrent(true);
                break;
            case Key::NextLong:
                pushPage(createTrackListPage());
                break;
            case Key::PrevLong:
                stopPlayback();
                goHome();
                break;
            case Key::OkLong:
                stopPlayback();
                popPage();
                break;
            case Key::Set:
                pausePlayback(playback().snapshot().state == Audio::PlaybackState::Playing);
                break;
            case Key::SetLong:
                goHome();
                break;
            default:
                break;
        }
    }

    void tick(uint32_t) override {
        const auto snapshot = playback().snapshot();
        const uint32_t seconds = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        const int state = static_cast<int>(snapshot.state);
        if (seconds != drawn_second_ || state != last_state_) invalidate();

        // Auto-advance once per generation when the decoder reports EOF.
        if (snapshot.state == Audio::PlaybackState::Ended && snapshot.generation != last_generation_ &&
            snapshot.generation > 0) {
            last_generation_ = snapshot.generation;
            if (playlist().move(1)) playCurrent(true);
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        const auto snapshot = playback().snapshot();
        const auto* track = playlist().current();
        const uint32_t seconds = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        const bool playing = snapshot.state == Audio::PlaybackState::Playing;

        g.fillScreen(c.ink);
        g.fillRect(0, 0, 320, 32, c.coral);
        Theme::setTopBarColor(c.coral);
        Theme::drawSmileFace(g, 19, 15, 10);
        g.setFont(Fonts::body());
        g.setTextColor(TFT_WHITE, c.coral);
        g.drawString("小小故事盒", 36, 7);
        char volume[16];
        std::snprintf(volume, sizeof(volume), "音量:%d", ctx().volume);
        g.drawRightString(volume, 310, 7);
        Theme::drawHallIcon(g, ctx().hall, c.coral);
        Theme::drawChildLockBadge(g, ctx().child_locked, c.coral);

        // Cover card
        g.fillRoundRect(13, 45, 92, 112, 8, c.yellow_light);
        g.fillRoundRect(10, 42, 92, 112, 8, c.aqua);
        g.fillRect(52, 70, 8, 46, c.ink);
        g.drawLine(52, 70, 76, 78, c.ink);
        g.fillCircle(45, 118, 12, c.ink);

        g.setFont(Fonts::small());
        g.setTextColor(c.yellow, c.ink);
        g.drawString(playing ? "播放中" : "已暂停", 116, 46);

        char title[96];
        Theme::fitText(g, track ? track->name.c_str() : "未加载曲目", 190, title, sizeof(title));
        g.setFont(Fonts::title());
        g.setTextColor(TFT_WHITE, c.ink);
        g.drawString(title, 116, 66);

        g.setFont(Fonts::small());
        g.drawString(snapshot.error == ESP_OK ? "MP3" : esp_err_to_name(snapshot.error), 116, 92);
        if (snapshot.sample_rate) {
            char format[40];
            std::snprintf(format, sizeof(format), "%lu Hz  %lu kbps",
                          static_cast<unsigned long>(snapshot.sample_rate),
                          static_cast<unsigned long>(snapshot.bitrate / 1000));
            g.drawString(format, 116, 110);
        }

        char position[32];
        std::snprintf(position, sizeof(position), "%02u:%02u",
                      static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
        g.setFont(Fonts::body());
        g.setTextColor(TFT_WHITE, c.ink);
        g.drawString(position, 116, 130);

        char count[32];
        std::snprintf(count, sizeof(count), "%u/%u", static_cast<unsigned>(playlist().position() + 1),
                      static_cast<unsigned>(playlist().size()));
        g.setFont(Fonts::small());
        g.drawRightString(count, 308, 134);

        // Progress groove: total duration is not decoded yet, so the bar shows
        // activity rather than a calibrated position.
        g.fillRoundRect(116, 152, 192, 8, 3, c.track);
        g.drawRoundRect(116, 152, 192, 8, 3, c.track_border);
        if (playing) {
            const int dot = 116 + static_cast<int>((seconds % 30) * 6);
            g.fillCircle(dot > 300 ? 300 : dot, 156, 4, c.blue);
        }

        // Transport bar
        g.fillRect(0, 188, 320, 52, c.yellow);
        g.fillRect(0, 188, 320, 3, c.coral_dark);
        g.fillRoundRect(41, 197, 52, 34, 10, c.button_border);
        g.fillRoundRect(43, 199, 48, 30, 8, c.button);
        g.fillRect(61, 208, 3, 12, TFT_WHITE);
        g.fillTriangle(71, 208, 71, 220, 61, 214, TFT_WHITE);
        g.fillRoundRect(227, 197, 52, 34, 10, c.button_border);
        g.fillRoundRect(229, 199, 48, 30, 8, c.button);
        g.fillRect(258, 208, 3, 12, TFT_WHITE);
        g.fillTriangle(249, 208, 249, 220, 259, 214, TFT_WHITE);
        g.fillRoundRect(119, 194, 82, 40, 12, c.yellow_light);
        g.fillRoundRect(121, 196, 78, 36, 10, c.yellow);
        if (playing) {
            g.fillRect(150, 205, 6, 18, c.ink);
            g.fillRect(160, 205, 6, 18, c.ink);
        } else {
            g.fillTriangle(150, 205, 150, 223, 166, 214, c.ink);
        }
        g.setFont(Fonts::small());
        g.setTextColor(c.ink, c.yellow);
        g.drawCenterString("OK 播放/暂停  长按OK停止  SET 长按回主页", 160, 232);

        drawn_second_ = seconds;
        last_state_ = static_cast<int>(snapshot.state);
    }

private:
    uint64_t request_ = 0;
    uint64_t last_generation_ = 0;
    uint32_t drawn_second_ = 0xFFFFFFFF;
    int last_state_ = -1;
};

MusicListPage& musicListInstance() { static MusicListPage instance; return instance; }
TrackListPage& trackListInstance() { static TrackListPage instance; return instance; }
PlayerPage& playerInstance() { static PlayerPage instance; return instance; }

}  // namespace

Page* createMusicListPage() { return &musicListInstance(); }
Page* createTrackListPage() { return &trackListInstance(); }
Page* createPlayerPage() { return &playerInstance(); }

}  // namespace App
