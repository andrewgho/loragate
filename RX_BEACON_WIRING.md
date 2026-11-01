# Receiver held-open beacon (draft)

**Draft/working doc.** Design, firmware, and pin choice are all
confirmed working on the breadboard (D0 — see "Pin: D0 / GPIO0" below
for why D6 didn't work and what replaced it). Currently moving to a
permanent build: a small standalone protoboard for just this circuit
(separate from rx's existing stacked radio protoboard) is laid out and
reviewed but **not yet soldered** — see "Protoboard layout" below,
including one real wiring bug caught before it got built. Once soldered
and reconfirmed, fold the relevant parts into `WIRING.md` (or split
into a standalone `rx` wiring doc — there isn't one yet; `WIRING.md`
today only covers the transmitter build) and retire this file.

## What this adds

A small rotating beacon light on the receiver, switched on while the gate
is reported held open, off otherwise. Everything else about rx (radio,
WiFi, the custom PCB work so far — which is tx-only; rx is still the
original stacked-protoboard build) is unchanged by this addition.

## Pin: D0 / GPIO0 (originally tried D6 — didn't work, see below)

**D6/GPIO16 was the original choice** — reasoned at the time that since
rx's `Serial` runs over native USB (no `CDCOnBoot=default` like tx needs
for monitoring after deep sleep), D6/D7 (GPIO16/17) would be genuinely
unused by rx's firmware. **That assumption was wrong, confirmed on the
bench**: with the beacon wired to D6, the MOSFET gate measured a
constant ~3V regardless of door state or anything the firmware wrote to
the pin.

Root cause, confirmed directly against the installed toolchain's own
compiled configuration (`~/.arduino15/packages/esp32/tools/esp32c6-libs/3.3.11/sdkconfig`
on the Linux dev box — the actual sdkconfig this project's `esp32c6`
builds link against, one shared file regardless of a sketch's
`CDCOnBoot` setting), not just a web search: `CONFIG_ESP_CONSOLE_UART=y`,
`CONFIG_ESP_CONSOLE_UART_NUM=0`. ESP-IDF's own internal console/logging
subsystem — used by the ROM bootloader's boot-time messages and by
every prebuilt IDF component's own diagnostic output (WiFi, etc.), not
by the Arduino sketch — is compiled to use UART0 (GPIO16/17) regardless
of what the sketch's `Serial` object does. **This is a toolchain/library
configuration choice baked into the prebuilt libraries Arduino-ESP32
ships for this chip, not a silicon-level limitation** — correcting an
earlier, less precise claim in this doc that GPIO16/17 were somehow
hard-wired at the chip level; the real mechanism is one layer up, in
how this specific SDK build was configured. Arduino's `CDCOnBoot`
setting only changes which object the sketch's own `Serial` variable
points at (native USB vs. this same UART0) — it doesn't touch
`CONFIG_ESP_CONSOLE_UART_NUM`, which is fixed at a lower layer the
Arduino board-manager menu doesn't expose. See ESP-IDF's
[Kconfig reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c6/api-reference/kconfig-reference.html)
for what this setting controls. **D7 is equally suspect for the same
reason** (UART0 RX) — not a safe fallback either; see "Optional:
mode-select from software" below for where this leaves that idea.

**D0/GPIO0 is the switch pin now, confirmed working on the bench.**
Genuinely free of any fixed peripheral claim (confirmed against
ESP-IDF's own GPIO docs, not just assumed this time), and not a
strapping pin on the C6. One real debugging detour on the way to that
confirmation, worth recording since it looked like a hardware problem
and wasn't: after moving the physical wire from D6 to D0 and reflashing
from the macOS flashing box, the gate read a steady ~1.6V and the
beacon never turned on — which happens to match almost exactly what
passive resistor-divider math alone predicts for an *undriven* D0 (the
Wio HAT's 10kΩ pull-up fighting this circuit's own 220Ω+10kΩ pulldown,
with nothing actively driving the node either way). That symptom was
real, but its cause wasn't a wiring fault — the macOS box hadn't pulled
the latest commit, so it was still flashing the old D6-targeting
firmware, which never touched D0 at all, leaving it genuinely
undriven and floating at that computed divider voltage. Re-pulled and
reflashed with the actual D0-targeting build, and it worked
immediately. No hardware fix was needed or made to the D0 circuit
itself — worth remembering next time a symptom looks electrical, since
this one very nearly sent debugging down a continuity-check path for a
connection that was never actually the problem.

It does, however, share a net with the Wio-SX1262 HAT's onboard button
and 10kΩ pull-up (`K1`/`R2` — see `WIRING.md`'s "The Wio-SX1262 HAT's
onboard button shares D0") — worth its own look here, since rx uses
this pin differently than tx does:

- **The sleep-current problem that motivated tx's whole D0 investigation
  doesn't apply here at all.** That was about shaving microamps off a
  battery-powered deep-sleep budget; rx is mains-powered and never
  sleeps, so a parasitic pull-up current measured in µA is completely
  lost in its normal milliamp-scale WiFi/radio draw. Nothing to fix on
  that front.
- **A different, new concern specific to driving D0 as an output,
  though**: tx only ever reads D0 as an input; rx now actively drives it
  (push-pull HIGH or LOW) to switch the MOSFET gate. If someone
  physically presses the Wio HAT's onboard button while the GPIO is
  driving the net HIGH, that's a direct short to GND through the
  button's switch contacts, fighting the GPIO's own output stage — not
  something to rely on the pin tolerating indefinitely. Mitigation:
  don't press that button. It's non-functional/decorative in this
  design regardless (same conclusion `WIRING.md` already reached for
  tx) — worth a strip of tape over it, or the same pin-snipping
  treatment `WIRING.md` documents for tx, if this ever moves off a
  breadboard into something permanent. When the GPIO drives LOW instead,
  R2's pull-up just adds a trivial ~330µA sink load (3.3V/10kΩ) —
  nowhere near any GPIO's sink capability, not a concern.

## Circuit

Low-side switch: the GPIO can't drive the beacon's 5V/current directly,
so it switches a transistor sitting in the beacon's **ground** return
instead — simpler to drive from a 3.3V logic pin than high-side switching
would be, and avoids a GPIO ever seeing the beacon's own voltage or any
motor kickback directly.

```
VBUS (5V) ──────────────────────────────────┬── beacon +5V
                                              │
                                         [beacon]
                                              │
                                         beacon GND ── MOSFET drain
                                                         │
GPIO D0 ──[220Ω]── MOSFET gate                     MOSFET source ── GND
                       │
                     [10kΩ]
                       │
                      GND
```

- **VBUS** for power — the XIAO's raw USB 5V pass-through, not
  current-limited by the board itself. Confirmed no concern: PPK2
  measurement of the actual unit shows ~20mA average, ~25mA peak (see
  "Confirmed on the bench" below) — nowhere near enough to worry about
  the USB adapter's budget.
- **10kΩ pull-down** on the gate: keeps the beacon reliably off during
  the brief window at boot before `pinMode()`/`digitalWrite()` run, since
  GPIO state is indeterminate for a moment right after reset.
- **No flyback diode.** Originally included as cheap insurance against
  the beacon turning out to be motor-driven; dropped once hooked up and
  measured — see "Confirmed on the bench" below for why.
- **Third wire (mode-select)**: left disconnected — confirmed on the
  bench that floating gives the default rotating pattern, which is what
  this design wants. See "Optional: mode-select from software" below
  for how to wire and drive it instead, if a non-default mode is ever
  wanted.

## Parts (through-hole only — no custom PCB for rx)

| Part | Qty | Notes |
|---|---|---|
| IRLZ44N (TO-220, logic-level N-channel MOSFET) | 1 | Vgs(th) ~1-2V, switches well under 3.3V drive — wildly overkill margin for a confirmed ~20-25mA load, but it's the standard, cheap, always-in-stock choice |
| 220Ω resistor | 1 | MOSFET gate series resistor |
| 10kΩ resistor | 1 | gate pull-down |
| 1x3 0.1"/2.54mm male pin header (THT, straight or right-angle per mounting) | 1 | mates directly with the beacon's factory connector — confirmed a standard 1x3 DuPont-style female header, no adapter needed. Populate all 3 positions even though only 2 are wired (see note below) so the connector always seats fully and can't be mis-plugged by one position. |

No flyback diode — see "Confirmed on the bench" below.

## Protoboard layout (in progress — reviewed, not yet soldered)

Small 1"x1", 9x9-hole board, pads grouped in sets of 3 per row (each
row splits into three independent 3-hole islands: columns A-C, D-F,
G-I — no bussing across groups or between rows), with a mounting hole
knocking out 4 pads (I4, H5, I5, I6).

Current plan: two vertical 1x3 headers in columns A (XIAO side: A1
VBUS, A2 GND, A3 D0) and I (beacon side: I1 BEACON_PWM — left floating,
I2 BEACON_5V, I3 BEACON_GND), MOSFET mounted standing on its 3 legs
down column E rows 2-4 (E2 Source, E3 Drain, E4 Gate — verified by
working through the physical rotation for "printed face toward column
I/east," matches twice via independent reasoning, but **still needs a
physical dry-fit against the datasheet pinout before soldering** — this
kind of 3D reasoning from text is exactly the kind of thing worth
distrusting until confirmed with the part in hand), then:

- `C2` (GND island) → `D2` (Source island): direct jumper.
- `B2` (GND island) → 10kΩ → `D4` (Gate island): pulldown. Landing at
  D4 rather than F4 keeps the lead's path inside columns B-D, clear of
  the Drain leg at E3.
- `B3` (D0 island) → 220Ω → `F4` (Gate island): gate series resistor,
  approaching from the opposite side of D4's lead — keeps the two
  gate-side connections naturally separated.
- `F3` (Drain island) → `H3` (BEACON_GND island): jumper.
- `B1` (VBUS island) → `H2` (BEACON_5V island): jumper.

Everything fits inside rows 1-4; no need to route around the back via
row 5 (which is also missing 2 of its 3 right-side holes from the
mounting-hole knockout, a constraint that would have mattered if a
detour through there were ever needed).

One real bug caught and fixed during review: an earlier draft of this
layout had the GND→Source jumper landing on `B3` instead of `C2` — B3
is actually part of row 3's island, which is the **D0** net here, not
GND (GND is row 2, D0 is row 3, per the header assignment above). Built
as originally drafted, that would have wired D0 directly to the
MOSFET's source with no resistance in between, leaving Source
disconnected from true GND entirely — not a cosmetic issue, a real
miswiring that would have kept the switch from working correctly and
put the GPIO pin at risk of sinking the beacon's switched current
directly. Caught by tracing each hole against the stated header order
rather than trusting the layout's own prose labels at face value.

**The header's "GND" position connects to the MOSFET drain, not
straight to the board's GND rail** — that's the switched node (see the
Circuit diagram above). Wiring it directly to GND instead would bypass
the switch entirely and leave the beacon permanently on. Also: wire the
header's +5V/GND positions to match whichever two of the beacon's three
factory-connector pins were confirmed as +5V and GND on the bench —
DuPont pinout order isn't standardized across RC-accessory vendors, so
don't assume a specific position without checking against what was
already tested working.

**Alternative to the MOSFET**: a 2N2222A/PN2222A (TO-92 NPN) with the
220Ω gate resistor swapped for a ~470Ω base resistor — equally fine for
a load this size (confirmed ~20-25mA, nowhere near either part's
limits), and maybe more likely to already be on hand. 470Ω gives
comfortable margin even at a conservative hFE estimate; a higher value
(2.2-4.7kΩ) would work too, if that's what's on hand instead.

## Confirmed on the bench

Beacon hooked up and measured directly with a Nordic PPK2
(`/var/tmp/beacon_current.png`, both a brief pulse and a multi-second
run — pattern didn't change):

- **~20mA average, ~25mA peak, no inrush at turn-on.** A motor's
  starting/stall current would show up as a sharp spike several times
  the running current right at power-on; instead there's a ~250ms
  trough at ~7mA before it settles to ~20mA — consistent with a driver
  IC's own power-on sequencing, not a motor spinning up. The steady-state
  waveform itself (small step-like dips and spikes, no ringing or decay
  curves) matches a ring of LEDs being switched in different
  combinations to fake rotation, not an inductive load.
- **No flyback diode needed** — there's no inductive element to protect
  against. Removed from the design on this basis (see above), not just
  deprioritized.
- **VBUS headroom is a non-issue** at this current — no further check
  needed there.

## Bench-test checklist before finalizing

- [x] Measure the beacon's actual current draw — ~20mA avg / ~25mA peak,
      see "Confirmed on the bench" above.
- [x] Confirm whether the beacon is motor-driven or LED-only —
      confirmed LED-only (strobed ring, no motor) both visually and via
      the current waveform shape.
- [x] Power the beacon with +5V/GND only, signal wire floating — does it
      run its default pattern? Confirmed yes: floating gives the
      rotating-beacon pattern. So the core design needs no connection to
      the signal wire at all — see "Optional: mode-select from software"
      below for the separate, optional addition if more than the default
      mode is ever wanted.
- [x] Decide the connector approach once the beacon's factory harness is
      in hand — confirmed standard 1x3 2.54mm DuPont-style female
      header. Matching 1x3 male header on the rx side, see the Parts
      table above.
- [x] Confirm D0 actually fixes the stuck-on symptom — confirmed working
      on the bench (one detour along the way where a stale flashing-box
      checkout, not a hardware fault, briefly looked like a second pin
      problem — see "Pin: D0 / GPIO0" above for the full story).
- [ ] Solder the protoboard per "Protoboard layout" above, dry-fitting
      the MOSFET against the datasheet pinout first, then re-confirm
      the beacon still switches correctly on the permanent build (not
      just on the breadboard prototype).

## Optional: mode-select from software

Not needed for the core design (confirmed above — floating gives the
default rotating pattern already) and not planned for now, but worth
sketching since it's genuinely easy if ever wanted.

**Pin: not actually settled.** This section originally suggested
D7/GPIO17 as "the other pin this doc set aside," reasoning that mirrored
the (wrong) D6 assumption above — D7 is UART0 RX, the same fixed
peripheral claim that ruled out D6, so it's equally suspect and
shouldn't be trusted without the same kind of bench confirmation that
caught D6. With D0 now spoken for by the on/off switch, there isn't an
obviously-free pin left on the standard 14-pin header for this — would
need fresh investigation if this is ever actually pursued, not a
simple "use the next one over" the way this section originally assumed.
The wiring/signal/firmware shape below is still the right *approach*,
just pending a real pin to attach it to.

**Wiring** (pin TBD — see above): beacon's signal wire straight to the
chosen GPIO, no transistor involved this time — unlike the beacon's own
+5V/GND, this is just a logic-level signal into what's almost certainly
a microcontroller input on the beacon's own driver board, not something
that needs power-level switching. A small series resistor (220-470Ω)
between the GPIO and the signal wire is cheap insurance, same reasoning
as the gate resistor elsewhere in this doc, but isn't expected to be
load-bearing.

**Signal**: these inputs are standard RC servo/ESC-style PWM — 50Hz
(20ms period), pulse width somewhere in the 1000-2000µs range selecting
a mode. There's no published mapping for this specific unit (these
aren't standardized beyond "50Hz, roughly 1-2ms"), so it'd take an
empirical sweep: hold each pulse width for a second or two across that
range in ~50-100µs steps and log what pattern plays, to build a mode
table by observation.

**Firmware**: the ESP32-C6's LEDC peripheral generates this in hardware
once configured — no per-loop bit-banging needed, it keeps outputting
the waveform on its own until told to change:

```cpp
#define BEACON_MODE_PIN      /* TBD — see "Pin: not actually settled" above */
#define BEACON_PWM_FREQ_HZ   50
#define BEACON_PWM_BITS      16     // plenty of duty-cycle resolution

void setup() {
  ledcAttach(BEACON_MODE_PIN, BEACON_PWM_FREQ_HZ, BEACON_PWM_BITS);
  set_beacon_mode_us(1500);  // pick a starting pulse width once mapped
}

// pulse_us: the pulse width (microseconds) to hold, out of a 20ms
// (50Hz) period — this is a "set and leave running" call, not
// something to call every loop() iteration.
void set_beacon_mode_us(uint32_t pulse_us) {
  const uint32_t period_us = 1000000UL / BEACON_PWM_FREQ_HZ;
  const uint32_t max_duty = (1UL << BEACON_PWM_BITS) - 1;
  uint32_t duty = (uint64_t)pulse_us * max_duty / period_us;
  ledcWrite(BEACON_MODE_PIN, duty);
}
```

(`ledcAttach`/`ledcWrite` is the current Arduino-ESP32 core 3.x API —
older examples online use `ledcSetup`/`ledcAttachPin`, which this core
no longer has.) This runs entirely independent of the on/off power
switch on D0 — the PWM signal can stay configured and running
regardless of whether the beacon's own power is actually on; it just
does nothing while unpowered.

## Firmware status

Written, flashed, and confirmed working on D0 (`rx.ino`'s
`beacon_should_be_on()`/`update_beacon()`, piggybacking on the existing
`door_is_open`/`short_open_alerted`/`PKT_DOOR_HELD_OPEN` tracking rather
than adding new state) — see that file's own comments for the
structure. Still deliberately in its testing-phase behavior (lit for
the entire time the door's open, per `beacon_should_be_on()`'s current
`return door_is_open;`), not yet switched to the intended "only after
the 45s/300s warning" behavior — that's a one-line change whenever
wanted, per the design note in that function's own comment.
