#include "ble_service.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#endif

namespace Ble {
namespace {

constexpr const char* kTag = "ble";
constexpr std::size_t kLineCapacity = 520;
constexpr int kQueueDepth = 8;

Hooks s_public_hooks;
Snapshot s_public_snapshot;
char s_name[24] = "StoryBox";

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

// Same 128-bit UUIDs as the Arduino firmware so existing clients keep working.
const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0c, 0x9b, 0x8a,
    0x5f, 0x4e, 0x4d, 0x3c, 0x01, 0x00, 0x7a, 0x6b);
const ble_uuid128_t kRxUuid = BLE_UUID128_INIT(
    0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0c, 0x9b, 0x8a,
    0x5f, 0x4e, 0x4d, 0x3c, 0x02, 0x00, 0x7a, 0x6b);
const ble_uuid128_t kTxUuid = BLE_UUID128_INIT(
    0x5b, 0x4a, 0x3f, 0x2e, 0x1d, 0x0c, 0x9b, 0x8a,
    0x5f, 0x4e, 0x4d, 0x3c, 0x03, 0x00, 0x7a, 0x6b);

struct Line {
    char text[kLineCapacity];
};

Hooks s_hooks;
QueueHandle_t s_line_queue = nullptr;
QueueHandle_t s_command_queue = nullptr;
Snapshot s_snapshot;
bool s_advertising = true;
bool s_authed = false;
bool s_hello_pending = false;
uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_rx_handle = 0;
uint16_t s_tx_handle = 0;
int64_t s_connected_at = 0;
int64_t s_last_state_at = 0;
char s_rx[kLineCapacity];
std::size_t s_rx_length = 0;
bool s_rx_overflow = false;

int64_t millisNow() {
    return static_cast<int64_t>(esp_timer_get_time() / 1000);
}

void pushAction(Action action, int32_t value = 0) {
    if (!s_command_queue) return;
    Command command{action, value};
    xQueueSend(s_command_queue, &command, 0);
}

bool sendText(const char* text) {
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return false;
    const std::size_t length = std::strlen(text);
    if (!length) return false;
    std::size_t sent = 0;
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < 23) mtu = 23;
    const std::size_t chunk = mtu - 3;
    while (sent < length) {
        std::size_t size = length - sent;
        if (size > chunk) size = chunk;
        os_mbuf* packet = ble_hs_mbuf_from_flat(text + sent, static_cast<uint16_t>(size));
        if (!packet) return false;
        if (ble_gatts_notify_custom(s_conn, s_tx_handle, packet) != 0) return false;
        sent += size;
    }
    return true;
}

void sendDoc(cJSON* root) {
    char buffer[760];
    const bool ok = cJSON_PrintPreallocated(root, buffer, sizeof(buffer), false);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(kTag, "tx overflow");
        return;
    }
    const std::size_t length = std::strlen(buffer);
    if (length + 1 >= sizeof(buffer)) return;
    buffer[length] = '\n';
    buffer[length + 1] = '\0';
    sendText(buffer);
}

// Parental status straight from the app; a device without a usable PCTL record
// reports "unavailable" so the phone never reads it as permissive.
Persistence::ParentalStatus readStatus() {
    Persistence::ParentalStatus status{};
    if (!s_hooks.status || !s_hooks.status(s_hooks.context, &status)) {
        return Persistence::parentalStatusUnavailable();
    }
    return status;
}

// Fields the legacy "evt:state" notification carried.
void addStatusFields(cJSON* data, const Persistence::ParentalStatus& status) {
    cJSON_AddNumberToObject(data, "volMin", status.volume_min);
    cJSON_AddNumberToObject(data, "volMax", status.volume_max);
    cJSON_AddNumberToObject(data, "used", status.used);
    cJSON_AddNumberToObject(data, "limit", status.limit);
    cJSON_AddNumberToObject(data, "rem", Persistence::parentalRemaining(status));
    cJSON_AddNumberToObject(data, "lock", status.lock);
    cJSON_AddStringToObject(data, "reason", status.reason);
    cJSON_AddBoolToObject(data, "bedtime", status.bedtime_enabled);
    cJSON_AddBoolToObject(data, "remote", status.remote_locked);
    cJSON_AddBoolToObject(data, "childLock", status.child_lock);
    cJSON_AddBoolToObject(data, "bound", status.bound);
    cJSON_AddBoolToObject(data, "hasPin", status.has_pin);
    cJSON_AddBoolToObject(data, "timeSynced", status.time_synced);
    cJSON_AddNumberToObject(data, "dayKey", status.day);
}

