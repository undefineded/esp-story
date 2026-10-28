#include "pn532.h"
#include "config.h"
#include <Wire.h>
#include <cstring>

namespace {

constexpr uint8_t  PN532_PREAMBLE   = 0x00;
constexpr uint8_t  PN532_START1     = 0x00;
constexpr uint8_t  PN532_START2     = 0xFF;
constexpr uint8_t  PN532_POSTAMBLE  = 0x00;
constexpr uint8_t  PN532_HOSTTOPN532 = 0xD4;
constexpr uint8_t  PN532_PN532TOHOST = 0xD5;
constexpr uint8_t  PN532_ACK[]       = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};

constexpr uint8_t  CMD_GET_FIRMWARE    = 0x02;
constexpr uint8_t  CMD_SAM_CONFIG      = 0x14;
constexpr uint8_t  CMD_RF_CONFIG       = 0x32;
constexpr uint8_t  CMD_IN_LIST_PASSIVE = 0x4A;
constexpr uint8_t  CMD_IN_DATA_EXCH    = 0x40;
constexpr uint8_t  CMD_IN_RELEASE      = 0x52;

constexpr uint8_t  RESP_GET_FIRMWARE    = 0x03;
constexpr uint8_t  RESP_SAM_CONFIG      = 0x15;
constexpr uint8_t  RESP_RF_CONFIG       = 0x33;
constexpr uint8_t  RESP_IN_LIST_PASSIVE = 0x4B;
constexpr uint8_t  RESP_IN_DATA_EXCH    = 0x41;
constexpr uint8_t  RESP_IN_RELEASE      = 0x53;

constexpr uint32_t kAckTimeoutMs      = 500;
constexpr uint32_t kResponseTimeoutMs = 600;

bool     s_present = false;
uint8_t  s_fwMajor = 0;
uint8_t  s_fwMinor = 0;
uint8_t  s_fwRev   = 0;
uint8_t  s_fwIci   = 0;
char     s_fwText[12] = "--";
Pn532::Error s_error = Pn532::NoDevice;

uint8_t i2cAddr() { return (uint8_t)PN532_I2C_ADDR; }

/* I2C 总线和 FRAM 共用 (见 config.h)。向"不存在的 0x24"发事务失败后, ESP-IDF 的
   i2c 驱动会停在错误态, 之后同一总线上的 FRAM 读写会一直报 INVALID_STATE (263) ——
   表现为开机卡死。所以探测失败后必须复位一次驱动, 把总线还给 FRAM。 */
void busRecover() {
  Wire.end();
  delay(2);
  Wire.begin(I2C_SDA, I2C_SCL, (uint32_t)PN532_I2C_HZ);
  Wire.setClock((uint32_t)PN532_I2C_HZ);
  delay(2);
}

bool i2cPing() {
  Wire.beginTransmission(i2cAddr());
  return Wire.endTransmission(true) == 0;
}

/* PN532 I2C 读: 第一个字节是 RDY (0x01 就绪), 其余为帧数据 */
bool readRaw(uint8_t* out, uint8_t len) {
  if (len == 0) return true;
  uint8_t got = (uint8_t)Wire.requestFrom((uint8_t)i2cAddr(), (uint8_t)(len + 1));
  if (got < (uint8_t)(len + 1)) {
    while (Wire.available()) Wire.read();
    return false;
  }
  int rdy = Wire.read();
  for (uint8_t i = 0; i < len; i++) out[i] = (uint8_t)Wire.read();
  return rdy == 0x01;
}

bool waitReady(uint32_t timeoutMs) {
  uint32_t start = millis();
  for (;;) {
    uint8_t got = (uint8_t)Wire.requestFrom((uint8_t)i2cAddr(), (uint8_t)1);
    if (got == 1) {
      int b = Wire.read();
      if (b == 0x01) return true;
    } else {
      while (Wire.available()) Wire.read();
    }
    if (millis() - start >= timeoutMs) return false;
    delay(2);
  }
}

bool writeCommand(const uint8_t* cmd, uint8_t cmdLen) {
  if (cmdLen > 64) { s_error = Pn532::BadArg; return false; }
  uint8_t frame[8 + 64];
  uint8_t len = (uint8_t)(cmdLen + 1);            // TFI + command
  frame[0] = PN532_PREAMBLE;
  frame[1] = PN532_START1;
  frame[2] = PN532_START2;
  frame[3] = len;
  frame[4] = (uint8_t)(~len + 1);
  frame[5] = PN532_HOSTTOPN532;
  uint8_t sum = PN532_HOSTTOPN532;
  for (uint8_t i = 0; i < cmdLen; i++) {
    frame[6 + i] = cmd[i];
    sum = (uint8_t)(sum + cmd[i]);
  }
  frame[6 + cmdLen] = (uint8_t)(~sum + 1);
  frame[7 + cmdLen] = PN532_POSTAMBLE;

  Wire.beginTransmission(i2cAddr());
  Wire.write(frame, (size_t)(8 + cmdLen));
  if (Wire.endTransmission() != 0) { s_error = Pn532::NoDevice; return false; }
  return true;
}

