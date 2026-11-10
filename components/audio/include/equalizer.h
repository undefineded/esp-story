#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "pcm_gain.h"
namespace Audio {
enum class EqPreset : uint8_t { Vocal, Pop, Bass, Flat };
// Four bands: low shelf, low-mid bell (mud), presence bell, high shelf.
// Tuned for speech/vocal intelligibility on a small full-range speaker, where
// everything under ~120 Hz mostly turns into distortion.
//
//   band 0  low shelf   : removes sub-bass / boom
//   band 1  bell        : cuts the 300-500 Hz "boxy" region
//   band 2  bell        : presence, 2-3 kHz is where consonants live
//   band 3  high shelf  : air; kept gentle because cheap tweeters get hissy
//
// pre_gain_db compensates the worst-case boost so the int16 output does not
// clip before the volume stage; the listeners lose ~2-3 dB of level instead.
class Equalizer {
public:
    void reset() {
        for (auto& band : bands_) for (auto& state : band.state) state = {};
    }
    void configure(uint32_t rate, EqPreset preset) {
        if (rate == rate_ && preset == preset_) return;
        rate_ = rate; preset_ = preset; reset();
        const auto index = std::min(unsigned(preset), 3u);
        for (unsigned i = 0; i < kBandCount; ++i) {
            const auto& spec = kSpec[index][i];
            configureBand(bands_[i], rate, spec.frequency, spec.db, spec.type, spec.q);
        }
        pre_gain_ = std::pow(10.0f, kSpec[index][0].pre_gain_db / 20.0f);
    }
    void process(int16_t* pcm, std::size_t frames, unsigned volume) {
        // Provisional gain curve shared with standalone PCM gain tests.
        const float gain = volumeGainQ15(volume) / 32768.0f * pre_gain_;
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
    static constexpr unsigned kBandCount = 4;
    struct State { float z1 = 0, z2 = 0; };
    struct Band { float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; State state[2]; };
    struct Spec {
        float frequency;   // Hz, clamped to 0.45 * sample rate
        float q;           // bell bandwidth; ignored by the shelves
        float db;          // band gain
        unsigned char type;  // 0 low shelf, 1 bell, 2 high shelf
        float pre_gain_db;   // only the first entry of a row is read
    };
    static constexpr Spec kSpec[4][kBandCount] = {
        // Vocal: thin the low end, scoop the box, lift presence and air.
        {{150, 0.7f, -5.0f, 0, -3.0f},
         {400, 0.9f, -2.0f, 1, 0},
         {2400, 1.1f, 3.0f, 1, 0},
         {7000, 0.7f, 1.5f, 2, 0}},
        // Pop: milder version, keeps some body, small presence lift.
        {{100, 0.7f, -3.0f, 0, -2.0f},
         {500, 1.0f, -1.0f, 1, 0},
         {3000, 1.0f, 2.0f, 1, 0},
         {8000, 0.7f, 1.0f, 2, 0}},
        // Bass: boost the octave the speaker can still reproduce, not below it.
        {{110, 0.7f, 6.0f, 0, -2.5f},
         {300, 1.0f, 1.0f, 1, 0},
         {3000, 1.0f, -1.0f, 1, 0},
         {8000, 0.7f, 0.0f, 2, 0}},
        // Flat: bypass, no pre-gain.
        {{150, 0.7f, 0.0f, 0, 0},
         {400, 0.9f, 0.0f, 1, 0},
         {2400, 1.1f, 0.0f, 1, 0},
         {7000, 0.7f, 0.0f, 2, 0}},
    };
    static void configureBand(Band& b, uint32_t rate, float frequency,
                              float db, unsigned type, float q) {
        b = {};
        if (!db || !rate) return;
        constexpr float pi = 3.14159265358979323846f;
        const float w = 2 * pi * std::min(frequency, float(rate) * 0.45f) / rate;
        const float c = std::cos(w), s = std::sin(w), A = std::pow(10.0f, db / 40.0f);
        float a0, a1, a2, b0, b1, b2;
        if (type == 1) {
            const float alpha = s / (2 * std::max(q, 0.1f));
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
    Band bands_[kBandCount];
    float pre_gain_ = 1.0f;
    uint32_t rate_ = 0;
    EqPreset preset_ = EqPreset::Flat;
};
}

