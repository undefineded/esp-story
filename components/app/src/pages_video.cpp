#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "esp_timer.h"
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

// Decoded frames are always 320x240 RGB565. tools/build_video.mjs letterboxes the
// picture and pads with exact black, so "高度铺满" can zoom in until the picture
// meets the top and bottom edges and crop the sides. The content box is measured
// once per clip from the first frames; a clip that already fills the height (or is
// too narrow to crop) keeps the plain blit.
constexpr int kBandRows = 8;
constexpr int kProbeFrames = 24;
uint16_t s_band[kVideoWidth * kBandRows];

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool darkPixel(uint16_t pixel) {
    return (pixel >> 11) < 2u && ((pixel >> 5) & 0x3fu) < 2u && (pixel & 0x1fu) < 2u;
}

struct ContentBox {
    int x = 0, y = 0, w = kVideoWidth, h = kVideoHeight;
};

// Bounding box of the non-black samples. Every other pixel is sampled: the padded
// edges are within a pixel and the probe stays around 40k reads per frame.
ContentBox measureContent(const uint16_t* pixels) {
    int left = kVideoWidth, right = -1, top = -1, bottom = -1;
    for (int y = 0; y < kVideoHeight; y += 2) {
        const uint16_t* row = pixels + y * kVideoWidth;
        int first = -1, last = -1;
        for (int x = 0; x < kVideoWidth; x += 2) {
            if (!darkPixel(row[x])) { first = x; break; }
        }
        if (first < 0) continue;
        for (int x = kVideoWidth - 2; x >= 0; x -= 2) {
            if (!darkPixel(row[x])) { last = x; break; }
        }
        if (top < 0) top = y;
        bottom = y;
        if (first < left) left = first;
        if (last > right) right = last;
    }
    if (top < 0 || right < 0) return ContentBox{0, 0, 0, 0};  // nothing lit: no opinion yet
    return ContentBox{left, top, right - left + 1, bottom - top + 1};
}

ContentBox unite(const ContentBox& a, const ContentBox& b) {
    const int left = a.x < b.x ? a.x : b.x;
    const int top = a.y < b.y ? a.y : b.y;
    const int right_a = a.x + a.w, right_b = b.x + b.w;
    const int bottom_a = a.y + a.h, bottom_b = b.y + b.h;
    const int right = right_a > right_b ? right_a : right_b;
    const int bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
    return ContentBox{left, top, right - left, bottom - top};
}

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

// Folder glyph (same shape as the music category rows).
void iconFolder(lgfx::LGFXBase& g, int x, int y, uint16_t color) {
    const auto& c = Theme::colors();
    g.fillRect(x, y + 2, 10, 5, color);
    g.drawRect(x, y + 2, 10, 5, c.ink);
    g.fillRoundRect(x, y + 5, 22, 15, 3, color);
    g.drawRoundRect(x, y + 5, 22, 15, 3, c.ink);
}

class VideoListPage final : public Page {
public:
    const char* id() const override { return "video-list"; }

    void enter() override {
        if (!cache_.refresh(storageGate(), Storage::SdVolume::root))
            notify("视频目录暂不可用，请稍后重新进入");
        buildRows();
        invalidate();
    }

    void handle(const Event& event) override {
        const int total = static_cast<int>(rows_.size());
        if (!total) {
            if (event.key == Key::SetLong || event.key == Key::OkDouble) goHome();
            return;
        }
        if (event.delta) {
            selected_ = wrap(selected_ + event.delta, total);
            invalidate();
            return;
        }
        switch (event.key) {
            case Key::Next: selected_ = wrap(selected_ + 1, total); invalidate(); break;
            case Key::Prev: selected_ = wrap(selected_ - 1, total); invalidate(); break;
            case Key::Ok:
            case Key::Set:
                activate();
                break;
            case Key::OkLong:
                if (!folder_.empty()) leaveFolder();
                else popPage();
                break;
            case Key::OkDouble:
                if (!folder_.empty()) leaveFolder();
                else goHome();
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
        g.fillScreen(c.paper);
        drawTopBar(g, folder_.empty() ? nullptr : folder_.c_str());

        const int total = static_cast<int>(rows_.size());
        const int top = listTop(selected_, total);
        g.fillRect(8, kListRowY, 306, kListRowHeight * kVisibleRows, c.paper);
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) break;
            const int y = kListRowY + row * kListRowHeight;
            const bool selected = index == selected_;
            const auto& item = rows_[static_cast<std::size_t>(index)];
            const bool playing = !item.folder && item.index == currentVideo();
            const uint16_t background = selected ? c.yellow : TFT_WHITE;
            if (selected) g.fillRoundRect(13, y + 3, 300, 38, 6, c.ink);
            g.fillRoundRect(10, y, 300, 38, 6, selected ? c.ink : TFT_WHITE);
            g.fillRoundRect(12, y + 2, 296, 34, 5, background);
            if (!selected) g.drawRoundRect(10, y, 300, 38, 6, c.track_border);

            if (item.folder) iconFolder(g, 24, y + 10, selected ? c.coral : c.aqua);
            else iconFilm(g, 24, y + 11, playing ? c.coral : c.aqua);

            char title[96];
            Theme::fitText(g, item.name.c_str(), 200, title, sizeof(title));
            g.setFont(Fonts::title());
            g.setTextColor(playing ? c.coral : c.text, background);
            g.drawString(title, 54, y + 9);

            char right[16] = {};
            if (item.folder) std::snprintf(right, sizeof(right), "%d个", item.count);
            else if (playing) std::snprintf(right, sizeof(right), "播放中");
            if (right[0]) {
                g.setFont(Fonts::small());
                g.setTextColor(playing ? c.coral : c.text, background);
                g.drawRightString(right, 296, y + 14);
            }
        }

