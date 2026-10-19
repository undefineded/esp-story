#pragma once
#include <stdint.h>
struct TestSerial {
  template<class... Args> void printf(const char*, Args...) {}
};
extern TestSerial Serial;