// "get config" / "set" payload: the settings the parent can edit.
void addConfigFields(cJSON* data, const Persistence::ParentalStatus& status) {
    addStatusFields(data, status);
    cJSON_AddNumberToObject(data, "dailyLimitMin", status.daily_limit);
    cJSON_AddNumberToObject(data, "bedStartMin", status.bedtime_start);
    cJSON_AddNumberToObject(data, "bedEndMin", status.bedtime_end);
    cJSON_AddBoolToObject(data, "bedtimeEnabled", status.bedtime_enabled);
    cJSON_AddBoolToObject(data, "allowlistEnabled", status.allowlist_enabled);
    cJSON_AddBoolToObject(data, "remoteLocked", status.remote_locked);
    cJSON_AddNumberToObject(data, "vol", s_snapshot.volume);
}

void sendError(int32_t id, const char* code) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    if (id) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "err", code);
    sendDoc(root);
}

void sendOk(int32_t id) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    if (id) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    sendDoc(root);
}

// "ok" plus the freshly read configuration; used by "set" so the phone shows
// exactly what was stored, not what it asked for.
void sendConfig(int32_t id) {
    const auto status = readStatus();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    if (id) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* data = cJSON_AddObjectToObject(root, "data");
    addConfigFields(data, status);
    sendDoc(root);
}

void addState(cJSON* data) {
    const auto status = readStatus();
    cJSON_AddBoolToObject(data, "play", s_snapshot.playing);
    cJSON_AddBoolToObject(data, "paused", s_snapshot.paused);
    cJSON_AddNumberToObject(data, "vol", s_snapshot.volume);
    cJSON_AddStringToObject(data, "mode", s_snapshot.mode);
    cJSON_AddStringToObject(data, "title", s_snapshot.title);
    cJSON_AddNumberToObject(data, "cur", s_snapshot.current);
    cJSON_AddNumberToObject(data, "dur", s_snapshot.duration);
    addStatusFields(data, status);
}

void sendHello(int32_t id) {
    const auto status = readStatus();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    if (id) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddStringToObject(root, "evt", "hello");
    cJSON_AddStringToObject(root, "fw", "storybox-idf");
    cJSON_AddStringToObject(root, "name", s_name);
    cJSON_AddBoolToObject(root, "bound", status.bound);
    cJSON_AddBoolToObject(root, "hasPin", status.has_pin);
    cJSON_AddNumberToObject(root, "volMin", status.volume_min);
    cJSON_AddNumberToObject(root, "volMax", status.volume_max);
    if (!status.bound && s_hooks.bind_code) {
        cJSON_AddStringToObject(root, "code", s_hooks.bind_code(s_hooks.context));
    }
    sendDoc(root);
}

void sendStateEvent() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "evt", "state");
    addState(root);
    sendDoc(root);
}

// ---- request field helpers ------------------------------------------------

bool numberField(const cJSON* request, const char* name, int& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(request, name);
    if (!cJSON_IsNumber(item)) return false;
    const double value = item->valuedouble;
    if (value > 1000000.0) out = 1000000;
    else if (value < -1000000.0) out = -1000000;
    else out = static_cast<int>(value);
    return true;
}

bool boolField(const cJSON* request, const char* name, bool& out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(request, name);
    if (!cJSON_IsBool(item)) return false;
    out = cJSON_IsTrue(item) != 0;
    return true;
}

// Story/category allowlists travel as index arrays; out-of-range indices are
// dropped instead of failing the whole command (the legacy firmware did the
// same). An empty array is a real value and blocks everything.
uint64_t indexMask(const cJSON* request, const char* name, int minimum, bool& present) {
    const cJSON* array = cJSON_GetObjectItemCaseSensitive(request, name);
    present = cJSON_IsArray(array) != 0;
    uint64_t mask = 0;
    if (!present) return 0;
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsNumber(item)) continue;
        const int index = static_cast<int>(item->valuedouble);
        if (index >= minimum && index < 64) mask |= uint64_t{1} << index;
    }
    return mask;
}

uint16_t clampMinutes(int value, int maximum) {
    if (value <= 0) return 0;
    if (value > maximum) return static_cast<uint16_t>(maximum);
    return static_cast<uint16_t>(value);
}

