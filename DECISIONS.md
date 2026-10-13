# DECISIONS.md

Written **before** the engine was coded; the tests in `tests/test_rules.cpp` pin every item below.
Time is an integer number of seconds on an injected clock. A hold with `expires_at = T` is
**expired when `now >= T`** (boundary included).

## The rule conflicts

### D1. R3 vs R4: "an expired hold can still be confirmed if nobody took the seat" vs "holds are never extended"
These collide if "still confirmable" is read as "still held". **Decision:** confirming an expired hold is
*not* an extension. `expires_at` never changes and `seat_status` reports the seat as **FREE** from the
moment of expiry, so anyone may take it immediately. The old hold is simply *not yet dead*: it stays
confirmable until something else happens to the seat. So the grace is "you haven't lost it yet", not "you
still hold it".

### D2. What does "taken" mean in R3? (resurrection problem)
Alice's hold expires, Bob holds the seat, Bob releases it. The seat is free again: may Alice's old hold come back?
**Decision: no.** Any later hold on the seat, by anyone, **permanently supersedes** the old one (`HOLD_LOST`).
Rationale: "nobody else has taken the seat in the meantime" is about history, not current state. Allowing
resurrection makes the outcome depend on whether Bob's hold is still around, and lets a payment page
that confirms late flip a seat that someone else has already seen and acted on.
An **explicit `release()`** also kills the grace (`HOLD_RELEASED`): the user said "I don't want it".

### D3. R5 (idempotent retry) vs R3/R4 (expiry, no extension)
- Same user re-holds a seat with a **live** hold: returns the **same hold_id**; the new `ttl` is ignored and
  `expires_at` is unchanged (R4). Not counted twice toward R2.
- Same user re-holds a seat whose hold has **expired**: this is *not* a retry of a live hold. Returning the old (dead, status-ambiguous) id
  would either lie or silently extend. **Decision:** it is a **new hold** (new id, fresh TTL) and the old hold becomes
  superseded (D2). Consequence: confirm(old_id) now fails. Known loophole: a user can re-hold after each expiry and
  effectively camp on a seat; R4 only forbids extending *a hold*. A cooldown would fix it; not specified, so not built (see Open questions).

### D4. R2 (max 4 holds) vs R3 grace vs R6
- R2 counts **live** holds (`now < expires_at`) only. Expired-but-untaken holds don't count; otherwise a user whose
  holds lapsed would be blocked by seats the system already shows as FREE. **SOLD seats don't count** (they are
  purchases, not holds).
- Consequence: a user with 4 live holds can still confirm an expired one (it becomes SOLD, not a hold), so
  "4 holds + 1 late purchase" is possible. This is the direct price of R3.

### D5. R2 vs R6 (`hold_many`)
- The cap check is on the **net-new** seats: `live_holds + seats_not_already_live_held_by_me <= 4`.
  Seats I already hold live are idempotent no-ops (R5) and are not double counted.
- Duplicate seats in the list are de-duplicated (counted once, same id returned for each occurrence).
- Anything that fails (seat sold, held by another user, cap) leaves **no** side effects (validate-then-mutate under one lock).
- All holds in one call use the same TTL. More than 4 distinct seats can never succeed.
- Failure precedence is deterministic: invalid args > first unavailable seat in list order (SOLD / HELD) > cap.

## Other decisions
- **D6. Lazy expiry.** No timers/threads. Expiry is computed from the clock on each operation.
- **D7. Clock read once per operation**, inside the lock, and used for the entire operation (no torn view of time inside one call).
- **D8. confirm/release semantics** (idempotent where retries are expected, per the "flaky network" remark):
  | state of hold | confirm | release |
  |---|---|---|
  | active (live or expired) | OK -> SOLD | OK (grace gone) |
  | confirmed | OK (idempotent) | `SEAT_SOLD` |
  | released | `HOLD_RELEASED` | OK (idempotent) |
  | superseded | `HOLD_LOST` | `HOLD_LOST` |
  | unknown id | `UNKNOWN_HOLD` | `UNKNOWN_HOLD` |
- **D9. Hold ids are capabilities.** confirm/release take only a hold_id, per the API; no user authentication in scope.
- **D10. Seats are free-form strings**; a seat that was never mentioned is FREE, there is no seat catalogue.
- **D11. Invalid input** (ttl <= 0, empty user, empty seat, empty list) -> `INVALID_ARGUMENT`.
- **D12. Memory is never reclaimed** (records of finished holds are kept). Fine for the assignment; production would compact.

## Locking strategy (what I can defend)
**One mutex guards all state.** Every public call: lock -> read clock -> validate -> mutate -> unlock.
- R1, R2 and R6 all need *cross-seat* state (a user's total, several seats at once). With one lock these are trivially atomic.
- **No deadlock is possible**: there is exactly one lock, so there is no lock order to get wrong when two users request
  overlapping seats in opposite order (tested explicitly).
- Cost: no parallelism between unrelated seats. Each op is a few hash lookups (microseconds) so contention is not the bottleneck
  at this scale. If it were: shard by seat, always acquire the shard locks in **sorted seat order** (and a per-user lock first for R2).
  I chose not to, since a sharded design has far more places to be subtly wrong in 60 minutes.

## Open questions I'd ask the product owner
1. Should re-holding after expiry be rate-limited (camping loophole in D3)?
2. Should the grace in R3 have an upper bound (e.g. 10 minutes)? Otherwise a payment arriving a day late can succeed if the seat was never touched.
3. Should the "4 holds" limit include a seat the user can still late-confirm (D4)?
