# loragate

A battery-powered door/gate sensor that reports open, close, and
"still open" events over LoRa to an always-on receiver, which forwards
them to a WiFi-connected monitoring server.

## Requirements

- Multi-year battery life on primary AA cells, outdoors, unattended —
  no solar, no mains run to the gate.
- Never silently miss a door state change, even across a reset.
- Usable range through walls/trees between the gate and the house
  (no WiFi or cellular at the gate).
- No recurring cost (no cellular plan, no cloud radio service).
- Some visibility into the transmitter's own health (is it still
  alive, did it reset unexpectedly) without needing a visit to check.

## Implementation pieces

- **`tx/`** — the outdoor transmitter: XIAO ESP32-C6 + Wio-SX1262,
  reed switch, 3x AA lithium cells. Deep-sleeps between events.
- **`rx/`** — the indoor receiver: same radio hardware, USB-powered,
  always on, WiFi-connected. Acks `tx`, forwards events to a
  monitoring server over HTTP.
- **`test/`** — host-side tests (no ESP32 toolchain needed) for the
  parts of `tx`/`rx` written to be testable without hardware: the wire
  format, `tx`'s wake-decision logic, and `rx`'s retry-dedup logic.

## Why these choices

**LoRa, not WiFi or cellular, for the transmitter.** WiFi's join/auth
cost alone is worse than LoRa's entire transmit burst, and there's no
cellular plan to pay for recurring. LoRa trades bandwidth (a few bytes
a message) for exactly what this needs: long range at near-zero duty
cycle, license-free. The receiver *is* WiFi-connected, but it's
mains-powered — it absorbs that cost so the battery-powered side
doesn't have to.

**XIAO ESP32-C6, deep-sleep by default, event-driven wake.** Polling
the reed switch on a timer would mean staying awake far more than the
door actually changes state. Instead, `tx` deep-sleeps and wakes only
on a reed-switch edge or a backstop RTC timer (see "Software" below) —
the C6's RTC domain is what makes both of those possible as a true
low-power wake source, and `RTC_NOINIT_ATTR` memory is what lets a few
bytes of state (sequence counter, last-reported door state) survive
both sleep and an unexpected restart without re-deriving them from
scratch every wake.

**Primary AA lithium (Energizer L91), not rechargeable Li-ion, through
an LDO.** A sealed outdoor enclosure in direct sun runs hotter than
ambient, and that's exactly where Li-ion/LiPo capacity fade and
pouch-cell failure modes get worse — the alarm industry ships
door/window sensors on primary lithium for the same reason. The
XIAO's `3V3` pin is wired straight to the chip's core supply with no
protection, and a fresh pack can sit close to the chip's own voltage
ceiling with no margin — an LDO (plus a PTC fuse ahead of it) fixes
both the regulation and the short-circuit exposure in one part. See
WIRING.md's "Power" for the specific part and the alternative
considered.

**Simple ack/retry over the radio link, not a heavier protocol.** Each
event is one small packet (type + sequence number + retry count);
`tx` retries with backoff until `rx` acks it or it gives up. That's
enough reliability for "a handful of events a day, on a link that's
usually clean," without the complexity of a synchronized/meshed
protocol this traffic pattern doesn't need.

**The wake/sleep decision logic is pure C++, separate from the
Arduino/radio code.** `tx/door_wake_logic.h` decides *what a wake
means and should do* with no Arduino, radio, or Serial dependency;
`tx/tx.ino` is just the I/O shell that reads the pin and carries out
the decision. That split is what lets `test/test_tx_wake_logic.cpp`
exercise the actual decision rules — including regression cases for
real bugs caught along the way (see "History" below) — with a plain
C++ compiler, no hardware required.

### Power budget

Deep-sleep current, measured live on the built transmitter with a
Nordic PPK2 (see "History" below for how the first measurement — 1.5mA,
not microamps — got root-caused):

| Source | Current |
|---|---|
| Deep sleep, door closed | **21.9µA** |
| Deep sleep, door open | **18.5µA** |
| Active transmit (~120ms burst, +22dBm) | **~125mA** |

Budgeting conservatively for ~54 radio events/day (20 door
transitions + an hourly heartbeat floor + occasional held-open
checks — see "Software" below) against ~2,100mAh realistically
extractable from a 3x AA L91 pack:

```
sleep:  0.0219mA * 24h                    ≈ 0.526 mAh/day
active: 54 * 0.120s * 125mA / 3600        ≈ 0.225 mAh/day
total                                     ≈ 0.751 mAh/day
2,100 mAh / (0.751 mAh/day * 365)         ≈ 7.7 years
```

Comfortably past the "2+ years would be satisfying" bar, with enough
margin to absorb temperature, pack-tolerance, and retry-rate
uncertainty the measurement doesn't cover. Not modeled: a worst case
of constant ack-retries would cost more than the single-burst-per-event
assumption above — live field logs show this is rare in practice, but
it's not measured.

