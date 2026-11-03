#include "power_policy.h"

#include <cassert>
#include <cstdio>

namespace {

Power::Inputs idle(uint32_t ms) {
    Power::Inputs inputs;
    inputs.idle_ms = ms;
    return inputs;
}

Power::Inputs audio(uint32_t ms) {
    Power::Inputs inputs;
    inputs.audio_playing = true;
    inputs.idle_ms = ms;
    return inputs;
}

}  // namespace

int main() {
    // Five minute screen window, one minute sleep window.
    const Power::Config five{5, 1};

    assert(!Power::evaluate(idle(0), five).screen_off);
    assert(!Power::evaluate(idle(0), five).deep_sleep);
    assert(!Power::evaluate(idle(59999), five).deep_sleep);   // just inside
    assert(Power::evaluate(idle(60000), five).deep_sleep);    // boundary
    assert(!Power::evaluate(idle(299999), five).screen_off);  // screen window not yet
    assert(Power::evaluate(idle(300000), five).screen_off);

    // Audio playing: panel may switch off, the device must never sleep.
    const auto playing = Power::evaluate(audio(5 * 60000), five);
    assert(playing.screen_off && !playing.deep_sleep);
    assert(!Power::evaluate(audio(24 * 3600000u), five).deep_sleep);

    // Zero disables the corresponding action.
    const Power::Config off{0, 0};
    assert(!Power::evaluate(idle(24 * 3600000u), off).screen_off);
    assert(!Power::evaluate(idle(24 * 3600000u), off).deep_sleep);

    // Host activity (USB transfer / BLE) keeps everything awake.
    Power::Inputs host = idle(24 * 3600000u);
    host.host_active = true;
    assert(!Power::evaluate(host, five).screen_off);
    assert(!Power::evaluate(host, five).deep_sleep);

    // Video is content: never blanks, never sleeps.
    Power::Inputs video = idle(24 * 3600000u);
    video.video_playing = true;
    assert(!Power::evaluate(video, five).screen_off);
    assert(!Power::evaluate(video, five).deep_sleep);

    // One-minute windows on both actions, boundary inclusive.
    const Power::Config one{1, 1};
    assert(!Power::evaluate(idle(59999), one).screen_off);
    assert(Power::evaluate(idle(60000), one).screen_off);
    assert(Power::evaluate(idle(60000), one).deep_sleep);

    std::puts("power policy ok");
    return 0;
}
