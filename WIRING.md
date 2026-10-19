# Transmitter wiring

Physical build reference for the outdoor transmitter: XIAO ESP32-C6 + Wio-SX1262
stack, magnetic reed switch, 3x AA lithium primary cells through a nanopower
LDO, optional DS3231 RTC.
Built and field-tested — this is the as-built reference, cross-checked
against the confirmed radio pinout in `tx-test/lora_pins.h`/`rx-test/lora_pins.h` and
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
| D6 | GPIO16 | **I2C SDA** — battery-voltage ADC (ADS1115); shared bus for RTC SDA if added |
| D7 | GPIO17 | **I2C SCL** — battery-voltage ADC (ADS1115); shared bus for RTC SCL if added |
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
13 of the 14 XIAO signals (D1-D10, 3V3, GND, VBUS) are available as
ordinary through-hole solder points on the protoboard's underside —
standard female header strips have their pins protrude slightly on the
solder side — which is where the reed switch leads, the D0 pull-up, and
the LDO's regulated output all land. D0 is the one exception: it's
bypassed around the stack entirely rather than landing here normally —
see "The Wio-SX1262 HAT's onboard button shares D0" below for why.

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

This pin choice turned out to have a real, separate consequence of its
own — see "The Wio-SX1262 HAT's onboard button shares D0" below.

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

### The Wio-SX1262 HAT's onboard button shares D0 — bypass required

The Wio-SX1262 HAT has its own onboard user button (unused by this
project) with a 10kΩ pull-up wired to the *same* `D0` net the reed
switch uses — confirmed directly from Seeed's own KiCad schematic for
the board, not inferred. This is on the board as shipped, not something
specific to one variant or assembly option.

It went undetected for a long time because it's silent in isolation —
with the door open, nothing pulls `D0` low, so the extra 10kΩ pull-up
just sits there agreeing with the external 1MΩ one, drawing no current.
The problem only shows up with the door *closed*: current then flows
through the parallel combination of the HAT's 10kΩ and the external
1MΩ (`3.3V / (1MΩ ∥ 10kΩ) ≈ 333µA`), instead of just the ~3.3µA the
external 1MΩ alone was designed for — live PPK2 measurement confirmed
~318µA sleep current with the door closed, matching that math closely,
against a budgeted ~41-44µA. See `PLAN.md`'s "Sleep current root cause"
for the full investigation (several other hypotheses were tested and
ruled out before this one was found) and the measured before/after
numbers confirming the fix.

**Fix: snip `D0` out of the stacking header entirely, bridge with a
direct wire.** Both the XIAO's own `D0` pin and the Wio HAT's `D0` pin
are cut flush where each protrudes past its own board's solder joint
(the exposed stub below the board, not through the board itself). Each
stacking pin is one continuous piece of metal running from its female
socket down to its male tip, soldered to its own board at the point
where it passes through — cutting the protruding stub removes the
portion that would otherwise reach the next board in the stack, without
disturbing the solder joint or the socket above it. A short wire is
then soldered directly from the XIAO's `D0` pin to the reed-switch
circuit on the protoboard, routed around the Wio HAT entirely. The
HAT's button and its pull-up are left fully isolated — connected to
nothing but each other and `+3V3` — functionally disabled, which costs
nothing since this project never uses that button.

Confirmed on the actual hardware before committing to anything
irreversible: live-wired by hand with test jumper cables first (no
cutting), reproducing both the faulty ~318µA/closed-door signature with
`D0` wired through the HAT normally, and a clean **21.9µA (closed) /
18.5µA (open)** with `D0` bypassing the HAT entirely.

A future PCB revision (planned) would put the XIAO and the Wio-SX1262
module in separate sockets rather than stacking them, sidestepping this
HAT-sharing issue entirely — this bypass is a fix for the current
stacked-HAT build specifically, not a permanent design choice.

## Power

