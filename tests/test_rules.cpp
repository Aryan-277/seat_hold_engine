// Example-based tests that pin down every decision in DECISIONS.md.
#include <cstdio>
#include <functional>
#include <vector>
#include "fake_clock.hpp"
#include "seat_hold.hpp"
using namespace seathold;

static int g_total = 0, g_fail = 0;
#define CHECK(c) do { ++g_total; if (!(c)) { ++g_fail; std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Fx {
  FakeClock clk;
  HoldEngine e{[this] { return clk.now(); }};
};

static void r1_single_owner_and_single_sale() {
  Fx f;
  auto a = f.e.hold("S1", "alice", 10);
  CHECK(a.ok());
  CHECK(f.e.hold("S1", "bob", 10).error == Error::SeatHeld);
  CHECK(f.e.confirm(a.hold_id) == Error::None);
  CHECK(f.e.hold("S1", "bob", 10).error == Error::SeatSold);
  auto st = f.e.seat_status("S1");
  CHECK(st.kind == SeatKind::Sold && st.user == "alice");
}

static void r2_cap_of_four() {
  Fx f;
  std::vector<HoldId> ids;
  for (int i = 1; i <= 4; ++i) { auto r = f.e.hold("S" + std::to_string(i), "u", 10); CHECK(r.ok()); ids.push_back(r.hold_id); }
  CHECK(f.e.hold("S5", "u", 10).error == Error::TooManyHolds);
  CHECK(f.e.release(ids[0]) == Error::None);
  CHECK(f.e.hold("S5", "u", 10).ok());
  f.clk.advance(10);  // all four remaining holds expire (now == expires_at => expired)
  for (int i = 6; i <= 9; ++i) CHECK(f.e.hold("S" + std::to_string(i), "u", 10).ok());  // expired holds don't count
  CHECK(f.e.hold("S10", "u", 10).error == Error::TooManyHolds);
}

static void r2_sold_seats_do_not_count() {
  Fx f;
  for (int i = 1; i <= 4; ++i) { auto r = f.e.hold("S" + std::to_string(i), "u", 100); CHECK(f.e.confirm(r.hold_id) == Error::None); }
  for (int i = 5; i <= 8; ++i) CHECK(f.e.hold("S" + std::to_string(i), "u", 100).ok());
}

static void r3_expired_but_untaken_is_confirmable() {
  Fx f;
  auto a = f.e.hold("S1", "alice", 5);
  f.clk.advance(5);  // boundary: expired exactly at expires_at
  CHECK(f.e.seat_status("S1").kind == SeatKind::Free);  // visible as FREE (D1)
  CHECK(f.e.confirm(a.hold_id) == Error::None);         // ...but alice can still pay
  auto st = f.e.seat_status("S1");
  CHECK(st.kind == SeatKind::Sold && st.user == "alice");
}

static void r3_expired_and_retaken_is_lost_forever() {
  Fx f;
  auto a = f.e.hold("S1", "alice", 5);
  f.clk.advance(6);
  auto b = f.e.hold("S1", "bob", 5);
  CHECK(b.ok());
  CHECK(f.e.confirm(a.hold_id) == Error::HoldLost);
  CHECK(f.e.release(b.hold_id) == Error::None);       // bob gives it up -> seat is free again
  CHECK(f.e.seat_status("S1").kind == SeatKind::Free);
  CHECK(f.e.confirm(a.hold_id) == Error::HoldLost);   // alice does NOT get resurrected (D2)
  auto c = f.e.hold("S1", "carol", 5);
  CHECK(f.e.confirm(c.hold_id) == Error::None);
  CHECK(f.e.confirm(a.hold_id) == Error::HoldLost);
  CHECK(f.e.seat_status("S1").user == "carol");
}

static void r3_release_kills_grace() {
  Fx f;
  auto a = f.e.hold("S1", "alice", 5);
  f.clk.advance(6);
  CHECK(f.e.release(a.hold_id) == Error::None);
  CHECK(f.e.confirm(a.hold_id) == Error::HoldReleased);
}

static void r4_never_extended() {
  Fx f;
  auto a = f.e.hold("S1", "alice", 5);   // expires at 5
  f.clk.advance(3);
  auto a2 = f.e.hold("S1", "alice", 100);  // retry with a bigger ttl
  CHECK(a2.ok() && a2.hold_id == a.hold_id);
  auto st = f.e.seat_status("S1");
  CHECK(st.kind == SeatKind::Held && st.expires_at == 5);  // unchanged
  f.clk.advance(2);
  CHECK(f.e.seat_status("S1").kind == SeatKind::Free);
}

static void r5_idempotent_retry() {
  Fx f;
  auto a = f.e.hold("S1", "alice", 10);
  auto again = f.e.hold("S1", "alice", 10);
  CHECK(again.ok() && again.hold_id == a.hold_id);
  CHECK(f.e.hold("S1", "bob", 10).error == Error::SeatHeld);
  for (int i = 2; i <= 4; ++i) CHECK(f.e.hold("S" + std::to_string(i), "alice", 10).ok());
  CHECK(f.e.hold("S1", "alice", 10).ok());  // retry at the cap still succeeds, not counted twice
  CHECK(f.e.hold("S5", "alice", 10).error == Error::TooManyHolds);
}

