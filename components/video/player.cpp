#include "video_player.h"
#include "latest_frame.h"
#include "versioned_control.h"
#include <atomic>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <cstdio>
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "media_lib_adapter.h"
#include "esp_player.h"
#include "esp_player_advance.h"
#include "esp_audio_render.h"
#include "esp_video_render.h"
#include "esp_video_dec_mjpeg.h"
#include "freertos/task.h"
#include "esp_extractor_defaults.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_video_color_convert.h"
#include "esp_gmf_video_scale.h"
#include "esp_gmf_video_crop.h"

namespace Video {
namespace {
constexpr unsigned Width = 320, Height = 240, Bytes = Width * Height * 2;
constexpr unsigned SampleRate = 44100;
esp_player_handle_t player = nullptr;
esp_audio_render_handle_t audio_render = nullptr;
esp_video_render_handle_t video_render = nullptr;
esp_gmf_pool_handle_t audio_pool = nullptr, video_pool = nullptr;
Storage::AccessGate* lease = nullptr;
Audio::Playback* legacy = nullptr;
Audio::PcmOutput output;
Audio::Equalizer equalizer;
std::atomic<unsigned> volume{4};
std::atomic<Audio::EqPreset> eq{Audio::EqPreset::Vocal};
LatestFrame latest;
std::mutex framebuffer_mutex;
uint8_t* framebuffer = nullptr;
uint8_t* mailbox = nullptr;
const char* last_error = "";
bool defaults_registered = false;
enum class Action { Open, Stop, Audio };
struct Control {
    Action action = Action::Stop;
    Storage::AccessGate* gate = nullptr;
    Audio::Playback* audio = nullptr;
    char path[512]{};
    bool start = true, drain = false;
    uint32_t serial = 0;
    int64_t requested_at = 0;
};
VersionedControl<Control, Status> control;
std::atomic<uint32_t> frame_serial{0};
std::atomic<bool> video_requested{false}, pause_requested{false}, owns_audio{false};
TaskHandle_t controller = nullptr;
uint32_t working_serial = 0;
int64_t working_requested_at = 0;
int64_t opened_at = 0;
std::atomic<uint32_t> first_frame_ms{0}, copy_us{0};
Status timings;
bool closeSync(bool drain_audio = false);
bool openSync(Storage::AccessGate&, Audio::Playback&, const std::string&);
Status statusSync();
void controlTask(void*);
bool ensureController() {
    if (controller) return true;
    return xTaskCreatePinnedToCore(controlTask, "video-control", 8192, nullptr, 3, &controller, 0) == pdPASS;
}
bool submit(Control command) {
    if (!ensureController()) return false;
    if (command.action == Action::Stop) command.audio = control.command().audio;
    const bool is_video = command.action == Action::Open;
    command.requested_at = esp_timer_get_time();
    Status initial;
    if (is_video) initial.state = State::Preparing;
    control.post(command, initial);
    video_requested = is_video;
    pause_requested = false;
    xTaskNotifyGive(controller);
    return true;
}


esp_video_render_err_t backendInit(void*, int, void** out) {
    *out = framebuffer;
    return framebuffer ? ESP_VIDEO_RENDER_ERR_OK : ESP_VIDEO_RENDER_ERR_NO_MEM;
}
bool withGram(void*) { return false; }
esp_video_render_err_t displayInfo(void*, esp_video_render_disp_info_t* info) {
    *info = {ESP_VIDEO_RENDER_FORMAT_RGB565, Width, Height};
    return ESP_VIDEO_RENDER_ERR_OK;
}
esp_video_render_err_t getFb(void*, esp_video_render_fb_t* fb) {
    *fb = {};
    fb->info = {ESP_VIDEO_RENDER_FORMAT_RGB565, Width, Height, 30};
    fb->data = framebuffer;
    fb->size = Bytes;
    return ESP_VIDEO_RENDER_ERR_OK;
}
esp_video_render_err_t lockFb(void*, esp_video_render_fb_t*, bool lock) {
    if (lock) framebuffer_mutex.lock(); else framebuffer_mutex.unlock();
    return ESP_VIDEO_RENDER_ERR_OK;
}
esp_video_render_err_t writeFb(void*, esp_video_render_fb_t* fb,
                             const esp_video_render_rect_t*, const esp_video_render_pos_t*) {
    if (!fb || !fb->data || fb->size < Bytes || fb->info.width != Width ||
        fb->info.height != Height || fb->info.format != ESP_VIDEO_RENDER_FORMAT_RGB565)
        return ESP_VIDEO_RENDER_ERR_INVALID_ARG;
    // The only UI contention is the short mailbox copy, never an SPI transfer.
    const auto started = esp_timer_get_time();
    const bool ok = latest.publish(fb->data, Bytes);
    copy_us = static_cast<uint32_t>(esp_timer_get_time() - started);
    if (ok && !first_frame_ms.load())
        first_frame_ms = static_cast<uint32_t>((esp_timer_get_time() - opened_at) / 1000) + 1;
    return ok ? ESP_VIDEO_RENDER_ERR_OK : ESP_VIDEO_RENDER_ERR_FAIL;
}
esp_video_render_err_t backendDeinit(void*) { return ESP_VIDEO_RENDER_ERR_OK; }
const esp_video_render_backend_ops_t backend = {
    backendInit, withGram, displayInfo, getFb, lockFb, writeFb, backendDeinit
};

int writeAudio(uint8_t* pcm, uint32_t size, void*) {
    if (!pcm || size % 4) return -1;
    equalizer.configure(SampleRate, eq.load());
    equalizer.process(reinterpret_cast<int16_t*>(pcm), size / 4, volume.load());
    std::size_t offset = 0;
    while (offset < size) {
        std::size_t written = 0;
        auto result = output.write(reinterpret_cast<int16_t*>(pcm + offset),
                                   (size - offset) / 4, written, 100);
        offset += written;
        if ((result != ESP_OK && result != ESP_ERR_TIMEOUT) || !written) return -1;
    }
    return 0;
}

bool pools() {
    if (esp_gmf_pool_init(&audio_pool) || esp_gmf_pool_init(&video_pool)) return false;
    esp_gmf_element_handle_t el = nullptr;
    auto add = [](esp_gmf_pool_handle_t pool, esp_gmf_element_handle_t element) {
        if (esp_gmf_pool_register_element(pool, element, nullptr) == ESP_GMF_ERR_OK) return true;
        esp_gmf_element_deinit(element);
        return false;
    };
    esp_ae_ch_cvt_cfg_t ch = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    esp_ae_bit_cvt_cfg_t bit = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    esp_ae_rate_cvt_cfg_t rate = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&ch, &el) || !add(audio_pool, el)) return false;
    if (esp_gmf_bit_cvt_init(&bit, &el) || !add(audio_pool, el)) return false;
    if (esp_gmf_rate_cvt_init(&rate, &el) || !add(audio_pool, el)) return false;
    esp_imgfx_color_convert_cfg_t color{};
    color.color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601;
    esp_imgfx_scale_cfg_t scale{};
    scale.filter_type = ESP_IMGFX_SCALE_FILTER_TYPE_BILINEAR;
    esp_imgfx_crop_cfg_t crop{};
    if (esp_gmf_video_color_convert_init(&color, &el) || !add(video_pool, el)) return false;
    if (esp_gmf_video_scale_init(&scale, &el) || !add(video_pool, el)) return false;
    if (esp_gmf_video_crop_init(&crop, &el) || !add(video_pool, el)) return false;
    return true;
}
bool fail(const char* message) {
    last_error = message;
    ESP_LOGE("video", "%s", message);
    closeSync();
    return false;
}


