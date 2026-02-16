#include "ble_manager.h"

#include <NimBLEDevice.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>

#include <algorithm>
#include <string>
#include <cstring>

#if __has_include(<NimBLEExtAdvertising.h>)
#define NIMBLE_V2_PLUS 1
#endif

namespace {

constexpr uint32_t kScanTimeMs = 5000;
constexpr uint32_t kScanTimeSec = 5;
constexpr uint16_t kScanInterval = 100;
constexpr uint16_t kScanWindow = 99;

constexpr uint16_t kAppearanceGenericHid = 0x03C0;
constexpr uint16_t kAppearanceKeyboard = 0x03C1;

constexpr uint16_t kUuidHidService = 0x1812;
constexpr uint16_t kUuidHidBootKeyboardInput = 0x2A22;
constexpr uint16_t kUuidHidReport = 0x2A4D;
constexpr uint16_t kWavHeaderBytes = 44;
constexpr size_t kAudioCaptureDrainBytes = 768;
constexpr unsigned long kAudioPacketTimeoutMs = 3000UL;
constexpr unsigned long kAudioFlushTailMs = 120UL;
constexpr uint32_t kBleAudioMinBytes = 256U;

constexpr const char *kNusServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char *kNusTxCharUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

portMUX_TYPE gBleAudioMux = portMUX_INITIALIZER_UNLOCKED;

bool containsAddress(const std::vector<BleDeviceInfo> &list,
                     const String &address) {
  for (std::vector<BleDeviceInfo>::const_iterator it = list.begin();
       it != list.end();
       ++it) {
    if (it->address.equalsIgnoreCase(address)) {
      return true;
    }
  }
  return false;
}

String safeDeviceName(const String &name, const String &fallbackAddress) {
  if (!name.isEmpty()) {
    return name;
  }
  return fallbackAddress;
}

String normalizeUuidLower(const String &value) {
  String out = value;
  out.trim();
  out.toLowerCase();
  return out;
}

bool extractBootKeyboardReport(const uint8_t *data,
                               size_t length,
                               const uint8_t **reportOut) {
  if (!reportOut || !data || length < 8) {
    return false;
  }

  // Standard boot keyboard report layout is 8 bytes:
  // [modifier][reserved][key1..key6]
  if (length == 8) {
    *reportOut = data;
    return true;
  }

  // Many HID report characteristics prepend a 1-byte report-id.
  if (length == 9) {
    *reportOut = data + 1;
    return true;
  }

  // Fallback: search for a plausible 8-byte window to improve compatibility
  // with devices that include extra metadata around boot payload.
  for (size_t offset = 0; offset + 8 <= length; ++offset) {
    const uint8_t *candidate = data + offset;
    if (candidate[1] != 0) {
      continue;
    }

    bool hasKey = false;
    for (size_t i = 0; i < 6; ++i) {
      if (candidate[2 + i] != 0) {
        hasKey = true;
        break;
      }
    }

    // Accept silent reports only at aligned edges to avoid random false hits.
    if (!hasKey && offset != 0 && offset + 8 != length) {
      continue;
    }

    *reportOut = candidate;
    return true;
  }

  // Last resort: preserve prior behavior and use trailing bytes.
  *reportOut = data + (length - 8);
  return true;
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

bool writeBleWavHeader(File &file, uint32_t sampleRate, uint32_t dataBytes) {
  uint8_t header[kWavHeaderBytes] = {0};

  const uint16_t channels = 1;
  const uint16_t bitsPerSample = 16;
  const uint32_t byteRate = sampleRate * static_cast<uint32_t>(channels) *
                            (static_cast<uint32_t>(bitsPerSample) / 8U);
  const uint16_t blockAlign =
      static_cast<uint16_t>(channels * (bitsPerSample / 8U));
  const uint32_t riffSize = 36U + dataBytes;

  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  writeLe32(header + 4, riffSize);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';

  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  writeLe32(header + 16, 16);
  writeLe16(header + 20, 1);
  writeLe16(header + 22, channels);
  writeLe32(header + 24, sampleRate);
  writeLe32(header + 28, byteRate);
  writeLe16(header + 32, blockAlign);
  writeLe16(header + 34, bitsPerSample);

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

}  // namespace

void BleManager::begin() {
  String error;
  ensureInitialized(&error);
}

void BleManager::configure(const RuntimeConfig &config) {
  const String prevSavedAddress = config_.bleDeviceAddress;
  config_ = config;

  if (connected_ && !prevSavedAddress.equalsIgnoreCase(config_.bleDeviceAddress) &&
      !connectedAddress_.equalsIgnoreCase(config_.bleDeviceAddress)) {
    disconnectNow();
  }
}

void BleManager::tick() {
  if (!client_) {
    return;
  }

  if (!client_->isConnected()) {
    if (connected_) {
      connected_ = false;
      connectedRssi_ = 0;
      resetSessionState();
      if (lastError_.isEmpty()) {
        lastError_ = "BLE device disconnected";
      }
    }
    return;
  }

  connected_ = true;
  connectedRssi_ = client_->getRssi();
}

bool BleManager::scanDevices(std::vector<BleDeviceInfo> &outDevices,
                             String *error) {
  outDevices.clear();

  if (!ensureInitialized(error)) {
    return false;
  }

  if (!scan_) {
    if (error) {
      *error = "BLE scanner is unavailable";
    }
    setError("BLE scanner is unavailable");
    return false;
  }

  scanning_ = true;

  if (scan_->isScanning()) {
    scan_->stop();
  }

#ifdef NIMBLE_V2_PLUS
  NimBLEScanResults results = scan_->getResults(kScanTimeMs, false);
  const int count = results.getCount();

  for (int i = 0; i < count; ++i) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (!device) {
      continue;
    }

    BleDeviceInfo info;
    if (!updateDeviceInfoFromAdvertised(device, info)) {
      continue;
    }

    if (containsAddress(outDevices, info.address)) {
      continue;
    }

    outDevices.push_back(info);
  }
#else
  NimBLEScanResults results = scan_->start(kScanTimeSec, false);
  const int count = results.getCount();

  for (int i = 0; i < count; ++i) {
    NimBLEAdvertisedDevice device = results.getDevice(i);

    BleDeviceInfo info;
    if (!updateDeviceInfoFromAdvertised(&device, info)) {
      continue;
    }

    if (containsAddress(outDevices, info.address)) {
      continue;
    }

    outDevices.push_back(info);
  }
#endif

  std::sort(outDevices.begin(),
            outDevices.end(),
            [](const BleDeviceInfo &a, const BleDeviceInfo &b) {
              if (a.rssi == b.rssi) {
                return a.name < b.name;
              }
              return a.rssi > b.rssi;
            });

  scan_->clearResults();
  scanning_ = false;

  if (outDevices.empty()) {
    setError("No BLE devices found");
  } else {
    setError("");
  }

  return true;
}

bool BleManager::connectToDevice(const String &address,
                                 const String &name,
                                 String *error) {
  if (!ensureInitialized(error)) {
    return false;
  }

  if (address.isEmpty()) {
    if (error) {
      *error = "BLE address is empty";
    }
    setError("BLE address is empty");
    return false;
  }

  if (scan_ && scan_->isScanning()) {
    scan_->stop();
  }

  disconnectNow();

  NimBLEClient *nextClient = NimBLEDevice::createClient();
  if (!nextClient) {
    if (error) {
      *error = "Failed to allocate BLE client";
    }
    setError("Failed to allocate BLE client");
    return false;
  }

  nextClient->setConnectTimeout(5);

  const std::string addressStr(address.c_str());
  const NimBLEAddress publicAddress(addressStr, BLE_ADDR_PUBLIC);
  bool connected = nextClient->connect(publicAddress);

  if (!connected) {
    const NimBLEAddress randomAddress(addressStr, BLE_ADDR_RANDOM);
    connected = nextClient->connect(randomAddress);
  }

  if (!connected) {
    NimBLEDevice::deleteClient(nextClient);
    if (error) {
      *error = "BLE connect failed";
    }
    setError("BLE connect failed");
    return false;
  }

  client_ = nextClient;
  connected_ = true;
  connectedAddress_ = address;
  connectedName_ = name.isEmpty() ? connectedAddress_ : name;
  connectedRssi_ = client_->getRssi();

  analyzeConnectedProfile();

  if (connectedIsKeyboard_) {
    setError("BLE keyboard connected");
  } else if (connectedHasAudioStream_) {
    setError("BLE audio stream ready");
  } else if (connectedLikelyAudio_) {
    pairingHint_ = "BLE audio-like device connected, but stream characteristic not found";
    setError("Connected, but BLE audio stream is unavailable");
  } else if (connectedIsHid_) {
    setError("HID device connected");
  } else {
    setError("");
  }

  return true;
}

bool BleManager::recordAudioStreamWavToSd(const String &path,
                                          uint16_t seconds,
                                          const std::function<void()> &backgroundTick,
                                          const std::function<bool()> &stopRequested,
                                          String *error,
                                          uint32_t *bytesWritten) {
  if (!client_ || !client_->isConnected()) {
    if (error) {
      *error = "BLE device is not connected";
    }
    return false;
  }

  if (path.isEmpty() || !path.startsWith("/")) {
    if (error) {
      *error = "Invalid file path";
    }
    return false;
  }

  if (seconds == 0) {
    if (error) {
      *error = "Recording time must be > 0 sec";
    }
    return false;
  }

  const uint16_t maxSeconds = static_cast<uint16_t>(
      std::max<uint32_t>(1U, static_cast<uint32_t>(USER_MIC_MAX_SECONDS)));
  if (seconds > maxSeconds) {
    if (error) {
      *error = "Recording time exceeds limit";
    }
    return false;
  }

  if (!audioStreamChr_ || !connectedHasAudioStream_) {
    audioStreamChr_ = findAudioStreamCharacteristic(&audioStreamServiceUuid_,
                                                    &audioStreamCharUuid_);
    connectedHasAudioStream_ = audioStreamChr_ != nullptr;
  }
  if (!audioStreamChr_ || !connectedHasAudioStream_) {
    if (error) {
      *error = "BLE audio stream characteristic not found";
    }
    return false;
  }

  if (SD.exists(path.c_str())) {
    SD.remove(path.c_str());
  }

  File file = SD.open(path.c_str(), FILE_WRITE);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (error) {
      *error = "Failed to create BLE voice file";
    }
    return false;
  }