### History

- The project's first draft (an AI-authored plan, not hardware-checked)
  got real things wrong: a reed-switch pin that collided with the
  radio's own NSS pin, and a direct battery-to-`3V3` connection with no
  margin against the chip's voltage ceiling. Both forced real design
  changes — see WIRING.md's pin table and LDO section.
- The transmit-current figure was off by more than 3x for a while
  (~35mA assumed vs. ~120-125mA actual) until it was checked against
  the SX1262 and Wio-SX1262 module datasheets directly.
- Deep-sleep current measured ~1.5mA on the first PPK2 pass — ~40x the
  budget. Root cause took two parts: the SX1262 wakes itself from
  SLEEP on an NSS falling edge, and the ESP32-C6 doesn't hold a GPIO's
  level across sleep unless told to (fixed with `gpio_hold_en()`); and
  the Wio-SX1262 HAT's own onboard button shares the reed switch's
  `D0` net with a pull-up that drew current whenever the door was
  closed (confirmed against Seeed's schematic, fixed by bypassing `D0`
  around the HAT in hardware — see WIRING.md). Final measured number
  is the 21.9µA/18.5µA above.
- A `while (!Serial)` wait-for-USB-monitor loop in `tx`'s `setup()`
  burned up to 3 seconds of fully-awake CPU time on every single wake
  in the field — a real battery-life cost, not just a latency
  annoyance — because deep sleep drops the USB peripheral and nothing
  was ever there to satisfy the wait. Fixed by gating it behind
  `-DDEBUG`.

## Hardware

Both nodes use the same radio hardware — a Seeed XIAO ESP32-C6 stacked
with a Seeed Wio-SX1262 (the "for XIAO ESP32S3" through-hole-header
variant; Seeed doesn't sell this exact pairing, so the pinout is
confirmed against the physical boards, not a datasheet — see
WIRING.md). What differs is what's attached and how each is powered:

- **Transmitter**: reed switch, 3x AA lithium pack through an LDO and
  a PTC fuse, in an IP67 enclosure with an external antenna. No USB
  power in the field.
- **Receiver**: USB-powered, indoors, no battery or enclosure
  concerns — it just needs to stay on and stay on WiFi.

A 3D-printable mount for the stacked boards is in `mount/`
(`mount.scad`/`mount.stl`).

This section covers what the pieces are and why; **WIRING.md has the
full pin tables, part numbers/links, and physical build/wiring
instructions** — that's the doc to open to actually order parts or
wire up a unit.

### History

- The Wio-SX1262's pinout for this XIAO had to be read off the boards'
  silkscreen — an earlier schematic-derived guess was off by one
  header position and failed radio init outright
  (`RADIOLIB_ERR_CHIP_NOT_FOUND`).
- The transmitter's first LDO pick (TI TPS7A0233, true nanopower) had
  a dead bench unit (0V output, bad part or solder joint, not a design
  issue); the MCP1700 — higher quiescent current, but simpler to build
  and confirmed working — is what's actually installed. TPS7A0233
  remains a documented alternative if a working unit gets built later.
  See WIRING.md's "Choosing an LDO".
- The Wio-SX1262 HAT's own onboard button turned out to share the
  reed switch's `D0` net (see "Power budget" → "History" above) — a
  hardware quirk of the stacked-header build, not something visible
  from either board's datasheet alone.

## Software

### Transmitter (`tx/`)

Deep sleeps by default. Each wake runs `setup()` once (there's no
persistent `loop()`), decides what — if anything — to send, and goes
back to sleep from inside that decision:

- Wakes on a reed-switch edge, or an RTC timer backstop that's always
  armed (a short one while an open episode hasn't yet been
  re-verified, a long hourly one otherwise).
- Every wake diffs the real pin state against a persisted "last
  reported" baseline — whether something changed is never inferred
  from *why* the chip woke up, only from that diff. A real edge that
  settles back to the same state (reed-switch bounce/chatter outdoors)
  is told apart from a genuinely elapsed timer and sent nothing.
- A door transition is reported immediately; a door left open past a
  threshold gets one follow-up "still open" check (not a repeating
  one); anything else falls back to an hourly heartbeat floor so
  silence never goes on indefinitely.
- The handful of values that need to survive both sleep *and* a
  restart (sequence counter, last-reported door state, a pending
  reset-diagnostic flag) live in `RTC_NOINIT_ATTR` memory, validated
  by a checksum (`tx/rtc_checksum.h`) since that memory section isn't
  zeroed by the bootloader the way `RTC_DATA_ATTR` would be.
