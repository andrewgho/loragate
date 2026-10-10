# Transmitter build plan

Design plan for the real deployed transmitter (phase 2) — event-driven on a
reed switch instead of tx-test's fixed 10s timer, battery-powered, and
implementing the actual door-open/door-closed/left-open/heartbeat protocol
sketched in `README.md`. Physical wiring reference is `WIRING.md`; this doc
covers firmware behavior and the power budget it needs to hit.

First draft came from an AI-authored plan
(`https://alleycat.moychan.com/at/lora_gate_sensor.html`) — see
`WIRING.md`'s "Corrections from the first draft" for what didn't hold up.
The state machine and power-budget structure below are adapted from it; the
numbers are re-derived against what we can actually confirm, not copied.

## Wake behavior

**Implemented in `tx/tx.ino`.** Two wakeup sources armed before every
sleep, matching whichever edge is next expected:

- **Reed switch edge (D0/GPIO0, `esp_sleep_enable_ext1_wakeup`)** — armed
  for the *opposite* of the current state: if the door is open, wake on
  LOW (closing); if closed, wake on HIGH (opening). This was originally
  planned as `esp_sleep_enable_ext0_wakeup()` (below), but the C6 doesn't
  implement ext0 at all — its `soc_caps.h` has no
  `SOC_PM_SUPPORT_EXT0_WAKEUP` — only ext1, which takes a GPIO bitmask
  instead of a single pin number. For one pin that's just
  `esp_sleep_enable_ext1_wakeup(1ULL << REED_PIN, mode)`; its doc comment
  confirms each call fully replaces the previous ext1 config, so flipping
  the wakeup level between wakes is safe without an explicit disable call
  first.
- **RTC timer, always armed** — `HELD_OPEN_CHECK_SECONDS` (300s) once, right
  after an open edge, to re-verify against real hardware state and report
  if the door's still open; after that one check fires (or if the door
  closes first and it's never needed), `HEARTBEAT_INTERVAL_SECONDS` (1h
  default) the rest of the time, closed or open, so *something* goes out at
  least that often even with no door activity. This is deliberately a
  one-shot check, not a repeating one — re-arming 300s for as long as the
  door stays open would mean ~280 wakes/day for a door left open all day,
  which is real battery budget (see "Power budget" below) spent on a
  condition the first check already reported. Every event the firmware
  sends — open, close, held-open, or the heartbeat itself — counts equally
  as "we're still alive" for this purpose; there's no separate heartbeat
  clock to track across sleeps.
  - The receiver's own ~45s "door open past" alert (independent of this,
    see `README.md`'s "Phase 2 protocol") is what covers "someone just
    opened the door" on a faster timescale than this 300s check needs to.

## Rapid-slam / bounce guard

If the door slams shut while the node is still transmitting the
door-open packet, the pin can flip before sleep is entered. Re-reading
the pin immediately before committing to `esp_deep_sleep_start()`, and
restarting instead of sleeping if it changed out from under us, avoids
arming a wakeup edge that already happened (which would just sleep through
it and miss the transition) — see `enter_safe_deep_sleep()` in
`tx/tx.ino`, which implements this.

