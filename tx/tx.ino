// loragate tx — phase-2 door-sensor transmitter, deep-sleep cycle.
//
// Deep sleeps by default. Wakes on any reed-switch (D0) edge and reports
// the new door state. After an open edge, wakes once more at
// HELD_OPEN_CHECK_SECONDS to re-verify against the real hardware state and
// report it, if the door's still open then — a one-shot "someone probably
// forgot about the door" check, not a repeating one: re-arming that same
// 300s timer for as long as the door stayed open would mean ~280 wakes/day
// of a left-open door, which is real battery budget (PLAN.md's power
// budget) for a condition that one check has already reported. Once that
// one check has fired (or if it never needed to, because the door closed
// first), wakes fall back to the regular HEARTBEAT_INTERVAL_SECONDS timer
// if nothing else has happened by then — but since every branch below
// already transmits something, that's really just "transmit at least this
// often", not "transmit a heartbeat specifically every hour".
//
// The receiver separately raises its own "door open past 45s" alert from
// the initial open event alone, on its own clock — see rx/rx.ino and
// README.md's "Phase 2 protocol". That's what covers "someone just opened
// the door"; this file's HELD_OPEN_CHECK_SECONDS is the slower, hardware
// re-verified "still open, probably forgotten" follow-up.
//
// RTC_NOINIT_ATTR state survives both deep sleep *and* a restart (see
// rtc_checksum.h) so the seq counter and committed door state carry
// across wake cycles; everything else (SPI, radio) is reinitialized from
// scratch each wake, same as tx-test.ino. There is no persistent loop()
// here — each wake runs setup() once, decides what (if anything) to
// transmit, and goes back to sleep from inside it.
//
// Also reports a PKT_DIAGNOSTIC if a wake was caused by anything other
// than a routine deep-sleep wake or our own deliberate restart, or if
// persisted state turns out to have been lost despite the reset reason
// claiming one of those two routine causes — detected as early as
// possible (before radio.begin()) and carried in diagnostic_pending
// across however many wakes it takes for the radio to actually come up.
// See PLAN.md's "Reset diagnostics" for why this exists (a live brownout
// investigation that turned up a real RTC_DATA_ATTR-vs-RTC_NOINIT_ATTR
// bug along the way).

#include <SPI.h>
#include <RadioLib.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include "lora_pins.h"
#include "lora_packet.h"
#include "door_wake_logic.h"
#include "rtc_checksum.h"

// D0/GPIO0 per WIRING.md: external 1M pull-up to 3V3, contacts closed
// (continuity, reads LOW) when the magnet is present (gate closed), open
// (reads HIGH) when the magnet moves away (gate open).
#define REED_PIN              0
#define DEBOUNCE_MS           50

// How long the door can stay open before we proactively re-report it —
// once per open episode, not repeatedly (see the file header comment).
#define HELD_OPEN_CHECK_SECONDS    300

// How often to transmit *something* when the door isn't doing that on its
// own. Configurable; any door open/close/held-open event resets this clock
// too, since it serves the same "transmitter is alive" purpose — see the
// wake-cause handling in setup().
#define HEARTBEAT_INTERVAL_SECONDS (60UL * 60UL)

#define MAX_RETRIES         5
#define ACK_TIMEOUT_MS      1500
#define INITIAL_BACKOFF_MS  250
#define MAX_BACKOFF_MS      4000

#ifdef DEBUG
  #define LOG(fmt, ...) Serial.printf("[DBG] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG(fmt, ...) do {} while (0)
#endif

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

// RTC_NOINIT_ATTR, not RTC_DATA_ATTR: these need to survive a restart
// (ESP.restart(), e.g. the rapid-slam guard below), and RTC_DATA_ATTR is
// only documented to survive deep sleep, not a restart — confirmed the
// hard way via live testing, then directly against esp_attr.h's own doc
// comments. No initializers here: .rtc_noinit is skipped entirely by the
// bootloader's startup init, so any "= ..." would never actually run —
// real starting values are established explicitly in setup() the first
// time persisted_checksum fails to validate. See PLAN.md's "Reset
// diagnostics" and rtc_checksum.h.
RTC_NOINIT_ATTR uint32_t next_seq;
RTC_NOINIT_ATTR DoorWakeState wake_state;  // see door_wake_logic.h

