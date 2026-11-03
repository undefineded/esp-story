#include "app.h"

#include <cstdio>
#include <cstring>

#include "app_context.h"
#include "app_page.h"
#include "app_services.h"
#include "app_theme.h"
#include "ble_service.h"
#include "cJSON.h"
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
#include "nfc_service.h"
#include "ota_service.h"
#include "parental_binding.h"
#include "parental_policy.h"
#include "playback.h"
#include "power_service.h"
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
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
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
bool s_video_active = false;
bool s_was_screen_off = false;

#if CONFIG_STORYBOX_FRAM_CONFIRMED
Persistence::SettingsStore<Board::FramDevice>* s_settings_store = nullptr;
Persistence::MediaRecords<Board::FramDevice>* s_records_store = nullptr;
Persistence::ParentalStore<Board::FramDevice>* s_parental_store = nullptr;
#endif

Persistence::ParentalPolicy s_policy;
bool s_parental_ready = false;
uint32_t s_parental_save_at = 0;
uint32_t s_parental_unlock_at = 0;
uint32_t s_ble_publish_at = 0;

#if CONFIG_STORYBOX_USB_PROTOCOL
Storage::FileReader* s_usb_files = nullptr;
uint32_t s_catalog_refresh_at = 0;
#endif

NativeNfc::Service* s_nfc = nullptr;

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

