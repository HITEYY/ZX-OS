#include "cc1101_radio.h"

#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <RCSwitch.h>
#include <SPI.h>

#include <cstring>

#include "board_pins.h"
#include "shared_spi_bus.h"
#include "user_config.h"
#include "../hal/board_config.h"

namespace {

// Pin mapping from HAL board config.
#if HAL_HAS_POWER_ENABLE
constexpr uint8_t PIN_POWER_ON = HAL_PIN_POWER_ENABLE;
#endif
constexpr uint8_t CC1101_GDO0_PIN = HAL_PIN_CC1101_GDO0;
#if HAL_HAS_ANTENNA_SWITCH
constexpr uint8_t CC1101_SW1_PIN = HAL_PIN_CC1101_SW1;
constexpr uint8_t CC1101_SW0_PIN = HAL_PIN_CC1101_SW0;
#endif
constexpr uint8_t CC1101_SS_PIN = HAL_PIN_CC1101_CS;
constexpr uint8_t CC1101_MISO_PIN = HAL_SPI_MISO;
constexpr uint8_t CC1101_MOSI_PIN = HAL_SPI_MOSI;
constexpr uint8_t CC1101_SCK_PIN = HAL_SPI_SCK;

constexpr uint8_t TX_POWER_DBM = 12;
constexpr float RF_MIN_MHZ = 280.0f;
constexpr float RF_MAX_MHZ = 928.0f;
constexpr float RF_SAFE_DEFAULT_MHZ = 433.92f;
constexpr size_t CC1101_MAX_PACKET_BYTES = 61;
constexpr int CC1101_MAX_RX_TIMEOUT_MS = 60000;
constexpr int CC1101_RX_POLL_MS = 5;
constexpr int CC1101_MIN_TX_DELAY_MS = 1;
constexpr int CC1101_MAX_TX_DELAY_MS = 2000;
constexpr int CC1101_DEFAULT_TX_DELAY_MS = 25;

constexpr unsigned long CC1101_BOOT_SETTLE_MS = 30UL;

bool gCc1101Ready = false;
float gCurrentFrequencyMhz = USER_DEFAULT_RF_FREQUENCY_MHZ;
Cc1101PacketConfig gPacketConfig;
RCSwitch gRcSwitch;

float clampFrequency(float mhz) {
  if (mhz < RF_MIN_MHZ || mhz > RF_MAX_MHZ) {
    return RF_SAFE_DEFAULT_MHZ;
  }
  return mhz;
}

void selectAntennaForFrequency(float mhz) {
#if HAL_HAS_ANTENNA_SWITCH
  // Antenna switch logic for boards with dual-band antenna switching.
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
#else
  (void)mhz;
#endif
}

bool validatePacketConfig(const Cc1101PacketConfig &config, String &errorOut) {
  if (config.modulation > 4) {
    errorOut = "modulation must be 0..4";
    return false;
  }
  if (config.dataRateKbps < 0.05f || config.dataRateKbps > 500.0f) {
    errorOut = "dataRate must be 0.05..500 kbps";
    return false;
  }
  if (config.deviationKHz < 1.0f || config.deviationKHz > 380.0f) {
    errorOut = "deviation must be 1..380 kHz";
    return false;
  }
  if (config.rxBandwidthKHz < 58.0f || config.rxBandwidthKHz > 812.0f) {
    errorOut = "rxBW must be 58..812 kHz";
    return false;
  }
  if (config.syncMode > 7) {
    errorOut = "syncMode must be 0..7";
    return false;
  }
  if (config.packetFormat > 3) {
    errorOut = "packetFormat must be 0..3";
    return false;
  }
  if (config.lengthConfig > 3) {
    errorOut = "lengthConfig must be 0..3";
    return false;
  }
  if (config.packetLength == 0) {
    errorOut = "packetLength must be 1..255";
    return false;
  }
  return true;
}

void applyPacketConfigNoValidate(const Cc1101PacketConfig &config) {
  setCc1101FrequencyMhz(gCurrentFrequencyMhz);

  ELECHOUSE_cc1101.setSidle();
  ELECHOUSE_cc1101.setModulation(config.modulation);
  ELECHOUSE_cc1101.setChannel(config.channel);
  ELECHOUSE_cc1101.setDRate(config.dataRateKbps);
  ELECHOUSE_cc1101.setDeviation(config.deviationKHz);
  ELECHOUSE_cc1101.setRxBW(config.rxBandwidthKHz);
  ELECHOUSE_cc1101.setPktFormat(config.packetFormat);
  ELECHOUSE_cc1101.setCrc(config.crcEnabled);
  ELECHOUSE_cc1101.setLengthConfig(config.lengthConfig);
  ELECHOUSE_cc1101.setPacketLength(config.packetLength);
  ELECHOUSE_cc1101.setWhiteData(config.whitening);
  ELECHOUSE_cc1101.setManchester(config.manchester);
  ELECHOUSE_cc1101.setSyncMode(config.syncMode);
  ELECHOUSE_cc1101.setAppendStatus(true);
  ELECHOUSE_cc1101.setPA(TX_POWER_DBM);
  ELECHOUSE_cc1101.SetRx();
}

int clampTxDelayMs(int txDelayMs) {
  if (txDelayMs < CC1101_MIN_TX_DELAY_MS) {
    return CC1101_DEFAULT_TX_DELAY_MS;
  }
  if (txDelayMs > CC1101_MAX_TX_DELAY_MS) {
    return CC1101_MAX_TX_DELAY_MS;
  }
  return txDelayMs;
}

}  // namespace

