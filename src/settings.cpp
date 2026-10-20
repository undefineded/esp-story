#include "settings.h"
#include "config.h"
#include "fram.h"

namespace {
  /* 默认: 满亮度, 横屏 180°, 人声 EQ, 蓝牙广播开, WiFi/AP 关 */
  SettingsData s_data = { 100, 3, EqPreset::Vocal, 0, 1, 0, 0 };

  const uint16_t FRAM_ADDR = 0x0000;
  const uint8_t  MAGIC[4]  = { 'M', 'P', 'L', '3' };  // v3: 12 字节布局 (含三个无线开关)

  /* 存储布局: magic(4) + brightness + rotation + eq + reserved
                + bleEnabled + wifiEnabled + apEnabled + checksum = 12 字节 */
  const size_t DATA_LEN = 11;
  uint8_t checksum(const uint8_t* b, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
  }
}

namespace Settings {

SettingsData& data() { return s_data; }

void load() {
  uint8_t buf[12];
  Fram::read(FRAM_ADDR, buf, sizeof(buf));

  bool valid = (memcmp(buf, MAGIC, 4) == 0) &&
               (checksum(buf, DATA_LEN) == buf[DATA_LEN]);
  if (valid) {
    s_data.brightness = buf[4];
    s_data.rotation   = buf[5];
    s_data.eq         = (EqPreset)buf[6];
    s_data.reserved   = buf[7];
    s_data.bleEnabled = buf[8];
    s_data.wifiEnabled = buf[9];
    s_data.apEnabled  = buf[10];
  }

  /* 合法性校验, 防止损坏数据导致无法操作 */
  if (s_data.brightness < BRIGHT_MIN || s_data.brightness > BRIGHT_MAX) s_data.brightness = 100;
  if (s_data.rotation != 1 && s_data.rotation != 3) s_data.rotation = 3;
  if ((int)s_data.eq >= (int)EqPreset::COUNT) s_data.eq = EqPreset::Vocal;
  if (s_data.bleEnabled > 1) s_data.bleEnabled = 1;
  if (s_data.wifiEnabled > 1) s_data.wifiEnabled = 0;
  if (s_data.apEnabled > 1) s_data.apEnabled = 0;
}

void save() {
  uint8_t buf[12];
  memcpy(buf, MAGIC, 4);
  buf[4] = s_data.brightness;
  buf[5] = s_data.rotation;
  buf[6] = (uint8_t)s_data.eq;
  buf[7] = s_data.reserved;
  buf[8] = s_data.bleEnabled;
  buf[9] = s_data.wifiEnabled;
  buf[10] = s_data.apEnabled;
  buf[11] = checksum(buf, DATA_LEN);
  Fram::write(FRAM_ADDR, buf, sizeof(buf));
}

}