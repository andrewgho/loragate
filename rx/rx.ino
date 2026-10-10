// loragate rx — phase-2 door-sensor receiver, always-on test bench.
//
// Listens forever, acks every recognized packet, logs it, and forwards it to
// the monitoring server: door open/close hit /notify/door/gate/{open,close},
// heartbeats hit /log/door/gate/heartbeat, and an abnormal tx reset reason
// (PKT_DIAGNOSTIC — see PLAN.md's "Reset diagnostics") hits
// /log/door/gate/diagnostic/<reason>. Every outgoing request also carries
// localtime=<this node's epoch seconds> (see http_get()). No retries or
// error handling on the HTTP side yet — a failed request just gets logged
// and dropped.

#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <RadioLib.h>
#include <esp_system.h>
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

// Our own short-open alert: fires if the door's been open this long without
// a close, independent of anything the transmitter itself decides to send.
#define SHORT_OPEN_THRESHOLD_SECONDS 45

// How long tx/tx.ino waits before sending PKT_DOOR_HELD_OPEN — needs to
// match its HELD_OPEN_CHECK_SECONDS, since that number isn't on the wire
// (the packet just says "still open", not "still open since when").
#define TX_HELD_OPEN_SECONDS 300

// How long we can go without hearing from the transmitter (any packet
// type) before raising an alert — safely over two of its default hourly
// heartbeat periods, so one missed/collided heartbeat alone won't trip it.
#define STALE_WINDOW_SECONDS 7260

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

// Our own idea of the door state, independent of whatever tx/tx.ino thinks
// — kept so we can raise the short-open alert without waiting on a packet.
bool door_is_open = false;
uint32_t door_opened_at_ms = 0;  // meaningful only while door_is_open
bool short_open_alerted = false; // true once we've reported this open episode

// Staleness tracking — last_rx_ms starts at setup()'s radio.startReceive()
// (not "whenever the first packet arrives"), so silence from boot onward
// counts too, not just silence after first contact.
uint32_t last_rx_ms;
bool stale_alerted = false;      // true once we've reported this silence

// Any real epoch is far larger than this; time(nullptr) reads back as
// just seconds-since-boot (near 0) until the first SNTP reply lands.
#define NTP_SANE_EPOCH 1700000000UL  // 2023-11-14, comfortably in the past
#define NTP_SYNC_WAIT_MS 2000        // bounded — see sync_ntp()

// Requests a clock sync from NTP_SERVER. Passing it for all three of
// configTime()'s server slots (instead of leaning on its pool.ntp.org /
// time.nist.gov defaults for the other two) keeps this from ever depending
// on internet access — every other address in this project is LAN-local too.
void sync_ntp() {
  Serial.printf("NTP: syncing from %s\n", NTP_SERVER);
  configTime(0, 0, NTP_SERVER, NTP_SERVER, NTP_SERVER);
  // configTime() only *starts* the SNTP client — the reply arrives
  // asynchronously, so a request that fires right after this (the startup
  // log, on first connect) can read time(nullptr) before the first reply
  // has landed. Observed in practice as localtime=2 (uptime, not a real
  // epoch) on that very first request. Wait briefly for it to look synced;
  // if the server doesn't answer in time, proceed anyway — localtime is
  // already best-effort everywhere else in this file.
  uint32_t t0 = millis();
  while (time(nullptr) < NTP_SANE_EPOCH && millis() - t0 < NTP_SYNC_WAIT_MS) {
    delay(50);
  }
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
  // wifi_connect() already no-ops when already connected; this is what
  // notices a drop and starts retrying even with no packet or outgoing
  // request around to trigger the lazy reconnect in http_get().
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: disconnected; attempting reconnect");
    wifi_connect();
  }

  if (WiFi.status() == WL_CONNECTED && millis() - last_ntp_sync_ms >= NTP_RESYNC_INTERVAL_MS) {
    sync_ntp();
  }

  if (door_is_open && !short_open_alerted &&
      millis() - door_opened_at_ms >= SHORT_OPEN_THRESHOLD_SECONDS * 1000UL) {
    short_open_alerted = true;
    report_door_open_past(SHORT_OPEN_THRESHOLD_SECONDS, nullptr);
  }

  uint32_t silence_s = (millis() - last_rx_ms) / 1000UL;
  if (!stale_alerted && silence_s >= STALE_WINDOW_SECONDS) {
    stale_alerted = true;
    char url[160];
    snprintf(url, sizeof(url), TRANSMISSION_STALE_URL_TEMPLATE, (unsigned long)silence_s);
    http_get(url);
  }
}

// Performs an HTTP GET against `base_url` plus a localtime=<epoch seconds>
// param reporting this node's current time (query string or bare path,
// either is fine — the separator is chosen to fit). Reconnects WiFi first
// if needed; if that fails, logs it and gives up — no retry on the HTTP
// side, a failed request just gets logged and dropped.
void http_get(const char *base_url) {
  wifi_connect();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: not connected; skipping request");
    return;
  }

  char url[320];
  const char *sep = strchr(base_url, '?') ? "&" : "?";
  snprintf(url, sizeof(url), "%s%slocaltime=%ld", base_url, sep, (long)time(nullptr));

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

