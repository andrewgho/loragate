# loragate repo — AI context

LoRa door-sensor gateway: a battery-powered transmitter (XIAO ESP32C6 +
Wio-SX1262, reed switch, deep sleep) sends door open/close/heartbeat
events to a mains-powered receiver (same hardware, always on), which
forwards them to a WiFi-connected monitoring server. This file is a
quick-orientation index of repo/toolchain specifics; for the actual
project — requirements, design rationale, hardware, and software
architecture, each with its own history of what didn't work — see
`README.md`. For wiring/BOM detail, see `WIRING.md`. Don't duplicate
either doc's content here; just point at it.

## Status

Hardware is built and field-tested (radio link confirmed solid at ~1
block through trees); firmware implements the full deep-sleep wake
cycle and is flashed to the deployed unit. See README.md's per-section
"History" subsections for what's been found and fixed along the way,
and `git log` for the full detail behind any specific change — commit
messages here are written to carry that detail rather than relying on
a separate design doc.

## Sketches

| Path | Role |
|---|---|
| `tx/` | Transmitter — deep-sleep wake cycle on the door-event protocol |
| `rx/` | Receiver — always-on, forwards events, runs its own housekeeping |
| `test/` | Host-side tests for the Arduino/radio-independent logic in both |

Arduino sketches can't share files across directories, so
`lora_pins.h` and `lora_packet.h` are duplicated between `tx/` and
`rx/` — `make test` diffs them to catch drift.

## Build system

Repo-root `Makefile`: `make compile`, `make test` (see "Testing"
below), `make flash-tx`/`make flash-rx`, `make clean`. Each sketch
directory also has its own Makefile (`make flash`, `make debug`,
`make monitor`, `make clean`, `PORT=... make flash`).

`tx/Makefile`'s production build (`FQBN`) sets `CDCOnBoot=default`,
routing `Serial` to hardware UART0 instead of native USB CDC — native
USB doesn't survive deep sleep and nobody's plugged into it in the
field, while UART0 (with an external FTDI adapter) does, which is what
made some of the live-hardware investigations in README.md's history
possible in the first place. `tx/Makefile`'s debug build (`DBG_FQBN`)
keeps `CDCOnBoot=cdc` for one-cable flash+monitor. `rx` has no deep
sleep to worry about, so it has no reason to differ from the native-USB
default.

Toolchain: `arduino-cli` + the ESP32 board package + `RadioLib` — same
setup as the `thermometer` repo. It's installed on this Linux dev box
too, so `make compile` and `make test` both work here; only flashing
needs the macOS box with the hardware actually attached. Monitoring a
production (non-debug) `tx` build means pointing `PORT` at the FTDI
adapter's device path, not the native USB one.

`rx/local.h` is gitignored — copy `rx/local.h.example` and fill in
WiFi credentials + URL templates (see README.md's "Software" section
for the field list). `tx/` needs no `local.h`.

## Testing

- `test/test_lora_packet.cpp` — wire format (struct layout, enum
  values, byte round-trip) against `tx/lora_packet.h`; `make test`
  also diffs it against `rx/lora_packet.h` for drift.
- `test/test_tx_wake_logic.cpp` — `tx/door_wake_logic.h`'s wake-decision
  logic. Several cases are explicit regression tests for real bugs
  (see README.md's Software history) caught by manual review before
  this file existed.
- `test/test_rtc_checksum.cpp` — `tx/rtc_checksum.h`'s checksum over
  the state that needs to survive `ESP.restart()`, not just deep sleep.
- `test/test_seq_dedup.cpp` — `rx/seq_dedup.h`'s retry-vs-duplicate
  check.
- None of this reaches hardware-in-the-loop behavior: whether
  `esp_sleep_enable_ext1_wakeup()` actually wakes the board on a real
  edge during real deep sleep, contact-bounce timing, RF range/retry,
  or WiFi/HTTP against the real server. Only flashing confirms those.

## Before touching tx/tx.ino, door_wake_logic.h, or rx/rx.ino

Read the file's own comments first — they carry the non-obvious "why"
for the code right next to it. README.md's "Software" section has the
higher-level architecture and a condensed history of what's been
found and fixed; `git log` has the full detail behind any individual
change (commit messages in this repo are written to stand on their
own, not to require a separate design doc alongside them).
