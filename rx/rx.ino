// loragate rx — phase-2 door-sensor receiver, always-on test bench.
//
// Listens forever, acks every recognized packet, logs it, and forwards it to
// the monitoring server: door open/close hit /notify/door/gate/{open,close},
// heartbeats hit /log/door/gate/heartbeat. No retries or error handling on
// the HTTP side yet — a failed request just gets logged and dropped.

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <RadioLib.h>
#include "lora_pins.h"
#include "lora_packet.h"
#include "local.h"

#define WIFI_TIMEOUT_MS 10000

// How often to resync the clock against NTP_SERVER when nothing else
// prompts a sync — see wifi_connect() for the "on every connect" half.
#define NTP_RESYNC_INTERVAL_MS (24UL * 60UL * 60UL * 1000UL)

// How often loop() runs its periodic (not packet-triggered) checks —
// NTP resync due, held-open/staleness thresholds, WiFi health.
#define HOUSEKEEPING_INTERVAL_MS 1000

#ifdef DEBUG
  #define LOG(fmt, ...) Serial.printf("[DBG] " fmt "\n", ##__VA_ARGS__)
#else
  #define LOG(fmt, ...) do {} while (0)
#endif

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

volatile bool rx_flag = false;
void IRAM_ATTR set_rx_flag() { rx_flag = true; }

bool startup_log_sent = false;
uint32_t last_ntp_sync_ms = 0;  // 0 = never synced; see run_housekeeping()

// Requests a clock sync from NTP_SERVER. Passing it for all three of
// configTime()'s server slots (instead of leaning on its pool.ntp.org /
// time.nist.gov defaults for the other two) keeps this from ever depending
// on internet access — every other address in this project is LAN-local too.
void sync_ntp() {
  Serial.printf("NTP: syncing from %s\n", NTP_SERVER);
  configTime(0, 0, NTP_SERVER, NTP_SERVER, NTP_SERVER);
  last_ntp_sync_ms = millis();
}

void wifi_connect() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("WiFi: connecting to %s...\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi: %s\n", WiFi.localIP().toString().c_str());
    sync_ntp();
    if (!startup_log_sent) {
      // Fire-and-forget, right here rather than in setup(), so it fires
      // whichever call happens to be the one that first gets us online —
      // the explicit connect in setup(), or a later lazy reconnect from
      // http_get(). http_get() calls wifi_connect() too; by the time it
      // does, WiFi is already up and this block won't run again, so
      // there's no recursion loop.
      startup_log_sent = true;
      http_get(STARTUP_LOG_URL);
    }
  } else {
    Serial.println("WiFi: connect timed out; will retry on next request");
  }
}

// Periodic checks that don't wait on a radio packet — called every loop()
// iteration, throttled to HOUSEKEEPING_INTERVAL_MS internally.
void run_housekeeping() {
  if (WiFi.status() == WL_CONNECTED && millis() - last_ntp_sync_ms >= NTP_RESYNC_INTERVAL_MS) {
    sync_ntp();
  }
}

// Performs an HTTP GET against `url`, which must already be the fully
// formed request URL. Reconnects WiFi first if needed; if that fails, logs
// it and gives up — no retry on the HTTP side, a failed request just gets
// logged and dropped.
void http_get(const char *url) {
  wifi_connect();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: not connected; skipping request");
    return;
  }

  Serial.printf("GET %s\n", url);

  HTTPClient http;
  http.begin(url);
  http.setReuse(false);
  int code = http.GET();
  Serial.printf("Response: %d\n", code);
  http.end();
}

// Formats a proxied transmitter-event URL (carrying seq/txno placeholders,
// in that order) and sends it.
void notify_transmitter_event(const char *url_template, uint32_t seq, uint8_t txno) {
  char url[256];
  snprintf(url, sizeof(url), url_template, (unsigned long)seq, (unsigned)txno);
  http_get(url);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Serial.println("loragate rx");

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

void handle_packet() {
  LoraPacket pkt;
  int st = radio.readData((uint8_t *)&pkt, sizeof(pkt));
  radio.startReceive();  // resume listening immediately, before doing anything slow

  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("readData failed: %d\n", st);
    return;
  }

  float rssi = radio.getRSSI();
  float snr = radio.getSNR();

  const char *url_template;
  switch (pkt.type) {
    case PKT_HEARTBEAT:
      Serial.printf("RX heartbeat seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      url_template = HEARTBEAT_LOG_URL_TEMPLATE;
      break;
    case PKT_DOOR_OPEN:
      Serial.printf("RX door open seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      url_template = DOOR_OPEN_URL_TEMPLATE;
      break;
    case PKT_DOOR_CLOSE:
      Serial.printf("RX door close seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      url_template = DOOR_CLOSE_URL_TEMPLATE;
      break;
    default:
      LOG("ignoring packet type=%d seq=%lu", pkt.type, (unsigned long)pkt.seq);
      return;
  }

  LoraPacket ack = { PKT_ACK, pkt.seq, 0 };
  int tx_st = radio.transmit((uint8_t *)&ack, sizeof(ack));
  if (tx_st != RADIOLIB_ERR_NONE) {
    Serial.printf("ack transmit failed: %d\n", tx_st);
  }
  radio.startReceive();  // transmit() leaves the radio in standby

  notify_transmitter_event(url_template, pkt.seq, pkt.txno);
}

void loop() {
  if (rx_flag) {
    rx_flag = false;
    handle_packet();
  }

  static uint32_t last_housekeeping_ms = 0;
  uint32_t now = millis();
  if (now - last_housekeeping_ms >= HOUSEKEEPING_INTERVAL_MS) {
    last_housekeeping_ms = now;
    run_housekeeping();
  }

  delay(5);
}
