# Sensor reference — interpretation & wiring

Datasheets live in [`docs/datasheets/`](datasheets/). This file distills how to
*read* each sensor's data correctly. Numbers are from the datasheets cited per
section; where a part is "qualitative only" that is the manufacturer's own word
and is called out explicitly.

ESP32 target: **ESP32-D0WDQ6** (classic ESP32, 2× ADC, 240 MHz, no USB-native).

---

## Wiring summary (which pin goes where)

All pins below match [`include/config.h`](../include/config.h). Power: every
sensor here runs from **3V3** and **GND** (the ESP32 is a 3.3 V part — do **not**
feed sensor signals from 5 V even though the gas/soil boards *accept* 5 V supply;
see the per-sensor notes).

| Sensor | Interface | Signal | ESP32 GPIO | Notes |
|--------|-----------|--------|-----------|-------|
| **SEN66** | I²C @0x6B | SDA | **GPIO16** | shared bus |
| | | SCL | **GPIO17** | shared bus |
| **ADXL345** | I²C @0x53 | SDA / SCL | 16 / 17 | SDO→GND sets 0x53 |
| | | INT1 | **GPIO27** | optional motion interrupt |
| **GY-30 / BH1750** | I²C @0x23 | SDA / SCL | 16 / 17 | ADDR→GND sets 0x23 |
| **SEN0563 HCHO** | analog | AOUT | **GPIO35** (ADC1_CH7) | input-only pin, fine for ADC; moved 32→35 |
| **Capacitive soil** | analog | AOUT | **GPIO34** (ADC1_CH6) | input-only pin, fine for ADC |
| **SEN0564 CO** | analog | AOUT | **GPIO39 / VN** (ADC1_CH3) | input-only pin, fine for ADC |
| **Battery divider** | analog | Vtap | **GPIO32** (ADC1_CH4) | external divider: BAT+→1M→tap→2M→GND, +0.1µF tap→GND; Vbat=Vpin×1.5 |
| **INMP441** | I²S | SCK (BCLK) | **GPIO26** | |
| | | WS (LRCLK) | **GPIO25** | |
| | | SD (DOUT) | **GPIO33** | mic→ESP32 |
| | | L/R | **GND** | GND = left channel |
| Status LED | GPIO | — | **GPIO2** | on-board on many boards |

### Why these pins (ESP32-D0WDQ6 gotchas)
- **All three analog sensors are on ADC1 (GPIO32–39).** ADC2 pins cannot be read
  while Wi-Fi is active. If you ever enable Wi-Fi for NTP, ADC1 keeps working.
- **GPIO34/35 are input-only** (no internal pull-ups, no output drivers) — ideal
  for analog inputs, useless for anything that must drive a line.
- **GPIO12 (MTDI) is a boot strapping pin** — left unused here; never wire a
  sensor that idles high to it.
- **I²S** can route to almost any GPIO via the GPIO matrix; 25/26/33 are convenient
  and conflict-free here. GPIO33 is ADC1 too but we use it as a digital I²S input,
  so it is unavailable for a 4th analog sensor.
- **The default ESP32 I²C pins (21/22) are not all broken out** on this board, so
  the bus is remapped to 16/17. Any free GPIO pair works for `Wire.begin(sda,scl)`.

### Shared I²C addresses — check before powering
SEN66 0x6B · ADXL345 0x53 · BH1750 0x23 are all distinct, no conflict. If you ever
add the DFRobot *digital* gas boards they default to the same address and must be
re-strapped first.

---

## SEN66 — PM / RH-T / VOC / NOx / CO₂  (Sensirion)
Datasheet: [`datasheets/SEN66.pdf`](datasheets/SEN66.pdf) · I²C address **0x6B**, 3.3 V.

The Sensirion driver returns **already-scaled** engineering values, so no manual
fixed-point conversion is needed. Interpretation of each field:

