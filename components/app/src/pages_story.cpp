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
#include "sd_volume.h"
#include "story_image.h"
#include "story_timeline.h"

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

const char* kindLabel(Media::StoryKind kind) {
    return kind == Media::StoryKind::Illustrated ? "图文" : "音频";
}

class StoryListPage final : public Page {
public:
    const char* id() const override { return "story-list"; }

    void enter() override {
        if (!catalog_) catalog_ = App::catalog();
        if (selected_ >= count()) selected_ = 0;
        invalidate();
        if (!ctx().catalog_ready) requestScan();
    }

    void handle(const Event& event) override {
        const int total = count();
        if (!total) return;
        if (event.delta) {
            selected_ = wrap(selected_ + event.delta, total);
            return;
        }
        switch (event.key) {
            case Key::Next: selected_ = wrap(selected_ + 1, total); break;
            case Key::Prev: selected_ = wrap(selected_ - 1, total); break;
            case Key::Ok:
            case Key::Set:
                if (catalog_ && selected_ < static_cast<int>(catalog_->stories.size())) {
                    openStory(static_cast<std::size_t>(selected_));
                }
                break;
            case Key::SetLong:
            case Key::OkDouble:
                goHome();
                break;
            default:
                break;
        }
    }

    void tick(uint32_t) override {
        auto latest = App::catalog();
        if (latest != catalog_) {
            catalog_ = latest;
            invalidate();
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g, "故事");

        const int total = count();
        int top = selected_ - (kVisibleRows - 1);
        if (top > total - kVisibleRows) top = total - kVisibleRows;
        if (top < 0) top = 0;

        g.fillRect(8, kListRowY, 310, 172, c.paper);
        for (int row = 0; row < kVisibleRows; ++row) {
            const int index = top + row;
            if (index >= total) break;
            const auto& story = catalog_->stories[index];
            const int y = kListRowY + row * kListRowHeight;
            drawRow(g, y, story, index == selected_);
        }

        g.setFont(Fonts::small());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString("旋钮选择  OK 播放  SET 长按回主页", 160, 224);
    }

private:
    int count() const { return catalog_ ? static_cast<int>(catalog_->stories.size()) : 0; }

    void drawRow(lgfx::LGFXBase& g, int y, const Media::Story& story, bool selected) {
        const auto& c = Theme::colors();
        const uint16_t background = selected ? c.yellow : TFT_WHITE;
        if (selected) g.fillRoundRect(13, y + 3, 300, kListRowHeight, 6, c.ink);
        g.fillRoundRect(10, y, 300, kListRowHeight, 6, selected ? c.ink : TFT_WHITE);
        g.fillRoundRect(12, y + 2, 296, kListRowHeight - 4, 5, background);
        if (!selected) g.drawRoundRect(10, y, 300, kListRowHeight, 6, c.track_border);

        char title[96];
        Theme::fitText(g, story.title.c_str(), 210, title, sizeof(title));
        g.setFont(Fonts::body());
        g.setTextColor(c.text, background);
        g.drawString(title, 24, y + 8);

        g.setFont(Fonts::small());
        g.setTextColor(c.text, background);
        g.drawRightString(kindLabel(story.kind), 296, y + 11);
    }

    void requestScan() {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g, "故事");
        Theme::drawSmileFace(g, 160, 96, 16);
        g.setFont(Fonts::title());
        g.setTextColor(c.text, c.paper);
        g.drawCenterString("正在整理故事", 160, 132);
        g.setFont(Fonts::small());
        g.drawCenterString("请勿拔出 TF 卡", 160, 160);
        Display::present(true);
        const bool ok = refreshCatalog();
        ctx().catalog_ready = ok;
        catalog_ = App::catalog();
        selected_ = 0;
        invalidate();
        if (!ok) notify("扫描失败，请检查 TF 卡");
    }

    int selected_ = 0;
    CatalogPtr catalog_;
};

class StoryPlayerPage final : public Page {
public:
    const char* id() const override { return "story-player"; }

    void setStory(std::size_t index) { index_ = index; }

    void enter() override {
        catalog_ = App::catalog();
        shown_panel_ = -2;
        image_ok_ = true;
        started_ = false;
        last_generation_ = 0;
        timeline_ = Media::Timeline{};
        start();
        invalidate();
    }

    void leave() override {
        if (started_) {
            stopPlayback();
            started_ = false;
        }
    }

    void handle(const Event& event) override {
        if (event.delta) {
            stepVolume(event.delta);
            return;
        }
        switch (event.key) {
            case Key::Ok: {
                const auto snapshot = playback().snapshot();
                pausePlayback(snapshot.state == Audio::PlaybackState::Playing);
                break;
            }
            case Key::Next:
            case Key::Prev: {
                if (!catalog_ || catalog_->stories.empty()) break;
                const int count = static_cast<int>(catalog_->stories.size());
                index_ = static_cast<std::size_t>(wrap(static_cast<int>(index_) + (event.key == Key::Next ? 1 : -1), count));
                shown_panel_ = -2;
                start();
                invalidate();
                break;
            }
            case Key::OkLong:
            case Key::NextLong:
                stopPlayback();
                started_ = false;
                popPage();
                break;
            case Key::SetLong:
                goHome();
                break;
            default:
                break;
        }
    }