bool initCc1101Radio() {
  gPacketConfig = Cc1101PacketConfig{};
  gCurrentFrequencyMhz = clampFrequency(gCurrentFrequencyMhz);

#if HAL_HAS_POWER_ENABLE
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
#endif

  // Shared SPI bus lines must keep other devices deselected.
#if HAL_HAS_DISPLAY && defined(HAL_PIN_TFT_CS)
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
#endif
#if HAL_HAS_SD_CARD && defined(HAL_PIN_SD_CS)
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);
#endif

#if HAL_HAS_ANTENNA_SWITCH
  pinMode(CC1101_SW1_PIN, OUTPUT);
  pinMode(CC1101_SW0_PIN, OUTPUT);
#endif
  pinMode(CC1101_SS_PIN, OUTPUT);
  digitalWrite(CC1101_SS_PIN, HIGH);

  delay(CC1101_BOOT_SETTLE_MS);

  sharedspi::init();

  // Reuse a single shared SPI bus so TFT/SD/CC1101 never fight over matrixed pins.
  ELECHOUSE_cc1101.setBeginEndLogic(false);
  SPIClass *spiBus = sharedspi::bus();
  ELECHOUSE_cc1101.setSPIinstance(spiBus);
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

  selectAntennaForFrequency(gCurrentFrequencyMhz);
  ELECHOUSE_cc1101.setMHZ(gCurrentFrequencyMhz);
  applyPacketConfigNoValidate(gPacketConfig);

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

const Cc1101PacketConfig &getCc1101PacketConfig() {
  return gPacketConfig;
}

bool configureCc1101Packet(const Cc1101PacketConfig &config, String &errorOut) {
  if (!gCc1101Ready) {
    errorOut = "CC1101 not initialized";
    return false;
  }

  String validateErr;
  if (!validatePacketConfig(config, validateErr)) {
    errorOut = validateErr;
    return false;
  }

  gPacketConfig = config;
  applyPacketConfigNoValidate(gPacketConfig);
  errorOut = "";
  return true;
}

int readCc1101RssiDbm(String *errorOut) {
  if (!gCc1101Ready) {
    if (errorOut) {
      *errorOut = "CC1101 not initialized";
    }
    return 0;
  }

  ELECHOUSE_cc1101.SetRx();
  delay(3);
  if (errorOut) {
    *errorOut = "";
  }
  return ELECHOUSE_cc1101.getRssi();
}

