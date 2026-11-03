#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "avi_index.h"
#include "display.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "fonts.h"
#include "sd_volume.h"
#include "video_library.h"

namespace App {
namespace {

constexpr int kVisibleRows = 4;
constexpr int kListRowY = 36;
constexpr int kListRowHeight = 38;
constexpr int kVideoWidth = 320;
constexpr int kVideoHeight = 240;

int wrap(int value, int count) {
    if (count <= 0) return 0;
    value %= count;
    return value < 0 ? value + count : value;
}

uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

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

class VideoListPage final : public Page {
public:
    const char* id() const override { return "video-list"; }

    void enter() override {
        entries_ = Video::scanLibrary(Storage::SdVolume::root);
        selected_ = 0;
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
        drawTopBar(g, "视频");

        const int total = static_cast<int>(entries_.size());
        int top = selected_ - (kVisibleRows - 1);
        if (top > total - kVisibleRows) top = total - kVisibleRows;
        if (top < 0) top = 0;

        g.fillRect(8, kListRowY, 310, 172, c.paper);
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) break;
            const int y = kListRowY + row * kListRowHeight;
            const bool selected = index == selected_;
            const uint16_t background = selected ? c.yellow : TFT_WHITE;
            if (selected) g.fillRoundRect(13, y + 3, 300, kListRowHeight, 6, c.ink);
            g.fillRoundRect(10, y, 300, kListRowHeight, 6, selected ? c.ink : TFT_WHITE);
            g.fillRoundRect(12, y + 2, 296, kListRowHeight - 4, 5, background);
            if (!selected) g.drawRoundRect(10, y, 300, kListRowHeight, 6, c.track_border);

            // Film glyph.
            g.fillRoundRect(24, y + 11, 16, 14, 2, c.ink);
            g.fillRect(27, y + 14, 3, 3, background);
            g.fillRect(27, y + 19, 3, 3, background);
            g.fillRect(34, y + 14, 3, 3, background);
            g.fillRect(34, y + 19, 3, 3, background);

            char title[96];
            Theme::fitText(g, entries_[index].title.c_str(), 210, title, sizeof(title));
            g.setFont(Fonts::body());
            g.setTextColor(c.text, background);
            g.drawString(title, 48, y + 8);

            g.setFont(Fonts::small());
            g.drawRightString("AVI", 296, y + 11);
        }

        g.setFont(Fonts::small());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString(total ? "OK 播放  SET 长按回主页" : "/video 下没有 AVI 文件", 160, 224);
    }

private:
    void openVideo(std::size_t index);
    std::vector<Video::Entry> entries_;
    int selected_ = 0;
};

class VideoPlayerPage final : public Page {
public:
    const char* id() const override { return "video-player"; }

    void setVideo(std::size_t index) { index_ = index; }

    void enter() override {
        close();
        setVideoActive(true);
        const auto entries = Video::scanLibrary(Storage::SdVolume::root);
        if (index_ >= entries.size()) {
            error_ = "没有视频";
            invalidate();
            return;
        }
        const auto& entry = entries[index_];
        if (!avi_.open(storageGate(), Storage::SdVolume::root, entry.path)) {
            error_ = avi_.error().empty() ? "打开失败" : avi_.error();
            invalidate();
            return;
        }
        if (avi_.width() != kVideoWidth || avi_.height() != kVideoHeight) {
            error_ = "仅支持 320x240";
            avi_.close();
            invalidate();
            return;
        }
        started_at_us_ = nowUs();
        paused_at_us_ = 0;
        paused_ms_ = 0;
        paused_ = false;
        finished_ = false;
        shown_frame_ = -1;
        last_decode_us_ = 0;

        // The narration track is optional; without it the clip is silent.
        const auto audio = Video::audioPathFor(entry.path);
        struct stat status {};
        const bool has_track = stat((Storage::SdVolume::root + audio).c_str(), &status) == 0;
        has_audio_ = false;
        if (has_track) {
            uint64_t request = 0;
            has_audio_ = playback().play(audio.c_str(), true, request) == ESP_OK;
        }
        invalidate();
    }

