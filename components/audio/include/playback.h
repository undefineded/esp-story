#pragma once
#include "mp3_stream.h"
#include "pcm_output.h"
#include "equalizer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <mutex>
#include <atomic>

namespace Audio {
enum class PlaybackState { Stopped, Paused, Playing, Draining, Ended, Failed };
struct PlaybackSnapshot {
    PlaybackState state = PlaybackState::Stopped;
    uint64_t request = 0, generation = 0, submitted_frames = 0;
    uint32_t sample_rate = 0, bitrate = 0;
    esp_err_t error = ESP_OK;
    char path[512]{};
};
// Application-lifetime service. begin once; all decoder/I2S work runs in its task.
class Playback {
public:
    explicit Playback(Storage::AccessGate& gate) : stream_(gate) {}
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;
    esp_err_t begin();
    esp_err_t play(const char* path, bool auto_start, uint64_t& request);
    esp_err_t pause(bool paused, uint64_t& request);
    // Closes admission before submitting stop. Call allowPlayback after maintenance.
    esp_err_t stopAndWait(uint32_t timeout_ms);
    void allowPlayback() { inhibited_ = false; }
    void setVolume(unsigned volume) { volume_ = volume > 21 ? 21 : volume; }
    void setEq(EqPreset preset) { eq_ = preset; }
    PlaybackSnapshot snapshot();
private:
    enum class Action { Play, Pause, Resume, Stop };
    struct Command { Action action; uint64_t id; bool auto_start; char path[512]; };
    esp_err_t submit(Command& command, uint64_t& id);
    void run();
    void execute(const Command& command);
    void step();
    void publish();
    void finish(PlaybackState state, esp_err_t error);
    QueueHandle_t queue_ = nullptr;
    std::mutex submit_mutex_, status_mutex_;
    uint64_t next_request_ = 0;
    std::atomic<bool> inhibited_{false};
    std::atomic<unsigned> volume_{4};
    std::atomic<EqPreset> eq_{EqPreset::Vocal};
    Equalizer equalizer_;
    PlaybackSnapshot current_, published_;
    Mp3Stream stream_;
    PcmOutput output_;
    std::size_t pending_frames_ = 0, pending_offset_ = 0;
    int64_t drain_until_ = 0;
    int64_t last_output_progress_ = 0;
};
}
