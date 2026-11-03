#include "app_context.h"
#include "app_page.h"
#include "app_theme.h"
#include "display.h"
#include "fonts.h"

#include <cstdio>

#include "../../src/home_scenes.h"
#include "../../src/home_video_scene.h"

namespace App {
namespace {

// Story Town home screen: four tickets on the left, one baked scene per theme
// on the right. The third tile uses the code-generated projector scene.
struct HomeTheme {
    const char* title;
    const char* subtitle;
    uint32_t sky;
    uint32_t ink;
};

constexpr HomeTheme kHomeThemes[4] = {
    {"音乐舞台", "听见好心情", 0xf9dfad, 0x694c51},
    {"故事森林", "打开新世界", 0xdcebd5, 0x315c52},
    {"视频剧场", "看动画学知识", 0xf8dfd4, 0x795e67},
    {"工具小屋", "准备好出发", 0xe2e3f3, 0x505878},
};

constexpr const char* kTileTitles[4] = {"音乐", "故事", "视频", "设置"};
constexpr uint32_t kTileFill[4] = {0xf5d67f, 0xa8d7b9, 0xefb5b0, 0xc5c0e8};
constexpr uint32_t kTileInk[4] = {0x886631, 0x386a56, 0x8b5362, 0x655887};

int wrap(int value, int count) {
    if (count <= 0) return 0;
    value %= count;
    return value < 0 ? value + count : value;
}

void drawVolumePill(lgfx::LGFXBase& g, uint16_t top) {
    const auto& c = Theme::colors();
    const uint16_t paper = Theme::rgb(0xfff5df);
    const uint16_t ink = Theme::rgb(0x315c52);
    g.fillRect(230, 0, 66, 30, top);
    g.fillRoundRect(244, 7, 48, 20, 8, paper);
    g.fillRect(250, 14, 3, 6, ink);
    g.fillTriangle(253, 14, 257, 11, 257, 23, ink);
    g.setFont(Fonts::small());
    g.setTextColor(ink, paper);
    char value[16];
    std::snprintf(value, sizeof(value), "%d", ctx().volume);
    g.drawRightString(value, 286, 11);
    (void)c;
}

void drawTicket(lgfx::LGFXBase& g, int index, bool selected) {
    const int x = 8 + index * 78;
    const int y = selected ? 176 : 182;
    const uint16_t fill = Theme::rgb(kTileFill[index]);
    const uint16_t ink = Theme::rgb(kTileInk[index]);
    const uint16_t punch = Theme::rgb(0xfff5df);

    g.fillRoundRect(x, y + 3, 70, 34, 5, ink);
    g.fillRoundRect(x, y, 70, 34, 5, fill);
    g.drawRoundRect(x, y, 70, 34, 5, ink);
    g.fillCircle(x, y + 22, 3, punch);
    g.fillCircle(x + 69, y + 22, 3, punch);
    for (int p = x + 7; p < x + 66; p += 5) g.fillRect(p, y + 26, 2, 1, ink);

    if (index == 0) {  // music note
        g.fillRect(x + 13, y + 7, 2, 11, ink);
        g.drawLine(x + 13, y + 7, x + 19, y + 9, ink);
        g.fillCircle(x + 10, y + 17, 3, ink);
    } else if (index == 1) {  // open book
        g.fillRect(x + 7, y + 8, 6, 11, ink);
        g.fillRect(x + 15, y + 8, 6, 11, ink);
        g.drawLine(x + 10, y + 20, x + 18, y + 20, ink);
    } else if (index == 2) {  // projector
        g.fillRoundRect(x + 5, y + 8, 14, 11, 2, ink);
        g.fillCircle(x + 10, y + 13, 2, fill);
        g.fillTriangle(x + 13, y + 10, x + 13, y + 17, x + 18, y + 13, fill);
        g.fillRect(x + 19, y + 10, 6, 3, ink);
        g.fillRect(x + 19, y + 15, 6, 3, ink);
    } else {  // gear
        g.fillCircle(x + 13, y + 13, 6, ink);
        g.fillRect(x + 11, y + 5, 4, 16, ink);
        g.fillRect(x + 5, y + 11, 16, 4, ink);
        g.fillCircle(x + 13, y + 13, 3, fill);
    }

    g.setFont(Fonts::story());
    g.setTextColor(ink, fill);
    g.drawCenterString(kTileTitles[index], x + 43, y + 6);
    if (selected) {
        g.fillCircle(x + 64, y + 2, 5, ink);
        g.drawLine(x + 61, y + 2, x + 63, y + 4, punch);
        g.drawLine(x + 63, y + 4, x + 67, y, punch);
    }
}

class HomePage final : public Page {
public:
    const char* id() const override { return "home"; }

