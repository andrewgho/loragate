// Pure helper for rx/rx.ino's handle_packet() — no Arduino/radio
// dependencies, so test/test_seq_dedup.cpp can exercise it with a plain
// C++ compiler.

#pragma once

#include <stdint.h>

// True if `seq` is an exact repeat of the last seq already fully
// processed. seq is a single counter shared across every packet type
// (see tx/tx.ino's send_event()), so this one check covers all of them:
// a transmitter retry (tx got no ack back, even though we did receive
// and ack the original) resends the same seq with a different txno, and
// that's the only way the same seq is ever seen twice. Anything else —
// including a seq lower than last_seq, which would mean real data loss
// on the transmitter (see PLAN.md's "Reset diagnostics") — isn't a
// duplicate; it's still a real event worth processing.
inline bool is_duplicate_seq(bool have_last_seq, uint32_t last_seq, uint32_t seq) {
  return have_last_seq && seq == last_seq;
}
