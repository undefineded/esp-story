#include <cstdio>
#include <cstring>
#include <string>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "display.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "fonts.h"
#include "player_view.h"

// Music pages rebuilt from the Arduino UI (src/ui.cpp). The layout constants,
// fonts and icons intentionally match that implementation: 42px rows with a
// leading note/folder/heart glyph, and the player's cover card + information
// column + ink transport bar. The migrated build had drifted to a simplified
// 38px row and a dark player, which no longer matched the shipped device.
namespace App {
namespace {

constexpr int kVisibleRows = 4;
constexpr int kListRowY = 36;
constexpr int kListRowHeight = 42;
constexpr int kInfoX = 112;
constexpr int kInfoW = 198;
constexpr int kTitleHeight = 22;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

int wrap(int value, int count) {
    if (count <= 0) return 0;
    value %= count;
    return value < 0 ? value + count : value;
}

std::string stripExtension(const std::string& name) {
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// ---- shared chrome (old drawTopBarBase / drawVolume / drawLogo) ------------

void drawTopBar(lgfx::LGFXBase& g) {
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
}

void drawCaption(lgfx::LGFXBase& g, const char* text) {
    const auto& c = Theme::colors();
    g.setFont(Fonts::body());
    g.setTextColor(c.text, c.paper);
    g.drawCenterString(text, 160, 212);
}

// ---- old icons -----------------------------------------------------------

void iconNote(lgfx::LGFXBase& g, int x, int y, uint16_t color) {
    const auto& c = Theme::colors();
    g.fillCircle(x + 6, y + 14, 5, color);
    g.drawCircle(x + 6, y + 14, 5, c.ink);
    g.fillRect(x + 10, y + 2, 3, 14, c.ink);
    g.fillRect(x + 13, y + 2, 7, 4, c.ink);
}

void iconFolder(lgfx::LGFXBase& g, int x, int y, uint16_t color) {
    const auto& c = Theme::colors();
    g.fillRect(x, y + 2, 10, 5, color);
    g.drawRect(x, y + 2, 10, 5, c.ink);
    g.fillRoundRect(x, y + 5, 22, 15, 3, color);
    g.drawRoundRect(x, y + 5, 22, 15, 3, c.ink);
}

void iconFav(lgfx::LGFXBase& g, int x, int y, uint16_t color) {
    g.fillCircle(x + 4, y + 5, 4, color);
    g.fillCircle(x + 11, y + 5, 4, color);
    g.fillTriangle(x, y + 7, x + 15, y + 7, x + 7, y + 16, color);
}

// Row card: white fill + outline; selected: yellow fill, ink outline and the
// 3px hard shadow used by every card in the Arduino UI.
void drawRowCard(lgfx::LGFXBase& g, int y, bool selected) {
    const auto& c = Theme::colors();
    if (selected) g.fillRoundRect(13, y + 3, 300, 38, 6, c.ink);
    g.fillRoundRect(10, y, 300, 38, 6, selected ? c.ink : TFT_WHITE);
    g.fillRoundRect(12, y + 2, 296, 34, 5, selected ? c.yellow : TFT_WHITE);
    if (!selected) g.drawRoundRect(10, y, 300, 38, 6, c.track_border);
}

// ---- music category list -------------------------------------------------

class MusicListPage final : public Page {
public:
    const char* id() const override { return "music-list"; }

    void enter() override {
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
            case Key::SetLong:
            case Key::OkDouble: goHome(); break;
            default: break;
        }
    }

    void tick(uint32_t) override {
        auto latest = catalog();
        if (latest != catalog_) {
            catalog_ = latest;
            if (selected_ >= entryCount()) selected_ = 0;
            invalidate();
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g);

        const int total = entryCount();
        const int top = listTop(selected_, total);
        g.fillRect(8, kListRowY, 306, kListRowHeight * kVisibleRows, c.paper);
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) break;
            const int y = kListRowY + row * kListRowHeight;
            const bool selected = index == selected_;
            drawRowCard(g, y, selected);

            const uint16_t tint = selected ? c.coral : c.aqua;
            if (index == 0) iconNote(g, 24, y + 8, tint);
            else iconFolder(g, 24, y + 10, tint);

            const uint16_t background = selected ? c.yellow : TFT_WHITE;
            char title[96];
            const char* name = index == 0 ? "全部歌曲"
                                          : catalog_->categories[static_cast<std::size_t>(index - 1)].c_str();
            Theme::fitText(g, name, 206, title, sizeof(title));
            g.setFont(Fonts::title());
            g.setTextColor(c.text, background);
            g.drawString(title, 54, y + 9);

            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "%d首", categoryCount(index));
            g.setFont(Fonts::small());
            g.setTextColor(c.text, background);
            g.drawRightString(suffix, 296, y + 14);
        }

