#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
constexpr int INPUT_PULLUP=1,OUTPUT=2,HIGH=1,LOW=0;
inline uint32_t fake_millis=0;
inline uint32_t millis(){return fake_millis;}
inline void delay(uint32_t value){fake_millis+=value;}
inline void pinMode(int,int){}
inline void digitalWrite(int,int){}

// The Arduino core exposes the ESP-IDF GPIO API through driver/gpio.h; the
// pn532 driver uses gpio_set_pull_mode so it never turns the shared I2C pins
// into plain inputs. Host builds need the same symbols.
using gpio_num_t = int;
constexpr int GPIO_PULLUP_ONLY = 2;
inline void gpio_set_pull_mode(int,int){}

// Shared logging shim used by src/pn532.cpp under ARDUINO.
struct FakeSerial {
    template <class... Args> void printf(const char*, Args...) {}
    void println(const char*) {}
    void println(){}
};
inline FakeSerial Serial;
