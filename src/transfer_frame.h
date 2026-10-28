#pragma once
#include "transfer_crc.h"
#include <cstring>
namespace TransferFrame {
constexpr size_t Header = 16, MaxJson = 24576, MaxData = 4096, Capacity = Header + MaxJson + MaxData;
inline uint16_t u16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
inline uint32_t u32(const uint8_t* p) { return u16(p) | (uint32_t(u16(p + 2)) << 16); }
inline void put32(uint8_t* p, uint32_t v) { for (int i=0;i<4;i++) p[i]=v>>(8*i); }
inline void header(uint8_t* p, size_t json, size_t raw) {
  memcpy(p, "SBX4", 4); p[4]=json; p[5]=json>>8; p[6]=raw; p[7]=raw>>8;
  put32(p+8, transferCrc32(p, 8)); put32(p+12, transferCrc32(p+Header, json+raw));
}
class Decoder {
public:
  uint8_t bytes[Capacity];
  size_t used=0;
  void reset() { used=0; }
  bool feed(uint8_t c) {
    bytes[used++]=c;
    while (used && memcmp(bytes, "SBX4", used<4?used:4)) {
      memmove(bytes, bytes+1, --used);
    }
    if (used < Header) return false;
    const size_t json=u16(bytes+4), raw=u16(bytes+6);
    if (!json || json>MaxJson || raw>MaxData || u32(bytes+8)!=transferCrc32(bytes,8)) {
      reset(); return false;
    }
    if (used != Header+json+raw) return false;
    if (u32(bytes+12)!=transferCrc32(bytes+Header,json+raw)) { reset(); return false; }
    return true;
  }
};
}
