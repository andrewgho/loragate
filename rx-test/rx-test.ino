// loragate rx-test — LoRa send/ack/retry proof of concept (receiver).
//
// Listens forever, acks every valid packet it receives, and reports the
// sequence number to a server over WiFi. Phase 1: no real event types, no
// downstream alerting — see lora_packet.h.

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <RadioLib.h>
#include "lora_pins.h"
#include "lora_packet.h"
#include "local.h"

#define WIFI_TIMEOUT_MS 10000

// 0-100: % chance of silently withholding an ack, to exercise tx-test's
// retry/backoff without needing real radio interference. 0 = never drop.
#define ACK_DROP_PCT 0

#ifdef DEBUG
  #define LOG(fmt, ...) Serial.printf("[DBG] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG(fmt, ...) do {} while (0)
#endif

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool rx_flag = false;
void IRAM_ATTR set_rx_flag() { rx_flag = true; }

void wifi_connect() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("WiFi: connecting to %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("WiFi: connect timed out; will retry on next report");
  }
}

void report_to_server(uint32_t seq, uint8_t txno, float rssi, float snr) {
  wifi_connect();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: not connected; skipping report");
    return;
  }

  char url[256];
  snprintf(url, sizeof(url), REPORT_URL_TEMPLATE,
           (unsigned long)seq, (unsigned)txno, rssi, snr);
  Serial.printf("GET %s\n", url);

  HTTPClient http;
  http.begin(url);
  http.setReuse(false);
  int code = http.GET();
  Serial.printf("Response: %d\n", code);
  http.end();
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Serial.println("loragate rx-test");

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

  wifi_connect();

  radio.setDio1Action(set_rx_flag);
  state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("startReceive failed: %d\n", state);
  }
  Serial.println("listening...");
}

void loop() {
  if (!rx_flag) {
    delay(5);
    return;
  }
  rx_flag = false;

  LoraPacket pkt;
  int st = radio.readData((uint8_t *)&pkt, sizeof(pkt));
  radio.startReceive();  // resume listening immediately, before doing anything slow

  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("readData failed: %d\n", st);
    return;
  }
  if (pkt.type != PKT_DATA_TEST) {
    LOG("ignoring packet type=%d seq=%lu", pkt.type, (unsigned long)pkt.seq);
    return;
  }

  float rssi = radio.getRSSI();
  float snr = radio.getSNR();
  Serial.printf("RX seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                (unsigned long)pkt.seq, pkt.txno, rssi, snr);

  if (ACK_DROP_PCT > 0 && (int)random(100) < ACK_DROP_PCT) {
    Serial.println("ack drop (test): withholding ack");
  } else {
    LoraPacket ack = { PKT_ACK, pkt.seq, 0 };
    int tx_st = radio.transmit((uint8_t *)&ack, sizeof(ack));
    if (tx_st != RADIOLIB_ERR_NONE) {
      Serial.printf("ack transmit failed: %d\n", tx_st);
    }
    radio.startReceive();  // transmit() leaves the radio in standby
  }

  report_to_server(pkt.seq, pkt.txno, rssi, snr);
}
