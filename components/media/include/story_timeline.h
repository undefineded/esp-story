#pragma once
#include "storage_access.h"
#include <vector>
#include <limits>
namespace Media {
struct Panel { uint32_t ms; std::string path; };
class Timeline {
public:
    static bool timestamp(const std::string& text,uint32_t& ms) {
        const auto colon=text.find(':');
        if (colon==std::string::npos) return false;
        const auto dot=text.find('.',colon+1);
        auto number=[](const std::string& value,uint32_t& output) {
            if (value.empty() || value.size()>5) return false;
            output=0;
            for (auto c:value) { if (c<'0'||c>'9') return false; output=output*10+(c-'0'); }
            return true;
        };
        uint32_t minutes,seconds,fraction=0;
        if (!number(text.substr(0,colon),minutes) ||
            !number(text.substr(colon+1,dot==std::string::npos?dot:dot-colon-1),seconds) || seconds>=60) return false;
        if (dot!=std::string::npos) {
            auto part=text.substr(dot+1);
            if (part.empty()) return false;
            for (auto c:part) if (c<'0'||c>'9') return false;
            if (!number(part.substr(0,2),fraction)) return false;
            fraction*=part.size()==1?100:10;
        }
        const uint64_t total=uint64_t(minutes)*60000+seconds*1000+fraction;
        if (total>std::numeric_limits<uint32_t>::max()) return false;
        ms=total; return true;
    }
    bool load(Storage::AccessGate& gate,const std::string& root,const std::string& path) {
        if (path.empty()) { panels_.clear(); return true; }
        Storage::FileStream file(gate);
        if (!file.open(root,path,"rb")) return false;
        std::vector<Panel> next;
        std::string line;
        const auto directory=path.substr(0,path.find_last_of('/'));
        auto accept=[&]() {
            const auto start=line.find_first_not_of(" \t\r");
            if (start==std::string::npos || line[start]!='[') return true;
            const auto end=line.find(']',start);
            if (end==std::string::npos) return true;
            uint32_t ms;
            if (!timestamp(line.substr(start+1,end-start-1),ms)) return true; // ignore LRC metadata
            const auto name_start=line.find_first_not_of(" \t",end+1);
            if (name_start==std::string::npos) return true;
            const auto name_end=line.find_first_of(" \t\r|",name_start);
            const auto name=line.substr(name_start,name_end==std::string::npos?name_end:name_end-name_start);
            if (name.empty() || name.front()=='/' || next.size()>=256) return false;
            std::string normalized;
            if (!Storage::normalizePath(directory+"/"+name,normalized) ||
                normalized.compare(0,directory.size()+1,directory+"/")!=0 ||
                (!next.empty() && ms<next.back().ms)) return false;
            next.push_back({ms,normalized}); return true;
        };
        uint8_t byte;
        while (file.read(&byte,1)==1) {
            if (byte=='\n') { if (!accept()) return false; line.clear(); }
            else { if (line.size()>=1024) return false; line+=static_cast<char>(byte); }
        }
        if (file.failed() || (!line.empty()&&!accept()) || !file.close()) return false;
        panels_=std::move(next); return true;
    }
    int atSeconds(uint32_t seconds) const {
        if (panels_.empty()) return -1;
        int index=0;
        for (std::size_t i=1;i<panels_.size();++i) {
            if (panels_[i].ms>uint64_t(seconds)*1000) break;
            index=i;
        }
        return index;
    }
    const std::vector<Panel>& panels() const { return panels_; }
private:
    std::vector<Panel> panels_;
};
}
