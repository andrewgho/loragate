# loragate - outdoor LoRA radio gate sensor

A battery-powered outdoor door/gate sensor that reports open, close, and
"still open" events over LoRa to an always-on receiver, which forwards
them to a WiFi-connected monitoring server.

## Requirements

- Report door open and close events at medium range, through walls and trees.
- Never miss a door state change, even across resets or transient failures.
- Multi-year battery life on primary lithium cells in a warm outdoor climate.

## Implementation pieces

- **[`tx`](tx/)** — outdoor transmitter: XIAO ESP32-C6 + Wio-SX1262,
  reed switch, 3x AA lithium cells. Deep-sleeps between events.
- **[`rx`](rx/)** — indoor receiver: same radio hardware, but USB-powered,
  always on, and WiFi connected. Listens for and acks transmissions from `tx`,
  forwards events to a monitoring server over HTTP, and lights an LED beacon.
- **[`test/`](test/)** — host-side tests (no ESP32 toolchain needed) for the
  parts of `tx`/`rx` written to be testable without hardware: the wire
  format, `tx`'s wake-decision logic, and `rx`'s retry-dedup logic.

## Why these choices

**LoRa for the transmitter.** WiFi and BLE are too expensive power wise.
Z-wave or Zigbee was empirically proven to not be able to transmit through
walls and low-E window coatings in my particular home setting. LoRA radio
has super low power requirements and has excellent range for my use case.

**XIAO ESP32-C6, deep-sleep by default, event-driven wake.** The outdoor
unit is battery powered, with the goal of 2+ years of battery life. The `tx`
unit deep-sleeps and wakes only on a reed-switch edge change or a backstop
RTC timer (see "Software" below), using the C6's RTC domain and `RTC_NOINIT_ATTR` memory to store state between wakes.

**Primary AA lithium cells through an LDO.** The outdoor enclosure is sealed
in a northern Californian climate which can get warm (up to 90°F ambient),
so Li-ion/LiPo rechargeable cells were rejected for safety reasons (spicy
battery). Consumer-available primary AA lithium cells (Energizer L91 or
equivalent) are rated up to 140°F and have inbuilt protections. An LDO with
low quiescent power draw and a PTC fuse protect against voltage fluctuation
over time and short circuits, respectively. See the Transmitter "Power"
section of [`WIRING.md`](WIRING.md) for details and alternatives considered.

**Simple ack/retry over the radio link.** For maximum simplicity, this is a
point to point LoRA protocol, where each event is one small packet (type +
sequence number + retry count); `tx` retries with backoff until `rx` acks it
or it gives up. The radio link is empirically clean so we do not need the
complexity of a meshed protocol.

**Wake/sleep decision logic is pure C++, separate from Arduino/radio code.**
`tx/door_wake_logic.h` decides what a wake means and what should do with no
Arduino, radio, or Serial dependencies. `tx/tx.ino` is just the I/O shell that
reads the pin and carries out the decision. That split lets us run unit tests
in `test/test_tx_wake_logic.cpp` to exercise the actual decision rules,
including regression cases for real bugs caught along the way (see "History"
below) with a plain C++ compiler, no hardware required.

### Power budget

The overall goal for the battery-powered transmitter is 2+ years between
battery changes. With the chosen components, the deep-sleep current was
measured on a Nordic PPK2 is as follows:

| Source | Current |
|---|---|
| Deep sleep, door closed | **21.9µA** |
| Deep sleep, door open | **18.5µA** |
| Active transmit (~120ms burst, +22dBm) | **~125mA** |

Budgeting conservatively for ~54 radio events/day (20 door transitions, an hourly heartbeat, and occasional held-open checks, see "Software" below)
against ~2,100mAh realistically extractable from a 3x AA L91 pack:

```
sleep:  0.0219mA * 24h             ≈ 0.526 mAh/day
active: 54 * 0.120s * 125mA / 3600 ≈ 0.225 mAh/day
total                              ≈ 0.751 mAh/day
2,100 mAh / (0.751 mAh/day * 365)  ≈ 7.7 years
```

In real life, temperature, battery degradation, and other factors will likely
bring the battery life down from the calculated figure, but we should be
comfortably over the "2+ years" bar.

### History

This and other History sections are AI authored, and contain the lessons
learned during iteration, with the hope of not repeating any false starts.

- The project's first draft (an AI-authored plan, not hardware-checked)
  got real things wrong: a reed-switch pin that collided with the
  radio's own NSS pin, and a direct battery-to-`3V3` connection with no
  margin against the chip's voltage ceiling. Both forced real design
  changes — see WIRING.md's Transmitter pin table and LDO section.
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