| Field | Unit | Range | How to read it |
|-------|------|-------|----------------|
| PM1.0 / PM2.5 / PM4 / PM10 | µg/m³ | 0–1000 | mass concentration, ±10 %. Compare to WHO/EU limits (e.g. PM2.5 24 h guideline 15 µg/m³). |
| **VOC Index** | index | 1–500 | **Not ppb.** Relative to a rolling 24 h baseline. **100 = typical recent indoor air**. >100 = elevated vs recent baseline; <100 = cleaner. Algorithm needs several hours to learn a stable baseline — early readings drift. Sustained >150 indicates a noticeable VOC event; >300 is significant. |
| **NOx Index** | index | 1–500 | Similar but **baseline = 1** (not 100). Outdoor/background air maps to ~1. Rises sharply with NO₂/NOx: cooking flames, traffic ingress, diesel exhaust. Values >20 indicate a meaningful NOx event indoors; >50 is high. Also adaptive — takes hours to settle. |
| CO₂ | ppm | 400–40000 | ±(50 ppm + 5 %). 400 ≈ outdoor air; >1000 indicates poor ventilation; >2000 noticeably stuffy. |
| Temperature | °C | — | ±0.45 °C typ. Reads slightly high from self-heating; SEN66 compensates internally but keep it out of other heat sources. |
| Humidity | %RH | 0–100 | ±4.5 %RH typ. |

**Warm-up / conditioning:** PM fan + measurement start within ~1 s, but the gas
indices (VOC/NOx) need their adaptive algorithm to settle — treat the first few
hours of index data as unstable. CO₂ has a short warm-up; for best CO₂ accuracy run
the SEN66's automatic self-calibration (ASC) by exposing it to fresh ~400 ppm air
periodically.

**Reading cadence:** the internal cycle is ~1 Hz; polling faster just re-returns the
same frame. `readMeasuredValues()` returning non-zero means "no new data yet" — not
necessarily an error.

---

## ADXL345 — 3-axis accelerometer  (Analog Devices)
Datasheet: [`datasheets/ADXL345.pdf`](datasheets/ADXL345.pdf) · I²C address **0x53**
(SDO→GND) or 0x1D (SDO→VDD), 3.3 V.

- **Identity check:** DEVID register `0x00` must read **0xE5**. The self-test in
  firmware uses this to distinguish "present and is really an ADXL345" from a random
  I²C ACK.
- **Data format:** registers `0x32`–`0x37` (DATAX0..DATAZ1), **16-bit two's
  complement, little-endian** (low byte first), right-justified.
- **Scale:** in **FULL_RES mode** the scale is fixed at **~3.9 mg/LSB**
  (≈256 LSB/g) across all ranges. So `g = raw * 0.0039`. At rest one axis reads
  ≈ ±1 g (gravity); magnitude of the 3-axis vector ≈ 1 g when still.
- **Use here:** the Adafruit_ADXL345 library handles the register reads and returns
  m/s². For "rumble"/vibration logging, watch the AC magnitude
  `sqrt(x²+y²+z²) − g`, or wire **INT1 (GPIO27)** with the activity interrupt so the
  ESP32 only logs on events.
- **Range:** set ±2/4/8/16 g via DATA_FORMAT (`0x31`). ±2 g gives best resolution
  for tilt; pick ±16 g if you expect hard knocks.

---

## GY-30 / BH1750FVI — ambient light  (ROHM)
Datasheet: [`datasheets/BH1750FVI.pdf`](datasheets/BH1750FVI.pdf) · I²C address
**0x23** (ADDR→GND) or 0x5C (ADDR→VDD), 3.3 V.

- **Output is directly in lux** after one division — no analog math. The light-
  sensing element is the clear-topped square chip; keep it unobstructed and facing
  the light.
- **Conversion:** `lux = raw_count / 1.2` in the high-resolution modes (the 1.2 is
  the datasheet "measurement accuracy" factor).
- **Modes:**
  - `CONTINUOUS_HIGH_RES_MODE` — 1 lx resolution, ~120 ms/measurement (default, good).
  - `CONTINUOUS_HIGH_RES_MODE_2` — 0.5 lx resolution, divide by an **additional 2**.
  - `LOW_RES_MODE` — 4 lx, ~16 ms (fast but coarse).
