#include "seat_hold.hpp"

#include <stdexcept>

// Mutation switches used ONLY by scripts/mutants.sh to prove the tests can fail.
//   MUTANT_NO_CAP            : skip the 4-hold limit
//   MUTANT_NO_SUPERSEDE_MARK : taking an expired seat doesn't kill the old hold
//   MUTANT_STRICT_EXPIRY     : confirm() refuses expired holds (ignores the R3 grace)

namespace seathold {

const char* to_string(Error e) {
  switch (e) {
    case Error::None: return "OK";
    case Error::InvalidArgument: return "INVALID_ARGUMENT";
    case Error::SeatHeld: return "SEAT_HELD";
    case Error::SeatSold: return "SEAT_SOLD";
    case Error::TooManyHolds: return "TOO_MANY_HOLDS";
    case Error::UnknownHold: return "UNKNOWN_HOLD";
    case Error::HoldReleased: return "HOLD_RELEASED";
    case Error::HoldLost: return "HOLD_LOST";
  }
  return "?";
}

HoldEngine::HoldEngine(ClockFn clock) : clock_(std::move(clock)) {}

void HoldEngine::enable_journal(bool on) { std::lock_guard<std::mutex> g(mu_); journal_on_ = on; }
void HoldEngine::enable_paranoid_checks(bool on) { std::lock_guard<std::mutex> g(mu_); paranoid_ = on; }
std::vector<Event> HoldEngine::journal() const { std::lock_guard<std::mutex> g(mu_); return journal_; }
void HoldEngine::check_invariants() {
  std::lock_guard<std::mutex> g(mu_);
  check_invariants_locked(clock_());
}

void HoldEngine::after_op(const Event& ev) {
  if (journal_on_) journal_.push_back(ev);
  if (paranoid_) check_invariants_locked(ev.now);
}

// ---------------------------------------------------------------- public API
// Every public method: take the lock, read the clock ONCE, do everything with that `now`.

HoldResult HoldEngine::hold(const std::string& seat, const std::string& user, Time ttl) {
  std::lock_guard<std::mutex> g(mu_);
  Time now = clock_();
  std::vector<std::string> seats{seat};
  HoldManyResult r = hold_many_locked(user, seats, ttl, now);
  Event ev;
  ev.op = Op::Hold; ev.now = now; ev.user = user; ev.seats = seats; ev.ttl = ttl;
  ev.error = r.error; ev.ids = r.hold_ids;
  after_op(ev);
  return {r.error, r.ok() ? r.hold_ids[0] : 0};
}

HoldManyResult HoldEngine::hold_many(const std::string& user, const std::vector<std::string>& seats,
                                     Time ttl) {
  std::lock_guard<std::mutex> g(mu_);
  Time now = clock_();
  HoldManyResult r = hold_many_locked(user, seats, ttl, now);
  Event ev;
  ev.op = Op::HoldMany; ev.now = now; ev.user = user; ev.seats = seats; ev.ttl = ttl;
  ev.error = r.error; ev.ids = r.hold_ids;
  after_op(ev);
  return r;
}

Error HoldEngine::confirm(HoldId id) {
  std::lock_guard<std::mutex> g(mu_);
  Time now = clock_();
  Error e;
#ifdef MUTANT_STRICT_EXPIRY
  auto it = recs_.find(id);
  if (it != recs_.end() && it->second.state == State::Active && now >= it->second.expires_at)
    e = Error::HoldLost;
  else
    e = confirm_locked(id);
#else
  e = confirm_locked(id);
#endif
  Event ev;
  ev.op = Op::Confirm; ev.now = now; ev.hold_id = id; ev.error = e;
  after_op(ev);
  return e;
}

Error HoldEngine::release(HoldId id) {
  std::lock_guard<std::mutex> g(mu_);
  Time now = clock_();
  Error e = release_locked(id);
  Event ev;
  ev.op = Op::Release; ev.now = now; ev.hold_id = id; ev.error = e;
  after_op(ev);
  return e;
}

Status HoldEngine::seat_status(const std::string& seat) {
  std::lock_guard<std::mutex> g(mu_);
  Time now = clock_();
  Status s;
  auto it = seats_.find(seat);
  if (it == seats_.end()) return s;
  const SeatInfo& si = it->second;
  if (si.sold) { s.kind = SeatKind::Sold; s.user = si.sold_to; return s; }
  if (si.active != 0) {
    const Rec& r = recs_.at(si.active);
    if (now < r.expires_at) { s.kind = SeatKind::Held; s.user = r.user; s.expires_at = r.expires_at; }
  }
  return s;  // Free (including "expired but not yet taken")
}

// ------------------------------------------------------------ locked helpers

int HoldEngine::count_live_locked(const std::string& user, Time now) const {
  auto it = user_active_.find(user);
  if (it == user_active_.end()) return 0;
  int n = 0;
  for (HoldId id : it->second)
    if (recs_.at(id).expires_at > now) ++n;  // expired-but-untaken holds don't count (D4)
  return n;
}

// All-or-nothing: PHASE 1 validates everything without mutating; PHASE 2 mutates and cannot fail.
HoldManyResult HoldEngine::hold_many_locked(const std::string& user,
                                            const std::vector<std::string>& seats, Time ttl,
                                            Time now) {
  if (ttl <= 0 || user.empty() || seats.empty()) return {Error::InvalidArgument, {}};
  for (const auto& s : seats)
    if (s.empty()) return {Error::InvalidArgument, {}};

  std::vector<std::string> uniq;  // de-duplicated, order preserved
  {
    std::unordered_set<std::string> seen;
    for (const auto& s : seats)
      if (seen.insert(s).second) uniq.push_back(s);
  }

  // ---- PHASE 1: validate
  std::unordered_map<std::string, HoldId> own_live;  // seat -> my live hold (idempotent retry, R5)
  for (const auto& s : uniq) {
    auto it = seats_.find(s);
    if (it == seats_.end()) continue;
    const SeatInfo& si = it->second;
    if (si.sold) return {Error::SeatSold, {}};
    if (si.active == 0) continue;
    const Rec& r = recs_.at(si.active);
    if (now >= r.expires_at) continue;  // expired -> anyone (incl. me) may take it
    if (r.user != user) return {Error::SeatHeld, {}};
    own_live[s] = r.id;
  }
  int live = count_live_locked(user, now);
  int fresh = static_cast<int>(uniq.size() - own_live.size());
#ifndef MUTANT_NO_CAP
  if (live + fresh > kMaxHoldsPerUser) return {Error::TooManyHolds, {}};
#endif

  // ---- PHASE 2: mutate (no failure paths below)
  std::unordered_map<std::string, HoldId> idmap = own_live;
  for (const auto& s : uniq) {
    if (idmap.count(s)) continue;
    SeatInfo& si = seats_[s];
    if (si.active != 0) {
#ifndef MUTANT_NO_SUPERSEDE_MARK
      Rec& old = recs_.at(si.active);  // necessarily expired (phase 1)
      old.state = State::Superseded;   // permanently dead: can never be confirmed (D2)
      user_active_[old.user].erase(old.id);
#endif
    }
    HoldId id = next_id_++;
    Rec r;
    r.id = id; r.seat = s; r.user = user; r.expires_at = now + ttl; r.state = State::Active;
    recs_.emplace(id, std::move(r));
    si.active = id;
    user_active_[user].insert(id);
    idmap[s] = id;
  }
  std::vector<HoldId> ids;
  ids.reserve(seats.size());
  for (const auto& s : seats) ids.push_back(idmap.at(s));
  return {Error::None, std::move(ids)};
}

Error HoldEngine::confirm_locked(HoldId id) {
  auto it = recs_.find(id);
  if (it == recs_.end()) return Error::UnknownHold;
  Rec& r = it->second;
  switch (r.state) {
    case State::Confirmed: return Error::None;          // idempotent
    case State::Released: return Error::HoldReleased;
    case State::Superseded: return Error::HoldLost;
    case State::Active: break;
  }
  // Active (live OR expired-but-untaken): by construction nobody else has taken the seat (R3 grace).
  SeatInfo& si = seats_.at(r.seat);
  r.state = State::Confirmed;
  si.sold = true;
  si.sold_to = r.user;
  si.active = 0;
  user_active_[r.user].erase(id);
  return Error::None;
}

Error HoldEngine::release_locked(HoldId id) {
  auto it = recs_.find(id);
  if (it == recs_.end()) return Error::UnknownHold;
  Rec& r = it->second;
  switch (r.state) {
    case State::Released: return Error::None;           // idempotent
    case State::Confirmed: return Error::SeatSold;      // can't un-sell
    case State::Superseded: return Error::HoldLost;
    case State::Active: break;
  }
  r.state = State::Released;  // also kills the R3 grace for an expired hold
  seats_.at(r.seat).active = 0;
  user_active_[r.user].erase(id);
  return Error::None;
}

// Independent O(n) audit computed from the raw records (not from the indexes it checks).
void HoldEngine::check_invariants_locked(Time now) const {
  auto fail = [](const std::string& m) { throw std::logic_error("INVARIANT VIOLATION: " + m); };
  std::unordered_map<std::string, int> active_n, confirmed_n, live_n;
  size_t active_total = 0;
  for (const auto& kv : recs_) {
    const Rec& r = kv.second;
    if (r.state == State::Active) {
      ++active_n[r.seat]; ++active_total;
      if (r.expires_at > now) ++live_n[r.user];
    } else if (r.state == State::Confirmed) {
      ++confirmed_n[r.seat];
    }
  }
  for (const auto& kv : active_n) if (kv.second > 1) fail("seat has >1 Active holds: " + kv.first);
  for (const auto& kv : confirmed_n) if (kv.second > 1) fail("seat SOLD twice: " + kv.first);
  for (const auto& kv : live_n)
    if (kv.second > kMaxHoldsPerUser) fail("user over cap: " + kv.first);
  for (const auto& kv : seats_) {
    const SeatInfo& si = kv.second;
    bool has_conf = confirmed_n.count(kv.first) > 0;
    if (si.sold != has_conf) fail("sold flag disagrees with records: " + kv.first);
    if (si.sold && si.active != 0) fail("sold seat still has active hold: " + kv.first);
    bool has_active = active_n.count(kv.first) > 0;
    if ((si.active != 0) != has_active) fail("active pointer disagrees with records: " + kv.first);
    if (si.active != 0) {
      const Rec& r = recs_.at(si.active);
      if (r.state != State::Active || r.seat != kv.first) fail("bad active pointer: " + kv.first);
    }
  }
  size_t idx = 0;
  for (const auto& kv : user_active_)
    for (HoldId id : kv.second) {
      const Rec& r = recs_.at(id);
      if (r.state != State::Active || r.user != kv.first) fail("user index stale");
      ++idx;
    }
  if (idx != active_total) fail("user index size mismatch");
}

}  // namespace seathold
