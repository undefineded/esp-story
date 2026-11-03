#pragma once
#include <cstddef>
#include <cstdint>

#include "media_records.h"
#include "settings_record.h"

namespace App {

// Application-lifetime state shared by pages. Only the UI task mutates it;
// pages read it directly because draw() and handle() run on that same task.
struct Context {
    Persistence::Settings settings{};
    int volume = 4;          // RAM mirror; FRAM record is authoritative when ready
    bool fram_confirmed = false;
    bool fram_ready = false;
    bool sd_mounted = false;
    bool catalog_ready = false;
    bool hall = false;       // magnetic card detected
    bool child_locked = false;
};

Context& ctx();

// Volume helpers keep the RAM mirror, FRAM record and audio service in step.
void setVolume(int value);
void stepVolume(int steps);

// Apply an equalizer preset (0..3) to the running playback service.
void applyEq(uint8_t preset);

// Favorites are indexed by position in the full catalog track list (FAV v1
// bitmap layout). Returns false when the FRAM record is unavailable.
bool favorite(std::size_t index);
bool setFavorite(std::size_t index, bool enabled);

// Persist settings (no-op when FRAM is unavailable). Returns false on a failed
// verified write so the caller can surface "保存失败".
bool saveSettings();

// Transient message shown at the bottom of the current page.
void notify(const char* message, uint32_t duration_ms = 2500);
bool noticeActive();
const char* noticeText();

// Video playback is "content": the power policy keeps the panel on for it.
bool videoActive();
void setVideoActive(bool active);

// Settings cycling helpers shared by the settings page.
uint8_t cycleMinutes(uint8_t current, int direction);
const char* minutesLabel(uint8_t minutes);

}
