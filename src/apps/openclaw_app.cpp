#include "openclaw_app.h"

#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <time.h>

#include <algorithm>
#include <vector>

#include "../core/cc1101_radio.h"
#include "../core/audio_recorder.h"
#include "../core/ble_manager.h"
#include "../core/board_pins.h"
#include "../core/gateway_client.h"
#include "../core/runtime_config.h"
#include "../core/shared_spi_bus.h"
#include "../core/wifi_manager.h"
#include "../ui/ui_runtime.h"
#include "user_config.h"

namespace {

constexpr const char *kMessageSenderId = "node-host";
constexpr const char *kDefaultAgentFallback = "default";
constexpr const char *kDefaultSessionAgentId = "main";
constexpr const char *kDefaultSessionKey = "agent:main:main";
constexpr size_t kMessageChunkBytes = 960;
constexpr uint32_t kMaxVoiceBytes = 2097152;
constexpr uint32_t kMaxFileBytes = 4194304;
constexpr size_t kOutboxCapacity = 40;

GatewayInboxMessage gOutbox[kOutboxCapacity];
size_t gOutboxStart = 0;
size_t gOutboxCount = 0;
String gMessengerSessionKey;
String gSubscribedSessionKey;
unsigned long gSubscribedConnectOkMs = 0;

struct ChatEntry {
  GatewayInboxMessage message;
  bool outgoing = false;
};

String boolLabel(bool value) {
  return value ? "Yes" : "No";
}

void markDirty(AppContext &ctx) {
  ctx.configDirty = true;
}

String defaultAgentId() {
  String id = USER_OPENCLAW_DEFAULT_AGENT_ID;
  id.trim();
  if (id.isEmpty()) {
    id = kDefaultAgentFallback;
  }
  return id;
}

String buildMainMessengerSessionKey() {
  return String(kDefaultSessionKey);
}

String activeMessengerSessionKey() {
  if (gMessengerSessionKey.isEmpty()) {
    gMessengerSessionKey = buildMainMessengerSessionKey();
  }
  return gMessengerSessionKey;
}

void pushOutbox(const GatewayInboxMessage &message) {
  if (kOutboxCapacity == 0) {
    return;
  }

  size_t pos = 0;
  if (gOutboxCount < kOutboxCapacity) {
    pos = (gOutboxStart + gOutboxCount) % kOutboxCapacity;
    ++gOutboxCount;
  } else {
    pos = gOutboxStart;
    gOutboxStart = (gOutboxStart + 1) % kOutboxCapacity;
  }
  gOutbox[pos] = message;
}

bool outboxMessage(size_t index, GatewayInboxMessage &out) {
  if (index >= gOutboxCount) {
    return false;
  }
  const size_t pos = (gOutboxStart + index) % kOutboxCapacity;
  out = gOutbox[pos];
  return true;
}

void clearOutbox() {
  gOutboxStart = 0;
  gOutboxCount = 0;
}

void clearMessengerMessages(AppContext &ctx) {
  ctx.gateway->clearInbox();
  clearOutbox();
}

bool sendChatSessionEvent(AppContext &ctx,
                          const char *eventName,
                          const String &sessionKey) {
  if (!eventName || sessionKey.isEmpty()) {
    return false;
  }
  DynamicJsonDocument payload(256);
  payload["sessionKey"] = sessionKey;
  return ctx.gateway->sendNodeEvent(eventName, payload);
}

bool ensureMessengerSessionSubscription(AppContext &ctx,
                                        const std::function<void()> &backgroundTick,
                                        bool showErrorToast = true) {
  const GatewayStatus status = ctx.gateway->status();
  if (!status.gatewayReady) {
    gSubscribedSessionKey = "";
    gSubscribedConnectOkMs = 0;
    return false;
  }

  const String sessionKey = activeMessengerSessionKey();
  const bool alreadySubscribed = gSubscribedSessionKey == sessionKey &&
                                 gSubscribedConnectOkMs == status.lastConnectOkMs;
  if (alreadySubscribed) {
    return true;
  }

  if (!gSubscribedSessionKey.isEmpty()) {
    sendChatSessionEvent(ctx, "chat.unsubscribe", gSubscribedSessionKey);
  }

  if (!sendChatSessionEvent(ctx, "chat.subscribe", sessionKey)) {
    if (showErrorToast) {
      ctx.uiRuntime->showToast("Messenger", "Chat subscribe failed", 1500, backgroundTick);
    }
    return false;
  }

  gSubscribedSessionKey = sessionKey;
  gSubscribedConnectOkMs = status.lastConnectOkMs;
  return true;
}

String trimMiddle(const String &value, size_t maxLength) {
  if (value.length() <= maxLength || maxLength < 6) {
    return value;
  }

  const size_t left = (maxLength - 3) / 2;
  const size_t right = maxLength - 3 - left;
  return value.substring(0, left) + "..." +
         value.substring(value.length() - right);
}

String baseName(const String &path) {
  const int slash = path.lastIndexOf('/');
  if (slash < 0 || slash + 1 >= static_cast<int>(path.length())) {
    return path;
  }
  return path.substring(static_cast<unsigned int>(slash + 1));
}

String detectAudioMime(const String &path) {
  String lower = path;
  lower.toLowerCase();
  if (lower.endsWith(".wav")) {
    return "audio/wav";
  }
  if (lower.endsWith(".mp3")) {
    return "audio/mpeg";
  }
  if (lower.endsWith(".m4a")) {
    return "audio/mp4";
  }
  if (lower.endsWith(".aac")) {
    return "audio/aac";
  }
  if (lower.endsWith(".opus")) {
    return "audio/opus";
  }
  if (lower.endsWith(".ogg")) {
    return "audio/ogg";
  }
  return "application/octet-stream";
}

String detectFileMime(const String &path) {
  String lower = path;
  lower.toLowerCase();

  if (lower.endsWith(".txt") || lower.endsWith(".log")) {
    return "text/plain";
  }
  if (lower.endsWith(".json")) {
    return "application/json";
  }
  if (lower.endsWith(".csv")) {
    return "text/csv";
  }
  if (lower.endsWith(".pdf")) {
    return "application/pdf";
  }
  if (lower.endsWith(".png")) {
    return "image/png";
  }
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) {
    return "image/jpeg";
  }
  if (lower.endsWith(".gif")) {
    return "image/gif";
  }
  if (lower.endsWith(".webp")) {
    return "image/webp";
  }
  if (lower.endsWith(".zip")) {
    return "application/zip";
  }
  if (lower.endsWith(".bin")) {
    return "application/octet-stream";
  }

