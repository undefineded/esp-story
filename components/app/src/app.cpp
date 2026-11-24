#include "app.h"
#include "video_player.h"
#include "frame_pacing.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <mutex>

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
#include "esp_system.h"
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
#include "parental_config.h"
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

// Internal RAM is the scarce resource: the video stack needs ~23 KB of
// contiguous DMA memory for its I2S ring on top of the decoder scratch, while
// the media catalog lives in internal heap. Log the delta at each boot stage so
// a "video needs X" bug can be traced to the allocation that ate the room.
void logHeap(const char* step) {
    ESP_LOGI(kTag, "%s: internal=%u largest=%u psram=%u", step,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

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
// The USB worker reads the published catalog for "media" pages while the app
// task may republish it after a rescan; hold this only while copying the
// shared_ptr (the catalog itself is immutable).
std::mutex s_catalog_mutex;

char s_notice[96] = {};
uint32_t s_notice_until = 0;
bool s_video_active = false;
bool s_was_screen_off = false;
const char* s_now_mode = "idle";
char s_now_title[128] = {};
int s_now_story = -1;
int s_now_video = -1;

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

// Written once during App::start(), read by the USB worker; an atomic keeps the
// handoff well defined even when a host sends a command during boot.
std::atomic<NativeNfc::Service*> s_nfc{nullptr};

uint32_t randomU32() { return esp_random(); }

void publishCatalog() {
    std::lock_guard<std::mutex> lock(s_catalog_mutex);
    s_catalog = g_library.snapshot();
}

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// ---- BLE hooks: the app owns the PCTL record, the service only asks --------

bool bleBind(void*, const char* code, char* token, std::size_t capacity) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store || !s_parental_ready) return false;
    Persistence::ParentalRecord record = s_parental_store->value();
    if (!Persistence::parentalBind(record, randomU32, code, token, capacity)) return false;
    if (!s_parental_store->save(record)) return false;
    s_policy.rebind(s_parental_store->value(), nowMs());
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

// Parental status for the phone. A build or boot without a usable PCTL record
// reports "unavailable" rather than the permissive defaults.
bool bleStatus(void*, Persistence::ParentalStatus* out) {
    if (!out) return false;
    if (!s_parental_ready) {
        *out = Persistence::parentalStatusUnavailable();
        return false;
    }
    *out = Persistence::parentalStatusOf(s_policy.record(), s_policy.lock(nowMs()),
                                        s_policy.timeSynced());
    return true;
}

const char* bleApplyConfig(void*, const Persistence::ParentalPatch& patch) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store || !s_parental_ready) return "UNAVAILABLE";
    Persistence::ParentalRecord record = s_policy.record();
    if (!Persistence::applyParentalPatch(record, patch)) return "NO_FIELDS";
    if (!parentalApply(record)) return "SAVE";
    // A volume range change must clamp the level that is playing right now.
    setVolume(s_context.volume);
    return nullptr;
#else
    (void)patch;
    return "UNAVAILABLE";
#endif
}

const char* bleSetPin(void*, const char* pin) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store || !s_parental_ready) return "UNAVAILABLE";
    if (!Persistence::parentalPinFormatOk(pin)) return "PIN_FORMAT";
    return parentalSetPin(pin) ? nullptr : "SAVE";
#else
    (void)pin;
    return "UNAVAILABLE";
#endif
}

const char* bleFactoryReset(void*) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store || !s_parental_ready) return "UNAVAILABLE";
    Persistence::ParentalRecord record = s_policy.record();
    Persistence::parentalFactoryDefaults(record, randomU32);
    if (!parentalApply(record)) return "SAVE";
    lockParentalSession();
    return nullptr;
#else
    return "UNAVAILABLE";
#endif
}

const char* bleUnbind(void*) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_parental_store || !s_parental_ready) return "UNAVAILABLE";
    Persistence::ParentalRecord record = s_policy.record();
    Persistence::parentalUnbind(record);
    if (!parentalApply(record)) return "SAVE";
    lockParentalSession();
    return nullptr;
#else
    return "UNAVAILABLE";
#endif
}

// Admission control for "ctrl": the app, not the BLE layer, decides whether a
// lock, the category allowlist or an out-of-range index rejects the request.
const char* bleGate(void*, Ble::Action action, int32_t value) {
    const auto snapshot = s_catalog;
    const int track_count = snapshot ? static_cast<int>(snapshot->tracks.size()) : 0;
    // Favourites are metadata, so they stay available on a locked device, but
    // the index must exist.
    if (action == Ble::Action::Favourite) {
        return value >= 0 && value < track_count ? "" : "RANGE";
    }
    switch (action) {
        case Ble::Action::Play:
        case Ble::Action::Toggle:
        case Ble::Action::Next:
        case Ble::Action::Prev:
        case Ble::Action::PlayStory:
        case Ble::Action::PlayMusic:
            break;
        default:
            return "";
    }
    if (!playbackAllowed()) return "LOCKED";
    if (action == Ble::Action::PlayStory) {
        return storyAllowed(static_cast<std::size_t>(value < 0 ? 0 : value)) ? "" : "NOT_ALLOWED";
    }
    if (action == Ble::Action::PlayMusic) {
        if (value < 0 || value >= track_count) return "RANGE";
        const auto& track = snapshot->tracks[static_cast<std::size_t>(value)];
        return categoryAllowed(track.category) ? "" : "NOT_ALLOWED";
    }
    return "";
}

// Category name in the catalog's 1-based numbering; 0 is "all songs".
const char* categoryName(int category) {
    static const char* const kAll = "全部歌曲";
    if (category <= 0) return kAll;
    const auto snapshot = s_catalog;
    if (!snapshot) return "";
    const std::size_t index = static_cast<std::size_t>(category - 1);
    return index < snapshot->categories.size() ? snapshot->categories[index].c_str() : "";
}

int categoryTrackCount(int category) {
    const auto snapshot = s_catalog;
    if (!snapshot || category <= 0) return 0;
    int count = 0;
    for (const auto& track : snapshot->tracks) {
        if (track.category == category) ++count;
    }
    return count;
}

// Content revision of everything a full listing would return: track and story
// names, their categories and the allowlist flag attached to each item. The
// phone caches its last listing and re-downloads it only when this changes, so
// a card with hundreds of songs costs one round trip per connect instead of
// dozens of pages.
uint32_t catalogRev() {
    const auto snapshot = s_catalog;
    if (!snapshot) return 0;
    const auto record = parentalRecord();
    // Only the allowlist switch and the two masks change the "a" flags a listing
    // carries; the other PCTL flags (remote/child lock, bedtime) must not force a
    // re-download.
    const uint64_t allowlist = (record.flags & 0x08u) ? 1ULL : 0ULL;
    const uint64_t masks = record.story_mask * 0x9E3779B97F4A7C15ULL ^
                           record.category_mask * 0xC2B2AE3D27D4EB4FULL ^
                           allowlist;
    // Hashing a few hundred names is cheap but not free, and every page would do
    // it. The generation counter bumps on each catalog publication, so it works
    // as the cache key together with the allowlist masks.
    static uint64_t cached_generation = 0;
    static uint64_t cached_masks = 0;
    static uint32_t cached_rev = 0;
    if (cached_rev && cached_generation == snapshot->generation && cached_masks == masks) {
        return cached_rev;
    }
    uint32_t hash = 2166136261u;
    auto mix = [&hash](uint32_t value) { hash = (hash ^ value) * 16777619u; };
    auto mixText = [&mix](const std::string& text) {
        for (unsigned char c : text) mix(c);
        mix(0x1fu);   // separator: "ab"+"c" must not hash like "a"+"bc"
    };
    mix(static_cast<uint32_t>(record.story_mask));
    mix(static_cast<uint32_t>(record.story_mask >> 32));
    mix(static_cast<uint32_t>(record.category_mask));
    mix(static_cast<uint32_t>(record.category_mask >> 32));
    mix(static_cast<uint32_t>(allowlist));
    for (const auto& story : snapshot->stories) mixText(story.title);
    for (const auto& track : snapshot->tracks) {
        mixText(track.name);
        mix(static_cast<uint32_t>(track.category + 1));
    }
    if (!hash) hash = 1;
    cached_generation = snapshot->generation;
    cached_masks = masks;
    cached_rev = hash;
    return hash;
}

