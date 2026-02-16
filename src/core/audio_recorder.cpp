#include "audio_recorder.h"

#include <SD.h>

#include <algorithm>

#if defined(ARDUINO_ARCH_ESP32)
#include <driver/i2s.h>
#endif

#include "user_config.h"

namespace {

constexpr uint16_t kWavHeaderBytes = 44;

uint32_t sampleRateHz() {
  const uint32_t configured = static_cast<uint32_t>(USER_MIC_SAMPLE_RATE);
  return std::max<uint32_t>(4000U, std::min<uint32_t>(configured, 22050U));
}

void writeLe16(uint8_t *out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFU);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
}

void writeLe32(uint8_t *out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value & 0xFFU);
  out[1] = static_cast<uint8_t>((value >> 8) & 0xFFU);
  out[2] = static_cast<uint8_t>((value >> 16) & 0xFFU);
  out[3] = static_cast<uint8_t>((value >> 24) & 0xFFU);
}

bool writeWavHeader(File &file, uint32_t sampleRate, uint32_t dataBytes) {
  uint8_t header[kWavHeaderBytes] = {0};

  const uint16_t channels = 1;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate = sampleRate * static_cast<uint32_t>(channels) *
                            (static_cast<uint32_t>(bitsPerSample) / 8U);
  const uint16_t blockAlign =
      static_cast<uint16_t>(channels * (bitsPerSample / 8U));
  const uint32_t riffSize = 36U + dataBytes;

  // RIFF chunk
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  writeLe32(header + 4, riffSize);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';

  // fmt subchunk
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  writeLe32(header + 16, 16);  // PCM fmt size
  writeLe16(header + 20, 1);   // PCM format
  writeLe16(header + 22, channels);
  writeLe32(header + 24, sampleRate);
  writeLe32(header + 28, byteRate);
  writeLe16(header + 32, blockAlign);
  writeLe16(header + 34, bitsPerSample);

  // data subchunk
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  writeLe32(header + 40, dataBytes);

  if (!file.seek(0)) {
    return false;
  }
  return file.write(header, sizeof(header)) == sizeof(header);
}

void setError(String *error, const String &value) {
  if (error) {
    *error = value;
  }
}

bool hasAdcMicConfigured() {
  return USER_MIC_ADC_PIN >= 0;
}

bool hasPdmMicConfigured() {
#if defined(USER_MIC_PDM_DATA_PIN) && defined(USER_MIC_PDM_CLK_PIN)
  return USER_MIC_PDM_DATA_PIN >= 0 && USER_MIC_PDM_CLK_PIN >= 0;
#else
  return false;
#endif
}

bool captureAdcSamples(File &file,
                       uint32_t totalSamples,
                       uint32_t sampleRate,
                       const std::function<void()> &backgroundTick,
                       const std::function<bool()> &stopRequested,
                       uint32_t *samplesWritten,
                       String *error) {
  if (samplesWritten) {
    *samplesWritten = 0;
  }

#if defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(12);
#if defined(ADC_11db)
  analogSetPinAttenuation(USER_MIC_ADC_PIN, ADC_11db);
#endif
#endif
  pinMode(USER_MIC_ADC_PIN, INPUT);

  const uint32_t sampleIntervalUs = 1000000UL / sampleRate;
  uint32_t nextSampleUs = micros();
  int32_t dcTrackQ8 = 0;
  const uint16_t tickStride = 192;
  uint32_t writtenSamples = 0;

  for (uint32_t i = 0; i < totalSamples; ++i) {
    if (stopRequested && stopRequested()) {
      break;
    }

    const int raw = analogRead(USER_MIC_ADC_PIN);
    int32_t centered = static_cast<int32_t>(raw) - 2048;
    centered <<= 4;

    const int32_t sampleQ8 = centered << 8;
    dcTrackQ8 += (sampleQ8 - dcTrackQ8) / 64;
    int32_t hp = centered - (dcTrackQ8 >> 8);
    hp = std::max<int32_t>(-32768, std::min<int32_t>(32767, hp));

    const int16_t sample = static_cast<int16_t>(hp);
    if (file.write(reinterpret_cast<const uint8_t *>(&sample), sizeof(sample)) !=
        sizeof(sample)) {
      setError(error, "Failed to write voice sample");
      return false;
    }
    ++writtenSamples;

    if (backgroundTick && ((i % tickStride) == 0U)) {
      backgroundTick();
    }

    nextSampleUs += sampleIntervalUs;
    const int32_t waitUs = static_cast<int32_t>(nextSampleUs - micros());
    if (waitUs > 0) {
      delayMicroseconds(static_cast<uint32_t>(waitUs));
    } else if (waitUs < -2000000) {
      nextSampleUs = micros();
    }
  }

  if (samplesWritten) {
    *samplesWritten = writtenSamples;
  }
  return true;
}

