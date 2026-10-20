/*
 *  ESP32-S3 音乐播放器 / 故事机
 *
 *  硬件（全部来自 Netlist_Schematic1_2026-10-08.enet）:
 *    MCU     : ESP32-S3-WROOM-1U-N16R8
 *    功放    : MAX98357A  (I2S, BCLK/LRCLK/DIN)
 *    TF 卡   : 1-bit SDMMC (CLK/CMD/D0)
 *    屏幕    : ST7789 240x320 SPI (LovyanGFX, BL 经 Q1 支持 PWM 调光)
 *    霍尔    : U9 HAL251SO -> IO40 (调试显示)
 *    按键    : OK / NEXT / LAST + EC11 编码器
 *
 *  组件:
 *    Display  - LovyanGFX ST7789 驱动 + 背光 PWM
 *    Storage  - TF 卡挂载与 /music 扫描
 *    Player   - ESP32-audioI2S 封装 + 人声 EQ 预设
 *    Story    - /story 故事扫描 + 时间轴插画同步
 *    Input    - 按键 / 编码器 (SET 支持长按)
 *    Hall     - 霍尔开关状态
 *    Fram     - I2C 铁电存储器 (U2 MB85RC16, 配置持久化)
 *    Settings - 亮度/旋转/EQ 配置 (掉电保存到 FRAM)
 *    Session  - 上次的分类/曲目/音量记忆 (掉电保存到 FRAM)
 *    Ui       - 自绘主界面 + 设置界面 + 故事模式界面
 *
 *  两种模式 (长按编码器 SET 切换):
 *    音乐模式 - /music (一级子文件夹为分类, 置顶"全部歌曲"递归收录全部 mp3)
 *    故事模式 - /story/<故事名>/ (mp3 + 插画 + timeline.lrc)
 *
 *  音乐分类列表操作 (开机默认界面):
 *    编码器/NEXT/LAST: 选择分类        OK 单击: 播放该分类
 *    SET 短按 : 播放当前分类           SET 长按: 进入故事模式
 *  开机记忆: 有上次收听记录时直接回到播放页, 曲目已载入但暂停, 连同音量一起恢复,
 *            按 OK 即接着听; 记录失效(换卡/删歌)则照常进分类列表。
 *  音乐播放操作:
 *    OK 单击  : 播放 / 暂停           OK 双击 : 进入设置
 *    NEXT/LAST: 下一曲 / 上一曲
 *    NEXT 长按: 当前歌曲列表          LAST 长按 / OK 长按: 分类列表
 *    编码器   : 音量, 短按=播放/暂停
 *    SET 长按 : 进入故事模式
 *  当前歌曲列表操作:
 *    编码器/NEXT/LAST: 选择曲目       OK 单击 / SET 短按: 播放选中曲目
 *    长按     : 返回播放界面          SET 长按: 进入故事模式
 *  设置界面操作:
 *    编码器   : 调整当前项             NEXT/LAST: 上下选择
 *    OK 单击  : 递增当前项             OK 双击 : 保存并返回
 *  故事列表操作:
 *    编码器/NEXT/LAST: 选择故事        OK 单击 / SET 短按: 播放
 *    SET 长按 : 回音乐模式
 *  故事播放操作:
 *    OK 单击 / SET 短按: 暂停/继续     编码器  : 音量
 *    OK 双击  : 进入设置               OK 长按 : 停止并返回列表
 *    SET 长按 : 回音乐模式
 */

#include <Arduino.h>
#include "config.h"
#include "display.h"
#include "storage.h"
#include "player.h"
#include "story.h"
#include "input.h"
#include "hall.h"
#include "fram.h"
#include "settings.h"
#include "session.h"
#include "fonts.h"
#include "ui.h"
#include "parental.h"
#include "ble.h"
#include "network.h"

/* ---------------- 界面状态机 ---------------- */

enum class Screen : uint8_t { MusicList, TrackList, Main, Settings, StoryList, StoryPlay, StoryError, NoContent, Locked };

static Screen s_screen   = Screen::MusicList;
static Screen s_settingsBack = Screen::Main;   // 设置界面来源屏 (保存后返回该屏)
static Screen s_lockedBack   = Screen::MusicList; // 锁定前所在屏 (解锁后返回)
static int    s_sel      = 0;      // 设置界面当前选中项 0..3
static int    s_musicSel = 0;      // 音乐分类列表当前选中项
static int    s_trackSel = 0;      // 当前歌曲列表选中项
static int    s_trackPlaying = 0;  // 当前歌曲列表中"正在播放"的曲目下标
static int    s_storySel = 0;      // 故事列表当前选中项
static bool   s_storyRedraw = false;  // 从设置返回故事页: 置位以强制整体重绘
static Parental::Lock s_lockShown = Parental::Lock::None;

