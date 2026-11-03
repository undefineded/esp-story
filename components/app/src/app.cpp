#include "app.h"

#include <cstdio>
#include <cstring>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "ble_service.h"
#include "config.h"
#include "display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "fonts.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hall.h"
#include "input.h"
#include "i2c_bus.h"
#include "media_library.h"
#include "parental_binding.h"
#include "parental_policy.h"
#include "playback.h"
#include "sd_volume.h"
#include "sdkconfig.h"
#include "storage_access.h"

#if CONFIG_STORYBOX_FRAM_CONFIRMED
#include "fram_device.h"
#include "media_records.h"
#include "settings_record.h"
#endif

#if CONFIG_STORYBOX_USB_PROTOCOL
#include "file_reader.h"
#include "usb_transport.h"
#endif

namespace App {
namespace {

constexpr char kTag[] = "app";
constexpr int kMaxDepth = 6;
constexpr uint32_t kFrameIntervalMs = 20;

Context s_context;
Page* s_stack[kMaxDepth] = {};
int s_depth = 0;

Audio::Playback* s_playback = nullptr;

// Service objects live for the whole application; pages only see references.
Storage::SdVolume g_volume;
Storage::AccessGate g_gate;
Media::Library g_library(g_gate);
Audio::Playback g_playback(g_gate);
Media::Playlist g_playlist;
CatalogPtr s_catalog;

char s_notice[96] = {};
uint32_t s_notice_until = 0;

#if CONFIG_STORYBOX_FRAM_CONFIRMED
Persistence::SettingsStore<Board::FramDevice>* s_settings_store = nullptr;
Persistence::MediaRecords<Board::FramDevice>* s_records_store = nullptr;
Persistence::ParentalStore<Board::FramDevice>* s_parental_store = nullptr;
#endif

Persistence::ParentalPolicy s_policy;
bool s_parental_ready = false;
uint32_t s_parental_save_at = 0;
uint32_t s_ble_publish_at = 0;

#if CONFIG_STORYBOX_USB_PROTOCOL
Storage::FileReader* s_usb_files = nullptr;
uint32_t s_catalog_refresh_at = 0;
#endif

uint32_t randomU32() { return esp_random(); }

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ---- BLE hooks: the app owns the PCTL record, the service only asks --------

bool bleBind(void*, const char* code, char* token, std::size_t capacity) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store || !s_parental_ready) return false;
    Persistence::ParentalRecord record = s_parental_store->value();
    if (!Persistence::parentalBind(record, randomU32, code, token, capacity)) return false;
    if (!s_parental_store->save(record)) return false;
    s_policy.begin(s_parental_store->value(), nowMs());
    return true;
#else
    (void)code; (void)token; (void)capacity;
    return false;
#endif
}

bool bleVerifyToken(void*, const char* token) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store || !token) return false;
    return Persistence::parentalVerifyToken(s_parental_store->value(), token);
#else
    (void)token;
    return false;
#endif
}

bool bleVerifyPin(void*, const char* pin) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store || !pin) return false;
    return Persistence::parentalVerifyPin(s_parental_store->value(), pin);
#else
    (void)pin;
    return false;
#endif
}

void bleSyncTime(void*, uint32_t epoch, int16_t timezone) {
    if (s_parental_ready) s_policy.syncTime(epoch, timezone, nowMs());
}

bool bleHasPin(void*) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    return s_parental_store && Persistence::parentalHasPin(s_parental_store->value());
#else
    return false;
#endif
}

bool bleBound(void*) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    return s_parental_store && Persistence::parentalBound(s_parental_store->value());
#else
    return false;
#endif
}

const char* bleBindCode(void*) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    return s_parental_store ? Persistence::parentalBindCode(s_parental_store->value()) : "";
#else
    return "";
#endif
}

