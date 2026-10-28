#include "power.h"
#include "filexfer.h"
#include "config.h"
#include "settings.h"
#include "input.h"
#include "display.h"
#include "player.h"
#include "story.h"
#include "ble.h"
#include "hostlink.h"
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

namespace {

bool     s_screenOff  = false;
uint8_t  s_savedBright = 100;      // 熄屏前的亮度
uint32_t s_lastAct    = 0;         // 最近一次活动 (按键 / 旋钮)
uint32_t s_seenInput  = 0;         // 上次看到的 Input::lastActivityMs()
bool     s_sleeping   = false;     // 已决定休眠, 避免重复进入

/* 只能靠 RTC 引脚唤醒: 编码器 A/B/SET */
const int WAKE_PINS[] = { ENC_A, ENC_B, ENC_SW };

void screenOn() {
  if (!s_screenOff) return;
  s_screenOff = false;
  Display::setBrightness(s_savedBright);
  Input::swallowFor(600);            // 点亮屏幕那一下不触发按键功能
  Serial.println("[PWR] screen on");
}

void screenOffNow() {
  if (s_screenOff) return;
  s_screenOff    = true;
  s_savedBright  = Settings::data().brightness;
  Display::setBrightness(0);
  Serial.printf("[PWR] screen off (playing, idle %umin)\n",
                (unsigned)Settings::data().screenOffMin);
}

/* 进入深度休眠. 不返回. */
[[noreturn]] void deepSleepNow(uint32_t idleMs) {
  s_sleeping = true;
  Serial.printf("[PWR] deep sleep (no audio, idle %lus)\n", (unsigned long)(idleMs / 1000));

  /* 收尾: 停播放/停故事, 关背光, 刷串口 */
  Story::stop();
  Player::stop();
  Display::setBrightness(0);
  delay(50);
  Serial.flush();

  /* RTC 域保持供电, 否则休眠期间上拉会失效, 引脚浮空可能立刻唤醒 */
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  uint64_t mask = 0;
  for (int pin : WAKE_PINS) {
    gpio_num_t g = (gpio_num_t)pin;
    rtc_gpio_init(g);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(g);
    rtc_gpio_pulldown_dis(g);
    mask |= (1ULL << (uint32_t)pin);
  }
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
#ifdef POWER_TEST_TIMER_WAKE
  esp_sleep_enable_timer_wakeup(10ULL * 1000000ULL);   // 测试用: 10s 后自动醒
#endif
  esp_deep_sleep_start();
}

} // namespace

namespace Power {

void begin() {
  s_lastAct      = millis();
  s_seenInput    = Input::lastActivityMs();
  s_savedBright  = Settings::data().brightness;
  s_screenOff    = false;
  s_sleeping     = false;

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  if (cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
    Serial.printf("[PWR] wake from sleep, cause=%d\n", (int)cause);
  }
}

bool loop(bool audioActive) {
  if (s_sleeping) return false;
  if (FileXfer::busy()) {
    s_lastAct = millis();
    if (s_screenOff) screenOn();
    return false;
  }

  uint32_t now = millis();

  /* 任意按键/旋钮动作都算活动 */
  uint32_t act = Input::lastActivityMs();
  bool newInput = (act != s_seenInput);
  bool woke = false;
  if (newInput) {
    s_seenInput = act;
    s_lastAct   = now;
    if (s_screenOff) { screenOn(); woke = true; }   // 熄屏状态下有操作就立刻亮屏
  }

  uint32_t offMs   = Settings::screenOffMs();
  uint32_t sleepMs = Settings::sleepMs();
#ifdef POWER_FAST_TEST
  offMs /= 60; sleepMs /= 60;      // 测试用: 分钟 -> 秒
#endif
  uint32_t idle = now - s_lastAct;

  if (audioActive) {
    /* 播放中: 只熄屏, 音频继续 */
    if (!s_screenOff && offMs && idle >= offMs) screenOffNow();
  } else if (sleepMs && idle >= sleepMs &&
             !Ble::connected() && !HostLink::recentlyActive()) {
    /* 无播放: 到点休眠 (熄屏状态下同样生效, 否则熄屏后音频停了就永不休眠) */
    deepSleepNow(idle);
  }
  return woke;
}

bool screenOff() { return s_screenOff; }

} // namespace Power