  uint8_t blankHeader[kWavHeaderBytes] = {0};
  if (file.write(blankHeader, sizeof(blankHeader)) != sizeof(blankHeader)) {
    file.close();
    SD.remove(path.c_str());
    if (error) {
      *error = "Failed to write WAV header";
    }
    return false;
  }

  resetAudioCaptureBuffer();
  portENTER_CRITICAL(&gBleAudioMux);
  audioCaptureActive_ = true;
  portEXIT_CRITICAL(&gBleAudioMux);

  const bool useNotify = audioStreamChr_->canNotify();
  const bool subscribed = audioStreamChr_->subscribe(
      useNotify,
      [this](NimBLERemoteCharacteristic *, uint8_t *pData, size_t length, bool) {
        handleAudioPacket(pData, length);
      });
  if (!subscribed) {
    portENTER_CRITICAL(&gBleAudioMux);
    audioCaptureActive_ = false;
    portEXIT_CRITICAL(&gBleAudioMux);
    file.close();
    SD.remove(path.c_str());
    if (error) {
      *error = "Failed to subscribe BLE audio stream";
    }
    return false;
  }

  const uint32_t sampleRate = std::max<uint32_t>(
      4000U,
      std::min<uint32_t>(static_cast<uint32_t>(USER_MIC_SAMPLE_RATE), 22050U));
  const unsigned long startMs = millis();
  const unsigned long endMs = startMs + static_cast<unsigned long>(seconds) * 1000UL;