static const int SETTINGS_ITEMS = 7;   // 亮度 / 旋转 / EQ / 刷新列表 / 蓝牙 / WiFi / AP
static void showMusicList();
static void showTrackList();
static void showStoryList();
static void showNoContent();
static void applyParentalConfig();

static void applySettings() {
  SettingsData& s = Settings::data();
  Display::setBrightness(s.brightness);
  Display::setRotation(s.rotation);
  Player::setEq(s.eq);
}

/* ---------------- 会话记忆 (FRAM) ---------------- */

static void applyContentFilters() {
  Storage::setCategoryFilter(Parental::catMask(), Parental::allowlistEnabled());
  Story::setStoryFilter(Parental::storyMask(), Parental::allowlistEnabled());
}

static void setVolumeLimited(int v) {
  int cv = Parental::clampVolume(v);
  Player::setVolume(cv);
  Session::setVolume((uint8_t)cv);
}

static bool categoryVisible(int actual) {
  for (int i = 0; i < Storage::viewCatCount(); i++) {
    if (Storage::viewCatActual(i) == actual) return true;
  }
  return false;
}

static void applyParentalConfig() {
  applyContentFilters();

  int v = Parental::clampVolume((int)Session::volume());
  if (v != (int)Session::volume()) Session::setVolume((uint8_t)v);
  Player::setVolume(v);

  if (Parental::allowlistEnabled()) {
    int ci = Storage::currentCategory();
    if (!categoryVisible(ci) && s_screen != Screen::Locked) {
      if (Story::active()) {
        Story::stop();
        showStoryList();
      } else {
        Player::stop();
        showMusicList();
      }
    }
    int si = Story::currentIndex();
    if (si >= 0 && !Parental::storyAllowed(si)) {
      Story::stop();
      showStoryList();
    }
  }

  /* 白名单变化后可见列表可能整体改变, 当前列表页需要重绘 */
  if (s_screen == Screen::MusicList) {
    showMusicList();
  } else if (s_screen == Screen::StoryList || s_screen == Screen::StoryError) {
    showStoryList();
  }
}

/* 音量: Session 是唯一真值源 (开机从 FRAM 恢复), 用绝对值下发避免与异步状态漂移 */
static void changeVolume(int delta) {
  uint8_t v = (uint8_t)constrain((int)Session::volume() + delta, 0, VOL_MAX);
  v = (uint8_t)Parental::clampVolume(v);
  Player::setVolume(v);
  Session::setVolume(v);            // 变化时落盘 (3 字节小记录, 拧旋钮也不拖慢 UI)
}

/* 把"当前分类 + 当前曲目"写进 FRAM; 内容未变时 Session 内部直接跳过 */
static void rememberPosition() {
  PlayerStatus st = Player::status();
  int ci = Storage::currentCategory();
  const char* cat  = (ci > 0) ? Storage::catName(ci) : "";   // 0 = 全部歌曲, 存空串
  const char* path = (st.index >= 0) ? Storage::path(st.index) : "";
  Session::setPosition(cat, path, st.index);
}

static void enterSettings() {
  s_settingsBack = s_screen;        // 记录来源屏, 保存后返回该屏
  s_screen = Screen::Settings;
  s_sel    = 0;
  Ui::drawSettings(Settings::data(), s_sel);
  Ui::settingsMessage("双击OK保存返回  刷新会停止播放");
}

static void exitSettings() {
  Settings::save();                 // 保存到 FRAM
  if (s_settingsBack == Screen::StoryPlay) {
    s_screen      = Screen::StoryPlay;   // 返回故事播放页
    s_storyRedraw = true;                // 插画+信息条由下一帧整体重绘
  } else if (s_settingsBack == Screen::MusicList) {
    showMusicList();
  } else if (s_settingsBack == Screen::TrackList) {
    showTrackList();
  } else if (s_settingsBack == Screen::StoryList || s_settingsBack == Screen::StoryError) {
    showStoryList();
  } else if (s_settingsBack == Screen::NoContent) {
    showNoContent();
  } else {
    s_screen = Screen::Main;
    Ui::drawStatic();
    Ui::update(Player::status(), Hall::detected());
  }
}

