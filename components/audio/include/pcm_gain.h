#pragma once
#include <cstddef>
#include <cstdint>
namespace Audio {
// Provisional -60..0 dB curve (3 dB/step); 0 is exact mute. Requires acoustic
// calibration against the legacy player before product acceptance.
inline int32_t volumeGainQ15(unsigned volume) {
    static constexpr int32_t gains[] = {0,33,46,65,92,130,184,260,368,519,734,
                                       1036,1464,2068,2920,4125,5827,8231,11626,16423,23198,32768};
    if (volume > 21) volume = 21;
    return gains[volume];
}
inline void applyGain(int16_t* samples, std::size_t count, unsigned volume) {
    const auto gain=volumeGainQ15(volume);
    for (std::size_t i = 0; i < count; ++i)
        samples[i] = static_cast<int16_t>((int32_t(samples[i]) * gain) / 32768);
}
}