  const String audioMime = detectAudioMime(path);
  if (audioMime != "application/octet-stream") {
    return audioMime;
  }
  return "application/octet-stream";
}

uint64_t currentUnixMs() {
  const time_t nowSec = time(nullptr);
  if (nowSec <= 0) {
    return 0;
  }
  return static_cast<uint64_t>(nowSec) * 1000ULL;
}

String formatTsShort(uint64_t tsMs) {
  if (tsMs == 0) {
    return "--:--";
  }

  const time_t sec = static_cast<time_t>(tsMs / 1000ULL);
  struct tm info;
  if (localtime_r(&sec, &info)) {
    char out[6];
    snprintf(out, sizeof(out), "%02d:%02d", info.tm_hour, info.tm_min);
    return String(out);
  }

  const uint32_t secInDay = static_cast<uint32_t>((tsMs / 1000ULL) % 86400ULL);
  const uint32_t hour = secInDay / 3600U;
  const uint32_t minute = (secInDay % 3600U) / 60U;
  char out[6];
  snprintf(out, sizeof(out), "%02u:%02u", hour, minute);
  return String(out);
}

String makeMessageId(const char *prefix) {
  static uint32_t seq = 0;
  ++seq;

  String id(prefix ? prefix : "msg");
  id += "-";
  id += String(static_cast<unsigned long>(millis()));
  id += "-";
  id += String(seq);
  return id;
}

String encodeBase64(const uint8_t *data, size_t len) {
  if (!data || len == 0) {
    return "";
  }

  const size_t outCap = ((len + 2) / 3) * 4 + 4;
  std::vector<unsigned char> encoded(outCap, 0);
  size_t outLen = 0;
  const int rc = mbedtls_base64_encode(encoded.data(),
                                       encoded.size(),
                                       &outLen,
                                       data,
                                       len);
  if (rc != 0 || outLen == 0 || outLen >= encoded.size()) {
    return "";
  }

  encoded[outLen] = '\0';
  return String(reinterpret_cast<const char *>(encoded.data()));
}

bool ensureSdMountedForVoice(String *error = nullptr) {
  pinMode(boardpins::kTftCs, OUTPUT);
  digitalWrite(boardpins::kTftCs, HIGH);
  pinMode(boardpins::kCc1101Cs, OUTPUT);
  digitalWrite(boardpins::kCc1101Cs, HIGH);
  pinMode(boardpins::kSdCs, OUTPUT);
  digitalWrite(boardpins::kSdCs, HIGH);

  SPIClass *spiBus = sharedspi::bus();
  const bool mounted = SD.begin(boardpins::kSdCs,
                                *spiBus,
                                25000000,
                                "/sd",
                                8,
                                false);
  if (!mounted && error) {
    *error = "SD mount failed";
  }
  return mounted;
}

bool ensureGatewayReady(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  const GatewayStatus status = ctx.gateway->status();
  if (!status.gatewayReady) {
    gSubscribedSessionKey = "";
    gSubscribedConnectOkMs = 0;
    ctx.uiRuntime->showToast("Messenger", "Gateway is not ready", 1500, backgroundTick);
    return false;
  }
  return true;
}

