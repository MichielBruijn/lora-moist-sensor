#include <RadioLib.h>
#include <DHT.h>
#include <Preferences.h>
#include "secrets.h"

// =============================================================================
// LoRaWAN Temperature & Humidity Sensor
// =============================================================================
// ESP32-C3 based sensor node that reads DHT11 temperature/humidity data
// and transmits it via LoRaWAN (OTAA) every 30 minutes.
//
// Power management:
//   - SX1276 radio is powered via 3 GPIO pins (0, 7, 10) which are set to
//     INPUT during deep sleep to fully cut power to the module.
//   - DHT11 is powered from the 3.3V rail (always on, ~0.1mA idle).
//   - SPI and DIO lines are held LOW during sleep to prevent parasitic
//     power feeding into the SX1276 via its internal ESD diodes.
//   - Pins 8 and 9 are ESP32-C3 strapping pins and must not be used
//     for power switching as they interfere with boot mode selection.
//
// LoRaWAN persistence:
//   - DevNonce counter is stored in NVS flash so it survives power cycles
//     and battery replacements. Without this, TTN would reject joins after
//     a reset because the DevNonce would restart at 0 (already used).
//   - Session restore is not used because the SX1276 loses all internal
//     state when GPIO power is cut, causing checksum mismatches.
//     A fresh OTAA join is performed every wake cycle instead.
//
// LED debug patterns (active on pin 8, a strapping pin — used briefly):
//   1 flash (300ms)     = boot OK
//   3 fast flashes       = radio.begin() failed
//   2 flashes (300ms)   = radio OK, starting OTAA join
//   5 fast flashes       = join succeeded
//   1 long flash (2s)   = join failed
//
// Payload format (4 bytes, big endian):
//   [0-1] int16_t  temperature * 10  (e.g. 23.4°C → 0x00EA)
//   [2-3] uint16_t humidity * 10     (e.g. 42.0%  → 0x01A4)
// =============================================================================

// --- Radio module ---
// SX1276 wired to ESP32-C3: CS=GPIO20, IRQ=GPIO2, RST=GPIO3, DIO0=GPIO21
SX1276 radio = new Module(20, 2, 3, 21);

// --- Sensor ---
#define DHTPIN  1       // DHT11 data pin

// --- Onboard LED ---
#define LED_PIN 8       // Strapping pin, only used briefly for debug flashes

// --- Radio power pins ---
// Three GPIO pins supply Vcc to the SX1276 module in parallel.
// Each GPIO can source ~40mA; 3 pins provide ~120mA total, enough for
// the SX1276 TX peak of ~96mA at +17dBm.
#define PWR1    0
#define PWR2    7
#define PWR3    10

// --- LoRaWAN configuration ---
const LoRaWANBand_t Region = EU868;
const uint8_t subBand = 0;
LoRaWANNode node(&radio, &Region, subBand);

// LoRaWAN credentials (joinEUI, devEUI, appKey, nwkKey) are defined in
// secrets.h which is excluded from version control via .gitignore.

// --- Peripherals ---
DHT dht(DHTPIN, DHT11);
Preferences store;

// --- Sleep duration ---
#define SLEEP_US (30 * 60 * 1000000ULL)  // 30 minutes in microseconds

// --- RTC memory (survives deep sleep, lost on power cycle / reset) ---
RTC_DATA_ATTR int bootCount = 0;

// =============================================================================
// Power management functions
// =============================================================================

// Apply power to the SX1276 module via GPIO pins and wait for it to stabilize.
void radioOn() {
  pinMode(PWR1, OUTPUT); digitalWrite(PWR1, HIGH);
  pinMode(PWR2, OUTPUT); digitalWrite(PWR2, HIGH);
  pinMode(PWR3, OUTPUT); digitalWrite(PWR3, HIGH);
  delay(500);  // SX1276 needs time to stabilize after power-on
}

// Enter deep sleep with radio powered off and SPI/DIO lines held low.
// All power pins are set to INPUT to cut current to the SX1276.
// SPI and DIO pins are driven LOW to prevent parasitic current flowing
// into the SX1276 Vcc rail through its internal ESD protection diodes.
void goSleep() {
  SPI.end();

  // Cut power to SX1276
  pinMode(PWR1, INPUT);
  pinMode(PWR2, INPUT);
  pinMode(PWR3, INPUT);

  // Hold SPI and DIO lines low to prevent ESD diode leakage
  pinMode(20, OUTPUT); digitalWrite(20, LOW);   // CS
  pinMode(6,  OUTPUT); digitalWrite(6,  LOW);   // SCK
  pinMode(5,  OUTPUT); digitalWrite(5,  LOW);   // MISO
  pinMode(4,  OUTPUT); digitalWrite(4,  LOW);   // MOSI
  pinMode(3,  OUTPUT); digitalWrite(3,  LOW);   // RST
  pinMode(21, OUTPUT); digitalWrite(21, LOW);   // DIO0
  pinMode(2,  OUTPUT); digitalWrite(2,  LOW);   // IRQ

  // Let DHT data pin float
  pinMode(1, INPUT);

  esp_sleep_enable_timer_wakeup(SLEEP_US);
  esp_deep_sleep_start();
}

