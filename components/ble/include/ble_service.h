#pragma once
#include <cstddef>
#include <cstdint>

#include "cJSON.h"
#include "parental_config.h"

namespace Ble {

enum class Action : uint8_t {
    None = 0,
    Play,
    Pause,
    Toggle,
    Next,
    Prev,
    Stop,
    Volume,
    PlayStory,
    PlayMusic,
    Favourite,
};

struct Command {
    Action action = Action::None;
    int32_t value = 0;
};

// Playback snapshot the app publishes once per second; the service merges it
// with the parental status it reads through Hooks::status.
struct Snapshot {
    bool playing = false;
    bool paused = false;
    int volume = 0;
    const char* mode = "idle";  // "music" | "story" | "idle"
    const char* title = "";
    uint32_t current = 0;       // seconds
    uint32_t duration = 0;      // seconds, 0 when unknown
};

// Hooks into the application. The BLE layer owns the wire protocol; the app owns
// the PCTL record, playback and persistence.
//
// The parental callbacks return nullptr on success or a protocol error code
// ("SAVE", "UNAVAILABLE") that the phone receives as {"ok":false,"err":...}.
struct Hooks {
    void* context = nullptr;
    bool (*bind)(void* context, const char* code, char* token, std::size_t capacity) = nullptr;
    bool (*verify_token)(void* context, const char* token) = nullptr;
    bool (*verify_pin)(void* context, const char* pin) = nullptr;
    void (*sync_time)(void* context, uint32_t epoch, int16_t timezone) = nullptr;
    const char* (*bind_code)(void* context) = nullptr;
    // Reads the parental status; false means "no usable record", which is
    // reported as unavailable rather than permissive.
    bool (*status)(void* context, Persistence::ParentalStatus* out) = nullptr;
    const char* (*apply_config)(void* context, const Persistence::ParentalPatch& patch) = nullptr;
    const char* (*set_pin)(void* context, const char* pin) = nullptr;
    const char* (*factory_reset)(void* context) = nullptr;
    const char* (*unbind)(void* context) = nullptr;
    // Playback admission control. Returns nullptr when the action may run, or a
    // protocol error code ("LOCKED", "NOT_ALLOWED", "RANGE") to reject it.
    const char* (*gate)(void* context, Action action, int32_t value) = nullptr;
    // "get stories|music|cats|report": fills `data` with one page. Returns false
    // when the list kind is unknown so the service can answer BAD_WHAT. The app
    // owns the catalog and the paging limits.
    bool (*list)(void* context, const char* what, const cJSON* request, cJSON* data) = nullptr;
    // "ctrl fav": toggles the favourite flag of one track and reports the new
    // state plus the total. Returns false when favourites are unavailable.
    bool (*toggle_favorite)(void* context, int32_t index, bool* now, int32_t* total) = nullptr;
};

// GATT service (RX writes JSON lines, TX notifies JSON lines) plus the protocol
// dispatcher. begin() starts the NimBLE host; loop() must run on the app task.
void begin(const char* device_name, Hooks hooks);
void loop();
void setAdvertisingEnabled(bool enabled);
bool advertisingEnabled();
bool connected();
void setSnapshot(const Snapshot& snapshot);
bool takeCommand(Command& out);
const char* deviceName();

}