/* 调整设置项: 0=亮度, 1=旋转, 2=EQ; 4..6 为开关 (旋钮向上开/向下关) */
static void adjustSetting(int item, int delta) {
  SettingsData& s = Settings::data();

  if (item == 0) {
    int v = (int)s.brightness + delta * 5;
    s.brightness = (uint8_t)constrain(v, BRIGHT_MIN, BRIGHT_MAX);
    Display::setBrightness(s.brightness);
    Ui::refreshSettings(s, item);
  } else if (item == 1) {
    s.rotation = (s.rotation == 1) ? 3 : 1;
    Display::setRotation(s.rotation);
    Ui::drawSettings(s, item);      // 旋转改变了显示方向, 需整屏重绘
  } else if (item == 2) {
    int e = ((int)s.eq + (delta >= 0 ? 1 : -1) + (int)EqPreset::COUNT) % (int)EqPreset::COUNT;
    s.eq = (EqPreset)e;
    Player::setEq(s.eq);
    Ui::refreshSettings(s, item);
  } else if (item >= 4) {
    uint8_t on = (delta > 0) ? 1 : 0;
    if (item == 4) {
      s.bleEnabled = on;
      Ble::setAdvertisingEnabled(on != 0);
    } else if (item == 5) {
      s.wifiEnabled = on;
      Network::apply(s.wifiEnabled != 0, s.apEnabled != 0);
    } else if (item == 6) {
      s.apEnabled = on;
      Network::apply(s.wifiEnabled != 0, s.apEnabled != 0);
    }
    Settings::save();               // 无线开关立即落盘
    Ui::refreshSettings(s, item);
  }
}

static void toggleSetting(int item) {
  SettingsData& s = Settings::data();
  if (item == 4) {
    s.bleEnabled = s.bleEnabled ? 0 : 1;
    Ble::setAdvertisingEnabled(s.bleEnabled != 0);
  } else if (item == 5) {
    s.wifiEnabled = s.wifiEnabled ? 0 : 1;
    Network::apply(s.wifiEnabled != 0, s.apEnabled != 0);
  } else if (item == 6) {
    s.apEnabled = s.apEnabled ? 0 : 1;
    Network::apply(s.wifiEnabled != 0, s.apEnabled != 0);
  }
  Settings::save();
  Ui::refreshSettings(s, item);
}

static void refreshCatalog() {
  Ui::boot("正在刷新列表", "将停止播放，请勿拔卡");
  Display::present(true);
  Story::stop();
  // STOP acknowledgement also drains earlier playback commands. Do not mutate
  // Storage's String arrays while the audio task may still hold their pointers.
  if (!Player::stopAndWait()) {
    s_settingsBack = Screen::MusicList;
    Ui::drawSettings(Settings::data(), s_sel);
    Ui::settingsMessage("停止播放失败，请重试");
    return;
  }
  auto result = Catalog::refresh();
  applyContentFilters();
  s_musicSel = s_trackSel = s_storySel = 0;
  s_trackPlaying = -1;
  s_settingsBack = Screen::MusicList;
  Input::encoderSteps(); // 丢弃扫描期间积累的旋钮步数
  Ui::drawSettings(Settings::data(), s_sel);
  if (result == Catalog::Result::Saved) {
    if (Storage::totalCount() >= MAX_TRACKS || Story::count() >= MAX_STORIES ||
        Storage::catCount() - 1 >= MAX_CATEGORIES) {
      Ui::settingsMessage("已保存，列表数量已达上限");
      return;
    }
    char message[80];
    snprintf(message, sizeof(message), "已保存：音乐%d 故事%d", Storage::totalCount(), Story::count());
    Ui::settingsMessage(message);
  } else if (result == Catalog::Result::SaveFailed) {
    Ui::settingsMessage("列表已更新，索引保存失败");
  } else {
    Ui::settingsMessage("扫描失败，请检查SD卡");
  }
}

/* ---------------- 音乐分类 ---------------- */

static void showNoContent() {
  s_screen = Screen::NoContent;
  Ui::drawNoContent();
}

/* 绘制音乐分类列表 (置顶"全部歌曲" + /music 一级子文件夹, 受家长白名单过滤) */
static void showMusicList() {
  int n = Storage::viewCatCount();
  if (n == 0) {
    if (Story::viewCount() > 0) showStoryList();
    else                        showNoContent();
    return;
  }
  if (s_musicSel >= n) s_musicSel = 0;
  if (s_musicSel < 0) s_musicSel = 0;
  s_screen = Screen::MusicList;
  Ui::drawMusicList(n, s_musicSel);
}