bool sendCc1101Packet(const uint8_t *data,
                      size_t size,
                      int txDelayMs,
                      String &errorOut) {
  if (!gCc1101Ready) {
    errorOut = "CC1101 not initialized";
    return false;
  }
  if (!data || size == 0) {
    errorOut = "packet is empty";
    return false;
  }
  if (size > CC1101_MAX_PACKET_BYTES) {
    errorOut = "packet max size is 61 bytes";
    return false;
  }

  uint8_t tx[CC1101_MAX_PACKET_BYTES];
  memcpy(tx, data, size);

  ELECHOUSE_cc1101.SetTx();
  ELECHOUSE_cc1101.SendData(tx,
                            static_cast<byte>(size),
                            clampTxDelayMs(txDelayMs));
  ELECHOUSE_cc1101.SetRx();
  errorOut = "";
  return true;
}

bool sendCc1101PacketText(const String &text,
                          int txDelayMs,
                          String &errorOut) {
  if (text.isEmpty()) {
    errorOut = "text is empty";
    return false;
  }
  if (text.length() > CC1101_MAX_PACKET_BYTES) {
    errorOut = "text max length is 61";
    return false;
  }

  uint8_t tx[CC1101_MAX_PACKET_BYTES];
  const size_t len = text.length();
  for (size_t i = 0; i < len; ++i) {
    tx[i] = static_cast<uint8_t>(text[static_cast<unsigned int>(i)]);
  }
  return sendCc1101Packet(tx, len, txDelayMs, errorOut);
}

bool receiveCc1101Packet(std::vector<uint8_t> &outData,
                         int timeoutMs,
                         int *rssiOut,
                         String &errorOut) {
  outData.clear();

  if (!gCc1101Ready) {
    errorOut = "CC1101 not initialized";
    return false;
  }
  if (timeoutMs < 1 || timeoutMs > CC1101_MAX_RX_TIMEOUT_MS) {
    errorOut = "timeout must be 1..60000 ms";
    return false;
  }

  ELECHOUSE_cc1101.SetRx();
  const unsigned long startedAt = millis();
  while (millis() - startedAt < static_cast<unsigned long>(timeoutMs)) {
    if (ELECHOUSE_cc1101.CheckRxFifo(0)) {
      uint8_t rx[CC1101_MAX_PACKET_BYTES] = {0};
      const uint8_t rxLen = ELECHOUSE_cc1101.ReceiveData(rx);
      if (rxLen > 0) {
        outData.assign(rx, rx + rxLen);
        if (rssiOut) {
          *rssiOut = ELECHOUSE_cc1101.getRssi();
        }
        errorOut = "";
        return true;
      }
    }
    delay(CC1101_RX_POLL_MS);
  }

  errorOut = "RX timeout";
  return false;
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

  applyPacketConfigNoValidate(gPacketConfig);
  return true;
}

void appendCc1101Info(JsonObject obj) {
  obj["board"] = HAL_BOARD_NAME;
  obj["cc1101Ready"] = gCc1101Ready;
  obj["cc1101Present"] = gCc1101Ready ? ELECHOUSE_cc1101.getCC1101() : false;
  obj["frequencyMhz"] = gCurrentFrequencyMhz;
  obj["packetModulation"] = gPacketConfig.modulation;
  obj["packetChannel"] = gPacketConfig.channel;
  obj["packetDataRateKbps"] = gPacketConfig.dataRateKbps;
  obj["packetDeviationKHz"] = gPacketConfig.deviationKHz;
  obj["packetRxBandwidthKHz"] = gPacketConfig.rxBandwidthKHz;
  obj["packetSyncMode"] = gPacketConfig.syncMode;
  obj["packetFormat"] = gPacketConfig.packetFormat;
  obj["packetLengthConfig"] = gPacketConfig.lengthConfig;
  obj["packetLength"] = gPacketConfig.packetLength;
}
