// Host-side test for tx/rtc_checksum.h. No ESP32/Arduino toolchain
// needed, just a C++ compiler — compute_persisted_checksum() is pure, no
// RTC/Arduino dependencies.

#include <cassert>
#include "rtc_checksum.h"

int main() {
  // Determinism: same inputs, same checksum, every time.
  DoorWakeState closed = {true, false, false};
  uint32_t a = compute_persisted_checksum(5, closed, false, 0);
  uint32_t b = compute_persisted_checksum(5, closed, false, 0);
  assert(a == b);

  // The all-safe-reset-values checksum must not be 0 — uninitialized SRAM
  // commonly powers up as all-zero bytes, and if next_seq=0,
  // wake_state={false,false,false}, diagnostic_pending=false,
  // pending_diagnostic_info=0 *and* persisted_checksum=0 all happened to
  // read back together from genuinely uninitialized memory, a checksum of
  // 0 for that input would make a true first boot look "valid" by sheer
  // coincidence instead of correctly failing validation. FNV-1a can't
  // actually produce 0 here (its offset basis and prime are both odd, and
  // odd*odd never lands on a multiple of 2^32), but that's exactly the
  // property worth pinning down with a test rather than trusting by
  // inspection.
  DoorWakeState all_false = {false, false, false};
  assert(compute_persisted_checksum(0, all_false, false, 0) != 0);

  // Sensitivity: changing any single input changes the checksum. Not a
  // formal no-collisions proof — just confirms the function actually
  // looks at all four parameters instead of silently ignoring one.
  uint32_t base = compute_persisted_checksum(5, closed, false, 0);

  DoorWakeState open = {true, true, false};
  assert(compute_persisted_checksum(6, closed, false, 0) != base);       // next_seq
  assert(compute_persisted_checksum(5, open, false, 0) != base);         // wake_state
  assert(compute_persisted_checksum(5, closed, true, 0) != base);        // diagnostic_pending
  assert(compute_persisted_checksum(5, closed, false, 9) != base);       // pending_diagnostic_info

  // fnv1a_update() itself: folding in zero-length data is a no-op: the
  // hash is unchanged (nothing to mix in), matching how
  // compute_persisted_checksum() is composed of several such calls in a
  // row.
  uint32_t h = 2166136261UL;
  assert(fnv1a_update(h, "", 0) == h);

  return 0;
}
