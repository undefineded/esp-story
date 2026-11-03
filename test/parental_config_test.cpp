#include "parental_config.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using namespace Persistence;

namespace {

ParentalRecord freshRecord() {
    ParentalRecord record{};
    std::memcpy(record.magic, "PCTL", 4);
    record.version = 1;
    record.daily_limit = 30;
    record.volume_min = 3;
    record.volume_max = 12;
    record.bedtime_start = 20 * 60 + 30;
    record.bedtime_end = 7 * 60;
    record.story_mask = ~uint64_t{0};
    record.category_mask = ~uint64_t{0};
    record.flags = kParentalBound | kParentalHasPin;
    record.timezone = 480;
    std::memcpy(record.bind_code, "123456", 7);
    return record;
}

bool recordIsValid(ParentalRecord record) {
    record.checksum = parentalChecksum(record);
    return validParentalRecord(record);
}

}  // namespace

int main() {
    // An empty patch is not an edit; the caller answers NO_FIELDS.
    ParentalRecord record = freshRecord();
    ParentalPatch empty;
    assert(parentalPatchEmpty(empty));
    assert(!applyParentalPatch(record, empty));

    // Daily limit: 0 stays unlimited, below the minimum is raised to 5, the top
    // of the range is 240.
    ParentalPatch patch;
    patch.has_daily_limit = true;
    patch.daily_limit = 1;
    assert(!parentalPatchEmpty(patch));
    assert(applyParentalPatch(record, patch) && record.daily_limit == 5);
    patch.daily_limit = 0;
    assert(applyParentalPatch(record, patch) && record.daily_limit == 0);
    patch.daily_limit = 600;
    assert(applyParentalPatch(record, patch) && record.daily_limit == 240);
    patch.daily_limit = 45;
    assert(applyParentalPatch(record, patch) && record.daily_limit == 45);

    // One-sided volume patches keep the other side; out-of-range and swapped
    // ranges are normalised.
    record = freshRecord();
    ParentalPatch volume;
    volume.has_volume_min = true;
    volume.volume_min = 7;
    assert(applyParentalPatch(record, volume));
    assert(record.volume_min == 7 && record.volume_max == 12);
    volume = ParentalPatch{};
    volume.has_volume_max = true;
    volume.volume_max = 9;
    assert(applyParentalPatch(record, volume));
    assert(record.volume_min == 7 && record.volume_max == 9);
    volume = ParentalPatch{};
    volume.has_volume_min = true;
    volume.volume_min = 20;
    volume.has_volume_max = true;
    volume.volume_max = 2;
    assert(applyParentalPatch(record, volume));
    assert(record.volume_min == 2 && record.volume_max == 20);
    volume = ParentalPatch{};
    volume.has_volume_min = true;
    volume.volume_min = 200;  // above the ceiling: clamped, not wrapped
    volume.has_volume_max = true;
    volume.volume_max = 200;
    assert(applyParentalPatch(record, volume));
    assert(record.volume_min == 21 && record.volume_max == 21);

    // Bedtime minutes outside a day roll back to midnight instead of making the
    // record invalid.
    record = freshRecord();
    ParentalPatch bedtime;
    bedtime.has_bedtime_start = true;
    bedtime.bedtime_start = 1440;
    bedtime.has_bedtime_end = true;
    bedtime.bedtime_end = 1441;
    bedtime.has_bedtime_enabled = true;
    bedtime.bedtime_enabled = true;
    assert(applyParentalPatch(record, bedtime));
    assert(record.bedtime_start == 0 && record.bedtime_end == 0);
    assert((record.flags & kParentalBedtime) != 0);
    bedtime.bedtime_enabled = false;
    assert(applyParentalPatch(record, bedtime));
    assert((record.flags & kParentalBedtime) == 0);

    // Masks and the independent boolean flags.
    record = freshRecord();
    ParentalPatch masks;
    masks.has_story_mask = true;
    masks.story_mask = 0x05;
    masks.has_category_mask = true;
    masks.category_mask = 1u << 0 | 1u << 3;
    masks.has_allowlist = true;
    masks.allowlist_enabled = true;
    masks.has_remote = true;
    masks.remote_locked = true;
    masks.has_child_lock = true;
    masks.child_lock = true;
    assert(applyParentalPatch(record, masks));
    assert(record.story_mask == 0x05 && record.category_mask == 0x09);
    assert((record.flags & kParentalAllowlist) != 0 && (record.flags & kParentalRemote) != 0 &&
           (record.flags & kParentalChildLock) != 0);
    // A patched record must still satisfy the persisted layout.
    assert(recordIsValid(record));

    // Full status view: seconds, remaining time and the legacy lock numbering.
    record = freshRecord();
    record.daily_limit = 45;
    record.used = 600;
    ParentalStatus status = parentalStatusOf(record, PolicyLock::None, true);
    assert(status.limit == 45 * 60 && status.used == 600);
    assert(parentalRemaining(status) == 45 * 60 - 600);
    assert(status.lock == 0 && status.reason[0] == '\0');
    assert(status.bound && status.has_pin && status.time_synced);
    assert(status.bedtime_start == 20 * 60 + 30 && status.bedtime_end == 7 * 60);

    assert(parentalStatusOf(record, PolicyLock::DailyLimit, true).lock == 1);
    assert(parentalStatusOf(record, PolicyLock::Bedtime, true).lock == 2);
    assert(parentalStatusOf(record, PolicyLock::Remote, true).lock == 3);
    assert(parentalStatusOf(record, PolicyLock::ClockRequired, false).lock == 4);
    assert(parentalStatusOf(record, PolicyLock::Unavailable, false).lock == 5);
    assert(std::strlen(parentalStatusOf(record, PolicyLock::Remote, true).reason) > 0);

    // Unlimited daily quota reports no remaining time instead of underflowing.
    record.daily_limit = 0;
    record.used = 5000;
    status = parentalStatusOf(record, PolicyLock::None, false);
    assert(status.limit == 0 && parentalRemaining(status) == 0);
    record.used = 100;
    assert(parentalStatusOf(record, PolicyLock::None, false).limit == 0);

    // Used beyond the quota clamps at zero.
    record.daily_limit = 10;
    record.used = 601;
    assert(parentalRemaining(parentalStatusOf(record, PolicyLock::DailyLimit, true)) == 0);

    // The unavailable view must never look permissive.
    const ParentalStatus unavailable = parentalStatusUnavailable();
    assert(unavailable.lock == 5 && unavailable.reason[0] != '\0');
    assert(unavailable.volume_max == 21 && !unavailable.bound && !unavailable.time_synced);

    // rebind() keeps the trusted clock and usage; begin() starts from scratch.
    ParentalPolicy policy;
    ParentalRecord live = freshRecord();
    live.checksum = parentalChecksum(live);
    assert(policy.begin(live, 1000));
    assert(!policy.timeSynced());
    assert(policy.syncTime(1700000000, 480, 5000));
    assert(policy.timeSynced());
    policy.tick(true, 7000);  // 6 s since the record began at t=1000 ms
    assert(policy.record().used == 6);

    // Editing one setting must not drop the clock or the accrued usage.
    ParentalRecord edited = policy.record();
    ParentalPatch quota;
    quota.has_daily_limit = true;
    quota.daily_limit = 60;
    assert(applyParentalPatch(edited, quota));
    edited.checksum = parentalChecksum(edited);
    assert(policy.rebind(edited, 8000));
    assert(policy.timeSynced() && policy.epoch(8000) == 1700000003);
    assert(policy.record().used == 6 && policy.record().daily_limit == 60);
    assert(policy.lock(8000) == PolicyLock::None);

    // The same edit applied through begin() would lose the clock, which is why
    // the app uses rebind() for authenticated changes.
    assert(policy.begin(edited, 8000));
    assert(!policy.timeSynced());

    // A record that no longer validates must not be adopted.
    assert(policy.rebind(edited, 8000));
    ParentalRecord broken = edited;
    broken.timezone = 1000;
    broken.checksum = parentalChecksum(broken);
    assert(!policy.rebind(broken, 9000) && !policy.timeSynced());
    assert(policy.lock(9000) == PolicyLock::Unavailable);

    std::puts("parental config ok");
    return 0;
}
