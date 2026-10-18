#include "hall.h"
#include "config.h"

namespace Hall {

void begin() {
  pinMode(HALL_PIN, INPUT_PULLUP);   // 推挽输出会覆盖内部上拉
}

int level() { return digitalRead(HALL_PIN); }

bool detected() { return level() == LOW; }

}