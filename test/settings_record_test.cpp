#include "../components/settings/include/settings_record.h"
#include <cassert>
struct Device {
    Persistence::SettingsRecord data{};
    bool fail_read=false,fail_write=false,drop_write=false;
    unsigned writes=0;
    int read(unsigned,uint8_t* out,std::size_t size) {
        if (fail_read) return -1;
        std::memcpy(out,data.data(),size);
        return 0;
    }
    int write(unsigned,const uint8_t* in,std::size_t size) {
        ++writes; if (fail_write) return -1;
        if (!drop_write) std::memcpy(data.data(),in,size);
        return 0;
    }
};
int main() {
    using namespace Persistence;
    Settings settings;
    settings.brightness=60; settings.screen_off_min=0; settings.sleep_min=0; settings.eq=3;
    settings.video_fit=1; settings.nfc=0;
    auto record=encodeSettings(settings);
    Settings decoded;
    assert(decodeSettings(record,decoded)==LoadResult::Loaded);
    assert(decoded.brightness==60 && decoded.screen_off_min==0 && decoded.sleep_min==0 && decoded.eq==3);
    assert(decoded.video_fit==1 && decoded.nfc==0);
    record[4]^=1;
    assert(decodeSettings(record,decoded)==LoadResult::Invalid && decoded.brightness==60);
    // MPL5 (15 bytes, no nfc) still loads, defaults NFC on, and is rewritten as MPL6.
    record=encodeSettings(settings); record[3]='5'; record[14]=checksum(record.data(),14);
    assert(decodeSettings(record,decoded)==LoadResult::Migrated);
    assert(decoded.video_fit==1 && decoded.nfc==1 && decoded.screen_off_min==0 && decoded.sleep_min==0);
    // MPL4 (14 bytes, no video_fit) still loads and is rewritten as MPL6.
    record=encodeSettings(settings); record[3]='4'; record[13]=checksum(record.data(),13);
    assert(decodeSettings(record,decoded)==LoadResult::Migrated);
    assert(decoded.video_fit==0 && decoded.nfc==1 && decoded.screen_off_min==0 && decoded.sleep_min==0);
    record=encodeSettings(settings); record[3]='3'; record[11]=checksum(record.data(),11);
    assert(decodeSettings(record,decoded)==LoadResult::Migrated);
    assert(decoded.screen_off_min==5 && decoded.sleep_min==1);
    Device device; device.data=record;
    SettingsStore<Device> store(device);
    assert(store.load()==LoadResult::Migrated && store.dirty() && device.writes==0);
    device.fail_write=true; assert(!store.save() && store.dirty());
    device.fail_write=false; device.drop_write=true; assert(!store.save() && store.dirty());
    device.drop_write=false; assert(store.save() && !store.dirty());
    assert(device.data[3]=='6');
    assert(device.data[13]==0);  // v3 record had no video_fit, default stays 0
    assert(device.data[14]==1);  // v3 record had no nfc switch, default stays on
    device.fail_read=true; const auto writes=device.writes;
    assert(store.load()==LoadResult::IoError && device.writes==writes);
    assert(store.value().brightness==60);
}
