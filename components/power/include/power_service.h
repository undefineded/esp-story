#pragma once
#include <cstdint>

#include "power_policy.h"

namespace Power {

// Call once from the app task after Display/Input are ready. Logs the wake
// cause so a blank screen after deep sleep is not a silent mystery.
void begin();

// Evaluates the idle policy. Returns true when the panel is off: the caller
// should skip rendering for that frame but keep polling input.
// Entering deep sleep does not return (when CONFIG_STORYBOX_POWER_SLEEP is on).
bool loop(uint32_t now_ms, const Inputs& inputs, const Config& config);

bool screenOff();

// Force the panel back on (for example before showing a modal page).
void wake(uint8_t brightness);

}
