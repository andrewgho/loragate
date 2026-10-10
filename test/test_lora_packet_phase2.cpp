// Host-side test for the phase-2 (tx/rx) door-sensor packet format. No
// ESP32/Arduino toolchain needed — just a C++ compiler. Compiled against
// tx/lora_packet.h; the repo-root `make test` separately diffs that file
// against rx/lora_packet.h to make sure the two required-identical copies
// haven't drifted apart (see the comment atop that header).

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "lora_packet.h"

int main() {
  // Wire size must be exactly 7 bytes (1 + 4 + 1 + 1) with no
  // compiler-inserted padding — tx and rx must agree on this, or
  // RadioLib's raw byte copy desyncs the two ends' interpretation of
  // every field after the first.
  static_assert(sizeof(LoraPacket) == 7, "LoraPacket must be 7 bytes on the wire");
  static_assert(offsetof(LoraPacket, type) == 0, "type must be the first byte");
  static_assert(offsetof(LoraPacket, seq)  == 1, "seq must start at byte 1");
  static_assert(offsetof(LoraPacket, txno) == 5, "txno must be at byte 5");
  static_assert(offsetof(LoraPacket, info) == 6, "info must be the last byte");

  // Enum values are part of the wire protocol between already-flashed
  // devices — pin them down so a future reorder doesn't silently break
  // compatibility.
  static_assert(PKT_HEARTBEAT      == 1, "PKT_HEARTBEAT value changed");
  static_assert(PKT_DOOR_OPEN      == 2, "PKT_DOOR_OPEN value changed");
  static_assert(PKT_DOOR_CLOSE     == 3, "PKT_DOOR_CLOSE value changed");
  static_assert(PKT_ACK            == 4, "PKT_ACK value changed");
  static_assert(PKT_DOOR_HELD_OPEN == 5, "PKT_DOOR_HELD_OPEN value changed");
  static_assert(PKT_DIAGNOSTIC     == 6, "PKT_DIAGNOSTIC value changed");

  // Round-trip through a raw byte buffer, the way RadioLib actually moves
  // this struct over the air (transmit()/readData() take a uint8_t*).
  LoraPacket sent = { PKT_DOOR_OPEN, 0xDEADBEEFu, 2, 0 };
  uint8_t buf[sizeof(LoraPacket)];
  memcpy(buf, &sent, sizeof(buf));

  LoraPacket received;
  memcpy(&received, buf, sizeof(received));
  assert(received.type == PKT_DOOR_OPEN);
  assert(received.seq  == 0xDEADBEEFu);
  assert(received.txno == 2);
  assert(received.info == 0);

  // And the held-open type, to the same depth as the others above.
  LoraPacket held_open_sent = { PKT_DOOR_HELD_OPEN, 42u, 0, 0 };
  memcpy(buf, &held_open_sent, sizeof(buf));
  LoraPacket held_open_received;
  memcpy(&held_open_received, buf, sizeof(held_open_received));
  assert(held_open_received.type == PKT_DOOR_HELD_OPEN);
  assert(held_open_received.seq  == 42u);
  assert(held_open_received.txno == 0);
  assert(held_open_received.info == 0);

  // PKT_DIAGNOSTIC is the one type where `info` carries real data (a reset
  // reason code) rather than sitting at its reserved 0.
  LoraPacket diag_sent = { PKT_DIAGNOSTIC, 7u, 1, 3 };  // info=3 ~ ESP_RST_BROWNOUT
  memcpy(buf, &diag_sent, sizeof(buf));
  LoraPacket diag_received;
  memcpy(&diag_received, buf, sizeof(diag_received));
  assert(diag_received.type == PKT_DIAGNOSTIC);
  assert(diag_received.seq  == 7u);
  assert(diag_received.txno == 1);
  assert(diag_received.info == 3);

  return 0;
}