- **MTreg (measurement time register):** scales sensitivity. Effective
  `lux = raw / 1.2 × (69 / MTreg)`, default MTreg = 69. Raise MTreg in very dim light
  for more resolution; lower it under bright light to avoid saturation
  (range MTreg 31–254). The `claws/BH1750` library exposes `setMTreg()`.
- **Spectral response:** peaks near **560 nm** (green), roughly approximating the
  human eye — it is a *photopic-ish lux* sensor, not a true radiometric one. Behind a
  tinted/frosted window, calibrate with a known reference; a diffuser improves cosine
  (angular) response at the cost of absolute lux.

---

## SEN0564 — Fermion MEMS **CO** (DFRobot, Winsen GM-702B element)
Datasheet: [`datasheets/SEN0564_CO_datasheet.pdf`](datasheets/SEN0564_CO_datasheet.pdf) ·
analog, 3.3–5 V supply, **AOUT → GPIO39 (VN)**.

> **Qualitative only.** DFRobot states this explicitly: for true ppm you need a
> factory-calibrated sensor. We log a *relative trend*, not a regulatory CO number.

**How it works (metal-oxide / MEMS MOS):** a heated SnO₂-type film (Winsen GM-702B)
whose resistance `Rs` drops as CO rises. The breakout puts `Rs` in a divider with
load resistor **RL = 4.7 kΩ**; AOUT is the voltage across RL:

```
Rs = RL × (Vsupply − Vout) / Vout    // sensor resistance [Ω]
ratio = Rs / R0                       // R0 = Rs in clean air (your baseline)
```

**Sensitivity curve** (from GM-702B datasheet Fig 3, log-log, standard conditions
20 °C / 55 %RH — read from graph, ±10 %):

| CO (ppm) | Rs/R0 |
|----------|-------|
| 10       | ~0.55 |
| 30       | ~0.37 |
| 50       | ~0.30 |
| 100      | ~0.24 |
| 150      | ≤0.33 (spec min: R0/Rs ≥ 3) |
| 200      | ~0.20 |
| 500      | ~0.14 |
| 1 000    | ~0.12 |
| 5 000    | ~0.10 |

Approximate ppm from ratio (log-log linear fit): `ppm ≈ 10^((log10(Rs/R0) − 0.4) / −0.45)`
— order-of-magnitude only; calibrate R0 in known clean air first.

**Cross-sensitivity (from datasheet):** H₂ and CH₄ also reduce Rs but less than CO
at the same ppm (CO curve sits below H₂/CH₄ curves on the log plot). Alcohols,
cigarette smoke and other reducing gases will also trigger a response.

**Measured clean-air baseline (your unit):** Vout = 333 mV → **R0 ≈ 41 900 Ω**
at ~20 °C in indoor air. Store this as your reference; it will shift with temperature
and humidity (Fig 4 shows ±40 % Rs/Rs0 from −20 to +50 °C).

**Warm-up:** usable after ~1–3 min; stable baseline after 10–15 min. Brand-new sensor:
allow 24–48 h burn-in at rated voltage for a stable R0.

**Supply note:** heater rated 2.5 V (high) / 0.5 V (low) — the DFRobot board drives
this internally. Power board from 3V3; AOUT stays in the ADC range.

---

## SEN0563 — Fermion MEMS **HCHO / formaldehyde** (DFRobot, SMD1001 element)
Datasheet: [`datasheets/SEN0563_HCHO_datasheet.pdf`](datasheets/SEN0563_HCHO_datasheet.pdf) ·
analog, 3.3–5 V supply, **AOUT → GPIO35** (moved from GPIO32, now the battery tap), range **0–3 ppm**.

Same MEMS MOS architecture as the CO sensor (Suzhou Huiwen Nanotech SMD1001 chip).
Key differences:

- **Load resistor RL = 10 kΩ** (not 4.7 kΩ — use the right value in the Rs formula).
- Resolution: **0.04 ppm**. WHO 30-min guideline ≈ 0.08 ppm; interesting range is very low end.
- Heater: 1.8 V AC/DC (driven internally on the DFRobot board).

