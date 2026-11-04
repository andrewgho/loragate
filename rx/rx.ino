// loragate rx — always-on door-sensor receiver.
//
// Listens forever, acks every recognized packet, and forwards each event
// to the monitoring server over HTTP (URLs in local.h; protocol and
// architecture in README.md's Software section). No persistent queue: an
// event that outlasts http_get()'s retries is logged and dropped.

#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <RadioLib.h>
#include <esp_system.h>
#include "lora_pins.h"
#include "lora_packet.h"
#include "seq_dedup.h"
#include "local.h"

#define WIFI_TIMEOUT_MS 10000

// Held-open warning beacon: drives a low-side MOSFET switching the
// beacon's 5V (circuit in WIRING.md's Receiver section). D0, not D6/D7: those are
// UART0, which this core's prebuilt ESP-IDF console claims regardless of
// CDCOnBoot. D0 shares a net with the Wio HAT's onboard button — don't
// press it while this drives HIGH (see WIRING.md).
#define BEACON_PIN 0  // D0/GPIO0

// Retries (after the first try) for one HTTP GET on a transient failure —
// see http_get().
#define HTTP_GET_RETRIES 2
#define HTTP_GET_RETRY_DELAY_MS 750

// How long to wait between radio.begin() retries at boot if the radio
// doesn't come up right away.
#define RADIO_BEGIN_RETRY_MS 5000UL

// Consecutive failed startReceive() retries (one per housekeeping tick)
// before escalating to a full radio_reinit() — repeated failure suggests
// the chip itself is wedged.
#define STARTRECEIVE_REINIT_AFTER_FAILURES 3

// How often loop() runs its periodic (not packet-triggered) checks —
// held-open/staleness thresholds, WiFi health.
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
bool mdns_started = false;

// Our own idea of the door state, independent of whatever tx/tx.ino thinks
// — kept so we can raise the short-open alert without waiting on a packet.
bool door_is_open = false;
uint32_t door_opened_at_ms = 0;  // meaningful only while door_is_open
bool short_open_alerted = false; // true once we've reported this open episode

// Testing phase: lit whenever the door's open. Intended final behavior is
// lit only once a held-open alert has fired (e.g. `return
// short_open_alerted;`); changing just this function is enough, since the
// pin is only ever set via update_beacon(), which runs every housekeeping
// tick.
bool beacon_should_be_on() {
  return door_is_open;
}

void update_beacon() {
  digitalWrite(BEACON_PIN, beacon_should_be_on() ? HIGH : LOW);
}

// Staleness clock — set at the end of setup(), so silence from boot
// counts too, not just silence after first contact.
uint32_t last_rx_ms;
bool stale_alerted = false;      // true once we've reported this silence

// Dedup for handle_packet() (see seq_dedup.h). Not RTC-persisted: rx
// rarely reboots, and losing it costs at most reprocessing one retry.
bool have_last_seq = false;
uint32_t last_seq = 0;

// Whether the radio is currently believed to be listening — see
// ensure_receiving(). Starts true so a successful first startReceive()
// at boot doesn't look like a "recovery" with nothing to recover from.
bool radio_listening = true;

void wifi_connect() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("WiFi: connecting to %s...\n", WIFI_SSID);
  // Must precede WiFi.begin().
  WiFi.setHostname(RX_HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi: %s as %s\n", WiFi.localIP().toString().c_str(), RX_HOSTNAME);
    // Once only: mDNS follows the netif through reconnects on its own.
    if (!mdns_started) {
      mdns_started = MDNS.begin(RX_HOSTNAME);
      Serial.printf("mDNS: %s\n", mdns_started ? RX_HOSTNAME ".local" : "failed to start");
    }
    if (!startup_log_sent) {
      // Here rather than in setup(), so it fires on whichever call first
      // gets us online (setup()'s, or a lazy one from http_get()). No
      // recursion: WiFi is already up when http_get() calls back in.
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
  // Notices a drop even with no request around to trigger http_get()'s
  // lazy reconnect.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: disconnected; attempting reconnect");
    wifi_connect();
  }

  // Once startReceive() has failed no IRQ fires again, so this is the
  // only recovery path: a few plain retries, then a full radio_reinit().
  if (!radio_listening) {
    static uint8_t consecutive_failures = 0;
    ensure_receiving();
    if (radio_listening) {
      consecutive_failures = 0;
    } else if (++consecutive_failures >= STARTRECEIVE_REINIT_AFTER_FAILURES) {
      consecutive_failures = 0;
      Serial.println("startReceive still failing; reinitializing radio");
      radio_reinit();
    }
  }

  if (door_is_open && !short_open_alerted &&
      millis() - door_opened_at_ms >= SHORT_OPEN_THRESHOLD_SECONDS * 1000UL) {
    short_open_alerted = true;
    report_door_open_past(SHORT_OPEN_THRESHOLD_SECONDS, nullptr);
  }

  // Backstop for the handle_packet() call sites: re-evaluated every tick
  // so the pin can't drift out of sync with the door state.
  update_beacon();

  uint32_t silence_s = (millis() - last_rx_ms) / 1000UL;
  if (!stale_alerted && silence_s >= STALE_WINDOW_SECONDS) {
    stale_alerted = true;
    char url[256];
    snprintf(url, sizeof(url), TRANSMISSION_STALE_URL_TEMPLATE, (unsigned long)silence_s);
    http_get(url);
  }
}