Ble::Hooks makeBleHooks() {
    Ble::Hooks hooks{};
    hooks.bind = bleBind;
    hooks.verify_token = bleVerifyToken;
    hooks.verify_pin = bleVerifyPin;
    hooks.sync_time = bleSyncTime;
    hooks.has_pin = bleHasPin;
    hooks.bound = bleBound;
    hooks.bind_code = bleBindCode;
    return hooks;
}

void handleBleCommands() {
    Ble::Command command;
    for (int i = 0; i < 4; ++i) {
        if (!Ble::takeCommand(command)) break;
        switch (command.action) {
            case Ble::Action::Volume:
                setVolume(command.value);
                break;
            case Ble::Action::Play:
                pausePlayback(false);
                break;
            case Ble::Action::Pause:
                pausePlayback(true);
                break;
            case Ble::Action::Toggle:
                pausePlayback(g_playback.snapshot().state == Audio::PlaybackState::Playing);
                break;
            case Ble::Action::Next:
                if (g_playlist.move(1)) playCurrent(true);
                break;
            case Ble::Action::Prev:
                if (g_playlist.move(-1)) playCurrent(true);
                break;
            case Ble::Action::Stop:
                stopPlayback();
                break;
            case Ble::Action::PlayStory:
                openStory(static_cast<std::size_t>(command.value < 0 ? 0 : command.value));
                break;
            case Ble::Action::PlayMusic: {
                const auto snapshot = s_catalog;
                if (!snapshot || command.value < 0 ||
                    static_cast<std::size_t>(command.value) >= snapshot->tracks.size()) {
                    break;
                }
                const auto& track = snapshot->tracks[static_cast<std::size_t>(command.value)];
                if (g_playlist.select(snapshot, 0) && g_playlist.seekId(track.id)) {
                    playCurrent(true);
                }
                break;
            }
            default:
                break;
        }
    }
}

void publishBleState() {
    Ble::Snapshot snapshot{};
    const auto state = g_playback.snapshot();
    snapshot.playing = state.state == Audio::PlaybackState::Playing;
    snapshot.paused = state.state == Audio::PlaybackState::Paused;
    snapshot.volume = s_context.volume;
    snapshot.volume_min = 0;
    snapshot.volume_max = 21;
    const auto* track = g_playlist.current();
    snapshot.mode = (snapshot.playing || snapshot.paused) ? "music" : "idle";
    snapshot.title = track ? track->name.c_str() : "";
    snapshot.current = state.sample_rate ? static_cast<uint32_t>(state.submitted_frames / state.sample_rate) : 0;
    snapshot.duration = 0;
    snapshot.used = parentalUsedSeconds();
    snapshot.lock = parentalLockCode();
    Ble::setSnapshot(snapshot);
}

void drawBoot(const char* title, const char* line) {
    auto& g = Display::canvas();
    const auto& c = Theme::colors();
    Theme::setTopBarColor(c.coral);
    g.fillScreen(c.paper);
    g.fillRect(0, 0, 320, 30, c.coral);
    g.fillRect(0, 30, 320, 2, c.coral_dark);
    Theme::drawSmileFace(g, 19, 15, 10);
    g.setFont(Fonts::body());
    g.setTextColor(TFT_WHITE, c.coral);
    g.drawString("小小故事盒", 36, 7);

    Theme::drawSmileFace(g, 160, 92, 16);
    g.setFont(Fonts::title());
    g.setTextColor(c.text, c.paper);
    g.drawCenterString(title, 160, 126);
    if (line && line[0]) {
        g.setFont(Fonts::body());
        g.drawCenterString(line, 160, 158);
    }
}

void sampleInput(Event& event) {
    const int steps = Input::encoderSteps();
    const auto ok = Input::okEvent();
    const auto next = Input::nextEvent();
    const auto prev = Input::prevEvent();
    const auto set = Input::encEvent();

    event.delta = steps;
    if (ok == 1) event.key = Key::Ok;
    else if (ok == 2) event.key = Key::OkDouble;
    else if (ok == 3) event.key = Key::OkLong;
    else if (next == 1) event.key = Key::Next;
    else if (next == 2) event.key = Key::NextLong;
    else if (prev == 1) event.key = Key::Prev;
    else if (prev == 2) event.key = Key::PrevLong;
    else if (set == 1) event.key = Key::Set;
    else if (set == 2) event.key = Key::SetLong;
}

