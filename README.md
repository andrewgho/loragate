# loragate

LoRa door-sensor gateway: a battery-powered transmitter (XIAO ESP32C6 +
Wio-SX1262, reed switch, deep sleep) sends door open/close/heartbeat events
to a mains-powered receiver (same hardware, always on), which forwards them
to a WiFi-connected monitoring server.

**Current status: phase 2.** `tx-test/`/`rx-test/` are the phase-1
link-layer proof of concept — a packet carrying just a sequence number,
retried with backoff until acked, reported to a server over HTTP GET —
superseded by `tx/`/`rx/`'s real door-event protocol (see "Phase 2
protocol" below). `tx/tx.ino` deep-sleeps between events; `rx/rx.ino` adds
its own door-held-open and transmitter-staleness detection, clock sync, and
WiFi reconnect on top of forwarding what it receives. Neither sketch has
client time or battery voltage on the wire yet — those stay open items.

## Hardware

- 2x Seeed XIAO ESP32C6
- 2x Seeed Wio-SX1262 ("for XIAO ESP32S3" variant — through-hole pin header
  version, not the hidden B2B-connector version)
- Transmitter: 2x AA lithium primary cells, magnetic reed switch, waterproof
  enclosure with cable glands (outdoor)
- Receiver: USB power, indoors, always on, WiFi-connected

### LoRa pin mapping

Seeed doesn't sell or document a Wio-SX1262 + XIAO ESP32C6 pairing (only
ESP32S3 and nRF52840 are official), so there's no vendor pinout for this
combination. The pin mapping in `tx-test/lora_pins.h` and `rx-test/lora_pins.h`
(kept identical in both) is confirmed against the physical boards' own
silkscreen — the pin labels printed next to the through-hole headers on each
board — see the comment block at the top of `lora_pins.h` for the table.
An earlier schematic-derived guess (before the boards were in hand to read)
was off by one header position on the control-line side and got
`RADIOLIB_ERR_CHIP_NOT_FOUND` (-2) from `radio.begin()`; that's why the file
credits the silkscreen over the schematic if they're ever next revised and
disagree again.

## Toolchain setup

Same as the `thermometer` repo (`arduino-cli` + the ESP32 board package);
see `~/work/thermometer/xiao-sen63c/README.md` for the from-scratch steps.
Additionally needed here:

```sh
arduino-cli lib install RadioLib
```

Flashing happens from macOS (this dev box has no `arduino-cli` installed —
it's Linux-only for editing/building the source). `make flash` from either
sketch directory, or `make flash-tx` / `make flash-rx` from the repo root.

## Build system

Each sketch directory (`tx-test/`, `rx-test/`) has its own Makefile,
following the `thermometer` repo's conventions:

- `make flash` — compile + upload
- `make debug` — compile with `-DDEBUG` + upload + open serial monitor.
  Substitutes a `delay()` + `esp_restart()` loop for deep sleep so the USB
  port stays up for monitoring (deep sleep powers down the USB peripheral).
- `make monitor` — open serial monitor without reflashing
- `make clean` — remove build artifacts
- `PORT=/dev/cu.usbmodemXXXX make flash` — override the default port

`rx-test/` additionally needs `local.h` (gitignored) for WiFi credentials
and the report URL template — copy `local.h.example` and fill it in:

```c
#define WIFI_SSID     "..."
#define WIFI_PASSWORD "..."
#define REPORT_URL_TEMPLATE "http://192.168.1.12/heartbeat.txt?from=lora-receiver-test&seqno=%lu&txno=%u&rssi=%.1f&snr=%.1f"
```

If you already have a `local.h` from before, it needs updating too — the
template now takes four placeholders, not one (see order below).

## Link-layer protocol (phase 1)

6-byte packet: `uint8_t type` (`1` = data, `2` = ack) + `uint32_t seq` +
`uint8_t txno` (which transmit attempt this is — `0` = first try, `1` = first
retry, etc.; meaningful on data packets, unused/`0` on acks). Transmitter
sends, waits up to 1.5 s for a matching ack, retries up to 5 times with
backoff (250 ms doubling to a 4 s cap), then deep sleeps 10 s either way.
Receiver acks every data packet it gets and reports seq, txno, and the
packet's RSSI/SNR to the server (query params in that order — `seqno`,
`txno`, `rssi`, `snr`). See `lora_packet.h` (duplicated identically in both
sketch directories — Arduino sketches can't share files across directories).

`rx-test/rx-test.ino` has an `ACK_DROP_PCT` knob (0 by default) — set it to
exercise the transmitter's retry/backoff path without needing real radio
interference; the receiver logs "ack drop (test)" whenever it withholds one.

## Phase 2 protocol (tx/rx)

- Packet type gains `PKT_DOOR_HELD_OPEN` alongside the phase-1
  heartbeat/open/close/ack types (see `lora_packet.h`) — no clock or
  battery-voltage fields on the wire yet; those remain open items (see
  `PLAN.md`).
- `tx/tx.ino` wakes on any reed-switch edge and reports the new door state.
  While the door is open it also wakes every 300s
  (`HELD_OPEN_CHECK_SECONDS`) to send `PKT_DOOR_HELD_OPEN` for as long as
  it stays open. While closed, it wakes on an hourly
  (`HEARTBEAT_INTERVAL_SECONDS`, configurable) timer and sends a heartbeat
  if nothing else has happened by then — every event type counts equally
  as "still alive", so the hourly timer is really just a floor on how long
  the transmitter can go quiet, not a guarantee of a literal heartbeat
  packet every hour. Deep sleep between wakes; see `PLAN.md`'s "Wake
  behavior" for the state machine and its rapid-slam/bounce guard.
- Same send/ack/retry link layer as phase 1.
- Still open: battery voltage, and whichever clock field ends up on the
  wire (see `PLAN.md`'s "Open items").
- `rx/rx.ino` forwards every recognized packet to the monitoring server
  (`open`/`close`/`heartbeat`, each carrying `seq`/`txno`) and also:
  - Hits `STARTUP_LOG_URL` once, the first time WiFi comes up after boot.
  - Syncs its clock from `NTP_SERVER` on every WiFi (re)connect and once a
    day after that, and includes `localtime=<epoch seconds>` on every
    outgoing request (`http_get()`).
  - Keeps its own idea of the door state: raises
    `DOOR_OPEN_PAST_URL_TEMPLATE`'s `.../open_past/45` if a door-open
    episode runs past `SHORT_OPEN_THRESHOLD_SECONDS` without a close, and
    `.../open_past/300` on receiving the transmitter's own
    `PKT_DOOR_HELD_OPEN` (carrying its `seq`/`txno` too).
  - Raises `TRANSMISSION_STALE_URL_TEMPLATE` if nothing's been heard from
    the transmitter (any packet type) in `STALE_WINDOW_SECONDS`.
  - Retries the WiFi connection on its own (`run_housekeeping()`) if it
    drops, independent of whether a packet or outgoing request happens to
    be pending.

  All of the above (URLs, thresholds) are compile-time `#define`s — URLs
  and credentials in `rx/local.h` (gitignored, copy `local.h.example`),
  behavior thresholds directly in `rx/rx.ino`.
