/* 电源管理: 两条互相独立的节能策略
 *
 *   1) 正在播放音频  -> 空闲 screenOffMin 分钟后只关背光 (MCU 继续跑, 音乐不断)
 *   2) 正在播放视频  -> 既不休眠也不熄屏 (看的正起劲, 关屏等于停播)
 *   3) 没有播放 (暂停/停止/无曲目) -> 空闲 sleepMin 分钟后进深度休眠
 *
 * 深度休眠只能被 RTC 引脚唤醒, 本板可用的是编码器那三根:
 *   ENC_A=GPIO14 / ENC_B=GPIO15 / ENC_SW=GPIO21  (低电平唤醒)
 * 三个按键 OK/NEXT/LAST 在 GPIO47/48/39, 不是 RTC 引脚, 无法唤醒深度休眠。
 * 唤醒后是正常复位启动, 靠 FRAM 里的会话记忆回到上次那首 (暂停).
 *
 * 休眠会被"上位机正在通信"和"手机 BLE 已连接"阻止, 免得把人正在用的连接踢掉。
 */
#pragma once
#include <Arduino.h>

namespace Power {
  void begin();                 // 初始化空闲计时
  /* audioActive: 音频在播 (只熄屏); videoActive: 视频在播 (不熄屏也不休眠) */
  bool loop(bool audioActive, bool videoActive = false);  // 每轮主循环调用; 返回 true = 刚刚从熄屏被唤醒
  bool screenOff();             // 当前是否处于"仅熄屏"状态
}
