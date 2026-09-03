// ProsperoLayer PS5 emulator - cross-thread signaling
#include "common/common.h"

#include <cstdint>

namespace Common {

void CondVar::SignalThread(uint64_t thread_id) {
        // The scheduler wakes guest threads through the Pthread layer; a
        // host-side signal is a no-op unless a native waiter exists.
        (void)thread_id;
}

} // namespace Common
