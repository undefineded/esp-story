#include "../src/transfer_crc.h"
#include <cassert>
int main() {
  const uint8_t data[] = "123456789";
  assert(transferCrc32(data, 9) == 0xcbf43926u);
  assert(transferCrc32(data, 0) == 0);
}
