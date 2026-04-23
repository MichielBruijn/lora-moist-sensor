#include <RadioLib.h>
#include <DHT.h>
#include <Preferences.h>
#include "secrets.h"

// SX1276 radio module pin mapping
// CS=20, IRQ=2, RST=3, DIO0=21
SX1276 radio = new Module(20, 2, 3, 21);

// DHT11 sensor on pin 1, powered from 3.3V rail (always on)
#define DHTPIN 1

// Onboard LED for visual debug (strapping pin, only used briefly at boot)
#define LED_PIN 8

// SX1276 power supply via GPIO pins (3x 40mA = 120mA max)
// Pins 8 and 9 are strapping pins on ESP32-C3, do not use for power
#define PWR1 0
#define PWR2 7
#define PWR3 10

// LoRaWAN configuration
const LoRaWANBand_t Region = EU868;
const uint8_t subBand = 0;
LoRaWANNode node(&radio, &Region, subBand);

// LoRaWAN credentials are defined in secrets.h (not committed to git)

DHT dht(DHTPIN, DHT11);
Preferences store;

// Deep sleep duration: 2 minutes for testing, change to 30 min for production

#define SLEEP_US (30 * 60 * 1000000ULL)

// Session buffer in RTC memory (survives deep sleep, lost on reset/power cycle)
RTC_DATA_ATTR uint8_t LWsession[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
RTC_DATA_ATTR bool sessionSaved = false;
RTC_DATA_ATTR int bootCount = 0;

// Nonce buffer loaded from NVS flash (survives both deep sleep and power cycle)
uint8_t LWnonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];

// Blink onboard LED to visually confirm wake-up (number of blinks = boot count)
void blinkLED() {
  pinMode(LED_PIN, OUTPUT);
  int blinks = min(bootCount, 10); // cap at 10 to keep it short
  for (int i = 0; i < blinks; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(200);
    digitalWrite(LED_PIN, LOW);
    delay(200);
  }
  pinMode(LED_PIN, INPUT); // release strapping pin
}

// Power on the SX1276 module via GPIO pins
void radioOn() {
  pinMode(PWR1, OUTPUT); digitalWrite(PWR1, HIGH);
  pinMode(PWR2, OUTPUT); digitalWrite(PWR2, HIGH);
  pinMode(PWR3, OUTPUT); digitalWrite(PWR3, HIGH);
  delay(200); // allow voltage to stabilize after power on
}

// Power off the SX1276 module and set all pins to INPUT
void radioOff() {
  SPI.end();
}

// Enter deep sleep with all pins floating
void goSleep() {
  SPI.end();
  // Cut power to SX1276
  pinMode(PWR1, INPUT);
  pinMode(PWR2, INPUT);
  pinMode(PWR3, INPUT);
  // Pull all SPI and DIO lines low
  pinMode(20, OUTPUT); digitalWrite(20, LOW);
  pinMode(6,  OUTPUT); digitalWrite(6,  LOW);
  pinMode(5,  OUTPUT); digitalWrite(5,  LOW);
  pinMode(4,  OUTPUT); digitalWrite(4,  LOW);
  pinMode(3,  OUTPUT); digitalWrite(3,  LOW);
  pinMode(21, OUTPUT); digitalWrite(21, LOW);
  pinMode(2,  OUTPUT); digitalWrite(2,  LOW);
  pinMode(1,  INPUT);
  esp_sleep_enable_timer_wakeup(SLEEP_US);
  esp_deep_sleep_start();
}

void setup() {
  bootCount++;

  // Visual debug: blink LED to confirm wake-up
  blinkLED();

  // Power on radio and initialize SPI
  radioOn();
  SPI.begin(6, 5, 4, 20);

  // Start DHT sensor (powered from 3.3V rail, always on)
  dht.begin();
  delay(2000); // DHT11 needs 1-2s to stabilize after power on

  // Initialize the SX1276 radio
  int16_t state = radio.begin();
  if (state != RADIOLIB_ERR_NONE) {
    goSleep();
  }

  radio.setOutputPower(17);
  node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);

  // Load nonces from NVS flash (survives power cycle)
  store.begin("lorawan");
  bool hasNonces = store.isKey("nonces");
  if (hasNonces) {
    store.getBytes("nonces", LWnonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  }
  store.end();

  // Restore nonces (from NVS) and session (from RTC) if available
  if (hasNonces) {
    node.setBufferNonces(LWnonces);
  }
  if (sessionSaved) {
    node.setBufferSession(LWsession);
  }

  // Activate: returns RADIOLIB_LORAWAN_NEW_SESSION (-1118) for fresh join
  // or RADIOLIB_LORAWAN_SESSION_RESTORED (-1117) for restored session
  state = node.activateOTAA();

  if (state != RADIOLIB_LORAWAN_NEW_SESSION && state != RADIOLIB_LORAWAN_SESSION_RESTORED) {
    goSleep();
  }

  // Save nonces to NVS flash (survives power cycle and deep sleep)
  memcpy(LWnonces, node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  store.begin("lorawan");
  store.putBytes("nonces", LWnonces, RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  store.end();

  // Save session to RTC memory (survives deep sleep only)
  memcpy(LWsession, node.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
  sessionSaved = true;

  // Disable ADR and duty cycle enforcement, use SF12 for max range
  node.setADR(false);
  node.setDatarate(0);
  node.setDutyCycle(false);

  // Read temperature and humidity
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  // Encode payload: temp*10 as int16, humidity*10 as uint16 (big endian)
  uint8_t payload[4];
  int16_t temp  = (int16_t)(t * 10);
  uint16_t humi = (uint16_t)(h * 10);
  payload[0] = temp >> 8;
  payload[1] = temp & 0xFF;
  payload[2] = humi >> 8;
  payload[3] = humi & 0xFF;

  // Send uplink on port 1, check for downlink
  uint8_t downlinkPayload[10];
  size_t downlinkSize = 0;
  LoRaWANEvent_t uplinkDetails;
  LoRaWANEvent_t downlinkDetails;

  node.sendReceive(payload, sizeof(payload), 1, downlinkPayload,
                   &downlinkSize, false, &uplinkDetails, &downlinkDetails);

  // Enter deep sleep until next measurement cycle
  goSleep();
}

void loop() {}