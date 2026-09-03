// ============================================================================
// ProsperoLayer RDNA2 Core - HLE libKernel Integration Test
// ============================================================================
// Item #3 (HLE: libKernel). The existing syscall_dispatcher_test covers the
// register-only syscall surface. This suite instead drives the *shipped*
// libKernel C++ primitive implementations directly through their guest-facing
// Kyty-compatible entry points -- the exact functions LIB_FUNC registers -- and
// asserts behavioural faithfulness, not just symbol resolvability:
//
//   * Semaphore (Semaphore::KernelCreateSema/Wait/Signal/Poll/Cancel/Delete):
//       - counting semantics and max_count clamping,
//       - argument validation and fail-closed error codes,
//       - Poll never blocks and reports empty correctly,
//       - a real producer/consumer handoff across two threads (Wait blocks
//         until another thread Signals),
//       - timeout path returns the timeout status without hanging.
//   * SyncOnAddress (Wait32/Wake): a thread parked on a guest address is
//       released both by a value change (futex-style) and by an explicit Wake,
//       and Wake reports the number of waiters it released.
//   * Time (KernelClockGettime/KernelGetProcessTimeCounter*/KernelGettimeofday):
//       monotonic counter advances, frequency is non-zero, wall clock is sane,
//       and null-pointer arguments fail closed.
//
// Dependency-free: it links only the three kernel .cpp units under test. No
// Vulkan, no VMM, no optional backends.
// ============================================================================

#include "kernel/semaphore.h"
#include "kernel/syncOnAddress.h"
#include "kernel/time.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(const char* name, bool ok) {
    ++g_checks;
    if (ok) {
        std::cout << "[PASS] " << name << "\n";
    } else {
        ++g_failures;
        std::cout << "[FAIL] " << name << "\n";
    }
}

using namespace Libs::LibKernel;

} // namespace

