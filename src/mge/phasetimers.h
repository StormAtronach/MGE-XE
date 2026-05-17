#pragma once

// Lightweight phase-timing instrumentation. Reports gated by the
// "Log Distant Pipeline" mge.ini flag — off by default, so the
// scoped timers accumulate but no per-frame log output is produced
// unless the user opts into pipeline logging.
//
// Usage: at the top of any scope you want to measure, write
//     MGE_SCOPED_TIMER("cullDistantStatics");
// and the RAII destructor will add the elapsed microseconds into a
// named accumulator at scope exit. MGEPhaseTimers::report() flushes a
// one-line-per-bucket summary into mgeXE.log and resets the counters;
// it's called from the per-frame diagnostic block in cullDistantStatics
// when LogDistantPipeline is enabled.
//
// Everything lives in two files and is grep-visible via the
// MGE_SCOPED_TIMER / MGEPhaseTimers tokens, so the instrumentation
// can be removed cleanly if it's ever no longer needed.
//
// ---- Naming convention (read this before adding a new timer) ----
// Use `parent:child[:grandchild...]` to reflect static call nesting,
// so the sorted log block reads as a hierarchy at a glance.
//
//   MGE_SCOPED_TIMER("renderDepth");                    // umbrella
//     MGE_SCOPED_TIMER("renderDepth:statics");          // sub-phase
//       MGE_SCOPED_TIMER("renderDepth:statics:foo");    // sub-sub
//
// If a function that ALWAYS runs inside another timed scope has its
// own timer, name it using the parent's prefix — not its function
// name — so the log shows the relationship. Example: applyMSOC...
// is called only from cullDistantStatics:finish, so its timer is
// "cullDistantStatics:apply", not "applyMSOCToDistantStatics".
//
// ---- Constraints ----
//
//   1. MAIN THREAD ONLY. g_buckets is not synchronized. If you ever
//      add a timer to a worker-thread scope (IPC server, async
//      scenegraph walk, MSOC worker — none today), wrap g_buckets
//      access in a mutex first or use thread-local accumulators.
//
//   2. NAME MUST BE A STRING LITERAL. The bucket map is keyed by
//      `const char*` pointer identity (assumes linker literal pool).
//      Passing std::string::c_str() or any dynamically-built buffer
//      will silently corrupt buckets.
//
//   3. NO RECURSION. If a timer's scope re-enters itself (direct or
//      via mutual recursion), elapsed times double-count.

#include <chrono>
#include <cstdint>

namespace MGEPhaseTimers {
    // Add `us` microseconds to the bucket identified by `name`. The
    // pointer is stored verbatim — it must point to string-literal or
    // otherwise-stable storage that outlives the next `report()` call.
    void add(const char* name, std::uint64_t us);

    // Emit one line per bucket (sorted by total descending) into the
    // MGE-XE log, then zero all buckets for the next sampling window.
    // Safe to call even when no buckets have recorded yet (no-op).
    void report();
}

// RAII scope timer. Uses steady_clock (monotonic, high-res on Win32).
struct MGEScopedTimer {
    const char* name;
    std::chrono::steady_clock::time_point start;

    explicit MGEScopedTimer(const char* n)
        : name(n), start(std::chrono::steady_clock::now()) {}

    ~MGEScopedTimer() {
        using namespace std::chrono;
        const auto us = duration_cast<microseconds>(steady_clock::now() - start).count();
        MGEPhaseTimers::add(name, static_cast<std::uint64_t>(us));
    }

    // Non-copyable / non-movable — lifetime is strictly lexical scope.
    MGEScopedTimer(const MGEScopedTimer&) = delete;
    MGEScopedTimer& operator=(const MGEScopedTimer&) = delete;
};

#define MGE_TIMER_CONCAT_INNER(a, b) a##b
#define MGE_TIMER_CONCAT(a, b) MGE_TIMER_CONCAT_INNER(a, b)
// __COUNTER__ (MSVC + GCC + clang) gives a monotonically-increasing
// unique integer per macro expansion, so two timers on the same line
// (possible via macro expansion or one-liner constructs) don't collide
// on the variable name like they would with __LINE__.
#define MGE_SCOPED_TIMER(name) MGEScopedTimer MGE_TIMER_CONCAT(_mgePhaseTimer_, __COUNTER__)(name)
