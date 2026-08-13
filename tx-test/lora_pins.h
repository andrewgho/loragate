// Pin mapping from the XIAO ESP32-C6 to the Wio-SX1262 expansion board.
//
// Confirmed against the physical boards' own silkscreen (printed pin labels
// on each side of the through-hole headers, read directly off the hardware):
//
//   C6 pin   Wio-SX1262 pin
//   VBUS     VIN
//   GND      GND
//   3V3      3V3
//   D10      MOSI
//   D9       MISO
//   D8       SCK
//   D7       D7    (passthrough, unused)
//   D0       D0    (passthrough, unused)
//   D1       DIO1
//   D2       RST
//   D3       BUSY
//   D4       NSS
//   D5       RF_SW
//   D6       D6    (passthrough, unused)
//
// This superseded an earlier schematic-derived guess that was off by one
// header position on the control-line side (D0-D5 instead of D1-D6) — that
// guess got RADIOLIB_ERR_CHIP_NOT_FOUND (-2) from radio.begin().

#pragma once

#define LORA_DIO1  1   // D1
#define LORA_RST   2   // D2
#define LORA_BUSY  21  // D3
#define LORA_NSS   22  // D4
#define LORA_RFSW  23  // D5 — RF switch /CTRL; drive HIGH once at boot and
                       // leave it (DIO2, handled internally by the SX1262/
                       // RadioLib, does the actual TX/RX switching)
#define LORA_SCK   19  // D8
#define LORA_MISO  20  // D9
#define LORA_MOSI  18  // D10

// DIO3 supplies the module's TCXO reference voltage; per a Seeed forum
// report for this module, that's 1.8V. Wrong TCXO voltage means the
// oscillator never starts and begin() fails without an obvious cause.
#define LORA_TCXO_VOLTAGE 1.8f

// Wio-SX1262 uses the DC-DC power option (not LDO) per Seeed's kit spec.
#define LORA_USE_LDO false

// US ISM band default — change for other regions.
#define LORA_FREQUENCY_MHZ  915.0f
#define LORA_BANDWIDTH_KHZ  125.0f
#define LORA_SPREADING_FACTOR 9
#define LORA_CODING_RATE    7
// "Private LoRa" sync word — a raw literal rather than RadioLib's
// RADIOLIB_SX126X_SYNC_WORD_PRIVATE macro since that name has changed across
// library versions; 0x12 has been stable.
#define LORA_SYNC_WORD       0x12
#define LORA_TX_POWER_DBM   22
#define LORA_PREAMBLE_LEN    8
