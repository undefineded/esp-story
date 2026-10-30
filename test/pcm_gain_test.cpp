#include "../components/audio/include/pcm_gain.h"
#include <cassert>
int main() {
    int16_t samples[] = {-32768, 32767, 0, -1, 1};
    Audio::applyGain(samples, 5, 21);
    assert(samples[0] == -32768 && samples[1] == 32767 && samples[3] == -1);
    Audio::applyGain(samples, 5, 0);
    for (auto sample : samples) assert(sample == 0);
    int last = 0;
    for (unsigned volume = 1; volume <= 21; ++volume) {
        int16_t value = 32767;
        Audio::applyGain(&value, 1, volume);
        assert(value > last && value <= 32767);
        last = value;
    }
}