        drawCaption(g, "OK播放  长按SET回主页");
    }

private:
    static int listTop(int selected, int count) {
        int top = selected >= kVisibleRows ? selected - kVisibleRows + 1 : 0;
        if (top > count - kVisibleRows) top = count - kVisibleRows;
        return top < 0 ? 0 : top;
    }

    int entryCount() const {
        return 1 + (catalog_ ? static_cast<int>(catalog_->categories.size()) : 0);
    }

    int categoryCount(int entry) const {
        if (!catalog_) return 0;
        // Entry 0 is "all songs": the scanner stores loose /music MP3s with
        // category 0, so summing category 0 would only count the top level.
        if (entry == 0) return static_cast<int>(catalog_->tracks.size());
        int count = 0;
        for (const auto& track : catalog_->tracks) {
            if (track.category == entry) ++count;
        }
        return count;
    }

    void openSelection() {
        if (!catalog_) {
            notify("列表为空，先刷新");
            return;
        }
        const bool already_current =
            currentStory() < 0 && playlist().category() == selected_ && playlist().size() > 0 &&
            (playback().snapshot().state == Audio::PlaybackState::Playing ||
             playback().snapshot().state == Audio::PlaybackState::Paused);
        if (!already_current) {
            if (!playlist().select(catalog_, selected_)) {
                notify("该分类没有曲目");
                return;
            }
            if (!App::playCurrent(true)) {
                notify("播放器加载失败");
                return;
            }
        }
        pushPage(createPlayerPage());
    }

    void requestScan() {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g);
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
        selected_ = 0;
        invalidate();
        if (!ok) notify("扫描失败，请检查 TF 卡");
    }

    int selected_ = 0;
    CatalogPtr catalog_;
};

// ---- current track list --------------------------------------------------

class TrackListPage final : public Page {
public:
    const char* id() const override { return "track-list"; }

    void enter() override {
        selected_ = static_cast<int>(playlist().position());
        invalidate();
    }

    void handle(const Event& event) override {
        const int count = static_cast<int>(playlist().size());
        if (!count) {
            if (event.key == Key::SetLong || event.key == Key::OkDouble) goHome();
            return;
        }
        if (event.delta) {
            selected_ = wrap(selected_ + event.delta, count);
            return;
        }
        switch (event.key) {
            case Key::Next: selected_ = wrap(selected_ + 1, count); break;
            case Key::Prev: selected_ = wrap(selected_ - 1, count); break;
            case Key::Ok:
            case Key::Set: {
                const int delta = selected_ - static_cast<int>(playlist().position());
                if (delta) playlist().move(delta);
                if (App::playCurrent(true)) popPage();
                break;
            }
            // Long NEXT/LAST returns to the player without stopping it, which is
            // how the Arduino track list behaved.
            case Key::NextLong:
            case Key::PrevLong:
            case Key::OkDouble:
                popPage();
                break;
            case Key::OkLong:
                toggleFavorite();
                break;
            case Key::SetLong:
                goHome();
                break;
            default: break;
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g);

        const int total = static_cast<int>(playlist().size());
        const int top = listTop(selected_, total);
        g.fillRect(8, kListRowY, 306, kListRowHeight * kVisibleRows, c.paper);
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) break;
            const int y = kListRowY + row * kListRowHeight;
            const bool selected = index == selected_;
            const bool playing = index == static_cast<int>(playlist().position());
            drawRowCard(g, y, selected);

            const uint16_t background = selected ? c.yellow : TFT_WHITE;
            const int delta = index - static_cast<int>(playlist().position());
            const auto* track = trackAt(delta);
            if (!track) break;

            const bool favorite = App::favorite(globalIndex(delta));
            if (favorite) iconFav(g, 24, y + 7, c.coral);
            else iconNote(g, 24, y + 8, playing ? c.coral : c.aqua);

            char title[96];
            Theme::fitText(g, stripExtension(track->name).c_str(), 200, title, sizeof(title));
            g.setFont(Fonts::title());
            g.setTextColor(playing ? c.coral : c.text, background);
            g.drawString(title, 54, y + 9);

            if (playing) {
                g.setFont(Fonts::small());
                g.setTextColor(c.coral, background);
                g.drawRightString("播放中", 296, y + 14);
            }
        }

        drawCaption(g, "OK播放 长按收藏 长按SET回主页");
    }

