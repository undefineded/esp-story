#include "mp3_stream.h"
#include "esp_mp3_dec.h"

namespace Audio {
esp_err_t Mp3Stream::open(const std::string& root, const std::string& path) {
    if (!close()) return error_ = ESP_FAIL;
    // Register only MP3; keep other codecs and their registries out of playback.
    static const auto registered = esp_mp3_dec_register();
    if (registered != ESP_AUDIO_ERR_OK) return error_ = ESP_ERR_NO_MEM;
    if (!file_.open(root, path, "rb")) return error_ = ESP_FAIL;
    esp_audio_simple_dec_cfg_t config{};
    config.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    const auto result = esp_audio_simple_dec_open(&config, &decoder_);
    if (result != ESP_AUDIO_ERR_OK) {
        file_.close();
        return error_ = result == ESP_AUDIO_ERR_MEM_LACK ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    remaining_ = position_ = frames_ = 0;
    rate_ = bitrate_ = 0;
    eof_ = produced_ = false;
    return error_ = ESP_OK;
}
bool Mp3Stream::close() {
    if (decoder_) esp_audio_simple_dec_close(decoder_);
    decoder_ = nullptr;
    return file_.close();
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
    produced_ = true;
    return DecodeStep::Pcm;
}
}