// HTTP GET, retried up to HTTP_GET_RETRIES times on a transient failure:
// a negative code (HTTPClient connection error — timeout, refused, DNS)
// or 5xx. A 4xx means a bad URL, so no retry. Connects WiFi first if
// needed, and skips the request if that fails.
void http_get(const char *base_url) {
  wifi_connect();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi: not connected; skipping request");
    return;
  }

  int code = 0;
  for (int attempt = 0; attempt <= HTTP_GET_RETRIES; attempt++) {
    Serial.printf("GET %s%s\n", base_url, attempt > 0 ? " (retry)" : "");
    HTTPClient http;
    http.begin(base_url);
    http.setReuse(false);
    code = http.GET();
    http.end();

    bool transient_failure = (code < 0 || code >= 500);
    if (!transient_failure) break;
    if (attempt < HTTP_GET_RETRIES) {
      Serial.printf("transient failure (%d); retrying in %dms\n", code, HTTP_GET_RETRY_DELAY_MS);
      delay(HTTP_GET_RETRY_DELAY_MS);
    }
  }
  Serial.printf("Response: %d\n", code);
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

// Percent-encodes spaces (the only special character our own fixed-format
// condition messages ever contain) while copying src into dst — a raw
// space in an HTTP request's path is invalid and breaks the request line.
void url_encode_spaces(char *dst, size_t dst_size, const char *src) {
  size_t out = 0;
  for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; i++) {
    if (src[i] == ' ' && out + 3 < dst_size) {
      dst[out++] = '%';
      dst[out++] = '2';
      dst[out++] = '0';
    } else if (src[i] != ' ') {
      dst[out++] = src[i];
    }
  }
  dst[out] = '\0';
}

// Reports an rx-side error/recovery condition (ERROR_URL_TEMPLATE /
// RECOVERY_URL_TEMPLATE) directly to the server — rx has WiFi, unlike tx,
// so it doesn't need a wire-protocol diagnostic packet to report on itself.
void report_rx_condition(const char *url_template, const char *message) {
  char encoded[128];
  url_encode_spaces(encoded, sizeof(encoded), message);
  char url[256];
  snprintf(url, sizeof(url), url_template, encoded);
  http_get(url);
}

// 255 isn't a real esp_reset_reason_t value — tx.ino uses it to flag
// persisted RTC state that didn't survive despite a reset reason that
// claimed it should have. Must match RTC_DATA_LOSS_INFO in tx/tx.ino.
#define RTC_DATA_LOSS_INFO 255

// Maps a PKT_DIAGNOSTIC packet's `info` byte (an esp_reset_reason_t, or
// RTC_DATA_LOSS_INFO) to a short name for the log URL. Unlisted codes map
// to "unknown"; the raw code is sent too (DIAGNOSTIC_URL_TEMPLATE's
// code=), so they're still visible.
const char *reset_reason_name(uint8_t code) {
  if (code == RTC_DATA_LOSS_INFO) return "rtc_data_loss";
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

// radio.begin() with the project's fixed parameters, retried every
// RADIO_BEGIN_RETRY_MS until it succeeds — rx runs unattended, so a
// transient fault (e.g. the radio rail not yet settled at power-on)
// should self-heal rather than halt. Reports one error on the first
// failure and one recovery on eventual success, not one per retry.
void radio_begin_with_retry() {
  int state = radio.begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ,
                           LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                           LORA_SYNC_WORD, LORA_TX_POWER_DBM,
                           LORA_PREAMBLE_LEN, LORA_TCXO_VOLTAGE, LORA_USE_LDO);
  bool failed = (state != RADIOLIB_ERR_NONE);
  if (failed) {
    char msg[64];
    snprintf(msg, sizeof(msg), "radio initialization failed with code %d", state);
    report_rx_condition(ERROR_URL_TEMPLATE, msg);
  }
  while (state != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin failed: %d (check lora_pins.h wiring); retrying in %lus\n",
                  state, RADIO_BEGIN_RETRY_MS / 1000UL);
    delay(RADIO_BEGIN_RETRY_MS);
    state = radio.begin(LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ,
                         LORA_SPREADING_FACTOR, LORA_CODING_RATE,
                         LORA_SYNC_WORD, LORA_TX_POWER_DBM,
                         LORA_PREAMBLE_LEN, LORA_TCXO_VOLTAGE, LORA_USE_LDO);
  }
  if (failed) {
    report_rx_condition(RECOVERY_URL_TEMPLATE, "radio initialization recovered");
  }
  Serial.println("radio init OK");
}

