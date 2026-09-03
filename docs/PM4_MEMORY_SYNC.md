# PM4 memory and synchronization model

ProsperoLayer's PM4 core now executes a bounded subset of memory/sync packets on the guest-memory bridge.

## WRITE_DATA

The compatibility adapter interprets the payload as:

- payload[0] = control (currently ignored)
- payload[1..2] = destination GVA (lo/hi)
- payload[3..] = contiguous 32-bit data

Writes are all-or-nothing from the translator perspective: invalid destination ranges are rejected without a partial write.

## COPY_DATA

The compatibility adapter interprets:

- payload[0] = control (currently ignored)
- payload[1..2] = source GVA
- payload[3..4] = destination GVA
- payload[5] = dword count

The source range is fully staged before the destination write so an unreadable source cannot produce a partial destination copy.

## WAIT_REG_MEM

The bounded adapter interprets:

- payload[0] bit 0 = 0 for equal, 1 for not-equal
- payload[1..2] = 64-bit guest address
- payload[3..4] = 64-bit reference value
- payload[5] = 32-bit mask; zero means all 64 bits

Waits are capped at 100 ms in this compatibility layer to prevent a malformed guest command stream from hanging the host indefinitely.

## RELEASE_MEM / EVENT_WRITE_EOP

Every packet advances an internal monotonic completion sequence. When a packet supplies the compatibility address/value fields (payload[1..4]), the 64-bit completion value is published to guest memory after the command reaches the translator.

## Renderer fence

`Graphics::GpuFence` is now an atomic 64-bit value paired with a condition variable. `SyncSignal` publishes monotonically increasing values and wakes waiters; `SyncWait` supports finite or indefinite waits.
