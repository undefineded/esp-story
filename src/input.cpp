#include "input.h"
#include "config.h"
#include <driver/gpio.h>

namespace {
  struct Btn { uint8_t pin; bool down; bool longSent; uint32_t edgeT; uint32_t downT; };
  /* 装机后屏幕为 180° 方向, 上下两个按键相对用户视角是颠倒的:
     故把 NEXT / LAST 的物理引脚对调, 使两键的"上/下"功能互换 */
  Btn s_next{ BTN_LAST, false, false, 0, 0 };
  Btn s_prev{ BTN_NEXT, false, false, 0, 0 };

  /* 25ms 消抖; 松开时判短按, 按住达 NAV_LONG_MS 判长按 (长按触发后抑制短按) */
  int8_t navEvent(Btn& b) {
    bool     cur = (digitalRead(b.pin) == LOW);
    uint32_t now = millis();

    if (cur != b.down && now - b.edgeT > 25) {
      b.edgeT = now;
      b.down  = cur;
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

  /* EC11 正交解码放在 GPIO 中断里:
     播放时 audio.loop() 会长时间占用主循环, 轮询会漏掉编码器的跳变,
     导致(尤其播放中)音量调不动。中断里只累加 1/4 步增量, 主循环取整。 */
  DRAM_ATTR const int8_t ENC_TBL[16] = { 0, -1, 1, 0,  1, 0, 0, -1,
                                        -1, 0, 0, 1,  0, 1, -1, 0 };
  volatile int32_t s_encAcc  = 0;
  volatile uint8_t s_encPrev = 0;

  void IRAM_ATTR encISR() {
    uint8_t cur = (uint8_t)((gpio_get_level((gpio_num_t)ENC_A) << 1) |
                             gpio_get_level((gpio_num_t)ENC_B));
    s_encAcc += ENC_TBL[(s_encPrev << 2) | cur];
    s_encPrev = cur;
  }
}

namespace Input {

void begin() {
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
}

int8_t nextEvent() { return navEvent(s_next); }
int8_t prevEvent() { return navEvent(s_prev); }

/* 编码器 SET: 按住达 ENC_LONG_MS 判为长按(切换模式), 否则释放时判为短按。
   长按触发后抑制短按, 避免切换模式时又触发播放/暂停。 */
int8_t encEvent() {
  static bool     down     = false;
  static bool     longSent = false;
  static uint32_t edgeT    = 0;
  static uint32_t downT    = 0;

  bool     cur = (digitalRead(ENC_SW) == LOW);
  uint32_t now = millis();

  if (cur != down && now - edgeT > 25) {      // 25ms 消抖
    edgeT = now;
    down  = cur;
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
int8_t okEvent() {
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

/* 取走中断累积的 1/4 步, 每 4 个 1/4 步(即一格)算 1 步
   装机后屏幕为 180° 方向, 旋钮的左右旋转相对用户视角颠倒, 故返回值取反 */
int encoderSteps() {
  noInterrupts();
  int32_t acc = s_encAcc;
  s_encAcc = 0;
  interrupts();

  static int32_t carry = 0;
  carry += acc;
  int steps = (int)(carry / 4);
  carry -= (int32_t)steps * 4;
  return -steps;
}

bool active() {
  if (digitalRead(BTN_OK) == LOW || digitalRead(BTN_NEXT) == LOW ||
      digitalRead(BTN_LAST) == LOW || digitalRead(ENC_SW) == LOW) return true;
  noInterrupts();
  bool moved = s_encAcc != 0;
  interrupts();
  return moved;
}

}
