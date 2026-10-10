// Pure, host-testable decision logic for tx/tx.ino's deep-sleep wake
// cycle — no Arduino/radio/Serial dependencies, so test/test_tx_wake_logic.cpp
// can exercise it with a plain C++ compiler and no ESP32 toolchain. tx.ino
// is the only thing that performs any actual I/O (reading the pin, sending
// packets, sleeping); this header only decides what tx.ino should do.
//
// Not shared with rx/ (unlike lora_packet.h) — this is tx-only wake
// scheduling, not wire protocol, so there's no second copy to keep in sync.

#pragma once

#include <stdint.h>

// Mirrors the fields tx.ino persists in RTC_DATA_ATTR across deep sleep.
struct DoorWakeState {
  bool have_committed_state;  // false only before the very first baseline
  bool door_open_committed;   // last state we've actually reported (or,
                               // before the first report, the established
                               // baseline)
  bool held_open_checked;     // true once PKT_DOOR_HELD_OPEN has gone out
                               // for the current open episode
};

enum WakeAction {
  WAKE_ACTION_NONE,               // first-ever baseline just established; nothing to send
  WAKE_ACTION_REPORT_TRANSITION,  // send PKT_DOOR_OPEN/CLOSE per state.door_open_committed
  WAKE_ACTION_HELD_OPEN_CHECK,    // send PKT_DOOR_HELD_OPEN
  WAKE_ACTION_HEARTBEAT,          // send PKT_HEARTBEAT
};

// If `state` has never had a baseline established, establishes one from
// `actual_open` and returns true; otherwise a no-op, returns false.
//
// Deliberately callable without a radio: tx.ino's radio.begin() failure
// branch calls this directly (not through decide_wake_action(), which
// assumes a send is about to happen) so that a radio fault on the very
// first wake ever can't leave have_committed_state false — see PLAN.md's
// "Never missing a state change" for why that mattered in practice.
inline bool establish_baseline_if_needed(DoorWakeState &state, bool actual_open) {
  if (state.have_committed_state) return false;
  state.door_open_committed = actual_open;
  state.have_committed_state = true;
  return true;
}

// Decides what a wake with a working radio should do, given a fresh
// debounced pin reading, updating `state` to match the decision made.
//
// Doesn't itself run the "door moved again while a send was in flight"
// catch-up loop (report_door_transition() in tx.ino) — once this returns
// WAKE_ACTION_REPORT_TRANSITION, state.door_open_committed already holds
// the state to send; converging on further mid-send flips is tx.ino's
// job, not a decision this function needs to make.
inline WakeAction decide_wake_action(DoorWakeState &state, bool actual_open) {
  if (establish_baseline_if_needed(state, actual_open)) {
    return WAKE_ACTION_NONE;
  }
  if (actual_open != state.door_open_committed) {
    state.door_open_committed = actual_open;
    state.held_open_checked = false;
    return WAKE_ACTION_REPORT_TRANSITION;
  }
  if (state.door_open_committed && !state.held_open_checked) {
    state.held_open_checked = true;
    return WAKE_ACTION_HELD_OPEN_CHECK;
  }
  return WAKE_ACTION_HEARTBEAT;
}

// How long to sleep given `state` (already updated by decide_wake_action()
// or establish_baseline_if_needed()): the one-shot held-open check needs a
// short wake; everything else uses the long heartbeat interval. The two
// interval values are passed in rather than hardcoded so this stays
// decoupled from tx.ino's #defines and testable with arbitrary values.
inline uint32_t decide_sleep_seconds(const DoorWakeState &state,
                                      uint32_t held_open_check_seconds,
                                      uint32_t heartbeat_interval_seconds) {
  return (state.door_open_committed && !state.held_open_checked)
             ? held_open_check_seconds
             : heartbeat_interval_seconds;
}
