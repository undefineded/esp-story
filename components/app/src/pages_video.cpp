#include <cstdio>
#include <string>
#include <vector>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "video_player.h"
#include "display.h"
#include "fonts.h"
#include "sd_volume.h"
#include "video_library.h"

namespace App {
namespace {

constexpr int kVisibleRows = 4;
constexpr int kListRowY = 36;
constexpr int kListRowHeight = 42;
constexpr int kVideoWidth = 320;
constexpr int kVideoHeight = 240;

int wrap(int value, int count) {
    if (count <= 0) return 0;
    value %= count;
    return value < 0 ? value + count : value;
}


void drawTopBar(lgfx::LGFXBase& g, const char* title = nullptr) {
    const auto& c = Theme::colors();
    Theme::setTopBarColor(c.coral);
    g.fillRect(0, 0, 320, 30, c.coral);
    g.fillRect(0, 30, 320, 2, c.coral_dark);
    Theme::drawSmileFace(g, 19, 15, 10);
    g.setFont(Fonts::body());
    g.setTextColor(TFT_WHITE, c.coral);
    g.drawString("小小故事盒", 36, 7);
    if (title && title[0]) g.drawRightString(title, 310, 7);
    Theme::drawHallIcon(g, ctx().hall, c.coral);
    Theme::drawChildLockBadge(g, ctx().child_locked, c.coral);
}

// Film glyph from the Arduino video list: 20x16 card with sprocket holes.
void iconFilm(lgfx::LGFXBase& g, int x, int y, uint16_t color) {
    const auto& c = Theme::colors();
    g.fillRoundRect(x, y, 20, 16, 3, color);
    for (int i = 0; i < 3; ++i) {
        g.fillRect(x + 3 + i * 5, y + 2, 2, 2, c.paper);
        g.fillRect(x + 3 + i * 5, y + 12, 2, 2, c.paper);
    }
    g.fillTriangle(x + 8, y + 5, x + 8, y + 11, x + 13, y + 8, c.paper);
}

class VideoListPage final : public Page {
public:
    const char* id() const override { return "video-list"; }

    void enter() override {
        if (!cache_.refresh(storageGate(), Storage::SdVolume::root))
            notify("视频目录暂不可用，请稍后重新进入");
        if (selected_ >= static_cast<int>(entries_.size())) selected_ = 0;
        invalidate();
    }

    void handle(const Event& event) override {
        const int total = static_cast<int>(entries_.size());
        if (!total) {
            if (event.key == Key::SetLong || event.key == Key::OkDouble) goHome();
            return;
        }
        if (event.delta) {
            selected_ = wrap(selected_ + event.delta, total);
            return;
        }
        switch (event.key) {
            case Key::Next: selected_ = wrap(selected_ + 1, total); break;
            case Key::Prev: selected_ = wrap(selected_ - 1, total); break;
            case Key::Ok:
            case Key::Set:
                openVideo(static_cast<std::size_t>(selected_));
                break;
            case Key::SetLong:
            case Key::OkDouble:
                goHome();
                break;
            default:
                break;
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g);

        const int total = static_cast<int>(entries_.size());
        const int top = listTop(selected_, total);
        g.fillRect(8, kListRowY, 306, kListRowHeight * kVisibleRows, c.paper);
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) break;
            const int y = kListRowY + row * kListRowHeight;
            const bool selected = index == selected_;
            const bool playing = currentVideo() == index;
            const uint16_t background = selected ? c.yellow : TFT_WHITE;
            if (selected) g.fillRoundRect(13, y + 3, 300, 38, 6, c.ink);
            g.fillRoundRect(10, y, 300, 38, 6, selected ? c.ink : TFT_WHITE);
            g.fillRoundRect(12, y + 2, 296, 34, 5, background);
            if (!selected) g.drawRoundRect(10, y, 300, 38, 6, c.track_border);

            iconFilm(g, 24, y + 11, playing ? c.coral : c.aqua);

            char title[96];
            Theme::fitText(g, entries_[index].title.c_str(), 200, title, sizeof(title));
            g.setFont(Fonts::title());
            g.setTextColor(playing ? c.coral : c.text, background);
            g.drawString(title, 54, y + 9);

            if (playing) {
                g.setFont(Fonts::small());
                g.setTextColor(c.coral, background);
                g.drawRightString("播放中", 296, y + 14);
            }
        }

        g.setFont(Fonts::body());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString(total ? "OK播放  NEXT/LAST选择  长按SET回主页" : "/video 下没有 AVI 文件",
                           160, 212);
    }

private:
    static int listTop(int selected, int count) {
        int top = selected >= kVisibleRows ? selected - kVisibleRows + 1 : 0;
        if (top > count - kVisibleRows) top = count - kVisibleRows;
        return top < 0 ? 0 : top;
    }