// Set as soon as an abnormal reset is detected (no radio needed for that),
// cleared only once PKT_DIAGNOSTIC has actually gone out. Needed because
// detection and the ability to send are on different wakes whenever
// radio.begin() fails on the wake right after the reset itself — plausible
// since that's exactly when things are least likely to be settled — in
// which case esp_reset_reason() has already moved on to describing a
// routine cause by the time a later wake's radio finally comes up. See
// PLAN.md's "Reset diagnostics".
RTC_NOINIT_ATTR bool diagnostic_pending;
RTC_NOINIT_ATTR uint8_t pending_diagnostic_info;

// Covers next_seq/wake_state/diagnostic_pending/pending_diagnostic_info —
// see rtc_checksum.h. Refreshed at the top of enter_safe_deep_sleep(), so
// it always reflects this wake's final values before anything that might
// restart or sleep; verified against (and, if it doesn't match, used to
// trigger resetting those four back to known-safe values) early in
// setup(), alongside the reset-reason check.
RTC_NOINIT_ATTR uint32_t persisted_checksum;

// 255 isn't a real esp_reset_reason_t value (those top out well below
// this) — used as PKT_DIAGNOSTIC's info when persisted_checksum doesn't
// match despite a reset reason that claimed to be routine, which should
// be impossible now but is worth a distinct, honest label if it ever
// happens again. Must match RTC_DATA_LOSS_INFO in rx/rx.ino.
#define RTC_DATA_LOSS_INFO 255

volatile bool rx_flag = false;
void IRAM_ATTR set_rx_flag() { rx_flag = true; }

// Sleeps for `seconds`, waking early on a reed-switch edge (the opposite
// level of `door_open`) or when the timer expires, whichever comes first.
// Under -DDEBUG, substitutes a delay()+esp_restart() loop so USB/serial
// stay up for monitoring (deep sleep powers down the USB peripheral) — same
// convention as tx-test.ino.
void enter_safe_deep_sleep(bool door_open, uint32_t seconds) __attribute__((noreturn));
void enter_safe_deep_sleep(bool door_open, uint32_t seconds) {
  // Refresh the checksum to match whatever next_seq/wake_state/etc. are
  // right now, before anything below might restart or actually sleep —
  // covers both outcomes of this function (the rapid-slam guard's
  // ESP.restart() just below, and the real esp_deep_sleep_start() further
  // down) from one place, so there's nothing to remember at each call site.
  persisted_checksum = compute_persisted_checksum(next_seq, wake_state, diagnostic_pending, pending_diagnostic_info);

  // Re-read the pin right before committing to sleep: if it changed since
  // we decided door_open, arming a wakeup edge for the *old* state would
  // sleep through the transition that already happened (PLAN.md's
  // rapid-slam guard). Restarting re-runs setup() against the pin's actual
  // current state instead of sleeping through a missed edge.
  delay(DEBOUNCE_MS);
  if ((digitalRead(REED_PIN) == HIGH) != door_open) {
    Serial.println("door moved while preparing to sleep; restarting to re-check");
    Serial.flush();
    ESP.restart();
  }

  Serial.printf("sleeping %lus (door %s)\n", (unsigned long)seconds, door_open ? "open" : "closed");
  Serial.flush();

#ifdef DEBUG
  delay(seconds * 1000UL);
  esp_restart();
#else
  radio.sleep();  // explicit SX1262 sleep — ~2.5uA vs standby's more (PLAN.md power budget)

  // EXPERIMENTAL. Holding NSS alone confirmed the leading theory: ESP32-C6
  // doesn't retain a GPIO's driven level across deep sleep unless held,
  // NSS floating looks like a falling edge (the SX1262's documented
  // wake-from-SLEEP trigger, datasheet section 8.2.2), and live PPK2
  // measurement dropped from ~1.5mA to ~318uA once it was held -- but
  // that's still well above the ~41-44uA budgeted target.
  //
  // Holding RFSW *HIGH* too was tried next and made things slightly
  // *worse* (~368uA) -- actively keeping the RF switch circuit enabled
  // through sleep costs real current, more than whatever undefined state
  // it floats to on its own. HIGH is the level lora_pins.h documents as
  // the "awake, radio in use" level, so that's not surprising in
  // hindsight. LOW is the only other defined level RFSW has never
  // actually been driven to; testing whether it's this switch's genuine
  // disable/low-power state rather than just reverting to floating.
  //
  // gpio_hold_en() alone is sufficient here -- unlike classic ESP32/S2/S3,
  // where it additionally needs a global gpio_deep_sleep_hold_en() call,
  // ESP32-C6 sets SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP, meaning a single
  // pin's hold already takes effect across deep sleep on its own; that
  // global call doesn't even exist in this chip's headers (confirmed by a
  // build error, not assumed).
  gpio_hold_en((gpio_num_t)LORA_NSS);
  digitalWrite(LORA_RFSW, LOW);
  gpio_hold_en((gpio_num_t)LORA_RFSW);

  // The ESP32-C6 has no ext0 wakeup (SOC_PM_SUPPORT_EXT0_WAKEUP is unset in
  // its soc_caps.h, unlike classic ESP32/S2/S3) — PLAN.md's original sketch
  // assumed ext0 and was wrong. ext1 covers a single pin just as well; it
  // takes a bitmask instead of a pin number, and (per its own doc comment)
  // fully replaces the previous ext1 config on every call, which is exactly
  // what's needed to flip the wakeup level between wakes.
  esp_sleep_enable_ext1_wakeup(1ULL << REED_PIN,
                                door_open ? ESP_EXT1_WAKEUP_ANY_LOW : ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

// Waits up to timeout_ms for an ACK matching expect_seq. Leaves the radio in
// standby on return either way.
bool wait_for_ack(uint32_t expect_seq, uint32_t timeout_ms) {
  rx_flag = false;
  radio.setDio1Action(set_rx_flag);
  int state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("startReceive failed: %d\n", state);
    return false;
  }

  bool got_ack = false;
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    if (rx_flag) {
      rx_flag = false;
      LoraPacket pkt;
      int st = radio.readData((uint8_t *)&pkt, sizeof(pkt));
      if (st == RADIOLIB_ERR_NONE && pkt.type == PKT_ACK && pkt.seq == expect_seq) {
        got_ack = true;
        break;
      }
      LOG("ignoring packet type=%d seq=%lu while waiting for ack", pkt.type, (unsigned long)pkt.seq);
      radio.startReceive();  // keep listening for the rest of the window
    }
    delay(5);
  }
  radio.clearDio1Action();
  radio.standby();
  return got_ack;
}

