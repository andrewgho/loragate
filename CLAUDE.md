# loragate repo — AI context

LoRa door-sensor gateway: a battery-powered transmitter (XIAO ESP32C6 +
Wio-SX1262, reed switch, deep sleep) sends door open/close/heartbeat
events to a mains-powered receiver (same hardware, always on), which
forwards them to a WiFi-connected monitoring server. `README.md` is the
user-facing overview; `PLAN.md` is the transmitter's design doc (wake
behavior, power budget, open items); `WIRING.md` is the hardware build
doc. This file is a quick-orientation index — the docs above have the
actual detail, don't duplicate it here, just point at it.

## Status

Phase 2. Hardware is built and field-tested (radio link confirmed solid
at ~1 block through trees). Firmware for the deep-sleep wake cycle (tx)
and the receiver enhancements (rx) below is implemented and host-tested
but **not yet flashed to real hardware** — `make compile` and `make
test` pass; nothing has been confirmed against an actual reed-switch
edge during real deep sleep yet. See PLAN.md's "Open items" for what's
still unconfirmed, and WIRING.md's "Battery voltage monitoring" for what's
designed but not wired into firmware yet (ADS1115 reading).

## Sketches

| Path | Role | Status |
|---|---|---|
| `tx-test/` | Transmitter, phase 1 | Link-layer proof of concept — fixed 10s timer, no sleep, no door events |
| `rx-test/` | Receiver, phase 1 | Matching proof of concept — acks everything, reports seq/txno/rssi/snr |
| `tx/` | Transmitter, phase 2 | Deep-sleep wake cycle on the real door-event protocol — implemented, not yet flashed |
| `rx/` | Receiver, phase 2 | Forwards real events + its own held-open/staleness detection, WiFi reconnect — implemented, not yet flashed |

Arduino sketches can't share files across directories, so `lora_pins.h`
and `lora_packet.h` are duplicated between each phase's tx/rx pair —
`make test` diffs them to catch drift (see "Testing" below).

## Build system