    void openVideo(std::size_t index);
    Video::LibraryCache cache_;
    const std::vector<Video::Entry>& entries_ = cache_.entries;
    int selected_ = 0;
};

class VideoPlayerPage final : public Page {
public:
    const char* id() const override { return "video-player"; }

    void setVideo(std::size_t index, const std::vector<Video::Entry>& entries) { index_ = index; entries_ = entries; }

    void enter() override {
        close();
        setCurrentVideo(-1);
        if (!playbackAllowed()) {
            error_ = "家长限制播放";
            invalidate();
            return;
        }
        if (index_ >= entries_.size()) {
            error_ = "没有视频";
            invalidate();
            return;
        }
        const auto& entry = entries_[index_];
        Video::setVolume(ctx().volume);
        Video::setEq(static_cast<Audio::EqPreset>(ctx().settings.eq));
        if (!Video::open(storageGate(), playback(), Storage::SdVolume::root + entry.path)) {
            error_ = Video::error();
            invalidate();
            return;
        }
        setCurrentVideo(static_cast<int>(index_));
        setVideoActive(true);
        title_ = entry.title;
        setNowPlaying("video", title_.c_str());
        paused_ = false;
        finished_ = false;
        status_ = {};
        status_.state = Video::State::Preparing;
        fps_ = fps_frames_ = 0;
        fps_window_ms_ = 0;
        invalidate();
    }

    void leave() override {
        close();
        setCurrentVideo(-1);
        setVideoActive(false);
    }

    void handle(const Event& event) override {
        if (event.delta) {
            stepVolume(event.delta);
            return;
        }
        switch (event.key) {
            case Key::Ok:
                togglePause();
                break;
            case Key::NextLong:
            case Key::OkLong:
                close();
                popPage();
                break;
            case Key::SetLong:
                close();
                goHome();
                break;
            case Key::Next:
            case Key::Prev: {
                if (entries_.empty()) break;
                const int count = static_cast<int>(entries_.size());
                index_ = static_cast<std::size_t>(
                    wrap(static_cast<int>(index_) + (event.key == Key::Next ? 1 : -1), count));
                enter();
                break;
            }
            default:
                break;
        }
    }

