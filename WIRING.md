# Transmitter wiring

Physical build reference for the outdoor transmitter: XIAO ESP32-C6 + Wio-SX1262
stack, magnetic reed switch, 3x AA lithium primary cells through a nanopower
LDO, optional DS3231 RTC.
Not yet built or firmware-tested — this is the plan, cross-checked against the
confirmed radio pinout in `tx-test/lora_pins.h`/`rx-test/lora_pins.h` and
Seeed's own docs. First draft came from an AI-authored plan
(`https://alleycat.moychan.com/at/lora_gate_sensor.html`); several things in
it didn't hold up against our actual hardware and are corrected below — see
"Corrections from the first draft."

## Pin table

All 14 XIAO header pins, one source of truth:

| XIAO pin | GPIO | Use |
|---|---|---|
| D0 | GPIO0 | **Reed switch** (see below for why this pin specifically) |
| D1 | GPIO1 | LoRa DIO1 |
| D2 | GPIO2 | LoRa RST |
| D3 | GPIO21 | LoRa BUSY |
| D4 | GPIO22 | LoRa NSS |
| D5 | GPIO23 | LoRa RF_SW |
| D6 | GPIO16 | free — **RTC SDA** if added |
| D7 | GPIO17 | free — **RTC SCL** if added |
| D8 | GPIO19 | LoRa SCK |
| D9 | GPIO20 | LoRa MISO |
| D10 | GPIO18 | LoRa MOSI |
| 3V3 | — | LDO regulated output (see "Power" below — not a direct battery connection) |
| GND | — | battery − / reed switch return / RTC GND |
| VBUS/5V | — | unused (no USB power in the field) |

