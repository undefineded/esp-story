#include "media_library.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>

namespace Media {
namespace {
std::string lower(std::string value) {
    for (auto& c : value) if (static_cast<unsigned char>(c) < 128) c = std::tolower(static_cast<unsigned char>(c));
    return value;
}
bool extension(const std::string& name, const char* suffix) {
    const auto value = lower(name);
    return value.size() >= 4 && value.compare(value.size()-4,4,suffix) == 0;
}
struct Directory {
    DIR* handle;
    explicit Directory(const std::string& path) : handle(opendir(path.c_str())) {}
    ~Directory() { if (handle) closedir(handle); }
};
struct Scanner {
    const std::string& root;
    Catalog& catalog;
    ScanResult result = ScanResult::Ok;
    template<class Visitor> bool visit(const std::string& path, Visitor visitor, bool missing_ok=false) {
        Directory directory(root+path);
        if (!directory.handle) {
            if (missing_ok && errno == ENOENT) return true;
            result=ScanResult::IoError; return false;
        }
        while (true) {
            errno=0;
            const auto* entry = readdir(directory.handle);
            if (!entry) { if (errno) result=ScanResult::IoError; break; }
            const std::string name=entry->d_name;
            if (name=="." || name=="..") continue;
            const auto child=path+"/"+name;
            if (child.size()>511) { result=ScanResult::LimitExceeded; break; }
            struct stat st{};
#ifdef ESP_PLATFORM
            // ESP-IDF FAT VFS has no symlinks or lstat entry point.
            const int status=stat((root+child).c_str(),&st);
#else
            const int status=lstat((root+child).c_str(),&st);
#endif
            if (status!=0) { result=ScanResult::IoError; break; }
            if (S_ISLNK(st.st_mode)) { result=ScanResult::IoError; break; }
            visitor(child,name,S_ISDIR(st.st_mode),S_ISREG(st.st_mode));
            if (result!=ScanResult::Ok) break;
        }
        return result==ScanResult::Ok;
    }
    void addTrack(const std::string& path,const std::string& name,int category) {
        if (catalog.tracks.size()>=300) { result=ScanResult::LimitExceeded; return; }
        catalog.tracks.push_back({path,name,Library::nameId(name),category});
    }
    void music(const std::string& path,int category,unsigned depth) {
        std::vector<std::pair<std::string,unsigned>> pending{{path,depth}};
        while (!pending.empty() && result==ScanResult::Ok) {
            const auto current=std::move(pending.back()); pending.pop_back();
            if (current.second>16) { result=ScanResult::LimitExceeded; return; }
            visit(current.first,[&](const auto& child,const auto& name,bool dir,bool regular) {
                if (dir) {
                    if (pending.size()>=300) { result=ScanResult::LimitExceeded; return; }
                    pending.emplace_back(child,current.second+1);
                } else if (regular && extension(name,".mp3")) addTrack(child,name,category);
            });
        }
    }
    void stories(const std::string& path,const std::string& title,unsigned depth) {
        std::string audio,timeline;
        std::vector<std::pair<std::string,std::string>> children;
        visit(path,[&](const auto& child,const auto& name,bool dir,bool regular) {
            if (dir && depth<3) {
                if (children.size()>=300) { result=ScanResult::LimitExceeded; return; }
                children.emplace_back(child,name);
            }
            if (regular && audio.empty() && extension(name,".mp3")) audio=child;
            if (regular && timeline.empty() && extension(name,".lrc")) timeline=child;
        },depth==0);
        if (result!=ScanResult::Ok) return;
        if (depth && !audio.empty()) {
            if (catalog.stories.size()>=32) { result=ScanResult::LimitExceeded; return; }
            catalog.stories.push_back({path,title,audio,timeline});
            return;
        }
        for (const auto& child:children) {
            stories(child.first,child.second,depth+1);
            if (result!=ScanResult::Ok) return;
        }
    }
};
}
uint32_t Library::nameId(const std::string& name) {
    uint32_t hash=2166136261u;
    for (unsigned char c:name) hash=(hash^c)*16777619u;
    return hash?hash:1;
}
bool Library::nursery(const std::string& category) {
    const auto value=lower(category);
    return value.find("童谣")!=std::string::npos || value.find("儿歌")!=std::string::npos ||
           value.find("nursery")!=std::string::npos || value.find("rhyme")!=std::string::npos;
}
ScanResult Library::scan(const std::string& root) {
    std::unique_lock<std::mutex> scanning(scan_mutex_,std::try_to_lock);
    if (!scanning.owns_lock() || !gate_.acquire()) return ScanResult::Busy;
    struct Lease { Storage::AccessGate& gate; ~Lease(){gate.release();} } lease{gate_};
    struct stat root_stat{};
    if (stat(root.c_str(), &root_stat)!=0 || !S_ISDIR(root_stat.st_mode)) return ScanResult::IoError;
    auto next=std::make_shared<Catalog>();
    Scanner scanner{root,*next};
    scanner.visit("/music",[&](const auto& path,const auto& name,bool dir,bool regular) {
        if (dir) {
            if (next->categories.size()>=32) { scanner.result=ScanResult::LimitExceeded; return; }
            const auto before=next->tracks.size();
            scanner.music(path,next->categories.size()+1,1);
            if (next->tracks.size()>before) next->categories.push_back(name);
        } else if (regular && extension(name,".mp3")) scanner.addTrack(path,name,0);
    },true);
    if (scanner.result==ScanResult::Ok) scanner.stories("/story","",0);
    if (scanner.result!=ScanResult::Ok) return scanner.result;
    std::sort(next->tracks.begin(),next->tracks.end(),[](const auto& a,const auto& b){return a.path<b.path;});
    std::sort(next->stories.begin(),next->stories.end(),[](const auto& a,const auto& b){return a.directory<b.directory;});
    std::lock_guard<std::mutex> lock(mutex_);
    next->generation=catalog_->generation+1;
    next->storage_revision=gate_.revision();
    catalog_=next;
    return ScanResult::Ok;
}
bool Playlist::select(std::shared_ptr<const Catalog> catalog,int category,bool nursery_only,uint64_t allowed) {
    if (!catalog || category<0 || std::size_t(category)>catalog->categories.size()) return false;
    std::vector<std::size_t> indices;
    for (std::size_t i=0;i<catalog->tracks.size();++i) {
        const auto& track=catalog->tracks[i];
        if (track.category<0 || track.category>32 || std::size_t(track.category)>catalog->categories.size()) return false;
        if (!(allowed & (uint64_t{1}<<track.category))) continue;
        if (category && category!=track.category) continue;
        if (nursery_only && (!track.category || !Library::nursery(catalog->categories[track.category-1]))) continue;
        indices.push_back(i);
    }
    catalog_=std::move(catalog); indices_=std::move(indices); position_=0;
    return !indices_.empty();
}
const Track* Playlist::current() const {
    return catalog_ && position_<indices_.size()? &catalog_->tracks[indices_[position_]]:nullptr;
}
bool Playlist::move(int delta) {
    if (indices_.empty()) return false;
    const auto count=static_cast<int64_t>(indices_.size());
    position_=((int64_t(position_)+delta)%count+count)%count;
    return true;
}
bool Playlist::seekId(uint32_t id) {
    if (!id || !catalog_) return false;
    for (std::size_t i=0;i<indices_.size();++i) {
        if (catalog_->tracks[indices_[i]].id==id) { position_=i; return true; }
    }
    return false;
}
}
