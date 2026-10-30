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