// One attempt at (re)starting receive. Reports error/recovery only when
// radio_listening flips, so it's safe to call as often as needed.
// Retrying is run_housekeeping()'s job.
void ensure_receiving() {
  int state = radio.startReceive();
  if (state == RADIOLIB_ERR_NONE) {
    if (!radio_listening) {
      radio_listening = true;
      report_rx_condition(RECOVERY_URL_TEMPLATE, "radio receive recovered");
    }
    return;
  }
  Serial.printf("startReceive failed: %d\n", state);
  if (radio_listening) {
    radio_listening = false;
    char msg[64];
    snprintf(msg, sizeof(msg), "radio startReceive failed with code %d", state);
    report_rx_condition(ERROR_URL_TEMPLATE, msg);
  }
}

// Full radio bring-up, used by setup() and by run_housekeeping() when
// plain startReceive() retries aren't enough. begin() resets
// DIO2-as-RF-switch and the DIO1 callback, so both are re-armed here.
void radio_reinit() {
  radio_begin_with_retry();

  // Without this the antenna switch never moves: transmit() and
  // startReceive() report success but nothing reaches the antenna.
  int state = radio.setDio2AsRfSwitch(true);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("setDio2AsRfSwitch failed: %d\n", state);
  }

  radio.setDio1Action(set_rx_flag);
  ensure_receiving();
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Serial.println("loragate rx");

  pinMode(LORA_RFSW, OUTPUT);
  digitalWrite(LORA_RFSW, HIGH);

  pinMode(BEACON_PIN, OUTPUT);
  update_beacon();  // off at boot — door_is_open starts false

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  radio_reinit();

  // After radio_reinit(), so the radio is already listening while this
  // blocks for up to WIFI_TIMEOUT_MS. (Any report_rx_condition() above
  // connects lazily via http_get().)
  wifi_connect();

  last_rx_ms = millis();  // staleness clock starts now, not on first packet
  Serial.println("listening...");
}

void handle_packet() {
  // Any IRQ resets the silence clock, but only a packet that decodes as a
  // known type re-arms stale_alerted (below): a CRC failure may just be
  // RF noise, which mustn't suppress the alert while tx is dead.
  last_rx_ms = millis();

  LoraPacket pkt;
  int st = radio.readData((uint8_t *)&pkt, sizeof(pkt));
  ensure_receiving();  // resume listening immediately, before doing anything slow

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

  // Even a duplicate retry is real evidence tx is alive.
  stale_alerted = false;

  // Ack every recognized packet so the transmitter stops retrying, before
  // doing anything slower (HTTP) below.
  LoraPacket ack = { PKT_ACK, pkt.seq, 0, 0 };
  int tx_st = radio.transmit((uint8_t *)&ack, sizeof(ack));
  if (tx_st != RADIOLIB_ERR_NONE) {
    Serial.printf("ack transmit failed: %d\n", tx_st);
  }
  ensure_receiving();  // transmit() leaves the radio in standby

  float rssi = radio.getRSSI();
  float snr = radio.getSNR();

  // An ack-loss retry resends the same seq. It's re-acked above (so tx
  // stops retrying) but not reprocessed: that would double-notify the
  // server and, for PKT_DOOR_OPEN, restart door_opened_at_ms.
  if (is_duplicate_seq(have_last_seq, last_seq, pkt.seq)) {
    LOG("duplicate seq=%lu txno=%u type=%d; already processed, ack only",
        (unsigned long)pkt.seq, pkt.txno, pkt.type);
    return;
  }
  have_last_seq = true;
  last_seq = pkt.seq;

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
      update_beacon();
      notify_transmitter_event(DOOR_OPEN_URL_TEMPLATE, pkt.seq, pkt.txno);
      break;
    case PKT_DOOR_CLOSE:
      Serial.printf("RX door close seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      door_is_open = false;
      short_open_alerted = false;
      update_beacon();
      notify_transmitter_event(DOOR_CLOSE_URL_TEMPLATE, pkt.seq, pkt.txno);
      break;
    case PKT_DOOR_HELD_OPEN:
      Serial.printf("RX door held open seq=%lu txno=%u rssi=%.1fdBm snr=%.1fdB\n",
                    (unsigned long)pkt.seq, pkt.txno, rssi, snr);
      door_is_open = true;  // defensive — should already be true
      update_beacon();
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