**Sensitivity curve** (from SMD1001 datasheet Fig 1, RL = 10 kΩ, 20 °C / 55 %RH).
The graph plots **Vs/V0** (voltage-ratio on RL) vs ppm, not Rs/R0 directly:

| HCHO (ppm) | Vs/V0 (voltage ratio) | Rs/R0 (approx, R0≈142 kΩ) |
|------------|----------------------|---------------------------|
| 0.1        | ~1.22                | ~0.76                     |
| 0.2        | ~1.45                | ~0.63                     |
| 0.4        | ~1.78                | ≤0.56 (spec: R0/Rs ≥ 1.8) |
| 0.6        | ~1.87                | ~0.52                     |
| 0.8        | ~2.12                | ~0.44                     |
| 1.0        | ~2.20                | ~0.42                     |
| 1.2        | ~2.65                | ~0.34                     |

Conversion: `Rs/R0 = ((R0 + RL) / (Vs/V0) − RL) / R0` — with R0≈142 kΩ, RL=10 kΩ.

**Measured clean-air baseline (your unit):** Vout = 216 mV →
**R0 = 10 000 × (3300 − 216) / 216 ≈ 142 800 Ω**.
*(Note: an earlier calculation used RL=4.7 kΩ by mistake and gave ~67 kΩ — that was wrong.)*

**Cross-sensitivity:** responds to other reducing gases and VOCs, not HCHO-selective.
Use the SEN66 VOC Index for a better VOC picture; use this for relative HCHO trends.

> SKU reminder: **SEN0567 = ammonia (NH₃)**, **SEN0566 = VOC**, **SEN0563 = HCHO**.

---

## Capacitive soil moisture sensor (generic analog)
No chip datasheet — these are unbranded capacitive probes. **AOUT → GPIO34**, 3V3.

- **Capacitive, not resistive:** measures the dielectric change of soil. Output is an
  analog voltage that **falls as moisture rises** (more water → higher capacitance →
  lower oscillator-derived voltage), though some boards invert this — verify on yours.
- **No absolute units.** Calibrate two endpoints and map linearly:
  - `V_dry` = reading in air / bone-dry soil,
  - `V_wet` = reading in a glass of water / saturated soil,
  - `moisture% = 100 × (V_dry − V_raw) / (V_dry − V_wet)` (clamp 0–100).
- **Supply matters:** these are notoriously supply-sensitive; the AOUT scales with
  Vcc. Power from a *stable* 3V3 and keep it the same rail you calibrated on. Many
  cheap boards are designed for 5 V and their AOUT can exceed 3.3 V at 5 V supply —
  power from 3V3 (or divide) so the ADC sees ≤3.1 V.
- **Placement:** insert to a consistent depth; the white line marks the max safe
  insertion (electronics must stay out of the soil). Reading drifts with temperature
  and soil salinity — it is a relative trend, not lab-grade.

---

## INMP441 — I²S MEMS microphone  (TDK InvenSense)
Datasheet: [`datasheets/INMP441.pdf`](datasheets/INMP441.pdf) · I²S, 3.3 V.
Wiring: SCK→GPIO26, WS→GPIO25, SD→GPIO33, L/R→GND (left).

### Electrical / data format
- **24-bit, two's-complement, MSB-first**, data clocked on the I²S frame. We read
  32-bit slots and the sample sits in the upper 24 bits → shift right by 8 (or treat
  as 32-bit and scale). Full scale = 2³¹ if read as 32-bit, or 2²³ for the 24-bit
  sample.
- **L/R pin:** tie to **GND** → the mic drives the **left** channel (data valid when
  WS is low). Tie to VDD for the right slot. Read one mono channel.
- **Clocks:** WS frequency **= the audio sample rate** (we use 16 kHz; raise to
  48 kHz for full octave-band coverage). BCLK = 64 × WS for a stereo 32-bit frame.

### Acoustic specs (the numbers that turn counts into dB)
- **Sensitivity: −26 dBFS at 94 dB SPL** (1 kHz). This is the anchor for the SPL
  estimate.
