#pragma once
#include <cstddef>
#include <cstdint>
namespace Storage { class FileReader; }
namespace Host {
// Diagnostic command router. Optional reader is accessed by one worker only.
// Returns serialized JSON size, zero on output failure; raw_size is reset first.
std::size_t respond(const uint8_t* input,std::size_t size,uint32_t uptime,uint32_t heap,
                    char* output,std::size_t capacity,
                    Storage::FileReader* files=nullptr,uint8_t* raw=nullptr,
                    std::size_t raw_capacity=0,std::size_t* raw_size=nullptr,
                    std::size_t input_raw_size=0,uint64_t monotonic_ms=0,const uint8_t* input_raw=nullptr);
}
