#pragma once
#include <cstddef>
#include <cstdint>

namespace Board {
enum class FramModel { Mb85rc16, Fm24cl64b };
struct Profile {
    const char* name;
    FramModel fram;
    std::size_t fram_capacity;
    uint8_t fram_address;
    int amplifier_enable;
    int usb_present;
};
constexpr Profile legacy{"legacy", FramModel::Mb85rc16, 2048, 0x50, -1, -1};
constexpr Profile rev20261028{"rev_20261028", FramModel::Fm24cl64b, 8192, 0x51, 38, 42};
constexpr int i2c_sda = 1;
constexpr int i2c_scl = 2;
struct Address {
    uint8_t device;
    uint8_t bytes[2];
    std::size_t length;
    std::size_t contiguous;
};
inline bool encode(const Profile& profile, std::size_t offset,
                   std::size_t size, Address& result) {
    if (offset >= profile.fram_capacity || size > profile.fram_capacity - offset) return false;
    if (profile.fram == FramModel::Mb85rc16) {
        result = {static_cast<uint8_t>(0x50 | (offset >> 8)),
                  {static_cast<uint8_t>(offset), 0}, 1, 256 - (offset & 255)};
    } else {
        result = {profile.fram_address,
                  {static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset)},
                  2, profile.fram_capacity - offset};
    }
    return true;
}
}