    void tick(uint32_t now) override {
        if (finished_) return;
        if (!Video::active()) {
            if (!finished_ && error_.empty()) {
                setVideoActive(false);
                finished_ = true;
                invalidate();
            }
            return;
        }
        if (!playbackAllowed()) {
            close();
            error_ = "家长限制播放";
            invalidate();
            return;
        }
        const auto previous = status_.state;
        status_ = Video::status();
        if (previous != status_.state) invalidate();
        paused_ = status_.state == Video::State::Paused;
        setVideoActive(status_.state == Video::State::Preparing ||
                       status_.state == Video::State::Playing);
        if (status_.state == Video::State::Failed || status_.state == Video::State::Finished) {
            const bool failed = status_.state == Video::State::Failed;
            // Resource cleanup must not depend on draw(): power policy can
            // suppress drawing in this very iteration. Keep the last canvas.
            Video::close(!failed);
            setVideoActive(false);
            setCurrentVideo(-1);
            setNowPlaying("idle", "");
            finished_ = true;
            if (failed) error_ = status_.error[0] ? status_.error : "解码失败，请转换为 MJPEG AVI";
            else notify("播放完毕，OK 重播", 1500);
            invalidate();
            return;
        }
        // Displayed frame rate: pending frames only become counted when the page
        // repaints them, so a slow SPI push shows up here instead of hiding.
        if (Video::framePending()) {
            ++fps_frames_;
            invalidate();
        }
        if (!fps_window_ms_) fps_window_ms_ = now;
        else if (now - fps_window_ms_ >= 1000) {
            fps_ = static_cast<unsigned>(fps_frames_ * 1000u / (now - fps_window_ms_));
            fps_frames_ = 0;
            fps_window_ms_ = now;
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        if (!error_.empty()) {
            Display::setVideoMode(false);
            g.fillScreen(c.paper);
            drawTopBar(g, "视频");
            Theme::drawSmileFace(g, 160, 96, 16);
            g.setFont(Fonts::title());
            g.setTextColor(c.text, c.paper);
            g.drawCenterString("视频无法播放", 160, 130);
            g.setFont(Fonts::body());
            g.drawCenterString(error_.c_str(), 160, 158);
            g.setFont(Fonts::small());
            g.drawCenterString("长按 OK 返回列表", 160, 210);
            return;
        }
        const bool drawn = Video::visitFrame([](const uint16_t* pixels, void* context) {
            static_cast<lgfx::LGFXBase*>(context)->pushImage(0, 0, kVideoWidth, kVideoHeight,
                reinterpret_cast<const lgfx::rgb565_t*>(pixels));
        }, &g);
        has_frame_ = has_frame_ || drawn;
        if (!has_frame_) {
            g.fillScreen(TFT_BLACK);
            g.setFont(Fonts::body());
            g.setTextColor(TFT_WHITE, TFT_BLACK);
            g.drawCenterString(finished_ ? "播放完毕，OK 重播" : "正在加载视频…", 160, 110);
        }
        Display::setVideoMode(true);
        drawOverlay();
    }

private:
    void close(bool drain_audio = false) {
        if (Video::active()) setNowPlaying("idle", "");
        Video::close(drain_audio);
        setVideoActive(false);
        setCurrentVideo(-1);
        Display::setVideoMode(false);
        has_frame_ = false;
        error_.clear();
    }

    void drawOverlay() {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        const uint64_t elapsed = status_.position_ms;
        const uint64_t total = status_.duration_ms;

        // The frame covers the whole panel, so the bar is redrawn every frame.
        // Geometry follows the Arduino video bar: 30px ink bar, yellow top edge,
        // 22x20 yellow pause button, progress groove and a right-aligned time.
        g.fillRect(0, 210, 320, 30, c.ink);
        g.fillRect(0, 210, 320, 2, c.yellow);
        g.fillRoundRect(8, 215, 22, 20, 6, c.yellow);
        const int cx = 8 + 11;
        const int cy = 215 + 10;
        if (paused_) {
            g.fillTriangle(cx - 4, cy - 5, cx - 4, cy + 5, cx + 5, cy, c.ink);
        } else {
            g.fillRect(cx - 4, cy - 5, 3, 10, c.ink);
            g.fillRect(cx + 2, cy - 5, 3, 10, c.ink);
        }

        constexpr int kProgressX = 38, kProgressY = 222, kProgressW = 196, kProgressH = 6;
        g.fillRoundRect(kProgressX, kProgressY, kProgressW, kProgressH, 2, c.track);
        g.drawRoundRect(kProgressX, kProgressY, kProgressW, kProgressH, 2, c.track_border);
        int width = 0;
        if (total > 0) width = static_cast<int>(static_cast<uint64_t>(kProgressW - 2) * elapsed / total);
        if (width > kProgressW - 2) width = kProgressW - 2;
        if (width > 1) {
            g.fillRect(kProgressX + 1, kProgressY + 1, width, kProgressH - 2, c.blue);
            if (width > 4) g.fillRect(kProgressX + 1 + width - 4, kProgressY + 1, 4, kProgressH - 2, c.yellow);
        }

        char time[48];
        std::snprintf(time, sizeof(time), "%02u:%02u/%02u:%02u",
                      static_cast<unsigned>((elapsed / 1000) / 60), static_cast<unsigned>((elapsed / 1000) % 60),
                      static_cast<unsigned>((total / 1000) / 60), static_cast<unsigned>((total / 1000) % 60));
        g.setFont(Fonts::small());
        g.setTextColor(c.track, c.ink);
        g.drawRightString(time, 312, 219);

        // Actual panel frame rate, plus the backend's frame/drop counters.
        char stats[40];
        std::snprintf(stats, sizeof(stats), "FPS%2u F%u D%u", unsigned(fps_),
                      unsigned(status_.frames), unsigned(status_.replaced_frames));
        g.setTextColor(c.yellow, c.ink);
        g.drawRightString(stats, 312, 10);

        // Title card for the first three seconds and while paused.
        if (paused_ || elapsed < 3000) {
            if (!title_.empty()) {
                char title[96];
                Theme::fitText(g, title_.c_str(), 212, title, sizeof(title));
                g.setFont(Fonts::body());
                const int box = g.textWidth(title) + 20;
                g.fillRoundRect(8, 8, box, 26, 7, c.ink);
                g.setTextColor(TFT_WHITE, c.ink);
                g.drawString(title, 18, 11);
            }
        }
    }

    void togglePause() {
        if (finished_ || !error_.empty()) { enter(); return; }
        if (Video::pause(!paused_)) {
            paused_ = !paused_;
            setVideoActive(!paused_);
            invalidate();
        }
    }

    std::size_t index_ = 0;
    std::string title_, error_;
    std::vector<Video::Entry> entries_;
    bool has_frame_ = false;
    Video::Status status_{};
    bool paused_ = false;
    bool finished_ = false;
    uint32_t fps_window_ms_ = 0, fps_frames_ = 0, fps_ = 0;
};

VideoListPage& videoListInstance() { static VideoListPage instance; return instance; }
VideoPlayerPage& videoPlayerInstance() { static VideoPlayerPage instance; return instance; }

void VideoListPage::openVideo(std::size_t index) {
    videoPlayerInstance().setVideo(index, entries_);
    pushPage(createVideoPlayerPage());
}

}  // namespace

Page* createVideoListPage() { return &videoListInstance(); }
Page* createVideoPlayerPage() { return &videoPlayerInstance(); }

}  // namespace App
