#include "../components/board/include/board_profile.h"
#include <cassert>
#include <limits>
int main() {
    Board::Address address{};
    assert(Board::encode(Board::legacy, 255, 2, address));
    assert(address.device == 0x50 && address.bytes[0] == 255 && address.contiguous == 1);
    assert(Board::encode(Board::legacy, 256, 1, address));
    assert(address.device == 0x51 && address.bytes[0] == 0 && address.length == 1);
    assert(Board::encode(Board::legacy, 2047, 1, address));
    assert(address.device == 0x57);
    assert(!Board::encode(Board::legacy, 2047, 2, address));
    assert(Board::encode(Board::rev20261028, 256, 1, address));
    assert(address.device == 0x51 && address.bytes[0] == 1 && address.bytes[1] == 0 && address.length == 2);
    assert(Board::encode(Board::rev20261028, 8191, 1, address));
    assert(!Board::encode(Board::rev20261028, 8192, 1, address));
    assert(!Board::encode(Board::legacy, 1, std::numeric_limits<std::size_t>::max(), address));
}