bool sendTextPayload(AppContext &ctx,
                     const String &rawText,
                     const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return false;
  }

  String text = rawText;
  text.trim();
  if (text.isEmpty()) {
    ctx.uiRuntime->showToast("Messenger", "Message is empty", 1400, backgroundTick);
    return false;
  }

  if (!ensureMessengerSessionSubscription(ctx, backgroundTick)) {
    return false;
  }

  DynamicJsonDocument payload(2048);
  const String messageId = makeMessageId("txt");
  const String target = kDefaultSessionAgentId;
  payload["message"] = text;
  payload["sessionKey"] = activeMessengerSessionKey();
  payload["deliver"] = false;
  const uint64_t ts = currentUnixMs();

  if (!ctx.gateway->sendNodeEvent("agent.request", payload)) {
    ctx.uiRuntime->showToast("Messenger", "Text send failed", 1500, backgroundTick);
    return false;
  }

  GatewayInboxMessage sent;
  sent.id = messageId;
  sent.event = "agent.request";
  sent.type = "text";
  sent.from = kMessageSenderId;
  sent.to = target;
  sent.text = text;
  sent.tsMs = ts;
  pushOutbox(sent);

  ctx.uiRuntime->showToast("Messenger", "Text sent", 1100, backgroundTick);
  return true;
}

void sendTextMessage(AppContext &ctx,
                     const std::function<void()> &backgroundTick) {
  String text;
  if (!ctx.uiRuntime->textInput("Text Message", text, false, backgroundTick)) {
    return;
  }

  sendTextPayload(ctx, text, backgroundTick);
}

bool sendVoiceFileMessage(AppContext &ctx,
                          const String &filePath,
                          const String &caption,
                          const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return false;
  }

  if (filePath.isEmpty()) {
    ctx.uiRuntime->showToast("Voice", "Path is empty", 1300, backgroundTick);
    return false;
  }

  const String target = defaultAgentId();

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    ctx.uiRuntime->showToast("Voice", "Open voice file failed", 1600, backgroundTick);
    return false;
  }

  const uint32_t totalBytes = static_cast<uint32_t>(file.size());
  if (totalBytes == 0) {
    file.close();
    ctx.uiRuntime->showToast("Voice", "Voice file is empty", 1500, backgroundTick);
    return false;
  }
  if (totalBytes > kMaxVoiceBytes) {
    file.close();
    ctx.uiRuntime->showToast("Voice", "File too large (max 2MB)", 1800, backgroundTick);
    return false;
  }

  const uint16_t totalChunks = static_cast<uint16_t>(
      (totalBytes + static_cast<uint32_t>(kMessageChunkBytes) - 1U) /
      static_cast<uint32_t>(kMessageChunkBytes));
  const String messageId = makeMessageId("voice");
  const String mimeType = detectAudioMime(filePath);

  DynamicJsonDocument meta(2048);
  meta["id"] = messageId;
  meta["from"] = kMessageSenderId;
  meta["to"] = target;
  meta["type"] = "voice";
  meta["fileName"] = baseName(filePath);
  meta["contentType"] = mimeType;
  meta["size"] = totalBytes;
  meta["chunks"] = totalChunks;
  if (!caption.isEmpty()) {
    meta["text"] = caption;
  }
  const uint64_t metaTs = currentUnixMs();
  if (metaTs > 0) {
    meta["ts"] = metaTs;
  }

  if (!ctx.gateway->sendNodeEvent("msg.voice.meta", meta)) {
    file.close();
    ctx.uiRuntime->showToast("Voice", "Voice meta send failed", 1700, backgroundTick);
    return false;
  }

  uint8_t raw[kMessageChunkBytes] = {0};
  uint16_t chunkIndex = 0;
  while (file.available() && chunkIndex < totalChunks) {
    const size_t readLen = file.read(raw, sizeof(raw));
    if (readLen == 0) {
      break;
    }

    const String encoded = encodeBase64(raw, readLen);
    if (encoded.isEmpty()) {
      file.close();
      ctx.uiRuntime->showToast("Voice", "Base64 encode failed", 1700, backgroundTick);
      return false;
    }

    DynamicJsonDocument chunk(2048);
    chunk["id"] = messageId;
    chunk["from"] = kMessageSenderId;
    chunk["to"] = target;
    chunk["seq"] = static_cast<uint32_t>(chunkIndex + 1);
    chunk["chunks"] = totalChunks;
    chunk["last"] = (chunkIndex + 1) >= totalChunks;
    chunk["data"] = encoded;
    const uint64_t chunkTs = currentUnixMs();
    if (chunkTs > 0) {
      chunk["ts"] = chunkTs;
    }

    if (!ctx.gateway->sendNodeEvent("msg.voice.chunk", chunk)) {
      file.close();
      ctx.uiRuntime->showToast("Voice", "Voice chunk send failed", 1700, backgroundTick);
      return false;
    }

    ++chunkIndex;
    if (backgroundTick) {
      backgroundTick();
    }
  }
  file.close();

  if (chunkIndex != totalChunks) {
    ctx.uiRuntime->showToast("Voice", "Voice send incomplete", 1700, backgroundTick);
    return false;
  }

  GatewayInboxMessage sent;
  sent.id = messageId;
  sent.event = "msg.voice.meta";
  sent.type = "voice";
  sent.from = kMessageSenderId;
  sent.to = target;
  sent.text = caption;
  sent.fileName = baseName(filePath);
  sent.contentType = mimeType;
  sent.voiceBytes = totalBytes;
  sent.tsMs = metaTs;
  pushOutbox(sent);

  ctx.uiRuntime->showToast("Voice", "Voice sent", 1200, backgroundTick);
  return true;
}

