# Receiver held-open beacon (draft)

**Draft/working doc.** Design, firmware, and pin (D0) are confirmed
working on the breadboard. A small standalone protoboard for this
circuit (separate from rx's stacked radio protoboard) is laid out and
reviewed but **not yet soldered**. Once it's built and reconfirmed, fold
this into `WIRING.md` (or a new rx wiring doc — `WIRING.md` today only
covers the transmitter) and retire this file.

## What this adds

A small rotating beacon light on the receiver, on while the gate is
reported held open, off otherwise. Nothing else about rx changes (rx is
still the original stacked-protoboard build; the custom PCB is tx-only).

## Pin: D0 / GPIO0

D0 is free of any fixed peripheral claim (per ESP-IDF's GPIO docs) and
isn't a C6 strapping pin. D6/D7 (GPIO16/17) look free but aren't: they're
UART0, and the installed core's prebuilt ESP-IDF is configured with its
console on UART0 (`CONFIG_ESP_CONSOLE_UART_NUM=0` in
`~/.arduino15/packages/esp32/tools/esp32c6-libs/3.3.11/sdkconfig`),
regardless of the sketch's `CDCOnBoot` setting. That's an SDK
configuration choice, not a silicon limit, but the Arduino board menu
doesn't expose it. See "History" below for how this was found.

D0 shares a net with the Wio-SX1262 HAT's onboard button and 10kΩ
pull-up (`K1`/`R2` — see `WIRING.md`'s "The Wio-SX1262 HAT's onboard
button shares D0"). Unlike tx, rx *drives* this pin:

- **No sleep-current concern.** That was tx's problem; rx is
  mains-powered and never sleeps, so µA of pull-up current doesn't
  matter.
- **Don't press the HAT's button.** While the GPIO drives HIGH, pressing
  it shorts the output to GND. The button does nothing in this design —
  tape over it, or snip it the way `WIRING.md` does for tx. Driving LOW
  against R2 is only a ~330µA sink load, which is fine.

## Circuit

Low-side switch: the GPIO can't drive the beacon's 5V/current directly,
so it switches a MOSFET in the beacon's **ground** return — easy to
drive from a 3.3V pin, and the GPIO never sees the beacon's own voltage.

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

- **VBUS** for power — the XIAO's raw USB 5V pass-through. The beacon
  draws ~20mA avg / ~25mA peak, well within a USB adapter's budget.
- **10kΩ pull-down** on the gate keeps the beacon off during the brief
  window after reset before `pinMode()`/`digitalWrite()` run.
- **No flyback diode** — the beacon is LED-only, no inductive load (see
  "Confirmed on the bench").
- **Third wire (mode-select)** left disconnected: floating gives the
  default rotating pattern, which is what this design wants. See
  "Optional: mode-select from software" for driving it instead.

## Parts (through-hole only — no custom PCB for rx)

| Part | Qty | Notes |
|---|---|---|
| IRLZ44N (TO-220, logic-level N-channel MOSFET) | 1 | Vgs(th) ~1-2V, switches well under 3.3V drive — overkill for ~20-25mA, but standard, cheap, always in stock |
| 220Ω resistor | 1 | MOSFET gate series resistor |
| 10kΩ resistor | 1 | gate pull-down |
| 1x3 0.1"/2.54mm male pin header (THT, straight or right-angle per mounting) | 1 | mates directly with the beacon's factory connector (a standard 1x3 DuPont-style female header). Populate all 3 positions even though only 2 are wired, so the connector seats fully and can't be mis-plugged by one position. |

**Alternative to the MOSFET**: a 2N2222A/PN2222A (TO-92 NPN), with the
220Ω gate resistor swapped for a ~470Ω base resistor — fine for a load
this size, and maybe more likely to be on hand. 470Ω leaves comfortable
margin at a conservative hFE; 2.2-4.7kΩ would work too.

## Protoboard layout (reviewed, not yet soldered)

Small 1"x1", 9x9-hole board, pads grouped in sets of 3 per row (each
row splits into three independent 3-hole islands: columns A-C, D-F,
G-I — no bussing across groups or between rows), with a mounting hole
knocking out 4 pads (I4, H5, I5, I6).

Two vertical 1x3 headers: column A (XIAO side: A1 VBUS, A2 GND, A3 D0)
and column I (beacon side: I1 BEACON_PWM — left floating, I2 BEACON_5V,
I3 BEACON_GND). MOSFET standing on its 3 legs down column E rows 2-4
(E2 Source, E3 Drain, E4 Gate, printed face toward column I/east).
**Dry-fit the MOSFET against the datasheet pinout before soldering** —
the orientation was worked out on paper, not with the part in hand.

- `C2` (GND island) → `D2` (Source island): direct jumper.
- `B2` (GND island) → 10kΩ → `D4` (Gate island): pulldown. D4 rather
  than F4 keeps the lead inside columns B-D, clear of the Drain leg at
  E3.
- `B3` (D0 island) → 220Ω → `F4` (Gate island): gate series resistor,
  approaching from the opposite side of D4's lead.
- `F3` (Drain island) → `H3` (BEACON_GND island): jumper.
- `B1` (VBUS island) → `H2` (BEACON_5V island): jumper.

Everything fits in rows 1-4; no routing via row 5 (which is missing 2
of its 3 right-side holes to the mounting-hole knockout anyway).

**The header's beacon "GND" position goes to the MOSFET drain, not the
board's GND rail** — that's the switched node. Wired straight to GND,
the beacon would be permanently on. Also match the header's +5V/GND
positions to the beacon pins confirmed on the bench: DuPont pin order
isn't standardized across RC-accessory vendors.

## Confirmed on the bench

Measured with a Nordic PPK2 (`/var/tmp/beacon_current.png`, a brief
pulse and a multi-second run — same pattern):

- **~20mA average, ~25mA peak, no inrush.** A ~250ms ~7mA trough at
  turn-on (driver IC power-on sequencing), then a step-like waveform with
  no ringing or decay — an LED ring strobed to fake rotation, not a
  motor. Visually confirmed LED-only too.
- So: no flyback diode needed, and no VBUS headroom concern.

## Bench-test checklist before finalizing

- [x] Measure current draw — ~20mA avg / ~25mA peak.
- [x] Motor-driven or LED-only — LED-only.
- [x] Signal wire floating gives the default rotating pattern — yes.
- [x] Connector — standard 1x3 2.54mm DuPont-style; matching male
      header on the rx side.
- [x] D0 switches the beacon correctly — yes.
- [ ] Solder the protoboard per "Protoboard layout" (dry-fit the MOSFET
      first), then re-confirm the beacon switches on the permanent
      build.

## Optional: mode-select from software

Not needed (floating gives the default pattern) and not planned, but
easy if ever wanted.

**Pin: not settled.** D7 is UART0 RX — ruled out for the same reason as
D6 — and D0 is taken by the on/off switch, so there's no obviously free
pin left on the 14-pin header. Needs fresh investigation if pursued; the
approach below still holds.

**Wiring**: beacon signal wire straight to the GPIO — it's a logic-level
input on the beacon's driver board, no transistor needed. A 220-470Ω
series resistor is cheap insurance.

**Signal**: standard RC servo/ESC-style PWM — 50Hz (20ms period), a
1000-2000µs pulse width selecting the mode. There's no published
mapping for this unit, so build a mode table empirically: hold each
pulse width for a second or two across that range in ~50-100µs steps
and note what pattern plays.

**Firmware**: the ESP32-C6's LEDC peripheral generates this in
hardware once configured:

```cpp
#define BEACON_MODE_PIN      /* TBD — see "Pin: not settled" above */
#define BEACON_PWM_FREQ_HZ   50
#define BEACON_PWM_BITS      16     // plenty of duty-cycle resolution

void setup() {
  ledcAttach(BEACON_MODE_PIN, BEACON_PWM_FREQ_HZ, BEACON_PWM_BITS);
  set_beacon_mode_us(1500);  // pick a starting pulse width once mapped
}

// pulse_us: the pulse width (microseconds) to hold, out of a 20ms
// (50Hz) period — set once and left running, not called every loop().
void set_beacon_mode_us(uint32_t pulse_us) {
  const uint32_t period_us = 1000000UL / BEACON_PWM_FREQ_HZ;
  const uint32_t max_duty = (1UL << BEACON_PWM_BITS) - 1;
  uint32_t duty = (uint64_t)pulse_us * max_duty / period_us;
  ledcWrite(BEACON_MODE_PIN, duty);
}
```

(`ledcAttach`/`ledcWrite` is the Arduino-ESP32 core 3.x API — older
examples use `ledcSetup`/`ledcAttachPin`, which this core no longer
has.) This is independent of the D0 power switch; the PWM can keep
running while the beacon is unpowered.

## Firmware status

Done and confirmed working on D0: `rx.ino`'s `beacon_should_be_on()` /
`update_beacon()`, built on the existing door-state tracking rather than
new state. Still in testing-phase behavior (lit the whole time the
door's open); switching to "lit only after the 45s/300s warning" is a
one-line change in `beacon_should_be_on()`.

## History

- **D6 was the original pin, and didn't work.** The reasoning was that
  rx's `Serial` runs over native USB, so UART0's D6/D7 would be unused.
  On the bench the MOSFET gate sat at a constant ~3V regardless of what
  the firmware wrote. The root cause (ESP-IDF's console compiled onto
  UART0 — see "Pin: D0 / GPIO0") was confirmed against the installed
  sdkconfig rather than a web search. An earlier version of this doc
  wrongly called GPIO16/17 hard-wired at the silicon level.
- **A stale flashing-box checkout briefly looked like a D0 wiring
  fault.** After moving the wire to D0 and reflashing, the gate read a
  steady ~1.6V and the beacon stayed off — almost exactly the
  resistor-divider voltage for an *undriven* D0 (the HAT's 10kΩ pull-up
  against this circuit's 220Ω+10kΩ pulldown). The macOS box hadn't
  pulled the D0 commit and was still flashing the D6 build. After a
  pull and reflash it worked immediately; no hardware change was made.
  Check `git pull` before chasing an electrical-looking symptom.
- **A protoboard layout bug was caught in review, before soldering.** An
  earlier draft landed the GND→Source jumper on `B3` instead of `C2`.
  B3 is in row 3's island — the D0 net — so it would have tied D0
  straight to the MOSFET source and left Source off true GND, risking
  the GPIO sinking the beacon's current. Found by tracing each hole
  against the header assignment rather than trusting the layout's prose
  labels.
- **The flyback diode was dropped after measurement.** It was included
  as insurance in case the beacon was motor-driven; the PPK2 waveform
  showed it's LED-only.
- **The mode-select section originally suggested D7**, by the same
  wrong reasoning as D6.
