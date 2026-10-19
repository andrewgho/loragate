// Pure, host-testable checksum over the handful of values tx.ino needs to
// survive not just deep sleep but also a restart — stored in
// RTC_NOINIT_ATTR, not RTC_DATA_ATTR (the latter is only documented to
// survive deep sleep, not ESP.restart(); see README.md's Software
// history).
//
// RTC_NOINIT_ATTR sections are skipped entirely by the bootloader's
// startup init, so their contents are undefined garbage on a true first
// boot. Comparing a freshly-computed checksum against one stored
// alongside the data tells "survived for real" apart from "first boot,
// or something corrupted it" — either way, not safe to use as-is.
//
// Not cryptographic — FNV-1a is just sensitive enough to accidental bit
// changes in a few bytes of non-adversarial data, which is all this needs
// to catch.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "door_wake_logic.h"

inline uint32_t fnv1a_update(uint32_t hash, const void *data, size_t len) {
  const uint8_t *bytes = (const uint8_t *)data;
  for (size_t i = 0; i < len; i++) {
    hash ^= bytes[i];
    hash *= 16777619UL;  // FNV-1a prime
  }
  return hash;
}

inline uint32_t compute_persisted_checksum(uint32_t next_seq, const DoorWakeState &wake_state,
                                            bool diagnostic_pending, uint8_t pending_diagnostic_info) {
  uint32_t h = 2166136261UL;  // FNV-1a offset basis
  h = fnv1a_update(h, &next_seq, sizeof(next_seq));
  h = fnv1a_update(h, &wake_state, sizeof(wake_state));
  h = fnv1a_update(h, &diagnostic_pending, sizeof(diagnostic_pending));
  h = fnv1a_update(h, &pending_diagnostic_info, sizeof(pending_diagnostic_info));
  return h;
}
