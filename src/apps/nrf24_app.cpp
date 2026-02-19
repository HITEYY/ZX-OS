#include "nrf24_app.h"

#include <SPI.h>

#include <cstdlib>
#include <vector>

#include "../core/board_pins.h"
#include "../ui/ui_runtime.h"
#include "user_config.h"

#if __has_include(<RF24.h>)
#include <RF24.h>
#define NRF24_LIB_AVAILABLE 1
#else
#define NRF24_LIB_AVAILABLE 0
#endif

namespace {

#if NRF24_LIB_AVAILABLE
RF24 gNrf24(USER_NRF24_CE_PIN, USER_NRF24_CSN_PIN);
bool gNrf24Inited = false;
bool gNrf24Present = false;
uint8_t gChannel = USER_NRF24_CHANNEL;
uint8_t gDataRate = USER_NRF24_DATA_RATE;
uint8_t gPaLevel = USER_NRF24_PA_LEVEL;
constexpr uint8_t kPipeAddress[6] = {'C', 'C', '2', '4', 'A', 0};
#endif

bool parseIntToken(const String &token, int &out) {
  char *endPtr = nullptr;
  const long value = strtol(token.c_str(), &endPtr, 10);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

String bytesToHex(const uint8_t *data, size_t len) {
  if (!data || len == 0) {
    return "";
  }

  String out;
  char buf[4] = {0};
  for (size_t i = 0; i < len; ++i) {
    if (i > 0) {
      out += ' ';
    }
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    out += buf;
  }
  return out;
}

String dataRateName(uint8_t dataRate) {
  switch (dataRate) {
    case 0:
      return "250kbps";
    case 1:
      return "1Mbps";
    case 2:
      return "2Mbps";
    default:
      return "Unknown";
  }
}

String paLevelName(uint8_t paLevel) {
  switch (paLevel) {
    case 0:
      return "MIN";
    case 1:
      return "LOW";
    case 2:
      return "HIGH";
    case 3:
      return "MAX";
    default:
      return "Unknown";
  }
}

#if NRF24_LIB_AVAILABLE
rf24_datarate_e toNrfDataRate(uint8_t rate) {
  if (rate == 0) {
    return RF24_250KBPS;
  }
  if (rate == 2) {
    return RF24_2MBPS;
  }
  return RF24_1MBPS;
}

rf24_pa_dbm_e toNrfPaLevel(uint8_t level) {
  if (level == 0) {
    return RF24_PA_MIN;
  }
  if (level == 2) {
    return RF24_PA_HIGH;
  }
  if (level == 3) {
    return RF24_PA_MAX;
  }
  return RF24_PA_LOW;
}

void applyNrf24Config() {
  gNrf24.setChannel(gChannel);
  gNrf24.setDataRate(toNrfDataRate(gDataRate));
  gNrf24.setPALevel(toNrfPaLevel(gPaLevel));
  gNrf24.setAutoAck(false);
  gNrf24.setPayloadSize(32);
  gNrf24.openWritingPipe(kPipeAddress);
  gNrf24.openReadingPipe(1, kPipeAddress);
  gNrf24.startListening();
}

bool ensureNrf24Ready(String *errorOut) {
  if (gNrf24Inited) {
    if (gNrf24Present) {
      return true;
    }
    if (errorOut) {
      *errorOut = "nRF24L01 not detected";
    }
    return false;
  }

#if HAL_HAS_DISPLAY
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
#endif
#if HAL_HAS_SD_CARD
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);
#endif
#if HAL_HAS_CC1101
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
#endif

  pinMode(USER_NRF24_CSN_PIN, OUTPUT);
  digitalWrite(USER_NRF24_CSN_PIN, HIGH);
  pinMode(USER_NRF24_CE_PIN, OUTPUT);
  digitalWrite(USER_NRF24_CE_PIN, LOW);

  SPI.begin(11, 10, 9, USER_NRF24_CSN_PIN);
  delay(10);

  gNrf24Present = gNrf24.begin();
  gNrf24Inited = true;
  if (!gNrf24Present) {
    if (errorOut) {
      *errorOut = "nRF24L01 not detected";
    }
    return false;
  }

  applyNrf24Config();
  return true;
}

void showNrf24Info(AppContext &ctx,
                   const std::function<void()> &backgroundTick) {
  std::vector<String> lines;
  lines.push_back("nRF24L01 (SPI)");
  lines.push_back("SCK/MISO/MOSI: 11/10/9");
  lines.push_back("CSN: " + String(USER_NRF24_CSN_PIN));
  lines.push_back("CE: " + String(USER_NRF24_CE_PIN));

  String err;
  if (!ensureNrf24Ready(&err)) {
    lines.push_back("State: Missing");
    lines.push_back(err.isEmpty() ? String("Check wiring/power") : err);
    ctx.uiRuntime->showInfo("NRF24", lines, backgroundTick, "OK/BACK Exit");
    return;
  }

  lines.push_back("State: Ready");
  lines.push_back("Channel: " + String(gChannel));
  lines.push_back("DataRate: " + dataRateName(gDataRate));
  lines.push_back("PA: " + paLevelName(gPaLevel));
  lines.push_back("Pipe: CC24A");
  ctx.uiRuntime->showInfo("NRF24", lines, backgroundTick, "OK/BACK Exit");
}

void configureNrf24(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureNrf24Ready(&err)) {
    ctx.uiRuntime->showToast("NRF24",
                      err.isEmpty() ? String("nRF24 not ready") : err,
                      1700,
                      backgroundTick);
    return;
  }

  String channelIn = String(gChannel);
  if (!ctx.uiRuntime->textInput("Channel (0..125)", channelIn, false, backgroundTick)) {
    return;
  }