  uint8_t drain[kAudioCaptureDrainBytes] = {0};
  uint8_t pendingByte = 0;
  bool hasPendingByte = false;
  uint32_t dataBytes = 0;
  bool failed = false;
  String failReason;

  while (millis() < endMs) {
    if (stopRequested && stopRequested()) {
      break;
    }

    if (!client_ || !client_->isConnected()) {
      failed = true;
      failReason = "BLE device disconnected";
      break;
    }

    const size_t readBytes = popAudioData(drain, sizeof(drain));
    if (readBytes > 0) {
      size_t offset = 0;
      if (hasPendingByte) {
        uint8_t pair[2] = {pendingByte, drain[0]};
        if (file.write(pair, sizeof(pair)) != sizeof(pair)) {
          failed = true;
          failReason = "Failed to write BLE audio";
          break;
        }
        dataBytes += 2;
        hasPendingByte = false;
        offset = 1;
      }

      if (!failed && offset < readBytes) {
        const size_t remain = readBytes - offset;
        const size_t evenBytes = remain & ~static_cast<size_t>(1);
        if (evenBytes > 0) {
          if (file.write(drain + offset, evenBytes) != evenBytes) {
            failed = true;
            failReason = "Failed to write BLE audio";
            break;
          }
          dataBytes += static_cast<uint32_t>(evenBytes);
          offset += evenBytes;
        }

        if (offset < readBytes) {
          pendingByte = drain[readBytes - 1];
          hasPendingByte = true;
        }
      }
    } else {
      delay(4);
    }

    if (backgroundTick) {
      backgroundTick();
    }

    const unsigned long now = millis();
    uint32_t receivedBytes = 0;
    unsigned long lastPacketMs = 0;
    portENTER_CRITICAL(&gBleAudioMux);
    receivedBytes = audioReceivedBytes_;
    lastPacketMs = audioLastPacketMs_;
    portEXIT_CRITICAL(&gBleAudioMux);

    if (receivedBytes == 0 && now - startMs >= kAudioPacketTimeoutMs) {
      failed = true;
      failReason = "No BLE audio packets received";
      break;
    }
    if (receivedBytes > 0 && lastPacketMs > 0 &&
        now - lastPacketMs >= kAudioPacketTimeoutMs) {
      failed = true;
      failReason = "BLE audio stream timed out";
      break;
    }
  }