void run() {
    Display::begin();
    Fonts::begin();
    Input::begin();
    Hall::begin();
    Ble::begin("StoryBox", makeBleHooks());
    Theme::init();
    drawBoot("正在启动", "");
    Display::present(true);

    s_context.fram_confirmed =
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        true;
#else
        false;
#endif

#if CONFIG_STORYBOX_FRAM_CONFIRMED
    static Board::I2cBus bus;
    static Board::FramDevice fram(
#if CONFIG_STORYBOX_BOARD_REV_20261028
        Board::rev20261028
#else
        Board::legacy
#endif
    );
    static Persistence::SettingsStore<Board::FramDevice> settings(fram);
    static Persistence::MediaRecords<Board::FramDevice> records(fram);
    static Persistence::ParentalStore<Board::FramDevice> parental_store(fram);
    if (bus.begin() == ESP_OK && fram.begin(bus.handle(), true) == ESP_OK) {
        const auto settings_result = settings.load();
        s_context.fram_ready = settings_result != Persistence::LoadResult::IoError;
        if (s_context.fram_ready) {
            s_context.settings = settings.value();
            s_settings_store = &settings;
            Display::setBrightness(s_context.settings.brightness);
            Display::setRotation(s_context.settings.rotation);
            if (records.load()) {
                s_records_store = &records;
                s_context.volume = records.session().volume;
            } else {
                ESP_LOGW(kTag, "volume/favorites record unreadable; using defaults");
            }
            // A device without a valid PCTL record is provisioned with the
            // legacy permissive defaults; nothing is written when reading fails.
            if (parental_store.load() == Persistence::LoadResult::Invalid) {
                Persistence::ParentalRecord fresh{};
                std::memcpy(fresh.magic, "PCTL", 4);
                fresh.version = 1;
                Persistence::parentalFactoryDefaults(fresh, randomU32);
                fresh.pin_salt = 0;
                fresh.pin_hash = 0;
                fresh.flags = 0;
                if (!parental_store.save(fresh)) {
                    ESP_LOGW(kTag, "parental provisioning failed; playback stays open");
                } else {
                    ESP_LOGI(kTag, "parental record provisioned");
                }
            }
            if (parental_store.ready()) {
                s_parental_ready = s_policy.begin(parental_store.value(), nowMs());
                s_parental_store = &parental_store;
                ESP_LOGI(kTag, "parental ready; bind code set=%d",
                         Persistence::parentalBindCode(parental_store.value())[0] != '\0');
            } else {
                ESP_LOGW(kTag, "parental record unavailable; policy not enforced");
            }
        } else {
            ESP_LOGW(kTag, "settings read failed; persistence disabled this boot");
        }
    } else {
        ESP_LOGW(kTag, "FRAM unavailable; running with RAM-only settings");
    }
#else
    ESP_LOGW(kTag, "FRAM not confirmed in this build; settings are RAM-only");
#endif

    s_context.sd_mounted = g_volume.mount() == ESP_OK;
    ESP_LOGI(kTag, "SD mount: %s", s_context.sd_mounted ? "ok" : "unavailable");

#if CONFIG_STORYBOX_USB_PROTOCOL
    if (s_context.sd_mounted) {
        // Writable reader: the desktop file manager can list and upload into
        // existing folders. Delete/rename stay unsupported, so hello advertises
        // fs_transfer rather than fs.
        static Storage::FileReader usb_files(g_gate, Storage::SdVolume::root, true);
        const auto attached = Host::attachFiles(usb_files);
        if (attached == ESP_OK) {
            s_usb_files = &usb_files;
            ESP_LOGI(kTag, "USB file reader attached (writable)");
        } else {
            ESP_LOGW(kTag, "USB file reader attach failed: %s", esp_err_to_name(attached));
        }
    }
#endif

    s_playback = &g_playback;
    ESP_ERROR_CHECK(g_playback.begin());
    g_playback.setVolume(static_cast<unsigned>(s_context.volume));
    g_playback.setEq(static_cast<Audio::EqPreset>(s_context.settings.eq));

    s_catalog = g_library.snapshot();
    if (s_context.sd_mounted && g_library.loadCache(Storage::SdVolume::root)) {
        s_context.catalog_ready = true;
        ESP_LOGI(kTag, "media index loaded");
    } else if (s_context.sd_mounted) {
        ESP_LOGI(kTag, "media index missing; first library page will scan");
    }

    goHome();

    uint32_t last_volume_save = nowMs();
    int last_saved_volume = s_context.volume;
    bool last_hall = Hall::detected();
    s_context.hall = last_hall;

    for (;;) {
        const uint32_t now = nowMs();

        Event event;
        sampleInput(event);
        if (!event.empty()) {
            Page* page = s_stack[s_depth - 1];
            page->handle(event);
            page->invalidate();
        }

        const bool hall = Hall::detected();
        if (hall != last_hall) {
            last_hall = hall;
            s_context.hall = hall;
            s_stack[s_depth - 1]->invalidate();
        }

        Page* page = s_stack[s_depth - 1];
        page->tick(now);

        // Parental enforcement: charge playback time and stop when a quota,
        // bedtime window or remote lock becomes active while audio is running.
        const auto playback_state = g_playback.snapshot().state;
        const bool playing = playback_state == Audio::PlaybackState::Playing;
        parentalTick(playing);
        if (s_parental_ready && !playbackAllowed() &&
            (playing || playback_state == Audio::PlaybackState::Paused)) {
            g_playback.stopAndWait(3000);
            page->invalidate();
        }
        parentalSaveIfDirty(now);

        // BLE control channel: drain commands, then publish a 1 Hz state
        // snapshot for the mini-program.
        Ble::loop();
        handleBleCommands();
        if (now - s_ble_publish_at >= 1000) {
            s_ble_publish_at = now;
            publishBleState();
        }

#if CONFIG_STORYBOX_USB_PROTOCOL
        // A host upload bumps the storage revision. Rescan once playback is
        // idle: the maintenance window needs every file lease released.
        if (s_context.sd_mounted && s_catalog &&
            s_catalog->storage_revision != g_gate.revision() && now - s_catalog_refresh_at >= 2000) {
            const auto state_now = g_playback.snapshot().state;
            const bool idle = state_now == Audio::PlaybackState::Stopped ||
                              state_now == Audio::PlaybackState::Ended ||
                              state_now == Audio::PlaybackState::Failed;
            if (idle) {
                s_catalog_refresh_at = now;
                if (refreshCatalog()) {
                    s_context.catalog_ready = true;
                    notify("媒体已更新", 2000);
                }
            }
        }
#endif

        if (s_notice_until && now >= s_notice_until) {
            s_notice_until = 0;
            page->invalidate();
        }

        if (page->dirty()) {
            page->draw();
            if (s_notice_until) {
                auto& g = Display::canvas();
                const auto& c = Theme::colors();
                g.fillRect(0, 218, 320, 22, c.paper);
                g.setFont(Fonts::body());
                g.setTextColor(c.text, c.paper);
                char line[96];
                Theme::fitText(g, s_notice, 304, line, sizeof(line));
                g.drawCenterString(line, 160, 220);
            }
            page->clean();
            Display::present();
        }

        if (s_context.volume != last_saved_volume && now - last_volume_save >= 1000) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
            if (s_records_store &&
                s_records_store->saveVolume(static_cast<uint8_t>(s_context.volume))) {
                last_volume_save = now;
                last_saved_volume = s_context.volume;
            }
#endif
        }

        vTaskDelay(pdMS_TO_TICKS(kFrameIntervalMs));
    }
}

}  // namespace

