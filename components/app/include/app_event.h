#pragma once
#include <cstdint>

namespace App {

// One input sample of the UI loop. Button events are already debounced by the
// input component; the UI must not apply a second layer of debouncing.
enum class Key {
    None,
    Ok,        // single click
    OkDouble,  // double click
    OkLong,    // long press (OK_LONG_MS)
    Next,      // NEXT short press
    Prev,      // LAST short press
    NextLong,
    PrevLong,
    Set,       // encoder push, short
    SetLong,   // encoder push, long
};

struct Event {
    Key key = Key::None;
    int delta = 0;  // encoder steps this sample, positive = clockwise

    bool empty() const { return key == Key::None && delta == 0; }
};

}