        g.setFont(Fonts::body());
        g.setTextColor(c.text, c.paper);
        const char* hint = !total ? "/video 下没有 AVI 文件"
                           : folder_.empty() ? "OK进入分类  长按SET回主页"
                                             : "OK播放  长按OK返回分类";
        g.drawCenterString(hint, 160, 212);
    }

private:
    struct Row {
        std::string name;
        int count = 0;
        int index = -1;    // flat entry index for file rows
        bool folder = false;
    };

    void buildRows() {
        rows_.clear();
        if (folder_.empty()) {
            std::vector<std::string> groups;
            for (const auto& entry : entries_) {
                if (entry.group.empty()) continue;
                if (std::find(groups.begin(), groups.end(), entry.group) == groups.end())
                    groups.push_back(entry.group);
            }
            std::sort(groups.begin(), groups.end());
            for (const auto& group : groups) {
                int count = 0;
                for (const auto& entry : entries_) {
                    if (entry.group == group) ++count;
                }
                rows_.push_back({group, count, -1, true});
            }
            for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
                if (entries_[static_cast<std::size_t>(i)].group.empty())
                    rows_.push_back({entries_[static_cast<std::size_t>(i)].title, 0, i, false});
            }
        } else {
            for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
                if (entries_[static_cast<std::size_t>(i)].group == folder_)
                    rows_.push_back({entries_[static_cast<std::size_t>(i)].title, 0, i, false});
            }
        }
        if (selected_ >= static_cast<int>(rows_.size())) selected_ = 0;
    }

    void leaveFolder() {
        folder_.clear();
        selected_ = 0;
        buildRows();
        invalidate();
    }

    void activate() {
        const auto& row = rows_[static_cast<std::size_t>(selected_)];
        if (row.folder) {
            folder_ = row.name;
            selected_ = 0;
            buildRows();
            invalidate();
            return;
        }
        openVideo(static_cast<std::size_t>(row.index));
    }

    static int listTop(int selected, int count) {
        int top = selected >= kVisibleRows ? selected - kVisibleRows + 1 : 0;
        if (top > count - kVisibleRows) top = count - kVisibleRows;
        return top < 0 ? 0 : top;
    }

    void openVideo(std::size_t index);
    Video::LibraryCache cache_;
    const std::vector<Video::Entry>& entries_ = cache_.entries;
    std::vector<Row> rows_;
    std::string folder_;
    int selected_ = 0;
};

class VideoPlayerPage final : public Page {
public:
    const char* id() const override { return "video-player"; }

    void setVideo(std::size_t index, const std::vector<Video::Entry>& entries) { index_ = index; entries_ = entries; }