#if defined(ARDUINO_ARCH_ESP32)
bool capturePdmSamples(File &file,
                       uint32_t targetDataBytes,
                       uint32_t sampleRate,
                       const std::function<void()> &backgroundTick,
                       const std::function<bool()> &stopRequested,
                       uint32_t *dataBytesWritten,
                       String *error) {
  if (dataBytesWritten) {
    *dataBytesWritten = 0;
  }

  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
  config.sample_rate = static_cast<int>(sampleRate);
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  config.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_desc_num = 8;
  config.dma_frame_num = 256;

  if (i2s_driver_install(I2S_NUM_0, &config, 0, nullptr) != ESP_OK) {
    setError(error, "MIC I2S init failed");
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.bck_io_num = USER_MIC_PDM_CLK_PIN;
  pins.ws_io_num = USER_MIC_PDM_CLK_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = USER_MIC_PDM_DATA_PIN;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0);
    setError(error, "MIC I2S pin config failed");
    return false;
  }

  i2s_zero_dma_buffer(I2S_NUM_0);

  constexpr size_t kChunkBytes = 1024;
  uint8_t chunk[kChunkBytes];
  uint32_t written = 0;
  uint8_t emptyReads = 0;
  while (written < targetDataBytes) {
    if (stopRequested && stopRequested()) {
      break;
    }

    const size_t toRead = std::min<size_t>(kChunkBytes, targetDataBytes - written);
    size_t readBytes = 0;
    const esp_err_t readErr =
        i2s_read(I2S_NUM_0, chunk, toRead, &readBytes, pdMS_TO_TICKS(80));
    if (readErr != ESP_OK) {
      i2s_driver_uninstall(I2S_NUM_0);
      setError(error, "MIC I2S read failed");
      return false;
    }
    if (readBytes == 0) {
      ++emptyReads;
      if (emptyReads > 20) {
        i2s_driver_uninstall(I2S_NUM_0);
        setError(error, "MIC I2S timeout");
        return false;
      }
      if (backgroundTick) {
        backgroundTick();
      }
      continue;
    }

    emptyReads = 0;
    if (file.write(chunk, readBytes) != readBytes) {
      i2s_driver_uninstall(I2S_NUM_0);
      setError(error, "Failed to write voice sample");
      return false;
    }
    written += static_cast<uint32_t>(readBytes);

    if (backgroundTick) {
      backgroundTick();
    }
  }

  i2s_driver_uninstall(I2S_NUM_0);
  if (dataBytesWritten) {
    *dataBytesWritten = written;
  }
  return true;
}
#endif

}  // namespace

bool isMicRecordingAvailable() {
  if (hasAdcMicConfigured()) {
    return true;
  }
#if defined(ARDUINO_ARCH_ESP32)
  if (hasPdmMicConfigured()) {
    return true;
  }
#endif
  return false;
}

bool recordMicWavToSd(const String &path,
                      uint16_t seconds,
                      const std::function<void()> &backgroundTick,
                      const std::function<bool()> &stopRequested,
                      String *error,
                      uint32_t *bytesWritten) {
  if (!isMicRecordingAvailable()) {
    setError(error, "MIC is not configured");
    return false;
  }

  if (path.isEmpty() || !path.startsWith("/")) {
    setError(error, "Invalid file path");
    return false;
  }

  if (seconds == 0) {
    setError(error, "Recording time must be > 0 sec");
    return false;
  }

  const uint16_t maxSeconds = static_cast<uint16_t>(
      std::max<uint32_t>(1U, static_cast<uint32_t>(USER_MIC_MAX_SECONDS)));
  if (seconds > maxSeconds) {
    setError(error, "Recording time exceeds limit");
    return false;
  }

  const uint32_t sampleRate = sampleRateHz();
  const uint32_t maxSamples = sampleRate * static_cast<uint32_t>(seconds);

  if (SD.exists(path.c_str())) {
    SD.remove(path.c_str());
  }

  File file = SD.open(path.c_str(), FILE_WRITE);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    setError(error, "Failed to create voice file");
    return false;
  }

  uint8_t blankHeader[kWavHeaderBytes] = {0};
  if (file.write(blankHeader, sizeof(blankHeader)) != sizeof(blankHeader)) {
    file.close();
    SD.remove(path.c_str());
    setError(error, "Failed to write WAV header");
    return false;
  }

  bool captured = false;
  uint32_t capturedDataBytes = 0;
  if (hasAdcMicConfigured()) {
    uint32_t capturedSamples = 0;
    captured = captureAdcSamples(file,
                                 maxSamples,
                                 sampleRate,
                                 backgroundTick,
                                 stopRequested,
                                 &capturedSamples,
                                 error);
    capturedDataBytes = capturedSamples * 2U;
  }
#if defined(ARDUINO_ARCH_ESP32)
  else if (hasPdmMicConfigured()) {
    const uint32_t targetDataBytes = maxSamples * 2U;
    captured = capturePdmSamples(file,
                                 targetDataBytes,
                                 sampleRate,
                                 backgroundTick,
                                 stopRequested,
                                 &capturedDataBytes,
                                 error);
  }
#endif

  if (!captured) {
    file.close();
    SD.remove(path.c_str());
    if (error && error->isEmpty()) {
      setError(error, "MIC capture failed");
    }
    return false;
  }

  if (capturedDataBytes == 0) {
    file.close();
    SD.remove(path.c_str());
    setError(error, "No audio captured");
    return false;
  }

  if (!writeWavHeader(file, sampleRate, capturedDataBytes)) {
    file.close();
    SD.remove(path.c_str());
    setError(error, "Failed to finalize WAV header");
    return false;
  }

  file.flush();
  file.close();

  if (bytesWritten) {
    *bytesWritten = capturedDataBytes + kWavHeaderBytes;
  }
  setError(error, "");
  return true;
}