// Bedtime is a minute of the day; anything outside it rolls back to midnight,
// matching the legacy setters.
uint16_t bedtimeMinute(int value) {
    if (value <= 0 || value >= 1440) return 0;
    return static_cast<uint16_t>(value);
}

uint8_t clampVolume(int value) {
    if (value <= 0) return 0;
    if (value > 21) return 21;
    return static_cast<uint8_t>(value);
}

void handleCtrl(cJSON* request, int32_t id) {
    const char* action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
    if (!action) action = "";
    const cJSON* value_item = cJSON_GetObjectItemCaseSensitive(request, "value");
    const int value = cJSON_IsNumber(value_item) ? static_cast<int>(value_item->valuedouble) : 0;

    Action parsed = Action::None;
    if (std::strcmp(action, "volume") == 0) parsed = Action::Volume;
    else if (std::strcmp(action, "story") == 0) parsed = Action::PlayStory;
    else if (std::strcmp(action, "music") == 0) parsed = Action::PlayMusic;
    else if (std::strcmp(action, "stop") == 0) parsed = Action::Stop;
    else if (std::strcmp(action, "play") == 0) parsed = Action::Play;
    else if (std::strcmp(action, "pause") == 0) parsed = Action::Pause;
    else if (std::strcmp(action, "toggle") == 0) parsed = Action::Toggle;
    else if (std::strcmp(action, "next") == 0) parsed = Action::Next;
    else if (std::strcmp(action, "prev") == 0) parsed = Action::Prev;
    else if (std::strcmp(action, "fav") == 0) parsed = Action::Favourite;
    else {
        sendError(id, "BAD_ACTION");
        return;
    }
    // The app owns the admission rules (lock, allowlist, index bounds) and
    // answers with the legacy error codes instead of a silent no-op.
    if (s_hooks.gate) {
        const char* rejected = s_hooks.gate(s_hooks.context, parsed, value);
        if (rejected && rejected[0]) {
            sendError(id, rejected);
            return;
        }
    }
    if (parsed == Action::Favourite) {
        bool now = false;
        int32_t total = 0;
        if (!s_hooks.toggle_favorite) {
            sendError(id, "UNAVAILABLE");
            return;
        }
        if (!s_hooks.toggle_favorite(s_hooks.context, value, &now, &total)) {
            sendError(id, "UNAVAILABLE");
            return;
        }
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "v", 1);
        if (id) cJSON_AddNumberToObject(root, "id", id);
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "f", now);
        cJSON_AddNumberToObject(root, "n", total);
        sendDoc(root);
        return;
    }
    pushAction(parsed, value);
    sendOk(id);
}

void handleGet(cJSON* request, int32_t id) {
    const char* what = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "what"));
    if (!what) what = "state";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    if (id) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    if (std::strcmp(what, "state") == 0) {
        cJSON* data = cJSON_AddObjectToObject(root, "data");
        addState(data);
        sendDoc(root);
        return;
    }
    if (std::strcmp(what, "config") == 0) {
        cJSON* data = cJSON_AddObjectToObject(root, "data");
        addConfigFields(data, readStatus());
        sendDoc(root);
        return;
    }
    // Catalog pages are built by the app (it owns the catalog and the limits).
    if (s_hooks.list) {
        cJSON* data = cJSON_AddObjectToObject(root, "data");
        if (s_hooks.list(s_hooks.context, what, request, data)) {
            sendDoc(root);
            return;
        }
    }
    cJSON_Delete(root);
    sendError(id, "BAD_WHAT");
}