Context& ctx() { return s_context; }

int volume() { return s_context.volume; }

void setVolume(int value) {
    if (value < 0) value = 0;
    if (value > 21) value = 21;
    value = clampVolume(value);
    s_context.volume = value;
    if (s_playback) s_playback->setVolume(static_cast<unsigned>(value));
}

void stepVolume(int steps) {
    if (!steps) return;
    setVolume(s_context.volume + steps);
}

void applyEq(uint8_t preset) {
    s_context.settings.eq = preset > 3 ? 0 : preset;
    if (s_playback) s_playback->setEq(static_cast<Audio::EqPreset>(s_context.settings.eq));
}

CatalogPtr catalog() { return s_catalog; }
Media::Playlist& playlist() { return g_playlist; }
Audio::Playback& playback() { return g_playback; }
Storage::AccessGate& storageGate() { return g_gate; }

Media::Catalog emptyCatalog() { return Media::Catalog{}; }

bool refreshCatalog() {
    if (!s_context.sd_mounted) return false;
    // Playback holds a file lease for the whole track; maintenance needs every
    // lease released, so stop first and re-allow playback afterwards.
    g_playback.stopAndWait(3000);
    const auto result = g_library.scan(Storage::SdVolume::root);
    g_playback.allowPlayback();
    if (result != Media::ScanResult::Ok && result != Media::ScanResult::LimitExceeded) {
        ESP_LOGW(kTag, "media scan failed: %d", static_cast<int>(result));
        return false;
    }
    s_catalog = g_library.snapshot();
    s_context.catalog_ready = true;
    const auto saved = g_library.saveCache(Storage::SdVolume::root);
    if (saved != Storage::UploadResult::Ok) {
        ESP_LOGW(kTag, "media index save: %d", static_cast<int>(saved));
    }
    ESP_LOGI(kTag, "catalog: %u categories, %u tracks",
             static_cast<unsigned>(s_catalog->categories.size()),
             static_cast<unsigned>(s_catalog->tracks.size()));
    return true;
}

