#pragma once
#include "storage_access.h"
#include "esp_audio_simple_dec.h"
#include "esp_err.h"
#include <array>

namespace Audio {
// Process-global decoder registration shared by music and esp_player video.
esp_err_t ensureMp3Decoder();
enum class DecodeStep { Pcm, NeedInput, End, Error };
// Called only by the audio owner task. No I2S or control thread inside decoder.
class Mp3Stream {
public:
    explicit Mp3Stream(Storage::AccessGate& gate) : file_(gate) {}
    ~Mp3Stream() { close(); }
    Mp3Stream(const Mp3Stream&) = delete;
    Mp3Stream& operator=(const Mp3Stream&) = delete;
    esp_err_t open(const std::string& root, const std::string& path, uint32_t start_offset = 0);
    // Reopens the decoder at a byte offset inside the already-opened file. MP3
    // frames are independent, so resuming a track needs no accurate seek table:
    // the parser resyncs on the next frame header. Used by the per-folder resume
    // record, which stores the offset captured while playing.
    bool seekTo(uint32_t byte_offset);
    uint32_t byteOffset() const { return start_offset_ + consumed_; }
    bool close();
    DecodeStep next();
    const int16_t* samples() const { return pcm_.data(); }
    int16_t* samples() { return pcm_.data(); }
    std::size_t frames() const { return frames_; }
    uint32_t sampleRate() const { return rate_; }
    uint32_t bitrate() const { return bitrate_; }
    // Whole-file duration in seconds: Xing/Info frame count when the file has
    // one, otherwise a CBR estimate from the file size and the first frame's
    // bitrate. The Arduino UI showed a real duration and the progress bar needs it.
    uint32_t duration() const { return duration_; }
    esp_err_t error() const { return error_; }
private:
    DecodeStep fail(esp_err_t error) { error_ = error; return DecodeStep::Error; }
    void parseDuration();
    Storage::FileStream file_;
    esp_audio_simple_dec_handle_t decoder_ = nullptr;
    std::array<uint8_t, 4096> input_{};
    // MPEG1 Layer III: at most 1152 samples/channel, stereo signed 16-bit.
    std::array<int16_t, 2304> pcm_{};
    std::size_t remaining_ = 0, position_ = 0, frames_ = 0;
    uint32_t rate_ = 0, bitrate_ = 0, duration_ = 0, file_size_ = 0;
    uint32_t start_offset_ = 0, consumed_ = 0;
    esp_err_t error_ = ESP_OK;
    bool eof_ = false, produced_ = false;
};
}