Confirming `esp_sleep_enable_ext1_wakeup()` actually wakes the board on a
real reed-switch edge (vs. just compiling) is still only done via the
`-DDEBUG` delay/restart substitute (see `tx/Makefile`'s `make debug`), not
yet against real deep sleep on battery power.

## Never missing a state change

The design goal here is stronger than "wake up reliably": a series of
rapid transitions can be collapsed into just reporting the final state
(see the rapid-slam guard above, and `report_door_transition()`'s
catch-up loop for the equivalent case while a send is in flight), but the
*net* state must never silently drift out of sync with what's been
reported. Two things make that hold:

- **A persisted "last reported" baseline** (`door_open_committed`,
  `RTC_DATA_ATTR`, survives deep sleep) that every wake diffs the pin
  against — not `esp_sleep_get_wakeup_cause()`, which is never consulted.
  Every wake, for any reason, ends up asking the same question: "does the
  real pin state match what we last told the world?" This sidesteps an
  entire class of bugs where a wake gets misattributed to the wrong cause.
- **A hard backstop on every path**: an open episode always gets a wake
  within `HELD_OPEN_CHECK_SECONDS`, a closed/idle period within
  `HEARTBEAT_INTERVAL_SECONDS`. Both of those wakes re-run the same diff.
  So even in a race this doc hasn't anticipated, the worst case is added
  latency (≤300s or ≤1h) before the mismatch is caught and reported, never
  a transition that's lost for good.

One real gap in this existed until it was caught by code review: `setup()`
established `door_open_committed` (and `have_committed_state`) only
*after* `radio.begin()` succeeded. If the radio failed on the very first
wake ever (e.g. a flaky SPI connection — independent of whether power/RTC
memory was fine) and only came up on some later wake, that later wake
would hit the "first boot, nothing to compare against yet" branch and
silently adopt whatever the door happened to be doing *then* as the
starting baseline — with no event ever sent for a real transition that
happened during the failed-radio window. Fixed by also establishing the
baseline (from the pin alone, which needs no radio) inside the
`radio.begin()` failure branch, so `have_committed_state` only ever means
"this device has been through a real power-on," never "...and the radio
also happened to work that same wake."

## Power budget

Deep sleep leakage sources, with confidence noted — none of these are
measured yet, they're either datasheet figures or estimates:

| Source | Estimate | Basis |
|---|---|---|
| C6 SoC deep sleep | ~18µA | Espressif datasheet, worst-case temp |
| SX1262 sleep | ~2.5µA | requires explicit `radio.sleep()` before deep sleep — tx-test doesn't do this yet, phase 2 firmware must add it |
| Board passive leakage | ~8µA | estimate, not measured |
| Floating/unused GPIOs | ~6.5µA | mitigated by setting unused pins to `ANALOG`/input-disabled before sleep |
| Reed switch pull-up | ~3.3µA (closed) | external 1MΩ per WIRING.md, not the ~73µA an internal pull-up would cost |
| Battery-voltage divider + ADC | ~1.3µA (fresh pack, worst case) | 4.7MΩ+2.2MΩ raw-battery divider (~0.78µA) plus the ADS1115's own ~0.5µA typical power-down draw — see WIRING.md's "Battery voltage monitoring" |
| LDO quiescent — Option A (TPS7A0233) | ~0.000025µA | 25nA typical, even in dropout — doesn't move the total. Not currently installed — see WIRING.md's "Power" → "Choosing an LDO" |
| LDO quiescent — Option B (MCP1700) | ~1.6µA typ / 4µA max | 60-160x Option A's figure, but confirmed working. **Currently installed** |
| **Total, Option A** | **~39µA** | vs. the original plan's ~65µA, mostly because the pull-up choice differs |
| **Total, Option B (as currently built)** | **~41µA typ / ~44µA max** | same breakdown, with Option B's higher LDO quiescent current in place of Option A's |

Active transmit: ~120ms at ~35mA per event (radio TX current at 22dBm,
datasheet figure) — negligible against sleep current unless the gate cycles
constantly.

At 20 transitions/day (10 open + 10 close) and ~2,100-2,250mAh realistically
extractable from the L91 pack (nominal 3,000mAh derated for cold-weather
internal resistance and brownout headroom — see the L91 datasheet linked in
WIRING.md). Series cells don't add capacity, only voltage, so the 3-cell
pack's mAh figure is the same as a 2-cell pack's would have been — the LDO
regulator draws roughly the same current from its input as it delivers at
its output regardless of input voltage, so the third cell costs nothing
here, per WIRING.md's "Power" section. Figured for both LDO options, since
neither is more "correct" than the other until Option A has a working unit
to measure:

```
Option A (TPS7A0233):
sleep:  0.039mA * 24h                    ≈ 0.936 mAh/day
active: 20 * 0.120s * 35mA / 3600        ≈ 0.023 mAh/day
total                                    ≈ 0.959 mAh/day
2,100 mAh / (0.959 mAh/day * 365)        ≈ 6.0 years

Option B (MCP1700, typical IQ — as currently built):
sleep:  0.0412mA * 24h                   ≈ 0.989 mAh/day
active: 20 * 0.120s * 35mA / 3600        ≈ 0.023 mAh/day
total                                    ≈ 1.012 mAh/day
2,100 mAh / (1.012 mAh/day * 365)        ≈ 5.7 years
```

Option B at its 4µA worst-case (not just 1.6µA typical) comes out to
roughly 5.4 years instead of 5.7 — still comfortably clears the "2+ years
would be satisfying in practice" bar this project is actually targeting,
which matters more here than the gap between the two options.

Treat this as optimistic-of-the-corrected-numbers, not a promise — the
individual leakage line items are still estimates. Worth an actual current
measurement (multimeter or a µCurrent-style tool in series with the battery)
once the real transmitter is built, both to sanity-check this table and to
catch anything the table missed.

## Open items

- The wake/sleep state machine and door-event packet types above are
  implemented in `tx/tx.ino`. Still missing from that firmware: battery
  voltage via the ADS1115 (see below) and, if added, DS3231 read-on-wake.
- Sleep current is unmeasured — everything in the power budget is a
  datasheet or estimated figure.
- `esp_sleep_enable_ext1_wakeup()` on D0 needs confirming against a real
  reed-switch edge during actual deep sleep, not just the `-DDEBUG`
  delay/restart substitute it's been exercised with so far.
- The floating/unused-GPIO power-budget line item (`ANALOG`/input-disable
  before sleep) isn't wired up in `tx/tx.ino` yet — the wake/sleep cycle
  itself was the priority; this is a smaller follow-up power optimization.
- If the DS3231 is added: confirm which of RTC-reported time vs. receiver
  receipt time actually ends up in the reported event, and whether it's
  worth the extra I2C traffic on every wake given receipt time was already
  judged "close enough" — this is explicitly the lower-priority half of the
  build.
- The LDO added for battery safety (WIRING.md) regulates out the raw battery
  voltage, so `A1`/`VBAT` reads a flat ~3.3V for most of the pack's life
  instead of trending down with it. Fixed in WIRING.md's "Battery voltage
  monitoring" section: a raw-battery divider into a small I2C ADC (ADS1115)
  on D6/D7, since it turned out no ESP32 ADC pin was actually free (see that
  section for why). Hardware designed; firmware to read it over I2C on wake
  and add the value to the heartbeat packet doesn't exist yet. When writing
  it: set the ADS1115's PGA gain register for ±4.096V full scale (not the
  library default ±2.048V) — this is a resolution/accuracy tradeoff in our
  favor, not a hazard like the MCP3421 alternative WIRING.md considered and
  rejected would have been, so getting it wrong just means slightly worse
  (not broken) readings. See WIRING.md's "Why the ADS1115 over the MCP3421
  considered earlier" for the numbers.