void sendVoiceMessage(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return;
  }

  String filePath = "/voice.wav";
  if (!ctx.uiRuntime->textInput("Voice File Path", filePath, false, backgroundTick)) {
    return;
  }
  filePath.trim();
  if (filePath.isEmpty()) {
    ctx.uiRuntime->showToast("Voice", "Path is empty", 1300, backgroundTick);
    return;
  }
  if (!filePath.startsWith("/")) {
    filePath = "/" + filePath;
  }

  String caption;
  if (!ctx.uiRuntime->textInput("Caption(optional)", caption, false, backgroundTick)) {
    return;
  }
  caption.trim();

  String mountErr;
  if (!ensureSdMountedForVoice(&mountErr)) {
    ctx.uiRuntime->showToast("Voice",
                      mountErr.isEmpty() ? String("SD mount failed") : mountErr,
                      1600,
                      backgroundTick);
    return;
  }

  sendVoiceFileMessage(ctx, filePath, caption, backgroundTick);
}

void recordVoiceFromMic(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return;
  }

  if (!isMicRecordingAvailable()) {
    ctx.uiRuntime->showToast("Voice", "MIC is not configured", 1700, backgroundTick);
    return;
  }

  const uint16_t maxSeconds = static_cast<uint16_t>(
      std::max<uint32_t>(1U, static_cast<uint32_t>(USER_MIC_MAX_SECONDS)));

  String caption;
  if (!ctx.uiRuntime->textInput("Caption(optional)", caption, false, backgroundTick)) {
    return;
  }
  caption.trim();

  String mountErr;
  if (!ensureSdMountedForVoice(&mountErr)) {
    ctx.uiRuntime->showToast("Voice",
                      mountErr.isEmpty() ? String("SD mount failed") : mountErr,
                      1600,
                      backgroundTick);
    return;
  }

  String voicePath = "/voice-";
  voicePath += String(static_cast<unsigned long>(millis()));
  voicePath += ".wav";

  ctx.uiRuntime->showToast("Voice", "Recording... OK/BACK to stop", 900, backgroundTick);

  String recordErr;
  uint32_t bytesWritten = 0;
  const auto stopRequested = [&ctx]() {
    UiEvent ev = ctx.uiRuntime->pollInput();
    return ev.ok || ev.back || ev.okLong || ev.okCount != 0 ||
           ev.backCount != 0 || ev.okLongCount != 0;
  };
  if (!recordMicWavToSd(voicePath,
                        maxSeconds,
                        backgroundTick,
                        stopRequested,
                        &recordErr,
                        &bytesWritten)) {
    ctx.uiRuntime->showToast("Voice",
                      recordErr.isEmpty() ? String("MIC recording failed")
                                          : recordErr,
                      1800,
                      backgroundTick);
    return;
  }

  if (bytesWritten > kMaxVoiceBytes) {
    SD.remove(voicePath.c_str());
    ctx.uiRuntime->showToast("Voice", "Recording too large for send", 1700, backgroundTick);
    return;
  }

  sendVoiceFileMessage(ctx, voicePath, caption, backgroundTick);
}

bool recordVoiceFromBle(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  BleStatus bs = ctx.ble->status();
  if (!bs.connected) {
    return false;
  }

  if (!bs.audioStreamAvailable) {
    String message;
    if (bs.likelyAudio) {
      message = "BLE audio device connected, stream not found";
    } else {
      message = "BLE stream unavailable";
    }
    message += " -> MIC fallback";
    ctx.uiRuntime->showToast("BLE", message, 1800, backgroundTick);
    return false;
  }

  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return true;
  }

  const uint16_t maxSeconds = static_cast<uint16_t>(
      std::max<uint32_t>(1U, static_cast<uint32_t>(USER_MIC_MAX_SECONDS)));

  String caption;
  if (!ctx.uiRuntime->textInput("Caption(optional)", caption, false, backgroundTick)) {
    return true;
  }
  caption.trim();

  String mountErr;
  if (!ensureSdMountedForVoice(&mountErr)) {
    ctx.uiRuntime->showToast("Voice",
                      mountErr.isEmpty() ? String("SD mount failed") : mountErr,
                      1600,
                      backgroundTick);
    return true;
  }

  String voicePath = "/voice-ble-";
  voicePath += String(static_cast<unsigned long>(millis()));
  voicePath += ".wav";

  ctx.uiRuntime->showToast("BLE", "Recording... OK/BACK to stop", 900, backgroundTick);

  String recordErr;
  uint32_t bytesWritten = 0;
  const auto stopRequested = [&ctx]() {
    UiEvent ev = ctx.uiRuntime->pollInput();
    return ev.ok || ev.back || ev.okLong || ev.okCount != 0 ||
           ev.backCount != 0 || ev.okLongCount != 0;
  };
  if (!ctx.ble->recordAudioStreamWavToSd(voicePath,
                                         maxSeconds,
                                         backgroundTick,
                                         stopRequested,
                                         &recordErr,
                                         &bytesWritten)) {
    ctx.uiRuntime->showToast("BLE",
                      recordErr.isEmpty() ? String("BLE recording failed")
                                          : recordErr,
                      1800,
                      backgroundTick);
    return true;
  }

  if (bytesWritten > kMaxVoiceBytes) {
    SD.remove(voicePath.c_str());
    ctx.uiRuntime->showToast("Voice", "Recording too large for send", 1700, backgroundTick);
    return true;
  }

  sendVoiceFileMessage(ctx, voicePath, caption, backgroundTick);
  return true;
}

