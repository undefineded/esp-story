#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "pcm_gain.h"
namespace Audio {
enum class EqPreset : uint8_t { Vocal, Pop, Bass, Flat };
class Equalizer {
public:
    void reset() { for (auto& band : bands_) for (auto& state : band.state) state = {}; }
    void configure(uint32_t rate, EqPreset preset) {
        if (rate == rate_ && preset == preset_) return;
        rate_ = rate; preset_ = preset; reset();
        const int gains[4][3] = {{-2, 4, -1}, {2, 2, 1}, {5, 0, -1}, {0, 0, 0}};
        const auto index = std::min(unsigned(preset), 3u);
        configureBand(bands_[0], rate, 500, gains[index][0], 0);
        configureBand(bands_[1], rate, 3000, gains[index][1], 1);
        configureBand(bands_[2], rate, 6000, gains[index][2], 2);
    }
    void process(int16_t* pcm, std::size_t frames, unsigned volume) {
        // Provisional gain curve shared with standalone PCM gain tests.
        const float gain = volumeGainQ15(volume) / 32768.0f;
        for (std::size_t i = 0; i < frames * 2; ++i) {
            float value = pcm[i];
            for (auto& band : bands_) {
                auto& state = band.state[i % 2];
                const float output = band.b0 * value + state.z1;
                state.z1 = band.b1 * value - band.a1 * output + state.z2;
                state.z2 = band.b2 * value - band.a2 * output;
                value = output;
            }
            // Apply volume after EQ, then saturate once to retain internal headroom.
            value *= gain;
            pcm[i] = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, value)));
        }
    }
private:
    struct State { float z1 = 0, z2 = 0; };
    struct Band { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; State state[2]; };
    static void configureBand(Band& b, uint32_t rate, float frequency, int db, int type) {
        b = {};
        if (!db || !rate) return;
        constexpr float pi = 3.14159265358979323846f;
        const float w = 2 * pi * std::min(frequency, float(rate) * 0.45f) / rate;
        const float c = std::cos(w), s = std::sin(w), A = std::pow(10.0f, db / 40.0f);
        float a0, a1, a2, b0, b1, b2;
        if (type == 1) {
            const float alpha = s / (2 * 2.5f);
            b0 = 1 + alpha*A; b1 = -2*c; b2 = 1-alpha*A;
            a0 = 1 + alpha/A; a1 = -2*c; a2 = 1-alpha/A;
        } else {
            const float beta = std::sqrt(2*A) * s;
            if (type == 0) {
                b0=A*((A+1)-(A-1)*c+beta); b1=2*A*((A-1)-(A+1)*c); b2=A*((A+1)-(A-1)*c-beta);
                a0=(A+1)+(A-1)*c+beta; a1=-2*((A-1)+(A+1)*c); a2=(A+1)+(A-1)*c-beta;
            } else {
                b0=A*((A+1)+(A-1)*c+beta); b1=-2*A*((A-1)+(A+1)*c); b2=A*((A+1)+(A-1)*c-beta);
                a0=(A+1)-(A-1)*c+beta; a1=2*((A-1)-(A+1)*c); a2=(A+1)-(A-1)*c-beta;
            }
        }
        b.b0=b0/a0; b.b1=b1/a0; b.b2=b2/a0; b.a1=a1/a0; b.a2=a2/a0;
    }
    Band bands_[3];
    uint32_t rate_ = 0;
    EqPreset preset_ = EqPreset::Flat;
};
}