  int channel = 0;
  if (!parseIntToken(channelIn, channel) || channel < 0 || channel > 125) {
    ctx.uiRuntime->showToast("NRF24", "Invalid channel", 1200, backgroundTick);
    return;
  }

  std::vector<String> rateMenu;
  rateMenu.push_back("0: 250kbps");
  rateMenu.push_back("1: 1Mbps");
  rateMenu.push_back("2: 2Mbps");
  int rateChoice = gDataRate <= 2 ? gDataRate : 1;
  const int selectedRate = ctx.uiRuntime->menuLoop("NRF24 DataRate",
                                             rateMenu,
                                             rateChoice,
                                             backgroundTick,
                                             "OK Select  BACK Exit",
                                             dataRateName(gDataRate));
  if (selectedRate < 0) {
    return;
  }

  std::vector<String> paMenu;
  paMenu.push_back("0: MIN");
  paMenu.push_back("1: LOW");
  paMenu.push_back("2: HIGH");
  paMenu.push_back("3: MAX");
  int paChoice = gPaLevel <= 3 ? gPaLevel : 1;
  const int selectedPa = ctx.uiRuntime->menuLoop("NRF24 PA",
                                           paMenu,
                                           paChoice,
                                           backgroundTick,
                                           "OK Select  BACK Exit",
                                           paLevelName(gPaLevel));
  if (selectedPa < 0) {
    return;
  }

  gChannel = static_cast<uint8_t>(channel);
  gDataRate = static_cast<uint8_t>(selectedRate);
  gPaLevel = static_cast<uint8_t>(selectedPa);
  applyNrf24Config();

  ctx.uiRuntime->showToast("NRF24", "Config applied", 1200, backgroundTick);
}

void sendNrf24Text(AppContext &ctx,
                   const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureNrf24Ready(&err)) {
    ctx.uiRuntime->showToast("NRF24",
                      err.isEmpty() ? String("nRF24 not ready") : err,
                      1700,
                      backgroundTick);
    return;
  }

  String text;
  if (!ctx.uiRuntime->textInput("TX Text (<=32)", text, false, backgroundTick)) {
    return;
  }
  if (text.isEmpty()) {
    ctx.uiRuntime->showToast("NRF24 TX", "Text is empty", 1100, backgroundTick);
    return;
  }
  if (text.length() > 32) {
    ctx.uiRuntime->showToast("NRF24 TX", "Max 32 bytes", 1200, backgroundTick);
    return;
  }

  uint8_t payload[32] = {0};
  const size_t len = text.length();
  for (size_t i = 0; i < len; ++i) {
    payload[i] = static_cast<uint8_t>(text[static_cast<unsigned int>(i)]);
  }

  gNrf24.stopListening();
  const bool ok = gNrf24.write(payload, len);
  gNrf24.startListening();

  ctx.uiRuntime->showToast("NRF24 TX",
                    ok ? String("Sent") : String("Send failed"),
                    1200,
                    backgroundTick);
}

void receiveNrf24Once(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  String err;
  if (!ensureNrf24Ready(&err)) {
    ctx.uiRuntime->showToast("NRF24",
                      err.isEmpty() ? String("nRF24 not ready") : err,
                      1700,
                      backgroundTick);
    return;
  }

  String timeoutIn = "3000";
  if (!ctx.uiRuntime->textInput("RX Timeout ms", timeoutIn, false, backgroundTick)) {
    return;
  }

  int timeoutMs = 0;
  if (!parseIntToken(timeoutIn, timeoutMs) || timeoutMs < 1 || timeoutMs > 60000) {
    ctx.uiRuntime->showToast("NRF24 RX", "Invalid timeout", 1200, backgroundTick);
    return;
  }

  gNrf24.startListening();
  const unsigned long started = millis();
  while (millis() - started < static_cast<unsigned long>(timeoutMs)) {
    if (gNrf24.available()) {
      uint8_t payload[32] = {0};
      const uint8_t len = 32;
      gNrf24.read(payload, len);

      String text;
      text.reserve(len);
      for (uint8_t i = 0; i < len; ++i) {
        const uint8_t c = payload[i];
        if (c >= 32 && c <= 126) {
          text += static_cast<char>(c);
        } else {
          text += '.';
        }
      }

      std::vector<String> lines;
      lines.push_back("Bytes: " + String(len));
      lines.push_back("ASCII: " + text);
      lines.push_back("HEX: " + bytesToHex(payload, len));
      ctx.uiRuntime->showInfo("NRF24 RX", lines, backgroundTick, "OK/BACK Exit");
      return;
    }

    if (backgroundTick) {
      backgroundTick();
    }
    delay(8);
  }

  ctx.uiRuntime->showToast("NRF24 RX", "Timeout", 1200, backgroundTick);
}
#endif

}  // namespace

void runNrf24App(AppContext &ctx,
                 const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Module Info");
    menu.push_back("Configure");
    menu.push_back("Send Text");
    menu.push_back("Receive Once");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("NRF24",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        "nRF24L01 SPI app");
    if (choice < 0 || choice == 4) {
      return;
    }

    selected = choice;

#if NRF24_LIB_AVAILABLE
    if (choice == 0) {
      showNrf24Info(ctx, backgroundTick);
    } else if (choice == 1) {
      configureNrf24(ctx, backgroundTick);
    } else if (choice == 2) {
      sendNrf24Text(ctx, backgroundTick);
    } else if (choice == 3) {
      receiveNrf24Once(ctx, backgroundTick);
    }
#else
    (void)choice;
    ctx.uiRuntime->showToast("NRF24", "RF24 library missing", 1800, backgroundTick);
    return;
#endif
  }
}
