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
in the first place. That question is effectively resolved now, though not
by this mechanism ever actually firing on one: see "Sleep current root
cause" immediately below — the elevated-current symptom that originally
motivated the brownout hypothesis turned out to have nothing to do with
brownouts at all.

## Sleep current root cause

The brownout question above was investigated by actually measuring, once
a Nordic PPK2 became available — and it turned out to be unrelated to
brownouts entirely. Live sleep current measured ~1.5mA with the door
closed, roughly 40x the ~41-44µA then-budgeted figure (see "Power
budget" below for the real numbers now that this is resolved).

Root-caused through a methodical elimination process — each step
confirmed or refuted on real hardware before moving to the next, nothing
assumed:

1. `LORA_NSS` wasn't held across deep sleep — ESP32-C6 doesn't retain a
   GPIO's driven level across sleep unless explicitly told to
   (`gpio_hold_en()`), and the SX1262 treats a falling edge on NSS as a
   wake-from-SLEEP trigger (Semtech datasheet section 8.2.2). Holding NSS
   dropped sleep current from ~1.5mA to ~318µA — real, but not the whole
   story.
2. Ruled out, each confirmed by a real hardware test rather than
   assumption: `LORA_RFSW`'s level (holding it HIGH made things slightly
   *worse*; holding it LOW was identical to not touching it at all),
   internal pull resistors on `REED_PIN` (`gpio_pullup_dis()`/
   `gpio_pulldown_dis()`, no measurable effect), `ext1` wakeup
   configuration itself (disabling it entirely left the ~318µA signature
   completely unchanged), and the RTC slow clock source (confirmed
   correctly set to the internal oscillator, not an absent external
   crystal that the chip might otherwise keep trying to start up).