Repo-root `Makefile`: `make compile` (all four sketches, no upload),
`make test` (see "Testing"), `make flash-tx`/`make flash-rx` (flash
`tx-test`/`rx-test`), `make clean`. Each sketch directory also has its
own Makefile (`make flash`, `make debug`, `make monitor`, `make clean`,
`PORT=... make flash`) — `rx-test`'s and `rx`'s are identical. `tx-test`'s
and `tx`'s differ in one respect: `tx/Makefile`'s production build
(`FQBN`) sets `CDCOnBoot=default`, routing `Serial` to hardware UART0
instead of native USB CDC — native USB doesn't survive deep sleep and
nobody's plugged into it in the field anyway, while UART0 (with an
external FTDI adapter) does, which is what made the brownout/
`RTC_NOINIT_ATTR` investigation possible (see `PLAN.md`'s "Reset
diagnostics"). `tx/Makefile`'s debug build (`DBG_FQBN`) keeps
`CDCOnBoot=cdc` — one USB cable for both flashing and monitoring when
someone's actually sitting there with it plugged in. `tx-test` never
deep sleeps, so it has no reason to differ from the native-USB default.
Monitoring a production `tx` build means pointing `PORT` at the FTDI
adapter's device path, not the native USB one. `make debug` substitutes
a `delay()` + `esp_restart()` loop for deep sleep so the USB port stays
up for monitoring.

Toolchain: `arduino-cli` + the ESP32 board package + `RadioLib` — same
setup as the `thermometer` repo. It's installed on this Linux dev box
too, so `make compile` and `make test` both work here; only flashing
needs the macOS box with the hardware actually attached.

`rx-test/local.h` and `rx/local.h` are both gitignored and differently
shaped — `rx-test/local.h.example` has one URL template (4 placeholders:
seq, txno, rssi, snr); `rx/local.h.example` has WiFi credentials plus
several URL templates (see README.md's "Phase 2 protocol" for the full
field list). Don't confuse the two when setting up a fresh checkout.

## Testing

- `test/test_lora_packet_phase1.cpp` / `test_lora_packet_phase2.cpp` —
  wire format (struct layout, enum values, byte round-trip) for each
  protocol phase, against `tx-test/lora_packet.h` / `tx/lora_packet.h`
  respectively. `test/Makefile`'s `test` target also diffs each phase's
  duplicated tx/rx copies against each other.
- `test/test_tx_wake_logic.cpp` — `tx/door_wake_logic.h`'s pure
  wake-decision logic (no Arduino/radio dependencies). Several cases are
  explicit regression tests for bugs caught by manual review: the
  held-open check firing once per episode rather than repeating, and the
  `radio.begin()`-failure baseline-establishment bug (see PLAN.md's
  "Never missing a state change"). Verified non-vacuous by deliberately
  reintroducing both bugs in a scratch copy and confirming the
  corresponding assertions fail.
- `test/test_rtc_checksum.cpp` — `tx/rtc_checksum.h`'s FNV-1a checksum
  over the state that needs to survive `ESP.restart()`, not just deep
  sleep (see "Design decisions" below).
- `test/test_seq_dedup.cpp` — `rx/seq_dedup.h`'s retry-vs-duplicate check
  (see "Design decisions" below).
- None of this reaches hardware-in-the-loop behavior: whether
  `esp_sleep_enable_ext1_wakeup()` actually wakes the board on a real
  edge during real deep sleep, contact-bounce timing, RF range/retry, or
  WiFi/HTTP against the real server. Only flashing confirms those.

## Design decisions worth knowing before touching tx/tx.ino or rx/rx.ino

- **ESP32-C6 has no `esp_sleep_enable_ext0_wakeup()`** — its `soc_caps.h`
  has no `SOC_PM_SUPPORT_EXT0_WAKEUP`, unlike classic ESP32/S2/S3. PLAN.md's
  original design sketch (adapted from an AI-authored first draft) assumed
  ext0 and was wrong; `tx/tx.ino` uses `esp_sleep_enable_ext1_wakeup()`
  instead (takes a GPIO bitmask, not a pin number; each call fully replaces
  the previous config, which is relied on to flip the wakeup level between
  wakes). Verified against the installed esp32-arduino-libs headers
  directly, not a datasheet or forum post.
- **The held-open check is one-shot per open episode, not repeating.**
  Re-arming the 300s check for as long as the door stays open would mean
  ~280 wakes/day for a door left open all day — real battery budget for a
  condition the first check already reported. `held_open_checked`
  (`DoorWakeState`) tracks this and resets on every transition.
- **Every wake diffs the real pin against a persisted "last reported"
  baseline — whether a transition happened is never decided from
  `esp_sleep_get_wakeup_cause()`.** This is the core invariant behind
  "never miss a door state change" — see PLAN.md's "Never missing a
  state change" for the full reasoning, including the radio.begin()-failure
  edge case that required a real fix (baseline must be established even
  when the radio is down, or a later successful wake can silently adopt a
  stale state as "the starting point"). `esp_sleep_get_wakeup_cause()` is
  consulted for a separate, narrower question once a wake is already
  known to carry no transition — see the spurious-wake bullet below.
- **A no-transition wake still needs to know if it was the timer or a
  spurious edge** (`decide_wake_action()`'s `timer_wakeup` parameter,
  `WAKE_ACTION_SPURIOUS`) — found via manual review of a live field log
  showing three heartbeats within ~2 seconds, nowhere near
  `HEARTBEAT_INTERVAL_SECONDS` apart. A reed-switch edge that settles
  back to the same debounced state (contact bounce/mechanical chatter,
  plausible outdoors) still fires a real ext1 wakeup; without
  distinguishing it from a genuinely elapsed timer, every such wake
  either sent an unneeded heartbeat or, worse, consumed the held-open
  check's one-shot budget before `HELD_OPEN_CHECK_SECONDS` had actually
  passed. See PLAN.md's "Wake behavior" for the fix.
- **tx's wake-decision logic is intentionally factored out of tx.ino**
  into `door_wake_logic.h` — plain C++, no Arduino/radio includes — so it
  can be unit-tested without the ESP32 toolchain. If you're touching the
  decision of what a wake means or should send, that's the file to edit;
  `tx.ino` should stay the thin I/O shell around it.
- **rx's `run_housekeeping()` ticks every loop() iteration** (throttled
  internally), not just when a radio packet arrives — this is what makes
  the 45s door-open-past check, the transmission-staleness alert, and the
  proactive WiFi reconnect possible without needing an incoming packet to
  trigger them.
