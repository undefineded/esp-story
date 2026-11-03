#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "parental_record.h"

namespace Persistence {

// PCTL v1 flag bits (must match the legacy Arduino layout).
inline constexpr uint8_t kParentalBound = 0x01;
inline constexpr uint8_t kParentalHasPin = 0x02;
inline constexpr uint8_t kParentalBedtime = 0x04;
inline constexpr uint8_t kParentalAllowlist = 0x08;
inline constexpr uint8_t kParentalRemote = 0x10;
inline constexpr uint8_t kParentalChildLock = 0x20;

// Source of entropy: esp_random() on device, a deterministic counter in tests.
using RandomSource = uint32_t (*)();

inline uint32_t parentalFnv1a(const uint8_t* data, std::size_t size) {
    uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

inline uint32_t parentalFnv1aText(const char* text) {
    return parentalFnv1a(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
}

inline bool parentalPinFormatOk(const char* pin) {
    if (!pin) return false;
    const std::size_t length = std::strlen(pin);
    if (length < 4 || length > 6) return false;
    for (std::size_t i = 0; i < length; ++i) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    return true;
}

// Six decimal digits; the mini-program shows them to the parent.
inline void parentalEnsureBindCode(ParentalRecord& record, RandomSource random) {
    if (record.bind_code[0] >= '0' && record.bind_code[0] <= '9') return;
    const uint32_t value = (random ? random() : 0u) % 1000000u;
    std::snprintf(record.bind_code, sizeof(record.bind_code), "%06u", static_cast<unsigned>(value));
}

inline void parentalGenerateToken(RandomSource random, char* out, std::size_t capacity) {
    if (!out || capacity < 33) {
        if (out && capacity) out[0] = '\0';
        return;
    }
    static const char kHex[] = "0123456789abcdef";
    for (int word = 0; word < 4; ++word) {
        const uint32_t value = random ? random() : 0u;
        for (int nibble = 0; nibble < 8; ++nibble) {
            out[word * 8 + nibble] = kHex[(value >> (28 - nibble * 4)) & 0x0F];
        }
    }
    out[32] = '\0';
}

inline bool parentalBound(const ParentalRecord& record) {
    return (record.flags & kParentalBound) != 0;
}
inline bool parentalHasPin(const ParentalRecord& record) {
    return (record.flags & kParentalHasPin) != 0;
}
inline const char* parentalBindCode(const ParentalRecord& record) {
    return record.bind_code;
}

// Binds the device to a parent account. Fails when already bound or when the
// code does not match the one shown on the device. The caller persists the
// record; a failed save must leave the device unbound in RAM too.
inline bool parentalBind(ParentalRecord& record, RandomSource random, const char* code,
                         char* token_out, std::size_t token_capacity) {
    if (parentalBound(record)) return false;
    if (!code || std::strcmp(code, record.bind_code) != 0) return false;
    char token[40];
    parentalGenerateToken(random, token, sizeof(token));
    record.token_hash = parentalFnv1aText(token);
    record.flags |= kParentalBound;
    if (token_out && token_capacity) {
        std::strncpy(token_out, token, token_capacity - 1);
        token_out[token_capacity - 1] = '\0';
    }
    return true;
}

inline bool parentalVerifyToken(const ParentalRecord& record, const char* token) {
    if (!parentalBound(record) || !token || !token[0]) return false;
    return record.token_hash == parentalFnv1aText(token);
}

// PIN is salted-FNV, matching the legacy record. It is a local convenience
// gate, not a security boundary; the token remains the pairing secret.
inline bool parentalSetPin(ParentalRecord& record, RandomSource random, const char* pin) {
    if (!parentalPinFormatOk(pin)) return false;
    record.pin_salt = random ? random() : 0u;
    uint8_t buffer[4 + 8] = {};
    std::memcpy(buffer, &record.pin_salt, 4);
    const std::size_t length = std::strlen(pin);
    std::memcpy(buffer + 4, pin, length);
    record.pin_hash = parentalFnv1a(buffer, 4 + length);
    record.flags |= kParentalHasPin;
    return true;
}

// No PIN configured means "first entry allowed", matching the legacy app.
inline bool parentalVerifyPin(const ParentalRecord& record, const char* pin) {
    if (!parentalHasPin(record)) return true;
    if (!parentalPinFormatOk(pin)) return false;
    uint8_t buffer[4 + 8] = {};
    std::memcpy(buffer, &record.pin_salt, 4);
    const std::size_t length = std::strlen(pin);
    std::memcpy(buffer + 4, pin, length);
    return record.pin_hash == parentalFnv1a(buffer, 4 + length);
}

inline void parentalUnbind(ParentalRecord& record) {
    record.flags &= static_cast<uint8_t>(~kParentalBound);
    record.token_hash = 0;
}

// Restores permissive defaults but keeps the usage history and the PIN state,
// so a reset never turns a protected device into an unprotected one silently.
inline void parentalFactoryDefaults(ParentalRecord& record, RandomSource random) {
    record.flags &= static_cast<uint8_t>(kParentalHasPin | kParentalBound);
    record.daily_limit = 30;
    record.volume_min = 3;
    record.volume_max = 12;
    record.bedtime_start = 20 * 60 + 30;
    record.bedtime_end = 7 * 60;
    record.story_mask = ~uint64_t{0};
    record.category_mask = ~uint64_t{0};
    record.token_hash = 0;
    record.flags &= static_cast<uint8_t>(~kParentalBound);
    record.bind_code[0] = '\0';
    parentalEnsureBindCode(record, random);
}

inline bool parentalSetFlag(ParentalRecord& record, uint8_t flag, bool enabled) {
    if (enabled) record.flags |= flag;
    else record.flags &= static_cast<uint8_t>(~flag);
    return true;
}

}