  const unsigned long flushUntil = millis() + kAudioFlushTailMs;
  while (!failed && millis() < flushUntil) {
    const size_t readBytes = popAudioData(drain, sizeof(drain));
    if (readBytes == 0) {
      delay(2);
      if (backgroundTick) {
        backgroundTick();
      }
      continue;
    }

    size_t offset = 0;
    if (hasPendingByte) {
      uint8_t pair[2] = {pendingByte, drain[0]};
      if (file.write(pair, sizeof(pair)) != sizeof(pair)) {
        failed = true;
        failReason = "Failed to write BLE audio";
        break;
      }
      dataBytes += 2;
      hasPendingByte = false;
      offset = 1;
    }

    if (offset < readBytes) {
      const size_t remain = readBytes - offset;
      const size_t evenBytes = remain & ~static_cast<size_t>(1);
      if (evenBytes > 0) {
        if (file.write(drain + offset, evenBytes) != evenBytes) {
          failed = true;
          failReason = "Failed to write BLE audio";
          break;
        }
        dataBytes += static_cast<uint32_t>(evenBytes);
        offset += evenBytes;
      }
      if (offset < readBytes) {
        pendingByte = drain[readBytes - 1];
        hasPendingByte = true;
      }
    }

    if (backgroundTick) {
      backgroundTick();
    }
  }

  portENTER_CRITICAL(&gBleAudioMux);
  audioCaptureActive_ = false;
  portEXIT_CRITICAL(&gBleAudioMux);
  audioStreamChr_->unsubscribe();

  if (!failed && dataBytes < kBleAudioMinBytes) {
    failed = true;
    failReason = "BLE audio data is too small";
  }

  if (!failed && !writeBleWavHeader(file, sampleRate, dataBytes)) {
    failed = true;
    failReason = "Failed to finalize WAV header";
  }

  file.flush();
  file.close();

  if (failed) {
    SD.remove(path.c_str());
    if (error) {
      *error = failReason;
    }
    setError(failReason);
    return false;
  }

  if (bytesWritten) {
    *bytesWritten = dataBytes + kWavHeaderBytes;
  }
  if (error) {
    *error = "";
  }
  uint32_t droppedBytes = 0;
  portENTER_CRITICAL(&gBleAudioMux);
  droppedBytes = audioDroppedBytes_;
  portEXIT_CRITICAL(&gBleAudioMux);
  if (droppedBytes > 0) {
    setError("BLE audio captured with packet drops");
  } else {
    setError("BLE audio captured");
  }
  return true;
}

void BleManager::disconnectNow() {
  if (client_) {
    if (client_->isConnected()) {
      client_->disconnect();
    }
    NimBLEDevice::deleteClient(client_);
    client_ = nullptr;
  }

  connected_ = false;
  connectedRssi_ = 0;
  connectedName_ = "";
  connectedAddress_ = "";
  resetSessionState();
}

void BleManager::clearKeyboardInput() {
  keyboardInputBuffer_.remove(0);
}

String BleManager::keyboardInputText() const {
  return keyboardInputBuffer_;
}

bool BleManager::isConnected() const {
  return connected_;
}

String BleManager::lastError() const {
  return lastError_;
}

BleStatus BleManager::status() const {
  BleStatus state;
  state.initialized = initialized_;
  state.scanning = scanning_;
  state.connected = connected_;
  state.deviceName = connected_ ? connectedName_ : config_.bleDeviceName;
  state.deviceAddress = connected_ ? connectedAddress_ : config_.bleDeviceAddress;
  state.rssi = connectedRssi_;
  state.profile = connectedProfile_;
  state.hidDevice = connectedIsHid_;
  state.hidKeyboard = connectedIsKeyboard_;
  state.likelyAudio = connectedLikelyAudio_;
  state.audioStreamAvailable = connectedHasAudioStream_;
  state.audioServiceUuid = audioStreamServiceUuid_;
  state.audioCharUuid = audioStreamCharUuid_;
  state.keyboardText = keyboardInputBuffer_;
  state.pairingHint = pairingHint_;
  state.lastError = lastError_;
  return state;
}

