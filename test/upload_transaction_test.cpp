#include "../components/storage/include/upload_transaction.h"
#include <cassert>
#include <fstream>
static std::string content(const std::string& path) {
    std::ifstream file(path);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
static unsigned renames=0,failRename=0;
static bool failRestore=false;
static int renameFault(const char* from,const char* to) noexcept {
    ++renames;
    if (renames==failRename || (failRestore && renames==3)) { errno=EIO; return -1; }
    return std::rename(from,to);
}
int main() {
    char dir[] = "/tmp/storybox-upload.XXXXXX";
    assert(mkdtemp(dir));
    const std::string target = std::string(dir) + "/song";
    { std::ofstream original(target); original << "old"; }
    Storage::AccessGate gate;
    Storage::UploadTransaction upload(gate);
    using R = Storage::UploadResult;
    const auto timeout = std::chrono::milliseconds(1);
    const uint8_t data[] = {'n', 'e', 'w'};
    assert(upload.begin(dir, "song", 3, timeout) == R::Ok);
    assert(!gate.acquire());
    assert(upload.append(1, data, 3) == R::Invalid);
    assert(upload.append(0, data, 3) == R::Ok);
    assert(upload.append(0, data, 3) == R::Ok);
    const uint8_t changed[] = {'b','a','d'};
    assert(upload.append(0, changed, 3) == R::Invalid);
    assert(content(target) == "old");
    assert(upload.cancel() == R::Ok);
    assert(content(target) == "old");
    assert(upload.begin(dir, "song", 3, timeout) == R::Ok);
    assert(upload.commit() == R::Invalid);
    assert(upload.append(0, data, 3) == R::Ok);
    assert(upload.commit() == R::Ok);
    assert(content(target) == "new");
    assert(gate.acquire()); gate.release();
    { std::ofstream stale(target + ".storybox-backup"); stale << "old"; }
    assert(upload.begin(dir, "song", 3, timeout) == R::RecoveryRequired);
    assert(content(target + ".storybox-backup") == "old");
    assert(upload.begin(dir, "../outside", 3, timeout) == R::Invalid);
    assert(std::remove((target + ".storybox-backup").c_str()) == 0);
    // Deterministic VFS failures must not truncate the original destination.
    for (unsigned fault=0;fault<6;++fault) {
        Storage::UploadIo io;
        if (fault==0) io.write=[](const void*,std::size_t,std::size_t,std::FILE*) -> std::size_t { return 0; };
        if (fault==1) io.flush=[](std::FILE*) { return EOF; };
        if (fault==2) io.sync=[](int) { return -1; };
        if (fault==3) io.close=[](std::FILE* f) { std::fclose(f); return EOF; };
        if (fault>=4) { io.rename=renameFault; renames=0; failRename=fault-3; }
        Storage::UploadTransaction failing(gate,io);
        assert(failing.begin(dir,"song",3,timeout)==R::Ok);
        if (fault==0) {
            assert(failing.append(0,data,3)==R::IoError);
            assert(failing.commit()==R::Invalid);
            failing.cancel();
        } else {
            assert(failing.append(0,data,3)==R::Ok);
            assert(failing.commit()==R::IoError);
        }
        assert(content(target)=="new");
        assert(gate.acquire()); gate.release();
    }
    // Failed final rename AND failed rollback leave an intact recoverable backup.
    Storage::UploadIo io; io.rename=renameFault;
    renames=0; failRename=2; failRestore=true;
    Storage::UploadTransaction interrupted(gate,io);
    assert(interrupted.begin(dir,"song",3,timeout)==R::Ok);
    assert(interrupted.append(0,changed,3)==R::Ok);
    assert(interrupted.commit()==R::RecoveryRequired);
    assert(content(target+".storybox-backup")=="new");
    assert(access(target.c_str(),F_OK)!=0);
    assert(std::rename((target+".storybox-backup").c_str(),target.c_str())==0);
    // A backend claiming a full write without storing bytes must not publish.
    Storage::UploadIo lying;
    lying.write=[](const void*,std::size_t,std::size_t n,std::FILE*) { return n; };
    Storage::UploadTransaction shortDisk(gate,lying);
    assert(shortDisk.begin(dir,"song",3,timeout)==R::Ok);
    assert(shortDisk.append(0,changed,3)==R::Ok);
    assert(shortDisk.commit()==R::IoError && content(target)=="new");
    assert(std::remove(target.c_str()) == 0);
    assert(rmdir(dir) == 0);
}
