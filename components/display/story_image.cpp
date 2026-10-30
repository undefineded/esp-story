#include "story_image.h"
#include "display.h"
namespace Display {
bool drawStoryImage(Storage::AccessGate& gate,const std::string& root,const std::string& path) {
    std::string normalized;
    if (!Storage::normalizePath(path,normalized) || !gate.acquire()) return false;
    struct Lease { Storage::AccessGate& gate; ~Lease(){gate.release();} } lease{gate};
    return canvas().drawJpgFile((root+normalized).c_str(),0,0,320,200);
}
}
