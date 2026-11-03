#include "parental_binding.h"

#include <cassert>
#include <cstdio>
#include <cstring>

using Persistence::ParentalRecord;

namespace {

uint32_t s_counter = 12345;
uint32_t nextRandom() {
    s_counter = s_counter * 1103515245u + 12345u;
    return s_counter;
}

bool isHex32(const char* text) {
    if (!text || std::strlen(text) != 32) return false;
    for (int i = 0; i < 32; ++i) {
        const char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

ParentalRecord freshRecord() {
    ParentalRecord record{};
    std::memcpy(record.magic, "PCTL", 4);
    record.version = 1;
    record.daily_limit = 30;
    record.volume_min = 3;
    record.volume_max = 12;
    record.bedtime_start = 20 * 60 + 30;
    record.bedtime_end = 7 * 60;
    record.story_mask = ~uint64_t{0};
    record.category_mask = ~uint64_t{0};
    std::memcpy(record.bind_code, "123456", 7);
    record.checksum = Persistence::parentalChecksum(record);
    return record;
}

bool recordIsValid(ParentalRecord record) {
    record.checksum = Persistence::parentalChecksum(record);
    return Persistence::validParentalRecord(record);
}

}  // namespace

int main() {
    using namespace Persistence;

    ParentalRecord record = freshRecord();
    assert(recordIsValid(record));
    assert(!parentalBound(record) && !parentalHasPin(record));
    assert(parentalVerifyPin(record, ""));  // no PIN configured yet

    parentalEnsureBindCode(record, nextRandom);
    const char* code = parentalBindCode(record);
    assert(std::strlen(code) == 6);
    for (int i = 0; i < 6; ++i) assert(code[i] >= '0' && code[i] <= '9');

    // A wrong code must not bind, and a bound device refuses a second bind.
    char token[40] = {};
    assert(!parentalBind(record, nextRandom, "000000", token, sizeof(token)));
    assert(!parentalBound(record));
    assert(parentalBind(record, nextRandom, code, token, sizeof(token)));
    assert(parentalBound(record));
    assert(isHex32(token));
    assert(parentalVerifyToken(record, token));
    assert(!parentalVerifyToken(record, "0123456789abcdef0123456789abcdef"));
    assert(!parentalVerifyToken(record, ""));
    assert(!parentalBind(record, nextRandom, code, token, sizeof(token)));

    // PIN rules mirror the legacy app: 4..6 digits, salted FNV hash.
    assert(!parentalSetPin(record, nextRandom, "12"));
    assert(!parentalSetPin(record, nextRandom, "12a4"));
    assert(!parentalSetPin(record, nextRandom, "1234567"));
    assert(parentalSetPin(record, nextRandom, "1234"));
    assert(parentalHasPin(record));
    assert(parentalVerifyPin(record, "1234"));
    assert(!parentalVerifyPin(record, "4321"));
    assert(parentalSetPin(record, nextRandom, "987654"));
    assert(parentalVerifyPin(record, "987654"));
    assert(!parentalVerifyPin(record, "1234"));

    // A persisted record must stay valid after the mutations (the store
    // recomputes the checksum on save).
    assert(recordIsValid(record));

    // Unbind keeps the PIN but drops the token.
    const uint32_t pin_hash = record.pin_hash;
    parentalUnbind(record);
    assert(!parentalBound(record));
    assert(record.token_hash == 0);
    assert(record.pin_hash == pin_hash && parentalHasPin(record));
    assert(!parentalVerifyToken(record, token));

    // Re-bind after unbind uses the same displayed code.
    assert(parentalBind(record, nextRandom, parentalBindCode(record), token, sizeof(token)));

    // Factory defaults keep PIN/binding state but reset quotas and masks.
    record.daily_limit = 5;
    record.story_mask = 0;
    record.volume_min = 21;
    record.volume_max = 21;
    parentalFactoryDefaults(record, nextRandom);
    assert(!parentalBound(record));
    assert(parentalHasPin(record));
    assert(record.daily_limit == 30 && record.volume_min == 3 && record.volume_max == 12);
    assert(record.story_mask == ~uint64_t{0} && record.category_mask == ~uint64_t{0});
    assert(std::strlen(parentalBindCode(record)) == 6);
    assert(recordIsValid(record));

    // Flag helpers.
    assert(parentalSetFlag(record, kParentalBedtime, true));
    assert((record.flags & kParentalBedtime) != 0);
    assert(parentalSetFlag(record, kParentalBedtime, false));
    assert((record.flags & kParentalBedtime) == 0);

    std::puts("parental binding ok");
    return 0;
}
