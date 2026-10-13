// Concurrency-safe seat hold engine. See DECISIONS.md for every rule interpretation.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace seathold {

using HoldId = std::uint64_t;            // 0 is never a valid id
using Time = std::int64_t;               // seconds on the injected clock
using ClockFn = std::function<Time()>;

enum class Error {
  None = 0,
  InvalidArgument,  // ttl <= 0, empty user/seat, empty seat list
  SeatHeld,         // another user holds it (live)
  SeatSold,
  TooManyHolds,     // would exceed 4 live holds (R2)
  UnknownHold,
  HoldReleased,     // confirm() on a hold that was explicitly released
  HoldLost          // hold was superseded by a later hold on the same seat
};
const char* to_string(Error e);

struct HoldResult {
  Error error;
  HoldId hold_id;
  bool ok() const { return error == Error::None; }
};
struct HoldManyResult {
  Error error;
  std::vector<HoldId> hold_ids;  // aligned with the input seat list
  bool ok() const { return error == Error::None; }
};

enum class SeatKind { Free, Held, Sold };
struct Status {
  SeatKind kind = SeatKind::Free;
  std::string user;      // Held / Sold
  Time expires_at = 0;   // Held only
  bool operator==(const Status& o) const {
    return kind == o.kind && user == o.user && expires_at == o.expires_at;
  }
};

// Journal of linearized operations (order == lock order). Test-only feature.
enum class Op { Hold, HoldMany, Confirm, Release };
struct Event {
  Op op = Op::Hold;
  Time now = 0;                    // the single clock reading used by this op
  std::string user;
  std::vector<std::string> seats;
  Time ttl = 0;
  HoldId hold_id = 0;              // Confirm / Release argument
  Error error = Error::None;
  std::vector<HoldId> ids;         // Hold / HoldMany result
};

class HoldEngine {
 public:
  static constexpr int kMaxHoldsPerUser = 4;

  explicit HoldEngine(ClockFn clock);

  HoldResult hold(const std::string& seat, const std::string& user, Time ttl_seconds);
  HoldManyResult hold_many(const std::string& user, const std::vector<std::string>& seats,
                           Time ttl_seconds);
  Error confirm(HoldId id);
  Error release(HoldId id);
  Status seat_status(const std::string& seat);

  // ---- test hooks ----
  void enable_journal(bool on);
  std::vector<Event> journal() const;
  void enable_paranoid_checks(bool on);  // verify invariants after every op (throws logic_error)
  void check_invariants();               // throws std::logic_error on violation

 private:
  enum class State { Active, Confirmed, Released, Superseded };
  struct Rec {
    HoldId id = 0;
    std::string seat, user;
    Time expires_at = 0;
    State state = State::Active;
  };
  struct SeatInfo {
    bool sold = false;
    std::string sold_to;
    HoldId active = 0;  // the single Active record for this seat (maybe expired), or 0
  };

  HoldManyResult hold_many_locked(const std::string& user, const std::vector<std::string>& seats,
                                  Time ttl, Time now);
  Error confirm_locked(HoldId id);
  Error release_locked(HoldId id);
  int count_live_locked(const std::string& user, Time now) const;
  void check_invariants_locked(Time now) const;
  void after_op(const Event& ev);

  mutable std::mutex mu_;  // THE lock: guards everything below
  ClockFn clock_;
  HoldId next_id_ = 1;
  std::unordered_map<HoldId, Rec> recs_;
  std::unordered_map<std::string, SeatInfo> seats_;
  std::unordered_map<std::string, std::unordered_set<HoldId>> user_active_;
  bool journal_on_ = false, paranoid_ = false;
  std::vector<Event> journal_;
};

}  // namespace seathold
