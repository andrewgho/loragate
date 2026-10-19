// loragate tx — door-sensor transmitter, deep-sleep cycle.
//
// Deep sleeps by default; each wake runs setup() once, decides what (if
// anything) to transmit, and goes back to sleep from inside it — there's
// no persistent loop(). See README.md's "Software" section for the wake
// behavior (reed-switch edge vs. held-open check vs. heartbeat) and
// "History" for why some of this looks the way it does.
//
// next_seq/wake_state/diagnostic_pending/pending_diagnostic_info are
// RTC_NOINIT_ATTR (survives both deep sleep and a restart — see
// rtc_checksum.h); everything else (SPI, radio) is reinitialized from
// scratch each wake.

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

// Centralizes the HIGH-means-open polarity convention above so it's only
// spelled out in one place.
bool reed_is_open() { return digitalRead(REED_PIN) == HIGH; }

// How long the door can stay open before we proactively re-report it —
// once per open episode, not repeatedly (README.md's "Software" section).
#define HELD_OPEN_CHECK_SECONDS    300

// How often to transmit *something* when the door isn't doing that on its
// own. Any door open/close/held-open event resets this clock too, since
// it serves the same "transmitter is alive" purpose.
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

// RTC_NOINIT_ATTR, not RTC_DATA_ATTR: these need to survive a restart too
// (ESP.restart(), e.g. the rapid-slam guard below) — see rtc_checksum.h.
// No initializers here: .rtc_noinit is skipped by the bootloader's
// startup init, so any "= ..." would never run; real starting values are
// established in setup() the first time persisted_checksum fails to
// validate.
RTC_NOINIT_ATTR uint32_t next_seq;
RTC_NOINIT_ATTR DoorWakeState wake_state;  // see door_wake_logic.h

// Set as soon as an abnormal reset is detected (no radio needed for
// that), cleared once PKT_DIAGNOSTIC actually goes out. Carried across
// wakes separately from reset_reason because detection and the ability
// to send can land on different wakes if radio.begin() fails right after
// the reset itself.
RTC_NOINIT_ATTR bool diagnostic_pending;
RTC_NOINIT_ATTR uint8_t pending_diagnostic_info;

// Checksum over the four fields above (see rtc_checksum.h) — refreshed at
// the top of enter_safe_deep_sleep() so it always reflects this wake's
// final values, verified against in setup() alongside the reset-reason
// check.
RTC_NOINIT_ATTR uint32_t persisted_checksum;

// 255 isn't a real esp_reset_reason_t value — used as PKT_DIAGNOSTIC's
// info when persisted_checksum doesn't match despite a reset reason that
// claimed to be routine. Must match RTC_DATA_LOSS_INFO in rx/rx.ino.
#define RTC_DATA_LOSS_INFO 255

volatile bool rx_flag = false;
void IRAM_ATTR set_rx_flag() { rx_flag = true; }

