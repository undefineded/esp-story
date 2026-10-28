/* 家长控制: 本地策略、使用时长、音量范围、内容白名单、锁定与绑定
 *
 * 设计原则:
 *   - 所有限制由设备执行, 手机只负责设置和查看;
 *   - 配置与今日用量持久化到 I2C FRAM (0x0300 起), 断电/断连不丢失;
 *   - 无 RTC, 手机校时后按 epoch + millis 推算; 未校时保守累计, 不自动跨日;
 *   - 音量范围 clamp 到 [volMin, volMax].
 */
#pragma once
#include <Arduino.h>

namespace Parental {

enum class Lock : uint8_t { None = 0, DailyLimit = 1, Bedtime = 2, Remote = 3 };

void begin();                       // 加载 FRAM, 首次启动生成绑定码

/* ---- 配置 ---- */
uint16_t dailyLimitMin();           // 0 = 不限
uint8_t  volMin();
uint8_t  volMax();
bool     bedtimeEnabled();
uint16_t bedtimeStartMin();         // 距 00:00 的分钟数
uint16_t bedtimeEndMin();
bool     allowlistEnabled();
bool     remoteLocked();
bool     childLock();
uint64_t storyMask();
uint64_t catMask();

void setDailyLimitMin(uint16_t minutes);
void setVolumeRange(uint8_t lo, uint8_t hi);
void setBedtime(bool enabled, uint16_t startMin, uint16_t endMin);
void setAllowlistEnabled(bool enabled);
void setStoryMask(uint64_t mask);
void setCatMask(uint64_t mask);
void setRemoteLocked(bool locked);
void setChildLock(bool locked);       // 童锁: 只锁实体按键/旋钮, 不阻止 BLE 控制

uint32_t revision();                // 配置版本, 供 Storage/Story 重建过滤视图
void save();
void factoryReset();                // 恢复默认并清空绑定/PIN/额度/白名单
bool unbind();                      // 只解除绑定, 保留绑定码/家长配置

/* ---- 时间 ---- */
void     syncTime(uint32_t epochSec, int16_t tzOffsetMin);
bool     timeSynced();
uint32_t nowEpoch();
uint32_t dayKey();
uint16_t minuteOfDay();

/* ---- 使用时长 ---- */
void     tick(bool playing);        // 主循环每次调用, 内部按毫秒累计
uint32_t usedTodaySec();
uint32_t remainingSec();
uint8_t  historyCount();
uint32_t historyDay(uint8_t i);
uint32_t historyUsed(uint8_t i);

/* ---- 锁定 ---- */
Lock lock();
bool canPlay();

/* ---- 音量 ---- */
int clampVolume(int v);

/* ---- 内容 ---- */
bool storyAllowed(int index);       // 实际故事下标
bool categoryAllowed(int index);    // 实际分类下标 (0 = 全部歌曲)

/* ---- 绑定 / 鉴权 ---- */
bool        bound();
const char* bindCode();             // 6 位数字
bool        bind(const char* code, char* tokenOut, size_t tokenLen);
bool        verifyToken(const char* token);
bool        hasPin();
bool        setPin(const char* pin);
bool        verifyPin(const char* pin);

} // namespace Parental
