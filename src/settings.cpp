#include "settings.h"
#include "config.h"
#include "fram.h"

namespace {
  /* 默认: 满亮度, 横屏 180°, 人声 EQ, 蓝牙广播开, WiFi/AP 关,
           播放中 5 分钟熄屏, 无播放 1 分钟休眠 */
  SettingsData s_data = { 100, 3, EqPreset::Vocal, 0, 1, 0, 0, 5, 1 };

  const uint16_t FRAM_ADDR = 0x0000;
  const uint8_t  MAGIC[4]    = { 'M', 'P', 'L', '4' };  // v4: 14 字节布局 (新增熄屏/休眠超时)
  const uint8_t  MAGIC_V3[4] = { 'M', 'P', 'L', '3' };  // 旧版: 12 字节, 升级时迁移

  /* 存储布局: magic(4) + brightness + rotation + eq + reserved
                + ble + wifi + ap + screenOffMin + sleepMin + checksum = 14 字节 */
  const size_t DATA_LEN = 13;

  /* 可选档位 (分钟), 0 = 关闭 */
  const uint8_t MINUTES[] = { 0, 1, 3, 5, 10, 30 };
  const int     MINUTES_N = (int)(sizeof(MINUTES) / sizeof(MINUTES[0]));

  bool minutesOk(uint8_t m) {
    for (int i = 0; i < MINUTES_N; i++) if (MINUTES[i] == m) return true;
    return false;
  }

  uint8_t checksum(const uint8_t* b, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
  }
}

namespace Settings {

SettingsData& data() { return s_data; }

void load() {
  uint8_t buf[14];
  Fram::read(FRAM_ADDR, buf, sizeof(buf));

  bool valid = (memcmp(buf, MAGIC, 4) == 0) &&
               (checksum(buf, DATA_LEN) == buf[DATA_LEN]);
  if (valid) {
    s_data.brightness   = buf[4];
    s_data.rotation     = buf[5];
    s_data.eq           = (EqPreset)buf[6];
    s_data.reserved     = buf[7];
    s_data.bleEnabled   = buf[8];
    s_data.wifiEnabled  = buf[9];
    s_data.apEnabled    = buf[10];
    s_data.screenOffMin = buf[11];
    s_data.sleepMin     = buf[12];
  } else if (memcmp(buf, MAGIC_V3, 4) == 0 && checksum(buf, 11) == buf[11]) {
    /* v3 -> v4 迁移: 保留原来的亮度/旋转/EQ/无线开关, 两个新超时取默认值 */
    s_data.brightness  = buf[4];
    s_data.rotation    = buf[5];
    s_data.eq          = (EqPreset)buf[6];
    s_data.reserved    = buf[7];
    s_data.bleEnabled  = buf[8];
    s_data.wifiEnabled = buf[9];
    s_data.apEnabled   = buf[10];
    save();
    Serial.println("[SET] settings migrated v3 -> v4");
  }

  /* 合法性校验, 防止损坏数据导致无法操作 */
  if (s_data.brightness < BRIGHT_MIN || s_data.brightness > BRIGHT_MAX) s_data.brightness = 100;
  if (s_data.rotation != 1 && s_data.rotation != 3) s_data.rotation = 3;
  if ((int)s_data.eq >= (int)EqPreset::COUNT) s_data.eq = EqPreset::Vocal;
  if (s_data.bleEnabled > 1) s_data.bleEnabled = 1;
  if (s_data.wifiEnabled > 1) s_data.wifiEnabled = 0;
  if (s_data.apEnabled > 1) s_data.apEnabled = 0;
  if (!minutesOk(s_data.screenOffMin)) s_data.screenOffMin = 5;
  if (!minutesOk(s_data.sleepMin))     s_data.sleepMin = 1;
}

void save() {
  uint8_t buf[14];
  memcpy(buf, MAGIC, 4);
  buf[4]  = s_data.brightness;
  buf[5]  = s_data.rotation;
  buf[6]  = (uint8_t)s_data.eq;
  buf[7]  = s_data.reserved;
  buf[8]  = s_data.bleEnabled;
  buf[9]  = s_data.wifiEnabled;
  buf[10] = s_data.apEnabled;
  buf[11] = s_data.screenOffMin;
  buf[12] = s_data.sleepMin;
  buf[13] = checksum(buf, DATA_LEN);
  Fram::write(FRAM_ADDR, buf, sizeof(buf));
}

uint32_t screenOffMs() { return (uint32_t)s_data.screenOffMin * 60000u; }
uint32_t sleepMs()     { return (uint32_t)s_data.sleepMin * 60000u; }

const char* minutesLabel(uint8_t minutes) {
  switch (minutes) {
    case 0:  return "关闭";
    case 1:  return "1 分钟";
    case 3:  return "3 分钟";
    case 5:  return "5 分钟";
    case 10: return "10 分钟";
    case 30: return "30 分钟";
    default: return "关闭";
  }
}

uint8_t cycleMinutes(uint8_t cur, int delta) {
  int idx = 0;
  for (int i = 0; i < MINUTES_N; i++) if (MINUTES[i] == cur) idx = i;
  idx = (idx + (delta >= 0 ? 1 : -1) + MINUTES_N) % MINUTES_N;
  return MINUTES[idx];
}

}
