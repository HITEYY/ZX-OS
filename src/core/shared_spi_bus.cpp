#include "shared_spi_bus.h"

#include "board_pins.h"
#include "../hal/board_config.h"

namespace {

constexpr uint8_t kSck = HAL_SPI_SCK;
constexpr uint8_t kMiso = HAL_SPI_MISO;
constexpr uint8_t kMosi = HAL_SPI_MOSI;

bool gInited = false;
SPIClass *gBus = &SPI;

}  // namespace

namespace sharedspi {

void prepareChipSelects() {
#if HAL_HAS_DISPLAY && defined(HAL_PIN_TFT_CS)
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
#endif
#if HAL_HAS_SD_CARD && defined(HAL_PIN_SD_CS)
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);
#endif
#if HAL_HAS_CC1101 && defined(HAL_PIN_CC1101_CS)
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
#endif
}

void init() {
  if (gInited) {
    return;
  }

  prepareChipSelects();
  gBus->begin(kSck, kMiso, kMosi);
  gInited = true;
}

void adoptInitializedBus(SPIClass *externalBus) {
  if (externalBus) {
    gBus = externalBus;
  }
  prepareChipSelects();
  gInited = true;
}

SPIClass *bus() {
  init();
  return gBus;
}

}  // namespace sharedspi
