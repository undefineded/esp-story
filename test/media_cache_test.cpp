#include "media_library.h"
#include "file_reader.h"
#include <cassert>
#include <fstream>
#include <unistd.h>
// Independent legacy v2 fixture builder: fields match src/catalog.cpp,
// src/storage.cpp and src/story.cpp, not the new reader implementation.
static std::vector<uint8_t> fixture() {
    std::vector<uint8_t> bytes;
    auto integer=[&](uint32_t n){for(unsigned i=0;i<4;++i) bytes.push_back(n>>(8*i));};
    auto text=[&](const std::string& value){integer(value.size());bytes.insert(bytes.end(),value.begin(),value.end());};
    integer(0x45535049);integer(2);integer(1);integer(1);
    text("儿歌");text("/music/儿歌/a.mp3");integer(1);integer(1234);
    integer(1);text("/story/小熊");text("/story/小熊/a.mp3");text("/story/小熊/a.lrc");
    uint32_t hash=2166136261u; for(auto byte:bytes) hash=(hash^byte)*16777619u;
    integer(hash);return bytes;
}
static void write(const std::string& path,const std::vector<uint8_t>& bytes) {
    std::ofstream file(path,std::ios::binary);file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
}
int main() {
    char dir[]="/tmp/storybox-cache.XXXXXX";assert(mkdtemp(dir));
    const std::string primary=std::string(dir)+"/.esp-story-index.bin",backup=std::string(dir)+"/.esp-story-index.bak";
    Storage::AccessGate gate;Media::Library library(gate);
    using Result=Storage::UploadResult;
    assert(library.saveCache(dir)==Result::Invalid);
    auto bytes=fixture();write(primary,bytes);
    assert(library.loadCache(dir));
    auto saved=library.snapshot();
    assert(saved->tracks.size()==1 && saved->tracks[0].id==1234 && saved->categories[0]=="儿歌");
    assert(saved->stories[0].title=="小熊");
    assert(gate.acquire()); assert(library.saveCache(dir)==Result::Busy); gate.release();
    assert(library.saveCache(dir)==Result::Ok);
    { std::ifstream file(primary,std::ios::binary);
      const std::vector<uint8_t> actual((std::istreambuf_iterator<char>(file)),{});
      assert(actual==fixture()); }
    const auto recovery=primary+".storybox-backup";
    assert(std::rename(primary.c_str(),recovery.c_str())==0);
    assert(library.loadCache(dir));
    assert(library.saveCache(dir)==Result::RecoveryRequired);
    assert(std::rename(recovery.c_str(),primary.c_str())==0);
    write(primary+".storybox-upload",{1,2,3});
    assert(library.saveCache(dir)==Result::RecoveryRequired);
    assert(std::remove((primary+".storybox-upload").c_str())==0);
    const auto beforeFailure=library.snapshot();
    assert(library.saveCache(std::string(dir)+"/missing")==Result::IoError);
    assert(library.snapshot()==beforeFailure);
    assert(gate.acquire()); gate.release(); // failed save releases maintenance
    write(backup,bytes);bytes.back()^=1;write(primary,bytes);
    assert(library.loadCache(dir)); // fallback to previous valid backup
    auto valid=library.snapshot();write(backup,bytes);
    assert(!library.loadCache(dir) && library.snapshot()==valid);
    bytes=fixture();bytes.push_back(0);write(primary,bytes);
    assert(!library.loadCache(dir)); // checksum valid but trailing garbage
    assert(mkdir((std::string(dir)+"/music").c_str(),0700)==0);
    assert(library.scan(dir)==Media::ScanResult::Ok);
    assert(library.saveCache(dir)==Result::Ok);
    Storage::FileReader transfer(gate,dir,true);
    assert(!transfer.beginSession(0));
    assert(!transfer.beginWrite("/music/new.mp3",3,false,0));
    const uint8_t data[]={1,2,3};
    assert(!transfer.write(transfer.xid(),0,data,3,1));
    assert(!transfer.endWrite(transfer.xid(),2));
    assert(library.scan(dir)==Media::ScanResult::Busy);
    assert(!transfer.endSession(3));
    assert(!library.loadCache(dir)); // Dirty marker blocks even a valid old backup.
    Storage::AccessGate rebootGate;
    Media::Library dirtyReboot(rebootGate);
    assert(!dirtyReboot.loadCache(dir)); // Persistent marker works even with revision reset to zero.
    assert(library.saveCache(dir)==Result::Busy); // Cannot save stale RAM over new media.
    assert(library.scan(dir)==Media::ScanResult::Ok);
    assert(library.snapshot()->tracks.size()==1);
    assert(library.saveCache(dir)==Result::Ok);
    assert(gate.beginMaintenance(std::chrono::milliseconds(0)));
    assert(!library.loadCache(dir));
    gate.endMaintenance();
    Media::Library rebooted(gate);
    assert(rebooted.loadCache(dir) && rebooted.snapshot()->tracks[0].name=="new.mp3");
    assert(access((std::string(dir)+"/.esp-story-index.dirty").c_str(),F_OK)!=0);
    assert(unlink((std::string(dir)+"/music/new.mp3").c_str())==0);
    assert(rmdir((std::string(dir)+"/music").c_str())==0);
    assert(std::remove(primary.c_str())==0 && std::remove(backup.c_str())==0);
    assert(rmdir(dir)==0);
}