// Reports the door held open for `seconds` (DOOR_OPEN_PAST_URL_TEMPLATE's
// path component). `pkt` is the PKT_DOOR_HELD_OPEN packet that prompted
// this, for its seq/txno — pass nullptr for the locally-detected
// SHORT_OPEN_THRESHOLD_SECONDS case, which isn't tied to any one packet.
void report_door_open_past(unsigned seconds, const LoraPacket *pkt) {
  char url[256];
  int n = snprintf(url, sizeof(url), DOOR_OPEN_PAST_URL_TEMPLATE, seconds);
  if (pkt && n > 0 && (size_t)n < sizeof(url)) {
    snprintf(url + n, sizeof(url) - (size_t)n, "?seq=%lu&txno=%u",
             (unsigned long)pkt->seq, (unsigned)pkt->txno);
  }
  http_get(url);
}

// Maps a PKT_DIAGNOSTIC packet's `info` byte (an esp_reset_reason_t cast to
// uint8_t — see tx/tx.ino) to a short name for the log URL's path
// component. Falls back to "unknown" for anything not listed here rather
// than failing — the raw numeric code is also sent separately (see
// DIAGNOSTIC_URL_TEMPLATE's code= param) so a future reset reason this
// table doesn't know about yet is still visible, just not named.
const char *reset_reason_name(uint8_t code) {
  switch ((esp_reset_reason_t)code) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
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
  last_rx_ms = millis();  // staleness clock starts now, not on first packet
  Serial.println("listening...");
}

void handle_packet() {
  // Reached only because the radio's IRQ actually fired — something
  // matching our frequency/sync word came in, whether or not it goes on to
  // decode as a packet we recognize below. That's already "we heard from
  // the transmitter" for staleness purposes, so the clock resets here
  // unconditionally, not only for the packet types we go on to act on.
  last_rx_ms = millis();
  stale_alerted = false;

  LoraPacket pkt;
  int st = radio.readData((uint8_t *)&pkt, sizeof(pkt));
  radio.startReceive();  // resume listening immediately, before doing anything slow

  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("readData failed: %d\n", st);
    return;
  }

  if (pkt.type != PKT_HEARTBEAT && pkt.type != PKT_DOOR_OPEN &&
      pkt.type != PKT_DOOR_CLOSE && pkt.type != PKT_DOOR_HELD_OPEN &&
      pkt.type != PKT_DIAGNOSTIC) {
    LOG("ignoring packet type=%d seq=%lu", pkt.type, (unsigned long)pkt.seq);
    return;
  }

  // Ack every recognized packet so the transmitter stops retrying, before
  // doing anything slower (HTTP) below.
  LoraPacket ack = { PKT_ACK, pkt.seq, 0 };
  int tx_st = radio.transmit((uint8_t *)&ack, sizeof(ack));
  if (tx_st != RADIOLIB_ERR_NONE) {
    Serial.printf("ack transmit failed: %d\n", tx_st);
  }
  radio.startReceive();  // transmit() leaves the radio in standby

  float rssi = radio.getRSSI();
  float snr = radio.getSNR();

  switch (pkt.type) {
    case PKT_HEARTBEAT:
      Serial.printf("RX heartbeat seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      notify_transmitter_event(HEARTBEAT_LOG_URL_TEMPLATE, pkt.seq, pkt.txno);
      break;
    case PKT_DOOR_OPEN:
      Serial.printf("RX door open seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      door_is_open = true;
      door_opened_at_ms = millis();
      short_open_alerted = false;
      notify_transmitter_event(DOOR_OPEN_URL_TEMPLATE, pkt.seq, pkt.txno);
      break;
    case PKT_DOOR_CLOSE:
      Serial.printf("RX door close seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      door_is_open = false;
      short_open_alerted = false;
      notify_transmitter_event(DOOR_CLOSE_URL_TEMPLATE, pkt.seq, pkt.txno);
      break;
    case PKT_DOOR_HELD_OPEN:
      Serial.printf("RX door held open seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      door_is_open = true;  // defensive — should already be true
      report_door_open_past(TX_HELD_OPEN_SECONDS, &pkt);
      break;
    case PKT_DIAGNOSTIC: {
      const char *reason = reset_reason_name(pkt.info);
      Serial.printf("RX diagnostic seq=%lu txno=%u info=%u (%s) rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, pkt.info, reason, rssi, snr);
      char url[256];
      snprintf(url, sizeof(url), DIAGNOSTIC_URL_TEMPLATE, reason,
               (unsigned)pkt.info, (unsigned long)pkt.seq, (unsigned)pkt.txno);
      http_get(url);
      break;
    }
    default:
      break;  // unreachable — filtered above
  }
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
