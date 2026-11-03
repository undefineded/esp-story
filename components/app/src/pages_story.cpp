#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "display.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "fonts.h"
#include "player_view.h"
#include "sd_volume.h"
#include "story_image.h"
#include "story_timeline.h"

namespace App {
namespace {

constexpr int kVisibleRows = 4;
constexpr int kListRowY = 36;
constexpr int kListRowHeight = 42;

bool s_story_page_active = false;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

int wrap(int value, int count) {
    if (count <= 0) return 0;
    value %= count;
    return value < 0 ? value + count : value;
}

// The Arduino story pages drew the brand bar only; the path lived in the
// bottom breadcrumb, so that is what the list below does too.
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

void iconBook(lgfx::LGFXBase& g, int y, uint16_t color) {
    const auto& c = Theme::colors();
    g.fillRoundRect(22, y + 8, 20, 22, 3, color);
    g.drawRoundRect(22, y + 8, 20, 22, 3, c.ink);
    g.fillRect(25, y + 8, 3, 22, c.ink);
}

void drawRowCard(lgfx::LGFXBase& g, int y, bool selected) {
    const auto& c = Theme::colors();
    if (selected) g.fillRoundRect(13, y + 3, 300, 38, 6, c.ink);
    g.fillRoundRect(10, y, 300, 38, 6, selected ? c.ink : TFT_WHITE);
    g.fillRoundRect(12, y + 2, 296, 34, 5, selected ? c.yellow : TFT_WHITE);
    if (!selected) g.drawRoundRect(10, y, 300, 38, 6, c.track_border);
}

// "故事 / 分类 / 子目录"
std::string crumb(const std::string& path) {
    if (path == "/story" || path.empty()) return "故事";
    std::string value = path;
    if (value.compare(0, 7, "/story/") == 0) value = value.substr(7);
    std::string result;
    std::size_t at = 0;
    while (at < value.size()) {
        const auto slash = value.find('/', at);
        const auto end = slash == std::string::npos ? value.size() : slash;
        if (!result.empty()) result += " / ";
        result += value.substr(at, end - at);
        at = end + 1;
    }
    return result.empty() ? "故事" : result;
}

// One row of the story browser: either a directory to descend into or a story
// to play. Directory rows are produced on the fly from the flat catalog so the
// index format does not need to store the tree.
struct BrowseEntry {
    std::string label;
    std::string path;
    bool directory = false;
    std::size_t story = 0;
};

std::vector<BrowseEntry> entriesAt(const Media::Catalog& catalog, const std::string& base) {
    const std::string prefix = base + "/";
    std::vector<BrowseEntry> directories, stories;
    std::vector<std::string> seen;
    for (std::size_t i = 0; i < catalog.stories.size(); ++i) {
        const auto& story = catalog.stories[i];
        if (story.directory.compare(0, prefix.size(), prefix) != 0) continue;
        const auto rest = story.directory.substr(prefix.size());
        const auto slash = rest.find('/');
        if (slash == std::string::npos) {
            // The story lives directly in base (illustrated folder or an audio
            // story whose MP3 sits in base).
            stories.push_back({story.title, story.directory, false, i});
        } else {
            const auto child = rest.substr(0, slash);
            if (std::find(seen.begin(), seen.end(), child) == seen.end()) {
                seen.push_back(child);
                directories.push_back({child, prefix + child, true, 0});
            }
        }
    }
    std::sort(directories.begin(), directories.end(),
              [](const auto& a, const auto& b) { return a.label < b.label; });
    std::sort(stories.begin(), stories.end(),
              [](const auto& a, const auto& b) { return a.label < b.label; });
    directories.insert(directories.end(), stories.begin(), stories.end());
    return directories;
}

class StoryListPage final : public Page {
public:
    const char* id() const override { return "story-list"; }

    void enter() override {
        path_ = "/story";
        selected_ = 0;
        if (!catalog_) catalog_ = App::catalog();
        invalidate();
        if (!ctx().catalog_ready) requestScan();
        reload();
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
                activate();
                break;
            case Key::OkLong:
            case Key::OkDouble:
                ascend();
                break;
            case Key::SetLong:
                goHome();
                break;
            default: break;
        }
    }

    void tick(uint32_t) override {
        auto latest = App::catalog();
        if (latest != catalog_) {
            catalog_ = latest;
            reload();
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
            drawRow(g, kListRowY + row * kListRowHeight, entries_[index], index == selected_);
        }

        g.setFont(Fonts::small());
        g.setTextColor(c.text, c.paper);
        std::string hint = total ? crumb(path_) + " · OK进入/播放 · 长按OK返回"
                                 : "这个文件夹没有故事";
        char line[192];
        Theme::fitText(g, hint.c_str(), 304, line, sizeof(line));
        g.drawCenterString(line, 160, 214);
    }

private:
    static int listTop(int selected, int count) {
        int top = selected >= kVisibleRows ? selected - kVisibleRows + 1 : 0;
        if (top > count - kVisibleRows) top = count - kVisibleRows;
        return top < 0 ? 0 : top;
    }

