#include "../components/audio/include/equalizer.h"
#include <cassert>
#include <vector>
static double rms(Audio::EqPreset preset, int frequency, int rate) {
    Audio::Equalizer eq; eq.configure(rate, preset);
    std::vector<int16_t> pcm(2 * rate);
    for (int i=0; i<rate; ++i) pcm[2*i] = pcm[2*i+1] = 1000*std::sin(2*3.141592653589793*frequency*i/rate);
    eq.process(pcm.data(), rate, 21);
    double power = 0;
    for (int i=rate/2; i<rate; ++i) { assert(pcm[2*i] == pcm[2*i+1]); power += double(pcm[2*i])*pcm[2*i]; }
    return std::sqrt(power/(rate/2));
}
int main() {
    Audio::Equalizer eq; eq.configure(44100, Audio::EqPreset::Flat);
    int16_t flat[] = {-32768,32767,-1,1,0,0};
    eq.process(flat,3,21);
    assert(flat[0]==-32768 && flat[1]==32767 && flat[2]==-1 && flat[3]==1);
    assert(rms(Audio::EqPreset::Bass, 100, 44100) > rms(Audio::EqPreset::Flat, 100, 44100)*1.6);
    assert(rms(Audio::EqPreset::Vocal, 3000, 44100) > rms(Audio::EqPreset::Flat, 3000, 44100)*1.3);
    for (int rate : {8000,16000,22050,24000,44100,48000}) {
        for (auto preset : {Audio::EqPreset::Vocal,Audio::EqPreset::Pop,Audio::EqPreset::Bass}) {
            assert(std::isfinite(rms(preset, 1000, rate)));
            eq.configure(rate,preset);
            int16_t pcm[] = {32767,-32768,32767,-32768};
            eq.process(pcm,2,0);
            for (auto sample:pcm) assert(sample==0);
        }
    }
}