uint32_t bleCatalogRev(void*) { return catalogRev(); }

// "get stories|music|cats|report": one page of a catalog listing. The phone
// picks the page size from the negotiated MTU; this is only the upper bound that
// keeps one response from growing without limit.
constexpr int kListPageMax = 24;

bool bleListPage(void*, const char* what, const cJSON* request, cJSON* data) {
    const cJSON* offset_item = cJSON_GetObjectItemCaseSensitive(request, "offset");
    const cJSON* limit_item = cJSON_GetObjectItemCaseSensitive(request, "limit");
    int offset = cJSON_IsNumber(offset_item) ? static_cast<int>(offset_item->valuedouble) : 0;
    int limit = cJSON_IsNumber(limit_item) ? static_cast<int>(limit_item->valuedouble) : 4;
    if (limit < 1) limit = 1;
    if (limit > kListPageMax) limit = kListPageMax;
    if (offset < 0) offset = 0;
    const auto snapshot = s_catalog;

    // Cheap "what does the phone need to fetch?" probe: no item payload, just the
    // revision and the sizes.
    if (std::strcmp(what, "meta") == 0) {
        cJSON_AddNumberToObject(data, "rev", catalogRev());
        cJSON_AddNumberToObject(data, "tracks", snapshot ? static_cast<double>(snapshot->tracks.size()) : 0);
        cJSON_AddNumberToObject(data, "stories", snapshot ? static_cast<double>(snapshot->stories.size()) : 0);
        cJSON_AddNumberToObject(data, "cats", snapshot ? static_cast<double>(snapshot->categories.size()) : 0);
        return true;
    }
    if (std::strcmp(what, "stories") == 0) {
        const int total = snapshot ? static_cast<int>(snapshot->stories.size()) : 0;
        if (offset > total) offset = total;
        cJSON_AddNumberToObject(data, "rev", catalogRev());
        cJSON_AddNumberToObject(data, "total", total);
        cJSON_AddNumberToObject(data, "offset", offset);
        cJSON* items = cJSON_AddArrayToObject(data, "items");
        for (int i = offset; i < total && i < offset + limit; ++i) {
            const auto& story = snapshot->stories[static_cast<std::size_t>(i)];
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "i", i);
            cJSON_AddStringToObject(item, "n", story.title.c_str());
            cJSON_AddBoolToObject(item, "a", storyAllowed(static_cast<std::size_t>(i)));
            cJSON_AddItemToArray(items, item);
        }
        return true;
    }
    if (std::strcmp(what, "music") == 0) {
        const int total = snapshot ? static_cast<int>(snapshot->tracks.size()) : 0;
        if (offset > total) offset = total;
        cJSON_AddNumberToObject(data, "rev", catalogRev());
        cJSON_AddNumberToObject(data, "total", total);
        cJSON_AddNumberToObject(data, "offset", offset);
        cJSON* items = cJSON_AddArrayToObject(data, "items");
        for (int i = offset; i < total && i < offset + limit; ++i) {
            const auto& track = snapshot->tracks[static_cast<std::size_t>(i)];
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "i", i);
            cJSON_AddStringToObject(item, "n", track.name.c_str());
            cJSON_AddStringToObject(item, "c", categoryName(track.category));
            cJSON_AddNumberToObject(item, "ci", track.category);
            cJSON_AddBoolToObject(item, "a", categoryAllowed(track.category));
            cJSON_AddBoolToObject(item, "f", favorite(static_cast<std::size_t>(i)));
            cJSON_AddItemToArray(items, item);
        }
        return true;
    }
    if (std::strcmp(what, "cats") == 0) {
        // Category 0 is "all songs" and is not a configurable entry.
        const int total = snapshot ? static_cast<int>(snapshot->categories.size()) : 0;
        if (offset > total) offset = total;
        cJSON_AddNumberToObject(data, "rev", catalogRev());
        cJSON_AddNumberToObject(data, "total", total);
        cJSON_AddNumberToObject(data, "offset", offset);
        cJSON* items = cJSON_AddArrayToObject(data, "items");
        for (int i = offset; i < total && i < offset + limit; ++i) {
            const int actual = i + 1;
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "i", actual);
            cJSON_AddStringToObject(item, "n", snapshot->categories[static_cast<std::size_t>(i)].c_str());
            cJSON_AddNumberToObject(item, "c", categoryTrackCount(actual));
            cJSON_AddBoolToObject(item, "a", categoryAllowed(actual));
            cJSON_AddItemToArray(items, item);
        }
        return true;
    }
    if (std::strcmp(what, "report") == 0) {
        const auto record = parentalRecord();
        cJSON_AddNumberToObject(data, "today", static_cast<double>(record.used));
        cJSON_AddNumberToObject(data, "day", static_cast<double>(record.day));
        cJSON_AddNumberToObject(data, "limit", static_cast<double>(record.daily_limit) * 60.0);
        cJSON* history = cJSON_AddArrayToObject(data, "history");
        const unsigned count = record.history_count > 7 ? 7 : record.history_count;
        for (unsigned i = 0; i < count; ++i) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddNumberToObject(item, "day", static_cast<double>(record.history[i].day));
            cJSON_AddNumberToObject(item, "used", static_cast<double>(record.history[i].used));
            cJSON_AddItemToArray(history, item);
        }
        return true;
    }
    return false;
}

bool bleToggleFavorite(void*, int32_t index, bool* now, int32_t* total) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (!s_records_store || index < 0 || index >= 300) return false;
    const auto slot = static_cast<unsigned>(index);
    const bool wanted = !s_records_store->favorite(slot);
    if (!s_records_store->setFavorite(slot, wanted)) return false;
    int count = 0;
    for (unsigned i = 0; i < 300; ++i) {
        if (s_records_store->favorite(i)) ++count;
    }
    if (now) *now = wanted;
    if (total) *total = count;
    return true;
#else
    (void)index;
    (void)now;
    (void)total;
    return false;
#endif
}

