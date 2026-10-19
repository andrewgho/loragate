// Door-sensor wire packet format, kept byte-identical between tx/ and
// rx/ (Arduino sketches can't share files across directories — `make
// test` diffs the two copies). Enum values are part of the wire
// protocol between already-flashed devices: never renumber an
// existing one, only append.

#pragma once

#include <stdint.h>

enum LoraPacketType : uint8_t {
  PKT_HEARTBEAT      = 1,
  PKT_DOOR_OPEN      = 2,
  PKT_DOOR_CLOSE     = 3,
  PKT_ACK            = 4,
  PKT_DOOR_HELD_OPEN = 5,  // door still open past the held-open check,
                           // one-shot per open episode — see tx/tx.ino
  PKT_DIAGNOSTIC     = 6,  // abnormal boot/reset reason in `info`
                           // (esp_reset_reason_t, or 255 — see tx/tx.ino's
                           // RTC_DATA_LOSS_INFO); not sent on routine wakes
};

struct __attribute__((packed)) LoraPacket {
  uint8_t  type;
  uint32_t seq;
  uint8_t  txno;  // transmit attempt number (0 = first try); unused/0 on acks
  uint8_t  info;  // reserved/0 except on PKT_DIAGNOSTIC — see above
};
