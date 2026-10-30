#include "../components/settings/include/media_records.h"
#include <cassert>
struct Device {
    std::array<uint8_t,2048> bytes{};
    bool fail=false,drop=false;
    unsigned writes=0;
    int read(unsigned offset,uint8_t* data,std::size_t size) {
        assert(offset+size<=bytes.size());std::memcpy(data,bytes.data()+offset,size);return 0;
    }
    int write(unsigned offset,const uint8_t* data,std::size_t size) {
        ++writes; assert(offset+size<=bytes.size());
        if (fail) return -1;
        if (!drop) std::memcpy(bytes.data()+offset,data,size);
        return 0;
    }
};
int main() {
    Device device;Persistence::MediaRecords<Device> records(device);
    assert(records.load() && device.writes==0);
    assert(records.session().volume==4 && records.session().track_id==0);
    assert(records.saveVolume(0));
    assert(device.bytes[0x10]=='V' && device.bytes[0x11]==0 && device.bytes[0x12]=='V');
    assert(records.savePosition("童谣",0x12345678));
    assert(std::memcmp(device.bytes.data()+0x20,"MPS2",4)==0);
    assert(records.setFavorite(299,true) && !records.favoritesDirty());
    assert(!records.setFavorite(300,true));
    Persistence::MediaRecords<Device> reloaded(device);assert(reloaded.load());
    assert(reloaded.session().volume==0 && reloaded.session().category=="童谣" && reloaded.session().track_id==0x12345678);
    assert(reloaded.favorite(299) && !reloaded.favorite(298));
    device.fail=true;assert(!records.saveVolume(10) && records.session().volume==0);
    assert(!records.setFavorite(1,true) && records.favoritesDirty());
    device.fail=false;device.drop=true;
    assert(!records.savePosition("new",3) && records.session().category=="童谣");
    device.drop=false;
    assert(records.setFavorite(1,true) && !records.favoritesDirty());
    assert(!records.savePosition(std::string(40,'x'),4));
    assert(!records.savePosition("../outside",4));
    device.bytes[0x400+42]^=1;
    assert(reloaded.load() && !reloaded.favorite(299));
}