// Sleeps for `seconds`, waking early on a reed-switch edge (the opposite
// level of `door_open`) or when the timer expires, whichever comes first.
// Under -DDEBUG, substitutes a delay()+esp_restart() loop so USB/serial
// stay up for monitoring (deep sleep powers down the USB peripheral).
void enter_safe_deep_sleep(bool door_open, uint32_t seconds) __attribute__((noreturn));
void enter_safe_deep_sleep(bool door_open, uint32_t seconds) {
  // Refresh before anything below might restart or sleep, covering both
  // outcomes of this function from one place.
  persisted_checksum = compute_persisted_checksum(next_seq, wake_state, diagnostic_pending, pending_diagnostic_info);

  // Re-read the pin right before committing to sleep: if it changed since
  // we decided door_open, arming a wakeup edge for the *old* state would
  // sleep through the transition that already happened. Restarting
  // re-runs setup() against the pin's actual current state instead.
  delay(DEBOUNCE_MS);
  if (reed_is_open() != door_open) {
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
  radio.sleep();  // explicit SX1262 SLEEP mode — ~2.5uA vs. STDBY's much more

  // Hold NSS driven across deep sleep: the SX1262 wakes itself from SLEEP
  // on an NSS falling edge (datasheet section 8.2.2), and the ESP32-C6
  // lets a GPIO float as sleep begins unless its level is explicitly
  // held — which looks exactly like that edge and keeps the radio in
  // STDBY (0.6-0.8mA) for the whole sleep duration instead of true SLEEP.
  // See README.md's power-budget History for the measurement trail.
  // gpio_hold_en() alone is sufficient on the C6 — unlike classic
  // ESP32/S2/S3, it doesn't also need a global gpio_deep_sleep_hold_en()
  // call (that function doesn't exist in this chip's headers at all).
  gpio_hold_en((gpio_num_t)LORA_NSS);

  // No ext0 wakeup on the ESP32-C6 (unlike classic ESP32/S2/S3) — ext1
  // covers a single pin just as well, taking a bitmask instead of a pin
  // number. Each call fully replaces the previous ext1 config, which is
  // what lets this flip the wakeup level between wakes.
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
  bool raw = reed_is_open();
  uint32_t stable_since = millis();
  while (true) {
    delay(5);
    bool r = reed_is_open();
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
  // Deep sleep powers down the USB peripheral, so in production nothing
  // is ever there to satisfy !Serial — this loop would burn its full
  // timeout on every wake instead, a real battery-life cost, not just a
  // debug-only nuisance (see README.md's power-budget History).
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
#endif

  Serial.println("loragate tx");

  pinMode(REED_PIN, INPUT);  // external 1M pull-up to 3V3 (WIRING.md) — no
                             // internal pull-up, matching the production circuit

  // Detect an abnormal reset, or persisted state that didn't survive
  // despite a reset reason claiming it should have, before radio.begin()
  // — so neither check is missed if that happens to fail this wake.
  // esp_reset_reason() reports only the immediately preceding boot's
  // cause: ESP_RST_DEEPSLEEP on every routine wake, ESP_RST_SW on our own
  // rapid-slam-guard restart below; anything else is worth knowing about.
  // Logged unconditionally so a live serial monitor shows it every boot.
  esp_reset_reason_t reset_reason = esp_reset_reason();
  Serial.printf("reset reason: %d\n", (int)reset_reason);

  // Independent check on top of reset_reason, since a reset reason
  // claiming "routine" can't always be trusted on its own (README.md's
  // Software history). A mismatch means next_seq/wake_state/etc. can't be
  // trusted either — reset them to known-safe values before anything
  // else reads them.
  bool persisted_state_valid = (persisted_checksum ==
      compute_persisted_checksum(next_seq, wake_state, diagnostic_pending, pending_diagnostic_info));
  if (!persisted_state_valid) {
    Serial.println("persisted state invalid (first boot, or unexpected loss); resetting");
    next_seq = 0;
    wake_state = DoorWakeState{false, false, false};
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

  // Release the NSS hold from the last sleep (see enter_safe_deep_sleep())
  // before SPI.begin() drives it fresh — a no-op if it was never held
  // (e.g. this wake's actual cause was ESP.restart(), or this is the very
  // first boot ever).
  gpio_hold_dis((gpio_num_t)LORA_NSS);

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
    // Establish the baseline now, from the pin alone, even though we
    // can't transmit this wake — otherwise a later wake where the radio
    // finally comes up could silently adopt whatever the door is doing
    // *then* as the starting state, losing any real transition in
    // between (see README.md's Software history).
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

  // See door_wake_logic.h's decide_wake_action() comment for why this
  // matters only in the already-no-transition branches.
  bool timer_wakeup = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER);

  // decide_wake_action() (door_wake_logic.h) is the pure decision of what
  // this wake means; everything below just carries it out.
  switch (decide_wake_action(wake_state, actual_open, timer_wakeup)) {
    case WAKE_ACTION_NONE:
      // First-ever wake where radio.begin() succeeded immediately —
      // nothing to compare against yet, just record the starting state.
      // (If radio.begin() had instead failed on an earlier wake, the
      // baseline was already established in the failure branch above.)
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
