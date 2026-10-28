/* NXP PN532 NFC 读卡器驱动 (I2C 模式)
 *
 * 只依赖 Wire, 不引入额外库:
 *   - PN532 I2C 固定地址 0x24, 与 FRAM 共用 SDA/SCL
 *   - 通过 I2C 的 RDY 字节轮询, 不强制连接 IRQ / RSTPDN
 *   - 命令帧/响应帧做长度、LCS、DCS 校验, 失败可重试
 *   - 支持 ISO14443A 寻卡、Type 2 (NTAG/Ultralight) 页读写、
 *     Mifare Classic 认证与块读写
 */
#pragma once
#include <Arduino.h>

namespace Pn532 {

enum Error : uint8_t {
  Ok = 0,
  NoDevice,       // I2C 地址无应答
  Timeout,        // 等 RDY / 响应超时
  BadFrame,       // 帧头 / 校验失败
  Nack,           // 未收到 ACK 帧
  CommandFailed,  // PN532 返回错误状态
  BadArg,
};

struct Target {
  bool    found = false;
  uint8_t uid[10] = {0};
  uint8_t uidLen = 0;
  uint16_t atqa = 0;
  uint8_t sak = 0;
  uint8_t target = 1;
};

bool begin();                                  // 配置 I2C, 唤醒并探测 PN532
bool probe();                                  // 重新探测, 成功则刷新固件版本
bool present();
Error lastError();

uint8_t firmwareMajor();
uint8_t firmwareMinor();
const char* firmwareVersion();                 // "1.6", 未知时 "--"

bool listPassiveTarget(Target& out, uint16_t timeoutMs = 90);
bool releaseTarget();

bool inDataExchange(const uint8_t* tx, uint8_t txLen, uint8_t* rx, uint8_t& rxLen);
bool readPage(uint8_t page, uint8_t out[16]);  // Type 2: 一次读 4 页 / 16B
bool writePage(uint8_t page, const uint8_t data[4]);
bool getVersion(uint8_t out[8], uint8_t& outLen);

bool authenticate(uint8_t block, bool keyB, const uint8_t key[6], const Target& t);
bool readBlock(uint8_t block, uint8_t out[16]);
bool writeBlock(uint8_t block, const uint8_t in[16]);

/* Type 2 标签规格 (由 GET_VERSION 或 CC 推断) */
struct Type2Info {
  bool     valid = false;
  uint16_t bytes = 0;       // 用户区总字节数
  uint16_t pages = 0;       // 用户页数 (从第 4 页开始)
  uint8_t  ccSize = 0;      // CC 中的容量字节 (size/8)
};
Type2Info resolveType2(uint8_t storageSize);

} // namespace Pn532