bool BleManager::ensureInitialized(String *error) {
  if (initialized_) {
    return true;
  }

  NimBLEDevice::init("");
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  NimBLEDevice::setSecurityPasskey(123456);

  scan_ = NimBLEDevice::getScan();
  if (!scan_) {
    if (error) {
      *error = "Failed to initialize BLE scanner";
    }
    setError("Failed to initialize BLE scanner");
    return false;
  }

  scan_->setActiveScan(true);
  scan_->setInterval(kScanInterval);
  scan_->setWindow(kScanWindow);

  initialized_ = true;
  return true;
}

void BleManager::setError(const String &message) {
  lastError_ = message;
}

void BleManager::analyzeConnectedProfile() {
  resetSessionState();

  connectedLikelyAudio_ = detectLikelyAudioByName(connectedName_);
  connectedProfile_ = buildProfileLabel(false, false, connectedLikelyAudio_);

  if (!client_ || !client_->isConnected()) {
    return;
  }

  NimBLERemoteService *hidService = client_->getService(NimBLEUUID(kUuidHidService));
  if (hidService) {
    connectedIsHid_ = true;
    connectedIsKeyboard_ = subscribeKeyboardInput();
    connectedProfile_ = buildProfileLabel(connectedIsHid_,
                                          connectedIsKeyboard_,
                                          connectedLikelyAudio_);

    if (!connectedIsKeyboard_ && pairingHint_.isEmpty()) {
      pairingHint_ = "HID connected but no keyboard input report found";
    }
  }

  audioStreamChr_ = findAudioStreamCharacteristic(&audioStreamServiceUuid_,
                                                  &audioStreamCharUuid_);
  connectedHasAudioStream_ = audioStreamChr_ != nullptr;
  if (connectedHasAudioStream_) {
    connectedProfile_ = "BLE Audio Stream";
    if (pairingHint_.isEmpty()) {
      pairingHint_ = "BLE audio stream characteristic discovered";
    }
  }
}

bool BleManager::subscribeKeyboardInput() {
  if (!client_ || !client_->isConnected()) {
    return false;
  }

  NimBLERemoteService *hidService = client_->getService(NimBLEUUID(kUuidHidService));
  if (!hidService) {
    return false;
  }

  NimBLERemoteCharacteristic *characteristics[2] = {
      hidService->getCharacteristic(NimBLEUUID(kUuidHidBootKeyboardInput)),
      hidService->getCharacteristic(NimBLEUUID(kUuidHidReport)),
  };

  for (size_t i = 0; i < 2; ++i) {
    NimBLERemoteCharacteristic *chr = characteristics[i];
    if (!chr) {
      continue;
    }

    if (!chr->canNotify() && !chr->canIndicate()) {
      continue;
    }

    const bool useNotify = chr->canNotify();
    const bool ok = chr->subscribe(
        useNotify,
        [this](NimBLERemoteCharacteristic *, uint8_t *pData, size_t length, bool) {
          handleKeyboardReport(pData, length);
        });

    if (ok) {
      std::memset(lastKeyboardKeys_, 0, sizeof(lastKeyboardKeys_));
      pairingHint_ = "";
      return true;
    }
  }

  pairingHint_ = "If pairing is requested, enter passkey 123456 on keyboard";
  return false;
}

void BleManager::handleKeyboardReport(const uint8_t *data, size_t length) {
  const uint8_t *report = nullptr;
  if (!extractBootKeyboardReport(data, length, &report) || !report) {
    return;
  }

  const uint8_t modifier = report[0];
  const bool shift = (modifier & 0x22) != 0;

  uint8_t currentKeys[6] = {0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < 6; ++i) {
    currentKeys[i] = report[2 + i];
  }

  for (size_t i = 0; i < 6; ++i) {
    const uint8_t keyCode = currentKeys[i];
    if (keyCode == 0) {
      continue;
    }
    if (containsKeyCode(lastKeyboardKeys_, 6, keyCode)) {
      continue;
    }

    if (keyCode == 42) {  // backspace
      if (keyboardInputBuffer_.length() > 0) {
        keyboardInputBuffer_.remove(keyboardInputBuffer_.length() - 1);
      }
      continue;
    }

    char out = translateKeyboardHidCode(keyCode, shift);
    if (out != 0) {
      keyboardInputBuffer_ += out;
    }
  }

  std::memcpy(lastKeyboardKeys_, currentKeys, sizeof(lastKeyboardKeys_));

  constexpr size_t kMaxKeyboardBuffer = 256;
  if (keyboardInputBuffer_.length() > kMaxKeyboardBuffer) {
    keyboardInputBuffer_.remove(0, keyboardInputBuffer_.length() - kMaxKeyboardBuffer);
  }
}

