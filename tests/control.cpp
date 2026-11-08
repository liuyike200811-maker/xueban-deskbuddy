#include "../firmware/DeskBuddy/Control.h"
#include <assert.h>

int main() {
  Timer timer;
  assert(!timer.start(Mode::Focus, 0, 0));
  assert(!timer.start(Mode::Rest, 7201, 0));
  assert(timer.start(Mode::Focus, 2, 0xFFFFFF00));
  assert(timer.remaining(0xFFFFFF00) == 2);
  assert(timer.tick(1743) == Mode::Idle);
  assert(timer.tick(1744) == Mode::Focus);
  assert(timer.tick(1745) == Mode::Idle);
  assert(timer.start(Mode::Rest, 10, 10000));
  assert(timer.remaining(11000) == 9);
  timer.stop();
  assert(timer.tick(20000) == Mode::Idle);

  Touch single;
  assert(single.update(true, 100) == Gesture::None);
  assert(single.update(false, 106) == Gesture::None); // Bounce.
  assert(single.update(true, 140) == Gesture::None);
  assert(single.update(true, 170) == Gesture::None);
  assert(single.update(false, 220) == Gesture::None);
  assert(single.update(false, 250) == Gesture::None);
  assert(single.update(false, 699) == Gesture::None);
  assert(single.update(false, 700) == Gesture::Single);

  Touch twice;
  twice.update(true, 100); twice.update(true, 130);
  twice.update(false, 170); twice.update(false, 200);
  twice.update(true, 600); twice.update(true, 615);
  twice.update(false, 650);
  assert(twice.update(false, 665) == Gesture::Double);
  assert(twice.update(false, 1000) == Gesture::None);

  Touch hold;
  hold.update(true, 100); hold.update(true, 130);
  assert(hold.update(true, 929) == Gesture::None);
  assert(hold.update(true, 930) == Gesture::Long);
  assert(hold.update(true, 1500) == Gesture::None);
  hold.update(false, 1550); hold.update(false, 1580);
  assert(hold.update(false, 2500) == Gesture::None);
}