    void tick(uint32_t) override {
        if (!started_ || !catalog_ || index_ >= catalog_->stories.size()) return;
        const auto snapshot = playback().snapshot();
        if (snapshot.generation != last_generation_) {
            last_generation_ = snapshot.generation;
            invalidate();
        }
        const uint32_t seconds = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        if (seconds != drawn_second_) invalidate();
        if (snapshot.state == Audio::PlaybackState::Ended && snapshot.generation != ended_generation_ &&
            snapshot.generation > 0) {
            ended_generation_ = snapshot.generation;
            stopPlayback();
            started_ = false;
            popPage();
        }
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        const auto snapshot = playback().snapshot();
        const uint32_t seconds = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        const Media::Story* story = (catalog_ && index_ < catalog_->stories.size())
                                        ? &catalog_->stories[index_]
                                        : nullptr;

        if (!story) {
            g.fillScreen(c.paper);
            drawTopBar(g, "故事");
            g.setFont(Fonts::title());
            g.setTextColor(c.text, c.paper);
            g.drawCenterString("没有故事", 160, 110);
            return;
        }

        const bool illustrated = story->kind == Media::StoryKind::Illustrated;
        int panel = -1;
        if (illustrated && !timeline_.panels().empty()) panel = timeline_.atSeconds(seconds);

        if (illustrated && panel >= 0 && panel != shown_panel_) {
            g.fillScreen(TFT_BLACK);
            image_ok_ = Display::drawStoryImage(storageGate(), Storage::SdVolume::root,
                                                timeline_.panels()[panel].path);
            if (!image_ok_) {
                g.fillScreen(TFT_BLACK);
                g.setFont(Fonts::body());
                g.setTextColor(TFT_WHITE, TFT_BLACK);
                g.drawCenterString("插画读取失败", 160, 110);
            }
            shown_panel_ = panel;
        } else if (!illustrated || panel < 0) {
            if (shown_panel_ != -3) {
                g.fillScreen(c.paper);
                Theme::setTopBarColor(c.coral);
                drawTopBar(g, "故事");
                Theme::drawSmileFace(g, 160, 92, 18);
                char title[96];
                Theme::fitText(g, story->title.c_str(), 280, title, sizeof(title));
                g.setFont(Fonts::title());
                g.setTextColor(c.text, c.paper);
                g.drawCenterString(title, 160, 138);
                shown_panel_ = -3;
            }
        }

        // Control bar
        g.fillRect(0, 202, 320, 38, c.ink);
        g.fillRect(0, 202, 320, 3, c.yellow);
        g.fillRoundRect(6, 207, 26, 26, 8, c.button_border);
        g.fillRoundRect(7, 208, 24, 24, 7, c.button);
        if (snapshot.state == Audio::PlaybackState::Playing) {
            g.fillRect(15, 214, 3, 12, TFT_WHITE);
            g.fillRect(21, 214, 3, 12, TFT_WHITE);
        } else {
            g.fillTriangle(15, 214, 15, 226, 24, 220, TFT_WHITE);
        }

        char title[96];
        Theme::fitText(g, story->title.c_str(), 190, title, sizeof(title));
        g.setFont(Fonts::story());
        g.setTextColor(TFT_WHITE, c.ink);
        g.drawString(title, 38, 207);

        char page[32];
        if (illustrated && !timeline_.panels().empty()) {
            std::snprintf(page, sizeof(page), "%d/%d", panel + 1,
                          static_cast<int>(timeline_.panels().size()));
        } else {
            std::snprintf(page, sizeof(page), "%02u:%02u",
                          static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
        }
        g.setFont(Fonts::small());
        g.setTextColor(c.yellow, c.ink);
        g.drawRightString(page, 312, 209);

        drawn_second_ = seconds;
    }

private:
    void start() {
        if (!catalog_ || index_ >= catalog_->stories.size()) return;
        const auto& story = catalog_->stories[index_];
        timeline_ = Media::Timeline{};
        if (story.kind == Media::StoryKind::Illustrated && !story.timeline.empty()) {
            if (!timeline_.load(storageGate(), Storage::SdVolume::root, story.timeline)) {
                timeline_ = Media::Timeline{};
                notify("时间轴读取失败");
            }
        }
        uint64_t request = 0;
        if (playback().play(story.audio.c_str(), true, request) == ESP_OK) {
            started_ = true;
            ended_generation_ = 0;
            last_generation_ = 0;
        } else {
            started_ = false;
            notify("故事音频加载失败");
        }
    }

    std::size_t index_ = 0;
    CatalogPtr catalog_;
    Media::Timeline timeline_;
    int shown_panel_ = -2;
    bool image_ok_ = true;
    bool started_ = false;
    uint64_t last_generation_ = 0;
    uint64_t ended_generation_ = 0;
    uint32_t drawn_second_ = 0xFFFFFFFF;
};

StoryListPage& storyListInstance() { static StoryListPage instance; return instance; }
StoryPlayerPage& storyPlayerInstance() { static StoryPlayerPage instance; return instance; }

}  // namespace

void openStory(std::size_t index) {
    storyPlayerInstance().setStory(index);
    pushPage(createStoryPlayerPage());
}

Page* createStoryListPage() { return &storyListInstance(); }
Page* createStoryPlayerPage() { return &storyPlayerInstance(); }

}  // namespace App
