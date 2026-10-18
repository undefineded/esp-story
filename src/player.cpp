#include "player.h"
#include "storage.h"
#include "config.h"
#include <Audio.h>
#include <SD_MMC.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

/*  音频架构 (FreeRTOS)
 *
 *  ESP32-audioI2S 的解码器必须被持续调用才能喂满 I2S DMA, 否则会出 underrun
 *  (可闻的卡顿/爆音)。它又是非线程安全的。因此:
 *    - 解码独占一个高优先级任务 audioTask (core 0), 只有这个任务碰 s_audio;
 *    - UI/输入仍在 Arduino 主任务 loopTask (core 1), 整屏重绘不会再饿死解码;
 *    - UI -> 音频任务: 通过命令队列 s_q 下发 (播放/暂停/换曲/音量/EQ);
 *    - 音频任务 -> UI: 通过状态快照 s_snap 上报 (s_mux 保护)。
 */

namespace {

  Audio s_audio;

  QueueHandle_t     s_q    = nullptr;   // UI -> 音频任务: 命令队列
  SemaphoreHandle_t s_mux  = nullptr;   // 保护状态快照 / EOF 标志
  TaskHandle_t      s_task = nullptr;

  const uint32_t AUDIO_TASK_STACK = 8192;   // 字节
  const UBaseType_t AUDIO_TASK_PRIO = 2;    // > loopTask(1)
  const BaseType_t  AUDIO_TASK_CORE = 0;    // UI 跑在 core 1

  /* 播放状态: 只由音频任务写 */
  struct State {
    int         index      = -1;
    bool        connected  = false;   // 已连接文件 (暂停时仍为 true)
    bool        paused     = true;
    const char* codec      = nullptr;
    uint32_t    curSec     = 0;
    uint32_t    durSec     = 0;
    uint32_t    bitrate    = 0;       // kbps
    uint32_t    sampleRate = 0;
    uint8_t     bits       = 0;
    uint8_t     vol        = 4;       // 默认调低, 减小失真/噪音
    EqPreset    eq         = EqPreset::Vocal;
  };
  State s_st;                       // 任务本地状态
  State s_snap;                     // 发布给 UI 的快照

  bool      s_eof         = false;  // 播放结束标志 (可能来自中断上下文之外的回调)
  bool      s_eqDirty     = true;   // 仅任务: 采样率就绪后应用 EQ
  bool      s_autoNext    = true;   // 仅任务: 音乐模式结束自动下一曲
  bool      s_pendingNext = false;  // 仅任务: EOF 后延迟换曲
  EqPreset  s_eqBoot      = EqPreset::Vocal;  // begin() 之前的初始 EQ

  /* 命令 (UI -> 音频任务) */
  enum Cmd : uint8_t {
    CMD_PLAY_INDEX, CMD_PLAY_PATH, CMD_NEXT, CMD_PREV,
    CMD_TOGGLE, CMD_STOP, CMD_VOL_SET, CMD_VOL_ADJ,
    CMD_EQ_SET, CMD_EQ_CYCLE, CMD_AUTONEXT
  };
  struct Msg {
    uint8_t cmd;
    int32_t arg;
    uint8_t flag;          // CMD_PLAY_PATH: autoStart
    char    path[192];
  };

  void lock()   { if (s_mux) xSemaphoreTake(s_mux, portMAX_DELAY); }
  void unlock() { if (s_mux) xSemaphoreGive(s_mux); }

  void eofSet(bool v) { lock(); s_eof = v; unlock(); }

  void enqueue(uint8_t cmd, int32_t arg = 0, bool flag = false, const char* path = nullptr) {
    if (!s_q) return;
    Msg m{};
    m.cmd  = cmd;
    m.arg  = arg;
    m.flag = flag ? 1 : 0;
    if (path) strncpy(m.path, path, sizeof(m.path) - 1);
    xQueueSend(s_q, &m, 0);        // 队列满则丢弃, 用户操作远达不到容量
  }

  /* 把解码器最新信息发布给 UI (仅在音频任务内调用) */
  void publish() {
    s_st.codec      = s_audio.getCodecname();
    s_st.curSec     = s_audio.getAudioCurrentTime();
    s_st.durSec     = s_audio.getAudioFileDuration();
    s_st.bitrate    = s_audio.getBitRate() / 1000;
    s_st.sampleRate = s_audio.getSampleRate();
    s_st.bits       = s_audio.getBitsPerSample();

    lock();
    s_snap = s_st;
    unlock();
  }

  void playPathTask(const char* path, bool autoStart) {
    if (!path) return;

    s_audio.stopSong();
    Serial.printf("[PLAY] %s\n", path);
    eofSet(false);
    if (s_audio.connecttoFS(SD_MMC, path)) {
      s_audio.setVolume(s_st.vol);
      s_eqDirty      = true;
      s_st.connected = true;
      s_st.paused    = !autoStart;
      if (!autoStart) s_audio.pauseResume();
    } else {
      Serial.printf("[ERR ] cannot open %s\n", path);
      s_st.connected = false;
      s_st.paused    = true;
    }
  }

  void playIndexTask(int i, bool autoStart) {
    const char* p = Storage::path(i);
    if (!p) return;
    s_st.index = i;
    playPathTask(p, autoStart);
  }

