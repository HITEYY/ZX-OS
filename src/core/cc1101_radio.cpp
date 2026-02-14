#include "cc1101_radio.h"

#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <RCSwitch.h>
#include <SPI.h>

#include "board_pins.h"
#include "user_config.h"

namespace {

// Pin mapping from BruceFirmware T-Embed CC1101 profile (T_EMBED_1101).
constexpr uint8_t PIN_POWER_ON = boardpins::kPowerEnable;
constexpr uint8_t CC1101_GDO0_PIN = 3;
constexpr uint8_t CC1101_SW1_PIN = 47;
constexpr uint8_t CC1101_SW0_PIN = 48;
constexpr uint8_t CC1101_SS_PIN = boardpins::kCc1101Cs;
constexpr uint8_t CC1101_MISO_PIN = 10;
constexpr uint8_t CC1101_MOSI_PIN = 9;
constexpr uint8_t CC1101_SCK_PIN = 11;

constexpr uint8_t TX_POWER_DBM = 12;
constexpr float RF_MIN_MHZ = 280.0f;
constexpr float RF_MAX_MHZ = 928.0f;
constexpr float RF_SAFE_DEFAULT_MHZ = 433.92f;

constexpr unsigned long CC1101_BOOT_SETTLE_MS = 30UL;

bool gCc1101Ready = false;
float gCurrentFrequencyMhz = USER_DEFAULT_RF_FREQUENCY_MHZ;
RCSwitch gRcSwitch;

float clampFrequency(float mhz) {
  if (mhz < RF_MIN_MHZ || mhz > RF_MAX_MHZ) {
    return RF_SAFE_DEFAULT_MHZ;
  }
  return mhz;
}

void selectAntennaForFrequency(float mhz) {
  // BruceFirmware antenna switch logic for T-Embed CC1101.
  if (mhz <= 350.0f) {
    digitalWrite(CC1101_SW1_PIN, HIGH);
    digitalWrite(CC1101_SW0_PIN, LOW);
  } else if (mhz > 350.0f && mhz < 468.0f) {
    digitalWrite(CC1101_SW1_PIN, HIGH);
    digitalWrite(CC1101_SW0_PIN, HIGH);
  } else if (mhz > 778.0f) {
    digitalWrite(CC1101_SW1_PIN, LOW);
    digitalWrite(CC1101_SW0_PIN, HIGH);
  }
}

}  // namespace

bool initCc1101Radio() {
  gCurrentFrequencyMhz = clampFrequency(gCurrentFrequencyMhz);

  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);

  // Shared SPI bus lines must keep other devices deselected.
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  pinMode(CC1101_SW1_PIN, OUTPUT);
  pinMode(CC1101_SW0_PIN, OUTPUT);
  pinMode(CC1101_SS_PIN, OUTPUT);
  digitalWrite(CC1101_SS_PIN, HIGH);

  delay(CC1101_BOOT_SETTLE_MS);

  // UI(TFT_eSPI) already initializes the shared SPI bus on this board.
  // Re-beginning the same bus can trigger duplicate APB callback logs.
  ELECHOUSE_cc1101.setBeginEndLogic(false);
  ELECHOUSE_cc1101.setSPIinstance(&SPI);
  ELECHOUSE_cc1101.setSpiPin(CC1101_SCK_PIN,
                             CC1101_MISO_PIN,
                             CC1101_MOSI_PIN,
                             CC1101_SS_PIN);
  ELECHOUSE_cc1101.setGDO0(CC1101_GDO0_PIN);
  ELECHOUSE_cc1101.Init();

  if (!ELECHOUSE_cc1101.getCC1101()) {
    gCc1101Ready = false;
    return false;
  }

  ELECHOUSE_cc1101.setModulation(2);  // ASK/OOK
  ELECHOUSE_cc1101.setPA(TX_POWER_DBM);
  ELECHOUSE_cc1101.setRxBW(256);

  selectAntennaForFrequency(gCurrentFrequencyMhz);
  ELECHOUSE_cc1101.setMHZ(gCurrentFrequencyMhz);

  pinMode(CC1101_GDO0_PIN, OUTPUT);
  ELECHOUSE_cc1101.SetTx();

  gRcSwitch.enableTransmit(CC1101_GDO0_PIN);
  gRcSwitch.setRepeatTransmit(10);

  gCc1101Ready = true;
  return true;
}

bool isCc1101Ready() {
  return gCc1101Ready;
}

float getCc1101FrequencyMhz() {
  return gCurrentFrequencyMhz;
}

void setCc1101FrequencyMhz(float mhz) {
  gCurrentFrequencyMhz = clampFrequency(mhz);
  if (!gCc1101Ready) {
    return;
  }
  selectAntennaForFrequency(gCurrentFrequencyMhz);
  ELECHOUSE_cc1101.setMHZ(gCurrentFrequencyMhz);
}

bool transmitCc1101(uint32_t code,
                    int bits,
                    int pulseLength,
                    int protocol,
                    int repeat,
                    String &errorOut) {
  if (!gCc1101Ready) {
    errorOut = "CC1101 not initialized";
    return false;
  }

  if (bits < 1 || bits > 32) {
    errorOut = "bits must be 1..32";
    return false;
  }
  if (pulseLength < 50 || pulseLength > 5000) {
    errorOut = "pulseLength out of range (50..5000)";
    return false;
  }
  if (protocol < 1 || protocol > 12) {
    errorOut = "protocol out of range (1..12)";
    return false;
  }
  if (repeat < 1 || repeat > 50) {
    errorOut = "repeat out of range (1..50)";
    return false;
  }

  setCc1101FrequencyMhz(gCurrentFrequencyMhz);

  ELECHOUSE_cc1101.setModulation(2);
  ELECHOUSE_cc1101.setPA(TX_POWER_DBM);
  pinMode(CC1101_GDO0_PIN, OUTPUT);
  ELECHOUSE_cc1101.SetTx();

  gRcSwitch.setProtocol(protocol);
  gRcSwitch.setPulseLength(pulseLength);
  gRcSwitch.setRepeatTransmit(repeat);
  gRcSwitch.send(code, bits);
  return true;
}

void appendCc1101Info(JsonObject obj) {
  obj["board"] = "LilyGo T-Embed CC1101";
  obj["cc1101Ready"] = gCc1101Ready;
  obj["cc1101Present"] = gCc1101Ready ? ELECHOUSE_cc1101.getCC1101() : false;
  obj["frequencyMhz"] = gCurrentFrequencyMhz;
}
