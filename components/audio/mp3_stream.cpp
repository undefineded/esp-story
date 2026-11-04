#include "mp3_stream.h"
#include "esp_mp3_dec.h"

#include <cstring>

namespace Audio {
namespace {
// MPEG audio sample-rate table indexed by [version][rate_index]. Version bits:
// 3 = MPEG1, 2 = MPEG2, 0 = MPEG2.5 (1 is reserved).
constexpr uint32_t kSampleRates[3][3] = {
    {44100, 48000, 32000},
    {22050, 24000, 16000},
    {11025, 12000, 8000},
};
}

esp_err_t ensureMp3Decoder() {
    static const auto registered = esp_mp3_dec_register();
    return registered == ESP_AUDIO_ERR_OK ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t Mp3Stream::open(const std::string& root, const std::string& path) {
    if (!close()) return error_ = ESP_FAIL;
    if (ensureMp3Decoder() != ESP_OK) return error_ = ESP_ERR_NO_MEM;
    if (!file_.open(root, path, "rb")) return error_ = ESP_FAIL;
    esp_audio_simple_dec_cfg_t config{};
    config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    const auto result = esp_audio_simple_dec_open(&config, &decoder_);
    if (result != ESP_AUDIO_ERR_OK) {
        file_.close();
        return error_ = result == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    remaining_ = position_ = frames_ = 0;
    rate_ = bitrate_ = duration_ = 0;
    eof_ = produced_ = false;

    // Remember the file size for the CBR fallback, then let the optional
    // Xing/Info header provide an exact frame count.
    file_size_ = 0;
    if (file_.seek(0, SEEK_END)) {
        const long end = file_.tell();
        if (end > 0) file_size_ = static_cast<uint32_t>(end);
    }
    parseDuration();
    file_.seek(0, SEEK_SET);
    remaining_ = position_ = 0;
    return error_ = ESP_OK;
}
bool Mp3Stream::close() {
    if (decoder_) esp_audio_simple_dec_close(decoder_);
    decoder_ = nullptr;
    return file_.close();
}
void Mp3Stream::parseDuration() {
    if (!file_size_ || !file_.seek(0, SEEK_SET)) return;

    // Skip an ID3v2 tag (header 10 bytes, syncsafe size) before looking for the
    // first MPEG frame. Files without a tag simply start at offset 0.
    uint8_t header[10] = {};
    long offset = 0;
    if (file_.read(header, sizeof(header)) == sizeof(header) && std::memcmp(header, "ID3", 3) == 0) {
        const uint32_t tag = (uint32_t(header[6] & 0x7F) << 21) |
                             (uint32_t(header[7] & 0x7F) << 14) |
                             (uint32_t(header[8] & 0x7F) << 7) |
                             uint32_t(header[9] & 0x7F);
        offset = static_cast<long>(10 + tag);
    }
    if (!file_.seek(offset)) return;

    uint8_t frame[512] = {};
    const std::size_t available = file_.read(frame, sizeof(frame));
    for (std::size_t i = 0; i + 4 <= available; ++i) {
        if (frame[i] != 0xFF || (frame[i + 1] & 0xE0) != 0xE0) continue;
        const int version = (frame[i + 1] >> 3) & 0x03;
        const int layer = (frame[i + 1] >> 1) & 0x03;
        if (version == 1 || layer != 1) continue;  // only Layer III is supported
        const int rate_index = (frame[i + 2] >> 2) & 0x03;
        if (rate_index == 3) continue;
        const int row = version == 3 ? 0 : (version == 2 ? 1 : 2);
        const uint32_t rate = kSampleRates[row][rate_index];
        const int mode = (frame[i + 3] >> 6) & 0x03;
        const std::size_t side = version == 3 ? (mode == 3 ? 17 : 32) : (mode == 3 ? 9 : 17);
        const std::size_t xing = i + 4 + side;
        if (xing + 12 > available) continue;
        if (std::memcmp(frame + xing, "Xing", 4) != 0 && std::memcmp(frame + xing, "Info", 4) != 0) continue;
        const uint32_t flags = (uint32_t(frame[xing + 4]) << 24) | (uint32_t(frame[xing + 5]) << 16) |
                               (uint32_t(frame[xing + 6]) << 8) | uint32_t(frame[xing + 7]);
        if ((flags & 1u) == 0) continue;  // frame count not present
        const uint32_t frames = (uint32_t(frame[xing + 8]) << 24) | (uint32_t(frame[xing + 9]) << 16) |
                                (uint32_t(frame[xing + 10]) << 8) | uint32_t(frame[xing + 11]);
        const uint32_t samples_per_frame = version == 3 ? 1152 : 576;
        if (frames && rate) {
            duration_ = static_cast<uint32_t>((uint64_t(frames) * samples_per_frame) / rate);
        }
        return;
    }
}
DecodeStep Mp3Stream::next() {
    frames_ = 0;
    if (!decoder_) return fail(ESP_ERR_INVALID_STATE);
    if (!remaining_ && !eof_) {
        remaining_ = file_.read(input_.data(), input_.size());
        position_ = 0;
        if (file_.failed()) return fail(ESP_FAIL);
        eof_ = file_.eof();
    }
    // MP3 parser consumes whole input blocks; like the upstream MP3 example,
    // do not send an unsupported empty decode call after the final block.
    if (!remaining_ && eof_) return produced_ ? DecodeStep::End : fail(ESP_ERR_INVALID_RESPONSE);
    esp_audio_simple_dec_raw_t raw{};
    raw.buffer = input_.data() + position_;
    raw.len = remaining_;
    raw.eos = eof_;
    esp_audio_simple_dec_out_t output{};
    output.buffer = reinterpret_cast<uint8_t*>(pcm_.data());
    output.len = sizeof(pcm_);
    const auto result = esp_audio_simple_dec_process(decoder_, &raw, &output);
    if (result != ESP_AUDIO_ERR_OK || raw.consumed > remaining_ || output.decoded_size > sizeof(pcm_))
        return fail(ESP_ERR_INVALID_RESPONSE);
    remaining_ -= raw.consumed;
    position_ += raw.consumed;
    if (!output.decoded_size) {
        if (eof_ && !remaining_) return produced_ ? DecodeStep::End : fail(ESP_ERR_INVALID_RESPONSE);
        if (!raw.consumed) return fail(ESP_ERR_INVALID_RESPONSE); // no infinite parser loop
        return DecodeStep::NeedInput;
    }
    esp_audio_simple_dec_info_t info{};
    if (esp_audio_simple_dec_get_info(decoder_, &info) != ESP_AUDIO_ERR_OK ||
        info.bits_per_sample != 16 || (info.channel != 1 && info.channel != 2) ||
        info.sample_rate < 8000 || info.sample_rate > 48000 ||
        output.decoded_size % (info.channel * sizeof(int16_t))) return fail(ESP_ERR_NOT_SUPPORTED);
    frames_ = output.decoded_size / (info.channel * sizeof(int16_t));
    if (frames_ > 1152) return fail(ESP_ERR_INVALID_SIZE);
    if (info.channel == 1) {
        for (std::size_t i = frames_; i-- > 0;) pcm_[2*i] = pcm_[2*i+1] = pcm_[i];
    }
    rate_ = info.sample_rate;
    bitrate_ = info.bitrate;
    // No Xing header: estimate from the byte size and the first frame's bitrate.
    if (!duration_ && file_size_ && bitrate_) {
        duration_ = static_cast<uint32_t>((uint64_t(file_size_) * 8) / bitrate_);
    }
    produced_ = true;
    return DecodeStep::Pcm;
}
}
