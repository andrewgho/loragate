// Pure helper for rx/rx.ino's handle_packet() — no Arduino/radio
// dependencies, so test/test_seq_dedup.cpp can exercise it with a plain
// C++ compiler.

#pragma once

#include <stdint.h>

// True if `seq` repeats the last processed seq. seq is one counter shared
// across all packet types (tx/tx.ino's send_event()), and a tx retry
// resends the same seq — the only way one is seen twice. A lower seq
// isn't a duplicate (it'd mean tx lost state) and is processed normally.
inline bool is_duplicate_seq(bool have_last_seq, uint32_t last_seq, uint32_t seq) {
  return have_last_seq && seq == last_seq;
}