void recordVoiceMessage(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  // BLE must always have priority for voice source selection.
  if (recordVoiceFromBle(ctx, backgroundTick)) {
    return;
  }
  recordVoiceFromMic(ctx, backgroundTick);
}

void sendFileMessage(AppContext &ctx,
                     const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return;
  }

  String filePath = "/upload.bin";
  if (!ctx.uiRuntime->textInput("File Path", filePath, false, backgroundTick)) {
    return;
  }
  filePath.trim();
  if (filePath.isEmpty()) {
    ctx.uiRuntime->showToast("File", "Path is empty", 1300, backgroundTick);
    return;
  }
  if (!filePath.startsWith("/")) {
    filePath = "/" + filePath;
  }

  const String target = defaultAgentId();
  String caption;
  if (!ctx.uiRuntime->textInput("Message(optional)", caption, false, backgroundTick)) {
    return;
  }
  caption.trim();

  String mountErr;
  if (!ensureSdMountedForVoice(&mountErr)) {
    ctx.uiRuntime->showToast("File",
                      mountErr.isEmpty() ? String("SD mount failed") : mountErr,
                      1600,
                      backgroundTick);
    return;
  }

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    ctx.uiRuntime->showToast("File", "Open file failed", 1600, backgroundTick);
    return;
  }

  const uint32_t totalBytes = static_cast<uint32_t>(file.size());
  if (totalBytes == 0) {
    file.close();
    ctx.uiRuntime->showToast("File", "File is empty", 1500, backgroundTick);
    return;
  }
  if (totalBytes > kMaxFileBytes) {
    file.close();
    ctx.uiRuntime->showToast("File", "File too large (max 4MB)", 1800, backgroundTick);
    return;
  }

  const uint16_t totalChunks = static_cast<uint16_t>(
      (totalBytes + static_cast<uint32_t>(kMessageChunkBytes) - 1U) /
      static_cast<uint32_t>(kMessageChunkBytes));
  const String messageId = makeMessageId("file");
  const String mimeType = detectFileMime(filePath);

  DynamicJsonDocument meta(2048);
  meta["id"] = messageId;
  meta["from"] = kMessageSenderId;
  meta["to"] = target;
  meta["type"] = "file";
  meta["fileName"] = baseName(filePath);
  meta["contentType"] = mimeType;
  meta["size"] = totalBytes;
  meta["chunks"] = totalChunks;
  if (!caption.isEmpty()) {
    meta["text"] = caption;
  }
  const uint64_t metaTs = currentUnixMs();
  if (metaTs > 0) {
    meta["ts"] = metaTs;
  }

  if (!ctx.gateway->sendNodeEvent("msg.file.meta", meta)) {
    file.close();
    ctx.uiRuntime->showToast("File", "File meta send failed", 1700, backgroundTick);
    return;
  }

  uint8_t raw[kMessageChunkBytes] = {0};
  uint16_t chunkIndex = 0;
  while (file.available() && chunkIndex < totalChunks) {
    const size_t readLen = file.read(raw, sizeof(raw));
    if (readLen == 0) {
      break;
    }

    const String encoded = encodeBase64(raw, readLen);
    if (encoded.isEmpty()) {
      file.close();
      ctx.uiRuntime->showToast("File", "Base64 encode failed", 1700, backgroundTick);
      return;
    }

    DynamicJsonDocument chunk(2048);
    chunk["id"] = messageId;
    chunk["from"] = kMessageSenderId;
    chunk["to"] = target;
    chunk["seq"] = static_cast<uint32_t>(chunkIndex + 1);
    chunk["chunks"] = totalChunks;
    chunk["last"] = (chunkIndex + 1) >= totalChunks;
    chunk["data"] = encoded;
    const uint64_t chunkTs = currentUnixMs();
    if (chunkTs > 0) {
      chunk["ts"] = chunkTs;
    }

    if (!ctx.gateway->sendNodeEvent("msg.file.chunk", chunk)) {
      file.close();
      ctx.uiRuntime->showToast("File", "File chunk send failed", 1700, backgroundTick);
      return;
    }

    ++chunkIndex;
    if (backgroundTick) {
      backgroundTick();
    }
  }
  file.close();

  if (chunkIndex != totalChunks) {
    ctx.uiRuntime->showToast("File", "File send incomplete", 1700, backgroundTick);
    return;
  }

  GatewayInboxMessage sent;
  sent.id = messageId;
  sent.event = "msg.file.meta";
  sent.type = "file";
  sent.from = kMessageSenderId;
  sent.to = target;
  sent.text = caption;
  sent.fileName = baseName(filePath);
  sent.contentType = mimeType;
  sent.voiceBytes = totalBytes;
  sent.tsMs = metaTs;
  pushOutbox(sent);

  ctx.uiRuntime->showToast("File", "File sent", 1200, backgroundTick);
}

