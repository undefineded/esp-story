#include "avi_index.h"

#include <cstring>

namespace Video {
namespace {

uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool tagIs(const uint8_t* p, const char* tag) { return std::memcmp(p, tag, 4) == 0; }

bool readExact(Storage::FileStream& file, void* dst, std::size_t size) {
    return file.read(static_cast<uint8_t*>(dst), size) == size;
}

struct Chunk {
    char id[5] = {0};
    uint32_t size = 0;
    long payload = 0;  // absolute offset of the payload
};

bool readChunk(Storage::FileStream& file, Chunk& chunk) {
    uint8_t header[8];
    if (!readExact(file, header, sizeof(header))) return false;
    std::memcpy(chunk.id, header, 4);
    chunk.id[4] = 0;
    chunk.size = readU32(header + 4);
    chunk.payload = file.tell();
    return true;
}

}  // namespace

bool AviIndex::open(Storage::AccessGate& gate, const std::string& root, const std::string& path) {
    close();
    path_ = path;
    error_.clear();

    Storage::FileStream file(gate);
    if (!file.open(root, path, "rb")) {
        error_ = "打开失败";
        return false;
    }

    uint8_t riff[12];
    if (!readExact(file, riff, sizeof(riff)) || !tagIs(riff, "RIFF") || !tagIs(riff + 8, "AVI ")) {
        error_ = "不是 AVI 文件";
        return false;
    }
    const std::size_t file_size = std::size_t(readU32(riff + 4)) + 8;

    long movi_payload = -1;
    uint32_t movi_size = 0;
    bool have_video_stream = false;
    uint32_t avih_interval_ms = 0;
    uint32_t scale = 0, rate = 0;

    // Walk the top-level chunks until both hdrl and movi are known.
    while (movi_payload < 0 || !have_video_stream) {
        Chunk chunk;
        if (!readChunk(file, chunk)) break;
        const long end = chunk.payload + static_cast<long>(chunk.size);
        if (tagIs(reinterpret_cast<const uint8_t*>(chunk.id), "LIST")) {
            uint8_t type[4];
            if (!readExact(file, type, 4)) break;
            if (tagIs(type, "hdrl")) {
                // Inside hdrl: avih gives the frame interval and the canvas size;
                // each strl carries one stream header.
                long cursor = file.tell();
                while (cursor + 8 <= end) {
                    if (!file.seek(cursor)) break;
                    Chunk inner;
                    if (!readChunk(file, inner)) break;
                    const long inner_end = inner.payload + static_cast<long>(inner.size);
                    if (tagIs(reinterpret_cast<const uint8_t*>(inner.id), "avih") && inner.size >= 40) {
                        uint8_t avih[40];
                        if (!readExact(file, avih, sizeof(avih))) break;
                        avih_interval_ms = readU32(avih) / 1000;
                        width_ = static_cast<int>(readU32(avih + 32));
                        height_ = static_cast<int>(readU32(avih + 36));
                    } else if (tagIs(reinterpret_cast<const uint8_t*>(inner.id), "LIST") && inner.size >= 4) {
                        uint8_t type2[4];
                        if (!readExact(file, type2, 4)) break;
                        if (tagIs(type2, "strl")) {
                            long stream_cursor = file.tell();
                            char handler[5] = {0};
                            while (stream_cursor + 8 <= inner_end) {
                                if (!file.seek(stream_cursor)) break;
                                Chunk part;
                                if (!readChunk(file, part)) break;
                                const long part_end = part.payload + static_cast<long>(part.size);
                                if (tagIs(reinterpret_cast<const uint8_t*>(part.id), "strh") && part.size >= 48) {
                                    uint8_t strh[48];
                                    if (!readExact(file, strh, sizeof(strh))) break;
                                    if (tagIs(strh, "vids")) {
                                        have_video_stream = true;
                                        std::memcpy(handler, strh + 4, 4);
                                        scale = readU32(strh + 20);
                                        rate = readU32(strh + 24);
                                        if (std::memcmp(handler, "MJPG", 4) == 0) codec_ = Codec::Mjpeg;
                                        else if (std::memcmp(handler, "QOI ", 4) == 0) codec_ = Codec::Qoi;
                                    }
                                }
                                stream_cursor = part_end + (part.size & 1);
                            }
                        }
                    }
                    cursor = inner_end + (inner.size & 1);
                }
            } else if (tagIs(type, "movi")) {
                movi_payload = file.tell();
                movi_size = chunk.size - 4;
            }
        }
        if (!file.seek(end + (chunk.size & 1))) break;
        if (file_size && static_cast<std::size_t>(end) >= file_size) break;
    }

    if (movi_payload < 0) {
        error_ = "缺少 movi 数据";
        return false;
    }
    if (!have_video_stream || width_ <= 0 || height_ <= 0) {
        error_ = "缺少视频流信息";
        return false;
    }
    if (!avih_interval_ms) {
        avih_interval_ms = (scale && rate) ? static_cast<uint32_t>(1000ull * scale / rate) : 40;
    }
    frame_interval_ms_ = avih_interval_ms;

    // Enumerate the video chunks inside movi; audio chunks are skipped.
    long cursor = movi_payload;
    const long movi_end = movi_payload + static_cast<long>(movi_size);
    while (cursor + 8 <= movi_end) {
        if (!file.seek(cursor)) break;
        Chunk chunk;
        if (!readChunk(file, chunk)) break;
        const long end = chunk.payload + static_cast<long>(chunk.size);
        if (end > movi_end) break;
        if (chunk.id[0] == '0' && chunk.id[1] == '0' &&
            (chunk.id[2] == 'd') && (chunk.id[3] == 'c' || chunk.id[3] == 'b')) {
            if (frames_.size() >= frame_limit_) {
                error_ = "帧数超过上限";
                frames_.clear();
                return false;
            }
            frames_.push_back({static_cast<uint32_t>(chunk.payload), chunk.size});
        }
        cursor = end + (chunk.size & 1);
    }

    if (frames_.empty()) {
        error_ = "没有视频帧";
        return false;
    }

    // Fall back to sniffing the first payload when the header lacks a codec.
    if (codec_ == Codec::Unknown) {
        uint8_t magic[4];
        if (file.seek(frames_[0].offset) && readExact(file, magic, sizeof(magic))) {
            if (tagIs(magic, "qoif")) codec_ = Codec::Qoi;
            else if (magic[0] == 0xFF && magic[1] == 0xD8) codec_ = Codec::Mjpeg;
        }
    }
    if (codec_ == Codec::Unknown) {
        error_ = "未知视频编码";
        return false;
    }
    if (!file.close()) {
        error_ = "关闭文件失败";
        return false;
    }
    return true;
}

void AviIndex::close() {
    frames_.clear();
    frames_.shrink_to_fit();
    codec_ = Codec::Unknown;
    width_ = height_ = 0;
    frame_interval_ms_ = 0;
    path_.clear();
}

uint32_t AviIndex::durationMs() const {
    return static_cast<uint32_t>(frames_.size()) * frame_interval_ms_;
}

bool AviIndex::readFrame(std::size_t index, Storage::AccessGate& gate, const std::string& root,
                         std::vector<uint8_t>& out, std::size_t max_frame_bytes) {
    if (index >= frames_.size()) return false;
    const auto& frame = frames_[index];
    if (frame.size == 0 || frame.size > max_frame_bytes) {
        error_ = "单帧超过上限";
        return false;
    }
    Storage::FileStream file(gate);
    if (!file.open(root, path_, "rb")) {
        error_ = "打开失败";
        return false;
    }
    out.resize(frame.size);
    if (!file.seek(static_cast<long>(frame.offset)) ||
        file.read(out.data(), out.size()) != out.size()) {
        error_ = "读取帧失败";
        return false;
    }
    if (!file.close()) {
        error_ = "关闭文件失败";
        return false;
    }
    return true;
}

}