bool readAck() {
  uint8_t ack[6];
  if (!readRaw(ack, sizeof(ack))) { s_error = Pn532::Timeout; return false; }
  if (memcmp(ack, PN532_ACK, sizeof(ack)) != 0) { s_error = Pn532::Nack; return false; }
  return true;
}

/* 读取完整响应帧; 返回 D5 CMD 之后的数据 (不含状态外的任何头) */
bool readResponse(uint8_t expectedCmd, uint8_t* out, uint8_t& outLen, uint8_t maxLen) {
  uint8_t hdr[7];
  if (!readRaw(hdr, sizeof(hdr))) { s_error = Pn532::Timeout; return false; }

  if (hdr[0] != 0x00 || hdr[1] != 0x00 || hdr[2] != 0xFF ||
      hdr[5] != PN532_PN532TOHOST || hdr[6] != (uint8_t)(expectedCmd + 1) ||
      (uint8_t)(hdr[3] + hdr[4]) != 0xFF || hdr[3] < 3) {
    s_error = Pn532::BadFrame;
    return false;
  }

  uint8_t len = hdr[3];
  /* 头部已经消费 TFI + CMD; 剩余 = 数据(含状态) + DCS + POSTAMBLE = len */
  uint8_t rest[72];
  if (len > sizeof(rest)) { s_error = Pn532::BadFrame; return false; }
  if (!readRaw(rest, len)) { s_error = Pn532::Timeout; return false; }
  if (rest[len - 1] != PN532_POSTAMBLE) { s_error = Pn532::BadFrame; return false; }

  uint8_t dataLen = (uint8_t)(len - 2);            // len - DCS - POSTAMBLE
  uint8_t sum = (uint8_t)(PN532_PN532TOHOST + hdr[6]);
  for (uint8_t i = 0; i < dataLen; i++) sum = (uint8_t)(sum + rest[i]);
  uint8_t expectDcs = (uint8_t)(~sum + 1);
  if (rest[dataLen] != expectDcs) { s_error = Pn532::BadFrame; return false; }

  uint8_t copy = dataLen < maxLen ? dataLen : maxLen;
  if (out && copy) memcpy(out, rest, copy);
  outLen = copy;
  return true;
}

/* 一条完整命令: 写帧 -> 等 ACK -> 等响应 */
bool command(const uint8_t* cmd, uint8_t cmdLen, uint8_t expectedResp,
             uint8_t* out, uint8_t& outLen, uint8_t maxLen,
             uint32_t timeoutMs = kResponseTimeoutMs) {
  s_error = Pn532::Ok;
  outLen = 0;
  if (!writeCommand(cmd, cmdLen)) return false;
  if (!waitReady(kAckTimeoutMs)) { s_error = Pn532::Timeout; return false; }
  if (!readAck()) return false;
  if (!waitReady(timeoutMs)) { s_error = Pn532::Timeout; return false; }
  return readResponse(expectedResp, out, outLen, maxLen);
}

bool getFirmwareInternal() {
  uint8_t cmd = CMD_GET_FIRMWARE;
  uint8_t out[16];
  uint8_t n = 0;
  if (!command(&cmd, 1, RESP_GET_FIRMWARE, out, n, sizeof(out))) return false;
  if (n < 4) { s_error = Pn532::BadFrame; return false; }
  s_fwIci   = out[0];
  s_fwMajor = out[1];
  s_fwMinor = out[2];
  s_fwRev   = out[3];
  snprintf(s_fwText, sizeof(s_fwText), "%u.%u", (unsigned)s_fwMajor, (unsigned)s_fwMinor);
  return true;
}

bool samConfig(uint32_t timeoutMs = kResponseTimeoutMs) {
  const uint8_t cmd[4] = {CMD_SAM_CONFIG, 0x01, 0x14, 0x01};  // normal, 1s, use IRQ
  uint8_t out[8];
  uint8_t n = 0;
  return command(cmd, sizeof(cmd), RESP_SAM_CONFIG, out, n, sizeof(out), timeoutMs);
}

