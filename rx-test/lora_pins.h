// Pin mapping from the XIAO ESP32-C6 to the Wio-SX1262 expansion board.
//
// Seeed only officially sells the Wio-SX1262 paired with the XIAO ESP32S3
// (through-hole pin header variant, which is what we have) or the XIAO
// nRF52840; there is no official ESP32C6 pairing, so this mapping is
// DERIVED, not vendor-confirmed or empirically tested yet:
//
//   1. Seeed's schematic for the module ("Schematic_Diagram_Wio-SX1262_for_
//      XIAO.pdf") shows its two 1x7 2.54mm headers wired to the SX1262 as:
//        J1 pin 1..5: LORA_DIO1, LORA_BUSY, LORA_RST, LORA_SPI_NSS, LORA_RF_SW1
//        J2 pin 1..6: VIN, GND, +3V3, LORA_SPI_MOSI, LORA_SPI_MISO, LORA_SPI_SCK
//   2. Seeed's schematic for the plain XIAO ESP32-C6 module shows its two
//      7-pin edges as:
//        side A pin 1..7: D0, D1, D2, D3, D4, D5, D6
//        side B pin 1..7: VBUS, GND, 3V3, D10, D9, D8, D7
//      J2's VIN/GND/+3V3 in positions 1-3 line up exactly with the C6's
//      VBUS/GND/3V3 in the same positions on side B — strong confirmation
//      the two headers mate position-for-position, unmirrored. That gives
//      SCK/MISO/MOSI on D8/D9/D10 (the pins already silkscreened SCK/MISO/
//      MOSI on every XIAO board) and the control lines on D0-D4 below.
//
// A community project pairing a C6 with this same module separately reports
// RST on D2 (matches what's derived here) but NSS on D5 (conflicts with the
// D3 derived here) — so treat NSS/BUSY/DIO1 as the least certain of the
// bunch. If the radio never reports a successful begin(), that's the first
// thing to re-check (bare board, continuity-test J1 against the C6
// castellations with a multimeter).

#pragma once

#define LORA_DIO1  0   // D0
#define LORA_BUSY  1   // D1
#define LORA_RST   2   // D2
#define LORA_NSS   21  // D3
#define LORA_RFSW  22  // D4 — RF switch /CTRL; drive HIGH once at boot and
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
