#pragma once
#include "parental_record.h"
#include <algorithm>
#include <limits>

namespace Persistence {
enum class PolicyLock { None, Unavailable, DailyLimit, Bedtime, Remote, ClockRequired };
// Owner-task policy core. Caller supplies monotonic milliseconds, authenticates
// changes/time synchronization, persists record(), and gates every play entry.
// No wall-clock trust is inferred from a previous boot's persisted epoch.
class ParentalPolicy {
public:
    bool begin(const ParentalRecord& record,uint64_t now_ms) {
        ready_=validParentalRecord(record);
        record_=record; last_tick_=now_ms; fraction_=0;
        synced_=false; dirty_=false; return ready_;
    }
    bool syncTime(uint32_t epoch,int16_t timezone,uint64_t now_ms) {
        if (!ready_ || epoch<1600000000u || timezone < -840 || timezone>840) return false;
        epoch_base_=epoch; clock_base_=now_ms; synced_=true;
        record_.last_epoch=epoch; record_.timezone=timezone;
        rollDay(localDay(epoch)); dirty_=true; return true;
    }
    uint32_t epoch(uint64_t now_ms) const {
        if (!synced_ || now_ms<clock_base_) return 0;
        const uint64_t value=epoch_base_+(now_ms-clock_base_)/1000;
        return static_cast<uint32_t>(std::min<uint64_t>(value,UINT32_MAX));
    }
    void tick(bool playing,uint64_t now_ms) {
        if (!ready_ || now_ms<last_tick_) { last_tick_=now_ms; return; }
        const uint64_t elapsed=now_ms-last_tick_; last_tick_=now_ms;
        // Charge elapsed playback conservatively to the existing day before
        // rollover; a delayed task never silently truncates usage to 2 seconds.
        if (playing) {
            const uint64_t seconds=elapsed/1000+(fraction_+elapsed%1000)/1000;
            fraction_=(fraction_+elapsed%1000)%1000;
            record_.used=static_cast<uint32_t>(std::min<uint64_t>(UINT32_MAX,uint64_t(record_.used)+seconds));
            if (seconds) dirty_=true;
        }
        if (synced_) {
            const auto current=epoch(now_ms);
            rollDay(localDay(current));
            if (record_.last_epoch!=current) { record_.last_epoch=current; dirty_=true; }
        }
    }
    PolicyLock lock(uint64_t now_ms) const {
        if (!ready_) return PolicyLock::Unavailable;
        if (record_.flags&0x10) return PolicyLock::Remote;
        if (record_.daily_limit && record_.used>=uint32_t(record_.daily_limit)*60)
            return PolicyLock::DailyLimit;
        if (record_.flags&0x04) {
            if (!synced_) return PolicyLock::ClockRequired;
            const auto minute=(localSeconds(epoch(now_ms))/60)%1440;
            const auto start=record_.bedtime_start,end=record_.bedtime_end;
            if (start<end ? minute>=start && minute<end :
                start!=end && (minute>=start || minute<end)) return PolicyLock::Bedtime;
        }
        return PolicyLock::None;
    }
    int clampVolume(int volume) const {
        return ready_ ? std::max<int>(record_.volume_min,std::min<int>(record_.volume_max,volume)) : 0;
    }
    bool childLocked() const { return !ready_ || (record_.flags&0x20); }
    bool storyAllowed(int index) const { return allowed(record_.story_mask,index); }
    bool categoryAllowed(int index) const {
        if ((record_.flags&0x08) && index==0) return false;
        return allowed(record_.category_mask,index);
    }
    bool dirty() const { return dirty_; }
    void saved() { dirty_=false; }
    ParentalRecord record() const {
        auto result=record_; result.checksum=parentalChecksum(result); return result;
    }
private:
    bool allowed(uint64_t mask,int index) const {
        if (!ready_ || index<0 || index>=64) return false;
        return !(record_.flags&0x08) || ((mask>>index)&1);
    }
    uint64_t localSeconds(uint32_t epoch) const {
        return static_cast<uint64_t>(std::max<int64_t>(0,int64_t(epoch)+int64_t(record_.timezone)*60));
    }
    uint32_t localDay(uint32_t epoch) const { return localSeconds(epoch)/86400; }
    void rollDay(uint32_t day) {
        if (day<=record_.day) return; // Backward clock correction cannot replenish quota.
        if (record_.day) {
            const auto count=std::min<unsigned>(record_.history_count,6);
            for (unsigned i=count;i>0;--i) record_.history[i]=record_.history[i-1];
            record_.history[0]={record_.day,record_.used};
            record_.history_count=count+1;
            record_.used=0; fraction_=0;
        }
        // First synchronization must retain usage accumulated without a clock.
        record_.day=day; dirty_=true;
    }
    ParentalRecord record_{};
    bool ready_=false, synced_=false, dirty_=false;
    uint64_t last_tick_=0, clock_base_=0, fraction_=0;
    uint32_t epoch_base_=0;
};
}