bool applyRfConfig() {
  /* MxRtyPassiveActivation = 1: 无卡时快速返回, 让主循环保持响应 */
  const uint8_t cmd[5] = {CMD_RF_CONFIG, 0x05, 0xFF, 0x01, 0x01};
  uint8_t out[4];
  uint8_t n = 0;
  return command(cmd, sizeof(cmd), RESP_RF_CONFIG, out, n, sizeof(out));
}

void hardwareReset() {
#if PN532_RST_PIN >= 0
  pinMode(PN532_RST_PIN, OUTPUT);
  digitalWrite(PN532_RST_PIN, HIGH);
  delay(10);
  digitalWrite(PN532_RST_PIN, LOW);
  delay(80);
  digitalWrite(PN532_RST_PIN, HIGH);
  delay(120);
#endif
}

} // namespace

namespace Pn532 {

bool begin() {
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, INPUT_PULLUP);
  /* 总线已由 Fram::begin() 用同一组引脚初始化, 这里只调速率:
     重复 Wire.begin() 会把可用驱动顶掉, 之后 FRAM 访问会持续失败。 */
  Wire.setClock((uint32_t)PN532_I2C_HZ);
  delay(20);

  s_present = false;
  s_error = NoDevice;
  hardwareReset();
  bool ok = probe();
  if (!ok) busRecover();          // 模块不在: 把总线恢复给 FRAM
  return ok;
}

bool probe() {
  bool ping = i2cPing();
  if (!ping) {
    /* 休眠中的模块可能暂时不应答地址探测; 直接尝试一次 SAMConfiguration 唤醒 */
    samConfig(800);
    delay(30);
    ping = i2cPing();
  }
  if (!ping) {
    s_present = false;
    s_error = NoDevice;
    busRecover();                 // 失败事务会让驱动停在错误态, 复位后 FRAM 才能继续用
    return false;
  }

  /* 模块可能处于 PowerDown, 先尝试 SAMConfiguration 唤醒 (PN532 会用时钟拉伸) */
  if (getFirmwareInternal()) {
    s_present = true;
    samConfig();
    applyRfConfig();
    return true;
  }

  if (samConfig(1000)) {
    delay(20);
    if (getFirmwareInternal()) {
      s_present = true;
      applyRfConfig();
      return true;
    }
  }

  s_present = false;
  if (!i2cPing()) s_error = NoDevice;
  busRecover();
  return false;
}

bool present() { return s_present; }
Error lastError() { return s_error; }
uint8_t firmwareMajor() { return s_fwMajor; }
uint8_t firmwareMinor() { return s_fwMinor; }
const char* firmwareVersion() { return s_present ? s_fwText : "--"; }

bool listPassiveTarget(Target& out, uint16_t timeoutMs) {
  out = Target{};
  const uint8_t cmd[3] = {CMD_IN_LIST_PASSIVE, 0x01, 0x00};  // 1 target, 106kbps Type A
  uint8_t data[64];
  uint8_t n = 0;
  if (!command(cmd, sizeof(cmd), RESP_IN_LIST_PASSIVE, data, n, sizeof(data),
               timeoutMs ? timeoutMs : kResponseTimeoutMs)) {
    return false;
  }
  if (n < 1) { s_error = BadFrame; return false; }
  if (data[0] == 0) { s_present = true; return true; }   // 无卡, 命令成功
  if (n < 6) { s_error = BadFrame; return false; }

  uint8_t uidLen = data[5];
  if (uidLen > sizeof(out.uid) || n < (uint8_t)(6 + uidLen)) {
    s_error = BadFrame;
    return false;
  }
  out.found  = true;
  out.target = data[1] ? data[1] : 1;
  out.atqa   = (uint16_t)(((uint16_t)data[2] << 8) | data[3]);
  out.sak    = data[4];
  out.uidLen = uidLen;
  memcpy(out.uid, data + 6, uidLen);
  s_present = true;
  return true;
}

bool releaseTarget() {
  const uint8_t cmd[2] = {CMD_IN_RELEASE, 0x00};
  uint8_t out[4];
  uint8_t n = 0;
  return command(cmd, sizeof(cmd), RESP_IN_RELEASE, out, n, sizeof(out));
}

