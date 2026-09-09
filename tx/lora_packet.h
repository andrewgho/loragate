// Phase-2 door-sensor packet format: real event types instead of tx-test's
// PKT_DATA_TEST placeholder. No clock/battery fields yet (WIRING.md/PLAN.md
// plan for those but they're not wired up here) — this is the minimal
// link-layer test bench for heartbeat + door open/close.
//
// Keep this file identical between tx/ and rx/ (Arduino sketches can't share
// files across sketch directories).

#pragma once

#include <stdint.h>

enum LoraPacketType : uint8_t {
  PKT_HEARTBEAT  = 1,
  PKT_DOOR_OPEN  = 2,
  PKT_DOOR_CLOSE = 3,
  PKT_ACK        = 4,
};

struct __attribute__((packed)) LoraPacket {
  uint8_t  type;
  uint32_t seq;
  uint8_t  txno;  // which transmit attempt this is (0 = first try, 1 = first
                  // retry, ...); meaningful on data packets, unused/0 on acks.
};
