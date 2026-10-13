// Concurrency stress test.
//  Phase 1: many short rounds; N threads hammer a tiny set of seats with random
//           hold / hold_many / confirm / release while a fake clock moves forward.
//           Afterwards the engine's linearized journal is checked three ways:
//             (a) replayed through the simple reference model -> identical results (linearizability),
//             (b) independent invariants: no double sale, no confirmed expired-and-retaken hold,
//             (c) final seat_status == reference.
//           Also, with paranoid mode the engine audits ITSELF after every op (user cap, one active
//           hold per seat, ...).
//  Phase 2: two users hold_many overlapping seats in OPPOSITE order -> must not deadlock (watchdog).
// Usage: test_stress [seed] [rounds] [threads] [ops_per_thread]
// NOTE: the seed fixes each thread's operation stream, NOT the OS thread schedule.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "fake_clock.hpp"
#include "reference.hpp"
#include "seat_hold.hpp"
using namespace seathold;

static std::mutex g_mu;
static std::string g_msg;
static std::atomic<bool> g_failed{false};
static void fail(const std::string& m) {
  std::lock_guard<std::mutex> g(g_mu);
  if (!g_failed.exchange(true)) g_msg = m;
}

struct Coverage {
  long ops = 0, hold_ok = 0, seat_held = 0, seat_sold = 0, too_many = 0, hold_lost = 0;
  long sales = 0, grace_sales = 0, many_ok = 0, many_fail = 0;
};

static std::string describe(size_t i, const Event& ev) {
  static const char* names[] = {"hold", "hold_many", "confirm", "release"};
  std::ostringstream o;
  o << "journal[" << i << "] " << names[(int)ev.op] << " now=" << ev.now << " user=" << ev.user << " seats=[";
  for (auto& s : ev.seats) o << s << " ";
  o << "] ttl=" << ev.ttl << " hold_id=" << ev.hold_id << " -> " << to_string(ev.error);
  return o.str();
}

static void analyze(const std::vector<Event>& j, HoldEngine& e, FakeClock& clk,
                    const std::vector<std::string>& seats, Coverage& cov) {
  struct H { std::string seat, user; Time exp; };
  std::unordered_map<HoldId, H> holds;
  std::unordered_map<std::string, HoldId> latest_on_seat, sold_by;
  std::unordered_set<HoldId> confirmed;
  Reference ref;
  Time prev = -1;
  for (size_t i = 0; i < j.size(); ++i) {
    const Event& ev = j[i];
    ++cov.ops;
    if (ev.now < prev) return fail("clock went backwards in journal: " + describe(i, ev));
    prev = ev.now;
    if (ev.op == Op::Hold || ev.op == Op::HoldMany) {
      auto r = ref.hold_many(ev.user, ev.seats, ev.ttl, ev.now);
      if (r.error != ev.error || r.hold_ids != ev.ids)
        return fail("engine disagrees with sequential reference: " + describe(i, ev));
      if (ev.error == Error::None) {
        (ev.op == Op::HoldMany ? cov.many_ok : cov.hold_ok)++;
        for (size_t k = 0; k < ev.seats.size(); ++k) {
          HoldId id = ev.ids[k];
          if (!holds.count(id)) { holds[id] = {ev.seats[k], ev.user, ev.now + ev.ttl}; latest_on_seat[ev.seats[k]] = id; }
        }
      } else {
        if (ev.op == Op::HoldMany) ++cov.many_fail;
        if (ev.error == Error::SeatHeld) ++cov.seat_held;
        if (ev.error == Error::SeatSold) ++cov.seat_sold;
        if (ev.error == Error::TooManyHolds) ++cov.too_many;
      }
    } else if (ev.op == Op::Confirm) {
      Error r = ref.confirm(ev.hold_id, ev.now);
      if (r != ev.error) return fail("engine disagrees with sequential reference: " + describe(i, ev));
      if (ev.error == Error::HoldLost) ++cov.hold_lost;
      if (ev.error == Error::None && !confirmed.count(ev.hold_id)) {
        confirmed.insert(ev.hold_id);
        auto it = holds.find(ev.hold_id);
        if (it == holds.end()) return fail("confirmed a hold that was never created: " + describe(i, ev));
        const H& h = it->second;
        if (sold_by.count(h.seat)) return fail("DOUBLE SALE of seat " + h.seat + ": " + describe(i, ev));
        sold_by[h.seat] = ev.hold_id;
        ++cov.sales;
        if (ev.now >= h.exp) {
          ++cov.grace_sales;
          if (latest_on_seat[h.seat] != ev.hold_id)
            return fail("confirmed an expired-and-retaken hold on " + h.seat + ": " + describe(i, ev));
        }
      }
    } else {
      Error r = ref.release(ev.hold_id, ev.now);
      if (r != ev.error) return fail("engine disagrees with sequential reference: " + describe(i, ev));
    }
  }
  for (auto& s : seats)
    if (!(e.seat_status(s) == ref.status(s, clk.now()))) return fail("final seat_status differs from reference for " + s);
}