const char* bleDeleteMusic(void*, int32_t index) {
    if (!s_context.sd_mounted) return "NO_SD";
    const auto snapshot = s_catalog;
    if (!snapshot || index < 0 || index >= static_cast<int>(snapshot->tracks.size())) return "RANGE";
    const auto& track = snapshot->tracks[static_cast<std::size_t>(index)];
    // Release the audio/video pipeline first: the stream keeps a file lease for
    // the whole track, so unlinking underneath it can fail on FAT.
    Video::close();
    setVideoActive(false);
    setCurrentVideo(-1);
    setNowPlaying("idle", "");
    g_playback.stopAndWait(3000);
    g_playback.allowPlayback();
    if (!g_gate.acquire()) return "BUSY";
    const std::string path = std::string(Storage::SdVolume::root) + track.path;
    const int removed = std::remove(path.c_str());
    g_gate.release();
    if (removed != 0) {
        ESP_LOGW(kTag, "delete %s failed", path.c_str());
        return "IO";
    }
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    // Catalog indices shift after the rescan; keep the favourite bitmap aligned.
    if (s_records_store) s_records_store->removeFavoriteAt(static_cast<unsigned>(index));
#endif
    if (!refreshCatalog()) return "RESCAN";
    ESP_LOGI(kTag, "deleted %s", track.path.c_str());
    return nullptr;
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
    hooks.bind_code = bleBindCode;
    hooks.status = bleStatus;
    hooks.apply_config = bleApplyConfig;
    hooks.set_pin = bleSetPin;
    hooks.factory_reset = bleFactoryReset;
    hooks.unbind = bleUnbind;
    hooks.gate = bleGate;
    hooks.list = bleListPage;
    hooks.toggle_favorite = bleToggleFavorite;
    hooks.delete_music = bleDeleteMusic;
    hooks.catalog_rev = bleCatalogRev;
    return hooks;
}

// USB route for "cmd":"nfc". Requests are marshalled onto the NFC owner task;
// the raw Pn532/Nfc state is never touched from the protocol worker. Actions
// match the Arduino firmware: status/poll/refresh/read/write_ndef/write_raw.
#if CONFIG_STORYBOX_USB_PROTOCOL
void addSystemStats(cJSON* data) {
    const auto state = g_playback.snapshot();
    const auto* track = g_playlist.current();
    const auto catalog_now = catalog();
    const uint32_t seconds = state.sample_rate ? static_cast<uint32_t>(state.submitted_frames / state.sample_rate) : 0;

    cJSON_AddNumberToObject(data, "uptime", static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON* cpu = cJSON_AddObjectToObject(data, "cpu");
    // CPU load needs FreeRTOS run-time stats; report 0 until that is enabled.
    cJSON_AddNumberToObject(cpu, "load", 0);
    cJSON_AddBoolToObject(cpu, "loadValid", false);
    cJSON_AddNumberToObject(cpu, "cores", portNUM_PROCESSORS);
    cJSON_AddNumberToObject(cpu, "freq", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    esp_chip_info_t chip{};
    esp_chip_info(&chip);
    cJSON_AddStringToObject(cpu, "model", "ESP32-S3");
    cJSON_AddNumberToObject(cpu, "revision", chip.revision);
    cJSON_AddStringToObject(cpu, "sdk", esp_get_idf_version());
    const auto video = Video::status();
    const auto display = Display::stats();
    cJSON* perf = cJSON_AddObjectToObject(data, "performance");
    cJSON_AddNumberToObject(perf, "videoStopAudioMs", video.stop_audio_ms);
    cJSON_AddNumberToObject(perf, "videoQueueMs", video.queue_ms);
    cJSON_AddNumberToObject(perf, "videoCloseMs", video.close_ms);
    cJSON_AddNumberToObject(perf, "videoCommandQueueMs", video.command_queue_ms);
    cJSON_AddNumberToObject(perf, "videoCommandCloseMs", video.command_close_ms);
    cJSON_AddNumberToObject(perf, "videoControlStackMinBytes", video.control_stack_min_bytes);
    cJSON_AddNumberToObject(perf, "videoSetupMs", video.setup_ms);
    cJSON_AddNumberToObject(perf, "videoRunMs", video.run_ms);
    cJSON_AddNumberToObject(perf, "videoFirstBackendFrameMs", video.first_frame_ms);
    cJSON_AddNumberToObject(perf, "videoBackendCopyUs", video.backend_copy_us);
    cJSON_AddNumberToObject(perf, "videoBackendFrames", video.frames);
    cJSON_AddNumberToObject(perf, "videoReplacedFrames", video.replaced_frames);
    cJSON_AddNumberToObject(perf, "lcdSubmissions", display.submissions);
    cJSON_AddNumberToObject(perf, "lcdVideoSubmissions", display.video_submissions);
    cJSON_AddNumberToObject(perf, "lcdBytes", display.bytes);
    cJSON_AddNumberToObject(perf, "lcdLastUs", display.last_us);
    cJSON_AddNumberToObject(perf, "lcdMaxUs", display.max_us);
    cJSON_AddNumberToObject(perf, "audioDecodeUs", state.decode_us);
    cJSON_AddNumberToObject(perf, "audioWriteUs", state.write_us);
    cJSON_AddNumberToObject(perf, "audioBudgetYields", state.budget_yields);

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
                            catalog_now ? static_cast<double>(catalog_now->tracks.size()) : 0);
    cJSON_AddNumberToObject(storage, "stories",
                            catalog_now ? static_cast<double>(catalog_now->stories.size()) : 0);

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
    cJSON_AddBoolToObject(settings, "nfc", s_context.settings.nfc != 0);
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

// ---- NFC status and card serialisation (same shape as the Arduino hostlink) --

void hexText(const uint8_t* data, std::size_t length, char* out, std::size_t capacity) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    if (!out || !capacity) return;
    std::size_t used = 0;
    for (std::size_t i = 0; i < length && used + 2 < capacity; ++i) {
        out[used++] = kDigits[data[i] >> 4];
        out[used++] = kDigits[data[i] & 0x0F];
    }
    out[used] = '\0';
}

int hexNibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

// Accepts the same separators as the Arduino protocol (spaces, ':' and '-').
int hexDecode(const char* text, uint8_t* out, std::size_t capacity) {
    if (!text) return -1;
    std::size_t used = 0;
    int high = -1;
    for (const char* p = text; *p; ++p) {
        const char value = *p;
        if (value == ' ' || value == ':' || value == '-') continue;
        const int nibble = hexNibble(value);
        if (nibble < 0) return -1;
        if (high < 0) {
            high = nibble;
        } else {
            if (used >= capacity) return -1;
            out[used++] = static_cast<uint8_t>((high << 4) | nibble);
            high = -1;
        }
    }
    if (high >= 0) return -1;
    return static_cast<int>(used);
}

void addNfcCard(cJSON* card, const Nfc::Card& state) {
    char text[sizeof(state.user) * 2 + 1];
    hexText(state.uid, state.uidLen, text, sizeof(text));
    cJSON_AddStringToObject(card, "uid", text);
    cJSON_AddNumberToObject(card, "uidLen", state.uidLen);
    char atqa[8];
    std::snprintf(atqa, sizeof(atqa), "%04X", static_cast<unsigned>(state.atqa));
    cJSON_AddStringToObject(card, "atqa", atqa);
    char sak[8];
    std::snprintf(sak, sizeof(sak), "%02X", static_cast<unsigned>(state.sak));
    cJSON_AddStringToObject(card, "sak", sak);
    cJSON_AddStringToObject(card, "kind", Nfc::kindId(state.kind));
    cJSON_AddStringToObject(card, "type", Nfc::kindName(state.kind));
    cJSON_AddStringToObject(card, "std", Nfc::standardName(state.kind));
    cJSON_AddBoolToObject(card, "type2", state.type2);
    cJSON_AddBoolToObject(card, "writable", state.writable);
    cJSON_AddBoolToObject(card, "formatted", state.formatted);
    cJSON_AddNumberToObject(card, "cap", state.capacity);
    cJSON_AddNumberToObject(card, "userStart", state.userStart);
    cJSON_AddNumberToObject(card, "userPages", state.userPages);
    cJSON_AddNumberToObject(card, "userTotal", state.userTotal);
    cJSON_AddNumberToObject(card, "userBytes", state.userBytes);
    hexText(state.version, state.versionOk ? state.versionLen : 0, text, sizeof(text));
    cJSON_AddStringToObject(card, "version", text);
    hexText(state.cc, state.ccValid ? 4 : 0, text, sizeof(text));
    cJSON_AddStringToObject(card, "cc", text);
    hexText(state.lock, sizeof(state.lock), text, sizeof(text));
    cJSON_AddStringToObject(card, "lock", text);
    hexText(state.user, state.userBytes, text, sizeof(text));
    cJSON_AddStringToObject(card, "data", text);
    if (state.hasBlock0) {
        hexText(state.block0, 16, text, sizeof(text));
        cJSON_AddStringToObject(card, "block0", text);
    }
}

// ---- Media catalog pages for the desktop resource picker -------------------
// The catalog is already in RAM (boot cache or the first library scan), so
// this avoids re-walking a slow/corrupt SD directory just to list resources.
unsigned mediaParam(const cJSON* request, const char* name, unsigned fallback) {
    const auto* item = cJSON_GetObjectItemCaseSensitive(request, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > 100000) return fallback;
    return static_cast<unsigned>(item->valuedouble);
}

void addMediaInfo(cJSON* data, const Media::Catalog& catalog, bool ready) {
    cJSON_AddBoolToObject(data, "ready", ready);
    cJSON_AddNumberToObject(data, "trackTotal", static_cast<double>(catalog.tracks.size()));
    cJSON_AddNumberToObject(data, "storyTotal", static_cast<double>(catalog.stories.size()));
    auto* categories = cJSON_AddArrayToObject(data, "categories");
    for (const auto& name : catalog.categories) {
        if (auto* item = cJSON_CreateString(name.c_str())) cJSON_AddItemToArray(categories, item);
    }
}

void addMediaTracks(cJSON* data, const Media::Catalog& catalog, unsigned offset, unsigned limit) {
    limit = std::min<unsigned>(limit ? limit : 12, 16);
    const unsigned total = static_cast<unsigned>(catalog.tracks.size());
    const unsigned start = std::min(offset, total);
    const unsigned count = std::min(limit, total - start);
    cJSON_AddNumberToObject(data, "total", static_cast<double>(total));
    cJSON_AddNumberToObject(data, "offset", static_cast<double>(start));
    cJSON_AddNumberToObject(data, "limit", static_cast<double>(limit));
    auto* rows = cJSON_AddArrayToObject(data, "tracks");
    for (unsigned i = 0; i < count; ++i) {
        const auto& track = catalog.tracks[start + i];
        auto* row = cJSON_CreateArray();
        if (!row) continue;
        cJSON_AddItemToArray(row, cJSON_CreateString(track.path.c_str()));
        cJSON_AddItemToArray(row, cJSON_CreateString(track.name.c_str()));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(track.id));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(track.category));
        cJSON_AddItemToArray(rows, row);
    }
}