/* 播放当前分类中的第 i 首, 并回到播放界面 */
static void playMusicIndex(int i) {
  if (!Parental::canPlay()) return;
  s_trackSel = i;
  Player::playIndex(i, true);
  s_screen = Screen::Main;
  Ui::drawStatic();
  Ui::update(Player::status(), Hall::detected());
}

/* 绘制当前分类的歌曲列表 (正在播放的曲目高亮), 无曲目则回分类列表 */
static void showTrackList() {
  int n = Storage::count();
  if (n == 0) { showMusicList(); return; }
  PlayerStatus st = Player::status();
  s_trackPlaying = (st.index >= 0 && st.index < n) ? st.index : 0;
  s_trackSel     = s_trackPlaying;
  s_screen = Screen::TrackList;
  Ui::drawTrackList(n, s_trackSel, s_trackPlaying);
}

/* 选定可见分类 (0=全部歌曲) 并开始播放该分类第一首 */
static void startMusicCategory(int viewCi) {
  int ci = Storage::viewCatActual(viewCi);
  if (ci < 0 || !categoryVisible(ci)) return;
  if (!Storage::selectCategory(ci)) {          // 该分类无曲目: 停留列表
    Serial.printf("[MUS] category %d has no track\n", ci);
    return;
  }
  s_musicSel = viewCi;
  playMusicIndex(0);
}

/* ---------------- 开机恢复上次收听位置 ---------------- */

/* 按 FRAM 记忆还原分类与曲目: 分类按名称、曲目按完整路径匹配 (换卡/增删歌后下标会错位)。
   成功则停在播放页并把该曲目载入为"暂停"状态, 按 OK 才出声; 返回 false 表示应回分类列表。 */
static bool restoreSession() {
  const char* cat = Session::category();
  int view = -1;
  int ncat = Storage::viewCatCount();
  for (int i = 0; i < ncat; i++) {
    int actual = Storage::viewCatActual(i);
    if (cat[0]) {
      if (strcmp(Storage::viewCatName(i), cat) == 0) { view = i; break; }
    } else if (actual == 0) {
      view = i; break;
    }
  }
  if (view < 0) return false;                    // 记住的分类已不存在或被家长关闭

  int ci = Storage::viewCatActual(view);
  if (!categoryVisible(ci) || !Storage::selectCategory(ci)) return false;

  s_musicSel = view;                             // 列表光标也停在记住的分类上

  int n  = Storage::count();
  int ti = -1;
  const char* path = Session::trackPath();
  if (path[0]) {
    for (int i = 0; i < n; i++) {
      if (strcmp(Storage::path(i), path) == 0) { ti = i; break; }
    }
  }
  if (ti < 0) {                                  // 路径没命中: 退回下标, 再不行就不恢复曲目
    int idx = Session::trackIndex();
    if (idx >= 0 && idx < n) ti = idx;
  }
  if (ti < 0) return false;                      // 只记住了分类, 由调用方显示分类列表

  s_trackSel     = ti;
  s_trackPlaying = ti;
  Player::playIndex(ti, false);                  // 载入但不自动播放
  Serial.printf("[SES] resume cat=\"%s\" #%d %s\n", cat, ti, Storage::path(ti));
  s_screen = Screen::Main;
  Ui::drawStatic();
  Ui::update(Player::status(), Hall::detected());
  return true;
}

/* ---------------- 故事模式 ---------------- */

static int s_storyPanel = -1;            // 已绘制的插画下标

/* 控制条刷新: Ui 内部按变化区域增量重绘(含故事名跑马灯), 每帧调用开销极小 */
static void refreshStoryBar() {
  PlayerStatus st = Player::status();
  Ui::drawStoryBar(Story::panelIndex(), Story::panelCount(),
                   st.curSec, st.durSec, st.volume, st.paused);
}

/* 绘制故事列表 (受家长白名单过滤); 一个故事都没有时回音乐或提示页 */
static void showStoryList() {
  int n = Story::viewCount();
  if (n == 0) {
    if (Storage::viewCatCount() > 0) showMusicList();
    else                             showNoContent();
    return;
  }
  if (s_storySel >= n) s_storySel = 0;
  if (s_storySel < 0) s_storySel = 0;
  s_screen = Screen::StoryList;
  Ui::drawStoryList(n, s_storySel);
}

