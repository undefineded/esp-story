#include "fram.h"
#include "config.h"
#include <Wire.h>

namespace {
  bool s_ok = false;

  /* 由 11 位地址得到 7 位 I2C 器件地址 (高 3 位编码在器件地址中) */
  inline uint8_t devAddr(uint16_t addr) {
    return (uint8_t)(0x50 | ((addr >> 8) & 0x07));
  }
}

namespace Fram {

bool begin() {
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  Wire.begin(I2C_SDA, I2C_SCL, 100000);           // 100kHz, 兼容较弱的外部上拉

  Wire.beginTransmission(devAddr(0));             // 探测器件
  s_ok = (Wire.endTransmission() == 0);
  return s_ok;
}

bool ok() { return s_ok; }

uint8_t readByte(uint16_t addr) {
  if (!s_ok) return 0xFF;
  uint8_t dev = devAddr(addr);
  Wire.beginTransmission(dev);
  Wire.write((uint8_t)(addr & 0xFF));
  if (Wire.endTransmission(false) != 0) return 0xFF;   // repeated start
  if (Wire.requestFrom((uint8_t)dev, (uint8_t)1) != 1) return 0xFF;
  return (uint8_t)Wire.read();
}

void writeByte(uint16_t addr, uint8_t v) {
  if (!s_ok) return;
  uint8_t dev = devAddr(addr);
  Wire.beginTransmission(dev);
  Wire.write((uint8_t)(addr & 0xFF));
  Wire.write(v);
  Wire.endTransmission();
}

void read(uint16_t addr, uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) buf[i] = readByte((uint16_t)(addr + i));
}

void write(uint16_t addr, const uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++) writeByte((uint16_t)(addr + i), buf[i]);
}

} // namespace Fram