- **The transmission-staleness alert's silence *clock* resets on any
  radio reception, but its one-shot alert *latch* only clears on a
  recognized packet type.** A CRC-failed or unrecognized reception means
  the radio's IRQ matched our frequency/sync word, but that's not
  reliable evidence it came from our own transmitter rather than ambient
  RF noise or interference — caught live: CRC failures logged while tx
  was confirmed powered off overnight, right after a staleness alert had
  already fired. If a corrupted reception could clear the latch too, a
  noise event landing after a real outage would silently suppress every
  alert after the first for as long as noise kept showing up, even while
  the transmitter stayed genuinely dead. Letting the silence clock itself
  reset on any reception is still fine — once a real packet arrives, it
  resets both together anyway.
- **rx keeps its own idea of door state**, independent of what the
  transmitter thinks: a local 45s-without-a-close alert
  (`SHORT_OPEN_THRESHOLD_SECONDS`) runs on rx's own clock, separate from
  (and faster than) the transmitter's 300s hardware-reverified check
  (`TX_HELD_OPEN_SECONDS` on the rx side — must match tx's
  `HELD_OPEN_CHECK_SECONDS`; not itself on the wire, so this is a
  compile-time contract between the two sketches, not something enforced
  automatically).
- **rx dedups by `seq` before acting on a packet, separate from acking.**
  Found via manual review of a long field log: tx retries a send whenever
  its ack doesn't come back, even if rx did receive and ack the original
  fine — rx was re-running the full per-type handling (including the HTTP
  notify, and for `PKT_DOOR_OPEN` resetting `door_opened_at_ms`) on every
  such retry, double-reporting one logical event. Acking stays
  unconditional (needed so tx actually stops retrying); `seq_dedup.h`'s
  `is_duplicate_seq()` gates everything after that on an exact repeat of
  the last `seq` processed, not a `<=` comparison — a `seq` that's lower
  than expected means real data loss on tx (see `RTC_DATA_LOSS_INFO`
  above), not a duplicate, and is still worth reporting.
- **tx reports abnormal reset reasons via `PKT_DIAGNOSTIC`**, prompted by
  a live-testing symptom (rapid door toggling occasionally dropped an
  event and reset `next_seq` to 0). `esp_reset_reason()` is checked as
  early as possible, before `radio.begin()` — live testing caught a real
  case where checking it only in the success path meant the diagnostic
  for a genuine anomaly went missing entirely, because `radio.begin()`
  itself failed on that wake and the *next* wake's reset reason read back
  as routine before radio ever came up. `diagnostic_pending`/
  `pending_diagnostic_info` carry a detected anomaly across however many
  wakes it takes for the radio to actually work.
- **Persisted tx state is `RTC_NOINIT_ATTR`, not `RTC_DATA_ATTR`** — the
  actual root cause of the dropped-event/reset-to-0 symptom above.
  `RTC_DATA_ATTR` is only documented to survive deep sleep, not
  `ESP.restart()` (the rapid-slam/bounce guard's software reset) —
  confirmed by live serial capture over a real UART0/FTDI connection
  (native USB CDC doesn't survive deep sleep, so this needed
  `CDCOnBoot=default` + an external adapter) showing `next_seq` and
  `wake_state` reverting to zero across exactly that kind of reset.
  `RTC_NOINIT_ATTR` survives both, but is never bootloader-zeroed, so a
  true first boot reads back as undefined garbage rather than the
  compile-time-initializer values it'd get under `RTC_DATA_ATTR`.
  `tx/rtc_checksum.h`'s FNV-1a checksum (`persisted_checksum`, itself
  `RTC_NOINIT_ATTR`) distinguishes genuine carried-over state from that
  garbage — a mismatch is treated the same as an abnormal reset reason
  (`RTC_DATA_LOSS_INFO` sentinel in the `info` byte) and resets the
  persisted variables to safe defaults. See PLAN.md's "Reset
  diagnostics" for the full mechanism, both bugs it covers, and the
  still-open hardware-brownout hypothesis (two TX current bursts landing
  close together on a 1µF-capped MCP1700) it was added to investigate.