void addMediaStories(cJSON* data, const Media::Catalog& catalog, unsigned offset, unsigned limit) {
    limit = std::min<unsigned>(limit ? limit : 8, 12);
    const unsigned total = static_cast<unsigned>(catalog.stories.size());
    const unsigned start = std::min(offset, total);
    const unsigned count = std::min(limit, total - start);
    cJSON_AddNumberToObject(data, "total", static_cast<double>(total));
    cJSON_AddNumberToObject(data, "offset", static_cast<double>(start));
    cJSON_AddNumberToObject(data, "limit", static_cast<double>(limit));
    auto* rows = cJSON_AddArrayToObject(data, "stories");
    for (unsigned i = 0; i < count; ++i) {
        const auto& story = catalog.stories[start + i];
        auto* row = cJSON_CreateArray();
        if (!row) continue;
        cJSON_AddItemToArray(row, cJSON_CreateString(story.audio.c_str()));
        cJSON_AddItemToArray(row, cJSON_CreateString(story.timeline.c_str()));
        cJSON_AddItemToArray(row, cJSON_CreateString(story.directory.c_str()));
        cJSON_AddItemToArray(row, cJSON_CreateString(story.category.c_str()));
        cJSON_AddItemToArray(rows, row);
    }
}

void addNfcStatus(cJSON* object, NativeNfc::Service& service) {
    const auto state = service.snapshot();
    cJSON_AddBoolToObject(object, "enabled", state.enabled);
    cJSON_AddBoolToObject(object, "present", state.module);
    cJSON_AddStringToObject(object, "chip", "PN532");
    cJSON_AddStringToObject(object, "fw", state.firmware);
    cJSON_AddStringToObject(object, "i2c", "0x24");
    cJSON_AddBoolToObject(object, "polling", state.polling);
    cJSON_AddBoolToObject(object, "busy", state.busy);
    cJSON_AddNumberToObject(object, "uptime", static_cast<double>(esp_timer_get_time() / 1000));
    cJSON_AddNumberToObject(object, "heap", static_cast<double>(esp_get_free_heap_size()));
    if (state.card.valid) {
        addNfcCard(cJSON_AddObjectToObject(object, "card"), state.card);
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
            const cJSON* eq = cJSON_GetObjectItemCaseSensitive(request, "eq");
            const cJSON* rotation = cJSON_GetObjectItemCaseSensitive(request, "rotation");
            const cJSON* nfc = cJSON_GetObjectItemCaseSensitive(request, "nfc");
            if (cJSON_IsNumber(volume)) setVolume(static_cast<int>(volume->valuedouble));
            if (cJSON_IsNumber(brightness)) {
                int value = static_cast<int>(brightness->valuedouble);
                if (value < 10) value = 10;
                if (value > 100) value = 100;
                s_context.settings.brightness = static_cast<uint8_t>(value);
                Display::setBrightness(s_context.settings.brightness);
                saveSettings();
            }
            if (cJSON_IsNumber(eq)) {
                const int value = static_cast<int>(eq->valuedouble);
                if (value >= 0 && value <= 3) {
                    applyEq(static_cast<uint8_t>(value));
                    saveSettings();
                }
            }
            if (cJSON_IsNumber(rotation)) {
                const int value = static_cast<int>(rotation->valuedouble);
                if (value == 1 || value == 3) {
                    s_context.settings.rotation = static_cast<uint8_t>(value);
                    Display::setRotation(s_context.settings.rotation);
                    saveSettings();
                }
            }
            if (cJSON_IsBool(nfc)) {
                if (!applyNfcEnabled(cJSON_IsTrue(nfc))) {
                    *error = "NFC_BUSY";
                    return true;
                }
                saveSettings();
            }
        } else if (std::strcmp(action, "reboot") == 0) {
            // Let the response leave the USB queue before restarting.
            static esp_timer_handle_t restart_timer = nullptr;
            if (!restart_timer) {
                const esp_timer_create_args_t args = {
                    .callback = [](void*) { esp_restart(); },
                    .arg = nullptr,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "ota-reboot",
                    .skip_unhandled_events = false,
                };
                esp_timer_create(&args, &restart_timer);
            }
            if (restart_timer) esp_timer_start_once(restart_timer, 500000);
        } else if (std::strcmp(action, "rescan") == 0) {
            // Rebuild the media index on demand (hosts that changed the card
            // outside the protocol need this).
            if (!s_context.sd_mounted) {
                *error = "NO_SD";
                return true;
            }
            if (!refreshCatalog()) {
                *error = "RESCAN_FAILED";
                return true;
            }
            s_context.catalog_ready = true;
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
    if (std::strcmp(command, "media") == 0) {
        cJSON_AddStringToObject(response, "cmd", "media");
        cJSON_AddStringToObject(response, "act", action ? action : "");
        if (!action) {
            *error = "BAD_ACT";
            return true;
        }
        const auto snapshot = catalog();
        if (std::strcmp(action, "info") == 0) {
            auto* data = cJSON_AddObjectToObject(response, "data");
            if (snapshot) {
                addMediaInfo(data, *snapshot, s_context.catalog_ready);
            } else {
                cJSON_AddBoolToObject(data, "ready", false);
                cJSON_AddNumberToObject(data, "trackTotal", 0);
                cJSON_AddNumberToObject(data, "storyTotal", 0);
                cJSON_AddArrayToObject(data, "categories");
            }
            return true;
        }
        if (!snapshot || !s_context.catalog_ready) {
            *error = "MEDIA_NOT_READY";
            return true;
        }
        if (std::strcmp(action, "tracks") == 0) {
            auto* data = cJSON_AddObjectToObject(response, "data");
            addMediaTracks(data, *snapshot, mediaParam(request, "offset", 0),
                           mediaParam(request, "limit", 24));
            return true;
        }
        if (std::strcmp(action, "stories") == 0) {
            auto* data = cJSON_AddObjectToObject(response, "data");
            addMediaStories(data, *snapshot, mediaParam(request, "offset", 0),
                            mediaParam(request, "limit", 12));
            return true;
        }
        *error = "BAD_ACT";
        return true;
    }
    if (std::strcmp(command, "nfc") == 0) {
        cJSON_AddStringToObject(response, "cmd", "nfc");
        cJSON_AddStringToObject(response, "act", action ? action : "");
        auto* service = s_nfc.load(std::memory_order_acquire);
        if (!service) {
            *error = "NFC_UNAVAILABLE";
            return true;
        }
        if (!action) {
            *error = "BAD_ACT";
            return true;
        }
        // The device settings switch is a master gate: status stays readable,
        // every other action reports the switch instead of pretending to work.
        if (!service->enabled() && std::strcmp(action, "status") != 0) {
            *error = "NFC_DISABLED";
            return true;
        }
        if (std::strcmp(action, "status") == 0) {
            addNfcStatus(cJSON_AddObjectToObject(response, "data"), *service);
            return true;
        }
        if (std::strcmp(action, "poll") == 0 || std::strcmp(action, "start") == 0 ||
            std::strcmp(action, "stop") == 0) {
            // stop always disables, even if the client also sent on:true.
            bool on = true;
            if (std::strcmp(action, "stop") == 0) {
                on = false;
            } else {
                const cJSON* requested = cJSON_GetObjectItemCaseSensitive(request, "on");
                if (cJSON_IsBool(requested)) on = cJSON_IsTrue(requested);
            }
            if (!service->pollNow(on)) {
                *error = "BUSY";
                return true;
            }
            addNfcStatus(cJSON_AddObjectToObject(response, "data"), *service);
            return true;
        }
        if (std::strcmp(action, "refresh") == 0) {
            const char* failure = "";
            if (!service->refresh(&failure)) {
                *error = (failure && failure[0]) ? failure : "BUSY";
                return true;
            }
            return true;
        }
        if (std::strcmp(action, "read") == 0) {
            const cJSON* kind_item = cJSON_GetObjectItemCaseSensitive(request, "kind");
            const char* kind = cJSON_IsString(kind_item) ? kind_item->valuestring : "page";
            const bool block = std::strcmp(kind, "block") == 0;
            int start = 0;
            const cJSON* start_item = cJSON_GetObjectItemCaseSensitive(request, "start");
            if (cJSON_IsNumber(start_item)) {
                const double value = start_item->valuedouble;
                if (!(value >= 0) || value > 255) {
                    *error = "RANGE";
                    return true;
                }
                start = static_cast<int>(value);
            }
            int count = 8;
            const cJSON* count_item = cJSON_GetObjectItemCaseSensitive(request, "count");
            if (cJSON_IsNumber(count_item)) {
                const double value = count_item->valuedouble;
                if (!(value >= 1)) count = 1;
                else if (value >= 64) count = 64;
                else count = static_cast<int>(value);
            }
            uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            bool has_key = false;
            const cJSON* key_item = cJSON_GetObjectItemCaseSensitive(request, "key");
            if (cJSON_IsString(key_item) && key_item->valuestring[0]) {
                if (hexDecode(key_item->valuestring, key, sizeof(key)) != 6) {
                    *error = "BAD_KEY";
                    return true;
                }
                has_key = true;
            }
            Nfc::RawResult result;
            if (!service->readRaw(block, static_cast<uint16_t>(start), static_cast<uint16_t>(count),
                                  key, has_key, result)) {
                *error = result.error[0] ? result.error : "READ";
                return true;
            }
            char hex[2 * 256 + 1];
            hexText(result.data, result.len, hex, sizeof(hex));
            cJSON* data = cJSON_AddObjectToObject(response, "data");
            cJSON_AddStringToObject(data, "kind", block ? "block" : "page");
            cJSON_AddNumberToObject(data, "start", start);
            cJSON_AddNumberToObject(data, "len", result.len);
            cJSON_AddStringToObject(data, "hex", hex);
            return true;
        }
        if (std::strcmp(action, "write_ndef") == 0) {
            const cJSON* ndef = cJSON_GetObjectItemCaseSensitive(request, "ndef");
            if (!cJSON_IsString(ndef)) {
                *error = "BAD_HEX";
                return true;
            }
            // One USB worker executes routes, so this staging buffer is private.
            static uint8_t buffer[1024];
            const int length = hexDecode(ndef->valuestring, buffer, sizeof(buffer));
            if (length <= 0) {
                *error = "BAD_HEX";
                return true;
            }
            bool verify = true;
            const cJSON* verify_item = cJSON_GetObjectItemCaseSensitive(request, "verify");
            if (cJSON_IsBool(verify_item)) verify = cJSON_IsTrue(verify_item);
            const char* failure = "";
            if (!service->startWriteNdef(buffer, static_cast<uint16_t>(length), verify, &failure)) {
                *error = (failure && failure[0]) ? failure : "WRITE";
                return true;
            }
            cJSON_AddBoolToObject(response, "accepted", true);
            cJSON_AddNumberToObject(response, "len", length);
            return true;
        }
        if (std::strcmp(action, "write_raw") == 0) {
            const cJSON* kind_item = cJSON_GetObjectItemCaseSensitive(request, "kind");
            const char* kind = cJSON_IsString(kind_item) ? kind_item->valuestring : "page";
            const bool block = std::strcmp(kind, "block") == 0;
            int start = 0;
            const cJSON* start_item = cJSON_GetObjectItemCaseSensitive(request, "start");
            if (cJSON_IsNumber(start_item)) {
                const double value = start_item->valuedouble;
                if (!(value >= 0) || value > 255) {
                    *error = "RANGE";
                    return true;
                }
                start = static_cast<int>(value);
            }
            const cJSON* data_item = cJSON_GetObjectItemCaseSensitive(request, "data");
            if (!cJSON_IsString(data_item)) {
                *error = "BAD_HEX";
                return true;
            }
            static uint8_t buffer[256];
            const int length = hexDecode(data_item->valuestring, buffer, sizeof(buffer));
            if (length <= 0) {
                *error = "BAD_HEX";
                return true;
            }
            uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            bool has_key = false;
            const cJSON* key_item = cJSON_GetObjectItemCaseSensitive(request, "key");
            if (cJSON_IsString(key_item) && key_item->valuestring[0]) {
                if (hexDecode(key_item->valuestring, key, sizeof(key)) != 6) {
                    *error = "BAD_KEY";
                    return true;
                }
                has_key = true;
            }
            const char* failure = "";
            if (!service->startWriteRaw(block, static_cast<uint16_t>(start), buffer,
                                        static_cast<uint16_t>(length), key, has_key, &failure)) {
                *error = (failure && failure[0]) ? failure : "WRITE";
                return true;
            }
            cJSON_AddBoolToObject(response, "accepted", true);
            cJSON_AddNumberToObject(response, "len", length);
            return true;
        }
        *error = "BAD_ACT";
        return true;
    }
    return false;
}

#if CONFIG_STORYBOX_NFC
const char* nfcWritePhase(Nfc::WritePhase phase) {
    switch (phase) {
        case Nfc::WritePhase::Start:     return "start";
        case Nfc::WritePhase::Writing:   return "writing";
        case Nfc::WritePhase::Verifying: return "verifying";
        case Nfc::WritePhase::Done:      return "done";
        case Nfc::WritePhase::Error:     return "error";
        default:                         return "unknown";
    }
}

// Turn one queued NFC state change into an unsolicited SBX4 "evt":"nfc" frame.
// The application task is the only event consumer, which keeps the JSON
// identical to the Arduino protocol so the desktop workbench is unchanged.
void publishNfcEventToHost(const Nfc::Event& event, NativeNfc::Service& service) {
        cJSON* doc = cJSON_CreateObject();
        if (!doc) return;
        cJSON_AddNumberToObject(doc, "v", 4);
        cJSON_AddStringToObject(doc, "evt", "nfc");
        bool publish = true;
        switch (event.type) {
            case Nfc::EventType::Module: {
                // Read the module state through the mutex-protected snapshot:
                // Pn532's firmware text belongs to the NFC task.
                const auto state = service.snapshot();
                cJSON_AddStringToObject(doc, "type", "module");
                cJSON_AddBoolToObject(doc, "present", event.modulePresent);
                cJSON_AddStringToObject(doc, "chip", Nfc::moduleName());
                cJSON_AddStringToObject(doc, "fw", state.firmware);
                cJSON_AddStringToObject(doc, "i2c", "0x24");
                cJSON_AddBoolToObject(doc, "polling", state.polling);
                break;
            }
            case Nfc::EventType::Card:
                cJSON_AddStringToObject(doc, "type", "card");
                addNfcCard(cJSON_AddObjectToObject(doc, "card"), event.card);
                break;
            case Nfc::EventType::Removed:
                cJSON_AddStringToObject(doc, "type", "removed");
                break;
            case Nfc::EventType::Write:
                cJSON_AddStringToObject(doc, "type", "write");
                cJSON_AddStringToObject(doc, "phase", nfcWritePhase(event.phase));
                cJSON_AddNumberToObject(doc, "pct", event.percent);
                cJSON_AddNumberToObject(doc, "written", event.written);
                cJSON_AddBoolToObject(doc, "ok", event.ok);
                if (event.code && event.code[0]) cJSON_AddStringToObject(doc, "code", event.code);
                if (event.message[0]) cJSON_AddStringToObject(doc, "msg", event.message);
                break;
            default:
                publish = false;
                break;
        }
        if (publish) {
            char* text = cJSON_PrintUnformatted(doc);
            if (text) {
                Host::publishJson(text, std::strlen(text));
                cJSON_free(text);
            }
        }
        cJSON_Delete(doc);
}
#endif  // CONFIG_STORYBOX_NFC
#endif  // CONFIG_STORYBOX_USB_PROTOCOL

#if CONFIG_STORYBOX_NFC
// A resource card is the only thing that starts playback by itself. A host that
// is actively sending commands (the desktop workbench) keeps control; a
// standalone device with only a charger sees no host traffic and plays.
bool nfcAutoPlayAllowed() {
#if CONFIG_STORYBOX_USB_PROTOCOL
    if (Host::recentlyActive(10000)) return false;
#endif
    return !Video::active() && !Video::busy();
}

void playNfcResource(const char* path) {
    if (!path || (std::strncmp(path, "/music/", 7) != 0 && std::strncmp(path, "/story/", 7) != 0)) {
        return;
    }
    const auto snapshot = catalog();
    if (!snapshot) return;

    for (std::size_t i = 0; i < snapshot->tracks.size(); ++i) {
        const auto& track = snapshot->tracks[i];
        if (track.path != path) continue;
        if (g_playlist.select(snapshot, track.category) && g_playlist.seekPath(track.path)) {
            setNowPlaying("music", track.name.c_str());
            playCurrent(true);
            ESP_LOGI(kTag, "NFC play music: %s", track.path.c_str());
        }
        return;
    }

    for (std::size_t i = 0; i < snapshot->stories.size(); ++i) {
        if (snapshot->stories[i].audio != path) continue;
        openStory(i);
        ESP_LOGI(kTag, "NFC play story: %s", path);
        return;
    }
    ESP_LOGW(kTag, "NFC resource not in catalog: %s", path);
}

void handleNfcCardEvent(const Nfc::Card& card) {
    if (!nfcAutoPlayAllowed()) return;
    char uri[384];
    if (!Nfc::ndefUri(card, uri, sizeof(uri))) return;
    static constexpr char kPrefix[] = "storybox://media?path=";
    if (std::strncmp(uri, kPrefix, sizeof(kPrefix) - 1) != 0) return;
    playNfcResource(uri + sizeof(kPrefix) - 1);
}

void handleNfcEvents() {
    auto* service = s_nfc.load(std::memory_order_acquire);
    if (!service) return;
    Nfc::Event event;
    for (int budget = 0; budget < 6 && Nfc::takeEvent(event); ++budget) {
#if CONFIG_STORYBOX_USB_PROTOCOL
        publishNfcEventToHost(event, *service);
#endif
        if (event.type == Nfc::EventType::Card) handleNfcCardEvent(event.card);
    }
}
#endif  // CONFIG_STORYBOX_NFC

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
                pausePlayback(Video::active() ? Video::status().state == Video::State::Playing :
                              g_playback.snapshot().state == Audio::PlaybackState::Playing);
                break;
            case Ble::Action::Next:
                if (Video::active() && s_depth) {
                    s_stack[s_depth - 1]->handle(Event{Key::Next, 0});
                    break;
                }
                if (g_playlist.move(1)) playCurrent(true);
                break;
            case Ble::Action::Prev:
                if (Video::active() && s_depth) {
                    s_stack[s_depth - 1]->handle(Event{Key::Prev, 0});
                    break;
                }
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
    const auto video = Video::status();
    if (Video::active()) {
        snapshot.playing = video.state == Video::State::Playing;
        snapshot.paused = video.state == Video::State::Paused;
    }
    snapshot.volume = s_context.volume;
    const auto* track = g_playlist.current();
    const bool active = snapshot.playing || snapshot.paused;
    snapshot.mode = active ? s_now_mode : "idle";
    if (active && s_now_title[0]) snapshot.title = s_now_title;
    else snapshot.title = track ? track->name.c_str() : "";
    snapshot.current = state.sample_rate ? static_cast<uint32_t>(state.submitted_frames / state.sample_rate) : 0;
    snapshot.duration = state.duration;
    if (Video::active()) {
        snapshot.current = static_cast<uint32_t>(video.position_ms / 1000);
        snapshot.duration = static_cast<uint32_t>(video.duration_ms / 1000);
    }
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
    logHeap("app task start");
    Display::begin();
    logHeap("after display");
    Fonts::begin();
    logHeap("after fonts");
    Input::begin();
    Hall::begin();
    Ble::begin("StoryBox", makeBleHooks());
    logHeap("after ble");
    Power::begin();
    Theme::init();
    drawBoot("正在启动", "");
    Display::present(true);
    logHeap("after boot screen");

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
            ESP_LOGI(kTag, "FRAM ready: brightness=%u rotation=%u eq=%u volume=%d favorites=%s",
                     static_cast<unsigned>(s_context.settings.brightness),
                     static_cast<unsigned>(s_context.settings.rotation),
                     static_cast<unsigned>(s_context.settings.eq), s_context.volume,
                     s_records_store ? "yes" : "no");
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

    // The Bluetooth broadcast flag is persisted like any other setting; apply it
    // once the record is loaded (Ble::begin() starts advertising unconditionally).
    Ble::setAdvertisingEnabled(s_context.settings.ble != 0);

#if CONFIG_STORYBOX_NFC
    static NativeNfc::Service nfc;
    const bool nfc_enabled = s_context.settings.nfc != 0;
    if (bus_ready && nfc.begin(bus.handle(), nfc_enabled) == ESP_OK) {
        // The host starts polling with nfc.poll, exactly like the Arduino
        // firmware; without a host the radio stays quiet on the shared I2C bus.
        s_nfc.store(&nfc, std::memory_order_release);
        // Poll from boot so a card works without a PC; an active workbench
        // suppresses the resulting auto-play separately.
        if (nfc_enabled) nfc.setPolling(true);
        ESP_LOGI(kTag, "NFC service %s", nfc_enabled ? "ready" : "disabled by settings");
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
    constexpr const char* kHostFeature = "nfc,nfc.ndef,nfc.raw,nfc.write-verify,media";
#else
    constexpr const char* kHostFeature = "media";
#endif
    if (!Host::attachRoutes(hostRoute, nullptr, kHostFeature)) {
        ESP_LOGW(kTag, "USB route already registered");
    }
#endif

    s_playback = &g_playback;
    ESP_ERROR_CHECK(g_playback.begin());
    s_context.volume = clampVolume(s_context.volume);
    g_playback.setVolume(static_cast<unsigned>(s_context.volume));
    g_playback.setEq(static_cast<Audio::EqPreset>(s_context.settings.eq));

    publishCatalog();
    logHeap("boot: before media index");
    if (s_context.sd_mounted && g_library.loadCache(Storage::SdVolume::root)) {
        publishCatalog();
        s_context.catalog_ready = true;
        const auto& loaded = *s_catalog;
        ESP_LOGI(kTag, "media index loaded: %u tracks, %u stories",
                 static_cast<unsigned>(loaded.tracks.size()),
                 static_cast<unsigned>(loaded.stories.size()));
        logHeap("after media index");
    } else if (s_context.sd_mounted) {
        ESP_LOGI(kTag, "media index missing; first library page will scan");
    }

    goHome();
    restoreLastSession();
    logHeap("after session restore");

    uint32_t last_volume_save = nowMs();
    uint32_t last_progress_save = last_volume_save;
    int last_saved_volume = s_context.volume;
    bool last_hall = Hall::detected();
    s_context.hall = last_hall;

    // This task owns its notification slot. The one-shot timer wakes it at a
    // sub-tick deadline, avoiding another 10ms tick after a full SPI transfer.
    esp_timer_handle_t frame_timer = nullptr;
    esp_timer_create_args_t frame_args{};
    frame_args.callback = [](void* task) { xTaskNotifyGive(static_cast<TaskHandle_t>(task)); };
    frame_args.arg = xTaskGetCurrentTaskHandle();
    frame_args.name = "ui-frame";
    if (esp_timer_create(&frame_args, &frame_timer) != ESP_OK)
        ESP_LOGW(kTag, "frame timer unavailable; using tick pacing");

    for (;;) {
        const auto frame_started = esp_timer_get_time();
        const uint32_t now = nowMs();

        // Child lock is a parental flag, not an input state: apply it before
        // sampling so a remote change takes effect on the next loop. When it is
        // active the only accepted gesture is the local OK+NEXT unlock combo.
        const bool child_lock = s_parental_ready && s_policy.childLocked();
        if (child_lock != s_context.child_locked) {
            s_context.child_locked = child_lock;
            Input::setLocked(child_lock);
            s_stack[s_depth - 1]->invalidate();
        }
        if (child_lock && Input::childUnlockHeld()) {
            Persistence::ParentalRecord record = s_policy.record();
            Persistence::parentalSetFlag(record, Persistence::kParentalChildLock, false);
            if (parentalApply(record)) notify("童锁已解除");
        }

        Event event;
        sampleInput(event);
        if (!event.empty() && !Power::screenOff()) {
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
        parentalTick(playing || s_video_active);
        if (s_parental_ready && !playbackAllowed() &&
            (playing || playback_state == Audio::PlaybackState::Paused)) {
            stopPlayback();
            setNowPlaying("idle", "");
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
        // Per-folder resume: write the position every few seconds so a power cut
        // loses at most that much. Writes are one 12-byte slot plus a header.
        if (now - last_progress_save >= 5000) {
            const auto state = g_playback.snapshot();
            if (state.state == Audio::PlaybackState::Playing ||
                state.state == Audio::PlaybackState::Paused) {
                last_progress_save = now;
                saveProgress();
            }
        }

#if CONFIG_STORYBOX_NFC
        // Drain NFC state changes every frame. The same consumer feeds the USB
        // event channel (when compiled) and the standalone card auto-play.
        handleNfcEvents();
#endif
#if CONFIG_STORYBOX_USB_PROTOCOL
        // A host upload bumps the storage revision. Rescan once playback is
        // idle: the maintenance window needs every file lease released.
        if (s_context.sd_mounted && s_catalog &&
            s_catalog->storage_revision != g_gate.revision() && now - s_catalog_refresh_at >= 2000) {
            const auto state_now = g_playback.snapshot().state;
            const bool idle = state_now == Audio::PlaybackState::Stopped ||
                              state_now == Audio::PlaybackState::Ended ||
                              state_now == Audio::PlaybackState::Failed;
            if (idle && !Video::active()) {
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
        // Close is asynchronous: keep awake until I2S/storage are released.
        power_inputs.video_playing = s_video_active || (Video::busy() && !Video::active());
        // A phone that merely stays connected must not pin the panel on: only
        // recent RX traffic (commands from the mini program) keeps it awake.
        power_inputs.host_active = Ble::connected() &&
                                   (now - Ble::lastActivityMs()) < 15000u;
#if CONFIG_STORYBOX_USB_PROTOCOL
        // The USB workbench sends a heartbeat every 5 s. Keep the device awake
        // while it is being used; close the app and the 3-minute idle sleep can
        // take over again.
        power_inputs.host_active = power_inputs.host_active || Host::recentlyActive(15000);
#endif
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
        }
        if (!screen_off) Display::present();

        if (s_context.volume != last_saved_volume && now - last_volume_save >= 1000) {
#if CONFIG_STORYBOX_FRAM_CONFIRMED
            if (s_records_store &&
                s_records_store->saveVolume(static_cast<uint8_t>(s_context.volume))) {
                last_volume_save = now;
                last_saved_volume = s_context.volume;
            }
#endif
        }

        if (Video::active() && frame_timer &&
            esp_timer_start_once(frame_timer, Video::frameWaitUs(frame_started, esp_timer_get_time())) == ESP_OK) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        } else {
            vTaskDelay(Video::active() ? 1 : pdMS_TO_TICKS(kFrameIntervalMs));
        }
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
    Video::setVolume(static_cast<unsigned>(value));
    if (s_playback) s_playback->setVolume(static_cast<unsigned>(value));
}

void stepVolume(int steps) {
    if (!steps) return;
    setVolume(s_context.volume + steps);
}

void applyEq(uint8_t preset) {
    s_context.settings.eq = preset > 3 ? 0 : preset;
    Video::setEq(static_cast<Audio::EqPreset>(s_context.settings.eq));
    if (s_playback) s_playback->setEq(static_cast<Audio::EqPreset>(s_context.settings.eq));
}

bool applyNfcEnabled(bool enabled) {
    auto* service = s_nfc.load(std::memory_order_acquire);
    if (!enabled && service && service->snapshot().busy) return false;
    s_context.settings.nfc = enabled ? 1 : 0;
    if (service) {
        service->setEnabled(enabled);
        // Standalone card use needs polling without a host; the host's own
        // nfc.poll remains idempotent.
        if (enabled) service->setPolling(true);
    }
    return true;
}

CatalogPtr catalog() {
    std::lock_guard<std::mutex> lock(s_catalog_mutex);
    return s_catalog;
}
Media::Playlist& playlist() { return g_playlist; }
Audio::Playback& playback() { return g_playback; }
Storage::AccessGate& storageGate() { return g_gate; }

Media::Catalog emptyCatalog() { return Media::Catalog{}; }

bool refreshCatalog() {
    if (!s_context.sd_mounted) return false;
    if (Video::busy()) return false;
    // Playback holds a file lease for the whole track; maintenance needs every
    // lease released, so stop first and re-allow playback afterwards.
    setNowPlaying("idle", "");
    g_playback.stopAndWait(3000);
    const auto result = g_library.scan(Storage::SdVolume::root);
    g_playback.allowPlayback();
    if (result != Media::ScanResult::Ok) {
        ESP_LOGW(kTag, "media scan failed: %d", static_cast<int>(result));
        return false;
    }
    publishCatalog();
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

namespace {
// FNV-1a of the folder name. The per-folder resume table is keyed by this so a
// renamed category just stops matching instead of pointing at another folder.
uint32_t folderHash(const char* text) {
    if (!text || !*text) return 1;  // "all songs" bucket
    uint32_t hash = 2166136261u;
    for (const char* p = text; *p; ++p) {
        hash ^= static_cast<uint8_t>(*p);
        hash *= 16777619u;
    }
    return hash ? hash : 1;
}
const char* trackCategoryName(const Media::Track* track) {
    if (!track || track->category <= 0 || !s_catalog) return "";
    const auto index = static_cast<std::size_t>(track->category - 1);
    return index < s_catalog->categories.size() ? s_catalog->categories[index].c_str() : "";
}
// Remembers "which track" for the boot session; the progress itself lives in the
// per-folder table and is written by the periodic save in the main loop.
void rememberSession(const Media::Track* track) {
    if (!s_records_store || !track) return;
    if (!s_records_store->savePosition(trackCategoryName(track), track->id)) {
        ESP_LOGW(kTag, "session position write failed");
    }
}
}  // namespace

bool playCurrentAt(uint32_t offset_bytes, uint32_t seconds, bool auto_start) {
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
    if (Video::active()) {
        Video::close();
        setVideoActive(false);
        setCurrentVideo(-1);
    }
    uint64_t request = 0;
    const auto result = Video::playAudio(g_playback, track->path.c_str(), auto_start, request,
                                         offset_bytes, seconds);
    if (result != ESP_OK) ESP_LOGW(kTag, "play %s: %s", track->path.c_str(), esp_err_to_name(result));
    else {
        setNowPlaying("music", track->name.c_str());
        rememberSession(track);
    }
    return result == ESP_OK;
}

bool playCurrent(bool auto_start) { return playCurrentAt(0, 0, auto_start); }

bool hasResume(const char* folder, uint32_t* track, uint32_t* seconds, uint32_t* offset_bytes) {
    if (!s_records_store) return false;
    Persistence::FolderResume entry{};
    if (!s_records_store->resume(folderHash(folder), entry)) return false;
    if (track) *track = entry.track;
    if (seconds) *seconds = entry.seconds;
    if (offset_bytes) *offset_bytes = uint32_t(entry.offset_kb) * 1024u;
    return true;
}

void saveProgress() {
    if (!s_records_store || !s_catalog) return;
    const auto state = g_playback.snapshot();
    const auto* track = g_playlist.current();
    if (!track) return;
    const uint32_t seconds = state.sample_rate
                                 ? static_cast<uint32_t>(state.submitted_frames / state.sample_rate)
                                 : 0;
    if (!s_records_store->setResume(folderHash(trackCategoryName(track)), track->id, seconds,
                                    state.offset_bytes)) {
        ESP_LOGW(kTag, "resume record write failed");
    }
}

// Boot: reopen the remembered track in the player page, paused at the saved
// position. The Arduino firmware did this for music; the IDF port had lost it.
void restoreLastSession() {
    if (!s_records_store || !s_catalog || !s_context.catalog_ready) return;
    const auto& session = s_records_store->session();
    if (!session.track_id) return;
    const Media::Track* found = nullptr;
    for (const auto& track : s_catalog->tracks) {
        if (track.id == session.track_id) { found = &track; break; }
    }
    if (!found) return;
    if (!g_playlist.select(s_catalog, found->category)) return;
    if (!g_playlist.seekId(session.track_id)) return;
    uint32_t track_id = 0, seconds = 0, offset = 0;
    if (hasResume(trackCategoryName(found), &track_id, &seconds, &offset) &&
        track_id == session.track_id && seconds > 0) {
        // keep the stored position
    } else {
        seconds = 0;
        offset = 0;
    }
    if (!playCurrentAt(offset, seconds, false)) return;
    pushPage(createPlayerPage());
    ESP_LOGI(kTag, "resume session: %s at %us", found->name.c_str(), static_cast<unsigned>(seconds));
}

void setNowPlaying(const char* mode, const char* title) {
    std::snprintf(s_now_title, sizeof(s_now_title), "%s", title ? title : "");
    s_now_mode = mode ? mode : "idle";
}

void setCurrentStory(int index) { s_now_story = index; }
int currentStory() { return s_now_story; }
void setCurrentVideo(int index) { s_now_video = index; }
int currentVideo() { return s_now_video; }

bool stopPlayback() {
    const bool video_busy = Video::busy();
    if (!Video::close()) return false;
    setVideoActive(false);
    setCurrentVideo(-1);
    setNowPlaying("idle", "");
    if (video_busy) return true;
    const bool stopped = g_playback.stopAndWait(3000) == ESP_OK;
    if (stopped) g_playback.allowPlayback();
    return stopped;
}

void pausePlayback(bool paused) {
    if (Video::active()) { Video::pause(paused); return; }
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
    return Persistence::parentalLockText(lock);
}

uint32_t parentalUsedSeconds() {
    if (!s_parental_ready) return 0;
    return s_policy.record().used;
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
    // Keep the trusted clock and the usage accumulator: editing one setting must
    // not turn a bedtime rule into "clock required" and block playback.
    s_parental_ready = s_policy.rebind(s_parental_store->value(), nowMs());
    if (s_parental_ready) setVolume(s_context.volume);
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
    // The UI task keeps an internal stack: BLE init writes NVS from this task, and
    // a PSRAM stack is unreachable while the flash cache is off (IDF asserts in
    // spi_flash_disable_interrupts_caches_and_other_cpu). The audio, USB and video
    // control tasks, which never touch flash, do live in PSRAM.
    const auto created = xTaskCreatePinnedToCore(
        [](void*) {
            run();
            vTaskDelete(nullptr);
        },
        "app", 16384, nullptr, 3, nullptr, 0);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "cannot create app task");
        return false;
    }
    return true;
}

}  // namespace App
