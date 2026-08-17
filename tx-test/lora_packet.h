// Minimal link-layer packet format for the tx-test/rx-test proof of concept.
// Not the eventual door-sensor protocol (event type, client time, battery
// voltage) — just enough to prove send/ack/retry over the radio works.
//
// Keep this file identical between tx-test/ and rx-test/ (Arduino sketches
// can't share files across sketch directories).

#pragma once

#include <stdint.h>

enum LoraPacketType : uint8_t {
  PKT_DATA_TEST = 1,
  PKT_ACK       = 2,
};

struct __attribute__((packed)) LoraPacket {
  uint8_t  type;
  uint32_t seq;
  uint8_t  txno;  // PKT_DATA_TEST only: which transmit attempt this is
                  // (0 = first try, 1 = first retry, ...). Unused/0 on acks.
};
