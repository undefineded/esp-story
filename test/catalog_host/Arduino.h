#pragma once
#include <stdint.h>
#include <string>
using String = std::string;
struct TestSerial {
  template<class... Args> void printf(const char*, Args...) {}
};
extern TestSerial Serial;
