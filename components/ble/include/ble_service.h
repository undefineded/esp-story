#pragma once
#include <cstddef>
#include <cstdint>

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
};

struct Command {
    Action action = Action::None;
    int32_t value = 0;
};

// Snapshot the app publishes once per second; the service turns it into the
// "state" notification the mini-program expects.
struct Snapshot {
    bool playing = false;
    bool paused = false;
    int volume = 0;
    int volume_min = 0;
    int volume_max = 21;
    const char* mode = "idle";  // "music" | "story" | "idle"
    const char* title = "";
    uint32_t current = 0;       // seconds
    uint32_t duration = 0;      // seconds, 0 when unknown
    uint32_t used = 0;          // parental usage today, seconds
    const char* lock = "";      // "", "daily", "bedtime", "remote"
};

// Binding/auth hooks: the app owns the PCTL record, the BLE layer only asks.
struct Hooks {
    void* context = nullptr;
    bool (*bind)(void* context, const char* code, char* token, std::size_t capacity) = nullptr;
    bool (*verify_token)(void* context, const char* token) = nullptr;
    bool (*verify_pin)(void* context, const char* pin) = nullptr;
    void (*sync_time)(void* context, uint32_t epoch, int16_t timezone) = nullptr;
    bool (*has_pin)(void* context) = nullptr;
    bool (*bound)(void* context) = nullptr;
    const char* (*bind_code)(void* context) = nullptr;
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