// Sends one packet of the given type on the shared seq counter, retrying
// with backoff until acked or out of attempts. `info` rides along
// unchanged on every attempt (unlike txno, which send_event() itself
// overwrites with the attempt number each retry) — reserved/0 for every
// type except PKT_DIAGNOSTIC, which uses it for a reset-reason code.
void send_event(LoraPacketType type, uint8_t info) {
  uint32_t seq = next_seq++;

  bool acked = false;
  uint32_t backoff_ms = INITIAL_BACKOFF_MS;
  for (int attempt = 0; attempt <= MAX_RETRIES && !acked; attempt++) {
    Serial.printf("TX type=%d seq=%lu attempt=%d/%d\n", type, (unsigned long)seq, attempt, MAX_RETRIES);
    LoraPacket pkt = { (uint8_t)type, seq, (uint8_t)attempt, info };
    int st = radio.transmit((uint8_t *)&pkt, sizeof(pkt));
    if (st != RADIOLIB_ERR_NONE) {
      Serial.printf("transmit failed: %d\n", st);
    } else {
      acked = wait_for_ack(seq, ACK_TIMEOUT_MS);
    }

    if (!acked && attempt < MAX_RETRIES) {
      Serial.printf("no ack, backing off %lums\n", (unsigned long)backoff_ms);
      delay(backoff_ms);
      backoff_ms = min(backoff_ms * 2, (uint32_t)MAX_BACKOFF_MS);
    }
  }

  if (acked) {
    Serial.printf("ACK received for seq=%lu\n", (unsigned long)seq);
  } else {
    Serial.printf("giving up on seq=%lu after %d attempts\n", (unsigned long)seq, MAX_RETRIES + 1);
  }
}

