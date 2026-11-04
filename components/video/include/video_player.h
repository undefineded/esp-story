#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "playback.h"
#include "storage_access.h"

namespace Video {
enum class State { Idle, Preparing, Playing, Paused, Finished, Failed };
struct Status {
    State state = State::Idle;
    uint64_t position_ms = 0, duration_ms = 0;
    uint32_t frames = 0, replaced_frames = 0;
    uint32_t stop_audio_ms = 0, setup_ms = 0, run_ms = 0, first_frame_ms = 0;
    uint32_t queue_ms = 0, close_ms = 0;
    uint32_t control_stack_min_bytes = 0;
    uint32_t backend_copy_us = 0;
    char error[96]{};
};
// Nonblocking control requests. A single worker owns every official player API.
bool open(Storage::AccessGate& gate, Audio::Playback& audio, const std::string& path);
// Returns request acceptance, not teardown completion. busy() stays true until
// ownership is released; teardown errors are reported in status().
bool close(bool drain_audio = false);
bool active();
bool busy(); // Includes queued teardown / audio handoff, not just video playback.
esp_err_t playAudio(Audio::Playback& audio, const char* path, bool start, uint64_t& request);
bool pause(bool paused);
void setVolume(unsigned volume);
void setEq(Audio::EqPreset preset);
Status status();
bool framePending();
// RGB565 native-endian pixels. One bounded latest-frame mailbox, not a backlog.
bool consumeFrame(uint16_t* pixels, std::size_t count);
using FrameVisitor = void (*)(const uint16_t*, void*);
bool visitFrame(FrameVisitor visitor, void* context);
const char* error();
}
