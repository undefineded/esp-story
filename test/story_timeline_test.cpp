#include "../components/media/include/story_timeline.h"
#include <cassert>
#include <fstream>
#include <unistd.h>
int main() {
    uint32_t ms=0;
    assert(Media::Timeline::timestamp("01:02.5",ms) && ms==62500);
    assert(Media::Timeline::timestamp("01:02.543",ms) && ms==62540);
    assert(!Media::Timeline::timestamp("01:60",ms));
    assert(!Media::Timeline::timestamp("99999:59",ms));
    assert(!Media::Timeline::timestamp("ar:title",ms));
    char dir[]="/tmp/storybox-timeline.XXXXXX"; assert(mkdtemp(dir));
    const std::string path=std::string(dir)+"/timeline.lrc";
    { std::ofstream f(path); f<<"[ar:作者]\n[00:00]01.jpg | 开始\n[00:02.50]02.jpg | 下一页\n"; }
    Storage::AccessGate gate; Media::Timeline timeline;
    assert(timeline.load(gate,dir,"/timeline.lrc"));
    assert(timeline.panels().size()==2);
    assert(timeline.atSeconds(2)==0 && timeline.atSeconds(3)==1);
    { std::ofstream f(path); f<<"[00:00]../outside.jpg\n"; }
    assert(!timeline.load(gate,dir,"/timeline.lrc") && timeline.panels().size()==2);
    { std::ofstream f(path); f<<"[00:02]02.jpg\n[00:01]01.jpg\n"; }
    assert(!timeline.load(gate,dir,"/timeline.lrc"));
    assert(gate.beginMaintenance(std::chrono::milliseconds(1)));
    assert(!timeline.load(gate,dir,"/timeline.lrc"));
    gate.endMaintenance();
    assert(timeline.load(gate,dir,"") && timeline.atSeconds(0)==-1);
    assert(std::remove(path.c_str())==0); assert(rmdir(dir)==0);
}