// USB route for "cmd":"nfc". Only status is exposed: the native service still
// has no raw read/write API, so those actions answer NOT_SUPPORTED.
#if CONFIG_STORYBOX_USB_PROTOCOL
void addSystemStats(cJSON* data) {
    const auto state = g_playback.snapshot();
    const auto* track = g_playlist.current();
    const uint32_t seconds = state.sample_rate ? static_cast<uint32_t>(state.submitted_frames / state.sample_rate) : 0;

    cJSON_AddNumberToObject(data, "uptime", static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON* cpu = cJSON_AddObjectToObject(data, "cpu");
    // CPU load needs FreeRTOS run-time stats; report 0 until that is enabled.
    cJSON_AddNumberToObject(cpu, "load", 0);
    cJSON_AddNumberToObject(cpu, "cores", portNUM_PROCESSORS);
    cJSON_AddNumberToObject(cpu, "freq", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    cJSON_AddStringToObject(cpu, "model", "ESP32-S3");
    cJSON_AddNumberToObject(cpu, "revision", chip.revision);
    cJSON_AddStringToObject(cpu, "sdk", esp_get_idf_version());

    cJSON* ram = cJSON_AddObjectToObject(data, "ram");
    cJSON_AddNumberToObject(ram, "heapTotal",
                            static_cast<double>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(ram, "heapFree",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(ram, "heapMin",
                            static_cast<double>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    cJSON_AddNumberToObject(ram, "psramTotal",
                            static_cast<double>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
    cJSON_AddNumberToObject(ram, "psramFree",
                            static_cast<double>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    cJSON* rom = cJSON_AddObjectToObject(data, "rom");
    uint32_t flash_bytes = 0;
    esp_flash_get_size(nullptr, &flash_bytes);
    const esp_partition_t* running = esp_ota_get_running_partition();
    uint32_t image_len = 0;
    if (running) {
        const esp_partition_pos_t position{running->address, running->size};
        esp_image_metadata_t metadata{};
        if (esp_image_get_metadata(&position, &metadata) == ESP_OK) image_len = metadata.image_len;
    }
    cJSON_AddNumberToObject(rom, "flashSize", flash_bytes);
    cJSON_AddNumberToObject(rom, "appSize", image_len);
    cJSON_AddNumberToObject(rom, "appTotal", running ? running->size : 0);
    cJSON_AddNumberToObject(rom, "appFree", running ? static_cast<double>(running->size - image_len) : 0);
    cJSON_AddStringToObject(rom, "partition", running ? running->label : "");

    cJSON* storage = cJSON_AddObjectToObject(data, "storage");
    uint64_t total = 0, free_bytes = 0;
    const bool mounted = s_context.sd_mounted &&
                         esp_vfs_fat_info(Storage::SdVolume::root, &total, &free_bytes) == ESP_OK;
    cJSON_AddBoolToObject(storage, "ready", mounted);
    cJSON_AddNumberToObject(storage, "total", static_cast<double>(total));
    cJSON_AddNumberToObject(storage, "free", static_cast<double>(free_bytes));
    cJSON_AddNumberToObject(storage, "used", static_cast<double>(total - free_bytes));
    cJSON_AddNumberToObject(storage, "tracks",
                            s_catalog ? static_cast<double>(s_catalog->tracks.size()) : 0);
    cJSON_AddNumberToObject(storage, "stories",
                            s_catalog ? static_cast<double>(s_catalog->stories.size()) : 0);

    cJSON* settings = cJSON_AddObjectToObject(data, "settings");
    cJSON_AddNumberToObject(settings, "volume", s_context.volume);
    cJSON_AddNumberToObject(settings, "volumeMax", 21);
    cJSON_AddNumberToObject(settings, "brightness", s_context.settings.brightness);
    cJSON_AddNumberToObject(settings, "brightnessMin", 10);
    cJSON_AddNumberToObject(settings, "rotation", s_context.settings.rotation);
    static const char* kEqNames[] = {"vocal", "pop", "bass", "flat"};
    cJSON_AddStringToObject(settings, "eq", s_context.settings.eq < 4 ? kEqNames[s_context.settings.eq] : "flat");
    cJSON_AddNumberToObject(settings, "screenOffMin", s_context.settings.screen_off_min);
    cJSON_AddNumberToObject(settings, "sleepMin", s_context.settings.sleep_min);
    cJSON_AddBoolToObject(settings, "ble", s_context.settings.ble != 0);
    cJSON_AddBoolToObject(settings, "wifi", s_context.settings.wifi != 0);
    cJSON_AddBoolToObject(settings, "ap", s_context.settings.ap != 0);
    cJSON_AddBoolToObject(settings, "childLock", s_parental_ready && s_policy.childLocked());
    cJSON_AddBoolToObject(settings, "bound", parentalBound());
    cJSON_AddStringToObject(settings, "name", Ble::deviceName());
    cJSON_AddStringToObject(settings, "fw", "storybox-idf");
    cJSON_AddNumberToObject(settings, "proto", 4);
    cJSON_AddStringToObject(settings, "role", "storybox");

    cJSON* network = cJSON_AddObjectToObject(data, "network");
    cJSON_AddBoolToObject(network, "wifi", false);
    cJSON_AddBoolToObject(network, "ap", false);
    cJSON_AddStringToObject(network, "apSsid", "");

    cJSON* player = cJSON_AddObjectToObject(data, "player");
    cJSON_AddBoolToObject(player, "playing", state.state == Audio::PlaybackState::Playing);
    cJSON_AddBoolToObject(player, "paused", state.state == Audio::PlaybackState::Paused);
    cJSON_AddBoolToObject(player, "hasTrack", track != nullptr);
    cJSON_AddBoolToObject(player, "story", false);
    cJSON_AddStringToObject(player, "title", track ? track->name.c_str() : "");
    cJSON_AddStringToObject(player, "codec", "mp3");
    cJSON_AddNumberToObject(player, "curSec", seconds);
    cJSON_AddNumberToObject(player, "durSec", 0);
    cJSON_AddNumberToObject(player, "bitrate", state.bitrate);
    cJSON_AddNumberToObject(player, "sampleRate", state.sample_rate);
    cJSON_AddNumberToObject(player, "volume", s_context.volume);
}

void addNfcStatus(cJSON* data) {
    const auto state = s_nfc->snapshot();
    cJSON_AddBoolToObject(data, "module", state.module);
    cJSON_AddBoolToObject(data, "polling", state.polling);
    cJSON_AddStringToObject(data, "firmware", state.firmware);
    cJSON_AddBoolToObject(data, "card", state.card.valid);
    if (state.card.valid) {
        cJSON_AddStringToObject(data, "kind", Nfc::kindId(state.card.kind));
        cJSON_AddStringToObject(data, "kindName", Nfc::kindName(state.card.kind));
        cJSON_AddBoolToObject(data, "writable", state.card.writable);
        cJSON_AddBoolToObject(data, "formatted", state.card.formatted);
        cJSON_AddNumberToObject(data, "capacity", state.card.capacity);
        char uid[32] = {};
        for (unsigned i = 0; i < state.card.uidLen && i < 10 && (i * 2 + 2) < sizeof(uid); ++i) {
            std::snprintf(uid + i * 2, sizeof(uid) - i * 2, "%02x", state.card.uid[i]);
        }
        cJSON_AddStringToObject(data, "uid", uid);
    }
}

bool hostRoute(void*, cJSON* request, const char* command, const char* action, cJSON* response,
               const char** error, const uint8_t* raw, std::size_t raw_length) {
    if (!command) return false;
    if (std::strcmp(command, "sys") == 0) {
        cJSON_AddStringToObject(response, "cmd", "sys");
        cJSON_AddStringToObject(response, "act", action ? action : "");
        if (!action) {
            *error = "BAD_ACT";
            return true;
        }
        if (std::strcmp(action, "ota_begin") == 0) {
            const cJSON* size = cJSON_GetObjectItemCaseSensitive(request, "size");
            const auto expected = cJSON_IsNumber(size) ? static_cast<std::size_t>(size->valuedouble) : 0;
            const auto result = Ota::begin(expected);
            if (result != ESP_OK) {
                *error = result == ESP_ERR_NOT_SUPPORTED ? "NO_OTA_SLOT" : "OTA_BEGIN";
                return true;
            }
        } else if (std::strcmp(action, "ota_chunk") == 0) {
            if (!raw || !raw_length) {
                *error = "BAD_LENGTH";
                return true;
            }
            if (Ota::write(raw, raw_length) != ESP_OK) {
                *error = "OTA_WRITE";
                return true;
            }
        } else if (std::strcmp(action, "ota_end") == 0) {
            if (Ota::finish() != ESP_OK) {
                *error = "OTA_END";
                return true;
            }
        } else if (std::strcmp(action, "ota_abort") == 0) {
            Ota::abort();
        } else if (std::strcmp(action, "set") == 0) {
            const cJSON* volume = cJSON_GetObjectItemCaseSensitive(request, "volume");
            const cJSON* brightness = cJSON_GetObjectItemCaseSensitive(request, "brightness");
            if (cJSON_IsNumber(volume)) setVolume(static_cast<int>(volume->valuedouble));
            if (cJSON_IsNumber(brightness)) {
                int value = static_cast<int>(brightness->valuedouble);
                if (value < 10) value = 10;
                if (value > 100) value = 100;
                s_context.settings.brightness = static_cast<uint8_t>(value);
                Display::setBrightness(s_context.settings.brightness);
                saveSettings();
            }
        } else if (std::strcmp(action, "stats") != 0) {
            *error = "BAD_ACT";
            return true;
        }
        addSystemStats(response);
        const auto ota = Ota::status();
        cJSON* info = cJSON_AddObjectToObject(response, "ota");
        cJSON_AddBoolToObject(info, "supported", ota.supported);
        cJSON_AddBoolToObject(info, "active", ota.active);
        cJSON_AddNumberToObject(info, "written", static_cast<double>(ota.written));
        cJSON_AddNumberToObject(info, "expected", static_cast<double>(ota.expected));
        return true;
    }
    if (std::strcmp(command, "nfc") == 0) {
        cJSON_AddStringToObject(response, "cmd", "nfc");
        cJSON_AddStringToObject(response, "act", action ? action : "");
        if (!s_nfc) {
            *error = "NFC_UNAVAILABLE";
            return true;
        }
        if (!action || std::strcmp(action, "status") != 0) {
            *error = "NOT_SUPPORTED";
            return true;
        }
        cJSON* data = cJSON_AddObjectToObject(response, "data");
        addNfcStatus(data);
        return true;
    }
    return false;
}
#endif  // CONFIG_STORYBOX_USB_PROTOCOL

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
    Power::begin();
    Theme::init();
    drawBoot("正在启动", "");
    Display::present(true);

    s_context.fram_confirmed =
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        true;
#else
        false;
#endif

    // The I2C bus is shared: FRAM and the PN532 sit on it. Create it once here
    // so NFC stays available even when FRAM is not confirmed.
    static Board::I2cBus bus;
    const bool bus_ready = bus.begin() == ESP_OK;
    if (!bus_ready) ESP_LOGW(kTag, "shared I2C bus unavailable");

#if CONFIG_STORYBOX_FRAM_CONFIRMED
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
    if (bus_ready && fram.begin(bus.handle(), true) == ESP_OK) {
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

#if CONFIG_STORYBOX_NFC
    static NativeNfc::Service nfc;
    if (bus_ready && nfc.begin(bus.handle()) == ESP_OK) {
        nfc.setPolling(true);
        s_nfc = &nfc;
        ESP_LOGI(kTag, "NFC polling enabled");
    } else {
        ESP_LOGW(kTag, "PN532 unavailable; NFC commands report NFC_UNAVAILABLE");
    }
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
#if CONFIG_STORYBOX_NFC
    constexpr const char* kHostFeature = "nfc";
#else
    constexpr const char* kHostFeature = nullptr;
#endif
    if (!Host::attachRoutes(hostRoute, nullptr, kHostFeature)) {
        ESP_LOGW(kTag, "USB route already registered");
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

        // Idle policy: panel off while audio plays, deep sleep (if enabled)
        // when silent. A host link or video playback keeps everything awake.
        Power::Inputs power_inputs{};
        power_inputs.audio_playing = playing;
        power_inputs.video_playing = s_video_active;
        power_inputs.host_active = Ble::connected();
        Power::Config power_config{};
        power_config.screen_off_min = s_context.settings.screen_off_min;
        power_config.sleep_min = s_context.settings.sleep_min;
        power_config.brightness = s_context.settings.brightness;
        const bool screen_off = Power::loop(now, power_inputs, power_config);
        if (!screen_off && s_was_screen_off) page->invalidate();
        s_was_screen_off = screen_off;

        if (page->dirty() && !screen_off) {
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

Persistence::ParentalRecord parentalRecord() {
    Persistence::ParentalRecord record{};
    if (s_parental_ready) record = s_policy.record();
    return record;
}

bool parentalApply(const Persistence::ParentalRecord& record) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store) return false;
    Persistence::ParentalRecord candidate = record;
    candidate.checksum = Persistence::parentalChecksum(candidate);
    if (!Persistence::validParentalRecord(candidate)) return false;
    if (!s_parental_store->save(candidate)) return false;
    s_parental_ready = s_policy.begin(s_parental_store->value(), nowMs());
    return s_parental_ready;
#else
    (void)record;
    return false;
#endif
}

bool parentalHasPin() {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    return s_parental_store && Persistence::parentalHasPin(s_parental_store->value());
#else
    return false;
#endif
}

bool parentalUnlocked() {
    if (!s_parental_ready) return false;
    if (!parentalHasPin()) return true;
    return s_parental_unlock_at != 0 && nowMs() - s_parental_unlock_at < 300000;
}

bool unlockParental(const char* pin) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_ready || !s_parental_store) return false;
    if (!Persistence::parentalVerifyPin(s_parental_store->value(), pin ? pin : "")) return false;
    s_parental_unlock_at = nowMs();
    return true;
#else
    (void)pin;
    return false;
#endif
}

void lockParentalSession() { s_parental_unlock_at = 0; }

bool parentalSetPin(const char* pin) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_ready) return false;
    Persistence::ParentalRecord record = s_policy.record();
    if (!Persistence::parentalSetPin(record, randomU32, pin ? pin : "")) return false;
    return parentalApply(record);
#else
    (void)pin;
    return false;
#endif
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

bool videoActive() { return s_video_active; }
void setVideoActive(bool active) { s_video_active = active; }

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