char BleManager::translateKeyboardHidCode(uint8_t keyCode, bool shift) const {
  if (keyCode >= 4 && keyCode <= 29) {
    char base = static_cast<char>('a' + (keyCode - 4));
    if (shift) {
      base = static_cast<char>(base - ('a' - 'A'));
    }
    return base;
  }

  if (keyCode >= 30 && keyCode <= 39) {
    static const char kNoShiftDigits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
    static const char kShiftDigits[] = {'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'};
    const size_t idx = static_cast<size_t>(keyCode - 30);
    return shift ? kShiftDigits[idx] : kNoShiftDigits[idx];
  }

  switch (keyCode) {
    case 40:
      return '\n';
    case 43:
      return '\t';
    case 44:
      return ' ';
    case 45:
      return shift ? '_' : '-';
    case 46:
      return shift ? '+' : '=';
    case 47:
      return shift ? '{' : '[';
    case 48:
      return shift ? '}' : ']';
    case 49:
      return shift ? '|' : '\\';
    case 51:
      return shift ? ':' : ';';
    case 52:
      return shift ? '"' : '\'';
    case 53:
      return shift ? '~' : '`';
    case 54:
      return shift ? '<' : ',';
    case 55:
      return shift ? '>' : '.';
    case 56:
      return shift ? '?' : '/';
    default:
      return 0;
  }
}

bool BleManager::containsKeyCode(const uint8_t *arr, size_t len, uint8_t code) const {
  if (!arr || len == 0) {
    return false;
  }

  for (size_t i = 0; i < len; ++i) {
    if (arr[i] == code) {
      return true;
    }
  }
  return false;
}

bool BleManager::detectLikelyAudioByName(const String &name) const {
  if (name.isEmpty()) {
    return false;
  }

  String lower = name;
  lower.toLowerCase();

  return lower.indexOf("ear") >= 0 ||
         lower.indexOf("bud") >= 0 ||
         lower.indexOf("headset") >= 0 ||
         lower.indexOf("speaker") >= 0 ||
         lower.indexOf("audio") >= 0 ||
         lower.indexOf("mic") >= 0;
}

String BleManager::buildProfileLabel(bool hid,
                                     bool keyboard,
                                     bool likelyAudio) const {
  if (keyboard) {
    return "HID Keyboard";
  }
  if (hid) {
    return "HID Device";
  }
  if (likelyAudio) {
    return "Audio-like BLE";
  }
  return "Generic BLE";
}

