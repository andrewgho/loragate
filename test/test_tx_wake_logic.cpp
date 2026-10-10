// Host-side test for tx/door_wake_logic.h — the pure wake-decision logic
// extracted out of tx/tx.ino's setup(). No ESP32/Arduino toolchain needed,
// just a C++ compiler; doesn't touch or replace testing the actual
// firmware (radio/sleep/reed-switch hardware behavior still only gets
// verified by flashing real hardware) — this locks down the *decisions*
// tx.ino makes, independent of the I/O that carries them out.
//
// Several of these cases are direct regression tests for bugs caught by
// manual code review before this file existed — see PLAN.md's "Never
// missing a state change" and "Wake behavior" sections for the reasoning.

#include <cassert>
#include "door_wake_logic.h"

int main() {
  // --- Cold boot: no event, baseline recorded, regardless of door state ---
  {
    DoorWakeState state = {false, false, false};
    WakeAction action = decide_wake_action(state, /*actual_open=*/false);
    assert(action == WAKE_ACTION_NONE);
    assert(state.have_committed_state == true);
    assert(state.door_open_committed == false);
    assert(state.held_open_checked == false);
  }
  {
    DoorWakeState state = {false, false, false};
    WakeAction action = decide_wake_action(state, /*actual_open=*/true);
    assert(action == WAKE_ACTION_NONE);
    assert(state.have_committed_state == true);
    assert(state.door_open_committed == true);
    // Cold boot never sends PKT_DOOR_HELD_OPEN even if the door happens to
    // already be open — that's not a transition we witnessed.
  }

  // --- establish_baseline_if_needed() alone: once set, never re-set ---
  {
    DoorWakeState state = {false, false, false};
    assert(establish_baseline_if_needed(state, /*actual_open=*/false) == true);
    assert(state.door_open_committed == false);

    // A second call, even with a different actual_open, must not silently
    // re-baseline past whatever happened in between — this is the crux of
    // the radio.begin()-failure bug: have_committed_state must mean "a
    // true power-on happened", not "...and the radio worked that time".
    assert(establish_baseline_if_needed(state, /*actual_open=*/true) == false);
    assert(state.door_open_committed == false);  // unchanged
  }

  // --- Regression: radio fails before any baseline exists, door moves
  // during the outage, radio recovers later — the transition must still
  // be reported, not silently adopted as "the starting state". ---
  {
    DoorWakeState state = {false, false, false};
    // Wake 1: radio.begin() fails. tx.ino calls establish_baseline_if_needed()
    // directly (not decide_wake_action()) in that branch.
    assert(establish_baseline_if_needed(state, /*actual_open=*/false) == true);

    // Door opens while the radio is down (not observable here — no wake
    // happens until the radio comes back up).

    // Wake N: radio.begin() succeeds this time; the door is now open.
    WakeAction action = decide_wake_action(state, /*actual_open=*/true);
    assert(action == WAKE_ACTION_REPORT_TRANSITION);
    assert(state.door_open_committed == true);
    assert(state.held_open_checked == false);
  }

  // --- Regression: radio fails *after* a baseline exists, door round-trips
  // open-then-closed entirely during the outage — net state unchanged, so
  // nothing is reported once the radio recovers. (Missing a *series* of
  // changes during an outage is explicitly fine; missing the net change
  // is not — and here there isn't one.) ---
  {
    DoorWakeState state = {true, false, false};  // already had a baseline: closed
    assert(establish_baseline_if_needed(state, /*actual_open=*/true) == false);
    assert(state.door_open_committed == false);  // untouched by the no-op

    WakeAction action = decide_wake_action(state, /*actual_open=*/false);
    assert(action == WAKE_ACTION_HEARTBEAT);  // closed, matches committed state
  }

  // --- Edge detected: closed -> open, and open -> closed ---
  {
    DoorWakeState state = {true, false, false};
    WakeAction action = decide_wake_action(state, /*actual_open=*/true);
    assert(action == WAKE_ACTION_REPORT_TRANSITION);
    assert(state.door_open_committed == true);
    assert(state.held_open_checked == false);
  }
  {
    DoorWakeState state = {true, true, true};  // open, already held-open-checked
    WakeAction action = decide_wake_action(state, /*actual_open=*/false);
    assert(action == WAKE_ACTION_REPORT_TRANSITION);
    assert(state.door_open_committed == false);
    assert(state.held_open_checked == false);  // reset on every transition
  }

  // --- Held-open check: fires once per open episode ---
  {
    DoorWakeState state = {true, true, false};  // open, not yet checked
    WakeAction action = decide_wake_action(state, /*actual_open=*/true);
    assert(action == WAKE_ACTION_HELD_OPEN_CHECK);
    assert(state.held_open_checked == true);
  }

  // --- Regression: once checked, staying open keeps sending heartbeats,
  // not a repeated PKT_DOOR_HELD_OPEN — the bug fixed in "Make the
  // held-open check one-shot instead of repeating". ---
  {
    DoorWakeState state = {true, true, true};  // open, already checked
    WakeAction action = decide_wake_action(state, /*actual_open=*/true);
    assert(action == WAKE_ACTION_HEARTBEAT);
    assert(state.held_open_checked == true);  // still true, not re-triggered
    assert(state.door_open_committed == true);  // still open
  }

  // --- Idle closed: plain heartbeat ---
  {
    DoorWakeState state = {true, false, false};
    WakeAction action = decide_wake_action(state, /*actual_open=*/false);
    assert(action == WAKE_ACTION_HEARTBEAT);
  }

  // --- Full lifecycle: open -> held-open check -> close -> reopen ->
  // gets its own fresh held-open check, not skipped because the flag was
  // left over from the previous episode. ---
  {
    DoorWakeState state = {true, false, false};

    assert(decide_wake_action(state, true) == WAKE_ACTION_REPORT_TRANSITION);
    assert(decide_wake_action(state, true) == WAKE_ACTION_HELD_OPEN_CHECK);
    assert(decide_wake_action(state, true) == WAKE_ACTION_HEARTBEAT);  // already checked

    assert(decide_wake_action(state, false) == WAKE_ACTION_REPORT_TRANSITION);  // closes
    assert(decide_wake_action(state, false) == WAKE_ACTION_HEARTBEAT);  // idle closed

    assert(decide_wake_action(state, true) == WAKE_ACTION_REPORT_TRANSITION);  // reopens
    assert(decide_wake_action(state, true) == WAKE_ACTION_HELD_OPEN_CHECK);  // fresh episode, fresh check
  }

  // --- Sleep duration follows the same "open and not yet checked" rule
  // decide_wake_action() uses, independent of its specific numeric values ---
  {
    DoorWakeState open_unchecked = {true, true, false};
    assert(decide_sleep_seconds(open_unchecked, 300, 3600) == 300);

    DoorWakeState open_checked = {true, true, true};
    assert(decide_sleep_seconds(open_checked, 300, 3600) == 3600);

    DoorWakeState closed = {true, false, false};
    assert(decide_sleep_seconds(closed, 300, 3600) == 3600);
    DoorWakeState closed_checked_flag_irrelevant = {true, false, true};
    assert(decide_sleep_seconds(closed_checked_flag_irrelevant, 300, 3600) == 3600);
  }

  return 0;
}