static void r3_r5_retry_after_expiry_is_a_new_hold() {
  Fx f;
  auto a = f.e.hold("S1", "alice", 5);
  f.clk.advance(5);
  auto b = f.e.hold("S1", "alice", 5);
  CHECK(b.ok() && b.hold_id != a.hold_id);
  CHECK(f.e.confirm(a.hold_id) == Error::HoldLost);
  CHECK(f.e.confirm(b.hold_id) == Error::None);
}

static void r6_all_or_nothing() {
  Fx f;
  CHECK(f.e.hold("S2", "bob", 10).ok());
  auto r = f.e.hold_many("alice", {"S1", "S2", "S3"}, 10);
  CHECK(r.error == Error::SeatHeld);
  CHECK(f.e.seat_status("S1").kind == SeatKind::Free);
  CHECK(f.e.seat_status("S3").kind == SeatKind::Free);
  auto ok = f.e.hold_many("alice", {"S1", "S3"}, 10);
  CHECK(ok.ok() && ok.hold_ids.size() == 2);
}

static void r2_r6_cap_counts_net_new_seats() {
  Fx f;
  auto a = f.e.hold_many("alice", {"A", "B", "C"}, 10);
  CHECK(a.ok());
  auto bad = f.e.hold_many("alice", {"C", "D", "E"}, 10);  // 3 live + 2 new = 5
  CHECK(bad.error == Error::TooManyHolds);
  CHECK(f.e.seat_status("D").kind == SeatKind::Free && f.e.seat_status("E").kind == SeatKind::Free);
  auto good = f.e.hold_many("alice", {"C", "D"}, 10);      // C is an idempotent retry; D is new => 4
  CHECK(good.ok() && good.hold_ids[0] == a.hold_ids[2]);
  CHECK(f.e.hold("F", "alice", 10).error == Error::TooManyHolds);
  CHECK(f.e.hold_many("zed", {"P", "Q", "R", "S", "T"}, 10).error == Error::TooManyHolds);  // >4 distinct
  auto dup = f.e.hold_many("bob", {"X", "X"}, 10);
  CHECK(dup.ok() && dup.hold_ids[0] == dup.hold_ids[1]);
  CHECK(f.e.hold_many("bob", {"Y1", "Y2", "Y3"}, 10).ok());  // dup counted once => 4 total
}

static void r6_takes_over_expired_seats() {
  Fx f;
  auto a = f.e.hold("S1", "bob", 5);
  f.clk.advance(5);
  CHECK(f.e.hold_many("alice", {"S1", "S2"}, 5).ok());
  CHECK(f.e.confirm(a.hold_id) == Error::HoldLost);
}

static void confirm_release_state_machine() {
  Fx f;
  auto a = f.e.hold("S1", "alice", 10);
  CHECK(f.e.release(a.hold_id) == Error::None);
  CHECK(f.e.release(a.hold_id) == Error::None);  // idempotent
  CHECK(f.e.confirm(a.hold_id) == Error::HoldReleased);
  CHECK(f.e.hold("S1", "bob", 10).ok());
  auto c = f.e.hold("S2", "alice", 10);
  CHECK(f.e.confirm(c.hold_id) == Error::None);
  CHECK(f.e.confirm(c.hold_id) == Error::None);  // idempotent
  CHECK(f.e.release(c.hold_id) == Error::SeatSold);
  CHECK(f.e.confirm(9999) == Error::UnknownHold);
  CHECK(f.e.release(9999) == Error::UnknownHold);
}

static void invalid_arguments() {
  Fx f;
  CHECK(f.e.hold("S1", "alice", 0).error == Error::InvalidArgument);
  CHECK(f.e.hold("S1", "alice", -3).error == Error::InvalidArgument);
  CHECK(f.e.hold("S1", "", 5).error == Error::InvalidArgument);
  CHECK(f.e.hold("", "alice", 5).error == Error::InvalidArgument);
  CHECK(f.e.hold_many("alice", {}, 5).error == Error::InvalidArgument);
  CHECK(f.e.seat_status("never-seen").kind == SeatKind::Free);
}

int main() {
  struct T { const char* name; void (*fn)(); } tests[] = {
      {"R1 single owner / single sale", r1_single_owner_and_single_sale},
      {"R2 cap of four", r2_cap_of_four},
      {"R2 sold seats don't count", r2_sold_seats_do_not_count},
      {"R3 expired+untaken is confirmable", r3_expired_but_untaken_is_confirmable},
      {"R3 expired+retaken is lost forever", r3_expired_and_retaken_is_lost_forever},
      {"R3 release kills grace", r3_release_kills_grace},
      {"R4 never extended", r4_never_extended},
      {"R5 idempotent retry", r5_idempotent_retry},
      {"R3/R5 retry after expiry = new hold", r3_r5_retry_after_expiry_is_a_new_hold},
      {"R6 all-or-nothing", r6_all_or_nothing},
      {"R2/R6 cap counts net-new seats", r2_r6_cap_counts_net_new_seats},
      {"R6 takes over expired seats", r6_takes_over_expired_seats},
      {"confirm/release state machine", confirm_release_state_machine},
      {"invalid arguments", invalid_arguments}};
  for (auto& t : tests) {
    int before = g_fail;
    t.fn();
    std::printf("[%s] %s\n", g_fail == before ? " ok " : "FAIL", t.name);
  }
  std::printf("%d checks, %d failed\n", g_total, g_fail);
  return g_fail ? 1 : 0;
}
