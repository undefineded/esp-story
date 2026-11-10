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
// Per-folder resume: which track a folder was playing and where.
// Stored at 0x0060, 12 bytes per slot, 32 slots (0x0060..0x01E7). The region
// stops before 0x01F8, which the FRAM bank probe owns.
struct FolderResume {
    uint32_t folder=0;      // FNV-1a of the category/folder name
    uint32_t track=0;       // Media::Library::nameId of the track name
    uint16_t seconds=0;
    uint16_t offset_kb=0;   // byte offset / 1024, replayed on resume
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
        loadResumes();
        return io_ok;
    }
    const Session& session() const { return session_; }
    bool resume(uint32_t folder, FolderResume& out) const {
        for (unsigned i=0;i<resume_count_;++i)
            if (resumes_[i].folder==folder) { out=resumes_[i]; return true; }
        return false;
    }
    // Writes only the changed slot plus the small header (verified), so a
    // progress save every few seconds costs ~20 I2C bytes, not the whole table.
    bool setResume(uint32_t folder, uint32_t track, uint32_t seconds, uint32_t offset_bytes) {
        if (!folder) return false;
        int slot=-1;
        for (unsigned i=0;i<resume_count_;++i)
            if (resumes_[i].folder==folder) { slot=int(i); break; }
        if (slot<0) {
            if (resume_count_>=kResumeSlots) return false;
            slot=int(resume_count_++);
        }
        const FolderResume entry={folder,track,
            static_cast<uint16_t>(seconds>0xFFFFu?0xFFFFu:seconds),
            static_cast<uint16_t>(offset_bytes/1024u>0xFFFFu?0xFFFFu:offset_bytes/1024u)};
        if (resumes_[slot].track==entry.track && resumes_[slot].seconds==entry.seconds &&
            resumes_[slot].offset_kb==entry.offset_kb) return true;
        resumes_[slot]=entry;
        std::array<uint8_t,kResumeSize> raw{};
        encodeResume(entry,raw.data());
        if (!writeVerified(kResumeBase+kResumeHeader+unsigned(slot)*kResumeSize,raw.data(),raw.size())) return false;
        const auto header=resumeHeader();
        return writeVerified(kResumeBase,header.data(),header.size());
    }
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
    // Deletes one catalog slot and shifts the bits above it down, so removing a
    // track from the card keeps the remaining favorites on their songs.
    bool removeFavoriteAt(unsigned index) {
        if (index>=300) return false;
        for (unsigned i=index;i+1<300;++i) {
            const auto bit=uint8_t(1u<<(i%8));
            if (favorite(i+1)) favorites_[4+i/8]|=bit;
            else favorites_[4+i/8]&=uint8_t(~bit);
        }
        favorites_[4+299/8]&=uint8_t(~(1u<<(299%8)));
        favorites_[42]=favoriteChecksum(favorites_);
        const bool ok=writeVerified(0x400,favorites_.data(),favorites_.size());
        favorites_dirty_=false;
        return ok;
    }
    bool favoritesDirty() const { return favorites_dirty_; }
private:
    static constexpr unsigned kResumeBase = 0x60;
    static constexpr unsigned kResumeHeader = 8;
    static constexpr unsigned kResumeSlots = 32;
    static constexpr unsigned kResumeSize = 12;
    static void encodeResume(const FolderResume& entry, uint8_t* raw) {
        for (unsigned i=0;i<4;++i) raw[i]=uint8_t(entry.folder>>(8*i));
        for (unsigned i=0;i<4;++i) raw[4+i]=uint8_t(entry.track>>(8*i));
        raw[8]=uint8_t(entry.seconds); raw[9]=uint8_t(entry.seconds>>8);
        raw[10]=uint8_t(entry.offset_kb); raw[11]=uint8_t(entry.offset_kb>>8);
    }
    static FolderResume decodeResume(const uint8_t* raw) {
        FolderResume entry{};
        for (unsigned i=0;i<4;++i) entry.folder|=uint32_t(raw[i])<<(8*i);
        for (unsigned i=0;i<4;++i) entry.track|=uint32_t(raw[4+i])<<(8*i);
        entry.seconds=uint16_t(raw[8]|(raw[9]<<8));
        entry.offset_kb=uint16_t(raw[10]|(raw[11]<<8));
        return entry;
    }
    static uint16_t resumeChecksum(const uint8_t* header, const uint8_t* entries, unsigned count) {
        uint16_t sum=0;
        for (unsigned i=0;i<6;++i) sum=uint16_t(sum*31u+header[i]);
        for (unsigned i=0;i<count*12u;++i) sum=uint16_t(sum*31u+entries[i]);
        return sum;
    }
    std::array<uint8_t,8> resumeHeader() const {
        std::array<uint8_t,8> header={'R','S','M','1',uint8_t(resume_count_),0,0,0};
        std::array<uint8_t,kResumeSlots*kResumeSize> raw{};
        for (unsigned i=0;i<resume_count_;++i) encodeResume(resumes_[i],raw.data()+i*kResumeSize);
        const uint16_t sum=resumeChecksum(header.data(),raw.data(),resume_count_);
        header[6]=uint8_t(sum); header[7]=uint8_t(sum>>8);
        return header;
    }
    void loadResumes() {
        resume_count_=0; resumes_.fill(FolderResume{});
        std::array<uint8_t,8> header{};
        if (device_.read(kResumeBase,header.data(),header.size())!=0) return;
        if (std::memcmp(header.data(),"RSM1",4)!=0) return;
        const unsigned count=header[4];
        if (!count || count>kResumeSlots) return;
        std::array<uint8_t,kResumeSlots*kResumeSize> raw{};
        if (device_.read(kResumeBase+kResumeHeader,raw.data(),count*kResumeSize)!=0) return;
        const uint16_t sum=uint16_t(header[6]|(header[7]<<8));
        if (sum!=resumeChecksum(header.data(),raw.data(),count)) return;
        for (unsigned i=0;i<count;++i) resumes_[i]=decodeResume(raw.data()+i*kResumeSize);
        resume_count_=uint8_t(count);
    }
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
    std::array<FolderResume,kResumeSlots> resumes_{};
    uint8_t resume_count_=0;
    std::array<uint8_t,43> favorites_{};
    bool favorites_dirty_=false;
};
}
