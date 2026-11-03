#pragma once
#include <cstdint>
#include <string>

#include "LovyanGFX.hpp"

namespace App {

// Values the shared player chrome needs. Both the music player and an audio
// story present the same layout the Arduino firmware used: cover card, category
// badge, count, marquee title, codec/bitrate, times, progress, status and the
// ink transport bar.
struct PlayerViewData {
    const char* title = "";
    const char* category = "音乐";
    const char* cover_label = "音乐播放";
    bool has_track = false;
    bool playing = false;
    bool paused = false;
    uint32_t seconds = 0;
    uint32_t duration = 0;
    uint32_t bitrate = 0;
    int index = 0;
    int count = 0;
    int volume = 0;
};

class PlayerView {
public:
    // Starts a new marquee cycle for the current title.
    void reset(uint32_t now);
    // Advances the title marquee; dirty() reports whether a redraw is needed.
    void tick(const PlayerViewData& data, uint32_t now);
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    void draw(lgfx::LGFXBase& g, const PlayerViewData& data);

private:
    void drawTopBar(lgfx::LGFXBase& g, int volume);
    void drawCover(lgfx::LGFXBase& g, const char* label);
    void drawCategory(lgfx::LGFXBase& g, const char* label);
    void drawCount(lgfx::LGFXBase& g, int index, int count);
    void drawTitle(lgfx::LGFXBase& g, const char* title);
    void drawStatus(lgfx::LGFXBase& g, const PlayerViewData& data);
    void drawTransport(lgfx::LGFXBase& g);
    void drawPlayButton(lgfx::LGFXBase& g, bool playing);

    std::string title_;
    int title_width_ = 0;
    int scroll_ = 0;
    uint32_t title_epoch_ = 0;
    bool dirty_ = true;
};

}  // namespace App
