/* FRAM 组件: MB85RC16PNF (16Kbit = 2KB) I2C 铁电存储器
 *
 * 注意 MB85RC16 的寻址方式与普通 I2C EEPROM 不同:
 *   11 位地址 = 高 3 位(编码进器件地址) + 低 8 位(字地址)
 *   器件地址字 = 1010 + 高3位 + R/W, 即高 3 位决定 0x50..0x57
 *   所以访问地址 a 时: 7 位器件地址 = 0x50 | (a >> 8), 再发送字地址 a & 0xFF
 */
#pragma once
#include <Arduino.h>

namespace Fram {
  bool    begin();                                   // 初始化 I2C 并探测器件
  bool    ok();                                      // FRAM 是否在线
  uint8_t readByte(uint16_t addr);
  void    writeByte(uint16_t addr, uint8_t v);
  void    read(uint16_t addr, uint8_t* buf, size_t len);
  void    write(uint16_t addr, const uint8_t* buf, size_t len);
}