    int descendants(const std::string& path) const {
        if (!catalog_) return 0;
        const std::string prefix = path + "/";
        int count = 0;
        for (const auto& story : catalog_->stories) {
            if (story.directory.compare(0, prefix.size(), prefix) == 0) ++count;
        }
        return count;
    }

    void reload() {
        entries_.clear();
        if (!catalog_) return;
        entries_ = entriesAt(*catalog_, path_);
        if (selected_ >= static_cast<int>(entries_.size())) selected_ = 0;
        invalidate();
    }

    void ascend() {
        if (path_ == "/story") {
            goHome();
            return;
        }
        const auto slash = path_.find_last_of('/');
        path_ = slash <= 5 ? std::string("/story") : path_.substr(0, slash);
        selected_ = 0;
        reload();
    }

    void activate() {
        if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return;
        const auto& entry = entries_[selected_];
        if (entry.directory) {
            path_ = entry.path;
            selected_ = 0;
            reload();
        } else {
            openStory(entry.story);
        }
    }

    void drawRow(lgfx::LGFXBase& g, int y, const BrowseEntry& entry, bool selected) {
        const auto& c = Theme::colors();
        const uint16_t background = selected ? c.yellow : TFT_WHITE;
        drawRowCard(g, y, selected);

        const bool playing = !entry.directory && currentStory() == static_cast<int>(entry.story);
        const Media::StoryKind kind =
            (!entry.directory && catalog_ && entry.story < catalog_->stories.size())
                ? catalog_->stories[entry.story].kind
                : Media::StoryKind::Audio;

        if (entry.directory) {
            iconFolder(g, 24, y + 10, selected ? c.coral : c.aqua);
        } else if (kind == Media::StoryKind::Audio) {
            iconNote(g, 24, y + 8, playing ? c.coral : (selected ? c.coral : c.aqua));
        } else {
            iconBook(g, y, playing ? c.coral : (selected ? c.coral : c.aqua));
        }

        char title[96];
        Theme::fitText(g, entry.label.c_str(), 178, title, sizeof(title));
        g.setFont(Fonts::title());
        g.setTextColor(playing ? c.coral : c.text, background);
        g.drawString(title, 52, y + 9);

        g.setFont(Fonts::small());
        if (entry.directory) {
            char suffix[20];
            std::snprintf(suffix, sizeof(suffix), "%d个", descendants(entry.path));
            g.setTextColor(c.text, background);
            g.drawRightString(suffix, 296, y + 14);
        } else if (playing) {
            g.setTextColor(c.coral, background);
            g.drawRightString("播放中", 296, y + 14);
        }
    }

    void requestScan() {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        g.fillScreen(c.paper);
        drawTopBar(g);
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
        reload();
        if (!ok) notify("扫描失败，请检查 TF 卡");
    }

    int selected_ = 0;
    std::string path_ = "/story";
    std::vector<BrowseEntry> entries_;
    CatalogPtr catalog_;
};

class StoryPlayerPage final : public Page {
public:
    const char* id() const override { return "story-player"; }

    void setStory(std::size_t index) { index_ = index; }

    void enter() override {
        s_story_page_active = true;
        catalog_ = App::catalog();
        shown_panel_ = -2;
        image_ok_ = true;
        started_ = false;
        last_generation_ = 0;
        timeline_ = Media::Timeline{};
        view_.reset(nowMs());
        start();
        invalidate();
    }

    void leave() override {
        s_story_page_active = false;
        if (started_) {
            stopPlayback();
            started_ = false;
        }
        setCurrentStory(-1);
        setNowPlaying("idle", "");
    }

    void handle(const Event& event) override {
        if (event.delta) {
            stepVolume(event.delta);
            return;
        }
        switch (event.key) {
            case Key::Ok:
            case Key::Set: {
                const auto snapshot = playback().snapshot();
                pausePlayback(snapshot.state == Audio::PlaybackState::Playing);
                break;
            }
            case Key::Next:
            case Key::Prev: {
                if (!catalog_ || catalog_->stories.empty()) break;
                const int count = static_cast<int>(catalog_->stories.size());
                index_ = static_cast<std::size_t>(
                    wrap(static_cast<int>(index_) + (event.key == Key::Next ? 1 : -1), count));
                shown_panel_ = -2;
                start();
                invalidate();
                break;
            }
            case Key::OkLong:
            case Key::NextLong:
            case Key::PrevLong:
                stopPlayback();
                started_ = false;
                setCurrentStory(-1);
                popPage();
                break;
            case Key::SetLong:
                goHome();
                break;
            default: break;
        }
    }

    void tick(uint32_t now) override {
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
            setCurrentStory(-1);
            popPage();
            return;
        }
        if (catalog_->stories[index_].kind == Media::StoryKind::Audio) {
            view_.tick(audioModel(), now);
            if (view_.dirty()) {
                view_.clearDirty();
                invalidate();
            }
        }
        drawn_second_ = seconds;
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        const Media::Story* story = (catalog_ && index_ < catalog_->stories.size())
                                        ? &catalog_->stories[index_]
                                        : nullptr;