static void enterStoryMode() {
  Story::stop();                    // 切换模式: 停掉音乐/上一个故事
  s_storySel = 0;
  showStoryList();
}

static void exitStoryMode() {
  Story::stop();
  showMusicList();                  // 回音乐分类列表
}

static void startStory(int view) {
  int actual = Story::viewActual(view);
  if (actual < 0 || !Parental::storyAllowed(actual) || !Parental::canPlay()) return;

  /* 按下确定立刻进入加载画面; 真实的目录/时间轴读取就在加载页显示期间完成
     (此时列表页没有音频播放, 阻塞是安全的) */
  String name = Story::title(actual);
  Ui::setStoryTitle(name.c_str());          // 控制条显示的故事名
  Ui::drawStoryLoadingBegin(name.c_str());
  Display::present(true);          // 在同步读取目录前提交加载页
  if (!Story::open(actual)) {
    s_screen = Screen::StoryError;
    Ui::boot("故事文件不可用", "双击OK进入设置刷新列表");
    return;
  }

  Story::start();
  s_storyPanel = Story::panelIndex();
  s_screen     = Screen::StoryPlay;
  Ui::drawStoryFrame(Story::panelPath());
  Ui::invalidateStoryBar();          // 插画覆盖了控制条区, 需整体重铺
  refreshStoryBar();
}

/* BLE 下发的是实际故事下标: 找到它在当前白名单视图中的位置 */
static void startStoryActual(int actual) {
  for (int i = 0; i < Story::viewCount(); i++) {
    if (Story::viewActual(i) == actual) { startStory(i); return; }
  }
}

/* BLE 下发的播放控制: 在 main loop 里执行, 保证 UI/Player 访问在同一任务 */
static void handleBleCommand(const Ble::Command& c) {
  switch (c.action) {
    case Ble::Action::Volume:
      setVolumeLimited(c.value);
      break;

    case Ble::Action::Play:
      if (!Parental::canPlay()) break;
      if (Player::paused()) Player::toggle();
      break;

    case Ble::Action::Pause:
      if (!Player::paused()) Player::toggle();
      break;

    case Ble::Action::Toggle:
      if (Parental::canPlay()) Player::toggle();
      break;

    case Ble::Action::Next:
    case Ble::Action::Prev: {
      if (!Parental::canPlay()) break;
      if (Story::active()) {
        int n = Story::viewCount();
        if (n <= 0) break;
        int cur = Story::currentIndex();
        int pos = 0;
        for (int i = 0; i < n; i++) if (Story::viewActual(i) == cur) { pos = i; break; }
        int next = (c.action == Ble::Action::Next) ? (pos + 1) % n : (pos - 1 + n) % n;
        startStory(next);
      } else if (c.action == Ble::Action::Next) {
        Player::next();
      } else {
        Player::prev();
      }
      break;
    }

    case Ble::Action::Stop:
      if (Story::active()) {
        Story::stop();
        showStoryList();
      } else {
        Player::stop();
        showMusicList();
      }
      break;

    case Ble::Action::PlayStory:
      startStoryActual(c.value);
      break;

    default:
      break;
  }
}

static const char* lockTitle(Parental::Lock lk) {
  switch (lk) {
    case Parental::Lock::DailyLimit: return "今日故事时间已用完";
    case Parental::Lock::Bedtime:    return "该睡觉啦";
    case Parental::Lock::Remote:     return "家长已暂停";
    default:                         return "家长控制中";
  }
}

static void armLock(Parental::Lock lk) {
  if (s_screen != Screen::Locked) {
    s_lockedBack = (s_screen == Screen::StoryPlay || s_screen == Screen::StoryList ||
                    s_screen == Screen::StoryError)
                       ? Screen::StoryList
                       : Screen::MusicList;
    if (Story::active()) Story::stop();
    else                 Player::stop();
    s_screen = Screen::Locked;
  }
  if (s_lockShown == lk) return;
  s_lockShown = lk;

  char l1[64] = {0};
  char l2[64] = {0};
  if (lk == Parental::Lock::DailyLimit) {
    snprintf(l1, sizeof(l1), "今日额度已用完");
    snprintf(l2, sizeof(l2), "明天 00:00 恢复 · 家长可调整额度");
    Ui::drawLock(lockTitle(lk), l1, l2);
  } else if (lk == Parental::Lock::Bedtime) {
    snprintf(l1, sizeof(l1), "休息时间 %02u:%02u - %02u:%02u",
             (unsigned)(Parental::bedtimeStartMin() / 60),
             (unsigned)(Parental::bedtimeStartMin() % 60),
             (unsigned)(Parental::bedtimeEndMin() / 60),
             (unsigned)(Parental::bedtimeEndMin() % 60));
    snprintf(l2, sizeof(l2), "明天早上再见");
    Ui::drawLock(lockTitle(lk), l1, l2);
  } else {
    snprintf(l1, sizeof(l1), "故事机已锁定");
    snprintf(l2, sizeof(l2), "家长可在小程序中解锁");
    Ui::drawLock(lockTitle(lk), l1, l2);
  }
}