String makeChatPreview(const ChatEntry &entry) {
  const GatewayInboxMessage &message = entry.message;
  String body;
  const bool isVoice = message.type.startsWith("voice");
  const bool isFile = message.type.startsWith("file");

  if (isVoice) {
    body = "[Voice] ";
    if (!message.fileName.isEmpty()) {
      body += message.fileName;
    } else if (message.voiceBytes > 0) {
      body += String(message.voiceBytes) + " bytes";
    } else {
      body += "attachment";
    }
  } else if (isFile) {
    body = "[File] ";
    if (!message.fileName.isEmpty()) {
      body += message.fileName;
    } else if (message.voiceBytes > 0) {
      body += String(message.voiceBytes) + " bytes";
    } else {
      body += "attachment";
    }
  } else if (!message.text.isEmpty()) {
    body = message.text;
  } else if (!message.fileName.isEmpty()) {
    body = message.fileName;
  } else {
    body = "(no text)";
  }

  String label = entry.outgoing ? "Me " : "Agent ";
  label += body;
  return label;
}

std::vector<ChatEntry> collectChatEntries(AppContext &ctx) {
  std::vector<ChatEntry> entries;
  const size_t inboxCount = ctx.gateway->inboxCount();
  entries.reserve(inboxCount + gOutboxCount);

  for (size_t i = 0; i < inboxCount; ++i) {
    GatewayInboxMessage message;
    if (!ctx.gateway->inboxMessage(i, message)) {
      continue;
    }
    ChatEntry entry;
    entry.message = message;
    entry.outgoing = false;
    entries.push_back(entry);
  }

  for (size_t i = 0; i < gOutboxCount; ++i) {
    GatewayInboxMessage message;
    if (!outboxMessage(i, message)) {
      continue;
    }
    ChatEntry entry;
    entry.message = message;
    entry.outgoing = true;
    entries.push_back(entry);
  }

  std::sort(entries.begin(),
            entries.end(),
            [](const ChatEntry &a, const ChatEntry &b) {
              const uint64_t ta = a.message.tsMs;
              const uint64_t tb = b.message.tsMs;
              if (ta == tb) {
                if (a.outgoing != b.outgoing) {
                  return a.outgoing && !b.outgoing;
                }
                return a.message.id < b.message.id;
              }
              if (ta == 0) {
                return false;
              }
              if (tb == 0) {
                return true;
              }
              return ta < tb;
            });

  return entries;
}

std::vector<String> buildMessengerPreviewLines(const std::vector<ChatEntry> &entries) {
  std::vector<String> lines;
  if (entries.empty()) {
    lines.push_back("(no messages)");
    return lines;
  }

  const int total = static_cast<int>(entries.size());
  const int start = total > 3 ? total - 3 : 0;
  for (int i = start; i < total; ++i) {
    lines.push_back(makeChatPreview(entries[static_cast<size_t>(i)]));
  }
  return lines;
}

void runMessagingMenu(AppContext &ctx,
                      const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    ensureMessengerSessionSubscription(ctx, backgroundTick, false);

    std::vector<ChatEntry> entries = collectChatEntries(ctx);
    std::vector<String> previewLines = buildMessengerPreviewLines(entries);
    const MessengerAction action = ctx.uiRuntime->messengerHomeLoop(previewLines,
                                                                    selected,
                                                                    backgroundTick);

    if (action == MessengerAction::Back) {
      return;
    }

    if (action == MessengerAction::Refresh) {
      continue;
    }

    if (action == MessengerAction::TextLong) {
      selected = 0;
      if (ctx.uiRuntime->confirm("New Session",
                                 "/new command send?",
                                 backgroundTick,
                                 "Send",
                                 "Cancel")) {
        sendTextPayload(ctx, "/new", backgroundTick);
      }
      continue;
    }

    if (action == MessengerAction::Text) {
      selected = 0;
      sendTextMessage(ctx, backgroundTick);
      continue;
    }

    if (action == MessengerAction::Voice) {
      selected = 1;
      recordVoiceMessage(ctx, backgroundTick);
      continue;
    }

    selected = 2;
    sendFileMessage(ctx, backgroundTick);
  }
}

