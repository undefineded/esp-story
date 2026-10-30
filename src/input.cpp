#include "input.h"
#include "config.h"
#include <driver/gpio.h>
#include <initializer_list>
#ifndef ARDUINO
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
namespace {
uint32_t millis() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
int digitalRead(int pin) { return gpio_get_level(static_cast<gpio_num_t>(pin)); }
constexpr int LOW = 0;
}
#endif

namespace {
  struct Btn { uint8_t pin; bool down; bool longSent; uint32_t edgeT; uint32_t downT; };
  /* 装机后屏幕为 180° 方向, 上下两个按键相对用户视角是颠倒的:
     故把 NEXT / LAST 的物理引脚对调, 使两键的"上/下"功能互换 */
  Btn s_next{ BTN_LAST, false, false, 0, 0 };
  Btn s_prev{ BTN_NEXT, false, false, 0, 0 };

  bool     s_locked  = false;
  uint32_t s_unlockAt = 0;
  volatile uint32_t s_activityMs = 0;   // 最近一次按键/旋钮动作 (电源管理用来判空闲)
  uint32_t s_swallowUntil = 0;          // 唤醒后的输入屏蔽窗口 (毫秒时间戳)

  inline void touch() { s_activityMs = millis(); }

  /* 锁定期间及解锁后 500ms 内屏蔽事件, 防止按住按键解锁时产生误动作,
     也覆盖 OK 单击等待双击的 350ms 窗口. 唤醒后也短暂屏蔽, 避免点亮屏幕那一下
     顺带触发按键功能. */
  bool inputsSuppressed() {
    return s_locked
        || (s_unlockAt && (uint32_t)(millis() - s_unlockAt) < 500)
        || (int32_t)(s_swallowUntil - millis()) > 0;
  }

  /* 25ms 消抖; 松开时判短按, 按住达 NAV_LONG_MS 判长按 (长按触发后抑制短按) */
  int8_t navEventRaw(Btn& b) {
    bool     cur = (digitalRead(b.pin) == LOW);
    uint32_t now = millis();

    if (cur != b.down && now - b.edgeT > 25) {
      b.edgeT = now;
      b.down  = cur;
      s_activityMs = now;                       // 任何边沿都算"有人在操作"
      if (cur) {
        b.downT    = now;
        b.longSent = false;
      } else if (!b.longSent) {
        return 1;                               // 短按(松开)
      }
    }
    if (b.down && !b.longSent && now - b.downT >= NAV_LONG_MS) {
      b.longSent = true;
      return 2;                                 // 按住达阈值 -> 长按
    }
    return 0;
  }

  int8_t navEvent(Btn& b) {
    int8_t e = navEventRaw(b);
    return inputsSuppressed() ? 0 : e;
  }

  /* EC11 正交解码放在 GPIO 中断里:
     播放时 audio.loop() 会长时间占用主循环, 轮询会漏掉编码器的跳变,
     导致(尤其播放中)音量调不动。中断里只累加 1/4 步增量, 主循环取整。 */
  DRAM_ATTR const int8_t ENC_TBL[16] = { 0, -1, 1, 0,  1, 0, 0, -1,
                                        -1, 0, 0, 1,  0, 1, -1, 0 };
  volatile int32_t s_encAcc  = 0;
  volatile uint8_t s_encPrev = 0;
  portMUX_TYPE s_encoderMux = portMUX_INITIALIZER_UNLOCKED;

  void IRAM_ATTR encISR() {
    portENTER_CRITICAL_ISR(&s_encoderMux);
    uint8_t cur = (uint8_t)((gpio_get_level((gpio_num_t)ENC_A) << 1) |
                             gpio_get_level((gpio_num_t)ENC_B));
    s_encAcc += ENC_TBL[(s_encPrev << 2) | cur];
    s_encPrev = cur;
    portEXIT_CRITICAL_ISR(&s_encoderMux);
  }
}

namespace Input {

void begin() {
#ifdef ARDUINO
  pinMode(BTN_OK,   INPUT_PULLUP);
  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_LAST, INPUT_PULLUP);
  pinMode(ENC_A,    INPUT_PULLUP);
  pinMode(ENC_B,    INPUT_PULLUP);
  pinMode(ENC_SW,   INPUT_PULLUP);