3. **The actual cause:** the Wio-SX1262 HAT has its own onboard user
   button (`K1`, unused by this project) with a 10kΩ pull-up (`R2`)
   wired to the *same* `D0` net the reed switch uses — confirmed
   directly from Seeed's own KiCad schematic for the board, not
   inferred. With the door closed, current flows through the parallel
   combination of `R2` (10kΩ) and the external 1MΩ pull-up:
   `3.3V / (1MΩ ∥ 10kΩ) ≈ 333µA`, matching the measured ~318µA closely.
   Confirmed definitively with a live A/B test on the actual hardware
   (jumper-wired by hand, no permanent modification, before committing
   to anything irreversible): bypassing the Wio HAT's `D0` connection
   entirely (direct wire from the XIAO's `D0` pin to the reed-switch
   circuit, Wio HAT's `D0` pin left unconnected) dropped sleep current to
   a clean **21.9µA (door closed) / 18.5µA (door open)** — *below* the
   original budget, and the remaining ~3.4µA difference matches the
   external 1MΩ pull-up's own expected cost almost exactly
   (`3.3V/1MΩ ≈ 3.3µA`). No further mystery current left unaccounted for.

**Fix, hardware-only, no firmware change:** see WIRING.md's "The
Wio-SX1262 HAT's onboard button shares D0" for the physical detail —
`D0` is snipped out of the stacking header entirely and bridged with a
direct wire from the XIAO straight to the reed-switch circuit, bypassing
the Wio HAT's shared net (and its button) completely.

One real, separate finding along the way, independent of the above and
deliberately not yet acted on: if `radio.begin()` ever fails with the
Wio HAT physically present and powered (observed once already, with
`RADIOLIB_ERR_CHIP_NOT_FOUND`, though not from a genuine hardware
fault), the existing fallback path sleeps without ever successfully
commanding the radio into its own SLEEP mode — and a powered,
never-commanded SX1262 draws ~2.7mA, not a few µA, for the entire sleep
duration. At the hourly heartbeat floor, one such failure would cost
more power in a single sleep period than this table's entire daily
budget. `radio.begin()` failing in the field hasn't actually been
observed, so this is deferred rather than fixed now — see "Open items".

## Power budget

Deep sleep leakage sources. Option B (MCP1700, currently installed) now
has a real measured total rather than a sum of estimates — see "Sleep
current root cause" above for how it was measured and what it took to
get there:

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
| **Total, Option A (estimated — not measured)** | **~39µA** | vs. the original plan's ~65µA, mostly because the pull-up choice differs; Option A isn't currently installed, so this is still a sum of estimates, not a measurement |
| **Total, Option B — MEASURED** | **21.9µA (door closed) / 18.5µA (door open)** | live PPK2 measurement on the actual built unit, after the `D0`/Wio-HAT fix above — see "Sleep current root cause". Lower than the ~41-44µA sum-of-estimates directly above it; one or more of those individual line items was itself conservative, not worth re-deriving which now that the real total is in hand. Door-closed is the conservative figure to budget against, since that's the gate's typical resting state. |

Active transmit: ~120ms at **~120mA** per event at +22dBm (the current
`LORA_TX_POWER_DBM`) — negligible against sleep current unless the gate
cycles constantly. This replaces an earlier ~35mA figure here that turned
out to be wrong by more than 3x: pulled directly from two independent
primary sources once the actual datasheets were on hand — Semtech's
SX1261/2 datasheet (Table 3-6, 915MHz band, SX1262: 118mA typical at
+22dBm) and Seeed's Wio-SX1262 *module* datasheet, the actual installed
part (`"Maximum operation current (Transmitter): 125mA @22dBm"`,
independently measured at the module level). RadioLib already applies Semtech's
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

At up to ~54 radio events/day — a deliberately conservative sum, not
netting out any overlap between the pieces below — and ~2,100-2,250mAh
realistically extractable from the L91 pack (nominal 3,000mAh derated for
cold-weather internal resistance and brownout headroom — see the L91
datasheet linked in WIRING.md). That event count is 20 door transitions/day
(10 open + 10 close, as before) plus two pieces "Wake behavior" describes
that this table previously left out entirely: up to 24 hourly
heartbeats/day (the floor that applies even on a day with zero door
activity) and up to 10 held-open checks/day (one per open episode that
runs past `HELD_OPEN_CHECK_SECONDS` — an upper bound; many opens close
well before that and send none). Series cells don't add capacity, only
voltage, so the 3-cell pack's mAh figure is the same as a 2-cell pack's
would have been — the LDO regulator draws roughly the same current from
its input as it delivers at its output regardless of input voltage, so
the third cell costs nothing here, per WIRING.md's "Power" section.
Figured for both LDO options, since neither is more "correct" than the
other until Option A has a working unit to measure:

```
Option A (TPS7A0233, estimated — not currently installed, not measured):
sleep:  0.039mA * 24h                    ≈ 0.936 mAh/day
active: 54 * 0.120s * 125mA / 3600       ≈ 0.225 mAh/day
total                                    ≈ 1.161 mAh/day
2,100 mAh / (1.161 mAh/day * 365)        ≈ 5.0 years

Option B (MCP1700 — currently installed, sleep current MEASURED):
sleep:  0.0219mA * 24h                   ≈ 0.526 mAh/day
active: 54 * 0.120s * 125mA / 3600       ≈ 0.225 mAh/day
total                                    ≈ 0.751 mAh/day
2,100 mAh / (0.751 mAh/day * 365)        ≈ 7.7 years
```

Option B's 21.9µA sleep figure is a real measurement from the actual
built unit (live PPK2, door closed — see "Sleep current root cause"
above), not a datasheet-derived estimate stacked with everything else's
uncertainty, so it's a meaningfully more trustworthy number than the
~41-44µA this table carried before. It could still vary somewhat with
temperature, pack voltage over the discharge curve, or unit-to-unit
tolerance on a different physical build — but 7.7 years clears the "2+
years would be satisfying in practice" bar with enormous margin either
way, more than enough room to absorb that uncertainty. (Using the 125mA
module-measured active-current figure here, not the 118mA
chip-datasheet one — slightly more conservative, and it's what the
actual installed part is rated for.)

Not modeled here, for lack of any measured data to base a number on:
`send_event()` retries up to 5 times with backoff if an ack doesn't come
back, and every active-current figure above assumes a single 120ms burst
per event. Live field logs show `txno=0` on nearly every send at the
tested range, so retries look rare in practice, but a worst case of
constant retries would cost meaningfully more than this table assumes —
worth a measured current-vs-time trace someday, same as the
CPU-awake-time caveat above. With sleep current now measured, this (and
the still-estimated Option A figures) is where this table's remaining
uncertainty actually lives.

## Open items

- The wake/sleep state machine and door-event packet types above are
  implemented in `tx/tx.ino`. Still missing from that firmware: battery
  voltage via the ADS1115 (see below) and, if added, DS3231 read-on-wake.
- **`rx/rx.ino` no longer syncs NTP or reports `localtime=`** — removed
  rather than fixed. The original intent was a stepping stone toward
  reconstructing the actual door-event time (a wire-protocol `age` field
  set by tx from its own wake-to-transmit `millis()`, used by rx to back-
  calculate an event timestamp), but that gap is usually 0-1 seconds at
  the radio level and gets swamped by the notification channel's own
  unpredictable latency (a Discord bot, currently) regardless — not worth
  the wire-format and firmware complexity for what it would buy. If this
  gets revisited, note that rx wouldn't need its own synced clock for it
  either: the monitoring server already timestamps every incoming
  request with its own trusted clock, so a relative age/offset carried on
  the wire is enough for the server to back-calculate the estimated event
  time itself, without rx needing to report an absolute epoch at all.
- Sleep current for Option B is now measured (21.9µA closed / 18.5µA
  open — see "Sleep current root cause"); Option A's figures remain
  pure estimates pending a working unit to actually measure.
- If `radio.begin()` ever fails with the Wio HAT physically present and
  powered, the current fallback path sleeps without ever commanding the
  radio into SLEEP mode, and a powered-but-uncommanded SX1262 draws
  ~2.7mA for the whole sleep duration — found as a side effect of the
  sleep-current investigation above, not yet observed in the field, and
  deliberately deferred rather than fixed. If `radio.begin()` failures
  ever show up for real, this is the first place to look.
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