void runGatewayMenu(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  int selected = 0;

  while (true) {
    std::vector<String> menu;
    menu.push_back("Edit URL");
    menu.push_back("Auth Mode");
    menu.push_back("Edit Credential");
    menu.push_back("Clear Gateway");
    menu.push_back("Back");

    String subtitle = "Auth: ";
    subtitle += gatewayAuthModeName(ctx.config.gatewayAuthMode);

    const int choice = ctx.uiRuntime->menuLoop("OpenClaw / Gateway",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);
    if (choice < 0 || choice == 4) {
      return;
    }
    selected = choice;

    if (choice == 0) {
      String url = ctx.config.gatewayUrl;
      if (ctx.uiRuntime->textInput("Gateway URL", url, false, backgroundTick)) {
        ctx.config.gatewayUrl = url;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 1) {
      std::vector<String> authItems;
      authItems.push_back("Token");
      authItems.push_back("Password");

      const int current = ctx.config.gatewayAuthMode == GatewayAuthMode::Password ? 1 : 0;
      const int authChoice = ctx.uiRuntime->menuLoop("Gateway Auth",
                                              authItems,
                                              current,
                                              backgroundTick,
                                              "OK Select  BACK Exit",
                                              "Choose auth mode");
      if (authChoice >= 0) {
        ctx.config.gatewayAuthMode = authChoice == 1
                                         ? GatewayAuthMode::Password
                                         : GatewayAuthMode::Token;
        markDirty(ctx);
      }
      continue;
    }

    if (choice == 2) {
      if (ctx.config.gatewayAuthMode == GatewayAuthMode::Password) {
        String password = ctx.config.gatewayPassword;
        if (ctx.uiRuntime->textInput("Gateway Password", password, true, backgroundTick)) {
          ctx.config.gatewayPassword = password;
          markDirty(ctx);
        }
      } else {
        String token = ctx.config.gatewayToken;
        if (ctx.uiRuntime->textInput("Gateway Token", token, true, backgroundTick)) {
          ctx.config.gatewayToken = token;
          markDirty(ctx);
        }
      }
      continue;
    }

    if (choice == 3) {
      ctx.config.gatewayUrl = "";
      ctx.config.gatewayToken = "";
      ctx.config.gatewayPassword = "";
      ctx.config.gatewayDeviceToken = "";
      markDirty(ctx);
      ctx.uiRuntime->showToast("Gateway", "Gateway config cleared", 1200, backgroundTick);
      continue;
    }
  }
}

void applyRuntimeConfig(AppContext &ctx,
                        const std::function<void()> &backgroundTick) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    ctx.uiRuntime->showToast("Validation", validateErr, 1800, backgroundTick);
    return;
  }

  String saveErr;
  if (!saveConfig(ctx.config, &saveErr)) {
    String message = saveErr.isEmpty() ? String("Failed to save config") : saveErr;
    message += " / previous config kept";
    ctx.uiRuntime->showToast("Save Error", message, 1900, backgroundTick);
    return;
  }

  ctx.configDirty = false;

  ctx.wifi->configure(ctx.config);
  ctx.gateway->configure(ctx.config);
  ctx.ble->configure(ctx.config);

  if (!ctx.config.gatewayUrl.isEmpty() && hasGatewayCredentials(ctx.config)) {
    ctx.gateway->reconnectNow();
  } else {
    ctx.gateway->disconnectNow();
  }

  if (ctx.config.bleDeviceAddress.isEmpty()) {
    ctx.ble->disconnectNow();
  } else if (ctx.config.bleAutoConnect) {
    String bleErr;
    if (!ctx.ble->connectToDevice(ctx.config.bleDeviceAddress,
                                  ctx.config.bleDeviceName,
                                  &bleErr)) {
      ctx.uiRuntime->showToast("BLE", bleErr, 1500, backgroundTick);
    }
  }

  ctx.uiRuntime->showToast("OpenClaw", "Saved and applied", 1400, backgroundTick);
}

