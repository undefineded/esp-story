#include "media_library.h"
#include "index_io.h"
#include <cctype>
namespace Media {
namespace {
struct CacheWriter {
    Storage::UploadTransaction* transaction=nullptr;
    std::size_t offset=0;
    std::size_t read(uint8_t*,std::size_t) { return 0; }
    bool available() { return false; }
    std::size_t write(const uint8_t* bytes,std::size_t size) {
        if (size>262144-offset) return 0;
        if (transaction && size && transaction->append(offset,bytes,size)!=Storage::UploadResult::Ok) return 0;
        offset+=size; return size;
    }
};
bool writeCatalog(CacheWriter& stream,const Catalog& catalog) {
    IndexIO::Archive<CacheWriter> archive(stream,false);
    uint32_t magic=0x45535049,version=2,categories=catalog.categories.size(),tracks=catalog.tracks.size();
    uint32_t stories=catalog.stories.size();
    if (categories>32 || tracks>300 || stories>32 || !archive.number(magic) || !archive.number(version) ||
        !archive.number(categories) || !archive.number(tracks)) return false;
    for (auto name:catalog.categories) if (!archive.text(name)) return false;
    for (const auto& track:catalog.tracks) {
        auto path=track.path; uint32_t category=track.category,id=track.id;
        if (!archive.text(path) || !archive.number(category) || !archive.number(id)) return false;
    }
    if (!archive.number(stories)) return false;
    for (const auto& story:catalog.stories) {
        auto directory=story.directory,audio=story.audio,timeline=story.timeline;
        if (!archive.text(directory) || !archive.text(audio) || !archive.text(timeline)) return false;
    }
    return archive.finish();
}
bool inside(const std::string& path,const std::string& directory,const char* suffix=nullptr) {
    std::string normalized;
    if (!Storage::normalizePath(path,normalized) || normalized!=path ||
        path.compare(0,directory.size()+1,directory+"/")!=0) return false;
    if (!suffix) return true;
    if (path.size()<4) return false;
    auto ext=path.substr(path.size()-4);
    for (auto& c:ext) c=std::tolower(static_cast<unsigned char>(c));
    return ext==suffix;
}
bool readCatalog(Storage::FileStream& file,Catalog& catalog) {
    IndexIO::Archive<Storage::FileStream> archive(file,true);
    uint32_t magic=0,version=0,categories=0,tracks=0,stories=0;
    if (!archive.number(magic) || magic!=0x45535049 || !archive.number(version) || version!=2 ||
        !archive.number(categories) || categories>32 || !archive.number(tracks) || tracks>300) return false;
    for (unsigned i=0;i<categories;++i) {
        std::string name;
        if (!archive.text(name) || name.empty() || name.find('/')!=std::string::npos) return false;
        catalog.categories.push_back(name);
    }
    for (unsigned i=0;i<tracks;++i) {
        Track track; uint32_t category=0,id=0;
        if (!archive.text(track.path) || !inside(track.path,"/music",".mp3") ||
            !archive.number(category) || category>categories || !archive.number(id)) return false;
        track.name=track.path.substr(track.path.find_last_of('/')+1);
        track.category=category;
        track.id=id?id:Library::nameId(track.name);
        catalog.tracks.push_back(std::move(track));
    }
    if (!archive.number(stories) || stories>32) return false;
    for (unsigned i=0;i<stories;++i) {
        Story story;
        if (!archive.text(story.directory) || !inside(story.directory,"/story") ||
            !archive.text(story.audio) || !archive.text(story.timeline) ||
            (!story.audio.empty() && !inside(story.audio,story.directory,".mp3")) ||
            (!story.timeline.empty() && !inside(story.timeline,story.directory,".lrc"))) return false;
        story.title=story.directory.substr(story.directory.find_last_of('/')+1);
        catalog.stories.push_back(std::move(story));
    }
    return archive.finish();
}
}
bool Library::loadCache(const std::string& root) {
    std::unique_lock<std::mutex> scanning(scan_mutex_,std::try_to_lock);
    if (!scanning.owns_lock()) return false;
    if (!gate_.acquire()) return false;
    struct Lease { Storage::AccessGate& gate; ~Lease(){gate.release();} } lease{gate_};
    struct stat dirty{};
    if (stat((root+"/.esp-story-index.dirty").c_str(),&dirty)==0 || errno!=ENOENT) return false;
    for (const auto* path:{"/.esp-story-index.bin","/.esp-story-index.bin.storybox-backup","/.esp-story-index.bak"}) {
        Storage::FileStream file(gate_);
        if (!file.open(root,path,"rb")) continue;
        auto candidate=std::make_shared<Catalog>();
        if (!readCatalog(file,*candidate) || !file.close()) continue;
        std::lock_guard<std::mutex> lock(mutex_);
        candidate->generation=catalog_->generation+1;
        candidate->storage_revision=gate_.revision();
        catalog_=candidate;
        return true;
    }
    return false;
}
Storage::UploadResult Library::saveCache(const std::string& root) {
    using Result=Storage::UploadResult;
    std::unique_lock<std::mutex> scanning(scan_mutex_,std::try_to_lock);
    if (!scanning.owns_lock()) return Result::Busy;
    const auto current=snapshot();
    if (!current->generation) return Result::Invalid; // Never persist an uninitialized catalog.
    CacheWriter counter;
    if (!writeCatalog(counter,*current)) return Result::Invalid;
    if (!gate_.beginMaintenance(std::chrono::milliseconds(0))) return Result::Busy;
    struct Lease { Storage::AccessGate& gate; ~Lease(){gate.endMaintenance();} } lease{gate_};
    if (current->storage_revision!=gate_.revision()) return Result::Busy;
    Storage::UploadTransaction transaction(gate_);
    const auto started=transaction.begin(root,"/.esp-story-index.bin",counter.offset,std::chrono::milliseconds(0),true);
    if (started!=Result::Ok) return started;
    CacheWriter writer{&transaction,0};
    if (!writeCatalog(writer,*current)) return Result::IoError;
    const auto result=transaction.commit();
    if (result!=Result::Ok) return result;
    if (std::remove((root+"/.esp-story-index.dirty").c_str())!=0 && errno!=ENOENT) return Result::IoError;
    return Result::Ok;
}
}
