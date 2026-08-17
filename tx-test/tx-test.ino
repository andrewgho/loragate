// loragate tx-test — LoRa send/ack/retry proof of concept (transmitter).
//
// Sends a packet carrying an incrementing sequence number, waits for an ack
// from rx-test, retries with backoff on timeout, and deep sleeps 10 s either
// way (on ack, or after giving up). Phase 1: no reed switch, no battery
// voltage, no real event types — see lora_packet.h.

#include <SPI.h>
#include <RadioLib.h>
#include "lora_pins.h"
#include "lora_packet.h"

#define SLEEP_SECONDS       10
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

volatile bool rx_flag = false;
void IRAM_ATTR set_rx_flag() { rx_flag = true; }

void deep_sleep(long seconds) __attribute__((noreturn));
void deep_sleep(long seconds) {
  Serial.printf("Sleeping %lds\n", seconds);
  Serial.flush();
#ifdef DEBUG
  delay((uint32_t)seconds * 1000);
  esp_restart();
#else
  esp_deep_sleep((uint64_t)seconds * 1000000ULL);
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

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Serial.println("loragate tx-test");

  pinMode(LORA_RFSW, OUTPUT);
  digitalWrite(LORA_RFSW, HIGH);

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  int state = radio.begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ,
                           LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                           LORA_SYNC_WORD, LORA_TX_POWER_DBM,
                           LORA_PREAMBLE_LEN, LORA_TCXO_VOLTAGE, LORA_USE_LDO);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin failed: %d (check lora_pins.h wiring)\n", state);
    deep_sleep(SLEEP_SECONDS);
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

  uint32_t seq = next_seq++;

  bool acked = false;
  uint32_t backoff_ms = INITIAL_BACKOFF_MS;
  for (int attempt = 0; attempt <= MAX_RETRIES && !acked; attempt++) {
    Serial.printf("TX seq=%lu attempt=%d/%d\n", (unsigned long)seq, attempt, MAX_RETRIES);
    LoraPacket pkt = { PKT_DATA_TEST, seq, (uint8_t)attempt };
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

  deep_sleep(SLEEP_SECONDS);
}

void loop() {}