bool playCurrent(bool auto_start) {
    const auto* track = g_playlist.current();
    if (!track) return false;
    if (!playbackAllowed()) {
        notify(lockReasonText(parentalLock()));
        return false;
    }
    if (!categoryAllowed(track->category)) {
        notify("该分类已被家长限制");
        return false;
    }
    uint64_t request = 0;
    const auto result = g_playback.play(track->path.c_str(), auto_start, request);
    if (result != ESP_OK) ESP_LOGW(kTag, "play %s: %s", track->path.c_str(), esp_err_to_name(result));
    return result == ESP_OK;
}

bool stopPlayback() { return g_playback.stopAndWait(3000) == ESP_OK; }

void pausePlayback(bool paused) {
    uint64_t request = 0;
    g_playback.pause(paused, request);
}

bool favorite(std::size_t index) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (s_records_store) return s_records_store->favorite(static_cast<unsigned>(index));
#endif
    return false;
}

bool setFavorite(std::size_t index, bool enabled) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (s_records_store) return s_records_store->setFavorite(static_cast<unsigned>(index), enabled);
#endif
    return false;
}

bool parentalReady() { return s_parental_ready; }

Persistence::PolicyLock parentalLock() {
    if (!s_parental_ready) return Persistence::PolicyLock::None;
    return s_policy.lock(nowMs());
}

bool parentalBound() {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (s_parental_store) return Persistence::parentalBound(s_parental_store->value());
#endif
    return false;
}

const char* parentalBindCode() {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (s_parental_store && s_parental_store->ready())
        return Persistence::parentalBindCode(s_parental_store->value());
#endif
    return "";
}

bool playbackAllowed() { return parentalLock() == Persistence::PolicyLock::None; }

int clampVolume(int value) { return s_parental_ready ? s_policy.clampVolume(value) : value; }

bool storyAllowed(std::size_t index) {
    if (!s_parental_ready) return true;
    return s_policy.storyAllowed(static_cast<int>(index));
}

