#include "media_library.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <unistd.h>
namespace fs = std::filesystem;
static void touch(const fs::path& path) { fs::create_directories(path.parent_path()); std::ofstream file(path); file<<"fixture"; }
int main() {
    char dir[]="/tmp/storybox-media.XXXXXX";
    assert(mkdtemp(dir));
    const fs::path root(dir);
    Storage::AccessGate gate;
    Media::Library library(gate);
    assert(library.scan((root/"missing").string())==Media::ScanResult::IoError);
    assert(library.scan(dir)==Media::ScanResult::Ok);
    touch(root/"music/z.mp3"); touch(root/"music/儿歌/deep/a.MP3");
    touch(root/"music/故事/b.mp3"); touch(root/"music/ignore.wav");
    touch(root/"story/group/book/audio.mp3"); touch(root/"story/group/book/timeline.lrc");
    touch(root/"story/a/b/c/too-deep/audio.mp3");
    assert(library.scan(dir)==Media::ScanResult::Ok);
    auto original=library.snapshot();
    assert(original->tracks.size()==3 && original->categories.size()==2 && original->stories.size()==1);
    assert(!original->stories[0].timeline.empty());
    for (std::size_t i=1;i<original->tracks.size();++i) assert(original->tracks[i-1].path<original->tracks[i].path);
    Media::Playlist active, browse;
    assert(active.select(original,0));
    assert(browse.select(original,0,true) && browse.size()==1);
    assert(active.size()==3 && active.position()==0);
    assert(active.move(-1) && active.position()==2);
    assert(active.move(1) && active.position()==0);
    assert(browse.current()->name=="a.MP3");
    assert(browse.current()->id==Media::Library::nameId("a.MP3"));
    assert(!browse.select(original,0,true,0));
    touch(root/"music/new.mp3");
    assert(library.scan(dir)==Media::ScanResult::Ok);
    assert(original->tracks.size()==3 && active.size()==3 && library.snapshot()->tracks.size()==4);
    auto valid=library.snapshot();
    fs::create_symlink(root/"music/z.mp3",root/"music/link.mp3");
    assert(library.scan(dir)==Media::ScanResult::IoError);
    assert(library.snapshot()==valid);
    fs::remove(root/"music/link.mp3");
    for (int i=0;i<301;++i) touch(root/"music"/(std::to_string(i)+".mp3"));
    assert(library.scan(dir)==Media::ScanResult::LimitExceeded && library.snapshot()==valid);
    assert(gate.beginMaintenance(std::chrono::milliseconds(1)));
    assert(library.scan(dir)==Media::ScanResult::Busy);
    gate.endMaintenance();
    fs::remove_all(root);
}