    void enter() override {
        close();
        setCurrentVideo(-1);
        last_action_ms_ = nowMs();
        volume_until_ms_ = 0;
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
        // A pre-cropped "<stem>.fill.avi" is sharper than cropping the letterboxed
        // file at runtime, so prefer it when the user picked "高度铺满".
        const bool use_fill = ctx().settings.video_fit == 1 && !entry.fill_path.empty();
        runtime_fit_ = ctx().settings.video_fit == 1 && entry.fill_path.empty();
        Video::setVolume(ctx().volume);
        Video::setEq(static_cast<Audio::EqPreset>(ctx().settings.eq));
        if (!Video::open(storageGate(), playback(),
                         Storage::SdVolume::root + (use_fill ? entry.fill_path : entry.path))) {
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
        last_action_ms_ = nowMs();
        if (event.delta) {
            stepVolume(event.delta);
            volume_until_ms_ = last_action_ms_;
            invalidate();
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
        const bool drawn = Video::visitFrame(&VideoPlayerPage::frameThunk, this);
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
    static void frameThunk(const uint16_t* pixels, void* context) {
        static_cast<VideoPlayerPage*>(context)->renderFrame(pixels);
    }

    // The mailbox hands over the decoded picture; drawing never blocks decoding.
    void renderFrame(const uint16_t* pixels) {
        auto& g = Display::canvas();
        if (!runtime_fit_) {
            pushFull(g, pixels);
            return;
        }
        if (probed_ < kProbeFrames) probe(pixels);
        if (!fill_) {
            pushFull(g, pixels);
            return;
        }
        for (int y = 0; y < kVideoHeight; y += kBandRows) {
            const int rows = (kVideoHeight - y) < kBandRows ? (kVideoHeight - y) : kBandRows;
            for (int r = 0; r < rows; ++r) {
                int sy = box_.y + (y + r) * box_.h / kVideoHeight;
                if (sy < 0) sy = 0;
                if (sy > kVideoHeight - 1) sy = kVideoHeight - 1;
                const uint16_t* src = pixels + sy * kVideoWidth;
                uint16_t* dst = s_band + r * kVideoWidth;
                for (int x = 0; x < kVideoWidth; ++x) dst[x] = src[source_x_[x]];
            }
            g.pushImage(0, y, kVideoWidth, rows, reinterpret_cast<const lgfx::rgb565_t*>(s_band));
        }
    }

    static void pushFull(lgfx::LGFXBase& g, const uint16_t* pixels) {
        g.pushImage(0, 0, kVideoWidth, kVideoHeight, reinterpret_cast<const lgfx::rgb565_t*>(pixels));
    }

    // Learn the picture box from the opening frames, then keep it. A dark opening
    // frame is merged with the following ones instead of deciding the crop early.
    void probe(const uint16_t* pixels) {
        const ContentBox measured = measureContent(pixels);
        if (measured.w > 2 && measured.h > 2) {
            box_ = box_valid_ ? unite(box_, measured) : measured;
            box_valid_ = true;
            updateFit();
        }
        ++probed_;
    }

    void updateFit() {
        const int visible = kVideoWidth * box_.h / kVideoHeight;
        if (box_.h >= kVideoHeight - 2 || visible >= box_.w) {
            fill_ = false;
            return;
        }
        const int x0 = box_.x + (box_.w - visible) / 2;
        for (int x = 0; x < kVideoWidth; ++x) {
            int sx = x0 + x * visible / kVideoWidth;
            if (sx < 0) sx = 0;
            if (sx > kVideoWidth - 1) sx = kVideoWidth - 1;
            source_x_[x] = sx;
        }
        fill_ = true;
    }

    void resetFit() {
        box_ = ContentBox{};
        box_valid_ = false;
        fill_ = false;
        probed_ = 0;
    }

    void close(bool drain_audio = false) {
        if (Video::active()) setNowPlaying("idle", "");
        Video::close(drain_audio);
        setVideoActive(false);
        setCurrentVideo(-1);
        Display::setVideoMode(false);
        has_frame_ = false;
        error_.clear();
        resetFit();
    }

    void drawOverlay() {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        const uint64_t elapsed = status_.position_ms;
        const uint64_t total = status_.duration_ms;
        const uint32_t now = nowMs();
        const bool show_volume = volume_until_ms_ != 0 && (now - volume_until_ms_) < 2000u;
        // Full controls for the opening seconds / recent input / volume changes;
        // after that only a 3px strip so the picture stays unobstructed.
        const bool full = elapsed < 3000u || (now - last_action_ms_) < 4000u || show_volume;

        if (!full) {
            const int width = total > 0
                                  ? static_cast<int>(static_cast<uint64_t>(kVideoWidth) * elapsed / total)
                                  : 0;
            g.fillRect(0, kVideoHeight - 3, kVideoWidth, 3, c.ink);
            if (width > 0) {
                const int w = width > kVideoWidth ? kVideoWidth : width;
                g.fillRect(0, kVideoHeight - 3, w, 3, c.blue);
                if (w > 4) g.fillRect(w - 4, kVideoHeight - 3, 4, 3, c.yellow);
            }
            return;
        }

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

        // Volume readout while the knob is being turned (the video bar has no
        // volume section, which made it look like the encoder did nothing).
        if (show_volume) {
            char volume[24];
            std::snprintf(volume, sizeof(volume), "音量:%d", ctx().volume);
            g.setFont(Fonts::body());
            const int box = g.textWidth(volume) + 20;
            const int x = 320 - box - 10;
            g.fillRoundRect(x, 186, box, 26, 7, c.ink);
            g.setTextColor(TFT_WHITE, c.ink);
            g.drawString(volume, x + 10, 189);
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
    ContentBox box_{};
    bool box_valid_ = false;
    bool fill_ = false;
    int probed_ = 0;
    int source_x_[kVideoWidth] = {};
    bool runtime_fit_ = false;
    uint32_t last_action_ms_ = 0;
    uint32_t volume_until_ms_ = 0;
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