int main() {
    std::cout << "=== HLE libKernel Integration Test ===\n";

    // ----------------------------------------------------------------------
    // 1. Counting semaphore: create / signal / clamp / poll / wait / delete.
    // ----------------------------------------------------------------------
    {
        Semaphore::KernelSema sem = 0;
        int rc = Semaphore::KernelCreateSema(&sem, "hle_sem", 0, /*init*/ 1, /*max*/ 2, nullptr);
        Check("SemaCreateSucceeds", rc == 0 && sem != 0);

        // Poll succeeds while a token is available, then reports empty without
        // blocking.
        Check("SemaPollTakesAvailableToken", Semaphore::KernelPollSema(sem, 1) == 0);
        Check("SemaPollEmptyFailsClosed", Semaphore::KernelPollSema(sem, 1) != 0);

        // Signal twice; max_count = 2 clamps the count.
        Check("SemaSignalOk", Semaphore::KernelSignalSema(sem, 1) == 0);
        Check("SemaSignalOk2", Semaphore::KernelSignalSema(sem, 1) == 0);
        // A third signal would exceed max_count; it must not raise the count
        // above the max, so exactly two Polls can succeed.
        Semaphore::KernelSignalSema(sem, 1);
        Check("SemaPollAfterClampTakes1", Semaphore::KernelPollSema(sem, 1) == 0);
        Check("SemaPollAfterClampTakes2", Semaphore::KernelPollSema(sem, 1) == 0);
        Check("SemaPollAfterClampEmpty", Semaphore::KernelPollSema(sem, 1) != 0);

        // Argument validation.
        Check("SemaSignalRejectsNonPositive", Semaphore::KernelSignalSema(sem, 0) != 0);
        Check("SemaDeleteSucceeds", Semaphore::KernelDeleteSema(sem) == 0);
        Check("SemaDoubleDeleteFailsClosed", Semaphore::KernelDeleteSema(sem) != 0);
    }

    // ----------------------------------------------------------------------
    // 2. Cross-thread producer/consumer: Wait blocks until Signal arrives.
    // ----------------------------------------------------------------------
    {
        Semaphore::KernelSema sem = 0;
        Semaphore::KernelCreateSema(&sem, "hle_pc", 0, /*init*/ 0, /*max*/ 1, nullptr);

        std::atomic<bool> consumed{false};
        std::thread consumer([&] {
            // Blocking wait (timeout == nullptr means wait forever).
            int rc = Semaphore::KernelWaitSema(sem, 1, nullptr);
            if (rc == 0) {
                consumed.store(true);
            }
        });

        // Give the consumer time to park on the empty semaphore.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        Check("SemaWaitBlocksUntilSignal", !consumed.load());

        Semaphore::KernelSignalSema(sem, 1);
        consumer.join();
        Check("SemaWaitWakesAfterSignal", consumed.load());
        Semaphore::KernelDeleteSema(sem);
    }

    // ----------------------------------------------------------------------
    // 3. Semaphore timeout path returns without hanging.
    // ----------------------------------------------------------------------
    {
        Semaphore::KernelSema sem = 0;
        Semaphore::KernelCreateSema(&sem, "hle_to", 0, /*init*/ 0, /*max*/ 1, nullptr);
        uint32_t timeout_usec = 20000; // 20 ms
        const auto start = std::chrono::steady_clock::now();
        int rc = Semaphore::KernelWaitSema(sem, 1, &timeout_usec);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        Check("SemaWaitTimesOut", rc != 0);
        Check("SemaWaitTimeoutReturnsPromptly",
              elapsed < std::chrono::milliseconds(500));
        Semaphore::KernelDeleteSema(sem);
    }

    // ----------------------------------------------------------------------
    // 4. SyncOnAddress: value-change wakeup (futex-style fast path).
    // ----------------------------------------------------------------------
    {
        std::atomic<uint32_t> word{7};
        std::atomic<bool> released{false};
        std::thread waiter([&] {
            int rc = SyncOnAddress::Wait32(
                reinterpret_cast<volatile uint32_t*>(&word), /*expected*/ 7,
                /*timeout_micros*/ 0, nullptr);
            if (rc == 0) {
                released.store(true);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Check("SyncWaitBlocksWhileValueUnchanged", !released.load());
        // Change the value: the waiter's fast path should observe it and return.
        word.store(9);
        waiter.join();
        Check("SyncWaitWakesOnValueChange", released.load());
    }

    // ----------------------------------------------------------------------
    // 5. SyncOnAddress: explicit Wake releases parked waiters and counts them.
    // ----------------------------------------------------------------------
    {
        std::atomic<uint32_t> word{3};
        std::atomic<int> woken{0};
        auto body = [&] {
            SyncOnAddress::Wait32(reinterpret_cast<volatile uint32_t*>(&word),
                                  /*expected*/ 3, /*timeout_micros*/ 0, nullptr);
            woken.fetch_add(1);
        };
        std::thread t0(body);
        std::thread t1(body);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        Check("SyncTwoWaitersParked", woken.load() == 0);
        int released = SyncOnAddress::Wake(
            reinterpret_cast<volatile void*>(&word), /*count*/ -1);
        t0.join();
        t1.join();
        Check("SyncWakeReleasesBothWaiters", woken.load() == 2);
        Check("SyncWakeReportsWaiterCount", released == 2);
    }

    // ----------------------------------------------------------------------
    // 6. Time subsystem: monotonic counter, non-zero frequency, sane wall
    //    clock, and fail-closed null handling.
    // ----------------------------------------------------------------------
    {
        const uint64_t freq = KernelGetProcessTimeCounterFrequency();
        Check("TimeCounterFrequencyNonZero", freq != 0);

        const uint64_t c0 = KernelGetProcessTimeCounter();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const uint64_t c1 = KernelGetProcessTimeCounter();
        Check("TimeCounterIsMonotonic", c1 >= c0);

        KernelTimespec ts{};
        int rc = KernelClockGettime(0, &ts);
        // Wall clock should be well past 2020-01-01 (1577836800 s).
        Check("ClockGettimeReturnsSaneWallClock",
              rc == 0 && ts.tv_sec > 1577836800);
        Check("ClockGettimeRejectsNull", KernelClockGettime(0, nullptr) != 0);

        KernelTimeval tv{};
        Check("GettimeofdayReturnsSaneWallClock",
              KernelGettimeofday(&tv) == 0 && tv.tv_sec > 1577836800);
    }

    std::cout << g_checks - g_failures << "/" << g_checks << " checks passed\n";
    if (g_failures == 0) {
        std::cout << ">> [PASS] HLE libKernel primitives verified "
                     "(semaphore, sync-on-address, time -- real cross-thread behaviour).\n";
    }
    return g_failures == 0 ? 0 : 1;
}