NimBLERemoteCharacteristic *BleManager::findAudioStreamCharacteristic(String *serviceUuid,
                                                                      String *charUuid) {
  if (serviceUuid) {
    *serviceUuid = "";
  }
  if (charUuid) {
    *charUuid = "";
  }

  if (!client_ || !client_->isConnected()) {
    return nullptr;
  }

  String configuredService;
  String configuredChar;
#if defined(USER_BLE_AUDIO_SERVICE_UUID)
  configuredService = normalizeUuidLower(String(USER_BLE_AUDIO_SERVICE_UUID));
#endif
#if defined(USER_BLE_AUDIO_CHAR_UUID)
  configuredChar = normalizeUuidLower(String(USER_BLE_AUDIO_CHAR_UUID));
#endif

  NimBLERemoteCharacteristic *configuredMatch = nullptr;
  String configuredSvcUuid;
  String configuredCharUuid;
  NimBLERemoteCharacteristic *nusMatch = nullptr;
  String nusSvcUuid;
  String nusCharUuid;
  NimBLERemoteCharacteristic *audioLikeMatch = nullptr;
  String audioLikeSvcUuid;
  String audioLikeCharUuid;
  NimBLERemoteCharacteristic *firstNotifyMatch = nullptr;
  String firstNotifySvcUuid;
  String firstNotifyCharUuid;

  const std::vector<NimBLERemoteService *> &services = client_->getServices(true);
  for (std::vector<NimBLERemoteService *>::const_iterator svcIt = services.begin();
       svcIt != services.end();
       ++svcIt) {
    NimBLERemoteService *service = *svcIt;
    if (!service) {
      continue;
    }

    const String svcUuid = normalizeUuidLower(String(service->getUUID().toString().c_str()));
    const std::vector<NimBLERemoteCharacteristic *> &characteristics =
        service->getCharacteristics(true);
    for (std::vector<NimBLERemoteCharacteristic *>::const_iterator chrIt = characteristics.begin();
         chrIt != characteristics.end();
         ++chrIt) {
      NimBLERemoteCharacteristic *chr = *chrIt;
      if (!chr) {
        continue;
      }
      if (!chr->canNotify() && !chr->canIndicate()) {
        continue;
      }

      const String chrUuid = normalizeUuidLower(String(chr->getUUID().toString().c_str()));
      if (!configuredService.isEmpty() || !configuredChar.isEmpty()) {
        const bool serviceMatch =
            configuredService.isEmpty() ||
            svcUuid == configuredService || svcUuid.indexOf(configuredService) >= 0;
        const bool charMatch =
            configuredChar.isEmpty() ||
            chrUuid == configuredChar || chrUuid.indexOf(configuredChar) >= 0;
        if (serviceMatch && charMatch) {
          configuredMatch = chr;
          configuredSvcUuid = svcUuid;
          configuredCharUuid = chrUuid;
          break;
        }
      }

      if (svcUuid.indexOf("1812") >= 0) {
        continue;
      }

      if (!nusMatch &&
          svcUuid.indexOf(kNusServiceUuid) >= 0 &&
          chrUuid.indexOf(kNusTxCharUuid) >= 0) {
        nusMatch = chr;
        nusSvcUuid = svcUuid;
        nusCharUuid = chrUuid;
      }

      const bool isAudioLike = isLikelyAudioServiceUuid(svcUuid) ||
                               isLikelyAudioServiceUuid(chrUuid);
      if (!audioLikeMatch && isAudioLike) {
        audioLikeMatch = chr;
        audioLikeSvcUuid = svcUuid;
        audioLikeCharUuid = chrUuid;
      }

      if (!firstNotifyMatch && !isLikelySystemServiceUuid(svcUuid)) {
        firstNotifyMatch = chr;
        firstNotifySvcUuid = svcUuid;
        firstNotifyCharUuid = chrUuid;
      }
    }

    if (configuredMatch) {
      break;
    }
  }

  if (configuredMatch) {
    if (serviceUuid) {
      *serviceUuid = configuredSvcUuid;
    }
    if (charUuid) {
      *charUuid = configuredCharUuid;
    }
    return configuredMatch;
  }

  if (!configuredService.isEmpty() || !configuredChar.isEmpty()) {
    return nullptr;
  }

  if (nusMatch) {
    if (serviceUuid) {
      *serviceUuid = nusSvcUuid;
    }
    if (charUuid) {
      *charUuid = nusCharUuid;
    }
    return nusMatch;
  }

  if (audioLikeMatch) {
    if (serviceUuid) {
      *serviceUuid = audioLikeSvcUuid;
    }
    if (charUuid) {
      *charUuid = audioLikeCharUuid;
    }
    return audioLikeMatch;
  }

  if (firstNotifyMatch) {
    if (serviceUuid) {
      *serviceUuid = firstNotifySvcUuid;
    }
    if (charUuid) {
      *charUuid = firstNotifyCharUuid;
    }
    return firstNotifyMatch;
  }

  return nullptr;
}

bool BleManager::isLikelySystemServiceUuid(const String &uuidLower) const {
  if (uuidLower.isEmpty()) {
    return true;
  }
  return uuidLower.indexOf("1800") >= 0 ||  // GAP
         uuidLower.indexOf("1801") >= 0 ||  // GATT
         uuidLower.indexOf("180a") >= 0 ||  // Device Info
         uuidLower.indexOf("180f") >= 0 ||  // Battery
         uuidLower.indexOf("1812") >= 0 ||  // HID
         uuidLower.indexOf("1805") >= 0;    // Current Time
}

bool BleManager::isLikelyAudioServiceUuid(const String &uuidLower) const {
  if (uuidLower.isEmpty()) {
    return false;
  }
  return uuidLower.indexOf("1843") >= 0 ||  // Audio Input Control
         uuidLower.indexOf("1844") >= 0 ||  // Volume Control
         uuidLower.indexOf("184d") >= 0 ||  // Microphone Control
         uuidLower.indexOf("184e") >= 0 ||  // Audio Stream Control
         uuidLower.indexOf("184f") >= 0 ||  // Broadcast Audio Scan
         uuidLower.indexOf("1850") >= 0 ||  // Published Audio Capabilities
         uuidLower.indexOf("1851") >= 0 ||  // Basic Audio Announcement
         uuidLower.indexOf("audio") >= 0 ||
         uuidLower.indexOf("6e400001") >= 0 ||
         uuidLower.indexOf("6e400003") >= 0;
}

