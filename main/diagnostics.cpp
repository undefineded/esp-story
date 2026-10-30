#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_flash.h"
#include "esp_timer.h"
#include "display.h"
#include "fonts.h"
#include "input.h"
#include "hall.h"
#include "sd_volume.h"
#include "playback.h"
#include "media_library.h"
#include "story_timeline.h"
#include "story_image.h"
#include "wifi_service.h"
#include "settings_record.h"
#include "media_records.h"
#include "parental_policy.h"
#include "i2c_bus.h"
#include "fram_device.h"
#include "config.h"
#include "nfc_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>

#include "diagnostics.h"
#include "file_reader.h"
#include "usb_transport.h"

void runDiagnostics()
{
#if CONFIG_STORYBOX_PERIPHERAL_DIAGNOSTICS
    constexpr const char* tag = "diagnostics";
    Display::begin();
    Fonts::begin();
    Input::begin();
    Hall::begin();
#if CONFIG_STORYBOX_NETWORK_DIAGNOSTICS
    static Connectivity::WifiService wifi;
    const bool wifi_ready = wifi.begin() == ESP_OK;
    unsigned wifi_mode = 0;
#endif
#if CONFIG_STORYBOX_FRAM_CONFIRMED || CONFIG_STORYBOX_NFC_DIAGNOSTICS
    static Board::I2cBus bus;
    const bool bus_ready = bus.begin() == ESP_OK;
#endif
#if CONFIG_STORYBOX_NFC_DIAGNOSTICS
    static NativeNfc::Service nfc;
    const bool nfc_ready = bus_ready && nfc.begin(bus.handle()) == ESP_OK;
    if (nfc_ready) nfc.setPolling(true);
#endif
#if CONFIG_STORYBOX_FRAM_CONFIRMED
#if CONFIG_STORYBOX_BOARD_REV_20261028
    static Board::FramDevice fram(Board::rev20261028);
#else
    static Board::FramDevice fram(Board::legacy);
#endif
    static Persistence::SettingsStore<Board::FramDevice> settings(fram);
    static Persistence::MediaRecords<Board::FramDevice> records(fram);
    static Persistence::ParentalStore<Board::FramDevice> parental_store(fram);
    Persistence::ParentalPolicy parental;
    bool parental_ready = false;
    bool records_ready = false;
    bool fram_ready = bus_ready && fram.begin(bus.handle(),true) == ESP_OK;
    if (fram_ready) {
        const auto result = settings.load();
        fram_ready = result != Persistence::LoadResult::IoError;
        ESP_LOGI(tag,"Settings load: %d",static_cast<int>(result));
        Display::setBrightness(settings.value().brightness);
        Display::setRotation(settings.value().rotation);
        records_ready = records.load();
        if (!records_ready) ESP_LOGW(tag,"Session/favorites read failed; persistence disabled");
        const auto parental_result = parental_store.load();
        parental_ready = parental_result == Persistence::LoadResult::Loaded &&
            parental.begin(parental_store.value(),esp_timer_get_time()/1000);
        ESP_LOGI(tag,"Parental record: %d; playback %s",static_cast<int>(parental_result),
            parental_ready ? "policy enforced" : "blocked (no valid policy)");
    }
#endif
    Storage::SdVolume volume;
    const auto sd_result = volume.mount();
    ESP_LOGI(tag, "SD mount: %s", esp_err_to_name(sd_result));
    int position = 0;
#if CONFIG_STORYBOX_AUDIO_DIAGNOSTICS
    static Storage::AccessGate access;
#if CONFIG_STORYBOX_USB_PROTOCOL
    static Storage::FileReader usb_files(access,Storage::SdVolume::root,
#if CONFIG_STORYBOX_USB_FILE_WRITES
        true
#else
        false
#endif
    );
    if (volume.mounted()) ESP_ERROR_CHECK(Host::attachFiles(usb_files));
#endif
    static Audio::Playback playback(access);
    static Media::Library library(access);
    Media::Playlist playlist;
    ESP_ERROR_CHECK(playback.begin());
    uint64_t request = 0;
    uint64_t last_play_request = 0, handled_eof = 0;
    enum class Source { Music, Nursery, Story };
    Source source = Source::Music;
    std::string active_category;
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    uint64_t persisted_generation = 0;
    int64_t last_persist_attempt = 0;
    int64_t last_parental_save = esp_timer_get_time();
#endif
    std::size_t story_index = 0;
    Media::Timeline timeline;
    int shown_panel = -2;
    bool image_ok = false;
    auto catalog = library.snapshot();
    int64_t last_refresh_attempt=0;
    auto load = [&](bool auto_start) {
        const auto* track = playlist.current();
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        if (!parental_ready || parental.lock(esp_timer_get_time()/1000)!=Persistence::PolicyLock::None)
            return ESP_ERR_INVALID_STATE;
        if (source == Source::Story ? !parental.storyAllowed(story_index) :
            (!track || !parental.categoryAllowed(track->category))) return ESP_ERR_INVALID_STATE;
#endif
        if (source == Source::Nursery && !track) return ESP_ERR_NOT_FOUND;
        const char* path = track ? track->path.c_str() : CONFIG_STORYBOX_AUDIO_TEST_PATH;
        Media::Timeline candidate;
        if (source == Source::Story) {
            if (catalog->stories.empty()) return ESP_ERR_NOT_FOUND;
            const auto& story = catalog->stories[story_index];
            path = story.audio.c_str();
            if (!candidate.load(access,Storage::SdVolume::root,story.timeline)) return ESP_ERR_INVALID_RESPONSE;
        }
        const auto result = playback.play(path, auto_start, request);
        if (result == ESP_OK) {
            last_play_request = request;
            timeline = std::move(candidate);
            shown_panel = -2;
        }
        else ESP_LOGW(tag, "MP3 load: %s", esp_err_to_name(result));
        return result;
    };
    if (volume.mounted()) {
        if (library.loadCache(Storage::SdVolume::root)) ESP_LOGI(tag,"Loaded legacy media index");
        else {
            const auto result = library.scan(Storage::SdVolume::root);
            ESP_LOGI(tag, "Media scan: %d", static_cast<int>(result));
            if (result==Media::ScanResult::Ok)
                ESP_LOGI(tag,"Media index save: %d",static_cast<int>(library.saveCache(Storage::SdVolume::root)));
        }
        catalog = library.snapshot();
        playlist.select(catalog, 0);
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        if (fram_ready && records_ready) {
            active_category = records.session().category;
            if (active_category == "童谣") {
                source = Source::Nursery;
                playlist.select(catalog,0,true);
            } else {
                int category = 0;
                for (std::size_t i=0;i<catalog->categories.size();++i)
                    if (catalog->categories[i]==active_category) category=i+1;
                if (!category && source!=Source::Nursery) active_category.clear();
                playlist.select(catalog,category);
                if (!category) active_category.clear();
            }
            playlist.seekId(records.session().track_id);
        }
#endif
        load(false);
    }
    int audio_volume = 4;
    auto eq = Audio::EqPreset::Vocal;
#if CONFIG_STORYBOX_FRAM_CONFIRMED
    if (records_ready) audio_volume = records.session().volume;
    eq = static_cast<Audio::EqPreset>(settings.value().eq);
    playback.setEq(eq);
#endif
#endif
    while (true) {
        int steps = Input::encoderSteps();
        position += steps;
        auto ok = Input::okEvent();
        auto next = Input::nextEvent();
        auto prev = Input::prevEvent();
        auto set = Input::encEvent();
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        if (parental.childLocked()) { steps=0; ok=0; next=0; prev=0; set=0; }
#endif
#if CONFIG_STORYBOX_NETWORK_DIAGNOSTICS
        if (prev == 2 && wifi_ready) {
            const unsigned mode = (wifi_mode + 1) % 4;
            const auto result = wifi.apply((mode & 2) != 0,(mode & 1) != 0,WIFI_STA_SSID,WIFI_STA_PASS);
            if (result == ESP_OK) wifi_mode = mode;
            else ESP_LOGW(tag,"Wi-Fi mode change failed: %s",esp_err_to_name(result));
        }
#endif
#if CONFIG_STORYBOX_AUDIO_DIAGNOSTICS
        if (catalog->storage_revision!=access.revision() &&
            esp_timer_get_time()-last_refresh_attempt>=1000000) {
            last_refresh_attempt=esp_timer_get_time();
            if (playback.stopAndWait(3000)==ESP_OK && library.scan(Storage::SdVolume::root)==Media::ScanResult::Ok) {
                const auto* previous=playlist.current();
                const uint32_t previous_id=previous?previous->id:0;
                catalog=library.snapshot();
                int category=0;
                for (std::size_t i=0;i<catalog->categories.size();++i)
                    if (catalog->categories[i]==active_category) category=i+1;
                playlist.select(catalog,category,source==Source::Nursery);
                playlist.seekId(previous_id);
                story_index=0; timeline=Media::Timeline{}; shown_panel=-2; image_ok=false;
                if (source==Source::Story && catalog->stories.empty()) source=Source::Music;
                ESP_LOGI(tag,"Media refreshed; index save: %d",static_cast<int>(library.saveCache(Storage::SdVolume::root)));
                playback.allowPlayback();
                load(false); // Never resume playback automatically after a host mutation.
            }
        }
        audio_volume = std::max(0, std::min(21, audio_volume + steps));
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        audio_volume = parental.clampVolume(audio_volume);
#endif
        playback.setVolume(audio_volume);
        auto state = playback.snapshot();
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        parental.tick(state.state==Audio::PlaybackState::Playing,esp_timer_get_time()/1000);
        if (!parental_ready || parental.lock(esp_timer_get_time()/1000)!=Persistence::PolicyLock::None) {
            if (state.state==Audio::PlaybackState::Playing || state.state==Audio::PlaybackState::Paused)
                playback.stopAndWait(3000);
            ok=0; // Do not permit resume while locked.
        }
        if (parental_ready && parental.dirty() && esp_timer_get_time()-last_parental_save>=60000000) {
            last_parental_save=esp_timer_get_time();
            if (parental_store.save(parental.record())) parental.saved();
            else {
                parental_ready=false;
                ok=0;
                playback.stopAndWait(3000);
                ESP_LOGE(tag,"Parental persistence failed; playback blocked until restart/recovery");
            }
        }
#endif
        if (ok == 1) {
            auto result = playback.pause(state.state == Audio::PlaybackState::Playing, request);
            if (result != ESP_OK) ESP_LOGW(tag, "Audio command: %s", esp_err_to_name(result));
        }
        if (ok == 2) {
            eq = static_cast<Audio::EqPreset>((static_cast<unsigned>(eq) + 1) % 4);
            playback.setEq(eq);
        }
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        if (ok == 3 && fram_ready) {
            auto value = settings.value();
            value.eq = static_cast<uint8_t>(eq);
            settings.set(value);
            ESP_LOGI(tag,"Settings save: %s",settings.save()?"verified":"FAILED (still dirty)");
        }
#endif
        if (next == 2 && volume.mounted()) {
            const auto previous_source = source;
            const auto previous_playlist = playlist;
            const auto previous_category = active_category;
            source = static_cast<Source>((static_cast<unsigned>(source)+1)%3);
            if (source == Source::Nursery) {
                Media::Playlist available;
                if (!available.select(catalog,0,true)) source = Source::Story;
            }
            if (source == Source::Story && catalog->stories.empty()) source = Source::Music;
            if (source != Source::Story) playlist.select(catalog,0,source==Source::Nursery);
            active_category = source == Source::Nursery ? "童谣" : "";
            playback.allowPlayback();
            const auto result = load(false);
            if (result != ESP_OK) {
                source = previous_source;
                playlist = previous_playlist;
                active_category = previous_category;
                ESP_LOGW(tag, "Source unavailable: %s",esp_err_to_name(result));
            }
        }
        const int delta = next == 1 ? 1 : (prev == 1 ? -1 : 0);
        if (delta && source == Source::Story && !catalog->stories.empty()) {
            const auto before = story_index;
            story_index = (int(story_index)+delta+int(catalog->stories.size()))%catalog->stories.size();
            playback.allowPlayback();
            if (load(true) != ESP_OK) story_index = before;
        } else if (delta && source != Source::Story && playlist.move(delta)) {
            playback.allowPlayback();
            if (load(true) != ESP_OK) playlist.move(-delta);
        }
        if (source != Source::Story && state.state == Audio::PlaybackState::Ended && state.request >= last_play_request &&
            state.generation != handled_eof && playlist.size()) {
            handled_eof = state.generation;
            playlist.move(1);
            if (load(true) != ESP_OK) playlist.move(-1);
        }
        if (set == 2) {
            ESP_LOGI(tag, "Stop: %s", esp_err_to_name(playback.stopAndWait(3000)));
        }
        if (set == 1 && volume.mounted()) {
            playback.allowPlayback();
            load(false);
        }
#if CONFIG_STORYBOX_FRAM_CONFIRMED
        if (fram_ready && records_ready && esp_timer_get_time()-last_persist_attempt >= 1000000) {
            last_persist_attempt = esp_timer_get_time();
            if (audio_volume != records.session().volume && !records.saveVolume(audio_volume))
                ESP_LOGW(tag,"Volume save failed");
            const auto* track = playlist.current();
            if (source != Source::Story && track && track->path==state.path &&
                state.state==Audio::PlaybackState::Playing &&
                state.generation!=persisted_generation) {
                if (records.savePosition(active_category,track->id)) persisted_generation=state.generation;
                else ESP_LOGW(tag,"Playback position save failed");
            }
        }
#endif
#endif
        auto& canvas = Display::canvas();
#if CONFIG_STORYBOX_AUDIO_DIAGNOSTICS
        const bool story_view = source == Source::Story && !catalog->stories.empty();
        if (story_view) {
            const auto& story = catalog->stories[story_index];
            const bool current = state.request >= last_play_request && story.audio == state.path;
            const uint32_t seconds = current && state.sample_rate ? state.submitted_frames/state.sample_rate : 0;
            const int panel = timeline.atSeconds(seconds);
            if (shown_panel != panel) {
                canvas.fillScreen(TFT_BLACK);
                image_ok = panel >= 0 && Display::drawStoryImage(access,Storage::SdVolume::root,timeline.panels()[panel].path);
                shown_panel = panel;
            }
            canvas.fillRect(0,200,320,40,TFT_BLACK);
            canvas.setTextColor(TFT_WHITE);
            canvas.setFont(Fonts::small());
            canvas.setCursor(4,202);
            canvas.println(story.title.c_str());
            canvas.printf("%lu s  vol:%d state:%d %s",static_cast<unsigned long>(seconds),audio_volume,
                          static_cast<int>(state.state),panel>=0&&!image_ok?"JPEG error":"");
        } else {
#endif
        canvas.fillScreen(TFT_BLACK);
        canvas.setTextColor(TFT_WHITE);
        canvas.setFont(Fonts::body());
        canvas.setCursor(8, 12);
        canvas.println("故事机 ESP-IDF 硬件诊断");
        canvas.printf("SD: %s\n", volume.mounted() ? "mounted" : "unavailable");
        canvas.printf("Encoder: %d  Hall: %d\n", position, Hall::detected());
        canvas.printf("OK:%d NEXT:%d PREV:%d SET:%d\n", ok, next, prev, set);
#if CONFIG_STORYBOX_NFC_DIAGNOSTICS
        const auto nfc_state = nfc.snapshot();
        canvas.printf("NFC:%s card:%s\n",nfc_state.module?nfc_state.firmware:"offline",
                      nfc_state.card.valid?Nfc::kindName(nfc_state.card.kind):"--");
#endif
#if CONFIG_STORYBOX_AUDIO_DIAGNOSTICS
        canvas.printf("MP3 state:%d volume:%d\n", static_cast<int>(state.state), audio_volume);
        canvas.printf("Track: %u/%u EQ:%u\n", unsigned(playlist.size() ? playlist.position()+1 : 0),
                      unsigned(playlist.size()), static_cast<unsigned>(eq));
        if (playlist.current()) canvas.println(playlist.current()->name.c_str());
        canvas.printf("%lu Hz  %lu kbps\n", static_cast<unsigned long>(state.sample_rate),
                      static_cast<unsigned long>(state.bitrate / 1000));
        canvas.printf("Audio error: %s\n", esp_err_to_name(state.error));
        }
#endif
        Display::present();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif
}