    void enter() override {
        selected_ = 0;
        drawn_volume_ = -1;
        invalidate();
    }

    void handle(const Event& event) override {
        if (event.delta) {
            selected_ = wrap(selected_ + event.delta, 4);
            return;
        }
        switch (event.key) {
            case Key::Next: selected_ = wrap(selected_ + 1, 4); break;
            case Key::Prev: selected_ = wrap(selected_ - 1, 4); break;
            case Key::Ok:
            case Key::Set:
                activate();
                break;
            default:
                break;
        }
    }

    void tick(uint32_t) override {
        if (ctx().volume != drawn_volume_) invalidate();
    }

    void draw() override {
        auto& g = Display::canvas();
        const auto& c = Theme::colors();
        const HomeTheme& theme = kHomeThemes[selected_];
        const uint16_t paper = Theme::rgb(0xfff5df);
        const uint16_t sky = Theme::rgb(theme.sky);
        const uint16_t ink = Theme::rgb(theme.ink);

        Theme::setTopBarColor(sky);
        g.fillScreen(paper);
        g.fillRect(0, 0, 320, 169, sky);

        // Brand mark
        g.fillCircle(17, 17, 7, ink);
        g.fillRect(14, 15, 1, 2, paper);
        g.fillRect(19, 15, 1, 2, paper);
        g.drawFastHLine(15, 20, 4, paper);
        g.setFont(Fonts::story());
        g.setTextColor(ink, sky);
        g.drawString("小小故事盒", 31, 10);
        drawVolumePill(g, sky);
        Theme::drawHallIcon(g, ctx().hall, sky);
        Theme::drawChildLockBadge(g, ctx().child_locked, sky);

        // Theme index and title
        char number[16];
        std::snprintf(number, sizeof(number), "0%d", selected_ + 1);
        g.setFont(Fonts::small());
        g.setTextColor(ink, sky);
        g.drawString(number, 15, 45);
        g.drawFastHLine(36, 51, 31, ink);
        g.setFont(Fonts::title());
        g.drawString(theme.title, 14, 68);
        g.setFont(Fonts::story());
        g.drawString(theme.subtitle, 14, 93);

        // "按下探索" pill
        const uint16_t yellow = Theme::rgb(0xf6cd68);
        g.fillRoundRect(14, 127, 113, 27, 9, ink);
        g.fillRoundRect(14, 123, 113, 27, 9, yellow);
        g.drawRoundRect(14, 123, 113, 27, 9, ink);
        g.fillCircle(28, 136, 8, paper);
        g.fillTriangle(26, 132, 26, 140, 32, 136, ink);
        g.setTextColor(ink, yellow);
        g.drawString("按下探索", 41, 129);
        g.drawFastHLine(113, 129, 8, paper);
        g.drawFastVLine(117, 125, 8, paper);

        const uint16_t* scene = (selected_ == 2) ? HOME_VIDEO_SCENE : HOME_SCENES[selected_];
        g.pushImage(174, 34, 128, 128, scene);
        g.fillRect(0, 167, 320, 2, ink);

        for (int i = 0; i < 4; ++i) drawTicket(g, i, i == selected_);

        g.setFont(Fonts::small());
        g.setTextColor(Theme::rgb(0x647b79), paper);
        g.drawString("旋转选择", 12, 224);
        g.drawRightString("OK 进入", 308, 224);
        for (int i = 0; i < 4; ++i) {
            g.fillCircle(148 + i * 8, 229, i == selected_ ? 3 : 2,
                         i == selected_ ? ink : Theme::rgb(0xd2cbb7));
        }
        (void)c;

        drawn_volume_ = ctx().volume;
    }

private:
    void activate() {
        switch (selected_) {
            case 0: pushPage(createMusicListPage()); break;
            case 1: pushPage(createStoryListPage()); break;
            case 2: notify("视频播放：P3 接入"); break;
            default: openSettings(); break;
        }
    }

    int selected_ = 0;
    int drawn_volume_ = -1;
};

HomePage& homeInstance() {
    static HomePage instance;
    return instance;
}

}  // namespace

Page* createHomePage() { return &homeInstance(); }

}  // namespace App
