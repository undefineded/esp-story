#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace Pn532Frame {
// Normal (non-extended) PN532 response, excluding the I2C RDY byte.
inline bool decode(const uint8_t* frame,std::size_t size,uint8_t response,
                   uint8_t* output,uint8_t& output_size,std::size_t capacity) {
    output_size=0;
    if (!frame || size<9 || frame[0] || frame[1] || frame[2]!=0xff) return false;
    const std::size_t length=frame[3];
    if (length<2 || uint8_t(frame[3]+frame[4])!=0 || length+7>size ||
        frame[5]!=0xd5 || frame[6]!=response || frame[length+6]!=0) return false;
    uint8_t sum=0;
    for (std::size_t i=5;i<=length+5;++i) sum+=frame[i];
    const auto payload=length-2;
    if (sum || payload>capacity || (payload && !output)) return false;
    if (payload) std::memcpy(output,frame+7,payload);
    output_size=payload;
    return true;
}
}