// "set": patch the parental configuration. Only recognised fields are applied;
// a command that carried none is rejected rather than reported as success.
void handleSet(cJSON* request, int32_t id) {
    Persistence::ParentalPatch patch;
    int number = 0;
    bool flag = false;
    if (numberField(request, "dailyLimitMin", number)) {
        patch.has_daily_limit = true;
        patch.daily_limit = clampMinutes(number, 240);
    }
    if (numberField(request, "volMin", number)) {
        patch.has_volume_min = true;
        patch.volume_min = clampVolume(number);
    }
    if (numberField(request, "volMax", number)) {
        patch.has_volume_max = true;
        patch.volume_max = clampVolume(number);
    }
    if (boolField(request, "bedtimeEnabled", flag)) {
        patch.has_bedtime_enabled = true;
        patch.bedtime_enabled = flag;
    }
    if (numberField(request, "bedStartMin", number)) {
        patch.has_bedtime_start = true;
        patch.bedtime_start = bedtimeMinute(number);
    }
    if (numberField(request, "bedEndMin", number)) {
        patch.has_bedtime_end = true;
        patch.bedtime_end = bedtimeMinute(number);
    }
    if (boolField(request, "allowlistEnabled", flag)) {
        patch.has_allowlist = true;
        patch.allowlist_enabled = flag;
    }
    if (boolField(request, "remoteLocked", flag)) {
        patch.has_remote = true;
        patch.remote_locked = flag;
    }
    if (boolField(request, "childLock", flag)) {
        patch.has_child_lock = true;
        patch.child_lock = flag;
    }
    patch.story_mask = indexMask(request, "allowStories", 0, patch.has_story_mask);
    // Category 0 is "all songs" and is not a configurable entry, matching the
    // legacy list command.
    patch.category_mask = indexMask(request, "allowCats", 1, patch.has_category_mask);

    if (Persistence::parentalPatchEmpty(patch)) {
        sendError(id, "NO_FIELDS");
        return;
    }
    if (!s_hooks.apply_config) {
        sendError(id, "UNAVAILABLE");
        return;
    }
    const char* rejected = s_hooks.apply_config(s_hooks.context, patch);
    if (rejected && rejected[0]) {
        sendError(id, rejected);
        return;
    }
    // Answer with what was stored, not with what the phone asked for.
    sendConfig(id);
}

void handlePin(cJSON* request, int32_t id) {
    const char* action = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "action"));
    const char* pin = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "pin"));
    if (!action) action = "";
    if (std::strcmp(action, "set") == 0) {
        if (!Persistence::parentalPinFormatOk(pin)) {
            sendError(id, "PIN_FORMAT");
            return;
        }
        if (!s_hooks.set_pin) {
            sendError(id, "UNAVAILABLE");
            return;
        }
        const char* rejected = s_hooks.set_pin(s_hooks.context, pin);
        if (rejected && rejected[0]) {
            sendError(id, rejected);
            return;
        }
        sendOk(id);
        return;
    }
    if (std::strcmp(action, "verify") == 0) {
        const bool valid = s_hooks.verify_pin && s_hooks.verify_pin(s_hooks.context, pin ? pin : "");
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "v", 1);
        if (id) cJSON_AddNumberToObject(root, "id", id);
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "valid", valid);
        sendDoc(root);
        return;
    }
    sendError(id, "BAD_ACTION");
}

// "reset" restores permissive defaults (quota, volume range, bedtime window,
// masks, remote/child lock) and shows a fresh bind code. The PIN and the usage
// history survive, so a remote reset cannot silently unlock the settings page.
void handleReset(int32_t id) {
    if (!s_hooks.factory_reset) {
        sendError(id, "UNAVAILABLE");
        return;
    }
    const char* rejected = s_hooks.factory_reset(s_hooks.context);
    if (rejected && rejected[0]) {
        sendError(id, rejected);
        return;
    }
    // The token is gone; force a fresh pairing instead of continuing the session.
    s_authed = false;
    const auto status = readStatus();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    if (id) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "bound", status.bound);
    if (!status.bound && s_hooks.bind_code) {
        cJSON_AddStringToObject(root, "code", s_hooks.bind_code(s_hooks.context));
    }
    sendDoc(root);
}

// "unbind" drops the pairing token but keeps quotas, masks and the PIN.
void handleUnbind(int32_t id) {
    if (!s_hooks.unbind) {
        sendError(id, "UNAVAILABLE");
        return;
    }
    const char* rejected = s_hooks.unbind(s_hooks.context);
    if (rejected && rejected[0]) {
        sendError(id, rejected);
        return;
    }
    s_authed = false;
    const auto status = readStatus();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    if (id) cJSON_AddNumberToObject(root, "id", id);
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "bound", status.bound);
    if (!status.bound && s_hooks.bind_code) {
        cJSON_AddStringToObject(root, "code", s_hooks.bind_code(s_hooks.context));
    }
    sendDoc(root);
}

