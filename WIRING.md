# Transmitter wiring

Physical build reference for the outdoor transmitter: XIAO ESP32-C6 +
Wio-SX1262 stack, magnetic reed switch, 3x AA lithium primary cells
through a nanopower LDO, optional DS3231 RTC. Built and field-tested —
this is the as-built reference. See README.md for the project-level
"why" behind these choices and their history; this doc is the
part-numbers-and-pinout reference for ordering and building one.

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
| D6 | GPIO16 | **I2C SDA** — battery-voltage ADC (future addition, see below); shared bus for RTC SDA if added |
| D7 | GPIO17 | **I2C SCL** — battery-voltage ADC (future addition, see below); shared bus for RTC SCL if added |
| D8 | GPIO19 | LoRa SCK |
| D9 | GPIO20 | LoRa MISO |
| D10 | GPIO18 | LoRa MOSI |
| 3V3 | — | LDO regulated output (see "Power" below — not a direct battery connection) |
| GND | — | battery − / reed switch return / RTC GND |
| VBUS/5V | — | unused (no USB power in the field) |

D1-D5 and D8-D10 are fixed by the Wio-SX1262 stack (confirmed by
reading the boards' silkscreen — see `tx/lora_pins.h`), leaving D0,
D6, D7 free.

## Mounting the stack

The XIAO ESP32-C6 + Wio-SX1262 stack mounts on an
[Adafruit Perma-Proto Quarter-sized breadboard PCB](https://www.digikey.com/en/products/detail/adafruit-industries-llc/1608/5154676)
(1.7" x 2.0", breadboard-style bus strips — chosen over a bare 9x9-hole
protoboard for room to fit the LDO, resistor, caps, and JST-PH pads
alongside the stack's socket): female pin headers soldered onto the
protoboard as a socket, with the Wio module (male pins facing down)
plugging into it. 13 of the 14 XIAO signals (D1-D10, 3V3, GND, VBUS)
land as ordinary through-hole solder points on the protoboard's
underside. **D0 is the one exception** — it's bypassed around the
stack entirely rather than landing here normally; see "The
Wio-SX1262 HAT's onboard button shares D0" below for why.

A 3D-printable mount for the assembled stack is in
`mount/mount.scad`/`mount.stl`.

### Why the reed switch is on D0, not an arbitrary free pin

Waking from deep sleep on a GPIO edge requires an RTC-domain pin —
on the ESP32-C6, that's GPIO0-7 only. Of the three free header pins,
D0 (GPIO0) is the only one in that range; D6/D7 (GPIO16/17) aren't
RTC-capable, so they're only useful for something read while already
awake (like an RTC's I2C bus). It's not a free choice — it's the only
option left once the radio has claimed D1/D2, the other two
RTC-capable pins.

GPIO0 is not a strapping/boot-select pin on the C6 (unlike classic
ESP32), so there's no boot hazard here — but see the next section for
a different hazard this pin choice runs into.

### Reed switch circuit

Standard alarm-style reed switch: contacts closed (continuity) when
the magnet is present (gate closed), open when it moves away (gate
open). D0 needs a pull-up to read a clean HIGH when open.

**External 1MΩ resistor from D0 to 3V3, internal pull-ups explicitly
disabled in firmware.** An internal ~45kΩ pull-up would leak ~73µA
whenever the gate is closed unless firmware disables it before every
single sleep entry — a recurring hazard on any code path that reaches
`esp_deep_sleep_start()`, including future ones. The external 1MΩ
leaks ~3.3µA regardless of code path and can't regress. If the real
installation turns out electrically noisy (long cable run, near AC
wiring), drop to 100kΩ.

### The Wio-SX1262 HAT's onboard button shares D0 — bypass required

The Wio-SX1262 HAT has its own onboard user button (unused by this
project) with a 10kΩ pull-up wired to the *same* `D0` net the reed
switch uses — confirmed from Seeed's own KiCad schematic, present on
the board as shipped. With the door closed, current flows through the
parallel combination of that 10kΩ and the external 1MΩ pull-up
(`3.3V / (1MΩ ∥ 10kΩ) ≈ 333µA` — matched a live measurement of ~318µA
closely; see README.md's power-budget History for the full
investigation).

**Fix: snip `D0` out of the stacking header entirely, bridge with a
direct wire.** Both the XIAO's own `D0` pin and the Wio HAT's `D0` pin
are cut flush where each protrudes past its own board's solder joint
(the stub below the board, not through it) — each stacking pin is one
continuous piece of metal soldered to its own board where it passes
through, so cutting the protruding stub removes only the portion that
would otherwise reach the next board, without disturbing either
board's own solder joint or socket. A short wire is then soldered
directly from the XIAO's `D0` pin to the reed-switch circuit on the
protoboard, routed around the Wio HAT entirely. The HAT's button and
its pull-up are left connected to nothing but each other and `+3V3` —
isolated and harmless, since this project never uses that button.

Confirmed on the actual hardware with test jumper cables before
cutting anything: **21.9µA (door closed) / 18.5µA (door open)** with
the bypass, vs. the faulty ~318µA/closed-door signature with `D0`
wired through the HAT normally.

A future PCB revision (planned) would put the XIAO and the Wio-SX1262
module in separate sockets rather than stacking them, sidestepping
this HAT-sharing issue entirely — this bypass is a fix for the current
stacked-HAT build specifically.

## Power

**3x AA Energizer Ultimate Lithium (L91) cells in series, through an
LDO, into the XIAO's 3V3/GND header pins** — not the board's dedicated
BAT solder pads, and not a direct battery connection.

### Why primary lithium, not rechargeable Li-ion/LiPo

The XIAO's onboard charge circuit (the BAT pads, below) makes
rechargeable Li-ion/LiPo the path of least resistance on this board,
so it's worth stating why this build avoids it. Deployment climate
here has no hard freezes, so L91's cold-weather edge doesn't matter —
but summers reach ~90°F ambient, and a sealed IP67 box in direct sun
runs hotter still inside, which is exactly where Li-ion/LiPo capacity
fade accelerates and pouch-cell failure modes (puncture, swelling,
thermal runaway) get worse with age. Primary cylindrical cells carry
none of those failure modes — matching the alarm industry's own
convention of primary lithium for door/window sensors.

### Why not a direct battery connection

The XIAO's exposed **3V3 header pin is wired directly to the
ESP32-C6's VDD** — confirmed on a Seeed forum thread about this pin —
with no protection or regulation. The datasheet-recommended range is
3.0-3.6V; a fresh L91 cell sits close to 1.8V under light load, so two
fresh cells in series can be close to 3.6V right out of the pack —
zero margin on day one, before pack-to-pack or temperature variance.
The board's **BAT pads** aren't an alternative either — they're wired
to an onboard Li-ion/LiPo charge controller that will try to charge
whatever's connected there, and primary cells aren't rechargeable.

### Short-circuit protection

**[Bourns MF-R050](https://www.digikey.com/en/products/detail/bourns-inc/MF-R050/259965)
(or TE/Raychem RXE050, dual-sourced equivalent), a resettable PTC
fuse, in series between the battery holder and the LDO's input** —
not on the regulated 3V3 output side. Radial leaded, 5.08mm/0.2" lead
spacing, drops into the Perma-Proto the same as everything else.

**Why needed at all, given both ends already protect themselves:**
each L91 cell has its own internal PTC plus a pressure-relief vent and
is independently rated to survive a direct external short; the LDO has
its own overcurrent/thermal shutdown on the output side. What neither
covers is the wiring, JST-PH connector, and battery holder's spring
contacts in between — none of which carry a comparable rating.

**Why MF-R050 specifically:** sized against the LDO's own rated
maximum continuous current (250mA, not this firmware's current
~125mA TX draw, so a future firmware change doesn't turn the fuse into
an unintended bottleneck), derated for a 60°C worst-case ambient (same
ceiling the L91 cells are rated to):

| Part | Ihold @23°C | Ihold @60°C (derated) | Margin over 250mA floor |
|---|---|---|---|
| MF-R030 | 300mA | 190mA | under the floor |
| MF-R040 | 400mA | 250mA | zero margin |
| **MF-R050 (chosen)** | 500mA | 320mA | **+70mA** |

MF-R050 also has *less* resistance than the smaller parts in this
family (0.77Ω max), adding only ~0.10V drop at our actual ~125mA TX
current.

**Trip time for a real short:** three L91 cells (~0.3Ω pulse
resistance) plus the fuse (0.41-0.77Ω) plus wiring (~0.15Ω) gives
roughly 1.0Ω total loop resistance — at a fresh pack's ~5.4V, an
estimated ~5A short-circuit current, comfortably above MF-R050's 2.5A
qualification current (guaranteed to trip within 4.0s at that point,
worst case; real trip time at ~5A is faster, though not quantified
beyond that spec figure).

### Choosing an LDO

Fixed 3.3V output, needs to accept the pack's full discharge range
without exceeding its own input ceiling. 3 cells (not 2) puts the
fresh-pack voltage (~5.1-5.4V) safely under either candidate's 6.0V
input ceiling with room to spare — at no mAh cost, since a linear
regulator draws roughly the same current from its input as it
delivers at its output regardless of input voltage.

#### MCP1700 (Microchip, TO-92) — installed

[Microchip MCP1700](https://ww1.microchip.com/downloads/aemDocuments/documents/APID/ProductDocuments/DataSheets/MCP1700-Data-Sheet-20001826F.pdf),
3.3V fixed output (`MCP1700-3302E/TO`, TO-92) — 1.6µA typical / 4µA
max quiescent current, 2.3-6.0V input range, 150mV typical dropout
@200mA. Bare 3-terminal part (GND, VIN, VOUT), no enable pin, no
SOT-to-DIP adapter needed — legs bent to the standard 0.1" grid, drops
straight into the Perma-Proto.

**Package orientation** (confirmed against the datasheet's mechanical
drawing, cross-checked against an independent pinout source): holding
the part with its flat, marked face toward you and legs pointing down,
pins left to right are **GND, VIN, VOUT**.

| MCP1700 pin (TO-92, flat face toward viewer) | Connects to |
|---|---|
| 1 (GND, leftmost) | battery − / system GND |
| 2 (VIN, middle) | battery + (through the JST-PH disconnect, below) |
| 3 (VOUT, rightmost) | XIAO 3V3 pin |

Needs a ~1µF capacitor on **both** VIN-GND and VOUT-GND — the
datasheet treats the input cap as required for stability here, not
just good practice. Ceramic, tantalum, or aluminum electrolytic are
all acceptable; 1µF X7R ceramics are what's installed.

#### TPS7A0233 (TI) — alternative, lower quiescent current

[TI TPS7A02](https://www.ti.com/lit/ds/symlink/tps7a02.pdf), 3.3V
fixed output (suffix `33`), SOT23-5 — 25nA quiescent current even in
dropout, 60-160x lower than the MCP1700's. The true-nanopower pick,
worth revisiting if squeezing out that last bit of sleep-current
budget ever matters; a bench unit was built but tested dead (0V
output, bad part or solder joint, not a design issue) and wasn't
retried. Needs its own build path since no breakout exists for this
chip: a bare `TPS7A0233PDBVR` hand-soldered onto a
[Proto Advantage/Chip Quik PA0086C](https://www.digikey.com/en/products/detail/chip-quik-inc/PA0086C/12352102)
SOT23-5-to-DIP-6 adapter (0.1" pitch, drops into any protoboard).

| TPS7A02 pin | Connects to |
|---|---|
| 1 (IN) | battery + (through the JST-PH disconnect, below) |
| 2 (GND) | battery − / system GND |
| 3 (EN) | tied to pin 1 (IN) — EN defaults *off* if left floating |
| 4 (NC) | unconnected |
| 5 (OUT) | XIAO 3V3 pin |

Needs a ~1µF ceramic (low-ESR, X7R/X5R/C0G — the datasheet is
compensated for ceramic specifically) on both IN-GND and OUT-GND.

### Battery disconnect

**JST-PH 2.0mm disconnect between the battery pack and the LDO's
input**, always removed before connecting USB. The LDO means a
backfeed is two regulated 3.3V sources contending on one node rather
than raw battery voltage hitting VDD directly — safer, but still not
a scenario to rely on rather than avoid.

**Adding the LDO costs the onboard battery gauge, though** — `A1`/
`VBAT` reads the regulated 3.3V output after the LDO, not the raw
battery voltage, so it stays flat for most of the pack's life. See
"Future additions" below for the fix.

## Future additions

### Battery voltage monitoring

Not yet wired into firmware. The ESP32-C6's own ADC can't be used for
this: of its three header-exposed ADC1 pins (`A0`/`A1`/`A2` =
D0/D1/D2), D0 is the reed switch and D1/D2 are wired point-to-point to
the radio's DIO1/RST — none free.

**Plan: a small I2C ADC on the D6/D7 bus instead**, reading a raw
battery-voltage divider tapped before the LDO. **ADS1115** (TI,
16-bit, I2C) — its single-shot mode auto-drops to ~0.5µA between
conversions, and its power-on-reset default is already that
power-down/single-shot mode, so misconfigured firmware still leaves
it idle rather than free-running. (An 18-bit MCP3421 was considered
first but its 2.25MΩ input impedance loads down a multi-MΩ divider
enough to need firmware workarounds; the ADS1115's impedance at
±4.096V full scale — the setting to use — doesn't.)

Divider: raw battery+ (tapped before the LDO) through 4.7MΩ to a tap
node, then 2.2MΩ from that node to GND; tap feeds AIN0. Adds roughly
~1.3µA worst-case to the sleep budget (divider + chip's own power-down
draw).

| ADS1115 pin | Connects to |
|---|---|
| VDD | XIAO 3V3 (regulated rail) |
| GND | system GND |
| SCL | D7/GPIO17 |
| SDA | D6/GPIO16 |
| ADDR | GND (I2C address 0x48) |
| AIN0 | raw-battery divider tap |
| AIN1 | GND (single-ended) |

**Sourcing:** the common "HW-543"-silkscreened ADS1115/ADS1015
breakout (widely resold, cheap, ships fast) works — confirmed no power
LED and no onboard regulator on the one checked, but since it's an
unbranded design, check the schematic printed on whatever specific
listing you buy from. [Adafruit #1085](https://www.adafruit.com/product/1085)
is the documented, slower-shipping fallback if that can't be
confirmed.

### DS3231 RTC

Opportunistic addition on D6/D7 (I2C), lower priority than everything
else here — receiver receipt time is probably close enough for door
events, so this is only worth adding if it stays this cheap.

**Don't use a stock DS3231 breakout as-is** — the common ~$2 modules
include an onboard CR2032 trickle-charge circuit and power LED that
would eat into the sleep budget. Use a bare DS3231(M) directly, or a
breakout confirmed to have the charge resistor and LED removed.

## Enclosure and cable routing

- IP67 ABS junction box, ~125 x 75 x 75mm, all penetrations on the
  bottom face: SMA bulkhead and a PG7 cable gland for the sensor lead.
- Antenna chain: Wio-SX1262's onboard U.FL connector →
  [U.FL-to-SMA-bulkhead pigtail](https://www.digikey.com/en/products/detail/rf-solutions/CBA-UFLSMA-1/5845754) →
  enclosure's bottom face →
  [915MHz SMA whip antenna](https://www.digikey.com/en/products/detail/nearson-inc/S463AM-915/4571855),
  pointing straight down (keeps the connector shielded from pooling
  water, radiation pattern horizontal). The antenna has no stated
  weatherproof rating of its own; pointing down and relying on the
  bulkhead's O-ring is the mitigation.
- 22/2 white UV-rated cable to the reed switch, joined with adhesive
  solder-seal heat-shrink sleeves.
- Reed switch: Interlogix 1085W-N or Honeywell 7945WH, fully potted,
  1.5" gap — wide gap resists false triggers from gate rattle/wind.
- Cable secured to the gate's lowest rail with GE Silicone II dabs
  every 12-18", or outdoor acrylic tape — both removable without
  damaging paint/powder coat.

## Bill of materials

| Part | Qty | Notes |
|---|---|---|
| XIAO ESP32-C6 | 1 | |
| Wio-SX1262 (for XIAO ESP32S3, through-hole variant) | 1 | |
| Adafruit Perma-Proto Quarter-sized breadboard PCB | 1 | mounts the whole build — see "Mounting the stack" |
| 2x7 female pin headers, 2.54mm pitch | 1 pair | |
| Interlogix 1085W-N or Honeywell 7945WH reed switch | 1 | |
| 3x AA Energizer Ultimate Lithium (L91) | 1 pack | |
| [Keystone Electronics 2465](https://www.digikey.com/en/products/detail/keystone-electronics/2465/303814) 3xAA holder | 1 | series-wired (4.5V), 6" bare leads, chassis mount, no cover |
| MCP1700-3302E/TO (Microchip, TO-92, 3.3V fixed LDO) | 1 | **installed** — see "Power" → "Choosing an LDO" |
| Through-hole ceramic capacitor, ~1µF ("105") | 2 | VIN and VOUT bypass — see "Power" |
| TPS7A0233PDBVR (TI, SOT23-5, 3.3V fixed LDO) | 1 | **alternative, not installed** — see "Power" → "Choosing an LDO" |
| Proto Advantage / Chip Quik PA0086C SOT23-5-to-DIP-6 adapter | 1 | TPS7A0233 only |
| Through-hole ceramic capacitor, ~1µF, low-ESR | 2 | TPS7A0233 only |
| JST-PH 2.0mm 2-pin connector pair | 1 | pre-crimped, inline-soldered |
| [Bourns MF-R050](https://www.digikey.com/en/products/detail/bourns-inc/MF-R050/259965) (or TE/Raychem RXE050) resettable PTC fuse | 1 | battery-to-LDO short-circuit protection — see "Power" → "Short-circuit protection" |
| 1MΩ resistor | 1 | reed-switch pull-up |
| IP67 ABS junction box, ~125x75x75mm | 1 | |
| [RF Solutions CBA-UFLSMA-1](https://www.digikey.com/en/products/detail/rf-solutions/CBA-UFLSMA-1/5845754) U.FL-to-SMA bulkhead pigtail | 1 | |
| [Nearson S463AM-915](https://www.digikey.com/en/products/detail/nearson-inc/S463AM-915/4571855) 915MHz SMA antenna | 1 | standard SMA male |
| PG7 cable gland | 1 | |
| 22/2 white UV cable | as needed | ~50ft roll |
| Solder-seal heat-shrink sleeves | a few | splice to switch pigtail |
| ADS1115 I2C ADC — "HW-543"-style breakout or [Adafruit #1085](https://www.adafruit.com/product/1085) | 1 | **future addition** — see "Future additions" → "Battery voltage monitoring" |
| 4.7MΩ resistor | 1 | **future addition** — raw-battery divider, top half |
| 2.2MΩ resistor | 1 | **future addition** — raw-battery divider, bottom half |
| DS3231(M) RTC, bare chip or de-modded breakout | 1 | **future addition**, optional |