std::vector<String> buildStatusLines(AppContext &ctx) {
  std::vector<String> lines;

  GatewayStatus gs = ctx.gateway->status();
  String cfgErr;
  const bool configOk = validateConfig(ctx.config, &cfgErr);

  lines.push_back("Config Valid: " + boolLabel(configOk));
  if (!configOk) {
    lines.push_back("OpenClaw settings required");
    lines.push_back("Config Error: " + cfgErr);
  }
  lines.push_back("Wi-Fi Connected: " + boolLabel(ctx.wifi->isConnected()));
  lines.push_back("Wi-Fi SSID: " + (ctx.wifi->ssid().isEmpty() ? String("(empty)") : ctx.wifi->ssid()));
  lines.push_back("IP: " + (ctx.wifi->ip().isEmpty() ? String("-") : ctx.wifi->ip()));
  lines.push_back("RSSI: " + String(ctx.wifi->rssi()));
  lines.push_back("Gateway URL: " + (ctx.config.gatewayUrl.isEmpty() ? String("(empty)") : ctx.config.gatewayUrl));
  lines.push_back("WS Connected: " + boolLabel(gs.wsConnected));
  lines.push_back("Gateway Ready: " + boolLabel(gs.gatewayReady));
  lines.push_back("Should Connect: " + boolLabel(gs.shouldConnect));
  const size_t receivedCount = ctx.gateway->inboxCount();
  const size_t sentCount = gOutboxCount;
  lines.push_back("Chat Messages: " +
                  String(static_cast<unsigned long>(receivedCount + sentCount)) +
                  " (Rx " + String(static_cast<unsigned long>(receivedCount)) +
                  " / Tx " + String(static_cast<unsigned long>(sentCount)) + ")");
  lines.push_back("Auth Mode: " + String(gatewayAuthModeName(ctx.config.gatewayAuthMode)));
  lines.push_back("Device Token: " + boolLabel(!ctx.config.gatewayDeviceToken.isEmpty()));
  lines.push_back("Device ID: " +
                  (ctx.config.gatewayDeviceId.isEmpty() ? String("(empty)")
                                                        : ctx.config.gatewayDeviceId));
  lines.push_back("CC1101 Ready: " + boolLabel(isCc1101Ready()));
  lines.push_back("CC1101 Freq MHz: " + String(getCc1101FrequencyMhz(), 2));

  BleStatus bs = ctx.ble->status();
  lines.push_back("BLE Connected: " + boolLabel(bs.connected));
  lines.push_back("BLE Device: " +
                  (bs.deviceName.isEmpty() ? String("(none)") : bs.deviceName));
  lines.push_back("BLE Address: " +
                  (bs.deviceAddress.isEmpty() ? String("(none)") : bs.deviceAddress));
  lines.push_back("Speaker Priority: BLE First");
  lines.push_back("BLE Audio-like Device: " + boolLabel(bs.likelyAudio));
  lines.push_back("BLE Audio Stream: " + boolLabel(bs.audioStreamAvailable));
  lines.push_back("BLE Profile: " +
                  (bs.profile.isEmpty() ? String("(unknown)") : bs.profile));
  if (bs.audioStreamAvailable) {
    lines.push_back("BLE Audio Svc: " +
                    (bs.audioServiceUuid.isEmpty() ? String("(auto)") : bs.audioServiceUuid));
    lines.push_back("BLE Audio Char: " +
                    (bs.audioCharUuid.isEmpty() ? String("(auto)") : bs.audioCharUuid));
  }
  if (bs.rssi != 0) {
    lines.push_back("BLE RSSI: " + String(bs.rssi));
  }
  lines.push_back("MIC Recording: " +
                  String(isMicRecordingAvailable() ? "Enabled" : "Disabled"));
  if (isMicRecordingAvailable()) {
    if (USER_MIC_ADC_PIN >= 0) {
      lines.push_back("MIC Source: ADC");
      lines.push_back("MIC Pin: " + String(static_cast<int>(USER_MIC_ADC_PIN)));
    } else if (USER_MIC_PDM_DATA_PIN >= 0 && USER_MIC_PDM_CLK_PIN >= 0) {
      lines.push_back("MIC Source: PDM");
      lines.push_back("MIC Data Pin: " + String(static_cast<int>(USER_MIC_PDM_DATA_PIN)));
      lines.push_back("MIC Clock Pin: " + String(static_cast<int>(USER_MIC_PDM_CLK_PIN)));
    }
    lines.push_back("MIC Sample Rate: " +
                    String(static_cast<unsigned long>(USER_MIC_SAMPLE_RATE)));
  }
  if (!bs.lastError.isEmpty()) {
    lines.push_back("BLE Last Error: " + bs.lastError);
  }

  if (!gs.lastError.isEmpty()) {
    lines.push_back("Last Error: " + gs.lastError);
  }

  return lines;
}

void ensureGatewayAutoConnectOnEnter(AppContext &ctx) {
  String validateErr;
  if (!validateConfig(ctx.config, &validateErr)) {
    return;
  }
  if (ctx.config.gatewayUrl.isEmpty() || !hasGatewayCredentials(ctx.config)) {
    return;
  }

  const GatewayStatus gs = ctx.gateway->status();
  if (gs.gatewayReady || gs.wsConnected || gs.shouldConnect) {
    return;
  }

  ctx.gateway->configure(ctx.config);
  ctx.gateway->connectNow();
}

}  // namespace

void runOpenClawApp(AppContext &ctx,
                    const std::function<void()> &backgroundTick) {
  ensureGatewayAutoConnectOnEnter(ctx);

  int selected = 0;

  while (true) {
    const GatewayStatus gs = ctx.gateway->status();
    String subtitle = "Wi-Fi:";
    subtitle += ctx.wifi->isConnected() ? "UP " : "DOWN ";
    subtitle += "GW:";
    subtitle += gs.gatewayReady ? "READY" : (gs.wsConnected ? "WS" : "IDLE");
    if (ctx.configDirty) {
      subtitle += " *DIRTY";
    }

    std::vector<String> menu;
    menu.push_back("Status");
    menu.push_back("Gateway");
    menu.push_back("Messenger");
    menu.push_back("Save & Apply");
    menu.push_back("Back");

    const int choice = ctx.uiRuntime->menuLoop("OpenClaw",
                                        menu,
                                        selected,
                                        backgroundTick,
                                        "OK Select  BACK Exit",
                                        subtitle);

    if (choice < 0 || choice == 4) {
      return;
    }

    selected = choice;

    if (choice == 0) {
      ctx.uiRuntime->showInfo("OpenClaw Status",
                       buildStatusLines(ctx),
                       backgroundTick,
                       "OK/BACK Exit");
      continue;
    }

    if (choice == 1) {
      runGatewayMenu(ctx, backgroundTick);
      continue;
    }

    if (choice == 2) {
      runMessagingMenu(ctx, backgroundTick);
      continue;
    }

    if (choice == 3) {
      applyRuntimeConfig(ctx, backgroundTick);
      continue;
    }
  }
}
