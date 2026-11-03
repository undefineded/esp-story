#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "storage_access.h"

namespace Video {

enum class Codec { Unknown, Qoi, Mjpeg };

struct Frame {
    uint32_t offset = 0;  // absolute file offset of the frame payload
    uint32_t size = 0;
};

// AVI (RIFF) index for frame-per-image 320x240 videos. Only the video stream is
// indexed; audio lives in a sibling MP3 handled by the playback service.
class AviIndex {
public:
    // Parses RIFF/hdrl and walks the movi list recording video chunks.
    bool open(Storage::AccessGate& gate, const std::string& root, const std::string& path);
    void close();

    Codec codec() const { return codec_; }
    int width() const { return width_; }
    int height() const { return height_; }
    std::size_t frames() const { return frames_.size(); }
    const Frame& frame(std::size_t index) const { return frames_[index]; }
    // Frame interval from dwScale/dwRate; 0 when the header did not provide one.
    uint32_t frameIntervalMs() const { return frame_interval_ms_; }
    uint32_t durationMs() const;
    const std::string& error() const { return error_; }

    // Reads one frame payload into `out`, growing it up to `max_frame_bytes`.
    // Returns false when the frame is larger than the limit or the read failed.
    bool readFrame(std::size_t index, Storage::AccessGate& gate, const std::string& root,
                   std::vector<uint8_t>& out, std::size_t max_frame_bytes = 192 * 1024);

private:
    std::string error_;
    Codec codec_ = Codec::Unknown;
    int width_ = 0, height_ = 0;
    uint32_t frame_interval_ms_ = 0;
    std::size_t frame_limit_ = 8000;
    std::string path_;
    std::vector<Frame> frames_;
};

}