- An abnormal boot/reset reason (anything other than a routine
  deep-sleep wake or the transmitter's own restart) gets reported as a
  diagnostic packet, detected before the radio is even brought up so
  it isn't lost if `radio.begin()` happens to fail on that particular
  wake.
- The wake-decision rules themselves live in `tx/door_wake_logic.h` —
  see "Why these choices" above for why that's a separate, pure file.

### Receiver (`rx/`)

Always on, always listening:

- Acks every recognized packet (so `tx` stops retrying) and dedups by
  sequence number before acting on it, since an ack-loss retry resends
  the same event.
- Forwards door open/close/heartbeat/held-open/diagnostic events to a
  monitoring server over HTTP — URLs and WiFi credentials live in
  `rx/local.h` (gitignored; copy `rx/local.h.example`).
- Runs its own lightweight checks independent of what the transmitter
  decides to send: a faster local "door open past 45s" alert, a
  "haven't heard from the transmitter in a while" staleness alert, and
  proactive WiFi reconnect if the connection drops.

### Wire protocol

One packet type covers acks (`PKT_ACK`); the rest cover transmitter
events (`PKT_HEARTBEAT`, `PKT_DOOR_OPEN`, `PKT_DOOR_CLOSE`,
`PKT_DOOR_HELD_OPEN`, `PKT_DIAGNOSTIC`) — see `lora_packet.h` (kept
byte-identical between `tx/` and `rx/`, checked by `make test`) for
the exact struct. `tx` sends, waits for a matching ack, and retries
with backoff (capped attempts) before giving up on that one event.

### Build, test, and flash

Toolchain: [`arduino-cli`](https://arduino.github.io/arduino-cli/) with
the ESP32 board package, plus the RadioLib library:

```sh
arduino-cli lib install RadioLib
```

From the repo root:

- `make compile` — compiles `tx/` and `rx/` against the installed
  ESP32 core + RadioLib. No upload, no hardware needed.
- `make test` — host-side tests (`test/`), no ESP32 toolchain needed
  at all, just a C++ compiler. Covers the wire format and the pure
  logic described above; **does not** reach actual hardware behavior
  (real deep sleep, a real reed-switch edge, radio range, WiFi/HTTP) —
  only flashing a real board confirms those.
- `make flash-tx` / `make flash-rx` — flash `tx`/`rx`.

Each sketch directory also has its own `Makefile` (`make flash`,
`make debug`, `make monitor`, `make clean`). `make debug` substitutes
a `delay()`+restart loop for deep sleep so USB/serial stays up for
monitoring, and builds with `-DDEBUG` (enables verbose logging and,
on `tx`, the wait-for-monitor loop that production builds skip — see
"History" below).

`rx/` needs `rx/local.h` (gitignored) for WiFi credentials and the
HTTP URL templates it forwards events to — copy `rx/local.h.example`
and fill it in. `tx/` needs no `local.h` (no WiFi).

### History

- The transmitter's wakeup source was originally planned as
  `esp_sleep_enable_ext0_wakeup()`; the ESP32-C6 doesn't implement
  ext0 at all. `esp_sleep_enable_ext1_wakeup()` (a GPIO bitmask, not a
  pin number) replaced it.
- A live brownout-looking symptom (rapid door toggling occasionally
  dropped an event and reset the sequence counter to 0) turned out to
  be a real persistence bug, not a hardware brownout: `RTC_DATA_ATTR`
  is only documented to survive deep sleep, not `ESP.restart()`
  (the rapid-slam/bounce guard's own restart) — switching to
  `RTC_NOINIT_ATTR` plus a checksum fixed it.
- A reed-switch edge that bounces back to the same debounced state
  (wind/vibration on an outdoor gate) was originally indistinguishable
  from a genuinely elapsed timer, so it could burn an unneeded
  heartbeat or consume the one-shot held-open check's budget early.
  Fixed by checking the actual wakeup cause, but only in the
  already-no-transition branches.
- If `radio.begin()` ever failed on the very first wake after a true
  power-on, a later successful wake could silently adopt whatever the
  door happened to be doing *then* as the starting baseline — losing
  any real transition that happened in between with no event ever
  sent. Fixed by establishing the baseline from the pin alone, before
  the radio is even touched.
- The held-open check originally re-armed every interval for as long
  as the door stayed open (~280 wakes/day for a day-long open door);
  made one-shot per open episode instead.
- An ack-loss retry (receiver got and acked the original fine, but the
  ack itself got lost) was double-notifying the monitoring server for
  the same event; fixed by dedup-by-sequence-number on the receiver
  before acting on a packet, while still acking unconditionally.
- NTP sync and a reported `localtime=` field were added, then removed:
  the actual timing gap they were meant to close (radio-level latency)
  is usually under a second and gets swamped by the monitoring
  channel's own latency anyway — not worth the added wire-format and
  firmware complexity.
