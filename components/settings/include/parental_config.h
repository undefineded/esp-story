#pragma once
#include <cstdint>

#include "parental_binding.h"
#include "parental_policy.h"

// Parental configuration as it crosses the phone boundary.
//
// The mini-program sends a partial "set" payload and reads a flat status object
// ("get config" and the 1 Hz "evt:state"). Keeping the patch/status types and
// the legacy clamping rules here, free of JSON and FreeRTOS, means the wire
// semantics are host-testable and both the BLE layer and the settings page
// share one definition.
namespace Persistence {

// Every field is optional so one command can change one thing; the has_* flags
// keep "field absent" apart from "field sent as zero/false".
struct ParentalPatch {
    bool has_daily_limit = false;
    uint16_t daily_limit = 0;
    bool has_volume_min = false;
    uint8_t volume_min = 0;
    bool has_volume_max = false;
    uint8_t volume_max = 21;
    bool has_bedtime_enabled = false;
    bool bedtime_enabled = false;
    bool has_bedtime_start = false;
    uint16_t bedtime_start = 0;
    bool has_bedtime_end = false;
    uint16_t bedtime_end = 0;
    bool has_allowlist = false;
    bool allowlist_enabled = false;
    bool has_story_mask = false;
    uint64_t story_mask = 0;
    bool has_category_mask = false;
    uint64_t category_mask = 0;
    bool has_remote = false;
    bool remote_locked = false;
    bool has_child_lock = false;
    bool child_lock = false;
};

inline bool parentalPatchEmpty(const ParentalPatch& patch) {
    return !patch.has_daily_limit && !patch.has_volume_min && !patch.has_volume_max &&
           !patch.has_bedtime_enabled && !patch.has_bedtime_start && !patch.has_bedtime_end &&
           !patch.has_allowlist && !patch.has_story_mask && !patch.has_category_mask &&
           !patch.has_remote && !patch.has_child_lock;
}

// Applies a patch with the legacy clamping rules: a daily limit of 0 means "no
// limit", any other value is 5..240 minutes; the volume range is 0..21 and is
// ordered; bedtime minutes must stay below 1440. Returns false when the patch
// carried no recognised field, so the caller can answer NO_FIELDS.
inline bool applyParentalPatch(ParentalRecord& record, const ParentalPatch& patch) {
    if (parentalPatchEmpty(patch)) return false;

    if (patch.has_daily_limit) {
        uint16_t minutes = patch.daily_limit;
        if (minutes != 0 && minutes < 5) minutes = 5;
        if (minutes > 240) minutes = 240;
        record.daily_limit = minutes;
    }
    if (patch.has_volume_min || patch.has_volume_max) {
        uint8_t lo = patch.has_volume_min ? patch.volume_min : record.volume_min;
        uint8_t hi = patch.has_volume_max ? patch.volume_max : record.volume_max;
        if (lo > 21) lo = 21;
        if (hi > 21) hi = 21;
        if (lo > hi) {
            const uint8_t swap = lo;
            lo = hi;
            hi = swap;
        }
        record.volume_min = lo;
        record.volume_max = hi;
    }
    if (patch.has_bedtime_start) {
        record.bedtime_start = patch.bedtime_start < 1440 ? patch.bedtime_start : 0;
    }
    if (patch.has_bedtime_end) {
        record.bedtime_end = patch.bedtime_end < 1440 ? patch.bedtime_end : 0;
    }
    if (patch.has_bedtime_enabled) {
        parentalSetFlag(record, kParentalBedtime, patch.bedtime_enabled);
    }
    if (patch.has_allowlist) {
        parentalSetFlag(record, kParentalAllowlist, patch.allowlist_enabled);
    }
    if (patch.has_story_mask) record.story_mask = patch.story_mask;
    if (patch.has_category_mask) record.category_mask = patch.category_mask;
    if (patch.has_remote) parentalSetFlag(record, kParentalRemote, patch.remote_locked);
    if (patch.has_child_lock) parentalSetFlag(record, kParentalChildLock, patch.child_lock);
    return true;
}

// Numeric "lock" field, legacy numbering first (None/DailyLimit/Bedtime/Remote
// are 0..3). Unavailable and ClockRequired are native additions, so they take
// the next free codes; older clients only look at 0..3 and show "reason".
inline uint8_t parentalLockCode(PolicyLock lock) {
    switch (lock) {
        case PolicyLock::None: return 0;
        case PolicyLock::DailyLimit: return 1;
        case PolicyLock::Bedtime: return 2;
        case PolicyLock::Remote: return 3;
        case PolicyLock::ClockRequired: return 4;
        case PolicyLock::Unavailable: return 5;
    }
    return 5;
}

// Human readable reason in the device's language; empty when nothing is locked.
inline const char* parentalLockText(PolicyLock lock) {
    switch (lock) {
        case PolicyLock::DailyLimit: return "今日使用时间已用完";
        case PolicyLock::Bedtime: return "已到就寝时间";
        case PolicyLock::Remote: return "家长已远程锁定";
        case PolicyLock::ClockRequired: return "需要先同步时间";
        case PolicyLock::Unavailable: return "家长记录不可用";
        case PolicyLock::None: break;
    }
    return "";
}

// Flat view of everything the phone reads about parental state.
struct ParentalStatus {
    uint16_t daily_limit = 30;
    uint8_t volume_min = 0;
    uint8_t volume_max = 21;
    bool bedtime_enabled = false;
    uint16_t bedtime_start = 0;
    uint16_t bedtime_end = 0;
    bool allowlist_enabled = false;
    bool remote_locked = false;
    bool child_lock = false;
    bool bound = false;
    bool has_pin = false;
    bool time_synced = false;
    uint32_t used = 0;      // seconds used today
    uint32_t limit = 0;     // seconds allowed today, 0 when unlimited
    uint32_t day = 0;       // local day index the usage belongs to
    uint8_t lock = 0;       // parentalLockCode()
    const char* reason = "";
};

inline ParentalStatus parentalStatusOf(const ParentalRecord& record, PolicyLock lock,
                                       bool time_synced) {
    ParentalStatus status{};
    status.daily_limit = record.daily_limit;
    status.volume_min = record.volume_min;
    status.volume_max = record.volume_max;
    status.bedtime_enabled = (record.flags & kParentalBedtime) != 0;
    status.bedtime_start = record.bedtime_start;
    status.bedtime_end = record.bedtime_end;
    status.allowlist_enabled = (record.flags & kParentalAllowlist) != 0;
    status.remote_locked = (record.flags & kParentalRemote) != 0;
    status.child_lock = (record.flags & kParentalChildLock) != 0;
    status.bound = parentalBound(record);
    status.has_pin = parentalHasPin(record);
    status.time_synced = time_synced;
    status.used = record.used;
    status.limit = static_cast<uint32_t>(record.daily_limit) * 60u;
    status.day = record.day;
    status.lock = parentalLockCode(lock);
    status.reason = parentalLockText(lock);
    return status;
}

// Reported by builds or boots where the PCTL record could not be used. The
// phone must not treat this as "permissive": every write is refused.
inline ParentalStatus parentalStatusUnavailable() {
    ParentalStatus status{};
    status.lock = parentalLockCode(PolicyLock::Unavailable);
    status.reason = parentalLockText(PolicyLock::Unavailable);
    return status;
}

// Seconds left today; 0 when unlimited or already exhausted.
inline uint32_t parentalRemaining(const ParentalStatus& status) {
    if (!status.limit || status.used >= status.limit) return 0;
    return status.limit - status.used;
}

}