        if (!story) {
            g.fillScreen(c.paper);
            drawTopBar(g);
            g.setFont(Fonts::title());
            g.setTextColor(c.text, c.paper);
            g.drawCenterString("没有故事", 160, 110);
            return;
        }

        if (story->kind == Media::StoryKind::Audio) {
            // The Arduino audio-story page reused the full music player layout;
            // only the cover label and category badge changed.
            view_.draw(g, audioModel());
            return;
        }

        const auto snapshot = playback().snapshot();
        const uint32_t seconds = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        int panel = -1;
        if (!timeline_.panels().empty()) panel = timeline_.atSeconds(seconds);

        if (panel >= 0 && panel != shown_panel_) {
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
        } else if (panel < 0) {
            if (shown_panel_ != -3) {
                g.fillScreen(c.paper);
                Theme::setTopBarColor(c.coral);
                drawTopBar(g);
                Theme::drawSmileFace(g, 160, 92, 18);
                char title[96];
                Theme::fitText(g, story->title.c_str(), 280, title, sizeof(title));
                g.setFont(Fonts::title());
                g.setTextColor(c.text, c.paper);
                g.drawCenterString(title, 160, 138);
                shown_panel_ = -3;
            }
        }

        // Control bar: same geometry as the Arduino story bar.
        g.fillRect(0, 202, 320, 38, c.ink);
        g.fillRect(0, 202, 320, 3, c.yellow);
        g.fillRoundRect(6, 207, 26, 26, 8, c.button_border);

        const bool playing = snapshot.state == Audio::PlaybackState::Playing;
        g.fillRoundRect(8, 209, 22, 22, 6, c.yellow);
        if (playing) {
            g.fillRect(15, 214, 3, 12, c.ink);
            g.fillRect(21, 214, 3, 12, c.ink);
        } else {
            g.fillTriangle(15, 214, 15, 226, 24, 220, c.ink);
        }

        char title[96];
        Theme::fitText(g, story->title.c_str(), 190, title, sizeof(title));
        g.setFont(Fonts::story());
        g.setTextColor(TFT_WHITE, c.ink);
        g.drawString(title, 38, 207);

        char page[32];
        if (!timeline_.panels().empty()) {
            std::snprintf(page, sizeof(page), "%02d/%02d", panel + 1,
                          static_cast<int>(timeline_.panels().size()));
        } else {
            std::snprintf(page, sizeof(page), "%02u:%02u",
                          static_cast<unsigned>(seconds / 60), static_cast<unsigned>(seconds % 60));
        }
        g.setFont(Fonts::small());
        g.setTextColor(c.yellow, c.ink);
        g.drawRightString(page, 312, 208);

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
            drawn_second_ = 0xFFFFFFFF;
            setCurrentStory(static_cast<int>(index_));
            setNowPlaying("story", story.title.c_str());
        } else {
            started_ = false;
            setCurrentStory(-1);
            setNowPlaying("idle", "");
            notify("故事音频加载失败");
        }
    }

    PlayerViewData audioModel() const {
        const auto snapshot = playback().snapshot();
        const Media::Story* story = (catalog_ && index_ < catalog_->stories.size())
                                        ? &catalog_->stories[index_]
                                        : nullptr;
        PlayerViewData model{};
        model.title = story ? story->title.c_str() : "";
        model.category = (story && !story->category.empty()) ? story->category.c_str() : "音频故事";
        model.cover_label = "音频故事";
        model.has_track = story != nullptr;
        model.playing = snapshot.state == Audio::PlaybackState::Playing ||
                        snapshot.state == Audio::PlaybackState::Draining;
        model.paused = snapshot.state == Audio::PlaybackState::Paused;
        model.seconds = snapshot.sample_rate ? snapshot.submitted_frames / snapshot.sample_rate : 0;
        model.duration = snapshot.duration;
        model.bitrate = snapshot.bitrate;
        model.index = static_cast<int>(index_);
        model.count = catalog_ ? static_cast<int>(catalog_->stories.size()) : 0;
        model.volume = ctx().volume;
        return model;
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
    PlayerView view_;
};

StoryListPage& storyListInstance() { static StoryListPage instance; return instance; }
StoryPlayerPage& storyPlayerInstance() { static StoryPlayerPage instance; return instance; }

}  // namespace

void openStory(std::size_t index) {
    auto& page = storyPlayerInstance();
    page.setStory(index);
    // BLE "ctrl story" can arrive while the story page is already open. Pushing
    // the singleton again would put the same object on the stack twice, so
    // restart the open page in place instead.
    if (s_story_page_active) {
        page.enter();
        return;
    }
    pushPage(createStoryPlayerPage());
}

Page* createStoryListPage() { return &storyListInstance(); }
Page* createStoryPlayerPage() { return &storyPlayerInstance(); }

}  // namespace App
