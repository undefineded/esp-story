#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace Persistence {
struct Settings {
    uint8_t brightness=100, rotation=3, eq=0, reserved=0;
    uint8_t ble=1, wifi=0, ap=0, screen_off_min=5, sleep_min=1;
};
using SettingsRecord = std::array<uint8_t,14>;
enum class LoadResult { Loaded, Migrated, Invalid, IoError };
inline uint8_t checksum(const uint8_t* data,unsigned size) {
    uint8_t result=0; for (unsigned i=0;i<size;++i) result+=data[i]; return result;
}
inline bool validMinutes(uint8_t minutes) {
    return minutes==0 || minutes==1 || minutes==3 || minutes==5 || minutes==10 || minutes==30;
}
inline Settings sanitize(Settings value) {
    if (value.brightness<10 || value.brightness>100) value.brightness=100;
    if (value.rotation!=1 && value.rotation!=3) value.rotation=3;
    if (value.eq>3) value.eq=0;
    if (value.ble>1) value.ble=1;
    if (value.wifi>1) value.wifi=0;
    if (value.ap>1) value.ap=0;
    if (!validMinutes(value.screen_off_min)) value.screen_off_min=5;
    if (!validMinutes(value.sleep_min)) value.sleep_min=1;
    return value;
}
inline LoadResult decodeSettings(const SettingsRecord& record,Settings& settings) {
    const bool v4=std::memcmp(record.data(),"MPL4",4)==0 && checksum(record.data(),13)==record[13];
    const bool v3=std::memcmp(record.data(),"MPL3",4)==0 && checksum(record.data(),11)==record[11];
    if (!v4 && !v3) return LoadResult::Invalid;
    Settings value;
    value.brightness=record[4]; value.rotation=record[5]; value.eq=record[6]; value.reserved=record[7];
    value.ble=record[8]; value.wifi=record[9]; value.ap=record[10];
    if (v4) { value.screen_off_min=record[11]; value.sleep_min=record[12]; }
    settings=sanitize(value);
    return v4?LoadResult::Loaded:LoadResult::Migrated;
}
inline SettingsRecord encodeSettings(Settings value) {
    value=sanitize(value);
    SettingsRecord result={'M','P','L','4',value.brightness,value.rotation,value.eq,value.reserved,
        value.ble,value.wifi,value.ap,value.screen_off_min,value.sleep_min,0};
    result[13]=checksum(result.data(),13); return result;
}
// Owner-task API. Device exposes read/write(offset, bytes, size), returning 0 on
// success. Read failure never triggers an implicit factory-reset write.
template<class Device> class SettingsStore {
public:
    explicit SettingsStore(Device& device):device_(device) {}
    LoadResult load() {
        SettingsRecord record{};
        if (device_.read(0,record.data(),record.size())!=0) return LoadResult::IoError;
        Settings candidate;
        const auto result=decodeSettings(record,candidate);
        if (result==LoadResult::Loaded || result==LoadResult::Migrated) {
            value_=candidate;
            dirty_=result==LoadResult::Migrated || encodeSettings(value_)!=record;
        }
        return result;
    }
    Settings value() const { return value_; }
    void set(Settings value) { value_=sanitize(value); dirty_=true; }
    bool dirty() const { return dirty_; }
    bool save() {
        if (!dirty_) return true;
        const auto record=encodeSettings(value_);
        if (device_.write(0,record.data(),record.size())!=0) return false;
        SettingsRecord verified{};
        if (device_.read(0,verified.data(),verified.size())!=0 || verified!=record) return false;
        dirty_=false; return true;
    }
private:
    Device& device_;
    Settings value_;
    bool dirty_=false;
};
}