- **Acoustic Overload Point (AOP): 116 dB SPL** — clipping above this.
- **SNR ≈ 61 dB(A)**, equivalent input noise / **noise floor ≈ 33 dB(A)** SPL — you
  cannot meaningfully measure quieter than ~33 dB(A).
- **Frequency response:** roughly **60 Hz – 15 kHz** (usable; rolls off below ~60 Hz).

### Converting samples → sound level
1. Capture an N-sample window (power-of-two for FFT, e.g. 2048).
2. **RMS** of the (DC-removed) samples → `dBFS = 20·log10(rms / FULL_SCALE)`.
3. **Absolute SPL estimate** from the sensitivity anchor:
   `SPL ≈ dBFS + 120` because 94 dB SPL ⇒ −26 dBFS, so the offset is 94−(−26)=**120**.
   This is **uncalibrated** (±a few dB part-to-part); for real measurements calibrate
   the offset against a reference SPL meter and store it.
4. **A-weighting** for `dB(A)` (what regulations use): weight each FFT bin by the
   IEC 61672 A-curve, sum the weighted energy, convert back to dB. The firmware does
   this per FFT bin (`micA_weight()` in [`src/mic.cpp`](../src/mic.cpp)).

### Frequency bands
The firmware bins the FFT into standard **octave bands** centred at
31.5, 63, 125, 250, 500, 1k, 2k, 4k, 8k Hz, and reports each band's level
(both unweighted and A-weighted). At 16 kHz sample rate the 8 kHz band is partly
above Nyquist — **set `I2S_SAMPLE_RATE_HZ` to 48000** for clean coverage of all
bands (the low 31.5/63 Hz bands also need a longer window: FFT size 2048 @ 48 kHz =
~43 ms, giving ~23 Hz bins — adequate but coarse at the very bottom).

### Interpreting against Dutch noise regulation
Dutch environmental noise law (*Activiteitenbesluit milieubeheer*, and *Wet
geluidhinder* / *Omgevingswet*) is written in **A-weighted levels**:
- **L_Ar,LT** (long-term rated level, industry/horeca) standard limits at dwellings:
  **50 dB(A) day (07–19 h), 45 dB(A) evening (19–23 h), 40 dB(A) night (23–07 h)**.
  Night peak `L_Amax` typically capped ~60 dB(A).
- **L_den** (day-evening-night, used for road/rail/industrial zoning) applies a
  **+5 dB penalty to evening and +10 dB to night** samples before averaging — this
  is the metric to compute if you want to compare to zoning limits (`L_den` preferred
  values often 48–53 dB depending on source).
- The energy-average metric is **L_Aeq,T** = `10·log10(mean(10^(L_A/10)))` over the
  period T. Accumulate this continuously; that, plus per-band breakdown, is what
  makes the data comparable to the regulations.

**Caveat:** the INMP441 is a single uncalibrated MEMS mic, not a Class-1/2 sound
level meter. Treat all dB(A) values as **relative trends** until you calibrate the
offset, and remember the ~33 dB(A) noise floor sets the quietest measurable level.

---

## Quick interpretation cheat-sheet

| Sensor | Raw → meaning | Absolute? |
|--------|---------------|-----------|
| SEN66 PM | µg/m³ direct | yes (±10 %) |
| SEN66 VOC/NOx | index, 100/1 = recent baseline | relative |
| SEN66 CO₂ | ppm direct | yes (±50 ppm+5 %) |
| ADXL345 | `g = raw × 3.9 mg`, DEVID 0xE5 | yes |
| BH1750 | `lux = raw/1.2 × 69/MTreg` | yes |
| SEN0564 CO | `Rs=4700(Vcc−Vo)/Vo`, R0≈41 900 Ω, track `Rs/R0` | **relative** |
| SEN0563 HCHO | `Rs=10000(Vcc−Vo)/Vo`, R0≈142 800 Ω, Vs/V0 curve in § above | **relative** |
| Soil | map `V_dry…V_wet` → 0–100 % | relative |
| INMP441 | `dBFS=20log10(rms/FS)`, `SPL≈dBFS+120`, A-weight for dB(A) | relative until calibrated |
