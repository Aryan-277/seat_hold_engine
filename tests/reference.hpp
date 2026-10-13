#pragma once
// Deliberately SIMPLE single-threaded reference model of the spec in DECISIONS.md.
// One flat vector of hold records, no indexes, no caching, linear scans everywhere,
// time passed in explicitly. Written differently from the engine on purpose.
#include <algorithm>
#include <string>
#include <vector>
#include "seat_hold.hpp"

class Reference {
 public:
  using HoldId = seathold::HoldId;
  using Time = seathold::Time;
  using Error = seathold::Error;

  seathold::HoldManyResult hold_many(const std::string& user, const std::vector<std::string>& seats,
                                     Time ttl, Time now) {
    if (ttl <= 0 || user.empty() || seats.empty()) return {Error::InvalidArgument, {}};
    for (auto& s : seats) if (s.empty()) return {Error::InvalidArgument, {}};
    std::vector<std::string> uniq;
    for (auto& s : seats) if (std::find(uniq.begin(), uniq.end(), s) == uniq.end()) uniq.push_back(s);

    for (auto& s : uniq) {
      if (sold(s)) return {Error::SeatSold, {}};
      int a = active(s);
      if (a >= 0 && recs_[a].exp > now && recs_[a].user != user) return {Error::SeatHeld, {}};
    }
    int live = 0;
    for (auto& r : recs_) if (r.state == 0 && r.user == user && r.exp > now) ++live;
    int fresh = 0;
    for (auto& s : uniq) {
      int a = active(s);
      if (!(a >= 0 && recs_[a].exp > now && recs_[a].user == user)) ++fresh;
    }
    if (live + fresh > 4) return {Error::TooManyHolds, {}};

    std::vector<HoldId> ids;
    for (auto& s : seats) {  // duplicates naturally hit the hold created for their first occurrence
      int a = active(s);
      if (a >= 0 && recs_[a].exp > now && recs_[a].user == user) { ids.push_back(recs_[a].id); continue; }
      if (a >= 0) recs_[a].state = 3;  // superseded
      recs_.push_back(Rec{next_, s, user, now + ttl, 0});
      ids.push_back(next_++);
    }
    return {Error::None, ids};
  }

  Error confirm(HoldId id, Time) {
    int i = find(id);
    if (i < 0) return Error::UnknownHold;
    switch (recs_[i].state) {
      case 1: return Error::None;
      case 2: return Error::HoldReleased;
      case 3: return Error::HoldLost;
    }
    recs_[i].state = 1;
    return Error::None;
  }

  Error release(HoldId id, Time) {
    int i = find(id);
    if (i < 0) return Error::UnknownHold;
    switch (recs_[i].state) {
      case 1: return Error::SeatSold;
      case 2: return Error::None;
      case 3: return Error::HoldLost;
    }
    recs_[i].state = 2;
    return Error::None;
  }

  seathold::Status status(const std::string& seat, Time now) const {
    seathold::Status st;
    for (auto& r : recs_) if (r.seat == seat && r.state == 1) {
      st.kind = seathold::SeatKind::Sold; st.user = r.user; return st;
    }
    for (auto& r : recs_) if (r.seat == seat && r.state == 0 && r.exp > now) {
      st.kind = seathold::SeatKind::Held; st.user = r.user; st.expires_at = r.exp; return st;
    }
    return st;
  }

  HoldId max_id() const { return next_ - 1; }

 private:
  struct Rec { HoldId id; std::string seat, user; Time exp; int state; };  // 0 active 1 confirmed 2 released 3 superseded
  std::vector<Rec> recs_;
  HoldId next_ = 1;

  int find(HoldId id) const {
    for (size_t i = 0; i < recs_.size(); ++i) if (recs_[i].id == id) return (int)i;
    return -1;
  }
  bool sold(const std::string& s) const {
    for (auto& r : recs_) if (r.seat == s && r.state == 1) return true;
    return false;
  }
  int active(const std::string& s) const {
    for (size_t i = 0; i < recs_.size(); ++i) if (recs_[i].seat == s && recs_[i].state == 0) return (int)i;
    return -1;
  }
};
