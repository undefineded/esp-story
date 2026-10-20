#include "settings.h"
#include "config.h"
#include "fram.h"

namespace {
  /* 默认: 满亮度, 横屏 180°, 人声 EQ */
  SettingsData s_data = { 100, 3, EqPreset::Vocal, (uint8_t)SleepTimeout::Min5 };

  const uint16_t FRAM_ADDR = 0x0000;
  const uint8_t  MAGIC[4]  = { 'M', 'P', 'L', '3' };
  const uint8_t  OLD_MAGIC[4] = { 'M', 'P', 'L', '2' };

  /* 存储布局: magic(4) + brightness + rotation + eq + sleepTimeout + checksum = 9 字节 */
  uint8_t checksum(const uint8_t* b, size_t n) {
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
  }
}

namespace Settings {

SettingsData& data() { return s_data; }

void load() {
  uint8_t buf[9];
  Fram::read(FRAM_ADDR, buf, sizeof(buf));

  bool valid = (memcmp(buf, MAGIC, 4) == 0) &&
               (checksum(buf, 8) == buf[8]);
  bool legacy = (memcmp(buf, OLD_MAGIC, 4) == 0) &&
                (checksum(buf, 8) == buf[8]);
  if (valid) {
    s_data.brightness = buf[4];
    s_data.rotation   = buf[5];
    s_data.eq         = (EqPreset)buf[6];
    s_data.sleepTimeout = buf[7];
  } else if (legacy) {
    s_data.brightness = buf[4];
    s_data.rotation   = buf[5];
    s_data.eq         = (EqPreset)buf[6];
    s_data.sleepTimeout = (uint8_t)SleepTimeout::Min5;
  }

  /* 合法性校验, 防止损坏数据导致无法操作 */
  if (s_data.brightness < BRIGHT_MIN || s_data.brightness > BRIGHT_MAX) s_data.brightness = 100;
  if (s_data.rotation != 1 && s_data.rotation != 3) s_data.rotation = 3;
  if ((int)s_data.eq >= (int)EqPreset::COUNT) s_data.eq = EqPreset::Vocal;
  if (s_data.sleepTimeout >= (uint8_t)SleepTimeout::COUNT) s_data.sleepTimeout = (uint8_t)SleepTimeout::Min5;
}

void save() {
  uint8_t buf[9];
  memcpy(buf, MAGIC, 4);
  buf[4] = s_data.brightness;
  buf[5] = s_data.rotation;
  buf[6] = (uint8_t)s_data.eq;
  buf[7] = s_data.sleepTimeout;
  buf[8] = checksum(buf, 8);
  Fram::write(FRAM_ADDR, buf, sizeof(buf));
}

uint32_t sleepTimeoutMs() {
  switch ((SleepTimeout)s_data.sleepTimeout) {
    case SleepTimeout::Min1:  return 60UL * 1000UL;
    case SleepTimeout::Min5:  return 5UL * 60UL * 1000UL;
    case SleepTimeout::Min10: return 10UL * 60UL * 1000UL;
    case SleepTimeout::Min30: return 30UL * 60UL * 1000UL;
    default: return 0;
  }
}

const char* sleepTimeoutLabel() {
  switch ((SleepTimeout)s_data.sleepTimeout) {
    case SleepTimeout::Min1:  return "1分钟";
    case SleepTimeout::Min5:  return "5分钟";
    case SleepTimeout::Min10: return "10分钟";
    case SleepTimeout::Min30: return "30分钟";
    default: return "关闭";
  }
}

}
