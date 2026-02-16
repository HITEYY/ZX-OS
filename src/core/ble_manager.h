#pragma once

#include <Arduino.h>

#include <functional>
#include <vector>

#include "runtime_config.h"

class NimBLEScan;
class NimBLEClient;
class NimBLEAdvertisedDevice;
class NimBLERemoteService;
class NimBLERemoteCharacteristic;

struct BleDeviceInfo {
  String name;
  String address;
  int rssi = 0;
  String profile;
  bool isHid = false;
  bool isKeyboard = false;
  bool isLikelyAudio = false;
};

struct BleStatus {
  bool initialized = false;
  bool scanning = false;
  bool connected = false;
  String deviceName;
  String deviceAddress;
  int rssi = 0;
  String profile;
  bool hidDevice = false;
  bool hidKeyboard = false;
  bool likelyAudio = false;
  bool audioStreamAvailable = false;
  String audioServiceUuid;
  String audioCharUuid;
  String keyboardText;
  String pairingHint;
  String lastError;
};

class BleManager {
 public:
  void begin();
  void configure(const RuntimeConfig &config);
  void tick();

  bool scanDevices(std::vector<BleDeviceInfo> &outDevices,
                   String *error = nullptr);
  bool connectToDevice(const String &address,
                       const String &name = "",
                       String *error = nullptr);
  bool recordAudioStreamWavToSd(const String &path,
                                uint16_t seconds,
                                const std::function<void()> &backgroundTick,
                                const std::function<bool()> &stopRequested = std::function<bool()>(),
                                String *error = nullptr,
                                uint32_t *bytesWritten = nullptr);
  void disconnectNow();
  void clearKeyboardInput();
  String keyboardInputText() const;

  bool isConnected() const;
  String lastError() const;
  BleStatus status() const;

 private:
  bool ensureInitialized(String *error = nullptr);
  void setError(const String &message);
  void analyzeConnectedProfile();
  bool subscribeKeyboardInput();
  void handleKeyboardReport(const uint8_t *data, size_t length);
  char translateKeyboardHidCode(uint8_t keyCode, bool shift) const;
  bool containsKeyCode(const uint8_t *arr, size_t len, uint8_t code) const;
  bool detectLikelyAudioByName(const String &name) const;
  String buildProfileLabel(bool hid, bool keyboard, bool likelyAudio) const;
  NimBLERemoteCharacteristic *findAudioStreamCharacteristic(String *serviceUuid = nullptr,
                                                            String *charUuid = nullptr);
  bool isLikelySystemServiceUuid(const String &uuidLower) const;
  bool isLikelyAudioServiceUuid(const String &uuidLower) const;
  void resetAudioStreamState();
  void resetAudioCaptureBuffer();
  void handleAudioPacket(const uint8_t *data, size_t length);
  size_t popAudioData(uint8_t *out, size_t maxLen);
  void resetSessionState();
  bool updateDeviceInfoFromAdvertised(const NimBLEAdvertisedDevice *device,
                                      BleDeviceInfo &info) const;

  RuntimeConfig config_;
  NimBLEScan *scan_ = nullptr;
  NimBLEClient *client_ = nullptr;

  bool initialized_ = false;
  bool scanning_ = false;
  bool connected_ = false;

  String connectedName_;
  String connectedAddress_;
  int connectedRssi_ = 0;
  String connectedProfile_;
  bool connectedIsHid_ = false;
  bool connectedIsKeyboard_ = false;
  bool connectedLikelyAudio_ = false;
  bool connectedHasAudioStream_ = false;
  NimBLERemoteCharacteristic *audioStreamChr_ = nullptr;
  String audioStreamServiceUuid_;
  String audioStreamCharUuid_;
  static constexpr size_t kAudioRingCapacity = 16384;
  uint8_t audioRing_[kAudioRingCapacity] = {0};
  size_t audioRingHead_ = 0;
  size_t audioRingTail_ = 0;
  uint32_t audioReceivedBytes_ = 0;
  uint32_t audioDroppedBytes_ = 0;
  unsigned long audioLastPacketMs_ = 0;
  bool audioCaptureActive_ = false;
  String keyboardInputBuffer_;
  String pairingHint_;
  uint8_t lastKeyboardKeys_[6] = {0, 0, 0, 0, 0, 0};
  String lastError_;
};
