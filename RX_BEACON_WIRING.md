# Receiver held-open beacon (draft)

**Draft/working doc — not yet bench-tested or built.** Once the design's
confirmed against the real beacon hardware, fold the relevant parts into
`WIRING.md` (or split into a standalone `rx` wiring doc — there isn't one
yet; `WIRING.md` today only covers the transmitter build) and retire this
file.

## What this adds

A small rotating beacon light on the receiver, switched on while the gate
is reported held open, off otherwise. Everything else about rx (radio,
WiFi, the custom PCB work so far — which is tx-only; rx is still the
original stacked-protoboard build) is unchanged by this addition.

## Pin: D6 / GPIO16

rx's `Serial` runs over native USB (no `CDCOnBoot=default` like tx needs
for monitoring after deep sleep, since rx never sleeps), so D6/D7
(GPIO16/17) are genuinely unused by rx's firmware today — unlike tx,
where the new PCB commits them to a UART breakout header. Using D6
instead of D0 also sidesteps the Wio-SX1262 HAT's onboard-button-shares-
D0 issue entirely (see `WIRING.md`'s "The Wio-SX1262 HAT's onboard button
shares D0"): rx never uses D0 for anything, so there's nothing to bypass
— the stack can stay exactly as it came from the manufacturer, no pin
snipping or bridge wire needed. D7 stays free for whatever comes up next.

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
GPIO D6 ──[220Ω]── MOSFET gate                     MOSFET source ── GND
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
- **Third wire (mode-select)**: not wired up yet. These are typically a
  standard RC servo-style PWM input (~50Hz, pulse width in the 1-2ms
  range selecting a mode) — easy to generate from the C6's hardware PWM
  (`ledcAttach()`/`ledcWrite()`) if wanted later, but needs its own GPIO,
  separate from the on/off switch above, since it'd need to be driven
  continuously rather than just held high/low. See the bench-test
  checklist below for what to confirm first.

## Parts (through-hole only — no custom PCB for rx)

| Part | Qty | Notes |
|---|---|---|
| IRLZ44N (TO-220, logic-level N-channel MOSFET) | 1 | Vgs(th) ~1-2V, switches well under 3.3V drive — wildly overkill margin for a confirmed ~20-25mA load, but it's the standard, cheap, always-in-stock choice |
| 220Ω resistor | 1 | MOSFET gate series resistor |
| 10kΩ resistor | 1 | gate pull-down |
| 3-pin 0.1" header or JST-XH (THT), or solder direct | 1 | optional — pick based on whatever connector's already on the beacon's factory harness |

No flyback diode — see "Confirmed on the bench" below.

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
- [ ] Power the beacon with +5V/GND only, signal wire floating — does it
      run its default pattern? (Sounds like this was already effectively
      answered by the measurement run — confirm explicitly if not.)
- [ ] If not, try tying signal to GND next — common default-safe idle
      state for this class of module.
- [ ] If *neither* works, the beacon needs an active PWM signal just to
      function, not merely to pick a mode — revisit the design before
      wiring anything permanently.
- [ ] Decide the connector approach once the beacon's factory harness is
      in hand.

## Not yet done: firmware

`rx.ino` already tracks held-open state (`door_is_open`,
`short_open_alerted`, and the `PKT_DOOR_HELD_OPEN` handler) — driving
the new GPIO would piggyback on that existing logic, not add new state.
Deliberately left for a follow-up once the hardware above is built and
confirmed working on the bench.