Both nodes use the same radio hardware — a Seeed XIAO ESP32-C6 paired
with a Seeed Wio-SX1262 (the "for XIAO ESP32S3" through-hole-header
variant; Seeed doesn't sell this exact pairing, so the pinout is
confirmed against the physical boards, not a datasheet — see
WIRING.md's "Shared radio stack"), each with the small ribbon antenna
that ships with the Wio-SX1262. What differs is what's attached, how
each is powered, and what the modules mount on.

This section covers what the pieces are and why; **WIRING.md has the
full pin tables, part numbers/links, and physical build/wiring
instructions**, in matching Transmitter/Receiver sections — that's the
doc to open to actually order parts or wire up a unit.

### Transmitter

Reed switch on `D0` (the only free deep-sleep-wake-capable pin), 3x AA
lithium pack through an LDO and a PTC fuse, on a custom KiCad PCB
(`kicad/`) in an IP65 enclosure with the antenna inside. No USB power
in the field. The PCB doesn't stack the modules as a HAT: it seats them
side by side, with the Wio flipped on its back for easier routing. That
lets it leave the Wio's `D0` unconnected, which isolates the reed
switch from the HAT's onboard button without modifying either module.
A UART header on D6/D7 takes an FTDI adapter for production-build
serial monitoring. A 3D-printed mount (`mount/`) holds the PCB and
battery holder in the enclosure. (The original Perma-Proto protoboard
build is still documented in WIRING.md.)

### Receiver

USB-powered, indoors, no battery or enclosure concerns — it just needs
to stay on and stay on WiFi. The usual XIAO + Wio HAT stack plugs into
a 1"x1" protoboard wired as a dual 7-pin socket, which also carries
the one peripheral: a small 5V LED rotating beacon, lit while the gate
is held open, switched from `D0` by a low-side logic-level MOSFET.

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
  from either board's datasheet alone. The protoboard build fixed it by
  snipping `D0` out of the stack; the PCB avoids it by design.
- The battery disconnect moved from JST-PH 2.0mm to Molex KK-254-style
  (2.54mm, generic KF2510 kit parts): JST-PH crimp pins aren't
  practical to crimp at home, and KK-254-style ones are.
- Long-range SMA antennas (with U.FL-to-bulkhead pigtails) were bought
  for both nodes. The Wio-SX1262's stock ribbon antenna turned out to
  give enough range, so both nodes use that: it fits inside the
  enclosure and takes up little room.
- The receiver's beacon was first wired to `D6`, reasoning that rx's
  `Serial` runs over native USB so UART0's pins were free. The gate sat
  at ~3V whatever the firmware wrote. Root cause, confirmed against the
  installed core's sdkconfig rather than a web search: the prebuilt
  ESP-IDF has its console on UART0 regardless of `CDCOnBoot` (an SDK
  configuration choice, not a silicon limit, as an earlier draft
  wrongly claimed). Moved to `D0`. `D7` is ruled out for the same
  reason.
- After the move to `D0`, the gate read a steady ~1.6V and the beacon
  stayed off. That's almost exactly the divider voltage for an
  *undriven* `D0` (the HAT's 10kΩ pull-up against the beacon's
  220Ω+10kΩ pulldown). The cause was the macOS flashing box's stale
  checkout, still flashing the `D6` build. A pull and reflash fixed it,
  with no hardware change. Check `git pull` before chasing an
  electrical-looking symptom.
- The beacon was first planned on a separate standalone protoboard,
  wired to the stack through a pair of 1x3 headers. That layout's first
  draft landed the GND→Source jumper on the `D0` island instead of a
  GND one, which would have tied `D0` straight to the MOSFET source.
  It was caught in review, before soldering, by tracing each hole
  against the header assignment rather than trusting the layout's
  prose labels. The as-built board instead puts the circuit on the
  protoboard that sockets the stack, with a 2-pin output header.
- The beacon circuit's flyback diode was dropped after a PPK2
  measurement showed the beacon is LED-only, not motor-driven.

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
  monitoring server over HTTP (URLs and WiFi credentials in
  `rx/local.h` — see "Build, test, and flash").
- Joins WiFi under `RX_HOSTNAME` (DHCP hostname, so the router shows it
  and serves local DNS for it) and advertises it over mDNS as
  `<name>.local`.
- Drives the held-open beacon through `beacon_should_be_on()`. For now
  it's lit whenever the door's open (testing phase); the intended final
  behavior, lit only after a held-open alert fires, is a one-line
  change there.
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
- The receiver's "transmitter's gone quiet" staleness alert originally
  cleared its one-shot latch on *any* radio reception, including a
  CRC failure — caught live when CRC failures logged while the
  transmitter was confirmed powered off overnight suppressed the
  alert that should have kept firing. Fixed by only clearing the
  latch on a reception that actually decodes as a known packet type
  (the silence *clock* still resets on any reception, since that part
  was never wrong).
- NTP sync and a reported `localtime=` field were added, then removed:
  the actual timing gap they were meant to close (radio-level latency)
  is usually under a second and gets swamped by the monitoring
  channel's own latency anyway — not worth the added wire-format and
  firmware complexity.