void BleManager::resetAudioStreamState() {
  audioStreamChr_ = nullptr;
  audioStreamServiceUuid_ = "";
  audioStreamCharUuid_ = "";
  connectedHasAudioStream_ = false;
  resetAudioCaptureBuffer();
}

void BleManager::resetAudioCaptureBuffer() {
  portENTER_CRITICAL(&gBleAudioMux);
  audioRingHead_ = 0;
  audioRingTail_ = 0;
  audioReceivedBytes_ = 0;
  audioDroppedBytes_ = 0;
  audioLastPacketMs_ = 0;
  audioCaptureActive_ = false;
  portEXIT_CRITICAL(&gBleAudioMux);
}

void BleManager::handleAudioPacket(const uint8_t *data, size_t length) {
  if (!data || length == 0) {
    return;
  }

  portENTER_CRITICAL(&gBleAudioMux);
  if (!audioCaptureActive_) {
    portEXIT_CRITICAL(&gBleAudioMux);
    return;
  }

  size_t head = audioRingHead_;
  const size_t tail = audioRingTail_;
  uint32_t written = 0;
  uint32_t dropped = 0;

  for (size_t i = 0; i < length; ++i) {
    const size_t next = (head + 1U) % kAudioRingCapacity;
    if (next == tail) {
      dropped += static_cast<uint32_t>(length - i);
      break;
    }
    audioRing_[head] = data[i];
    head = next;
    ++written;
  }

  audioRingHead_ = head;
  audioReceivedBytes_ += written;
  audioDroppedBytes_ += dropped;
  if (written > 0) {
    audioLastPacketMs_ = millis();
  }
  portEXIT_CRITICAL(&gBleAudioMux);
}

size_t BleManager::popAudioData(uint8_t *out, size_t maxLen) {
  if (!out || maxLen == 0) {
    return 0;
  }

  portENTER_CRITICAL(&gBleAudioMux);
  size_t tail = audioRingTail_;
  const size_t head = audioRingHead_;
  size_t copied = 0;

  while (tail != head && copied < maxLen) {
    out[copied++] = audioRing_[tail];
    tail = (tail + 1U) % kAudioRingCapacity;
  }

  audioRingTail_ = tail;
  portEXIT_CRITICAL(&gBleAudioMux);
  return copied;
}

void BleManager::resetSessionState() {
  connectedProfile_ = "";
  connectedIsHid_ = false;
  connectedIsKeyboard_ = false;
  connectedLikelyAudio_ = false;
  resetAudioStreamState();
  pairingHint_ = "";
  keyboardInputBuffer_ = "";
  keyboardInputBuffer_.reserve(320);
  std::memset(lastKeyboardKeys_, 0, sizeof(lastKeyboardKeys_));
}

bool BleManager::updateDeviceInfoFromAdvertised(const NimBLEAdvertisedDevice *device,
                                                BleDeviceInfo &info) const {
  if (!device) {
    return false;
  }

  const String address = String(device->getAddress().toString().c_str());
  if (address.isEmpty()) {
    return false;
  }

  const String name = safeDeviceName(String(device->getName().c_str()), address);
  const bool hasHidService = device->isAdvertisingService(NimBLEUUID(kUuidHidService));

  bool appearsHid = false;
  bool appearsKeyboard = false;
  if (device->haveAppearance()) {
    const uint16_t appearance = device->getAppearance();
    appearsKeyboard = appearance == kAppearanceKeyboard;
    appearsHid = (appearance >= kAppearanceGenericHid &&
                  appearance < (kAppearanceGenericHid + 16));
  }

  String lowerName = name;
  lowerName.toLowerCase();
  const bool nameKeyboard = lowerName.indexOf("kbd") >= 0 ||
                            lowerName.indexOf("keyboard") >= 0;

  const bool isKeyboard = appearsKeyboard || (hasHidService && nameKeyboard);
  const bool isHid = hasHidService || appearsHid || isKeyboard;
  const bool isLikelyAudio = detectLikelyAudioByName(name);

  info.name = name;
  info.address = address;
  info.rssi = device->getRSSI();
  info.isHid = isHid;
  info.isKeyboard = isKeyboard;
  info.isLikelyAudio = isLikelyAudio;
  info.profile = buildProfileLabel(isHid, isKeyboard, isLikelyAudio);
  return true;
}
