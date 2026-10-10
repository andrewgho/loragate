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
// RTC_DATA_ATTR state survives deep sleep (reset to zero only on power-on)
// so the seq counter and committed door state carry across wake cycles;
// everything else (SPI, radio) is reinitialized from scratch each wake,
// same as tx-test.ino. There is no persistent loop() here — each wake runs
// setup() once, decides what (if anything) to transmit, and goes back to
// sleep from inside it.

#include <SPI.h>
#include <RadioLib.h>
#include "lora_pins.h"
#include "lora_packet.h"
#include "door_wake_logic.h"

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

RTC_DATA_ATTR uint32_t next_seq = 0;
RTC_DATA_ATTR DoorWakeState wake_state = {false, false, false};  // see door_wake_logic.h

volatile bool rx_flag = false;
void IRAM_ATTR set_rx_flag() { rx_flag = true; }

// Sleeps for `seconds`, waking early on a reed-switch edge (the opposite
// level of `door_open`) or when the timer expires, whichever comes first.
// Under -DDEBUG, substitutes a delay()+esp_restart() loop so USB/serial
// stay up for monitoring (deep sleep powers down the USB peripheral) — same
// convention as tx-test.ino.
void enter_safe_deep_sleep(bool door_open, uint32_t seconds) __attribute__((noreturn));
void enter_safe_deep_sleep(bool door_open, uint32_t seconds) {
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
// with backoff until acked or out of attempts.
void send_event(LoraPacketType type) {
  uint32_t seq = next_seq++;

  bool acked = false;
  uint32_t backoff_ms = INITIAL_BACKOFF_MS;
  for (int attempt = 0; attempt <= MAX_RETRIES && !acked; attempt++) {
    Serial.printf("TX type=%d seq=%lu attempt=%d/%d\n", type, (unsigned long)seq, attempt, MAX_RETRIES);
    LoraPacket pkt = { (uint8_t)type, seq, (uint8_t)attempt };
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
  send_event(wake_state.door_open_committed ? PKT_DOOR_OPEN : PKT_DOOR_CLOSE);

  while (true) {
    bool actual = read_debounced_state();
    if (actual == wake_state.door_open_committed) break;
    Serial.println(actual ? "door: open (caught up)" : "door: close (caught up)");
    wake_state.door_open_committed = actual;
    send_event(wake_state.door_open_committed ? PKT_DOOR_OPEN : PKT_DOOR_CLOSE);
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Serial.println("loragate tx");

  pinMode(REED_PIN, INPUT);  // external 1M pull-up to 3V3 (WIRING.md) — no
                             // internal pull-up, matching the production circuit

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

  bool actual_open = read_debounced_state();

  // decide_wake_action() (door_wake_logic.h) is the pure decision — no
  // Arduino/radio dependencies, host-tested in test/test_tx_wake_logic.cpp
  // — of what this wake means; everything below is just carrying out
  // whichever decision it already made and updated wake_state for.
  switch (decide_wake_action(wake_state, actual_open)) {
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
      send_event(PKT_DOOR_HELD_OPEN);
      break;
    case WAKE_ACTION_HEARTBEAT:
      // Either the door's closed, or it's open but already had its
      // one-shot check — either way, HEARTBEAT_INTERVAL_SECONDS elapsed
      // without anything else to report.
      Serial.println("heartbeat");
      send_event(PKT_HEARTBEAT);
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