  s_encPrev = (uint8_t)((digitalRead(ENC_A) << 1) | digitalRead(ENC_B));
  s_encAcc  = 0;
  attachInterrupt(digitalPinToInterrupt(ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encISR, CHANGE);
#else
  gpio_config_t config{};
  config.pin_bit_mask = (1ULL << BTN_OK) | (1ULL << BTN_NEXT) | (1ULL << BTN_LAST)
                     | (1ULL << ENC_A) | (1ULL << ENC_B) | (1ULL << ENC_SW);
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK(gpio_config(&config));
  s_encPrev = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  auto result = gpio_install_isr_service(0);
  if (result != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(result);
  for (auto pin : {ENC_A, ENC_B}) {
    ESP_ERROR_CHECK(gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_ANYEDGE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(static_cast<gpio_num_t>(pin),
        [](void*) { encISR(); }, nullptr));
  }
#endif
}

int8_t nextEvent() { return navEvent(s_next); }
int8_t prevEvent() { return navEvent(s_prev); }

/* 编码器 SET: 按住达 ENC_LONG_MS 判为长按(切换模式), 否则释放时判为短按。
   长按触发后抑制短按, 避免切换模式时又触发播放/暂停。 */
int8_t encEventRaw() {
  static bool     down     = false;
  static bool     longSent = false;
  static uint32_t edgeT    = 0;
  static uint32_t downT    = 0;

  bool     cur = (digitalRead(ENC_SW) == LOW);
  uint32_t now = millis();

  if (cur != down && now - edgeT > 25) {      // 25ms 消抖
    edgeT = now;
    down  = cur;
    s_activityMs = now;                       // 任何边沿都算"有人在操作"
    if (cur) {
      downT    = now;
      longSent = false;
    } else if (!longSent) {
      return 1;                               // 未触发长按 -> 短按
    }
  }
  if (down && !longSent && now - downT >= ENC_LONG_MS) {
    longSent = true;
    return 2;                                 // 按住达阈值 -> 长按
  }
  return 0;
}

/* 单击延迟 350ms 以区分双击; 30ms 消抖; 按住达 OK_LONG_MS 判为长按。
   长按触发后抑制单击/双击, 松开也不再产生单击。 */
int8_t okEventRaw() {
  static bool     lastLevel = false;
  static uint32_t lastEdgeT = 0;
  static uint32_t downT     = 0;      // 本次按下时刻
  static uint32_t lastUpT   = 0;      // 上次松开时刻
  static bool     longSent  = false;  // 本次按压已判定为长按
  static bool     pending   = false;  // 有一个待确认的单击(等待看是否双击)

  bool     pressed = (digitalRead(BTN_OK) == LOW);
  uint32_t now     = millis();

  if (pressed != lastLevel && now - lastEdgeT > 30) {   // 30ms 消抖
    lastEdgeT = now;
    lastLevel = pressed;
    s_activityMs = now;                               // 任何边沿都算"有人在操作"
    if (pressed) {
      downT    = now;
      longSent = false;
      if (pending && now - lastUpT < 350) {   // 距上次松开 <350ms -> 第二击(双击)
        pending = false;
        return 2;
      }
      pending = true;
    } else {
      lastUpT = now;
      if (longSent) pending = false;          // 长按后松开: 丢弃单击
    }
  }

  if (lastLevel && !longSent && (int32_t)(now - downT) >= OK_LONG_MS) {
    longSent = true;
    pending  = false;
    return 3;                                 // 按住达阈值 -> 长按
  }
  if (pending && !lastLevel && now - lastUpT >= 350) {   // 松开且过双击窗口 -> 单击
    pending = false;
    return 1;
  }
  return 0;
}

int8_t encEvent() {
  int8_t e = encEventRaw();
  return inputsSuppressed() ? 0 : e;
}

int8_t okEvent() {
  int8_t e = okEventRaw();
  return inputsSuppressed() ? 0 : e;
}

/* 取走中断累积的 1/4 步, 每 4 个 1/4 步(即一格)算 1 步
   装机后屏幕为 180° 方向, 旋钮的左右旋转相对用户视角颠倒, 故返回值取反 */
int encoderSteps() {
  portENTER_CRITICAL(&s_encoderMux);
  int32_t acc = s_encAcc;
  s_encAcc = 0;
  portEXIT_CRITICAL(&s_encoderMux);

  static int32_t carry = 0;
  if (inputsSuppressed()) {
    carry = 0;                                // 童锁期间丢弃旋钮步数
    return 0;
  }
  carry += acc;
  int steps = (int)(carry / 4);
  carry -= (int32_t)steps * 4;
  if (steps) s_activityMs = millis();         // 转动旋钮也算操作
  return -steps;
}

uint32_t lastActivityMs() { return s_activityMs; }

void swallowFor(uint32_t ms) { s_swallowUntil = millis() + ms; }

void setLocked(bool locked) {
  if (locked == s_locked) return;
  s_locked = locked;
  if (locked) {
    s_unlockAt = 0;
    portENTER_CRITICAL(&s_encoderMux);
    s_encAcc = 0;
    portEXIT_CRITICAL(&s_encoderMux);
  } else {
    s_unlockAt = millis();
  }
#ifdef ARDUINO
  Serial.printf("[INPUT] %s\n", locked ? "child lock" : "unlock");
#else
  ESP_LOGI("input", "%s", locked ? "child lock" : "unlock");
#endif
}

/* 童锁本地解锁: OK + NEXT 同时按住 5 秒。用原始电平读取, 不受事件屏蔽影响。 */
bool childUnlockHeld() {
  static bool     comboDown = false;
  static bool     fired     = false;
  static uint32_t comboAt   = 0;

  if (!s_locked) {
    comboDown = false;
    fired     = false;
    return false;
  }

  bool both = (digitalRead(BTN_OK) == LOW) && (digitalRead(s_next.pin) == LOW);
  uint32_t now = millis();
  if (!both) {
    comboDown = false;
    fired     = false;
    return false;
  }
  if (!comboDown) {
    comboDown = true;
    comboAt   = now;
    fired     = false;
    return false;
  }
  if (!fired && (uint32_t)(now - comboAt) >= CHILD_UNLOCK_MS) {
    fired = true;
    return true;
  }
  return false;
}

}
