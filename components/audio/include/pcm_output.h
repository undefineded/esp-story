#pragma once
#include "driver/i2s_std.h"
#include <cstddef>
#include <cstdint>
namespace Audio {
// Audio-owner-task API: interleaved signed 16-bit stereo PCM.
// Decoder never owns I2S. No amplifier enable until board polarity is verified.
class PcmOutput {
public:
    PcmOutput() = default;
    PcmOutput(const PcmOutput&) = delete;
    PcmOutput& operator=(const PcmOutput&) = delete;
    ~PcmOutput();
    esp_err_t begin(uint32_t sample_rate);
    esp_err_t write(const int16_t* samples, std::size_t frames,
                    std::size_t& bytes_written, uint32_t timeout_ms);
    esp_err_t stop();
private:
    i2s_chan_handle_t channel_ = nullptr;
    bool enabled_ = false;
};
}
