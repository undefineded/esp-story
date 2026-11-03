#include "avi_index.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void putU32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

void putTag(std::vector<uint8_t>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

std::vector<uint8_t> chunk(const char* id, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    putTag(out, id);
    putU32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    if (payload.size() & 1) out.push_back(0);
    return out;
}

std::vector<uint8_t> list(const char* type, const std::vector<std::vector<uint8_t>>& children) {
    std::vector<uint8_t> payload;
    putTag(payload, type);
    for (const auto& child : children) payload.insert(payload.end(), child.begin(), child.end());
    return chunk("LIST", payload);
}

std::vector<uint8_t> avih(uint32_t width, uint32_t height, uint32_t interval_ms) {
    std::vector<uint8_t> out(56, 0);
    const uint32_t us = interval_ms * 1000;
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(us >> (8 * i));
        out[32 + i] = static_cast<uint8_t>(width >> (8 * i));
        out[36 + i] = static_cast<uint8_t>(height >> (8 * i));
    }
    return chunk("avih", out);
}

std::vector<uint8_t> strh(const char* handler, uint32_t scale, uint32_t rate) {
    std::vector<uint8_t> out(56, 0);
    out[0] = 'v'; out[1] = 'i'; out[2] = 'd'; out[3] = 's';
    for (unsigned i = 0; i < 4; ++i) {
        out[4 + i] = static_cast<uint8_t>(handler[i]);
        out[20 + i] = static_cast<uint8_t>(scale >> (8 * i));
        out[24 + i] = static_cast<uint8_t>(rate >> (8 * i));
    }
    return chunk("strh", out);
}

std::vector<uint8_t> strf(const char* compression, uint32_t width, uint32_t height) {
    std::vector<uint8_t> out(40, 0);
    for (unsigned i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(40 >> (8 * i));
        out[4 + i] = static_cast<uint8_t>(width >> (8 * i));
        out[8 + i] = static_cast<uint8_t>(height >> (8 * i));
        out[16 + i] = static_cast<uint8_t>(compression[i]);
    }
    out[12] = 1;   // biPlanes
    out[14] = 24;  // biBitCount
    return chunk("strf", out);
}

std::vector<uint8_t> avi(const std::vector<std::vector<uint8_t>>& frames, const char* handler) {
    const auto hdrl = list("hdrl", {avih(320, 240, 40),
                                    list("strl", {strh(handler, 1, 25), strf(handler, 320, 240)})});
    std::vector<uint8_t> movi_payload;
    putTag(movi_payload, "movi");
    for (const auto& frame : frames) {
        const auto c = chunk("00dc", frame);
        movi_payload.insert(movi_payload.end(), c.begin(), c.end());
    }
    std::vector<uint8_t> body;
    putTag(body, "AVI ");
    body.insert(body.end(), hdrl.begin(), hdrl.end());
    const auto movi = chunk("LIST", movi_payload);
    body.insert(body.end(), movi.begin(), movi.end());

    std::vector<uint8_t> out;
    putTag(out, "RIFF");
    putU32(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::string writeTemp(const std::vector<uint8_t>& bytes, std::string& dir) {
    char template_path[] = "/tmp/sbx-avi-XXXXXX";
    assert(mkdtemp(template_path));
    dir = template_path;
    const std::string path = dir + "/clip.avi";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    assert(file);
    assert(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
    std::fclose(file);
    return path;
}

void cleanup(const std::string& path, const std::string& dir) {
    std::remove(path.c_str());
    rmdir(dir.c_str());
}

}  // namespace

int main() {
    const std::vector<uint8_t> frame0 = {0xFF, 0xD8, 1, 2, 0xFF, 0xD9};
    const std::vector<uint8_t> frame1 = {0xFF, 0xD8, 3, 4, 5, 0xFF, 0xD9};
    const std::vector<uint8_t> frame2 = {0xFF, 0xD8, 6, 0xFF, 0xD9};

    Storage::AccessGate gate;
    Video::AviIndex index;
    std::string dir;
    const auto path = writeTemp(avi({frame0, frame1, frame2}, "MJPG"), dir);
    const std::string name = path.substr(path.find_last_of('/'));

    assert(index.open(gate, dir, name));
    assert(index.codec() == Video::Codec::Mjpeg);
    assert(index.width() == 320 && index.height() == 240);
    assert(index.frames() == 3);
    assert(index.frameIntervalMs() == 40);
    assert(index.durationMs() == 120);

    std::vector<uint8_t> out;
    assert(index.readFrame(1, gate, dir, out) && out.size() == frame1.size());
    assert(std::equal(out.begin(), out.end(), frame1.begin()));
    assert(!index.readFrame(3, gate, dir, out));
    // A tight frame budget must be rejected instead of overflowing the buffer.
    assert(!index.readFrame(0, gate, dir, out, 4));

    // QOI is accepted from the stream handler; the magic is a second signal.
    std::string qoi_dir;
    const auto qoi_path = writeTemp(avi({{'q', 'o', 'i', 'f', 0, 0, 0, 0}}, "QOI "), qoi_dir);
    Video::AviIndex qoi;
    assert(qoi.open(gate, qoi_dir, qoi_path.substr(qoi_path.find_last_of('/'))));
    assert(qoi.codec() == Video::Codec::Qoi && qoi.frames() == 1);

    // Missing files are reported as an error, never as an empty video.
    Video::AviIndex missing;
    assert(!missing.open(gate, "/tmp", "/sbx-does-not-exist.avi"));
    // A RIFF container without movi is rejected.
    std::string riff_dir;
    std::vector<uint8_t> riff_only;
    putTag(riff_only, "RIFF");
    putU32(riff_only, 4);
    putTag(riff_only, "AVI ");
    const auto riff_path = writeTemp(riff_only, riff_dir);
    Video::AviIndex no_movi;
    assert(!no_movi.open(gate, riff_dir, riff_path.substr(riff_path.find_last_of('/'))));

    cleanup(path, dir);
    cleanup(qoi_path, qoi_dir);
    cleanup(riff_path, riff_dir);
    std::puts("avi index ok");
    return 0;
}
