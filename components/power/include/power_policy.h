#pragma once
#include <cstdint>

namespace Power {

struct Config {
    uint16_t screen_off_min = 0;  // 0 disables
    uint16_t sleep_min = 0;       // 0 disables
    uint8_t brightness = 100;     // restored when the panel wakes up
};

struct Inputs {
    bool audio_playing = false;
    bool video_playing = false;
    bool host_active = false;     // USB transfer or BLE connection in progress
    uint32_t idle_ms = 0;
};

struct Decision {
    bool screen_off = false;
    bool deep_sleep = false;
};

// Pure idle policy shared by the device and host tests:
// - A host transfer or BLE link keeps the device fully awake.
// - Video playback is content: neither the panel nor the CPU idles out.
// - Playing audio may switch the panel off, never the device.
// - Deep sleep needs silence and both windows elapsed.
inline Decision evaluate(const Inputs& inputs, const Config& config) {
    Decision decision;
    if (inputs.host_active || inputs.video_playing) return decision;
    const uint64_t screen_ms = uint64_t{config.screen_off_min} * 60000u;
    const uint64_t sleep_ms = uint64_t{config.sleep_min} * 60000u;
    if (screen_ms && inputs.idle_ms >= screen_ms) decision.screen_off = true;
    if (!inputs.audio_playing && sleep_ms && inputs.idle_ms >= sleep_ms) decision.deep_sleep = true;
    return decision;
}

}
