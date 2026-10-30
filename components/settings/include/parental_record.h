#pragma once
#include <cstdint>
#include <cstring>
#include "settings_record.h"

namespace Persistence {
// Exact legacy PCTL v1 layout at FRAM 0x0300. ESP32 and supported host tests
// are little-endian; do not replace this with an ABI-aligned struct.
#pragma pack(push, 1)
struct ParentalHistory { uint32_t day, used; };
struct ParentalRecord {
    char magic[4];
    uint8_t version, flags;
    uint16_t daily_limit;
    uint8_t volume_min, volume_max;
    uint16_t bedtime_start, bedtime_end;
    uint32_t used, day, last_epoch;
    int16_t timezone;
    uint16_t reserved;
    uint64_t story_mask, category_mask;
    uint32_t pin_salt, pin_hash, token_hash;
    char bind_code[8];
    uint8_t history_count, padding[3];
    ParentalHistory history[7];
    uint32_t checksum;
};
#pragma pack(pop)
static_assert(sizeof(ParentalRecord)==130, "Legacy PCTL layout changed");
inline uint32_t parentalChecksum(const ParentalRecord& record) {
    const auto* bytes=reinterpret_cast<const uint8_t*>(&record);
    uint32_t sum=0;
    for (unsigned i=0;i<sizeof(record)-4;++i) sum=sum*31u+bytes[i];
    return sum;
}
inline bool validParentalRecord(const ParentalRecord& r) {
    if (std::memcmp(r.magic,"PCTL",4)!=0 || r.version!=1 ||
        r.checksum!=parentalChecksum(r)) return false;
    if ((r.flags & ~0x3fu)!=0 || r.volume_min>r.volume_max || r.volume_max>21 ||
        (r.daily_limit && (r.daily_limit<5 || r.daily_limit>240)) ||
        r.bedtime_start>=1440 || r.bedtime_end>=1440 ||
        r.timezone < -840 || r.timezone>840 || r.history_count>7) return false;
    for (unsigned i=0;i<6;++i)
        if (r.bind_code[i]<'0' || r.bind_code[i]>'9') return false;
    return r.bind_code[6]==0;
}

// Single owner. Invalid/unreadable records require explicit provisioning;
// they never become permissive defaults and load never writes to FRAM.
template<class Device> class ParentalStore {
public:
    explicit ParentalStore(Device& device):device_(device) {}
    LoadResult load() {
        ready_=false;
        ParentalRecord candidate{};
        if (device_.read(0x300,reinterpret_cast<uint8_t*>(&candidate),sizeof(candidate))!=0)
            return LoadResult::IoError;
        if (!validParentalRecord(candidate)) return LoadResult::Invalid;
        value_=candidate; ready_=true; return LoadResult::Loaded;
    }
    bool ready() const { return ready_; }
    const ParentalRecord& value() const { return value_; }
    bool save(ParentalRecord candidate) {
        candidate.checksum=parentalChecksum(candidate);
        if (!validParentalRecord(candidate)) return false;
        // A partial write makes persisted authorization uncertain until reload.
        ready_=false;
        if (device_.write(0x300,reinterpret_cast<const uint8_t*>(&candidate),sizeof(candidate))!=0)
            return false;
        ParentalRecord verified{};
        if (device_.read(0x300,reinterpret_cast<uint8_t*>(&verified),sizeof(verified))!=0 ||
            std::memcmp(&candidate,&verified,sizeof(candidate))!=0) return false;
        value_=candidate; ready_=true; return true;
    }
private:
    Device& device_;
    ParentalRecord value_{};
    bool ready_=false;
};
}