static void run_round(uint64_t seed, int threads, int ops, Coverage& cov) {
  const std::vector<std::string> seats = {"S1", "S2", "S3", "S4", "S5", "S6"};
  const std::vector<std::string> users = {"u1", "u2", "u3", "u4"};
  FakeClock clk;
  HoldEngine e([&] { return clk.now(); });
  e.enable_journal(true);
  e.enable_paranoid_checks(true);
  std::atomic<HoldId> max_id{0};
  std::atomic<bool> go{false};

  auto worker = [&](int tid) {
    std::mt19937_64 rng(seed * 1000003ULL + tid);
    while (!go.load()) std::this_thread::yield();
    try {
      for (int i = 0; i < ops && !g_failed.load(); ++i) {
        int r = rng() % 100;
        const std::string& user = users[rng() % users.size()];
        auto note = [&](HoldId id) { HoldId c = max_id.load(); while (id > c && !max_id.compare_exchange_weak(c, id)) {} };
        if (r < 35) {
          auto h = e.hold(seats[rng() % seats.size()], user, 1 + rng() % 8);
          if (h.ok()) note(h.hold_id);
        } else if (r < 50) {
          std::vector<std::string> v;
          int n = 1 + rng() % 4;
          for (int k = 0; k < n; ++k) v.push_back(seats[rng() % seats.size()]);
          auto h = e.hold_many(user, v, 1 + rng() % 8);
          if (h.ok()) for (auto id : h.hold_ids) note(id);
        } else if (r < 62) {
          e.confirm(1 + rng() % (max_id.load() + 1));
        } else if (r < 80) {
          e.release(1 + rng() % (max_id.load() + 1));
        } else {
          clk.advance(rng() % 3);
        }
      }
    } catch (const std::exception& ex) {
      fail(std::string("worker threw: ") + ex.what() + " (round seed " + std::to_string(seed) + ")");
    }
  };
  std::vector<std::thread> ts;
  for (int t = 0; t < threads; ++t) ts.emplace_back(worker, t);
  go = true;
  for (auto& t : ts) t.join();
  if (g_failed.load()) return;
  try { e.check_invariants(); } catch (const std::exception& ex) { return fail(ex.what()); }
  analyze(e.journal(), e, clk, seats, cov);
  if (g_failed.load()) g_msg += " (round seed " + std::to_string(seed) + ")";
}

// Opposite-order overlapping hold_many; also checks atomicity (a failed hold_many leaves nothing behind).
static void deadlock_phase(int iters) {
  FakeClock clk;
  HoldEngine e([&] { return clk.now(); });
  e.enable_paranoid_checks(true);
  auto body = [&](const std::string& me, std::vector<std::string> order) {
    try {
      for (int i = 0; i < iters && !g_failed.load(); ++i) {
        auto r = e.hold_many(me, order, 1000);
        if (r.ok()) {
          for (auto id : r.hold_ids) if (e.release(id) != Error::None) fail("release failed");
        } else {
          for (auto& s : order) {
            auto st = e.seat_status(s);
            if (st.kind == SeatKind::Held && st.user == me) fail("PARTIAL hold_many left " + s + " held by " + me);
          }
        }
      }
    } catch (const std::exception& ex) { fail(ex.what()); }
  };
  auto fut = std::async(std::launch::async, [&] {
    std::thread t1(body, "x", std::vector<std::string>{"A", "B", "C"});
    std::thread t2(body, "y", std::vector<std::string>{"C", "B", "A"});
    std::thread t3(body, "z", std::vector<std::string>{"B", "A", "C"});
    t1.join(); t2.join(); t3.join();
  });
  if (fut.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
    std::printf("FAIL: DEADLOCK suspected (phase 2 did not finish in 60s)\n");
    std::_Exit(1);
  }
}

int main(int argc, char** argv) {
  uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10)
                           : (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
  int rounds = argc > 2 ? std::atoi(argv[2]) : 300;
  int threads = argc > 3 ? std::atoi(argv[3]) : 8;
  int ops = argc > 4 ? std::atoi(argv[4]) : 150;
  std::printf("test_stress seed=%llu rounds=%d threads=%d ops/thread=%d\n", (unsigned long long)seed, rounds, threads, ops);

  Coverage cov;
  for (int r = 0; r < rounds && !g_failed.load(); ++r) run_round(seed + r, threads, ops, cov);
  if (!g_failed.load()) deadlock_phase(20000);

  if (g_failed.load()) {
    std::printf("FAIL: %s\nreproduce (same op streams, schedule may differ): ./build/test_stress %llu %d %d %d\n",
                g_msg.c_str(), (unsigned long long)seed, rounds, threads, ops);
    return 1;
  }
  std::printf("OK. journal ops=%ld holds=%ld hold_many ok/fail=%ld/%ld SeatHeld=%ld SeatSold=%ld TooMany=%ld HoldLost=%ld sales=%ld (of which grace/expired=%ld)\n",
              cov.ops, cov.hold_ok, cov.many_ok, cov.many_fail, cov.seat_held, cov.seat_sold, cov.too_many, cov.hold_lost, cov.sales, cov.grace_sales);
  if (!cov.seat_held || !cov.too_many || !cov.hold_lost || !cov.grace_sales || !cov.many_fail) {
    std::printf("FAIL: vacuous run - a key branch was never exercised\n");
    return 1;
  }
  return 0;
}
