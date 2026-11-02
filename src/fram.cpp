#include "fram.h"
#include "config.h"
#include <Wire.h>

namespace {
  bool s_ok = false;

  /* 由 11 位地址得到 7 位 I2C 器件地址 (高 3 位编码在器件地址中) */
  inline uint8_t devAddr(uint16_t addr) {
    return (uint8_t)(0x50 | ((addr >> 8) & 0x07));
  }

  /* 标准 I2C 总线恢复。
     从机(这里是 PN532)在一次传输进行到一半时被复位, 会一直把 SDA 拉低;
     之后主机的任何事务都失败, 总线上所有器件(FRAM 也)全部失联。
     办法: 主机在 SCL 上补最多 9 个时钟, 让从机把当前字节发完并松开 SDA,
     再补一个 STOP。正常总线上 SDA/SCL 都是高, 直接返回, 不影响启动时间。 */
  void recoverBus() {
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, INPUT_PULLUP);
    delayMicroseconds(50);
    if (digitalRead(I2C_SDA) == HIGH && digitalRead(I2C_SCL) == HIGH) return;

    Serial.printf("[SYS] I2C bus stuck (SDA=%d SCL=%d), recovering\n",
                  digitalRead(I2C_SDA), digitalRead(I2C_SCL));
    pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
    for (int i = 0; i < 9; i++) {
      if (digitalRead(I2C_SDA) == HIGH && digitalRead(I2C_SCL) == HIGH) break;
      digitalWrite(I2C_SCL, LOW);
      delayMicroseconds(10);
      digitalWrite(I2C_SCL, HIGH);
      delayMicroseconds(10);
    }
    /* STOP: SCL 高电平期间 SDA 由低变高 */
    pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
    digitalWrite(I2C_SDA, LOW);
    delayMicroseconds(10);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(10);
    digitalWrite(I2C_SDA, HIGH);
    delayMicroseconds(10);
    pinMode(I2C_SDA, INPUT_PULLUP);
    pinMode(I2C_SCL, INPUT_PULLUP);
    delayMicroseconds(50);
    if (digitalRead(I2C_SDA) == LOW || digitalRead(I2C_SCL) == LOW) {
      /* 9 个时钟还解不开: 从机把 SCL 一直拉低(或线路短路), 只能断电重启模块;
         若 RSTPDN 接到了 GPIO, 可以改成软件复位 (见 config.h 的 PN532_RST_PIN)。 */
      Serial.println("[SYS] I2C bus still stuck, power-cycle the PN532");
    } else {
      Serial.println("[SYS] I2C bus recovered");
    }
  }
}

namespace Fram {

bool begin() {
  recoverBus();                                   // 先把可能被从机拉死的总线解开
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