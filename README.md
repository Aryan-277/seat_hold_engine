# Seat Hold Engine

In-memory, thread-safe hold/confirm/release engine for concert seats. C++17, no dependencies.

## Run
Needs g++ with std::mutex support (GCC 7+ with POSIX threads; on Windows use MSYS2 UCRT64).
    make test        # rules + model-based + concurrency stress (seeds are printed)
    make mutants     # three deliberately broken engines must all be caught by the tests
    make tsan        # stress test under ThreadSanitizer (Linux/Mac)
Without make:
    g++ -std=c++17 -O1 -pthread -Iinclude -Itests -o build/test_stress tests/test_stress.cpp src/seat_hold.cpp
    ./build/test_stress <seed> <rounds> <threads> <ops_per_thread>

Layout: include/ and src/ (engine), tests/, DECISIONS.md, BUGS_I_FEAR.md, AI_USAGE.md.
API (seathold::HoldEngine, clock injected): hold, hold_many (takes a ttl too), confirm, release, seat_status.

## What works
- R1-R6, with every rule conflict resolved in DECISIONS.md and pinned by tests/test_rules.cpp.
- One mutex, lazy expiry, atomic hold_many (validate everything, then mutate).
- Tests: per-decision examples; 900k random ops vs a naive reference model; multi-threaded stress that
  replays the engine's journal through the reference, checks no double sale / no confirmed
  expired-and-retaken hold, audits invariants after every op, and checks opposite-order hold_many
  does not deadlock. Seeds are printed.

## What doesn't / limits
- The seed fixes each thread's op stream, not the OS schedule, so a concurrency failure may need several runs to reproduce.
- Single lock: no parallelism across seats (sharded design described in DECISIONS.md).
- No persistence, no purging of finished holds, no auth on hold ids, no cap on the R3 grace period.
- Verification status: all tests, the mutation check and ThreadSanitizer were run on Linux when the code was written.
  I could not yet run them on my own Windows machine (old MinGW lacked std::mutex; newer compiler was still installing at the deadline).