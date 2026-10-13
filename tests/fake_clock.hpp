#pragma once
#include <atomic>
#include "seat_hold.hpp"

// Injectable clock. Tests never sleep; they call advance().
struct FakeClock {
  std::atomic<seathold::Time> t{0};
  seathold::Time now() const { return t.load(); }
  void advance(seathold::Time d) { t.fetch_add(d); }
  void set(seathold::Time v) { t.store(v); }
};
