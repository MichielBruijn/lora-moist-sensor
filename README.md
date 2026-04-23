# LoRa Moist Sensor

Ultra low-power LoRaWAN temperature and humidity sensor built around an ESP32-C3 and SX1276.
Designed to run for months on a single 18650 cell.

## Components

| Component | Role |
|-----------|------|
| ESP32-C3 (bare module) | Microcontroller, deep sleep host |
| SX1276 LoRa transceiver | LoRaWAN uplink (EU868, SF12) |
| DHT11 | Temperature and humidity measurement |
| HT7333 LDO regulator | 3.3 V supply, ~55 µA quiescent current |
| 18650 Li-Ion cell | Main power source |

## Wiring (SPI + power)

| SX1276 function | ESP32-C3 GPIO |
|-----------------|---------------|
| CS | 20 |
| IRQ | 2 |
| RST | 3 |
| DIO0 | 21 |
| SCK | 6 |
| MISO | 5 |
| MOSI | 4 |
| VCC (via GPIO) | 0, 7, 10 |

DHT11 data pin: GPIO 1, powered from the 3.3 V rail.

## Hardware modifications

Out-of-the-box dev boards are not optimised for battery operation. Two physical modifications were made before deploying this device:

**ESP32-C3 dev board :** the onboard power LED was desoldered. A typical power LED with a 1 kΩ series resistor draws ~3 mA continuously — more than ten times the entire sleep budget of the circuit.

**Moisture sensor board:** the onboard power LED was desoldered. Note: this was not strictly necessary — the sensor is only powered when switched on via GPIO, so the LED would only be on during the brief measurement window. It was removed out of caution.

Both LEDs were removed with a soldering iron. No other components were changed. The measured sleep floor is 220 µA, with periodic peaks to 330 µA from the DHT11's internal measurement cycle.

## Credentials

Copy `secrets.h.example` to `secrets.h` and fill in your LoRaWAN keys before compiling:

```bash
cp secrets.h.example secrets.h
```

Register your device on [The Things Network](https://www.thethingsnetwork.org/) or another LNS using OTAA to obtain `devEUI`, `appKey`, and `nwkKey`.

`secrets.h` is excluded from git via `.gitignore` so your keys never end up in version control.

## Dependencies

Install these via the Arduino Library Manager:

- [RadioLib](https://github.com/jgromes/RadioLib)
- [DHT sensor library](https://github.com/adafruit/DHT-sensor-library) (Adafruit)

## How the power budget works

The core challenge with battery-powered LoRa sensors is that most designs waste energy in one of three places: the regulator sitting idle, the radio module leaking current while "off", or the MCU staying awake between measurements. This design addresses all three.

### 1. ESP32-C3 deep sleep

Between measurements the ESP32-C3 enters deep sleep, drawing around **5 µA**. The RTC domain stays alive to keep the 30-minute wakeup timer running and to preserve two small buffers in RTC RAM.

### 2. LoRaWAN session in RTC memory

A normal OTAA join takes several seconds and multiple radio round-trips. On every wake-up that would dominate the energy budget. Instead, the LoRaWAN nonces and session state (frame counters, session keys) are stored in `RTC_DATA_ATTR` variables that survive deep sleep. From the third boot onward the node skips the join entirely and goes straight to sending, keeping the radio on for only a few seconds per cycle.

### 3. SX1276 powered via three GPIO pins

The SX1276 module is not connected to the 3.3 V rail directly. Instead, its VCC is fed from three ESP32-C3 GPIO output pins in parallel (GPIO 0, 7, 10), each rated for 40 mA, giving 120 mA combined — enough to cover SX1276 TX at 17 dBm (~120 mA peak). When going to sleep, those pins are switched to high-impedance inputs, cutting all power to the radio.

This would not be enough on its own: the SX1276 has ESD protection diodes on every IO pin. If the SPI lines (MOSI, MISO, SCK, CS) and DIO lines are left floating or high while VCC is 0 V, current leaks back through those diodes and the "off" radio still draws several milliamps. The `radioOff()` function therefore explicitly pulls every SPI and DIO pin low before entering sleep, eliminating that leakage path entirely.

### 4. HT7333 LDO

A typical LDO regulator draws 1–5 mA of quiescent current regardless of load. The HT7333 draws around **55 µA**, which matters a lot when the system sleeps for 30 minutes at a time.

### Estimated battery life on a 2500 mAh 18650

The power budget has two distinct components: the sleep baseline and the transmit overhead.

**Sleep baseline (measured):**
220 µA × 24 h = **5.3 mAh/day**
This is the steady-state sleep floor: ESP32-C3 deep sleep ~5 µA + DHT11 standby + PCB leakage. The DHT11 adds periodic spikes up to 330 µA (~110 µA extra) roughly every 2 seconds as it runs its internal measurement cycle; these are brief enough that average current stays close to the 220 µA floor.

**Transmit overhead (estimated, 48 cycles/day):**

| Sub-phase | Duration | Current | Energy/day |
|-----------|----------|---------|------------|
| LoRaWAN TX (SF12, ~17 bytes) | ~2 s | 120 mA | ~3.2 mAh |
| ESP32-C3 active (boot, DHT, RX windows) | ~8 s | ~25 mA | ~2.7 mAh |
| **TX subtotal** | | | **~5.9 mAh** |

**Total: ~11 mAh/day**, giving an estimated runtime of:

> **2500 mAh ÷ 11 mAh/day ≈ 227 days (~7.5 months)**

Real-world runtime depends on cell capacity, temperature, and how often a fresh OTAA join is needed after a reset. Measuring actual active-phase current with a shunt resistor or power analyser would sharpen the estimate further.

## LoRa payload format

4 bytes, big-endian:

| Bytes | Type | Value |
|-------|------|-------|
| 0–1 | int16 | Temperature × 10 (°C) |
| 2–3 | uint16 | Humidity × 10 (%) |

Example: `01 1A 02 58` → 28.2 °C, 60.0 % RH

## License

MIT