// Blocks until the pin has read the same value for DEBOUNCE_MS straight,
// then returns that value — a trustworthy "what is the door doing right
// now", as opposed to the single raw read esp_sleep's wakeup cause implies.
bool read_debounced_state() {
  bool raw = digitalRead(REED_PIN) == HIGH;
  uint32_t stable_since = millis();
  while (true) {
    delay(5);
    bool r = digitalRead(REED_PIN) == HIGH;
    if (r != raw) {
      raw = r;
      stable_since = millis();
    } else if (millis() - stable_since >= DEBOUNCE_MS) {
      return raw;
    }
  }
}

// Sends wake_state.door_open_committed's current value — already decided,
// along with resetting held_open_checked for this new episode, by
// decide_wake_action() before this is called — then immediately rechecks
// the actual pin state (no delay/sleep) in case the door moved again while
// send_event() was blocked in its ack/retry loop. Keeps reporting and
// rechecking until the reported state matches reality — so however many
// transitions happened while we weren't listening, we always converge on
// reporting the true end state, never leaving the receiver holding a
// stale one.
void report_door_transition() {
  Serial.println(wake_state.door_open_committed ? "door: open" : "door: close");
  send_event(wake_state.door_open_committed ? PKT_DOOR_OPEN : PKT_DOOR_CLOSE, 0);

  while (true) {
    bool actual = read_debounced_state();
    if (actual == wake_state.door_open_committed) break;
    Serial.println(actual ? "door: open (caught up)" : "door: close (caught up)");
    wake_state.door_open_committed = actual;
    send_event(wake_state.door_open_committed ? PKT_DOOR_OPEN : PKT_DOOR_CLOSE, 0);
  }
}

