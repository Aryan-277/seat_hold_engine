// Model-based randomized test: engine vs. the simple reference on random single-threaded sequences.
// Usage: test_model [seed] [sequences] [steps]
#include <chrono>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <sstream>
#include "fake_clock.hpp"
#include "reference.hpp"
#include "seat_hold.hpp"
using namespace seathold;

int main(int argc, char** argv) {
  uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10)
                           : (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
  int sequences = argc > 2 ? std::atoi(argv[2]) : 3000;
  int steps = argc > 3 ? std::atoi(argv[3]) : 300;
  std::printf("test_model seed=%llu sequences=%d steps=%d\n", (unsigned long long)seed, sequences, steps);

  const std::vector<std::string> seats = {"A", "B", "C", "D", "E", "F"};
  const std::vector<std::string> users = {"u1", "u2", "u3"};
  long ops = 0, lost = 0, toomany = 0, held = 0;

  for (int sq = 0; sq < sequences; ++sq) {
    std::mt19937_64 rng(seed + sq);
    FakeClock clk;
    HoldEngine e([&] { return clk.now(); });
    e.enable_paranoid_checks(true);
    Reference ref;
    std::map<HoldId, Time> exp_of;  // for coverage only

    for (int step = 0; step < steps; ++step) {
      Time now = clk.now();
      int r = rng() % 100;
      std::ostringstream desc;
      bool bad = false;
      if (r < 30) {
        std::string s = seats[rng() % seats.size()], u = users[rng() % users.size()];
        Time ttl = 1 + rng() % 5;
        desc << "hold(" << s << "," << u << "," << ttl << ")";
        auto a = e.hold(s, u, ttl);
        auto b = ref.hold_many(u, {s}, ttl, now);
        bad = a.error != b.error || (a.ok() && a.hold_id != b.hold_ids[0]);
        if (a.error == Error::SeatHeld) ++held;
        if (a.error == Error::TooManyHolds) ++toomany;
      } else if (r < 50) {
        std::string u = users[rng() % users.size()];
        int n = 1 + rng() % 5;
        std::vector<std::string> v;
        for (int i = 0; i < n; ++i) v.push_back(seats[rng() % seats.size()]);
        Time ttl = 1 + rng() % 5;
        desc << "hold_many(" << u << ",[";
        for (auto& x : v) desc << x << " ";
        desc << "]," << ttl << ")";
        auto a = e.hold_many(u, v, ttl);
        auto b = ref.hold_many(u, v, ttl, now);
        bad = a.error != b.error || a.hold_ids != b.hold_ids;
        if (a.error == Error::TooManyHolds) ++toomany;
      } else if (r < 65) {
        HoldId id = rng() % (ref.max_id() + 3);
        desc << "confirm(" << id << ")";
        auto a = e.confirm(id);
        auto b = ref.confirm(id, now);
        bad = a != b;
        if (a == Error::HoldLost) ++lost;
      } else if (r < 80) {
        HoldId id = rng() % (ref.max_id() + 3);
        desc << "release(" << id << ")";
        bad = e.release(id) != ref.release(id, now);
      } else {
        Time d = rng() % 5;  // 0 is allowed: zero-advance boundary
        desc << "advance(" << d << ")";
        clk.advance(d);
      }
      ++ops;
      // Compare the observable state of every seat after every operation.
      Time t2 = clk.now();
      for (auto& s : seats)
        if (!(e.seat_status(s) == ref.status(s, t2))) {
          bad = true;
          desc << " [seat_status(" << s << ") differs]";
        }
      if (bad) {
        std::printf("MISMATCH: seed=%llu sequence=%d step=%d op=%s now=%lld\n",
                    (unsigned long long)seed, sq, step, desc.str().c_str(), (long long)now);
        std::printf("reproduce: ./build/test_model %llu %d %d\n", (unsigned long long)seed, sequences, steps);
        return 1;
      }
    }
  }
  std::printf("OK: %ld ops matched. coverage: SeatHeld=%ld TooManyHolds=%ld HoldLost=%ld\n", ops, held, toomany, lost);
  if (!held || !toomany || !lost) { std::printf("WARNING: vacuous run (some branch never hit)\n"); return 1; }
  return 0;
}
