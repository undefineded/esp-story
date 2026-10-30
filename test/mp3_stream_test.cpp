// Fault-inject the codec C API while compiling the production stream adapter.
// This verifies I/O/lifetime/framing, not the proprietary MP3 decoder algorithm.
#include "mp3_stream.h"
#include "esp_mp3_dec.h"
#include <cassert>
#include <cstring>
#include <unistd.h>

static unsigned mode = 0, closes = 0, decoded = 0;
extern "C" esp_audio_err_t esp_mp3_dec_register() { return ESP_AUDIO_ERR_OK; }
extern "C" esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t*, void** handle) {
    *handle = &mode; decoded = 0; return ESP_AUDIO_ERR_OK;
}
extern "C" void esp_audio_simple_dec_close(void*) { ++closes; }
extern "C" esp_audio_err_t esp_audio_simple_dec_get_info(void*, esp_audio_simple_dec_info_t* info) {
    *info = {}; info->channel = mode == 3 ? 3 : 1;
    info->bits_per_sample = 16; info->sample_rate = 44100; info->bitrate = 128000;
    return ESP_AUDIO_ERR_OK;
}
extern "C" esp_audio_err_t esp_audio_simple_dec_process(void*, esp_audio_simple_dec_raw_t* raw,
                                                        esp_audio_simple_dec_out_t* out) {
    assert(raw->len); // adapter must not submit an empty MP3 buffer
    if (mode == 1) return ESP_AUDIO_ERR_OK; // parser made no progress
    if (mode == 2) { raw->consumed = raw->len + 1; return ESP_AUDIO_ERR_OK; }
    raw->consumed = std::min(raw->len, uint32_t{1024});
    for (unsigned i = 0; i < raw->consumed; ++i) assert(raw->buffer[i] == uint8_t(decoded++));
    const int16_t mono[] = {123, -456};
    std::memcpy(out->buffer, mono, sizeof(mono));
    out->decoded_size = sizeof(mono);
    return ESP_AUDIO_ERR_OK;
}
int main() {
    char dir[] = "/tmp/storybox-mp3-stream.XXXXXX";
    assert(mkdtemp(dir));
    const std::string path = std::string(dir) + "/sample";
    auto* source = std::fopen(path.c_str(), "wb");
    assert(source);
    for (unsigned i = 0; i < 4098; ++i) assert(std::fputc(uint8_t(i), source) != EOF);
    assert(std::fclose(source) == 0);
    Storage::AccessGate gate;
    Audio::Mp3Stream stream(gate);
    assert(stream.open(dir, "sample") == ESP_OK);
    assert(!gate.beginMaintenance(std::chrono::milliseconds(1)));
    unsigned blocks = 0;
    while (stream.next() == Audio::DecodeStep::Pcm) {
        assert(stream.frames() == 2 && stream.sampleRate() == 44100);
        const auto* pcm = stream.samples();
        assert(pcm[0] == 123 && pcm[1] == 123 && pcm[2] == -456 && pcm[3] == -456);
        ++blocks;
    }
    assert(blocks == 5 && decoded == 4098);
    assert(stream.next() == Audio::DecodeStep::End);
    assert(stream.close() && closes == 1);
    assert(gate.beginMaintenance(std::chrono::milliseconds(1)));
    assert(stream.open(dir, "sample") != ESP_OK);
    gate.endMaintenance();
    for (mode = 1; mode <= 3; ++mode) {
        assert(stream.open(dir, "sample") == ESP_OK);
        assert(stream.next() == Audio::DecodeStep::Error);
        assert(stream.close());
    }
    assert(std::remove(path.c_str()) == 0);
    assert(rmdir(dir) == 0);
}