private:
    static int listTop(int selected, int count) {
        int top = selected >= kVisibleRows ? selected - kVisibleRows + 1 : 0;
        if (top > count - kVisibleRows) top = count - kVisibleRows;
        return top < 0 ? 0 : top;
    }

    // Walk a playlist copy so browsing never disturbs the active position.
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

// ---- music player --------------------------------------------------------

class PlayerPage final : public Page {
public:
    const char* id() const override { return "player"; }

    void enter() override {
        last_generation_ = 0;
        drawn_second_ = 0xFFFFFFFF;
        last_state_ = -1;
        drawn_volume_ = -1;
        drawn_hall_ = -1;
        view_.reset(nowMs());
        invalidate();
        last_generation_ = playback().snapshot().generation;
    }

    void handle(const Event& event) override {
        if (event.delta) {
            stepVolume(event.delta);
            return;
        }
        const bool playing = playback().snapshot().state == Audio::PlaybackState::Playing;
        switch (event.key) {
            case Key::Ok:
            case Key::Set:
                pausePlayback(playing);
                break;
            case Key::Next:
                if (playlist().move(1)) playCurrent(true);
                break;
            case Key::Prev:
                if (playlist().move(-1)) playCurrent(true);
                break;
            case Key::NextLong:
                pushPage(createTrackListPage());
                break;
            // Long PREV and long OK stop and return to the category list, as in
            // the Arduino player. The player always sits directly above it.
            case Key::PrevLong:
            case Key::OkLong:
                stopPlayback();
                popPage();
                break;
            case Key::SetLong:
                goHome();
                break;
            default: break;
        }
    }

    void tick(uint32_t now) override {
        const auto snapshot = playback().snapshot();
        const uint32_t seconds = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        const int state = static_cast<int>(snapshot.state);

        if (seconds != drawn_second_ || state != last_state_ ||
            ctx().volume != drawn_volume_ || static_cast<int>(ctx().hall) != drawn_hall_) {
            invalidate();
        }

        // New generation: the decoder restarted, so the playlist advances once
        // when the previous track reported EOF.
        if (snapshot.generation != last_generation_) {
            const bool ended = snapshot.state == Audio::PlaybackState::Ended;
            last_generation_ = snapshot.generation;
            last_state_ = -1;
            invalidate();
            if (ended && playlist().move(1)) playCurrent(true);
        }

        const auto model = data();
        view_.tick(model, now);
        if (view_.dirty()) {
            view_.clearDirty();
            invalidate();
        }
    }

    void draw() override {
        view_.draw(Display::canvas(), data());
        const auto snapshot = playback().snapshot();
        drawn_second_ = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        last_state_ = static_cast<int>(snapshot.state);
        drawn_volume_ = ctx().volume;
        drawn_hall_ = ctx().hall ? 1 : 0;
    }

private:
    static const char* categoryLabel(const Media::Track* track) {
        if (!track || track->category <= 0) return "音乐";
        const auto snapshot = App::catalog();
        const auto index = static_cast<std::size_t>(track->category - 1);
        if (!snapshot || index >= snapshot->categories.size()) return "音乐";
        return snapshot->categories[index].c_str();
    }

    PlayerViewData data() const {
        const auto snapshot = playback().snapshot();
        const auto* track = playlist().current();
        PlayerViewData model{};
        model.title = track ? track->name.c_str() : "未发现音乐文件";
        model.category = categoryLabel(track);
        model.cover_label = "音乐播放";
        model.has_track = track != nullptr;
        model.playing = snapshot.state == Audio::PlaybackState::Playing ||
                        snapshot.state == Audio::PlaybackState::Draining;
        model.paused = snapshot.state == Audio::PlaybackState::Paused;
        model.seconds = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        model.duration = snapshot.duration;
        model.bitrate = snapshot.bitrate;
        model.index = static_cast<int>(playlist().position());
        model.count = static_cast<int>(playlist().size());
        model.volume = ctx().volume;
        return model;
    }

    PlayerView view_;
    uint64_t last_generation_ = 0;
    uint32_t drawn_second_ = 0xFFFFFFFF;
    int last_state_ = -1;
    int drawn_volume_ = -1;
    int drawn_hall_ = -1;
};

MusicListPage& musicListInstance() { static MusicListPage instance; return instance; }
TrackListPage& trackListInstance() { static TrackListPage instance; return instance; }
PlayerPage& playerInstance() { static PlayerPage instance; return instance; }

}  // namespace

Page* createMusicListPage() { return &musicListInstance(); }
Page* createTrackListPage() { return &trackListInstance(); }
Page* createPlayerPage() { return &playerInstance(); }

}  // namespace App