// =============================================================================
// LED debug helper
// =============================================================================

// Flash the onboard LED a given number of times.
// Pin 8 is a strapping pin, so it is released (set to INPUT) after use.
void ledFlash(int count, int onMs, int offMs) {
  pinMode(LED_PIN, OUTPUT);
  for (int i = 0; i < count; i++) {
    digitalWrite(LED_PIN, HIGH); delay(onMs);
    digitalWrite(LED_PIN, LOW);  delay(offMs);
  }
  pinMode(LED_PIN, INPUT);
}

// =============================================================================
// Main program — runs once per wake cycle, then enters deep sleep
// =============================================================================

void setup() {
  bootCount++;

  // --- Stage 1: Sign of life ---
  ledFlash(1, 300, 300);

  // --- Stage 2: Power on radio and initialize SPI ---
  radioOn();

  // SPI pins were held OUTPUT LOW during sleep; reset them to INPUT
  // so SPI.begin() can take control cleanly.
  pinMode(6,  INPUT);   // SCK
  pinMode(5,  INPUT);   // MISO
  pinMode(4,  INPUT);   // MOSI
  pinMode(20, INPUT);   // CS
  delay(10);

  SPI.begin(6, 5, 4, 20);

  // --- Stage 3: Start DHT11 sensor ---
  dht.begin();
  delay(2000);  // DHT11 needs 1-2 seconds to stabilize

  // --- Stage 4: Initialize radio ---
  int16_t state = radio.begin();
  if (state != RADIOLIB_ERR_NONE) {
    ledFlash(3, 100, 100);  // Signal radio failure
    goSleep();
  }

  ledFlash(2, 300, 300);  // Signal radio OK

  // --- Stage 5: LoRaWAN OTAA join ---
  // A fresh join is performed every wake cycle. Session restore is not used
  // because the SX1276 loses all internal state when GPIO power is cut,
  // causing session checksum mismatches on restore.
  radio.setOutputPower(17);               // +17 dBm (max for EU868)
  node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);

  // Load DevNonce counter from NVS flash so it continues incrementing
  // after power cycles and battery replacements. Without this, TTN would
  // reject joins because the DevNonce restarts at 0 (already used).
  store.begin("lorawan");
  if (store.isKey("nonces")) {
    uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    store.getBytes("nonces", nonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
    node.setBufferNonces(nonces);
  }
  store.end();

  state = node.activateOTAA();

  if (state == RADIOLIB_LORAWAN_NEW_SESSION) {
    ledFlash(5, 100, 100);  // Signal join success
  } else {
    ledFlash(1, 2000, 0);   // Signal join failure
    goSleep();              // Retry next wake cycle
  }

  // Save updated DevNonce counter to NVS flash for next boot
  uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
  memcpy(nonces, node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  store.begin("lorawan");
  store.putBytes("nonces", nonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  store.end();

  // --- Stage 6: Configure radio parameters ---
  node.setADR(false);         // Fixed data rate, no ADR
  node.setDatarate(0);        // DR0 = SF12BW125 (max range)
  node.setDutyCycle(false);   // Duty cycle managed by sleep interval

  // --- Stage 7: Read sensor data ---
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  // --- Stage 8: Encode and transmit payload ---
  // Format: 4 bytes, big endian
  //   bytes 0-1: temperature * 10 as signed int16
  //   bytes 2-3: humidity * 10 as unsigned uint16
  uint8_t payload[4];
  int16_t temp  = (int16_t)(t * 10);
  uint16_t humi = (uint16_t)(h * 10);
  payload[0] = temp >> 8;
  payload[1] = temp & 0xFF;
  payload[2] = humi >> 8;
  payload[3] = humi & 0xFF;

  // Send uplink on FPort 1, listen for optional downlink
  uint8_t downlinkPayload[10];
  size_t downlinkSize = 0;
  LoRaWANEvent_t uplinkDetails;
  LoRaWANEvent_t downlinkDetails;

  node.sendReceive(payload, sizeof(payload), 1, downlinkPayload,
                   &downlinkSize, false, &uplinkDetails, &downlinkDetails);

  // --- Stage 9: Enter deep sleep until next cycle ---
  goSleep();
}

// loop() is never reached — the device enters deep sleep at the end of setup()
void loop() {}