void setup() {
  Serial.begin(115200);
#ifdef DEBUG
  // Only wait for a monitor in debug builds, where one is deliberately
  // attached before the device sleeps (see tx/Makefile's make debug).
  // Waiting here in production would cost up to 3s of fully-awake CPU
  // time on *every* wake: deep sleep powers down the USB peripheral, so
  // there's never anyone listening in the field and !Serial never goes
  // true early — this loop would just burn out its full timeout, every
  // time, forever. That's both the dominant source of wake-to-report
  // latency and a real battery-life cost (comparable to the entire rest
  // of the daily budget in PLAN.md's power budget table), not just a
  // debug-build-only nuisance.
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
#endif

  Serial.println("loragate tx");

  pinMode(REED_PIN, INPUT);  // external 1M pull-up to 3V3 (WIRING.md) — no
                             // internal pull-up, matching the production circuit

  // EXPERIMENTAL: REED_PIN is one of the C6's 8 RTC-capable GPIOs (required
  // for ext1 wakeup) and has its own RTC/LP-IO-domain pull-up/pulldown
  // enable bits, entirely separate from the regular digital-GPIO pull
  // config pinMode() just set above -- so pinMode(INPUT) alone may not be
  // disabling them. Leading theory for the still-unexplained ~297uA sleep
  // current gap after the NSS/RFSW fixes: live PPK2 measurement found
  // sleep current at ~318uA while the door is closed but only ~21uA
  // (matching the budgeted target) while open. A redundant internal pull
  // sitting in parallel with the external 1M pull-up (WIRING.md) would be
  // invisible while open (no completed circuit, no current either way)
  // but would draw real current once the switch closes and completes a
  // path to ground -- exactly this asymmetry. PLAN.md's own power budget
  // already flags "~73uA" as what an internal pull-up alone would cost
  // here, which is why the design uses an external 1M instead; this
  // makes sure neither internal pull is left on to stack with it.
  // gpio_pullup_dis()/gpio_pulldown_dis() (not the rtc_gpio_* equivalents)
  // per their own doc comment, since those also work for RTC IOs without
  // needing rtc_gpio_init() to reroute the pin through the RTC IOMUX --
  // which could interfere with the normal pinMode()/digitalRead() path
  // used while awake.
  gpio_pullup_dis((gpio_num_t)REED_PIN);
  gpio_pulldown_dis((gpio_num_t)REED_PIN);

  // Detect an abnormal reset, or persisted state that didn't survive
  // despite a reset reason claiming it should have, as early as possible
  // — neither check needs the radio, so neither is missed if
  // radio.begin() below happens to fail on this particular wake.
  //
  // esp_reset_reason() reports only the *immediately preceding* boot's
  // cause: ESP_RST_DEEPSLEEP on every routine wake, ESP_RST_SW on our own
  // rapid-slam-guard restarts (enter_safe_deep_sleep() above); anything
  // else (ESP_RST_POWERON, ESP_RST_BROWNOUT, a watchdog, a panic, ...) is
  // worth knowing about. Logged unconditionally (not just when abnormal)
  // so a live serial monitor shows it on every boot.
  esp_reset_reason_t reset_reason = esp_reset_reason();
  Serial.printf("reset reason: %d\n", (int)reset_reason);

  // persisted_checksum is a second, independent check on top of
  // reset_reason: live testing found that ESP.restart() does not reliably
  // preserve RTC_DATA_ATTR (only RTC_NOINIT_ATTR is documented to survive
  // a restart, not just deep sleep — see PLAN.md's "Reset diagnostics"),
  // so a reset reason claiming "routine" couldn't always be trusted on
  // its own. A mismatch here means next_seq/wake_state/etc. can't be
  // trusted either (true first boot, or something corrupted them) — reset
  // them to known-safe values before anything else reads them.
  bool persisted_state_valid = (persisted_checksum ==
      compute_persisted_checksum(next_seq, wake_state, diagnostic_pending, pending_diagnostic_info));
  if (!persisted_state_valid) {
    Serial.println("persisted state invalid (first boot, or unexpected loss); resetting");
    next_seq = 0;
    wake_state = (DoorWakeState){false, false, false};
    diagnostic_pending = false;
    pending_diagnostic_info = 0;
  }
  Serial.printf("persisted state valid: %d, diagnostic_pending=%d\n", persisted_state_valid, diagnostic_pending);

  if (reset_reason != ESP_RST_DEEPSLEEP && reset_reason != ESP_RST_SW) {
    Serial.printf("abnormal reset reason: %d\n", (int)reset_reason);
    diagnostic_pending = true;
    pending_diagnostic_info = (uint8_t)reset_reason;
  } else if (!persisted_state_valid) {
    // reset_reason claimed this was routine, but persisted state didn't
    // survive anyway — shouldn't be possible now, but if it happens,
    // label it honestly rather than under a reason that doesn't fit.
    Serial.println("persisted state lost despite routine reset reason");
    diagnostic_pending = true;
    pending_diagnostic_info = RTC_DATA_LOSS_INFO;
  }

  // Release the NSS/RFSW holds from the last sleep (see
  // enter_safe_deep_sleep()) before driving them fresh below — a no-op if
  // they were never held (e.g. this wake's actual cause was
  // ESP.restart(), or this is the very first boot ever).
  gpio_hold_dis((gpio_num_t)LORA_NSS);
  gpio_hold_dis((gpio_num_t)LORA_RFSW);

  pinMode(LORA_RFSW, OUTPUT);
  digitalWrite(LORA_RFSW, HIGH);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int state = radio.begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ,
                           LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                           LORA_SYNC_WORD, LORA_TX_POWER_DBM,
                           LORA_PREAMBLE_LEN, LORA_TCXO_VOLTAGE, LORA_USE_LDO);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin failed: %d (check lora_pins.h wiring)\n", state);
    bool actual_open = read_debounced_state();
    // Establish the baseline now, from the pin alone — it doesn't need the
    // radio — even though we can't transmit anything this wake. Without
    // this, a radio fault on this very first-ever wake would leave
    // have_committed_state false and skip straight to sleeping below; a
    // *later* wake, once radio.begin() finally succeeds, would then hit
    // the "first boot" case further down and silently adopt whatever the
    // door happens to be doing *then* as "the starting state" —
    // swallowing any real open/close that happened during however many
    // failed retries came before that, with no event ever sent for it.
    // establish_baseline_if_needed() (not decide_wake_action(), which
    // assumes a send is about to happen) means have_committed_state only
    // ever reflects "has this device been through a real power-on," not
    // "did the radio also happen to work that time" — see PLAN.md's
    // "Never missing a state change".
    if (establish_baseline_if_needed(wake_state, actual_open)) {
      Serial.printf("initial door state: %s (radio not yet up)\n", actual_open ? "open" : "closed");
    }
    // No point halting forever on a battery-powered device — sleep and
    // retry on the normal schedule instead, keyed off whatever the door
    // is actually doing right now.
    enter_safe_deep_sleep(actual_open, HEARTBEAT_INTERVAL_SECONDS);
  }
  Serial.println("radio init OK");

  // The antenna switch is steered by DIO2, but the SX1262 doesn't route
  // DIO2 to the switch automatically out of reset — without this call,
  // transmit()/startReceive() report success but the switch never moves,
  // so nothing actually gets to the antenna.
  state = radio.setDio2AsRfSwitch(true);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("setDio2AsRfSwitch failed: %d\n", state);
  }

  // Now that the radio is confirmed working, send whatever reset reason
  // is still waiting to be reported — detected above on this wake, or
  // carried over from an earlier one where radio.begin() failed before
  // it could be sent.
  if (diagnostic_pending) {
    send_event(PKT_DIAGNOSTIC, pending_diagnostic_info);
    diagnostic_pending = false;
  }

  bool actual_open = read_debounced_state();

  // Whether this wake's actual cause was the RTC timer we armed, as
  // opposed to the reed-switch ext1 edge (almost always, when it isn't
  // the timer) — only consulted by decide_wake_action() in the
  // already-no-transition branches, to tell a genuinely elapsed timer
  // apart from reed-switch contact bounce/chatter that fires the edge
  // wakeup without actually changing the debounced state. See
  // door_wake_logic.h's comment on decide_wake_action() for why.
  bool timer_wakeup = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);

  // decide_wake_action() (door_wake_logic.h) is the pure decision — no
  // Arduino/radio dependencies, host-tested in test/test_tx_wake_logic.cpp
  // — of what this wake means; everything below is just carrying out
  // whichever decision it already made and updated wake_state for.
  switch (decide_wake_action(wake_state, actual_open, timer_wakeup)) {
    case WAKE_ACTION_NONE:
      // First boot (power-on, not a sleep wake) where radio.begin()
      // happened to succeed on the very first attempt — nothing to
      // compare against yet, so just record the starting state silently,
      // same as the old no-sleep tx.ino did. (If radio.begin() had
      // instead failed on an earlier wake, the baseline was already
      // established in the failure branch above.)
      Serial.printf("initial door state: %s\n", wake_state.door_open_committed ? "open" : "closed");
      break;
    case WAKE_ACTION_REPORT_TRANSITION:
      // A reed-switch edge woke us (or we raced one on the way into the
      // previous sleep) — report the transition.
      report_door_transition();
      break;
    case WAKE_ACTION_HELD_OPEN_CHECK:
      // Door was already open and still is, and this open episode hadn't
      // had its one-shot HELD_OPEN_CHECK_SECONDS check yet — this wake is
      // that check.
      Serial.println("door: still open (held-open check)");
      send_event(PKT_DOOR_HELD_OPEN, 0);
      break;
    case WAKE_ACTION_HEARTBEAT:
      // Either the door's closed, or it's open but already had its
      // one-shot check — either way, HEARTBEAT_INTERVAL_SECONDS elapsed
      // without anything else to report.
      Serial.println("heartbeat");
      send_event(PKT_HEARTBEAT, 0);
      break;
    case WAKE_ACTION_SPURIOUS:
      // A real ext1 edge fired, but the debounced read settled back to
      // the same state we already have committed, and this wasn't the
      // timer either — reed-switch contact bounce or mechanical chatter,
      // not a real transition. Nothing to send; go back to sleep without
      // spending a radio/transmit cycle on it.
      Serial.println("spurious wake (edge, no net change); nothing to report");
      break;
  }

  // Only the still-unchecked-open case needs the short HELD_OPEN_CHECK_SECONDS
  // wake; everything else (closed, or open-and-already-checked) uses the
  // long heartbeat interval instead, so a left-open door doesn't keep this
  // waking every 5 minutes for as long as it stays open.
  uint32_t sleep_seconds = decide_sleep_seconds(wake_state, HELD_OPEN_CHECK_SECONDS, HEARTBEAT_INTERVAL_SECONDS);
  enter_safe_deep_sleep(wake_state.door_open_committed, sleep_seconds);
}

void loop() {}  // everything happens in setup(); each wake re-runs it from scratch