bool categoryAllowed(int category) {
    if (!s_parental_ready) return true;
    return s_policy.categoryAllowed(category);
}

void parentalTick(bool playing) {
    if (s_parental_ready) s_policy.tick(playing, nowMs());
}

void parentalSaveIfDirty(uint32_t now) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_ready || !s_parental_store || !s_policy.dirty()) return;
    if (now - s_parental_save_at < 60000) return;
    s_parental_save_at = now;
    if (s_parental_store->save(s_policy.record())) {
        s_policy.saved();
    } else {
        ESP_LOGW(kTag, "parental usage save failed");
    }
#else
    (void)now;
#endif
}

const char* lockReasonText(Persistence::PolicyLock lock) {
    switch (lock) {
        case Persistence::PolicyLock::Unavailable: return "家长记录不可用";
        case Persistence::PolicyLock::DailyLimit: return "今日使用时间已用完";
        case Persistence::PolicyLock::Bedtime: return "已到就寝时间";
        case Persistence::PolicyLock::Remote: return "家长已远程锁定";
        case Persistence::PolicyLock::ClockRequired: return "需要先同步时间";
        default: return "";
    }
}

uint32_t parentalUsedSeconds() {
    if (!s_parental_ready) return 0;
    return s_policy.record().used;
}

const char* parentalLockCode() {
    switch (parentalLock()) {
        case Persistence::PolicyLock::DailyLimit: return "daily";
        case Persistence::PolicyLock::Bedtime: return "bedtime";
        case Persistence::PolicyLock::Remote: return "remote";
        default: return "";
    }
}

bool saveSettings() {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_settings_store) return false;
    s_settings_store->set(s_context.settings);
    if (!s_settings_store->save()) {
        ESP_LOGW(kTag, "settings write failed; value kept in RAM");
        return false;
    }
    return true;
#else
    (void)s_context;
    return false;
#endif
}

void notify(const char* message, uint32_t duration_ms) {
    std::snprintf(s_notice, sizeof(s_notice), "%s", message ? message : "");
    s_notice_until = nowMs() + (duration_ms ? duration_ms : 1);
}

bool noticeActive() { return s_notice_until != 0; }
const char* noticeText() { return s_notice; }

uint8_t cycleMinutes(uint8_t current, int direction) {
    static const uint8_t kMinutes[] = {0, 1, 3, 5, 10, 30};
    constexpr int kCount = static_cast<int>(sizeof(kMinutes) / sizeof(kMinutes[0]));
    int index = 0;
    for (int i = 0; i < kCount; ++i) {
        if (kMinutes[i] == current) {
            index = i;
            break;
        }
    }
    index = (index + (direction >= 0 ? 1 : -1) + kCount) % kCount;
    return kMinutes[index];
}

const char* minutesLabel(uint8_t minutes) {
    switch (minutes) {
        case 0: return "关闭";
        case 1: return "1 分钟";
        case 3: return "3 分钟";
        case 5: return "5 分钟";
        case 10: return "10 分钟";
        default: return "30 分钟";
    }
}

void pushPage(Page* page) {
    if (!page || s_depth >= kMaxDepth) return;
    if (s_depth) s_stack[s_depth - 1]->leave();
    s_stack[s_depth++] = page;
    page->enter();
    page->invalidate();
}

void popPage() {
    if (s_depth <= 1) return;
    s_stack[--s_depth]->leave();
    s_stack[s_depth - 1]->enter();
    s_stack[s_depth - 1]->invalidate();
}

void goHome() {
    while (s_depth > 1) popPage();
    if (s_depth == 0) pushPage(createHomePage());
    s_stack[0]->enter();
    s_stack[0]->invalidate();
}

void openSettings() { pushPage(createSettingsPage()); }

bool start() {
    const auto created = xTaskCreate(
        [](void*) {
            run();
            vTaskDelete(nullptr);
        },
        "app", 16384, nullptr, 3, nullptr);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "cannot create app task");
        return false;
    }
    return true;
}

}  // namespace App