bool inDataExchange(const uint8_t* tx, uint8_t txLen, uint8_t* rx, uint8_t& rxLen) {
  if (!tx || txLen == 0 || txLen > 32) { s_error = BadArg; return false; }
  uint8_t cmd[2 + 32];
  cmd[0] = CMD_IN_DATA_EXCH;
  cmd[1] = 0x01;                                  // target 1
  memcpy(cmd + 2, tx, txLen);
  uint8_t data[64];
  uint8_t n = 0;
  if (!command(cmd, (uint8_t)(txLen + 2), RESP_IN_DATA_EXCH, data, n, sizeof(data),
               kResponseTimeoutMs)) {
    return false;
  }
  if (n < 1) { s_error = BadFrame; return false; }
  if (data[0] != 0x00) { s_error = CommandFailed; return false; }
  rxLen = (uint8_t)(n - 1);
  if (rx && rxLen) memcpy(rx, data + 1, rxLen);
  return true;
}

bool readPage(uint8_t page, uint8_t out[16]) {
  uint8_t tx[2] = {0x30, page};
  uint8_t rx[32];
  uint8_t n = 0;
  if (!inDataExchange(tx, sizeof(tx), rx, n)) return false;
  if (n < 16) { s_error = BadFrame; return false; }
  memcpy(out, rx, 16);
  return true;
}

bool writePage(uint8_t page, const uint8_t data[4]) {
  uint8_t tx[6] = {0xA2, page, data[0], data[1], data[2], data[3]};
  uint8_t rx[8];
  uint8_t n = 0;
  if (!inDataExchange(tx, sizeof(tx), rx, n)) return false;
  delay(10);                                      // NTAG EEPROM 写入周期 (tWR max)
  return true;
}

bool getVersion(uint8_t out[8], uint8_t& outLen) {
  uint8_t tx = 0x60;
  uint8_t rx[16];
  uint8_t n = 0;
  if (!inDataExchange(&tx, 1, rx, n)) return false;
  if (n < 8) { s_error = BadFrame; return false; }
  outLen = 8;
  memcpy(out, rx, 8);
  return true;
}

bool authenticate(uint8_t block, bool keyB, const uint8_t key[6], const Target& t) {
  if (!t.found || t.uidLen == 0 || t.uidLen > 10) { s_error = BadArg; return false; }
  uint8_t tx[20];
  uint8_t i = 0;
  tx[i++] = keyB ? 0x61 : 0x60;
  tx[i++] = block;
  memcpy(tx + i, key, 6); i += 6;
  memcpy(tx + i, t.uid, t.uidLen); i += t.uidLen;
  uint8_t rx[8];
  uint8_t n = 0;
  return inDataExchange(tx, i, rx, n);
}

bool readBlock(uint8_t block, uint8_t out[16]) {
  uint8_t tx[2] = {0x30, block};
  uint8_t rx[32];
  uint8_t n = 0;
  if (!inDataExchange(tx, sizeof(tx), rx, n)) return false;
  if (n < 16) { s_error = BadFrame; return false; }
  memcpy(out, rx, 16);
  return true;
}

bool writeBlock(uint8_t block, const uint8_t in[16]) {
  uint8_t tx[18];
  tx[0] = 0xA0;
  tx[1] = block;
  memcpy(tx + 2, in, 16);
  uint8_t rx[8];
  uint8_t n = 0;
  if (!inDataExchange(tx, sizeof(tx), rx, n)) return false;
  delay(10);
  return true;
}

Type2Info resolveType2(uint8_t storageSize) {
  Type2Info info;
  /* NXP NTAG21x GET_VERSION 的 storage size:
   *   0x0B NTAG210 (48B)  0x0E NTAG212 (128B)  0x0F NTAG213 (144B)
   *   0x11 NTAG215 (504B) 0x13 NTAG216 (888B)
   * 低 7 位 n = 2^n 的 n, LSB=1 表示容量落在 2^n..2^(n+1) 之间。
   */
  switch (storageSize) {
    case 0x06: info.valid = true; info.bytes = 48;  info.pages = 12;  info.ccSize = 0x06; break;  // MF0UL11
    case 0x07: info.valid = true; info.bytes = 144; info.pages = 36;  info.ccSize = 0x12; break;  // MF0UL21
    case 0x0B: info.valid = true; info.bytes = 48;  info.pages = 12;  info.ccSize = 0x06; break;  // NTAG210
    case 0x0E: info.valid = true; info.bytes = 128; info.pages = 32;  info.ccSize = 0x10; break;  // NTAG212
    case 0x0F: info.valid = true; info.bytes = 144; info.pages = 36;  info.ccSize = 0x12; break;  // NTAG213
    case 0x11: info.valid = true; info.bytes = 504; info.pages = 126; info.ccSize = 0x3E; break;  // NTAG215
    case 0x13: info.valid = true; info.bytes = 888; info.pages = 222; info.ccSize = 0x6D; break;  // NTAG216
    default: break;
  }
  return info;
}

} // namespace Pn532
