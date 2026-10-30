#include "../components/settings/include/parental_record.h"
#include "../components/settings/include/parental_policy.h"
#include <array>
#include <cassert>
struct Device {
    std::array<uint8_t,2048> bytes{};
    bool fail_read=false, fail_write=false, corrupt=false;
    unsigned writes=0;
    int read(unsigned offset,uint8_t* out,unsigned size) {
        if (fail_read) return -1;
        std::memcpy(out,bytes.data()+offset,size); return 0;
    }
    int write(unsigned offset,const uint8_t* in,unsigned size) {
        ++writes;
        if (fail_write) return -1;
        std::memcpy(bytes.data()+offset,in,size);
        if (corrupt) bytes[offset]^=1;
        return 0;
    }
};
int main() {
    using namespace Persistence;
    Device d;
    ParentalStore<Device> store(d);
    assert(store.load()==LoadResult::Invalid && !store.ready() && d.writes==0);
    ParentalRecord r{};
    std::memcpy(r.magic,"PCTL",4); r.version=1;
    r.daily_limit=30; r.volume_min=3; r.volume_max=12;
    r.bedtime_start=1230; r.bedtime_end=420; r.timezone=480;
    r.story_mask=~0ULL; r.category_mask=~0ULL;
    std::memcpy(r.bind_code,"123456",7);
    assert(store.save(r) && store.ready());
    // Fixed byte offsets protect compatibility independently of struct size.
    assert(d.bytes[0x306]==30 && d.bytes[0x308]==3 && d.bytes[0x33a]=='1');
    assert(store.load()==LoadResult::Loaded);
    auto invalid=r; invalid.history_count=8;
    assert(!store.save(invalid) && store.ready());
    invalid=r; invalid.bind_code[6]='X'; assert(!store.save(invalid));
    invalid=r; invalid.timezone=1000; assert(!store.save(invalid));
    d.fail_read=true;
    assert(store.load()==LoadResult::IoError && !store.ready());
    d.fail_read=false; assert(store.load()==LoadResult::Loaded);
    d.fail_write=true; assert(!store.save(r) && !store.ready());
    d.fail_write=false; d.corrupt=true;
    assert(!store.save(r) && !store.ready());
    assert(store.load()==LoadResult::Invalid);
    d.corrupt=false; assert(store.save(r));
    d.bytes[0x310]^=1;
    assert(store.load()==LoadResult::Invalid && !store.ready());
    ParentalPolicy policy;
    assert(policy.lock(0)==PolicyLock::Unavailable && policy.clampVolume(10)==0);
    r.checksum=parentalChecksum(r);
    assert(policy.begin(r,0));
    policy.tick(true,299999);
    assert(policy.record().used==299 && policy.lock(299999)==PolicyLock::None);
    policy.tick(true,1800000);
    assert(policy.lock(1800000)==PolicyLock::DailyLimit);
    assert(policy.syncTime(1700000000,480,1800000));
    assert(policy.record().used==1800); // First sync cannot clear unsynced usage.
    policy.tick(false,1800000+86400000);
    assert(policy.record().used==0 && policy.record().history[0].used==1800);
    assert(policy.syncTime(1700000000,480,90000000));
    assert(policy.record().history_count==1); // Backward date does not roll day.
    assert(policy.clampVolume(0)==3 && policy.clampVolume(99)==12);
    r.flags=0x0c; r.story_mask=uint64_t(1)<<63;
    r.checksum=parentalChecksum(r); assert(policy.begin(r,0));
    assert(policy.lock(0)==PolicyLock::ClockRequired);
    assert(policy.storyAllowed(63) && !policy.storyAllowed(64) && !policy.storyAllowed(-1));
    assert(!policy.storyAllowed(0));
    assert(!policy.categoryAllowed(0) && policy.categoryAllowed(1));
    // 2023-11-14 22:13 UTC lies inside the overnight bedtime interval.
    assert(policy.syncTime(1700000000,0,0));
    assert(policy.lock(0)==PolicyLock::Bedtime);
    assert(!policy.syncTime(1,0,0));
    assert(policy.dirty()); policy.saved(); assert(!policy.dirty());
    // Exact bedtime boundaries; same-day intervals and disabled intervals.
    r.bedtime_start=22*60; r.bedtime_end=23*60;
    r.checksum=parentalChecksum(r); assert(policy.begin(r,0));
    assert(policy.syncTime(1699999200,0,0)); // 22:00 UTC
    assert(policy.lock(0)==PolicyLock::Bedtime);
    assert(policy.lock(3600000)==PolicyLock::None);
    r.bedtime_end=r.bedtime_start;
    r.checksum=parentalChecksum(r); assert(policy.begin(r,0));
    assert(policy.syncTime(1699999200,0,0));
    assert(policy.lock(0)==PolicyLock::None);
    r.flags=0x30; r.checksum=parentalChecksum(r); assert(policy.begin(r,0));
    assert(policy.childLocked() && policy.lock(0)==PolicyLock::Remote);
    r.flags=0; r.used=UINT32_MAX-1; r.daily_limit=0;
    r.checksum=parentalChecksum(r); assert(policy.begin(r,100));
    policy.tick(true,10100);
    assert(policy.record().used==UINT32_MAX && policy.lock(10100)==PolicyLock::None);
    assert(validParentalRecord(policy.record()));
    // Pause does not consume quota, fractional playing intervals accumulate.
    r.used=0; r.checksum=parentalChecksum(r); assert(policy.begin(r,0));
    policy.tick(true,500); policy.tick(false,10000); policy.tick(true,10500);
    assert(policy.record().used==1);
    // A failed verification read cannot acknowledge a successful write.
    d.fail_read=true; assert(!store.save(r) && !store.ready());
    d.fail_read=false; assert(store.load()==LoadResult::Loaded);
}