  void execCmd(const Msg& m) {
    int n = Storage::count();
    switch (m.cmd) {
      case CMD_PLAY_INDEX: playIndexTask((int)m.arg, true); break;
      case CMD_PLAY_PATH:  playPathTask(m.path, m.flag != 0); break;
      case CMD_NEXT:       if (n) playIndexTask((s_st.index + 1) % n, true); break;
      case CMD_PREV:       if (n) playIndexTask((s_st.index - 1 + n) % n, true); break;

      case CMD_TOGGLE:
        if (!n) break;
        if (s_st.index < 0) s_st.index = 0;
        if (!s_st.connected) { playIndexTask(s_st.index, true); break; }
        /* 已连接: pauseResume 只切输出开关, 文件位置保留 -> 暂停/继续不丢进度 */
        s_audio.pauseResume();
        s_st.paused = !s_st.paused;
        break;

      case CMD_STOP:
        s_audio.stopSong();
        s_st.connected = false;
        s_st.paused    = true;
        eofSet(false);
        break;

      case CMD_VOL_SET:
        s_st.vol = (uint8_t)constrain((int)m.arg, 0, VOL_MAX);
        s_audio.setVolume(s_st.vol);
        break;
      case CMD_VOL_ADJ:
        s_st.vol = (uint8_t)constrain((int)s_st.vol + (int)m.arg, 0, VOL_MAX);
        s_audio.setVolume(s_st.vol);
        break;

      case CMD_EQ_SET:   s_st.eq = (EqPreset)m.arg; s_eqDirty = true; break;
      case CMD_EQ_CYCLE: s_st.eq = (EqPreset)(((int)s_st.eq + 1) % (int)EqPreset::COUNT); s_eqDirty = true; break;

      case CMD_AUTONEXT: s_autoNext = (m.flag != 0); eofSet(false); break;
    }
  }

  /* 音频任务主体: 处理命令 -> 解码 -> 应用 EQ -> 处理 EOF -> 发布状态 */
  void audioTask(void*) {
    s_audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);

    s_st.index  = (Storage::count() > 0) ? 0 : -1;   // 预选第一首, 不自动播放
    s_st.vol    = 4;
    s_st.eq     = s_eqBoot;
    s_st.paused = true;
    s_audio.setVolume(s_st.vol);

    for (;;) {
      Msg m{};
      if (xQueueReceive(s_q, &m, pdMS_TO_TICKS(1)) == pdTRUE) execCmd(m);

      s_audio.loop();

      /* EQ 的 IIR 系数依赖采样率, 就绪后再应用 */
      if (s_eqDirty && s_audio.isRunning() && s_audio.getSampleRate() > 1000) {
        EqBands b = eqBands(s_st.eq);
        s_audio.setTone(b.low, b.peak, b.high);
        s_eqDirty = false;
      }

      /* EOF 后延迟到此处换曲: 不在解码回调里重入 connecttoFS */
      if (s_pendingNext) {
        s_pendingNext = false;
        int n = Storage::count();
        if (n) playIndexTask((s_st.index + 1) % n, true);
      }

      publish();
    }
  }

} // namespace

namespace Player {

void begin() {
  s_mux = xSemaphoreCreateMutex();
  s_q   = xQueueCreate(8, sizeof(Msg));
  xTaskCreatePinnedToCore(audioTask, "audio", AUDIO_TASK_STACK, nullptr,
                          AUDIO_TASK_PRIO, &s_task, AUDIO_TASK_CORE);
}

void playIndex(int i, bool autoStart)          { enqueue(CMD_PLAY_INDEX, i, autoStart); }
void playPath(const char* path, bool autoStart){ enqueue(CMD_PLAY_PATH, 0, autoStart, path); }
void next()                                    { enqueue(CMD_NEXT); }
void prev()                                    { enqueue(CMD_PREV); }
void toggle()                                  { enqueue(CMD_TOGGLE); }
void stop()                                    { enqueue(CMD_STOP); }

void setAutoNext(bool on)                      { enqueue(CMD_AUTONEXT, 0, on); }

bool takeEof() {
  lock();
  bool f = s_eof;
  s_eof  = false;
  unlock();
  return f;
}

bool connected() { lock(); bool v = s_snap.connected; unlock(); return v; }
bool paused()    { lock(); bool v = s_snap.paused;    unlock(); return v; }

void setVolume(int v)     { enqueue(CMD_VOL_SET, v); }
void adjustVolume(int delta) { enqueue(CMD_VOL_ADJ, delta); }

void setEq(EqPreset p) { if (!s_q) { s_eqBoot = p; return; } enqueue(CMD_EQ_SET, (int)p); }
void cycleEq()         { enqueue(CMD_EQ_CYCLE); }

PlayerStatus status() {
  PlayerStatus st{};

  int cnt = Storage::count();
  st.count = cnt;

  lock();
  State s = s_snap;
  unlock();

  st.index    = s.index;
  st.hasTrack = (s.index >= 0 && cnt > 0);
  st.name     = st.hasTrack ? Storage::name(s.index) : "";
  st.playing  = s.connected;    // 已连接即视为"有曲目" (暂停时仍为 true)
  st.paused   = s.paused;
  st.codec    = s.codec;
  st.curSec   = s.curSec;
  st.durSec   = s.durSec;
  st.bitrateKbps = s.bitrate;
  st.sampleRate  = s.sampleRate;
  st.bits        = s.bits;
  st.volume      = s.vol;
  st.eq          = s.eq;
  return st;
}

} // namespace Player

/* ---- ESP32-audioI2S 弱符号回调 (在音频任务内被调用) ---- */

void audio_eof_mp3(const char* info) {
  (void)info;
  eofSet(true);
  if (s_autoNext) s_pendingNext = true;   // 音乐模式自动下一曲 / 故事模式仅置标志
}

/* 不要在这里刷串口: USB CDC 写入会阻塞 audio.loop() 引起 I2S 丢帧噪音 */
void audio_info(const char* info)  { (void)info; }
void audio_bitrate(const char* info) { (void)info; }