bool openSync(Storage::AccessGate& gate, Audio::Playback& audio, const std::string& path) {
    if (!closeSync()) return false;
    last_error = "";
    opened_at = working_requested_at;
    first_frame_ms = 0; copy_us = 0;
    const auto stop_at = esp_timer_get_time();
    if (audio.stopAndWait(3000) != ESP_OK) return fail("音频停止超时");
    timings.stop_audio_ms = static_cast<uint32_t>((esp_timer_get_time() - stop_at) / 1000);
    const auto setup_at = esp_timer_get_time();
    if (control.serial() != working_serial) { audio.allowPlayback(); return false; }
    owns_audio = true;
    legacy = &audio; // Admission stays closed until all official renderers are destroyed.
    if (!gate.acquire()) return fail("存储正在维护");
    lease = &gate; // esp_player owns FILE internally; keep our lease for its entire lifetime.
    framebuffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, Bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    mailbox = static_cast<uint8_t*>(heap_caps_aligned_alloc(16, Bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!framebuffer || !mailbox) return fail("视频需要 PSRAM");
    std::memset(framebuffer, 0, Bytes);
    latest.attach(mailbox, Bytes);
    if (!defaults_registered) {
        media_lib_add_default_adapter();
        // Registries are process-global and shared with the MP3 service. Never unregister on page exit.
        if (Audio::ensureMp3Decoder() || esp_video_dec_register_sw_mjpeg() ||
            esp_avi_extractor_register()) return fail("解码器注册失败");
        defaults_registered = true;
    }
    if (!pools() || output.begin(SampleRate) != ESP_OK) return fail("渲染资源不足");
    equalizer.reset();
    esp_audio_render_cfg_t ac{};
    ac.max_stream_num = 1; ac.out_writer = writeAudio;
    ac.out_sample_info = {SampleRate, 16, 2}; ac.pool = audio_pool;
    if (esp_audio_render_create(&ac, &audio_render)) return fail("音频渲染初始化失败");
    esp_video_render_cfg_t vc{};
    vc.pool = video_pool; vc.fps = 30;
    if (esp_video_render_create(&vc, &video_render)) return fail("视频渲染初始化失败");
    esp_video_render_backend_cfg_t bc{};
    bc.ops = &backend;
    if (esp_video_render_set_display(video_render, &bc)) return fail("屏幕适配初始化失败");
    esp_audio_render_stream_handle_t stream = nullptr;
    if (esp_audio_render_stream_get(audio_render, ESP_AUDIO_RENDER_FIRST_STREAM, &stream)) return fail("音频流初始化失败");
    esp_player_config_t cfg = ESP_PLAYER_CONFIG_DEFAULT();
    cfg.audio_render_hd = stream; cfg.video_render_hd = video_render;
    if (esp_player_init(&cfg, &player)) return fail("播放器初始化失败");
    // Core 0: SD demux + audio + UI/SPI; core 1: expensive video decode.
    // Internal stacks avoid flash/cache-disabled access to PSRAM stacks.
    esp_player_task_config_t tasks{};
    auto task = [](int core, int priority) {
        esp_gmf_task_config_t t{};
        t.stack = 6144; t.prio = priority; t.core = core; t.stack_in_ext = false;
        return t;
    };
    tasks.extractor = task(0, 4);
    tasks.audio_decoder = task(0, 5);
    tasks.audio_render = task(0, 6);
    tasks.video_decoder = task(1, 5);
    tasks.video_render = task(1, 4);
    if (esp_player_set_task_config(player, &tasks)) return fail("分核配置失败");
    frame_serial = working_serial;
    if (control.serial() != working_serial) { closeSync(); return false; }
    timings.setup_ms = static_cast<uint32_t>((esp_timer_get_time() - setup_at) / 1000);
    const auto run_at = esp_timer_get_time();
    esp_player_data_src_t source = ESP_PLAYER_DATA_SRC(path.c_str(), ESP_PLAYER_MASK_AV);
    if (esp_player_set_data_src(player, &source) || esp_player_run(player)) return fail("无法播放，请转换为 MJPEG AVI");
    timings.run_ms = static_cast<uint32_t>((esp_timer_get_time() - run_at) / 1000);
    return true;
}

bool closeSync(bool drain_audio) {
    if (player) { esp_player_deinit(player); player = nullptr; }
    if (audio_render) { esp_audio_render_destroy(audio_render); audio_render = nullptr; }
    if (video_render) { esp_video_render_destroy(video_render); video_render = nullptr; }
    // A successful PCM write means queued, not yet audible. Preserve the EOF
    // tail (4 DMA descriptors * 240 frames / 44100 Hz), but stop immediately on cancel/error.
    if (drain_audio && legacy) vTaskDelay(pdMS_TO_TICKS(30) + 1);
    const bool output_stopped = output.stop() == ESP_OK;
    if (audio_pool) { esp_gmf_pool_deinit(audio_pool); audio_pool = nullptr; }
    if (video_pool) { esp_gmf_pool_deinit(video_pool); video_pool = nullptr; }
    heap_caps_free(framebuffer); framebuffer = nullptr;
    latest.attach(nullptr, 0);
    heap_caps_free(mailbox); mailbox = nullptr;
    if (lease) { lease->release(); lease = nullptr; }
    if (!output_stopped) {
        last_error = "I2S 释放失败，请重试";
        ESP_LOGE("video", "%s", last_error);
        return false;
    }
    if (legacy) { legacy->allowPlayback(); legacy = nullptr; }
    owns_audio = false;
    return true;
}
Status statusSync() {
    Status result = timings;
    result.first_frame_ms = first_frame_ms;
    result.backend_copy_us = copy_us;
    result.control_stack_min_bytes = uxTaskGetStackHighWaterMark(nullptr);
    result.frames = latest.frames(); result.replaced_frames = latest.replaced();
    if (!player) return result;
    esp_player_state_t state;
    if (esp_player_get_state(player, &state)) { result.state = State::Failed; return result; }
    switch (state) {
        case ESP_PLAYER_STATE_PREPARING: result.state = State::Preparing; break;
        case ESP_PLAYER_STATE_PLAYING: result.state = State::Playing; break;
        case ESP_PLAYER_STATE_PAUSED: result.state = State::Paused; break;
        case ESP_PLAYER_STATE_FINISHED: result.state = State::Finished; break;
        case ESP_PLAYER_STATE_ERROR: result.state = State::Failed; break;
        default: break;
    }
    esp_player_get_play_time(player, &result.position_ms);
    esp_player_get_duration(player, &result.duration_ms);
    return result;
}

void controlTask(void*) {
    uint32_t processed = 0;
    for (;;) {
        Control command;
        command = control.command();
        if (command.serial != processed) {
            working_serial = command.serial;
            working_requested_at = command.requested_at;
            timings = {};
            timings.queue_ms = static_cast<uint32_t>((esp_timer_get_time() - command.requested_at) / 1000);
            const auto close_at = esp_timer_get_time();
            bool ok = closeSync(command.drain);
            timings.close_ms = static_cast<uint32_t>((esp_timer_get_time() - close_at) / 1000);
            if (ok && control.serial() == command.serial) {
                if (command.action == Action::Open)
                    ok = openSync(*command.gate, *command.audio, command.path);
                else if (command.action == Action::Audio) {
                    uint64_t id = 0;
                    ok = command.audio->play(command.path, command.start, id) == ESP_OK;
                    if (!ok) last_error = "音频启动失败";
                } else if (command.audio) {
                    // A stop may overtake a queued audio handoff. Queue its stop
                    // after any already-submitted play before reopening admission.
                    ok = command.audio->stopAndWait(3000) == ESP_OK;
                    if (ok) command.audio->allowPlayback();
                    else last_error = "音频停止超时";
                }
            }
            processed = command.serial;
            Status value = statusSync();
            if (!ok) {
                value.state = State::Failed;
                std::snprintf(value.error, sizeof(value.error), "%s", last_error);
            }
            control.complete(processed, value);
            ESP_LOGI("video-perf", "request=%lu stop=%lums setup=%lums run=%lums heap=%u",
                     (unsigned long)processed, (unsigned long)value.stop_audio_ms,
                     (unsigned long)value.setup_ms, (unsigned long)value.run_ms,
                     unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        }
        if (player && processed == control.serial()) {
            Status value = statusSync();
            const bool want_pause = pause_requested;
            if (want_pause && value.state == State::Playing) esp_player_pause(player);
            else if (!want_pause && value.state == State::Paused) esp_player_resume(player);
            if (value.state == State::Failed)
                std::snprintf(value.error, sizeof(value.error), "%s", "视频解码失败");
            control.update(processed, value);
        }
        // Idle control tasks sleep until a request; active playback refreshes
        // its snapshot at 100 Hz. Notifications also wake pause/cancel promptly.
        ulTaskNotifyTake(pdTRUE, player ? std::max(TickType_t{1}, pdMS_TO_TICKS(10)) : portMAX_DELAY);
    }
}
} // namespace

bool open(Storage::AccessGate& gate, Audio::Playback& audio, const std::string& path) {
    if (path.size() >= sizeof(Control::path)) return false;
    Control c;
    c.action = Action::Open; c.gate = &gate; c.audio = &audio;
    std::snprintf(c.path, sizeof(c.path), "%s", path.c_str());
    return submit(c);
}
bool close(bool drain_audio) {
    if (!busy()) return true;
    Control c; c.drain = drain_audio;
    return submit(c);
}
bool active() { return video_requested; }
bool busy() { return video_requested || owns_audio || control.pending(); }
bool pause(bool paused) {
    if (!active()) return false;
    pause_requested = paused;
    xTaskNotifyGive(controller);
    return true;
}
esp_err_t playAudio(Audio::Playback& audio, const char* path, bool start, uint64_t& request) {
    if (!busy()) return audio.play(path, start, request);
    if (!path || std::strlen(path) >= sizeof(Control::path)) return ESP_ERR_INVALID_ARG;
    Control c; c.action = Action::Audio; c.audio = &audio; c.start = start;
    std::snprintf(c.path, sizeof(c.path), "%s", path);
    request = 0;
    return submit(c) ? ESP_OK : ESP_ERR_NO_MEM;
}
void setVolume(unsigned value) { volume = value > 21 ? 21 : value; }
void setEq(Audio::EqPreset preset) { eq = preset; }
Status status() {
    return control.snapshot();
}
bool framePending() { return active() && frame_serial.load() == control.serial() && latest.pending(); }
bool consumeFrame(uint16_t* pixels, std::size_t count) {
    if (!active() || frame_serial.load() != control.serial() || !pixels || count < Width * Height) return false;
    return latest.consume(pixels, Bytes);
}
bool visitFrame(FrameVisitor visitor, void* context) {
    if (!visitor || !active() || frame_serial.load() != control.serial()) return false;
    return latest.visit([&](const void* data) { visitor(static_cast<const uint16_t*>(data), context); });
}
const char* error() {
    static char message[96];
    const auto value = control.snapshot();
    std::snprintf(message, sizeof(message), "%s", value.error[0] ? value.error : "播放器请求失败");
    return message;
}
}
