#pragma once
#include "Arduino.h"
#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>

namespace fs {
class File {
  std::shared_ptr<std::vector<uint8_t>> data;
  size_t position = 0;
  bool writable = false, fail = false;
public:
  File() = default;
  File(std::shared_ptr<std::vector<uint8_t>> bytes, bool write, bool shortWrite)
      : data(bytes), writable(write), fail(shortWrite) {}
  explicit operator bool() const { return bool(data); }
  size_t read(uint8_t* out, size_t n) {
    if (!data || position > data->size()) return 0;
    n = std::min(n, data->size() - position);
    if (n) memcpy(out, data->data() + position, n);
    position += n;
    return n;
  }
  size_t write(const uint8_t* in, size_t n) {
    if (!data || !writable || fail) return 0;
    data->insert(data->end(), in, in + n);
    position += n;
    return n;
  }
  bool available() const { return data && position < data->size(); }
  bool seek(size_t n) { if (!data || n > data->size()) return false; position = n; return true; }
  void flush() {}
  void close() { data.reset(); }
};
}
using fs::File;
