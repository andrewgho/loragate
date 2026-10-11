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

**Implemented in `tx/tx.ino`** — with the decision of what each wake
means and should send factored out into `tx/door_wake_logic.h`, kept free
of Arduino/radio dependencies specifically so it's unit-tested
(`test/test_tx_wake_logic.cpp`, see README.md's "Testing") independent of
any hardware. `tx.ino`'s `setup()` is the I/O shell: reads the pin, calls
into that logic, and carries out whichever action it decided. Two wakeup
sources armed before every sleep, matching whichever edge is next
expected:

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

**A real ext1 edge with no net transition isn't nothing — it needs its
own outcome.** Caught via manual review of a live field log: a reed
switch (especially outdoor, subject to wind/vibration on the gate) can
bounce or chatter briefly enough that `read_debounced_state()` settles
back to the same value, while still firing a real ext1 wakeup. Before
this was handled, `decide_wake_action()` had no way to tell that apart
from "the timer legitimately elapsed" — both landed on the same
heartbeat/held-open-check branches — so every such chatter wake burned a
full radio-and-transmit cycle on an unneeded heartbeat, and during an
open episode could consume the held-open check's one-shot budget long
before `HELD_OPEN_CHECK_SECONDS` had actually passed. Fixed by passing
`esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER` into
`decide_wake_action()` as `timer_wakeup`, consulted only in the
already-no-transition branches — a new `WAKE_ACTION_SPURIOUS` covers "no
change, and it wasn't the timer either," which just goes back to sleep
without transmitting. See "Never missing a state change" below for why
this doesn't weaken that invariant.

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

- **A persisted "last reported" baseline** (`wake_state.door_open_committed`
  in `door_wake_logic.h`'s `DoorWakeState`, held in an `RTC_NOINIT_ATTR`
  global so it survives both deep sleep and a restart — see "Reset
  diagnostics") that every wake diffs the pin against. Every wake, for
  any reason, ends up asking the same question: "does the real pin state
  match what we last told the world?" — whether a transition happened is
  never decided from `esp_sleep_get_wakeup_cause()`, sidestepping an
  entire class of bugs where that gets misattributed to the wrong cause.
  `esp_sleep_get_wakeup_cause()` *is* consulted for a narrower, separate
  question — once a wake is already known to carry no transition, was it
  the timer or a spurious edge (see "Wake behavior"'s
  `WAKE_ACTION_SPURIOUS`) — which never feeds back into whether a
  transition gets reported.
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
`radio.begin()` failure branch, via `establish_baseline_if_needed()`, so
`have_committed_state` only ever means "this device has been through a
real power-on," never "...and the radio also happened to work that same
wake."

Both this fix and the one-shot held-open behavior above are pinned down
by `test/test_tx_wake_logic.cpp` as explicit regression cases — see
README.md's "Testing" — rather than resting on manual review alone.

## Reset diagnostics

Prompted by a live-testing symptom on battery (3x fresh AA lithium
primaries into the MCP1700, 1µF in/out caps): rapid open/close toggling
occasionally dropped the close event entirely, and the *next* event pair
showed `seq` reset to 0. `next_seq` is `RTC_DATA_ATTR` and nothing in
`tx/tx.ino` ever sets it back to 0 itself, so a seq reset can only mean
an actual power-on or brownout reset occurred — most likely from two LoRa
TX current bursts (open immediately followed by close) landing close
enough together that the battery/LDO hadn't recovered from the first
before the second, browning out mid-transmission before the close event
could complete or get acked.

`setup()` checks `esp_reset_reason()` as early as possible — before
`radio.begin()`, since that read needs no radio — and sends
`PKT_DIAGNOSTIC` (carrying the raw reason code in the packet's `info`
byte) whenever it reads back anything other than the two routine causes:
`ESP_RST_DEEPSLEEP` (every ordinary wake) and `ESP_RST_SW` (our own
rapid-slam-guard restart, above). `rx/rx.ino` maps the code to a short
name (`reset_reason_name()`) and logs it to `DIAGNOSTIC_URL_TEMPLATE` —
see README.md's "Phase 2 protocol".

Two real bugs turned up while chasing this, each caught by a live
capture, each fixed:

1. **The diagnostic itself could go missing.** The wake immediately after
   an abnormal reset is exactly the one `radio.begin()` is least likely to
   succeed on (things are least settled right then) — if detection only
   happened in the success path, that wake would hit the `radio.begin()`
   failure branch and go straight back to a normal sleep with nothing ever
   checked or sent. The *next* wake's own `esp_reset_reason()` then reads
   back as the routine `ESP_RST_DEEPSLEEP` cause for *that* wake, and the
   anomaly is gone for good. Fixed by moving detection to before
   `radio.begin()` and persisting it (`diagnostic_pending` /
   `pending_diagnostic_info`) across however many wakes it takes for the
   radio to actually come up — sending still only happens once it does.

2. **The actual root cause, found with full tx-side serial visibility**
   (native USB CDC drops on every deep sleep, so this needed UART0 routed
   to an external USB-TTL adapter — `CDCOnBoot=default` in the FQBN,
   D6/D7, no code change). A captured boot showed `rst:0xc (SW_CPU)` —
   our own rapid-slam-guard's `ESP.restart()` — immediately followed by
   `wake_state.have_committed_state` reading back `false` and `next_seq`
   reset to 0, as if it had been a power-on. It hadn't: `RTC_DATA_ATTR` is
   only ever documented to survive *deep sleep* ("will keep its value
   during a deep sleep / wake cycle" — `esp_attr.h`'s own comment,
   confirmed against the installed core directly after a Stack Overflow
   answer pointed at the right distinction); a restart was never covered.
   `RTC_NOINIT_ATTR` is the one documented to survive "after restart or
   during a deep sleep / wake cycle." This was never just a missing
   diagnostic — the door-open transition that triggered the rapid-slam
   guard in the first place was silently re-baselined away on that reset,
   with no event ever sent for it, a real violation of "never missing a
   state change" above, not just a gap in the feature built to
   investigate a suspected brownout.

   `next_seq`, `wake_state`, `diagnostic_pending`, and
   `pending_diagnostic_info` all moved to `RTC_NOINIT_ATTR`. Since that
   section is skipped entirely by the bootloader's startup init (contents
   are undefined on a true first boot, unlike `RTC_DATA_ATTR`'s), a
   checksum (`tx/rtc_checksum.h`, FNV-1a over all four values, pure and
   host-tested — `test/test_rtc_checksum.cpp`) stands in for the
   compile-time initializers that would no longer be honored: refreshed
   at the top of `enter_safe_deep_sleep()` so it always reflects a wake's
   final values, verified on the next wake, and used to reset everything
   to known-safe values on a mismatch. A mismatch alongside a
   `reset_reason` that still claims to be routine — which should be
   impossible now — gets a distinct sentinel `info` value
   (`RTC_DATA_LOSS_INFO`, 255) rather than being mislabeled with a reason
   that doesn't fit.

This mechanism reports *that* an abnormal reset happened and *what kind*
(brownout, panic, watchdog, a repeat power-on, …) — it doesn't change
anything about the hardware's actual susceptibility to a genuine brownout
in the first place, which (independent of the `ESP.restart()` bug above)
remains a real, still-open question. If `brownout` specifically keeps
showing up, the next place to look is the power rail itself — see "Power
budget" below for the corrected (much higher than first assumed)
active-current figures this is actually stressing, and the already-"free"
lever (lower `LORA_TX_POWER_DBM`) that follows directly from them.

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

Active transmit: ~120ms at **~120mA** per event at +22dBm (the current
`LORA_TX_POWER_DBM`) — negligible against sleep current unless the gate
cycles constantly. This replaces an earlier ~35mA figure here that turned
out to be wrong by more than 3x: pulled directly from two independent
primary sources once the actual datasheets were on hand — Semtech's
SX1261/2 datasheet (Table 3-6, 915MHz band, SX1262: 118mA typical at
+22dBm) and Seeed's Wio-SX1262 *module* datasheet, the actual installed
part (`"Maximum operation current (Transmitter): 125mA @22dBm"`,
independently measured at the module level). Also relevant to the
brownout investigation below: RadioLib already applies Semtech's
per-power-level PA optimization by default (`SX1262::setOutputPower()`'s
`optimize` parameter defaults `true`), so dropping `LORA_TX_POWER_DBM` is
a real, already-"free" lever — roughly 84mA at +20dBm, 58mA at +17dBm,
45mA at +14dBm, no code change beyond the `#define`, not yet done.

That 120ms figure is the radio burst alone — it doesn't include whatever
the CPU does awake before and after it, which this table has otherwise
been treating as negligible. That assumption was briefly false in
practice: `tx/tx.ino` had (until the fix below) a `while (!Serial && ...)`
wait-for-USB-monitor loop at the top of `setup()`, capped at 3000ms, which
never short-circuits early when nothing's listening — i.e. every real
deployment, every wake. At ~20-30mA of plain-awake CPU current, 3s/wake is
roughly 0.02 mAh *per wake*, which at 24+ wakes/day (the hourly floor
alone) is in the same ballpark as this table's entire daily total —
observed directly as human-noticeable wake-to-report latency during live
testing, which is how it was caught. Fixed by gating that wait behind
`-DDEBUG` (a monitor is only ever actually attached in debug builds, per
`make debug`'s own convention), so production wakes proceed immediately.
The rest of setup()'s awake time before/after the radio burst (SPI/radio
init, debounce settles, Serial.println calls) is still real but much
smaller and currently unmeasured — worth confirming with an actual
current-vs-time trace once hardware is available, not assumed negligible
by default now that this table has already been wrong about that once.

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
active: 20 * 0.120s * 125mA / 3600       ≈ 0.083 mAh/day
total                                    ≈ 1.019 mAh/day
2,100 mAh / (1.019 mAh/day * 365)        ≈ 5.6 years

Option B (MCP1700, typical IQ — as currently built):
sleep:  0.0412mA * 24h                   ≈ 0.989 mAh/day
active: 20 * 0.120s * 125mA / 3600       ≈ 0.083 mAh/day
total                                    ≈ 1.072 mAh/day
2,100 mAh / (1.072 mAh/day * 365)        ≈ 5.4 years
```

Option B at its 4µA worst-case (not just 1.6µA typical) comes out to
roughly 5.1 years instead of 5.4 — still comfortably clears the "2+ years
would be satisfying in practice" bar this project is actually targeting,
which matters more here than the gap between the two options. (Using the
125mA module-measured figure here, not the 118mA chip-datasheet one —
slightly more conservative, and it's what the actual installed part is
rated for.) Dropping to a lower `LORA_TX_POWER_DBM` would shrink the
active line further still, on top of easing the brownout risk below.

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