    void leave() override {
        close();
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
                const auto entries = Video::scanLibrary(Storage::SdVolume::root);
                if (entries.empty()) break;
                const int count = static_cast<int>(entries.size());
                index_ = static_cast<std::size_t>(
                    wrap(static_cast<int>(index_) + (event.key == Key::Next ? 1 : -1), count));
                enter();
                break;
            }
            default:
                break;
        }
    }

    void tick(uint32_t) override {
        if (!avi_.frames() || !error_.empty()) return;
        const uint32_t interval = avi_.frameIntervalMs() ? avi_.frameIntervalMs() : 40;
        std::size_t frame = elapsedMs() / interval;
        if (frame >= avi_.frames()) {
            frame = avi_.frames() - 1;
            if (!finished_) {
                finished_ = true;
                notify("播放完毕", 1500);
            }
        }
        if (static_cast<int>(frame) != shown_frame_) invalidate();
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        if (!error_.empty()) {
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
        const uint32_t interval = avi_.frameIntervalMs() ? avi_.frameIntervalMs() : 40;
        std::size_t frame = elapsedMs() / interval;
        if (frame >= avi_.frames()) frame = avi_.frames() - 1;

        if (static_cast<int>(frame) != shown_frame_) {
            decodeFrame(frame);
            shown_frame_ = static_cast<int>(frame);
        }
        drawOverlay(frame);
    }

private:
    void close() {
        if (has_audio_) stopPlayback();
        has_audio_ = false;
        avi_.close();
        error_.clear();
    }

    void decodeFrame(std::size_t frame) {
        auto& g = Display::canvas();
        const uint64_t started = nowUs();
        if (!avi_.readFrame(frame, storageGate(), Storage::SdVolume::root, buffer_)) {
            error_ = avi_.error().empty() ? "读取帧失败" : avi_.error();
            return;
        }
        const bool ok = avi_.codec() == Video::Codec::Qoi
                            ? g.drawQoi(buffer_.data(), buffer_.size(), 0, 0, kVideoWidth, kVideoHeight)
                            : g.drawJpg(buffer_.data(), buffer_.size(), 0, 0, kVideoWidth, kVideoHeight);
        if (!ok) {
            g.fillScreen(TFT_BLACK);
            g.setFont(Fonts::body());
            g.setTextColor(TFT_WHITE, TFT_BLACK);
            g.drawCenterString("帧解码失败", 160, 110);
        }
        last_decode_us_ = nowUs() - started;
    }

    void drawOverlay(std::size_t frame) {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        // The frame covers the whole panel, so the bar is redrawn every frame.
        g.fillRect(0, 210, 320, 30, c.ink);
        g.fillRect(0, 210, 320, 2, c.yellow);
        if (paused_) {
            g.fillTriangle(10, 216, 10, 232, 22, 224, c.yellow);
        } else {
            g.fillRect(11, 216, 4, 16, c.yellow);
            g.fillRect(18, 216, 4, 16, c.yellow);
        }
        const uint32_t elapsed = elapsedMs();
        const uint32_t total = avi_.durationMs();
        char position[48];
        std::snprintf(position, sizeof(position), "%02u:%02u / %02u:%02u",
                      static_cast<unsigned>((elapsed / 1000) / 60), static_cast<unsigned>((elapsed / 1000) % 60),
                      static_cast<unsigned>((total / 1000) / 60), static_cast<unsigned>((total / 1000) % 60));
        g.setFont(Fonts::small());
        g.setTextColor(TFT_WHITE, c.ink);
        g.drawString(position, 30, 216);

        char volume[24];
        std::snprintf(volume, sizeof(volume), "VOL %d", ctx().volume);
        g.setTextColor(c.yellow, c.ink);
        g.drawRightString(volume, 312, 216);

        char stats[48];
        std::snprintf(stats, sizeof(stats), "%u/%u", static_cast<unsigned>(frame + 1),
                      static_cast<unsigned>(avi_.frames()));
        g.setTextColor(TFT_WHITE, c.ink);
        g.drawString(stats, 30, 228);
        std::snprintf(stats, sizeof(stats), "%u ms", static_cast<unsigned>(last_decode_us_ / 1000));
        g.drawRightString(stats, 312, 228);
    }

    void togglePause() {
        if (paused_) {
            paused_ = false;
            paused_ms_ += static_cast<uint32_t>((nowUs() - paused_at_us_) / 1000);
            if (has_audio_) pausePlayback(false);
        } else {
            paused_ = true;
            paused_at_us_ = nowUs();
            if (has_audio_) pausePlayback(true);
        }
        invalidate();
    }

    uint32_t elapsedMs() const {
        const uint64_t reference = paused_ ? paused_at_us_ : nowUs();
        return static_cast<uint32_t>((reference - started_at_us_) / 1000) - paused_ms_;
    }

    std::size_t index_ = 0;
    Video::AviIndex avi_;
    std::vector<uint8_t> buffer_;
    std::string error_;
    bool has_audio_ = false;
    bool paused_ = false;
    bool finished_ = false;
    uint64_t started_at_us_ = 0;
    uint64_t paused_at_us_ = 0;
    uint32_t paused_ms_ = 0;
    uint64_t last_decode_us_ = 0;
    int shown_frame_ = -1;
};

VideoListPage& videoListInstance() { static VideoListPage instance; return instance; }
VideoPlayerPage& videoPlayerInstance() { static VideoPlayerPage instance; return instance; }

void VideoListPage::openVideo(std::size_t index) {
    videoPlayerInstance().setVideo(index);
    pushPage(createVideoPlayerPage());
}

}  // namespace

Page* createVideoListPage() { return &videoListInstance(); }
Page* createVideoPlayerPage() { return &videoPlayerInstance(); }

}  // namespace App
