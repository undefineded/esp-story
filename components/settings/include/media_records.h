#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
namespace Persistence {
struct Session {
    uint8_t volume=4;
    uint32_t track_id=0;
    std::string category;
};
// Legacy FRAM regions: volume 0x10; MPS2 position 0x20; FAV v1 bitmap 0x400.
// No automatic writes on missing/corrupt records. Caller owns task serialization.
template<class Device> class MediaRecords {
public:
    explicit MediaRecords(Device& device):device_(device) {
        favorites_[0]='F'; favorites_[1]='A'; favorites_[2]='V'; favorites_[3]=1;
    }
    bool load() {
        session_={}; favorites_.fill(0); favorites_[0]='F'; favorites_[1]='A'; favorites_[2]='V'; favorites_[3]=1;
        bool io_ok=true;
        std::array<uint8_t,3> volume{};
        if (device_.read(0x10,volume.data(),volume.size())!=0) io_ok=false;
        else if (volume[0]=='V' && volume[1]<=21 && uint8_t(volume[0]+volume[1])==volume[2]) session_.volume=volume[1];
        std::array<uint8_t,49> position{};
        if (device_.read(0x20,position.data(),position.size())!=0) io_ok=false;
        else if (std::memcmp(position.data(),"MPS2",4)==0 && position[4]<40) {
            const auto length=position[4];
            uint8_t sum=0; for (unsigned i=0;i<9u+length;++i) sum+=position[i];
            const std::string category(reinterpret_cast<char*>(position.data()+5),length);
            if (sum==position[9+length] && category.find('/')==std::string::npos && category.find('\0')==std::string::npos) {
                session_.category=category;
                for (unsigned i=0;i<4;++i) session_.track_id|=uint32_t(position[5+length+i])<<(8*i);
            }
        }
        std::array<uint8_t,43> favorite{};
        if (device_.read(0x400,favorite.data(),favorite.size())!=0) io_ok=false;
        else if (std::memcmp(favorite.data(),"FAV",3)==0 && favorite[3]==1 && favorite[42]==favoriteChecksum(favorite)) favorites_=favorite;
        favorites_dirty_=false;
        return io_ok;
    }
    const Session& session() const { return session_; }
    bool saveVolume(uint8_t value) {
        if (value>21) return false;
        const std::array<uint8_t,3> record={'V',value,uint8_t('V'+value)};
        if (!writeVerified(0x10,record.data(),record.size())) return false;
        session_.volume=value; return true;
    }
    bool savePosition(const std::string& category,uint32_t id) {
        if (category.size()>=40 || category.find('/')!=std::string::npos || category.find('\0')!=std::string::npos) return false;
        std::array<uint8_t,49> record{};
        std::memcpy(record.data(),"MPS2",4); record[4]=category.size();
        std::memcpy(record.data()+5,category.data(),category.size());
        const auto end=5+category.size();
        for (unsigned i=0;i<4;++i) record[end+i]=id>>(8*i);
        uint8_t sum=0; for (std::size_t i=0;i<end+4;++i) sum+=record[i];
        record[end+4]=sum;
        if (!writeVerified(0x20,record.data(),end+5)) return false;
        session_.category=category; session_.track_id=id; return true;
    }
    bool favorite(unsigned index) const { return index<300 && (favorites_[4+index/8]&(1u<<(index%8))); }
    bool setFavorite(unsigned index,bool enabled) {
        if (index>=300) return false;
        const auto bit=uint8_t(1u<<(index%8));
        if (enabled) favorites_[4+index/8]|=bit;
        else favorites_[4+index/8]&=uint8_t(~bit);
        favorites_[42]=favoriteChecksum(favorites_); favorites_dirty_=true;
        if (!writeVerified(0x400,favorites_.data(),favorites_.size())) return false;
        favorites_dirty_=false; return true;
    }
    bool favoritesDirty() const { return favorites_dirty_; }
private:
    static uint8_t favoriteChecksum(const std::array<uint8_t,43>& record) {
        uint8_t sum=0;for (unsigned i=0;i<42;++i) sum=uint8_t(sum*31u+record[i]);return sum;
    }
    bool writeVerified(unsigned offset,const uint8_t* data,std::size_t size) {
        if (device_.write(offset,data,size)!=0) return false;
        std::array<uint8_t,49> verified{};
        return device_.read(offset,verified.data(),size)==0 && std::memcmp(data,verified.data(),size)==0;
    }
    Device& device_;
    Session session_;
    std::array<uint8_t,43> favorites_{};
    bool favorites_dirty_=false;
};
}