D1-D5 and D8-D10 are fixed by the Wio-SX1262 stack (confirmed by reading the
boards' silkscreen — see `lora_pins.h`), leaving D0, D6, D7 free.

## Mounting the stack

The XIAO ESP32-C6 + Wio-SX1262 stack mounts on an
[Adafruit Perma-Proto Quarter-sized breadboard PCB](https://www.digikey.com/en/products/detail/adafruit-industries-llc/1608/5154676)
(1.7" x 2.0", breadboard-style bus strips rather than isolated pads — chosen
over a bare 9x9-hole protoboard specifically for the room to fit the LDO
adapter, resistor, caps, and JST-PH pads alongside the stack's socket rather
than needing a second board): female pin headers soldered onto the
protoboard as a socket, with the Wio module (the bottom half of the stack,
its male header pins facing down) plugging into that socket. Once seated,
all 14 XIAO signals (D0-D10, 3V3, GND, VBUS) are available as ordinary
through-hole solder points on the protoboard's underside — standard female
header strips have their pins protrude slightly on the solder side — which
is where the reed switch leads, the D0 pull-up, and the LDO's regulated
output all land.

### Why the reed switch is on D0, not an arbitrary free pin

Waking from deep sleep on a GPIO edge (`esp_sleep_enable_ext0_wakeup()`)
requires an RTC-domain pin. On the ESP32-C6 that's GPIO0-7 only. Of the three
free header pins, D0 (GPIO0) is the only one in that range — D6/D7 (GPIO16/17)
are not RTC-capable, so they're only useful for something read/written while
already awake, like an RTC's I2C bus. That's also why the reed switch has to
be D0 specifically: it's not a free choice, it's the only remaining option
once the radio has claimed D1/D2 (the other two RTC-capable header pins).

GPIO0 is not a strapping/boot-select pin on the C6 (unlike classic ESP32,
where it is) — confirmed against Seeed's own schematic — so there's no boot
hazard to worry about here.

### Reed switch circuit

Standard alarm-style reed switch: contacts closed (continuity) when the
magnet is present (gate closed), open when the magnet moves away (gate open).
D0 needs a pull-up to read a clean HIGH when open.

**Recommendation: external 1MΩ resistor from D0 to 3V3, internal pull-ups
explicitly disabled in firmware.** The original plan used the ESP32's
internal ~45kΩ pull-up and just remembered to disable it before every sleep
entry — that leaks ~73µA whenever the gate is closed if the disable is ever
skipped, and it's a recurring "did the code definitely run this line before
sleeping" hazard on every code path that reaches `esp_deep_sleep_start()`,
including future ones we haven't written yet. An external 1MΩ resistor with
internal pull-ups permanently disabled leaks ~3.3µA regardless of code path,
is one resistor, and can't regress. The C6's LP-domain input buffers are
high-impedance enough that 1MΩ still gives a clean logic level over the
enclosure-to-switch wire run; if the real installation turns out to be
electrically noisy (long run, near AC wiring), drop to 100kΩ before adding
back software pull-up juggling.

## Power

**3x AA Energizer Ultimate Lithium (L91) cells in series, through a
TPS7A0233 nanopower LDO, into the XIAO's 3V3/GND header pins — not the
board's dedicated BAT solder pads, and not a direct battery connection.**

### Why primary lithium over rechargeable Li-ion/LiPo

The XIAO's onboard charge circuit (the BAT pads, see below) makes
rechargeable Li-ion/LiPo the path of least resistance on this board, so it's
worth stating explicitly why this build isn't using it. Deployment climate
here has no hard freezes, so L91's cold-weather edge doesn't matter — but
summers reach ~90°F ambient, and heat is where the tradeoff actually cuts
*against* Li-ion: L91 is rated to 140°F with only mild, already-budgeted
capacity derating (see `PLAN.md`), while Li-ion/LiPo capacity fade
accelerates substantially with sustained heat, and a sealed IP67 box in
direct sun will run hotter inside than the ambient air. Primary cylindrical
cells also carry none of pouch-cell Li-ion's puncture/swelling/thermal-
runaway failure modes, which age worse with heat exposure — matching the
alarm industry's own convention of shipping door/window sensors on primary
lithium, not rechargeables, for the same reasons.

### Why not a direct connection (the original plan's approach)

The C6's exposed **3V3 header pin is wired directly to the ESP32-C6's VDD**
— confirmed on a Seeed forum thread specifically about this pin — there's no
protection or regulation in that direction. Whatever voltage is put there
*is* the chip's core supply voltage. The datasheet-recommended range is
3.0-3.6V. A fresh L91 cell sits close to 1.8V under light load (per
Energizer's own discharge-profile chart, not the 1.5V nominal figure), so
two fresh cells in series can be close to 3.6V right out of the pack —
zero margin against the chip's own ceiling on day one, before accounting for
pack-to-pack or temperature variance. That ruled out wiring the battery
straight to 3V3 as the original plan did.

The **BAT pads** (separate solder points on the board's underside, near the
D5/D8 silkscreen markers) aren't an alternative either — they're wired to an
onboard Li-ion/LiPo charge controller tuned for a rechargeable 3.7V cell,
which will try to charge whatever's connected there. Primary cells aren't
designed to be recharged.

### The fix: TPS7A0233 LDO + a third cell

[TI TPS7A02](https://www.ti.com/lit/ds/symlink/tps7a02.pdf), 3.3V fixed
output (part suffix `33`), SOT23-5 package (`DBV`) — 25nA quiescent current
even in dropout, 1.5-6.0V input range, 270mV max dropout at a full 200mA
load (far less at our currents). Ordering as
[TPS7A0233PDBVR](https://www.digikey.com/en/products/detail/texas-instruments/TPS7A0233PDBVR/12165099)
— the `P` before the package code is TI's convention for "built at a newer
assembly/test site," not a spec or pinout difference (confirmed against TI's
own explanation of the same convention on a different part family);
`TPS7A0233DBVR` without the `P` is the same device and works identically.
Going to 3 cells instead of 2 puts the
fresh-pack voltage (~5.1-5.4V) safely under the LDO's 6.0V input ceiling
with room to spare, and keeps the LDO in regulation through nearly the
entire discharge curve rather than just clipping the fresh-cell peak. This
doesn't cost anything in mAh-based battery life — a linear regulator draws
roughly the same current from its input as it delivers at its output,
regardless of the input voltage, so battery life (capacity ÷ current drawn)
is unaffected by adding a third cell in series.

No one sells a ready-made breakout for this (or any other nanopower-tier
LDO) — it's an SMD-chip-only market segment. The practical build path is a
**bare TPS7A0233PDBVR chip hand-soldered onto a SOT23-5-to-DIP adapter
board** — this needs no reflow or hot air, just a fine-tip iron, same as any
other through-hole work, and turns the bare chip into a
breadboard/protoboard-compatible DIP-pitch part.

Confirmed part: **Proto Advantage / Chip Quik PA0086C** ("SOT23-5 to DIP-6
SMT Adapter, 0.95mm pitch, Compact Series"; [DigiKey PA0086C](https://www.digikey.com/en/products/detail/chip-quik-inc/PA0086C/12352102))
— checked against its datasheet:
0.100" pin pitch and 0.300" row spacing (standard DIP-6/breadboard grid,
drops straight into any 0.1"-grid protoboard), DIP pins factory-installed
(gold-plated, 0.46mm diameter — thinner than a typical resistor lead, so no
tight fit going through protoboard holes). Nothing to add beyond the bare
chip itself; solder its 5 leads onto the adapter's top-side SMT pads and the
bottom-side DIP pins are ready to go straight into the protoboard.
Equivalent adapters from SparkFun/Schmartboard/SchmalzTech/generic
multi-packs should work the same way — just confirm 0.1" pin pitch before
buying, since some cheap SMD-to-DIP adapters use denser/nonstandard spacing.

Wiring (by pin *function* — the adapter board's own physical pad layout and
labeling varies by vendor, so match against whichever one is on hand rather
than assuming pin-for-pin correspondence with the datasheet's SOT23-5
numbering below):

| TPS7A02 pin | Connects to |
|---|---|
| 1 (IN) | battery + (through the JST-PH disconnect, see below) |
| 2 (GND) | battery − / system GND |
| 3 (EN) | tied to pin 1 (IN) — EN has an internal pulldown and defaults *off* if left floating, so it must be actively driven high to enable the regulator |
| 4 (NC) | unconnected |
| 5 (OUT) | XIAO 3V3 pin (lands on the protoboard's 3V3 socket pad — see "Mounting the stack" above) |

Needs a ~1µF ceramic capacitor from IN to GND and another from OUT to GND,
both placed close to the chip (datasheet minimum is 0.5µF effective on OUT
for stability; 1µF+ is recommended for transient response, which matters
here given the LoRa TX current step) — these can land on the adapter board
itself if it breaks out IN/OUT/GND to separate pads with room to solder a
0805/0603 cap across them, or just off-board on the same protoboard as
everything else.

**Still keep the JST-PH 2.0mm disconnect** between the battery pack and the
LDO's input, and still disconnect it before ever connecting USB. The LDO
means a backfeed is now two regulated 3.3V sources contending on one node
instead of raw unprotected battery voltage hitting VDD directly — safer,
but still not a scenario to rely on rather than avoid.

**Adding the LDO costs us the battery gauge, though.** `A1`/`VBAT` reads the
board's own supply rail, which after the LDO is the *regulated* 3.3V output,
not the raw battery voltage — it'll read a flat ~3.3V for most of the pack's
life and only start to sag once the pack drops close to the LDO's dropout
point, right near end of life. That's not enough resolution to trend battery
health the way the protocol's heartbeat message is meant to. If that
matters, the fix is a simple resistor divider from the *raw* battery (before
the LDO) into a spare ADC-capable pin — D6 or D7, whichever isn't claimed by
the DS3231 if that gets added — sized to bring the ~2.4-5.4V pack range down
under the ADC's ~3.3V ceiling. Not designed yet; flagging it here since it's
a direct consequence of the LDO decision above, not a separate nice-to-have.

## Optional: DS3231 RTC

Opportunistic addition on the two pins left over (D6/D7 as I2C SDA/SCL via
`Wire.begin(SDA, SCL)` with explicit pins — the pins don't need to be the
chip's default I2C pins since the ESP32 can route I2C to any GPIO through
its peripheral matrix). Lower priority than everything else here — reported
receive time is probably close enough for door events, so this is worth
adding only if it stays this cheap.

**Caveat if you go this route: don't use a stock DS3231 breakout board
as-is.** The common ~$2 eBay/Amazon modules include an onboard CR2032
trickle-charge circuit and a power LED, both of which draw well beyond what
the DS3231 chip itself needs and would meaningfully eat into the sleep
budget in `PLAN.md`. Either use a bare DS3231 (or DS3231M, no external
crystal needed) directly, or a breakout confirmed to have the charge
resistor and LED removed/absent.

## Enclosure and cable routing

Kept from the original plan — these parts didn't depend on getting the
pinout right and hold up fine:

- IP67 ABS junction box, ~125 x 75 x 75mm, all penetrations on the bottom
  face only: SMA bulkhead (external antenna, screwed on pointing straight
  down — keeps the connector shielded from pooling water and the radiation
  pattern horizontal) and a PG7 cable gland for the sensor lead.
- 22/2 white UV-rated cable to the reed switch, joined to the switch's
  pigtail with adhesive solder-seal heat-shrink sleeves.
- Reed switch: Interlogix 1085W-N or Honeywell 7945WH, fully potted, 1.5"
  gap — wide gap resists false triggers from gate rattle/wind.
- Cable secured to the underside of the gate's lowest rail with small dabs
  of GE Silicone II every 12-18", or outdoor acrylic tape — both removable
  without damaging paint/powder coat if the install needs to move.

## Bill of materials

| Part | Qty | Notes |
|---|---|---|
| XIAO ESP32-C6 | 1 | already have |
| Wio-SX1262 (for XIAO ESP32S3, through-hole variant) | 1 | already have |
| Adafruit Perma-Proto Quarter-sized breadboard PCB | 1 | mounts the whole build — see "Mounting the stack" |
| 2x7 female pin headers, 2.54mm pitch | 1 pair | socket for the stack, soldered to the protoboard |
| Interlogix 1085W-N or Honeywell 7945WH reed switch | 1 | potted, 1.5" gap |
| 3x AA Energizer Ultimate Lithium (L91) | 1 pack | ~3,000mAh nominal, ~1.0-1.8V/cell across life |
| [Keystone Electronics 2465](https://www.digikey.com/en/products/detail/keystone-electronics/2465/303814) 3xAA holder | 1 | series-wired (4.5V), 6" bare leads, chassis mount, no cover |
| TPS7A0233PDBVR (TI, SOT23-5, 3.3V fixed LDO) | 1 | bare chip — see "Power" |
| Proto Advantage / Chip Quik PA0086C SOT23-5-to-DIP-6 adapter | 1 | confirmed 0.1" pitch, pins pre-installed — see "Power" |
| 1µF ceramic capacitor | 2 | TPS7A02 IN and OUT bypass |
| JST-PH 2.0mm 2-pin connector pair | 1 | battery disconnect for USB safety |
| 1MΩ resistor | 1 | D0 external pull-up |
| IP67 ABS junction box, ~125x75x75mm | 1 | |
| SMA bulkhead + waterproof 915MHz antenna | 1 | |
| PG7 cable gland | 1 | |
| 22/2 white UV cable | as needed | ~50ft roll |
| Solder-seal heat-shrink sleeves | a few | splice to switch pigtail |
| DS3231(M) RTC, bare chip or de-modded breakout | 1 | optional |

## Corrections from the first draft

The AI-authored plan this was based on got two things wrong that mattered
enough to change the design, not just tweak it:

- **Reed switch pin (`D4`)** — collides with our hardware-confirmed radio
  NSS pin (see `lora_pins.h`); would have broken the radio, not just the
  switch. Its whole SPI/radio pin table doesn't match our silkscreen-
  confirmed values — it appears to have been generated without hardware in
  hand to check against.
- **Direct battery-to-3V3 power architecture** — the 3V3 header pin is
  wired straight to the ESP32-C6's VDD with no protection, and a fresh
  2-cell L91 pack can sit close to the chip's own voltage ceiling with
  effectively no margin (see "Power" above for the numbers). Fixed with a
  nanopower LDO and a third cell.

Everything else structural (JST-PH disconnect for USB safety, reed switch
model, enclosure/cable plan, dual-trigger deep sleep + rapid-slam guard
pattern) held up and is carried forward, the dual-trigger sleep logic into
`PLAN.md`.
