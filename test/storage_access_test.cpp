#include "../components/storage/include/storage_access.h"
#include "../src/index_io.h"
#include <cassert>
#include <thread>
#include <atomic>
#include <unistd.h>
int main() {
    std::string path;
    assert(Storage::normalizePath("music//中文/./a.mp3", path));
    assert(path == "/music/中文/a.mp3");
    assert(!Storage::normalizePath("music/../config", path));
    assert(!Storage::normalizePath(std::string("a\0b", 3), path));
    Storage::AccessGate gate;
    assert(gate.acquire());
    assert(!gate.beginMaintenance(std::chrono::milliseconds(1)));
    gate.release();
    assert(gate.beginMaintenance(std::chrono::milliseconds(1)));
    assert(!gate.acquire());
    gate.endMaintenance();
    assert(gate.acquire());
    std::atomic<bool> started{false};
    std::thread maintenance([&] {
        started = true;
        assert(gate.beginMaintenance(std::chrono::seconds(2)));
        assert(!gate.acquire());
        gate.endMaintenance();
    });
    while (!started) std::this_thread::yield();
    gate.release();
    maintenance.join();
    char dir[] = "/tmp/storybox-storage.XXXXXX";
    assert(mkdtemp(dir));
    Storage::FileStream file(gate);
    assert(file.open(dir, "index", "wb"));
    IndexIO::Archive<Storage::FileStream> save(file, false);
    uint32_t number = 42;
    std::string title = "中文标题";
    assert(save.number(number) && save.text(title) && save.finish());
    assert(!gate.beginMaintenance(std::chrono::milliseconds(1)));
    assert(file.close());
    assert(file.open(dir, "index", "rb"));
    IndexIO::Archive<Storage::FileStream> load(file, true);
    number = 0; title.clear();
    assert(load.number(number) && load.text(title) && load.finish());
    assert(number == 42 && title == "中文标题");
    assert(file.close());
    assert(gate.beginMaintenance(std::chrono::milliseconds(1)));
    assert(!file.open(dir, "index", "rb"));
    gate.endMaintenance();
    assert(std::remove((std::string(dir) + "/index").c_str()) == 0);
    assert(rmdir(dir) == 0);
}
