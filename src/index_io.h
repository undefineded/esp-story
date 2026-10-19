#pragma once
#include <stdint.h>
#include <stddef.h>

// Portable bounded binary codec. All integers are little endian; the trailing
// FNV-1a checksum detects corrupt/truncated files, not malicious modification.
namespace IndexIO {
template<class Stream> class Archive {
  Stream& file;
  bool reading;
  bool valid = true;
  uint32_t hash = 2166136261u;
  bool bytes(uint8_t* data, size_t size, bool checksum = true) {
    if (!valid) return false;
    const size_t done = reading ? file.read(data, size) : file.write(data, size);
    if (done != size) return valid = false;
    if (checksum) for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return true;
  }
public:
  Archive(Stream& stream, bool load) : file(stream), reading(load) {}
  bool number(uint32_t& n, bool checksum = true) {
    uint8_t b[4] = {uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24)};
    if (!bytes(b, 4, checksum)) return false;
    if (reading) n = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    return true;
  }
  template<class Text> bool text(Text& value) {
    uint32_t size = reading ? 0 : value.length();
    if (!number(size) || size > 511) return valid = false;
    char buffer[512];
    if (reading) {
      if (!bytes(reinterpret_cast<uint8_t*>(buffer), size)) return false;
      for (uint32_t i = 0; i < size; ++i) if (!buffer[i]) return valid = false;
      buffer[size] = 0;
      value = buffer;
    } else if (!bytes(reinterpret_cast<uint8_t*>(const_cast<char*>(value.c_str())), size)) return false;
    return true;
  }
  bool finish() {
    uint32_t expected = hash, stored = hash;
    if (!number(stored, false)) return false;
    return valid && stored == expected && (!reading || !file.available());
  }
};
}