void processLine(const char* line) {
    cJSON* request = cJSON_Parse(line);
    if (!request) {
        sendError(0, "BAD_JSON");
        return;
    }
    const cJSON* id_item = cJSON_GetObjectItemCaseSensitive(request, "id");
    const int32_t id = cJSON_IsNumber(id_item) ? static_cast<int32_t>(id_item->valuedouble) : 0;
    const char* cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "cmd"));
    if (!cmd) cmd = "";

    if (std::strcmp(cmd, "hello") == 0) {
        sendHello(id);
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "bind") == 0) {
        const char* code = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "code"));
        char token[40] = {};
        if (!s_hooks.bind || !s_hooks.bind(s_hooks.context, code ? code : "", token, sizeof(token))) {
            sendError(id, "BIND");
            cJSON_Delete(request);
            return;
        }
        s_authed = true;
        cJSON* root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "v", 1);
        if (id) cJSON_AddNumberToObject(root, "id", id);
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "token", token);
        sendDoc(root);
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "auth") == 0) {
        const char* token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "token"));
        const char* pin = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(request, "pin"));
        bool ok = token && s_hooks.verify_token && s_hooks.verify_token(s_hooks.context, token);
        if (!ok && pin && s_hooks.verify_pin) ok = s_hooks.verify_pin(s_hooks.context, pin);
        const auto status = readStatus();
        // Legacy rule: a bound device without a PIN has no verifiable secret.
        if (!ok && status.bound && !status.has_pin) ok = true;
        if (!ok) {
            sendError(id, "AUTH");
            cJSON_Delete(request);
            return;
        }
        s_authed = true;
        sendOk(id);
        cJSON_Delete(request);
        return;
    }
    if (!s_authed) {
        sendError(id, "AUTH");
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "sync") == 0) {
        const cJSON* epoch_item = cJSON_GetObjectItemCaseSensitive(request, "epoch");
        const cJSON* tz_item = cJSON_GetObjectItemCaseSensitive(request, "tz");
        const auto epoch = cJSON_IsNumber(epoch_item) ? static_cast<uint32_t>(epoch_item->valuedouble) : 0u;
        const auto tz = cJSON_IsNumber(tz_item) ? static_cast<int16_t>(tz_item->valuedouble) : int16_t{480};
        if (s_hooks.sync_time) s_hooks.sync_time(s_hooks.context, epoch, tz);
        sendOk(id);
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "get") == 0) {
        handleGet(request, id);
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "ctrl") == 0) {
        handleCtrl(request, id);
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "pin") == 0) {
        handlePin(request, id);
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "set") == 0) {
        handleSet(request, id);
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "reset") == 0) {
        handleReset(id);
        cJSON_Delete(request);
        return;
    }
    if (std::strcmp(cmd, "unbind") == 0) {
        handleUnbind(id);
        cJSON_Delete(request);
        return;
    }
    sendError(id, "BAD_CMD");
    cJSON_Delete(request);
}

void startAdvertising() {
    if (!s_advertising || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kServiceUuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    fields.name = reinterpret_cast<const uint8_t*>(s_name);
    fields.name_len = static_cast<uint8_t>(std::strlen(s_name));
    fields.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&fields) != 0) {
        ESP_LOGW(kTag, "advertising fields rejected");
    }
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER, &params,
                                     [](ble_gap_event* event, void*) -> int {
                                         if (event->type == BLE_GAP_EVENT_CONNECT) {
                                             if (event->connect.status == 0) {
                                                 s_conn = event->connect.conn_handle;
                                                 s_connected_at = millisNow();
                                                 s_hello_pending = true;
                                                 s_authed = false;
                                                 s_rx_length = 0;
                                                 s_rx_overflow = false;
                                                 ble_gap_adv_stop();
                                             } else {
                                                 startAdvertising();
                                             }
                                             return 0;
                                         }
                                         if (event->type == BLE_GAP_EVENT_DISCONNECT) {
                                             s_conn = BLE_HS_CONN_HANDLE_NONE;
                                             s_authed = false;
                                             startAdvertising();
                                             return 0;
                                         }
                                         return 0;
                                     },
                                     nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(kTag, "adv start rc=%d", rc);
}

int accessCallback(uint16_t, uint16_t attr_handle, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_rx_handle) return BLE_ATT_ERR_UNLIKELY;
    const uint16_t length = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t buffer[256];
    if (length > sizeof(buffer)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(ctxt->om, 0, length, buffer) != 0) return BLE_ATT_ERR_UNLIKELY;
    for (uint16_t i = 0; i < length; ++i) {
        const char c = static_cast<char>(buffer[i]);
        if (c == '\n') {
            if (!s_rx_overflow && s_rx_length) {
                Line line{};
                std::memcpy(line.text, s_rx, s_rx_length);
                line.text[s_rx_length < kLineCapacity ? s_rx_length : kLineCapacity - 1] = '\0';
                if (s_line_queue) xQueueSend(s_line_queue, &line, 0);
            }
            s_rx_length = 0;
            s_rx_overflow = false;
            continue;
        }
        if (s_rx_length + 1 >= sizeof(s_rx)) {
            s_rx_overflow = true;
            continue;
        }
        s_rx[s_rx_length++] = c;
    }
    return 0;
}