/* ---------------- setup / loop ---------------- */

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32-S3 Music Player ===");
  Serial.printf("[SYS] PSRAM %s, size: %u bytes\n",
                psramFound() ? "OK" : "NOT FOUND", ESP.getPsramSize());

  Display::begin();

  Fram::begin();                    // I2C FRAM (设置持久化)
  Serial.printf("[SYS] FRAM %s\n", Fram::ok() ? "OK" : "NOT FOUND");
  Settings::load();                 // 读取掉电保存的配置
  Session::load();                  // 读取掉电保存的音量/上次播放位置
  Parental::begin();                // 家长策略 + 今日用量 + 绑定信息
#ifdef SESSION_SELFTEST
  Session::selfTest();              // 临时自检开关: -DSESSION_SELFTEST
#endif
  Serial.printf("[SES] FRAM 记忆: volume=%u, cat=\"%s\"\n",
                Session::volume(), Session::category());
  applySettings();

  Fonts::begin();                   // 内置 efont 中文点阵字体
  Ui::begin();
  Ui::boot("MUSIC PLAYER", "Mounting TF card ...");
  Display::present(true);

  Input::begin();
  Hall::begin();
  Serial.printf("[SYS] HALL level=%d (%s)\n",
                Hall::level(), Hall::detected() ? "card in" : "card out");
  Ui::setHall(Hall::detected());     // 顶栏通知位: 卡片插入状态

  if (!Storage::begin()) {
    Ui::boot("SD CARD ERROR", "Insert TF card then reset");
  } else {
    auto result = Catalog::begin();
    if (result == Catalog::Result::ScanFailed) {
      Ui::boot("列表读取失败", "请在设置中刷新列表");
    } else if (result == Catalog::Result::SaveFailed) {
      Ui::boot("索引保存失败", "本次列表可用，请检查SD卡");
    } else if (Storage::totalCount() == 0 && Story::count() == 0) {
      Ui::boot("NO MUSIC FOUND", "Copy MP3 folders to /music");
    } else {
      Ui::boot("MUSIC PLAYER", "Ready - press OK to select");
    }
    applyContentFilters();           // 家长白名单视图
  }
  Display::present(true);
  delay(600);

  Player::begin();
  Player::setEq(Settings::data().eq);
  setVolumeLimited(Session::volume());   // 恢复上次音量, 并套用家长音量范围
  Ble::begin(Settings::data().bleEnabled != 0);
  Network::begin(Settings::data().wifiEnabled != 0, Settings::data().apEnabled != 0);

#ifdef RADIO_SELFTEST
  Serial.println("[TEST] radio selftest: WiFi+AP on");
  Network::apply(true, true);
  delay(6000);
  Serial.println("[TEST] radio selftest: BLE advertising off/on");
  Ble::setAdvertisingEnabled(false);
  delay(1000);
  Ble::setAdvertisingEnabled(true);
  delay(1000);
  Network::apply(false, false);
  Serial.println("[TEST] radio selftest done");
#endif

  if (!Parental::bound()) {          // 首次启动显示绑定码, BLE 同时已可连接
    Ui::drawBindCode(Parental::bindCode());
    Display::present(true);
    delay(2500);
  }

  if (Storage::totalCount() == 0 && Story::count() == 0) {
    s_screen = Screen::Main;
    Ui::drawStatic();
    Ui::update(Player::status(), Hall::detected());
  } else if (!restoreSession()) {
    showMusicList();                // 没有记忆 / 记忆已失效: 开机进入分类列表
  }
}

