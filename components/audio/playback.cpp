#include "playback.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <cstring>
#include <algorithm>

namespace Audio {
esp_err_t Playback::begin() {
    if (queue_) return ESP_ERR_INVALID_STATE;
    queue_ = xQueueCreate(8, sizeof(Command));
    if (!queue_) return ESP_ERR_NO_MEM;
    if (xTaskCreate([](void* self) { static_cast<Playback*>(self)->run(); },
                    "audio", 8192, this, 4, nullptr) != pdPASS) {
        vQueueDelete(queue_); queue_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t Playback::submit(Command& command, uint64_t& id) {
    std::lock_guard<std::mutex> lock(submit_mutex_);
    if (!queue_) return ESP_ERR_INVALID_STATE;
    command.id = next_request_ + 1;
    if (xQueueSend(queue_, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    id = ++next_request_;
    return ESP_OK;
}
esp_err_t Playback::play(const char* path, bool auto_start, uint64_t& id) {
    if (inhibited_) return ESP_ERR_INVALID_STATE;
    if (!path || std::strlen(path) >= sizeof(Command::path)) return ESP_ERR_INVALID_ARG;
    std::string normalized;
    if (!Storage::normalizePath(path, normalized) || normalized == "/") return ESP_ERR_INVALID_ARG;
    Command command{};
    command.action = Action::Play;
    command.auto_start = auto_start;
    std::strcpy(command.path, normalized.c_str());
    return submit(command, id);
}
esp_err_t Playback::pause(bool paused, uint64_t& id) {
    Command command{};
    command.action = paused ? Action::Pause : Action::Resume;
    return submit(command, id);
}
PlaybackSnapshot Playback::snapshot() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    return published_;
}
void Playback::publish() {
    std::lock_guard<std::mutex> lock(status_mutex_);
    published_ = current_;
}
esp_err_t Playback::stopAndWait(uint32_t timeout_ms) {
    inhibited_ = true;
    const auto deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    Command command{};
    command.action = Action::Stop;
    uint64_t id = 0;
    esp_err_t result;
    do {
        result = submit(command, id);
        if (result != ESP_ERR_TIMEOUT) break;
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    if (result != ESP_OK) return result;
    do {
        const auto state = snapshot();
        if (state.request >= id) {
            if (state.state == PlaybackState::Stopped) return ESP_OK;
            return state.error != ESP_OK ? state.error : ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    return ESP_ERR_TIMEOUT;
}
void Playback::finish(PlaybackState state, esp_err_t error) {
    const auto stopped = output_.stop();
    const bool closed = stream_.close();
    pending_frames_ = pending_offset_ = 0;
    if (stopped != ESP_OK) error = stopped;
    if (!closed && error == ESP_OK) error = ESP_FAIL;
    current_.state = error == ESP_OK ? state : PlaybackState::Failed;
    current_.error = error;
}
void Playback::execute(const Command& command) {
    current_.request = command.id;
    if (command.action == Action::Stop) {
        finish(PlaybackState::Stopped, ESP_OK);
    } else if (command.action == Action::Play) {
        if (inhibited_) return; // queued play requests cannot pass maintenance admission
        finish(PlaybackState::Stopped, ESP_OK);
        if (current_.error != ESP_OK) return;
        ++current_.generation;
        equalizer_.reset();
        current_.submitted_frames = 0;
        current_.sample_rate = current_.bitrate = 0;
        std::strcpy(current_.path, command.path);
        const auto result = stream_.open("/sdcard", command.path);
        if (result != ESP_OK) finish(PlaybackState::Failed, result);
        else current_.state = command.auto_start ? PlaybackState::Playing : PlaybackState::Paused;
    } else if (command.action == Action::Pause && current_.state == PlaybackState::Playing) {
        // Keep decoded PCM and file position. Already queued DMA may drain.
        current_.state = PlaybackState::Paused;
    } else if (command.action == Action::Resume && current_.state == PlaybackState::Paused && !inhibited_) {
        current_.state = PlaybackState::Playing;
        last_output_progress_ = esp_timer_get_time();
    }
}
void Playback::step() {
    if (current_.state == PlaybackState::Draining) {
        if (esp_timer_get_time() >= drain_until_) finish(PlaybackState::Ended, ESP_OK);
        return;
    }
    if (current_.state != PlaybackState::Playing) return;
    if (!pending_frames_) {
        switch (stream_.next()) {
        case DecodeStep::NeedInput: return;
        case DecodeStep::Error: finish(PlaybackState::Failed, stream_.error()); return;
        case DecodeStep::End:
            current_.state = PlaybackState::Draining;
            // TX configured with four 240-frame DMA descriptors; allow final tail.
            drain_until_ = esp_timer_get_time() + 10000 + 960000000LL / current_.sample_rate;
            return;
        case DecodeStep::Pcm: break;
        }
        if (!current_.sample_rate) {
            const auto result = output_.begin(stream_.sampleRate());
            if (result != ESP_OK) { finish(PlaybackState::Failed, result); return; }
            current_.sample_rate = stream_.sampleRate();
        } else if (current_.sample_rate != stream_.sampleRate()) {
            finish(PlaybackState::Failed, ESP_ERR_NOT_SUPPORTED); return;
        }
        current_.bitrate = stream_.bitrate();
        pending_frames_ = stream_.frames();
        pending_offset_ = 0;
        equalizer_.configure(current_.sample_rate, eq_.load());
        equalizer_.process(stream_.samples(), pending_frames_, volume_.load());
        last_output_progress_ = esp_timer_get_time();
    }
    std::size_t written = 0;
    const auto result = output_.write(stream_.samples() + pending_offset_ * 2,
                                     pending_frames_, written, 20);
    if (written % 4 || written / 4 > pending_frames_) {
        finish(PlaybackState::Failed, ESP_ERR_INVALID_SIZE); return;
    }
    pending_offset_ += written / 4;
    pending_frames_ -= written / 4;
    current_.submitted_frames += written / 4;
    if (written) last_output_progress_ = esp_timer_get_time();
    else if (esp_timer_get_time() - last_output_progress_ > 2000000) {
        finish(PlaybackState::Failed, ESP_ERR_TIMEOUT); return;
    }
    if (result != ESP_OK && result != ESP_ERR_TIMEOUT) finish(PlaybackState::Failed, result);
}
void Playback::run() {
    while (true) {
        Command command{};
        const auto active = current_.state == PlaybackState::Playing;
        if (xQueueReceive(queue_, &command, active ? 0 : pdMS_TO_TICKS(10)) == pdTRUE) execute(command);
        step();
        publish();
        // Parser/cache-only iterations must not starve idle/watchdog tasks.
        if (active) vTaskDelay(1);
    }
}
}
