#pragma once
#include "driver/i2s_std.h"
#include <cstddef>
#include <cstdint>
namespace Audio {
// Audio-owner-task API: interleaved signed 16-bit stereo PCM.
// Decoder never owns I2S. No amplifier enable until board polarity is verified.
class PcmOutput {
public:
    // Writes are split into one DMA descriptor each, so a caller may hand over any
    // chunk size without forcing a wait for the whole chunk to drain. The ring then
    // only sets latency and slack:
    //   music: 8 x 480 = 87 ms, covers the page-redraw burst when a track starts;
    //   video: 12 x 480 = 130 ms, because esp_player's render element hands over
    //          100 ms of PCM per call (ESP_PLAYER_AUDIO_RB_TIME_MS >> 2); a ring
    //          smaller than that chunk makes every write block for a full chunk
    //          and the 8-9 ms gap between chunks becomes a permanent underrun.
    static constexpr uint32_t kMusicCapacity = 8 * 480;
    static constexpr uint32_t kVideoCapacity = 12 * 480;
    static constexpr uint32_t kChunkFrames = 480;
    PcmOutput() = default;
    PcmOutput(const PcmOutput&) = delete;
    PcmOutput& operator=(const PcmOutput&) = delete;
    ~PcmOutput();
    esp_err_t begin(uint32_t sample_rate, uint32_t total_frames = kMusicCapacity);
    uint32_t capacity() const { return capacity_; }
    esp_err_t write(const int16_t* samples, std::size_t frames,
                    std::size_t& bytes_written, uint32_t timeout_ms);
    esp_err_t stop();
private:
    i2s_chan_handle_t channel_ = nullptr;
    uint32_t capacity_ = 0;
    bool enabled_ = false;
};
}
