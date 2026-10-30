#pragma once
#include "storage_access.h"
#include "upload_transaction.h"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <sys/stat.h>
#include <dirent.h>
#include <vector>

namespace Storage {
struct FileEntry { std::string name,path; bool directory=false; uint64_t size=0; uint32_t mtime=0; };
struct FileListing { std::string path; std::vector<FileEntry> entries; uint32_t total=0,offset=0,limit=0; };
// One file-worker owner. A session holds maintenance admission closed, so audio,
// images and index publication cannot overlap transfers. Caller must first stop
// active playback; a busy store is never forcibly unmounted or reset.
class FileReader {
public:
    FileReader(AccessGate& gate,std::string root,bool writable=false):gate_(gate),root_(std::move(root)),upload_(gate),writable_(writable) {}
    ~FileReader() { abort(); }
    FileReader(const FileReader&)=delete;
    FileReader& operator=(const FileReader&)=delete;
    const char* beginSession(uint64_t now) {
        expire(now);
        if (session_) return "BUSY";
        if (!gate_.beginMaintenance(std::chrono::milliseconds(0))) return "AUDIO_BUSY";
        session_=true; activity_=now; return nullptr;
    }
    const char* endSession(uint64_t now) {
        expire(now);
        if (file_ || writing_) return "BUSY";
        abort(); return nullptr;
    }
    const char* begin(const std::string& path,uint64_t now) {
        expire(now);
        if (!session_) return "NO_SESSION";
        if (file_ || writing_) return "BUSY";
        std::string normalized;
        struct stat status{};
        if (const auto* error=resolve(path,normalized,status)) return error;
        if (S_ISDIR(status.st_mode)) return "IS_DIR";
        if (!S_ISREG(status.st_mode)) return "BAD_PATH";
        if (status.st_size<0 || uint64_t(status.st_size)>std::min<uint64_t>(UINT32_MAX,LONG_MAX))
            return "TOO_LARGE";
        file_=std::fopen((root_+normalized).c_str(),"rb");
        if (!file_) return "IO";
        path_=std::move(normalized); size_=status.st_size;
        if (++sequence_==0) ++sequence_;
        xid_=sequence_; activity_=read_activity_=now; return nullptr;
    }
    const char* list(const std::string& path,uint32_t offset,uint32_t limit,FileListing& out,uint64_t now) {
        expire(now);
        // Listings can run without a transfer session, using an ordinary lease.
        const bool lease=!session_;
        if (lease && !gate_.acquire()) return "BUSY";
        struct Release { AccessGate& gate; bool held; ~Release() { if (held) gate.release(); } } release{gate_,lease};
        FileListing result; struct stat status{};
        if (const auto* error=resolve(path,result.path,status)) return error;
        if (!S_ISDIR(status.st_mode)) return "NOT_DIR";
        auto* directory=opendir((root_+result.path).c_str());
        if (!directory) return "IO";
        struct Close { DIR* dir; ~Close() { closedir(dir); } } close{directory};
        result.offset=offset; result.limit=std::max<uint32_t>(1,std::min<uint32_t>(limit,16));
        uint32_t examined=0;
        for (;;) {
            errno=0;
            const auto* entry=readdir(directory);
            if (!entry) { if (errno) return "IO"; break; }
            if (++examined>4096) return "LIMIT_EXCEEDED";
            const std::string name=entry->d_name;
            if (name=="." || name=="..") continue;
            std::string full;
            const auto child=(result.path=="/"?"":result.path)+"/"+name;
            const auto* error=resolve(child,full,status);
            if (error) return error;
            if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) continue;
            if (result.total>=offset && result.entries.size()<result.limit)
                result.entries.push_back({name,full,S_ISDIR(status.st_mode),
                    S_ISDIR(status.st_mode)?0:static_cast<uint64_t>(status.st_size),
                    status.st_mtime>0?static_cast<uint32_t>(status.st_mtime):0});
            ++result.total;
        }
        out=std::move(result); activity_=now; return nullptr;
    }
    const char* read(uint32_t xid,uint32_t offset,uint8_t* data,std::size_t capacity,
                     std::size_t& length,uint64_t now) {
        length=0; expire(now);
        if (!session_) return "NO_SESSION";
        if (!file_ || xid!=xid_) return "BAD_XFER";
        if (offset>size_) return "RANGE";
        const auto expected=std::min<uint32_t>(4096,size_-offset);
        if (capacity<expected || (!data && expected)) return "BAD_LENGTH";
        std::clearerr(file_);
        if (std::fseek(file_,offset,SEEK_SET)!=0) return "IO";
        if (expected && std::fread(data,1,expected,file_)!=expected) return "IO";
        if (std::ferror(file_)) return "IO";
        length=expected; activity_=read_activity_=now; return nullptr;
    }
    const char* end(uint32_t xid,uint64_t now) {
        expire(now);
        if (!session_) return "NO_SESSION";
        if (!file_ || xid!=xid_) return "BAD_XFER";
        const bool ok=close(); activity_=now; return ok ? nullptr : "IO";
    }
    void expire(uint64_t now) {
        if (session_ && (now<activity_ || now-activity_>=60000)) { abort(); return; }
        if (file_ && (now<read_activity_ || now-read_activity_>=30000)) close();
    }
    void abort() { upload_.cancel(); writing_=false; committed_xid_=0; close(); if (session_) gate_.endMaintenance(); session_=false; }
    bool writable() const { return writable_; }
    // Explicit recovery never deletes data. Interrupted staging is quarantined;
    // an old backup is restored only when no destination exists. Conflicting
    // destination+backup requires operator inspection, not a guessed winner.
    const char* recover(const std::string& path,uint64_t now) {
        expire(now);
        if (!writable_) return "READ_ONLY";
        if (!session_) return "NO_SESSION";
        if (file_ || writing_) return "BUSY";
        std::string normalized;
        if (!normalizePath(path,normalized) || normalized=="/" || normalized.size()>480 ||
            normalized.find(".storybox-")!=std::string::npos || normalized.find("/.esp-story-index")!=std::string::npos)
            return "BAD_PATH";
        for (unsigned char c:normalized) if (c<32 || c==127) return "BAD_PATH";
        auto exists=[&](const std::string& name,bool& present) -> const char* {
            std::string resolved; struct stat status{};
            const auto* error=resolve(name,resolved,status);
            if (error && std::strcmp(error,"NOT_FOUND")!=0) return error;
            present=!error;
            return present && !S_ISREG(status.st_mode) ? "BAD_PATH" : nullptr;
        };
        bool target=false,backup=false,temporary=false,quarantine=false;
        const auto backupPath=normalized+".storybox-backup",tempPath=normalized+".storybox-upload";
        const auto quarantinePath=normalized+".storybox-interrupted";
        if (const auto* e=exists(normalized,target)) return e;
        if (const auto* e=exists(backupPath,backup)) return e;
        if (const auto* e=exists(tempPath,temporary)) return e;
        if (const auto* e=exists(quarantinePath,quarantine)) return e;
        if ((target && backup) || (temporary && quarantine)) return "RECOVERY_REQUIRED";
        if ((temporary || backup) && !markDirty()) return "IO";
        if (temporary && std::rename((root_+tempPath).c_str(),(root_+quarantinePath).c_str())!=0) return "IO";
        if (backup && std::rename((root_+backupPath).c_str(),(root_+normalized).c_str())!=0) return "RECOVERY_REQUIRED";
        activity_=now; return nullptr;
    }
    uint32_t written() const { return written_; }
    const std::string& committedPath() const { return committed_path_; }
    uint32_t committedSize() const { return committed_size_; }
    const char* beginWrite(const std::string& path,uint32_t size,bool overwrite,uint64_t now) {
        expire(now);
        if (!writable_) return "READ_ONLY";
        if (!session_) return "NO_SESSION";
        if (file_ || writing_) return "BUSY";
#if LONG_MAX < UINT32_MAX
        if (size>static_cast<uint32_t>(LONG_MAX)) return "TOO_LARGE";
#endif
        std::string normalized;
        if (!normalizePath(path,normalized) || normalized=="/" || normalized.size()>480) return "BAD_PATH";
        for (unsigned char c:normalized) if (c<32 || c==127) return "BAD_PATH";
        if (normalized.find(".storybox-")!=std::string::npos || normalized.find("/.esp-story-index")!=std::string::npos)
            return "RESERVED_PATH";
        struct stat status{}; std::string parent;
        auto parentPath=normalized.substr(0,normalized.find_last_of('/'));
        if (const auto* error=resolve(parentPath.empty()?"/":parentPath,parent,status)) return error;
        if (!S_ISDIR(status.st_mode)) return "NOT_DIR";
        std::string existing;
        const auto* found=resolve(normalized,existing,status);
        if (!found) {
            if (S_ISDIR(status.st_mode)) return "IS_DIR";
            if (!overwrite) return "EXISTS";
        } else if (std::strcmp(found,"NOT_FOUND")!=0) return found;
        if (!markDirty()) return "IO";
        const auto result=upload_.begin(root_,normalized,size,std::chrono::milliseconds(0),true);
        if (result!=UploadResult::Ok) return uploadError(result);
        writing_=true; path_=normalized; size_=size; written_=0;
        if (++sequence_==0) ++sequence_;
        xid_=sequence_; committed_xid_=0; activity_=now; return nullptr;
    }
    const char* write(uint32_t xid,uint32_t offset,const uint8_t* data,std::size_t length,uint64_t now) {
        expire(now);
        if (!session_) return "NO_SESSION";
        if (!writing_ || xid!=xid_) return "BAD_XFER";
        const auto result=upload_.append(offset,data,length);
        if (result!=UploadResult::Ok) return uploadError(result);
        written_=std::max<uint32_t>(written_,offset+length); activity_=now; return nullptr;
    }
    const char* endWrite(uint32_t xid,uint64_t now) {
        expire(now);
        if (!session_) return "NO_SESSION";
        if (!writing_) return xid && xid==committed_xid_ ? nullptr : "BAD_XFER";
        if (xid!=xid_) return "BAD_XFER";
        if (written_!=size_) return "INCOMPLETE";
        const auto result=upload_.commit();
        writing_=false; upload_.cancel();
        if (result==UploadResult::Ok) {
            committed_xid_=xid; committed_path_=path_; committed_size_=size_;
        }
        close(); activity_=now;
        return result==UploadResult::Ok ? nullptr : uploadError(result);
    }
    const char* cancel(uint32_t xid,uint64_t now) {
        expire(now);
        if (!session_) return "NO_SESSION";
        if (!writing_) return end(xid,now);
        if (xid!=xid_) return "BAD_XFER";
        const auto result=upload_.cancel(); writing_=false; close(); activity_=now;
        return result==UploadResult::Ok ? nullptr : uploadError(result);
    }
    uint32_t xid() const { return xid_; }
    uint32_t size() const { return size_; }
    const std::string& path() const { return path_; }
private:
    bool markDirty() {
        // Called only while this service owns maintenance. Persist invalidation
        // before touching media, including operations later cancelled or failed.
        auto* marker=std::fopen((root_+"/.esp-story-index.dirty").c_str(),"wb");
        if (!marker) return false;
        const bool synced=std::fputc('1',marker)!=EOF && std::fflush(marker)==0 && fsync(fileno(marker))==0;
        const bool closed=std::fclose(marker)==0;
        if (!synced || !closed) return false;
        gate_.changed(); return true;
    }
    static const char* uploadError(UploadResult result) {
        switch (result) {
            case UploadResult::Busy: return "BUSY";
            case UploadResult::Invalid: return "RANGE";
            case UploadResult::RecoveryRequired: return "RECOVERY_REQUIRED";
            default: return "IO";
        }
    }
    const char* resolve(const std::string& path,std::string& normalized,struct stat& status) const {
        if (path.empty() || !normalizePath(path,normalized)) return "BAD_PATH";
        for (const unsigned char c:normalized) if (c<0x20 || c==0x7f) return "BAD_PATH";
        // FAT has no symlinks. Reject all symlink components on the host too.
        std::size_t at=1;
        for (;;) {
            const auto end=normalized.find('/',at);
            const auto full=root_+normalized.substr(0,end);
#ifdef ESP_PLATFORM
            const int result=stat(full.c_str(),&status);
#else
            const int result=lstat(full.c_str(),&status);
#endif
            if (result!=0) return errno==ENOENT ? "NOT_FOUND" : "IO";
            if (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) return "BAD_PATH";
            if (end==std::string::npos) break;
            if (!S_ISDIR(status.st_mode)) return "BAD_PATH";
            at=end+1;
        }
        return nullptr;
    }
    bool close() {
        const bool ok=!file_ || std::fclose(file_)==0;
        file_=nullptr; xid_=0; size_=0; path_.clear(); return ok;
    }
    AccessGate& gate_;
    const std::string root_;
    std::FILE* file_=nullptr;
    std::string path_;
    uint32_t xid_=0,sequence_=0,size_=0;
    uint64_t activity_=0,read_activity_=0;
    bool session_=false;
    UploadTransaction upload_;
    bool writable_=false,writing_=false;
    uint32_t written_=0,committed_xid_=0;
    std::string committed_path_;
    uint32_t committed_size_=0;
};
}