// NimBLE's ble_gatts_chr_is_sane() rejects any characteristic without an
// access callback, even a notify-only one (`BLE_HS_EINVAL` from
// ble_gatts_count_cfg, which is what the first real board reported as
// "GATT registration failed: 3/0"). TX is notify-only, so this callback must
// never be reached; refuse politely instead of dereferencing anything.
int txAccessCallback(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    ESP_LOGW(kTag, "unexpected TX attribute access op=%d", static_cast<int>(ctxt->op));
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

const ble_gatt_chr_def kCharacteristics[] = {
    {
        &kRxUuid.u,          // uuid
        accessCallback,      // access_cb
        nullptr,             // arg
        nullptr,             // descriptors
        static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP),
        0,                   // min_key_size
        &s_rx_handle,        // val_handle
        nullptr,             // cpfd
    },
    {
        &kTxUuid.u,
        txAccessCallback,
        nullptr,
        nullptr,
        BLE_GATT_CHR_F_NOTIFY,
        0,
        &s_tx_handle,
        nullptr,
    },
    {},
};

const ble_gatt_svc_def kServices[] = {
    {
        BLE_GATT_SVC_TYPE_PRIMARY,
        &kServiceUuid.u,
        nullptr,           // includes
        kCharacteristics,  // characteristics
    },
    {},
};

void onSync() {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    startAdvertising();
}

void onReset(int reason) { ESP_LOGW(kTag, "host reset %d", reason); }

void hostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

#endif  // CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

}  // namespace

void begin(const char* device_name, Hooks hooks) {
    s_public_hooks = hooks;
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    std::snprintf(s_name, sizeof(s_name), "%s", device_name ? device_name : "StoryBox");
    s_hooks = hooks;
    s_line_queue = xQueueCreate(kQueueDepth, sizeof(Line));
    s_command_queue = xQueueCreate(kQueueDepth, sizeof(Command));
    if (!s_line_queue || !s_command_queue) {
        ESP_LOGE(kTag, "cannot allocate queues");
        return;
    }
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(kTag, "nimble_port_init failed");
        return;
    }
    ble_hs_cfg.sync_cb = onSync;
    ble_hs_cfg.reset_cb = onReset;
    ble_svc_gap_device_name_set(s_name);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    const int count = ble_gatts_count_cfg(kServices);
    const int added = ble_gatts_add_svcs(kServices);
    if (count != 0 || added != 0) {
        ESP_LOGE(kTag, "GATT registration failed: %d/%d", count, added);
        return;
    }
    nimble_port_freertos_init(hostTask);
    ESP_LOGI(kTag, "BLE service '%s' starting", s_name);
#else
    (void)device_name;
    (void)hooks;
    ESP_LOGW(kTag, "Bluetooth disabled in this build");
#endif
}

void loop() {
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    if (s_hello_pending && s_conn != BLE_HS_CONN_HANDLE_NONE &&
        millisNow() - s_connected_at >= 500) {
        s_hello_pending = false;
        sendHello(0);
    }
    Line line;
    for (int i = 0; i < 4; ++i) {
        if (!s_line_queue || xQueueReceive(s_line_queue, &line, 0) != pdTRUE) break;
        processLine(line.text);
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_authed &&
        millisNow() - s_last_state_at >= 1000) {
        s_last_state_at = millisNow();
        sendStateEvent();
    }
#endif
}

void setAdvertisingEnabled(bool enabled) {
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    s_advertising = enabled;
    if (enabled) {
        startAdvertising();
    } else {
        ble_gap_adv_stop();
    }
#else
    (void)enabled;
#endif
}

bool advertisingEnabled() {
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    return s_advertising;
#else
    return false;
#endif
}

bool connected() {
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
#else
    return false;
#endif
}

void setSnapshot(const Snapshot& snapshot) {
    s_public_snapshot = snapshot;
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    s_snapshot = snapshot;
#endif
}

bool takeCommand(Command& out) {
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED
    return s_command_queue && xQueueReceive(s_command_queue, &out, 0) == pdTRUE;
#else
    (void)out;
    return false;
#endif
}

const char* deviceName() { return s_name; }

}
