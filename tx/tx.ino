// loragate tx — phase-2 door-sensor transmitter, no-sleep test bench.
//
// Sends a heartbeat packet on a fixed cadence, plus a door-open/door-close
// packet whenever the reed switch (D0, WIRING.md) changes state. Every
// packet shares one incrementing seq counter and goes through the same
// send/wait-for-ack/retry dance as tx-test.ino. Deep sleep is intentionally
// left out here so both ends can be watched live over serial; PLAN.md's
// wake-on-reed-switch + deep-sleep behavior replaces this loop later.

#include <SPI.h>
#include <RadioLib.h>
#include "lora_pins.h"
#include "lora_packet.h"

// D0/GPIO0 per WIRING.md: external 1M pull-up to 3V3, contacts closed
// (continuity, reads LOW) when the magnet is present (gate closed), open
// (reads HIGH) when the magnet moves away (gate open).
#define REED_PIN              0
#define DEBOUNCE_MS           50

// How often to send a heartbeat regardless of door events. Independent of
// the debounce/event handling below; tune freely while testing.
#define HEARTBEAT_INTERVAL_MS 30000

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

uint32_t next_seq = 0;

volatile bool rx_flag = false;
void IRAM_ATTR set_rx_flag() { rx_flag = true; }

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

bool door_open;            // last debounced, committed, and reported state
bool raw_last;              // last raw pin reading, for debounce timing
uint32_t debounce_t0;
uint32_t last_heartbeat_ms;

// Blocks until the pin has read the same value for DEBOUNCE_MS straight,
// then returns that value. Used to get a trustworthy reading of "the door's
// state right now" — as opposed to the non-blocking poll in loop(), which
// exists only to catch the *first* edge without stalling the heartbeat
// schedule.
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

// Reports new_state, then immediately rechecks the actual pin state (no
// delay/sleep) in case the door moved again while send_event() was blocked
// in its ack/retry loop. Keeps reporting and rechecking until the reported
// state matches reality — so however many transitions happened while we
// weren't listening, we always converge on reporting the true end state,
// never leaving the receiver holding a stale one.
void report_door_transition(bool new_state) {
  door_open = new_state;
  raw_last = new_state;
  Serial.println(door_open ? "door: open" : "door: close");
  send_event(door_open ? PKT_DOOR_OPEN : PKT_DOOR_CLOSE);

  while (true) {
    bool actual = read_debounced_state();
    if (actual == door_open) break;
    Serial.println(actual ? "door: open (caught up)" : "door: close (caught up)");
    door_open = actual;
    raw_last = actual;
    send_event(door_open ? PKT_DOOR_OPEN : PKT_DOOR_CLOSE);
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
    Serial.println("halting");
    while (true) delay(1000);
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

  door_open = digitalRead(REED_PIN) == HIGH;
  raw_last = door_open;
  Serial.printf("initial door state: %s\n", door_open ? "open" : "closed");

  last_heartbeat_ms = millis();
}

void loop() {
  bool raw = digitalRead(REED_PIN) == HIGH;
  if (raw != raw_last) {
    raw_last = raw;
    debounce_t0 = millis();
  } else if (raw != door_open && millis() - debounce_t0 >= DEBOUNCE_MS) {
    report_door_transition(raw);
  }

  if (millis() - last_heartbeat_ms >= HEARTBEAT_INTERVAL_MS) {
    Serial.println("heartbeat");
    send_event(PKT_HEARTBEAT);
    last_heartbeat_ms = millis();
  }

  delay(10);  // poll cadence — independent of HEARTBEAT_INTERVAL_MS so a
              // door edge is never more than ~10ms + DEBOUNCE_MS late
}