void loop() {
  // 即使页面处理提前 return，也在所有绘制完成后提交；输入仍可高速轮询。
  struct PresentOnExit {
    ~PresentOnExit() { Display::present(); }
  } presentOnExit;
  /* 解码在独立 FreeRTOS 音频任务里持续运行, 这里只处理界面与输入 */

  Ui::setHall(Hall::detected());    // 卡片插拔: 任意界面都即时更新顶栏通知位
  Network::loop();                  // WiFi STA 连接状态日志

  /* BLE 通道: 先收包/下发动作, 再由主循环统一执行 */
  Ble::loop();
  Ble::Command bleCmd;
  for (int i = 0; i < 4 && Ble::takeCommand(bleCmd); i++) handleBleCommand(bleCmd);

  PlayerStatus ps = Player::status();
  Parental::tick(ps.playing && !ps.paused);

  if (Ble::takeConfigChanged()) applyParentalConfig();

  Parental::Lock lk = Parental::lock();
  if (lk != Parental::Lock::None) {
    armLock(lk);
    return;
  }
  if (s_screen == Screen::Locked) {
    s_lockShown = Parental::Lock::None;
    if (s_lockedBack == Screen::StoryList) showStoryList();
    else                                   showMusicList();
    return;
  }

  switch (s_screen) {

  /* ---------------- 无内容 / 锁定占位 ---------------- */
  case Screen::NoContent: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { enterStoryMode(); return; }
    if (enc == 1) {
      if (Storage::viewCatCount() > 0) showMusicList();
      else if (Story::viewCount() > 0) showStoryList();
      return;
    }
    int8_t ok = Input::okEvent();
    if (ok == 2) { enterSettings(); return; }
    if (ok == 1) {
      if (Storage::viewCatCount() > 0) showMusicList();
      else if (Story::viewCount() > 0) showStoryList();
      return;
    }
    break;
  }

  case Screen::Locked:
    break;

  /* ---------------- 音乐分类列表 ---------------- */
  case Screen::MusicList: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { enterStoryMode(); return; }                  // SET 长按: 进入故事模式
    if (enc == 1) { startMusicCategory(s_musicSel); return; }    // SET 短按: 播放选中分类

    int move = Input::encoderSteps();
    if (Input::nextEvent() == 1) move += 1;
    if (Input::prevEvent() == 1) move -= 1;

    int n = Storage::viewCatCount();
    if (move != 0 && n > 0) {
      s_musicSel = (int)((s_musicSel + move % n + n) % n);
      Ui::updateMusicList(n, s_musicSel);
      return;
    }

    int8_t ok = Input::okEvent();
    if (ok == 2) { enterSettings(); return; }
    if (ok == 1) { startMusicCategory(s_musicSel); return; }
    break;
  }

  /* ---------------- 当前歌曲列表 ---------------- */
  case Screen::TrackList: {
    int n = Storage::count();

    int8_t enc = Input::encEvent();
    if (enc == 2) { enterStoryMode(); return; }                 // SET 长按: 进入故事模式
    if (enc == 1) { playMusicIndex(s_trackSel); return; }       // SET 短按: 播放选中曲目

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 2 || pv == 2) {                                   // 长按: 返回播放界面(保持播放)
      s_screen = Screen::Main;
      Ui::drawStatic();
      Ui::update(Player::status(), Hall::detected());
      return;
    }

    int move = Input::encoderSteps() + (nx == 1 ? 1 : 0) + (pv == 1 ? -1 : 0);
    if (move != 0 && n > 0) {
      s_trackSel = (int)((s_trackSel + move % n + n) % n);
      Ui::updateTrackList(n, s_trackSel, s_trackPlaying);
      return;
    }

    switch (Input::okEvent()) {
      case 1: playMusicIndex(s_trackSel); return;               // 单击: 播放选中曲目
      case 2: enterSettings(); return;
      case 3:                                                   // 长按: 返回播放界面(保持播放)
        s_screen = Screen::Main;
        Ui::drawStatic();
        Ui::update(Player::status(), Hall::detected());
        return;
      default: break;
    }

    /* 自动续播换曲时同步"播放中"高亮 */
    static uint32_t lastSync = 0;
    if (millis() - lastSync > 500) {
      lastSync = millis();
      int cur = Player::status().index;
      if (cur >= 0 && cur < n && cur != s_trackPlaying) {
        s_trackPlaying = cur;
        Ui::updateTrackList(n, s_trackSel, s_trackPlaying);
      }
      rememberPosition();           // 在列表页自动续播时也要更新记忆
    }
    break;
  }

  /* ---------------- 音乐播放界面 ---------------- */
  case Screen::Main: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { enterStoryMode(); return; }   // SET 长按: 进入故事模式
    if (enc == 1) Player::toggle();               // SET 短按: 播放/暂停

    int8_t nx = Input::nextEvent();
    int8_t pv = Input::prevEvent();
    if (nx == 1)      Player::next();                       // 短按: 下一曲
    else if (nx == 2) { showTrackList(); return; }          // 长按: 当前歌曲列表
    if (pv == 1)      Player::prev();                       // 短按: 上一曲
    else if (pv == 2) { Player::stop(); showMusicList(); return; }   // 长按: 分类列表

    switch (Input::okEvent()) {
      case 1: Player::toggle(); break;    // 单击: 播放/暂停
      case 2: enterSettings();  return;   // 双击: 进入设置
      case 3: Player::stop(); showMusicList(); return;   // 长按: 停止并返回分类列表
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) changeVolume(steps);

    static uint32_t lastUi = 0;
    if (millis() - lastUi >= 33) {
      lastUi = millis();
      Ui::update(Player::status(), Hall::detected());
      rememberPosition();           // 换曲/自动续播/选分类都在这里落到 FRAM
    }
    break;
  }

  /* ---------------- 设置界面 ---------------- */
  case Screen::Settings: {
    if (Input::nextEvent() == 1) { s_sel = (s_sel + 1) % SETTINGS_ITEMS; Ui::refreshSettings(Settings::data(), s_sel); }
    if (Input::prevEvent() == 1) { s_sel = (s_sel - 1 + SETTINGS_ITEMS) % SETTINGS_ITEMS; Ui::refreshSettings(Settings::data(), s_sel); }

    switch (Input::okEvent()) {
      case 1:
        if (s_sel == 3) { refreshCatalog(); return; }
        if (s_sel >= 4) { toggleSetting(s_sel); break; }
        adjustSetting(s_sel, +1); break;
      case 2: exitSettings();           return;  // 双击: 保存返回
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) adjustSetting(s_sel, steps);
    break;
  }

  /* ---------------- 故事列表 ---------------- */
  case Screen::StoryList: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { exitStoryMode(); return; }    // SET 长按: 回音乐模式
    if (enc == 1) { startStory(s_storySel); return; }

    int move = Input::encoderSteps();
    if (Input::nextEvent() == 1) move += 1;
    if (Input::prevEvent() == 1) move -= 1;

    int n = Story::viewCount();
    if (move != 0 && n > 0) {
      s_storySel = (int)((s_storySel + move % n + n) % n);
      Ui::updateStoryList(n, s_storySel);
      return;
    }

    int8_t ok = Input::okEvent();
    if (ok == 2) { enterSettings(); return; }
    if (ok == 1) { startStory(s_storySel); return; }
    break;
  }

  /* ---------------- 故事播放 ---------------- */
  case Screen::StoryPlay: {
    Story::loop();

    if (Story::takeFinished()) {                  // 播放完毕 -> 回列表
      Story::stop();
      showStoryList();
      return;
    }

    if (s_storyRedraw || Story::panelIndex() != s_storyPanel) {   // 换插画 / 从设置返回
      s_storyRedraw = false;
      s_storyPanel  = Story::panelIndex();
      Ui::drawStoryFrame(Story::panelPath());
      Ui::invalidateStoryBar();                      // 插画覆盖控制条区, 需整体重铺
    }

    int8_t enc = Input::encEvent();
    if (enc == 2) { exitStoryMode(); return; }    // SET 长按: 回音乐模式
    if (enc == 1) Player::toggle();               // SET 短按: 暂停/继续

    switch (Input::okEvent()) {
      case 1: Player::toggle(); break;              // 单击: 暂停/继续
      case 2: enterSettings(); return;              // 双击: 进入设置
      case 3: Story::stop(); showStoryList();  return;   // 长按: 停止并返回列表
      default: break;
    }

    int steps = Input::encoderSteps();
    if (steps) changeVolume(steps);

    refreshStoryBar();                              // 每帧刷新, 内部只重绘变化区域
    break;
  }

  /* ---------------- 故事提示页 ---------------- */
  case Screen::StoryError: {
    int8_t enc = Input::encEvent();
    if (enc == 2) { exitStoryMode(); return; }    // SET 长按: 回音乐模式

    bool back = (enc == 1) || Input::nextEvent() == 1 || Input::prevEvent() == 1;
    int8_t ok = Input::okEvent();
    if (ok == 2) { enterSettings(); return; }
    if (ok == 1) back = true;

    if (back) {
      s_storySel = 0;
      showStoryList();
    }
    break;
  }

  }
}
