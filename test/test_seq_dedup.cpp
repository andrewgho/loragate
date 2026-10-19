// Host-side test for rx/seq_dedup.h. No ESP32/Arduino toolchain needed,
// just a C++ compiler — is_duplicate_seq() is pure.
//
// Regression test for a real bug caught by manual log review: rx acked
// every recognized packet (correct, needed to stop tx retrying) but also
// re-ran the full per-type handling — including the HTTP notify — on a
// retry whose only problem was a lost ack, producing a second
// /notify/door/gate/close hit for the same logical event. See README.md's
// Software History.

#include <cassert>
#include "seq_dedup.h"

int main() {
  // First packet ever seen (have_last_seq still false): never a duplicate,
  // regardless of its seq value.
  assert(is_duplicate_seq(/*have_last_seq=*/false, /*last_seq=*/0, /*seq=*/0) == false);
  assert(is_duplicate_seq(false, 0, 37) == false);

  // Exact repeat of the last seq: the signature of an ack-loss retry.
  assert(is_duplicate_seq(/*have_last_seq=*/true, /*last_seq=*/37, /*seq=*/37) == true);

  // A new, higher seq: a real subsequent event, not a duplicate.
  assert(is_duplicate_seq(true, 37, 38) == false);

  // A seq *lower* than the last one seen: real data loss on the
  // transmitter side (e.g. a true first-boot / RTC data loss resetting
  // next_seq), not a retry — still worth processing, not deduped.
  assert(is_duplicate_seq(true, 37, 0) == false);

  return 0;
}