**3x AA Energizer Ultimate Lithium (L91) cells in series, through an LDO,
into the XIAO's 3V3/GND header pins — not the board's dedicated BAT solder
pads, and not a direct battery connection.** Two LDO options are documented
below ("Choosing an LDO") rather than one — the goal here isn't just this
one transmitter but building up a small library of validated power
solutions for this class of project, so both are kept even though only one
is installed at a time. **Currently installed: Option B (MCP1700, TO-92),**
confirmed regulating 3.3V on the bench. Option A (TPS7A0233) is the lower-
quiescent-current pick and remains the long-term target, but the first
bench unit tested dead (0V output, no load) and is set aside pending a
replacement part to retry.

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

### Short-circuit protection

**[Bourns MF-R050](https://www.digikey.com/en/products/detail/bourns-inc/MF-R050/259965)
(or TE Connectivity/Raychem RXE050 as a dual-sourced equivalent — the two
datasheets agree closely enough to be interchangeable), a resettable PTC
fuse, in series between the battery holder and the LDO's input** — not on
the regulated 3V3 output side. Radial leaded, 5.08mm/0.2" lead spacing
(2× the standard 0.1" grid), so it plugs into the Perma-Proto the same
way everything else here does.

**Why this is needed at all, given both ends of this path already protect
themselves:** each L91 cell has its own internal PTC (trips at 85°C) plus
a mechanical pressure-relief vent, and is independently safety-rated to
survive a direct external short without fire or explosion — the same bar
UL 1642/IEC 62133-style cell safety testing requires, already met with
zero added protection (see Energizer's *Cylindrical Primary Lithium
Handbook and Application Manual*). The MCP1700 (Option B, currently
installed) has its own internal overcurrent monitor and thermal shutdown
on the output side — a short there gets hiccup-limited by the chip
itself, not left to the battery. What neither of those covers is the
**wiring, JST-PH connector, and battery holder's spring contacts between
the pack and the LDO** — none of which carry a comparable rating, and a
raw-pack short can still drive several amps through them for a few
seconds before the cells' own PTCs clamp it down (see the trip-time
estimate below).

**Why MF-R050 specifically, not a smaller part:** the never-nuisance-trip
floor should be set by the MCP1700's own rated maximum continuous current
(250mA, its datasheet's absolute ceiling), not by this firmware's current
~125mA TX-burst draw — sizing to today's firmware would let the fuse
become an unintended bottleneck against anything the regulator itself is
legitimately rated to do, now or after a future firmware change. Checked
against this project's own already-established 140°F/60°C worst-case
ambient (same ceiling the L91 cells themselves are rated to, "Why primary
lithium over rechargeable Li-ion/LiPo" above) using Bourns' and TE's own
thermal-derating tables:

| Part | Ihold @23°C | Ihold @60°C (derated) | Margin over 250mA floor | Ri max |
|---|---|---|---|---|
| MF-R030 | 300mA | 190mA | **under the floor** | 1.36Ω |
| MF-R040 | 400mA | 250mA | **zero margin** | 0.86Ω |
| **MF-R050 (chosen)** | 500mA | 320mA | **+70mA** | 0.77Ω |

MF-R050 is the smallest part in this family that clears the corrected
floor with real margin. It also happens to carry *less* resistance than
the smaller parts, not more — this family's resistance drops as its
current rating rises, so correcting the floor upward didn't cost
anything on voltage drop: ~0.10V at our actual ~125mA TX current
(0.77Ω max × 0.125A), using the worst-case (not yet measured) resistance
figure. That adds to whatever the battery's own internal resistance
already costs at end-of-discharge — small, but not zero, and not yet
checked against a measured near-end-of-life voltage-sag-under-load trace.

**Trip-time estimate for a real short:** Energizer's application manual
gives a measured effective pulse internal resistance of "about 100mΩ"
per L91 cell (10mA/50ms then 500mA/100ms pulse regime — not a true
dead-short characterization, so this is an extrapolation, not a
validated figure at short-circuit current). Three cells in series
(~0.3Ω) plus MF-R050's own resistance (0.41-0.77Ω) plus an unsourced
wiring/connector allowance (~0.15Ω) gives roughly 1.0Ω total loop
resistance, and at a fresh pack's ~5.4V, an estimated ~5A short-circuit
current — comfortably above MF-R050's 2.5A qualification current, at
which its datasheet guarantees tripping within 4.0 seconds, worst case.
Real trip time at ~5A is very likely faster than that 4.0s figure (PTC
trip time drops as overcurrent rises above the qualification point), but
the exact curve wasn't extractable from the datasheet's graph (vector
data, not text) to quote a tighter number.

There's no separate industry-standard "external protection must trip
within N seconds" figure independent of the cell-level short-circuit pass/
fail test already covered above — this fuse is a secondary, non-regulatory
layer protecting the wiring and holder specifically, not something
required to keep the cells themselves from being dangerous.

### Choosing an LDO

Whichever chip is used, the sizing logic is the same: fixed 3.3V output,
needs to accept the pack's full discharge range without exceeding its own
input ceiling. Going to 3 cells instead of 2 puts the fresh-pack voltage
(~5.1-5.4V) safely under either candidate's 6.0V input ceiling with room to
spare, and keeps the LDO in regulation through nearly the entire discharge
curve rather than just clipping the fresh-cell peak. This doesn't cost
anything in mAh-based battery life — a linear regulator draws roughly the
same current from its input as it delivers at its output, regardless of
the input voltage, so battery life (capacity ÷ current drawn) is unaffected
by adding a third cell in series.

Two options are documented below, both confirmed against their datasheets
and (for Option B) against a working bench test:

#### Option A: TPS7A0233 (TI) — nanopower target, debugging in progress

[TI TPS7A02](https://www.ti.com/lit/ds/symlink/tps7a02.pdf), 3.3V fixed
output (part suffix `33`), SOT23-5 package (`DBV`) — 25nA quiescent current
even in dropout, 1.5-6.0V input range, 270mV max dropout at a full 200mA
load (far less at our currents). This is the true-nanopower pick — its
quiescent current is roughly 60-160x lower than Option B's — but the first
bench unit built tested dead (0V on OUT despite correct IN and EN voltages,
narrowed down to a hardware fault on that specific unit or its solder joint,
not the design); a replacement part is on order to retry. Ordering as
[TPS7A0233PDBVR](https://www.digikey.com/en/products/detail/texas-instruments/TPS7A0233PDBVR/12165099)
— the `P` before the package code is TI's convention for "built at a newer
assembly/test site," not a spec or pinout difference (confirmed against TI's
own explanation of the same convention on a different part family);
`TPS7A0233DBVR` without the `P` is the same device and works identically.

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
for stability — input cap is not strictly required for stability per the
datasheet but is good practice; 1µF+ on both is recommended for transient
response, which matters here given the LoRa TX current step). **Must be
low-ESR ceramic** (X7R/X5R/C0G) — the datasheet specifies the part is
compensated for ceramic specifically; an electrolytic's higher ESR falls
outside that design and risks output oscillation that a slow multimeter
reading can mask rather than reveal. Through-hole, not SMD — the adapter
board's own pads are too small for a through-hole part, so these land on
the Perma-Proto's protoboard holes right next to where the adapter's IN/OUT
pins seat, wired over with a short lead.

#### Option B: MCP1700 (Microchip, TO-92) — confirmed working, currently installed

[Microchip MCP1700](https://ww1.microchip.com/downloads/aemDocuments/documents/APID/ProductDocuments/DataSheets/MCP1700-Data-Sheet-20001826F.pdf),
3.3V fixed output (`MCP1700T-3302E` for SOT-23, `MCP1700-3302E/TO` for
TO-92) — 1.6µA typical / 4µA max quiescent current, 2.3-6.0V input range,
150mV typical dropout @ 200mA / 178mV @ 250mA (both far less at our
currents). Quiescent current is meaningfully higher than Option A's — see
`PLAN.md`'s power budget for the resulting battery-life difference — but
this part is simpler to build and confirmed working: **bare 3-terminal
part (GND, VIN, VOUT) with no enable pin at all**, so there's no EN-tied-
wrong, EN-left-floating class of mistake to make. Built and bench-tested in
the **TO-92** package specifically, which plugs directly into the
Perma-Proto with no SOT-to-DIP adapter needed — legs bent to the standard
0.1" grid.

**Package orientation (confirmed against the datasheet's mechanical outline
drawing and cross-checked against an independent pinout source, since the
datasheet's own orthographic TOP VIEW/VIEW A-A drawing doesn't label pin
numbers on the view that shows flat-vs-round orientation):** holding the
part with its flat, marked face toward you and the three legs pointing down
toward the board, the pins left to right are **GND, VIN, VOUT** — matching
Table 3-1 of the datasheet. The SOT-23 package (if used instead) has a
*different* order — GND, VOUT, VIN, with VOUT and VIN swapped — so don't
assume the two packages share a pinout.

| MCP1700 pin (TO-92, flat face toward viewer) | Connects to |
|---|---|
| 1 (GND, leftmost) | battery − / system GND |
| 2 (VIN, middle) | battery + (through the JST-PH disconnect, see below) |
| 3 (VOUT, rightmost) | XIAO 3V3 pin (lands on the protoboard's 3V3 socket pad — see "Mounting the stack" above) |

Needs a ~1µF capacitor on **both** VIN-to-GND and VOUT-to-GND — unlike
Option A, the datasheet treats the input capacitor as required for
stability, not just good practice ("a relatively low source impedance is
needed to prevent the input impedance from causing the LDO to become
unstable... 1µF ceramic capacitor will be sufficient"), and separately
requires "a minimum output capacitance of 1.0µF... for small signal
stability." More forgiving than Option A on capacitor type, though: ceramic,
tantalum, or aluminum electrolytic are all explicitly stated as acceptable,
with the output cap tolerating an ESR range of 0 to 2.0Ω — the existing
1µF X7R ceramics work fine and are what's actually installed.

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
health the way the protocol's heartbeat message is meant to.

### Battery voltage monitoring

The original fix-it-later idea here was "read the raw battery through a
divider into a spare ADC pin" — D6 or D7. That turned out not to work: the
ESP32-C6's ADC1 only exists on GPIO0-6, and this board only actually breaks
three of those out to the header at all — `A0`/`A1`/`A2` = D0/D1/D2
(confirmed against Espressif's own ESP32-C6 datasheet and Seeed's XIAO
ESP32C6 pin docs, not just the schematic). D6/D7 (GPIO16/17) aren't
ADC-capable on this chip — they're the board's default UART0 TX/RX lines.
Of the three real ADC pins, D0 is the reed switch and D1/D2 are physically
wired point-to-point to the Wio-SX1262's DIO1/RST pins (see the pin table
above) — not just claimed by firmware, but electrically contended if
anything else tries to share that node. So there's no ADC-capable pin left
at all, not just none conveniently free.

**Fix: move the raw-battery reading off the ESP32's own ADC entirely, onto
a small I2C ADC on the D6/D7 bus instead.** I2C doesn't have the ADC1
pin restriction — the ESP32 can route it to any GPIO via its peripheral
matrix — so this sidesteps the pin shortage rather than fighting it, and
D6/D7 were already earmarked for I2C (RTC) anyway; both devices can share
the same two-wire bus at different addresses if the DS3231 gets added later.

**ADS1115** (TI, 16-bit single-channel-as-used ΔΣ ADC, I2C): its single-shot
mode does one conversion per trigger and then drops to ~0.5µA power-down
*automatically* — same reasoning as the external reed pull-up above,
preferring a part whose low-power state is the hardware default over one
that depends on every code path getting a shutdown call right. It also
fails safe on misconfiguration: the config register's power-on-reset value
is already power-down/single-shot mode, so even firmware that never touches
the register leaves the chip idle rather than free-running.

Wiring:

| ADS1115 pin | Connects to |
|---|---|
| VDD | XIAO 3V3 (regulated rail — a stable supply, not raw battery) |
| GND | system GND |
| SCL | D7/GPIO17 |
| SDA | D6/GPIO16 |
| ADDR | GND (selects I2C address 0x48 — see "Sourcing" below on address flexibility) |
| AIN0 | raw-battery divider tap (below) |
| AIN1 | GND (single-ended: `AIN0 - GND`) |

Divider: raw battery+ (tapped before the LDO, same node as the LDO's IN
pin) through 4.7MΩ to a tap node, then 2.2MΩ from that node to GND; the tap
feeds AIN0. Ratio ≈0.319 maps the pack's ~2.4-5.4V range to ~0.77-1.72V.

This divider is the only new always-on current path (the ADS1115 itself is
asleep except for one brief conversion per wake cycle, and idle I2C
pull-ups draw nothing when both lines rest high). Worst case, fresh pack at
~5.4V: 5.4V / 6.9MΩ ≈ 0.78µA, plus the chip's own ~0.5µA typical power-down
draw — about a 3% addition to the ~38µA sleep budget in `PLAN.md`.

#### Why the ADS1115 over the MCP3421 considered earlier

An 18-bit MCP3421 was the first part specced here, and it technically
works, but it forces an awkward firmware constraint: its switched-capacitor
front end presents only **2.25MΩ/PGA** input impedance (2.25MΩ at its
lowest gain), and our ~6.9MΩ divider (1.5MΩ Thevenin-equivalent) loads that
down by roughly 40% if sampled naively. The fix was solvable but fiddly — a
reservoir cap at the tap plus forcing the ADC into its 12-bit/240 SPS mode
specifically, since its native 18-bit/3.75 SPS mode would sag that cap by
~200mV mid-conversion. Workable, but a firmware bug that left it at 18-bit
would silently corrupt every reading rather than just being coarser.

The ADS1115 sidesteps this rather than needing the workaround. Its
differential input impedance depends on which full-scale range (PGA
setting) is selected — TI's datasheet gives 4.9MΩ at the default ±2.048V
range, climbing to **15MΩ at ±4.096V** and 22MΩ at ±6.144V. Our divider tap
tops out at 1.72V, comfortably inside any of those ranges, so there's no
reason to use the default — set the gain register for **±4.096V full
scale** (`GAIN_ONE` in most libraries) and the loading error drops to
1.5MΩ/(1.5MΩ+15MΩ) ≈ 9%, a fixed, calibratable scale factor rather than a
dynamic conversion-time-dependent sag. (±6.144V/22MΩ loads even less, ~6%,
but per the datasheet only reaches its full range unclamped when
VDD > 4V — ours is 3.3V — so ±4.096V is the clean choice without reading
into that caveat.) At that setting the LSB is 125µV at the tap ≈ 0.39mV at
the battery — still far finer than a battery trend needs.

Because the loading current is so much smaller here (peak ~115nA vs. the
MCP3421's ~760nA), even a modest 1µF cap at the tap keeps droop negligible
at *any* ADS1115 data rate, including the slowest (8 SPS, ~125ms
conversion ≈ 14µV droop — still tiny next to the 125µV LSB). There's no
resolution/conversion-time tradeoff to get right in firmware here; the
library's ordinary default data rate (typically 128 SPS) is fine as-is.
The cap does still need time to recover between readings — the same
~5 × (1.5MΩ × 1µF) ≈ 7.5s as before — but one reading per wake, as the
heartbeat wants anyway, is well clear of that.

#### Sourcing

A breakout board avoids needing a SOT-to-DIP adapter alongside the LDO's,
and a 0.1" header drops straight into the Perma-Proto. Generic unbranded
modules are the usual worry here — some bundle an onboard 5V→3.3V regulator
or an undocumented LED, either adding leakage that doesn't show up until
it's measured, which is what ruled out a generic MCP3421 module earlier.
That's a real risk in general, but it's checkable rather than a reason to
default to the pricier documented option, *if* the specific listing's
schematic can actually be seen first.

**Recommended: the common "HW-543"-silkscreened purple-PCB ADS1115/ADS1015
board** sold under many different storefront names on Amazon (same shared
PCB stuffed with either chip, same way Adafruit's #1085/#1083 pair is) —
cheap, and ships same/next-day from Amazon rather than direct from
Adafruit. Confirmed by reading the schematic printed on one such listing's
own photos, not just the description:

- **No power LED at all** — not a jumper to cut, the circuit simply doesn't
  include one. Also no onboard voltage regulator; VDD passes to the chip
  through two ferrite beads (filtering only), matching the photographed
  board (just the ADS1115/ADS1015 SOIC and a handful of passives — no
  second chip that would indicate a hidden regulator).
- 10kΩ pull-ups on SCL, SDA, and ADDR (and one on ALERT, unused here), plus
  a 1µF + ~0.1µF decoupling pair at VDD — same values assumed elsewhere in
  this doc.
- 10-pin 0.1" header: `V G SCL SDA ADDR ALERT A0 A1 A2 A3`. ADDR is a bare
  header pin here rather than a solder jumper, so any of TI's four
  addresses can be wired directly (GND/VDD/SDA/SCL → 0x48/0x49/0x4A/0x4B) —
  tie to GND for 0x48, clear of the DS3231's fixed 0x68, same as the plan
  above.

Because this is an unbranded design, **check the schematic printed on
whichever specific listing you buy from** before assuming it matches — the
"HW-543" reference design is extremely widely cloned, but a seller
substituting a different silkscreen board under the same listing text
isn't unheard of on Amazon. A listing that shows its schematic (as
[this one's product photos did](https://m.media-amazon.com/images/I/71fcLPzMRHL._SL1500_.jpg))
is confirmable the same way the DS3231 caveat below asks for; one that
doesn't isn't a stop-ship, since these are simple enough that a $10 meter
can verify no-LED/no-regulator in a minute once it arrives, but confirm
before soldering it into the final build.

**Alternative: [Adafruit ADS1115 breakout (#1085)](https://www.adafruit.com/product/1085)**
— or its 12-bit sibling on the same PCB, [ADS1015 (#1083)](https://www.adafruit.com/product/1083),
which would work identically here since 12 bits is already more than this
application needs. Costs more and ships slower than an Amazon listing, but
is the fully-documented fallback if a specific Amazon board's schematic
can't be confirmed: 10kΩ I2C pull-ups onboard, 2-5V VIN, a 0.1" header
included in the bag, a power LED with a **documented cuttable jumper** on
the back of the board (no desoldering needed), and an I2C address
pin-selectable across all four values (0x48/0x49/0x4A/0x4B via ADDR, plus a
second back-of-board solder jumper for a further option).

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
  face only: SMA bulkhead and a PG7 cable gland for the sensor lead.
- Antenna chain: the Wio-SX1262's onboard U.FL connector to a
  [U.FL-to-SMA-bulkhead pigtail](https://www.digikey.com/en/products/detail/rf-solutions/CBA-UFLSMA-1/5845754)
  (confirmed bulkhead mount, standard SMA — not RP-SMA), through the
  enclosure's bottom face, to an external
  [915MHz SMA whip antenna](https://www.digikey.com/en/products/detail/nearson-inc/S463AM-915/4571855)
  (confirmed matching standard-SMA-male connector) screwed directly onto the
  bulkhead's outside face, pointing straight down — keeps the connector
  shielded from pooling water and the radiation pattern horizontal. The
  antenna itself has no stated weatherproof rating; pointing it down and
  relying on the bulkhead's own O-ring to seal the wall penetration is the
  mitigation, not a substitute for one — worth swapping for an explicitly
  IP-rated antenna (more of an Amazon/eBay category than a DigiKey one) if
  that matters more than sourcing everything from one vendor.
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
| 2x7 female pin headers, 2.54mm pitch | 1 pair | already have |
| Interlogix 1085W-N or Honeywell 7945WH reed switch | 1 | ordered |
| 3x AA Energizer Ultimate Lithium (L91) | 1 pack | already have |
| [Keystone Electronics 2465](https://www.digikey.com/en/products/detail/keystone-electronics/2465/303814) 3xAA holder | 1 | series-wired (4.5V), 6" bare leads, chassis mount, no cover |
| TPS7A0233PDBVR (TI, SOT23-5, 3.3V fixed LDO) | 1 | **Option A**, not currently installed — bare chip, debugging in progress — see "Power" → "Choosing an LDO" |
| Proto Advantage / Chip Quik PA0086C SOT23-5-to-DIP-6 adapter | 1 | **Option A only** — confirmed 0.1" pitch, pins pre-installed — see "Power" |
| Through-hole ceramic capacitor, ~1µF | 2 | **Option A** — IN and OUT bypass, must be low-ESR ceramic — see "Power" |
| MCP1700-3302E/TO (Microchip, TO-92, 3.3V fixed LDO) | 1 | **Option B, currently installed** — confirmed working, no adapter needed — see "Power" → "Choosing an LDO" |
| Through-hole ceramic capacitor, ~1µF ("105") | 2 | **Option B** — VIN and VOUT bypass, both required for stability; ceramic/tantalum/electrolytic all acceptable — see "Power" |
| JST-PH 2.0mm 2-pin connector pair | 1 | already have (pre-crimped, inline-soldered) |
| [Bourns MF-R050](https://www.digikey.com/en/products/detail/bourns-inc/MF-R050/259965) (or TE/Raychem RXE050) resettable PTC fuse | 1 | battery-to-LDO short-circuit protection — see "Power" → "Short-circuit protection" |
| 1MΩ resistor | 1 | already have (from assortment) |
| ADS1115 I2C ADC — "HW-543"-style purple-PCB breakout (generic, ships fast from Amazon; confirm the specific listing's schematic — no LED, no regulator), or [Adafruit #1085](https://www.adafruit.com/product/1085) as the documented fallback | 1 | battery-voltage monitoring, address 0x48 (clear of the DS3231's 0x68) — see "Battery voltage monitoring" → "Sourcing" |
| 4.7MΩ resistor | 1 | raw-battery divider, top half |
| 2.2MΩ resistor | 1 | raw-battery divider, bottom half |
| Through-hole ceramic capacitor, ~1µF | 1 | divider-tap filter cap — cheap insurance, not load-bearing at this ADC's impedance, see "Battery voltage monitoring" |
| IP67 ABS junction box, ~125x75x75mm | 1 | |
| [RF Solutions CBA-UFLSMA-1](https://www.digikey.com/en/products/detail/rf-solutions/CBA-UFLSMA-1/5845754) U.FL-to-SMA bulkhead pigtail | 1 | bulkhead, standard SMA (not RP-SMA) — see "Enclosure and cable routing" |
| [Nearson S463AM-915](https://www.digikey.com/en/products/detail/nearson-inc/S463AM-915/4571855) 915MHz SMA antenna | 1 | standard SMA male, matches the pigtail above; no weatherproof rating specified — see "Enclosure and cable routing" |
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
