# Transmitter build plan

Design plan for the real deployed transmitter (phase 2) — event-driven on a
reed switch instead of tx-test's fixed 10s timer, battery-powered, and
implementing the actual door-open/door-closed/left-open/heartbeat protocol
sketched in `README.md`. Physical wiring reference is `WIRING.md`; this doc
covers firmware behavior and the power budget it needs to hit.

First draft came from an AI-authored plan
(`https://alleycat.moychan.com/at/lora_gate_sensor.html`) — see
`WIRING.md`'s "Corrections from the first draft" for what didn't hold up.
The state machine and power-budget structure below are adapted from it; the
numbers are re-derived against what we can actually confirm, not copied.

## Wake behavior

Two wakeup sources armed before every sleep, matching whichever edge is
next expected:

- **Reed switch edge (D0/GPIO0, `esp_sleep_enable_ext0_wakeup`)** — armed
  for the *opposite* of the current state: if the gate is open, wake on
  LOW (closing); if closed, wake on HIGH (opening).
- **RTC timer, only while the gate is open** — 300s (the "left open"
  threshold). If it fires before the switch does, send the
  `GATE_STILL_OPEN` event and re-arm for another 300s, still watching for
  the close edge.

## Rapid-slam / bounce guard

If the gate slams shut while the node is still transmitting the
`GATE_OPEN` packet, the pin can flip before sleep is entered. Re-reading
the pin immediately before committing to `esp_deep_sleep_start()`, and
restarting instead of sleeping if it changed out from under us, avoids
arming a wakeup edge that already happened (which would just sleep through
it and miss the transition):

```c
// Design sketch — not yet implemented. REED_PIN is D0/GPIO0 (WIRING.md).
void enter_safe_deep_sleep() {
  delay(20);  // debounce settle
  bool pin_state = digitalRead(REED_PIN);

  if (pin_state == HIGH) {  // currently open — wake on close
    esp_sleep_enable_ext0_wakeup(REED_PIN, 0);
    esp_sleep_enable_timer_wakeup(300ULL * 1000000ULL);  // left-open threshold
  } else {                  // currently closed — wake on open
    esp_sleep_enable_ext0_wakeup(REED_PIN, 1);
  }

  if (digitalRead(REED_PIN) != pin_state) {
    ESP.restart();  // state changed mid-setup; don't sleep through it
  }

  radio.sleep();  // explicit SX1262 sleep command — ~2.5µA vs standby's more
  esp_deep_sleep_start();
}
```

`esp_sleep_enable_ext0_wakeup()` is confirmed to work for deep sleep on the
C6 via its RTC_IO peripheral (GPIO0-7) — not a given, since the C6's
low-power subsystem is otherwise quite different from classic ESP32 (see
the thermometer repo's notes on why its LP-core sensor sampling idea didn't
pan out on this same chip). Still needs confirming empirically once this is
actually flashed, same as everything else in this doc.

## Power budget

Deep sleep leakage sources, with confidence noted — none of these are
measured yet, they're either datasheet figures or estimates:

| Source | Estimate | Basis |
|---|---|---|
| C6 SoC deep sleep | ~18µA | Espressif datasheet, worst-case temp |
| SX1262 sleep | ~2.5µA | requires explicit `radio.sleep()` before deep sleep — tx-test doesn't do this yet, phase 2 firmware must add it |
| Board passive leakage | ~8µA | estimate, not measured |
| Floating/unused GPIOs | ~6.5µA | mitigated by setting unused pins to `ANALOG`/input-disabled before sleep |
| Reed switch pull-up | ~3.3µA (closed) | external 1MΩ per WIRING.md, not the ~73µA an internal pull-up would cost |
| TPS7A02 LDO | ~0.000025µA | 25nA typical, even in dropout — doesn't move the total |
| Battery-voltage divider + ADC | ~1.3µA (fresh pack, worst case) | 4.7MΩ+2.2MΩ raw-battery divider (~0.78µA) plus the ADS1115's own ~0.5µA typical power-down draw — see WIRING.md's "Battery voltage monitoring" |
| **Total estimate** | **~39µA** | vs. the original plan's ~65µA, mostly because the pull-up choice differs |

Active transmit: ~120ms at ~35mA per event (radio TX current at 22dBm,
datasheet figure) — negligible against sleep current unless the gate cycles
constantly.

At 20 transitions/day (10 open + 10 close) and ~2,100-2,250mAh realistically
extractable from the L91 pack (nominal 3,000mAh derated for cold-weather
internal resistance and brownout headroom — see the L91 datasheet linked in
WIRING.md). Series cells don't add capacity, only voltage, so the 3-cell
pack's mAh figure is the same as a 2-cell pack's would have been — the LDO
regulator draws roughly the same current from its input as it delivers at
its output regardless of input voltage, so the third cell costs nothing
here, per WIRING.md's "Power" section:

```
sleep:  0.039mA * 24h                    ≈ 0.936 mAh/day
active: 20 * 0.120s * 35mA / 3600        ≈ 0.023 mAh/day
total                                    ≈ 0.959 mAh/day

2,100 mAh / (0.959 mAh/day * 365)        ≈ 6.0 years
```

Treat this as optimistic-of-the-corrected-numbers, not a promise — the
individual leakage line items are still estimates. Worth an actual current
measurement (multimeter or a µCurrent-style tool in series with the battery)
once the real transmitter is built, both to sanity-check this table and to
catch anything the table missed.

## Open items

- Firmware for all of this doesn't exist yet — tx-test.ino is a fixed
  10s-timer proof of concept, not event-driven. Phase 2 firmware needs: the
  wake/sleep state machine above, the door-event packet types from
  `README.md`'s phase-2 protocol section, battery voltage via `A1`/`VBAT`,
  and (if added) DS3231 read-on-wake.
- Sleep current is unmeasured — everything in the power budget is a
  datasheet or estimated figure.
- `esp_sleep_enable_ext0_wakeup()` on D0 needs confirming in practice, not
  just via forum/doc reports.
- If the DS3231 is added: confirm which of RTC-reported time vs. receiver
  receipt time actually ends up in the reported event, and whether it's
  worth the extra I2C traffic on every wake given receipt time was already
  judged "close enough" — this is explicitly the lower-priority half of the
  build.
- The LDO added for battery safety (WIRING.md) regulates out the raw battery
  voltage, so `A1`/`VBAT` reads a flat ~3.3V for most of the pack's life
  instead of trending down with it. Fixed in WIRING.md's "Battery voltage
  monitoring" section: a raw-battery divider into a small I2C ADC (ADS1115)
  on D6/D7, since it turned out no ESP32 ADC pin was actually free (see that
  section for why). Hardware designed; firmware to read it over I2C on wake
  and add the value to the heartbeat packet doesn't exist yet. When writing
  it: set the ADS1115's PGA gain register for ±4.096V full scale (not the
  library default ±2.048V) — this is a resolution/accuracy tradeoff in our
  favor, not a hazard like the MCP3421 alternative WIRING.md considered and
  rejected would have been, so getting it wrong just means slightly worse
  (not broken) readings. See WIRING.md's "Why the ADS1115 over the MCP3421
  considered earlier" for the numbers.
