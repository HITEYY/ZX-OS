#include "openclaw_app.h"

#include <SD.h>
#include <SHA256.h>
#include <SPI.h>
#include <WiFi.h>
#include <limits.h>
#include <time.h>

#include <algorithm>
#include <vector>

extern "C" {
#include <libb64/cencode.h>
}

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
constexpr size_t kMessageChunkBytes = 256;
constexpr size_t kBase64ChunkBufferBytes =
    ((kMessageChunkBytes + 2U) / 3U) * 4U + 1U;
constexpr size_t kAgentAttachmentChunkBytes = 3840;
constexpr size_t kAgentAttachmentBase64ChunkBytes =
    ((kAgentAttachmentChunkBytes + 2U) / 3U) * 4U + 1U;
constexpr uint32_t kMessengerBinaryAttachMaxBytes =
    static_cast<uint32_t>(USER_MESSENGER_BINARY_ATTACH_MAX_BYTES);
constexpr size_t kMessengerTextFallbackPreviewMaxChars =
    static_cast<size_t>(USER_MESSENGER_TEXT_FALLBACK_PREVIEW_MAX_CHARS);
constexpr uint16_t kAgentAttachmentMaxChunks =
    static_cast<uint16_t>((kMessengerBinaryAttachMaxBytes +
                           static_cast<uint32_t>(kAgentAttachmentChunkBytes) - 1U) /
                          static_cast<uint32_t>(kAgentAttachmentChunkBytes));
constexpr uint32_t kAgentAttachmentMaxBytes = kMessengerBinaryAttachMaxBytes;
constexpr bool kLegacyMediaFallbackEnabled =
    USER_MESSENGER_ENABLE_LEGACY_MEDIA_FALLBACK != 0;
constexpr size_t kAgentRequestMessageMaxChars = 19000U;
constexpr uint32_t kMaxVoiceBytes = 2097152;
constexpr uint32_t kMaxFileBytes = 4194304;
constexpr uint32_t kChatSendAttachmentMaxBytes = 98304;
constexpr uint8_t kChunkSendMaxRetries = 3;
constexpr unsigned long kChunkRetryWaitMs = 2500UL;
constexpr size_t kOutboxCapacity = 40;
constexpr size_t kOutboxMaxIdLen = 96;
constexpr size_t kOutboxMaxMetaLen = 64;
constexpr size_t kOutboxMaxTextLen = 768;
constexpr size_t kOutboxMaxFileNameLen = 128;

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

struct SdSelectEntry {
  String fullPath;
  String label;
  bool isDirectory = false;
  uint64_t size = 0;
};

enum class AttachmentKind : uint8_t {
  File = 0,
  Voice = 1,
};

enum class AttachmentRoute : uint8_t {
  ApiAttachment = 0,
  Framed = 1,
  TextFallback = 2,
  LegacyMetaChunk = 3,
  Failed = 4,
};

struct AttachmentSendResult {
  bool ok = false;
  AttachmentRoute route = AttachmentRoute::Failed;
  String error;
  String messageId;
  String eventName;
  String mimeType;
  String fileName;
  uint32_t totalBytes = 0;
};

class ScopedOkBackBlock {
 public:
  explicit ScopedOkBackBlock(UiRuntime *ui) : ui_(ui) {
    if (ui_) {
      ui_->setOkBackBlocked(true);
    }
  }

  ~ScopedOkBackBlock() {
    if (ui_) {
      ui_->setOkBackBlocked(false);
    }
  }

 private:
  UiRuntime *ui_ = nullptr;
};

class ScopedProgressOverlay {
 public:
  ScopedProgressOverlay(UiRuntime *ui, const String &title, const String &message)
      : ui_(ui), title_(title) {
    if (ui_) {
      ui_->showProgressOverlay(title_, message, 0);
    }
  }

  ~ScopedProgressOverlay() {
    if (ui_) {
      ui_->hideProgressOverlay();
    }
  }

  void update(const String &message, int percent = -1) {
    if (ui_) {
      ui_->showProgressOverlay(title_, message, percent);
    }
  }

 private:
  UiRuntime *ui_ = nullptr;
  String title_;
};

bool sendTextPayload(AppContext &ctx,
                     const String &rawText,
                     const std::function<void()> &backgroundTick);

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

const char *attachmentKindToken(AttachmentKind kind) {
  return kind == AttachmentKind::Voice ? "voice" : "file";
}

String attachmentUiTitle(AttachmentKind kind) {
  return kind == AttachmentKind::Voice ? "Voice" : "File";
}

String attachmentRouteToast(AttachmentRoute route) {
  if (route == AttachmentRoute::ApiAttachment) {
    return "Sent (chat.send attachment)";
  }
  if (route == AttachmentRoute::Framed) {
    return "Sent (framed)";
  }
  if (route == AttachmentRoute::TextFallback) {
    return "Sent (text fallback)";
  }
  if (route == AttachmentRoute::LegacyMetaChunk) {
    return "Sent (legacy fallback)";
  }
  return "Send failed";
}

bool isImageMimeType(const String &mimeType) {
  return mimeType.startsWith("image/");
}

bool isTextPreviewMimeType(const String &mimeType) {
  return mimeType == "text/plain" || mimeType == "application/json" ||
         mimeType == "text/csv";
}

String withGatewayErrorSuffix(const String &base, GatewayClient *gateway) {
  if (!gateway) {
    return base;
  }
  String err = gateway->lastError();
  err.trim();
  if (err.isEmpty()) {
    return base;
  }
  String merged = base;
  merged += ": ";
  merged += err;
  if (merged.length() > 84) {
    merged = merged.substring(0, 81) + "...";
  }
  return merged;
}

bool sendGatewayEventWithRetry(AppContext &ctx,
                               const char *eventName,
                               JsonDocument &payload,
                               const std::function<void()> &backgroundTick,
                               uint8_t maxRetries = kChunkSendMaxRetries) {
  if (maxRetries == 0) {
    maxRetries = 1;
  }

  for (uint8_t attempt = 0; attempt < maxRetries; ++attempt) {
    if (ctx.gateway->sendNodeEvent(eventName, payload)) {
      return true;
    }

    if (attempt + 1 >= maxRetries) {
      break;
    }

    ctx.gateway->connectNow();

    const unsigned long startMs = millis();
    while (millis() - startMs < kChunkRetryWaitMs) {
      if (backgroundTick) {
        backgroundTick();
      }
      const GatewayStatus now = ctx.gateway->status();
      if (now.gatewayReady && now.wsConnected) {
        break;
      }
      delay(25);
    }
  }

  return false;
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

void clampString(String &value, size_t maxLen) {
  if (maxLen == 0) {
    value = "";
    return;
  }
  if (value.length() <= maxLen) {
    return;
  }
  if (maxLen <= 3) {
    value = value.substring(0, maxLen);
    return;
  }
  value = value.substring(0, maxLen - 3) + "...";
}

void clampOutboxMessage(GatewayInboxMessage &message) {
  clampString(message.id, kOutboxMaxIdLen);
  clampString(message.event, kOutboxMaxMetaLen);
  clampString(message.type, kOutboxMaxMetaLen);
  clampString(message.from, kOutboxMaxMetaLen);
  clampString(message.to, kOutboxMaxMetaLen);
  clampString(message.text, kOutboxMaxTextLen);
  clampString(message.fileName, kOutboxMaxFileNameLen);
  clampString(message.contentType, kOutboxMaxMetaLen);
}

void pushOutbox(const GatewayInboxMessage &message) {
  if (kOutboxCapacity == 0) {
    return;
  }

  GatewayInboxMessage bounded = message;
  clampOutboxMessage(bounded);

  size_t pos = 0;
  if (gOutboxCount < kOutboxCapacity) {
    pos = (gOutboxStart + gOutboxCount) % kOutboxCapacity;
    ++gOutboxCount;
  } else {
    pos = gOutboxStart;
    gOutboxStart = (gOutboxStart + 1) % kOutboxCapacity;
  }
  gOutbox[pos] = bounded;
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

String parentPath(const String &path) {
  if (path.isEmpty() || path == "/") {
    return "/";
  }
  const int slash = path.lastIndexOf('/');
  if (slash <= 0) {
    return "/";
  }
  return path.substring(0, static_cast<unsigned int>(slash));
}

String buildChildPath(const String &dirPath, const String &name) {
  if (name.startsWith("/")) {
    return name;
  }
  if (dirPath == "/") {
    return "/" + name;
  }
  return dirPath + "/" + name;
}

String formatBytes(uint64_t bytes) {
  static const char *kUnits[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }

  char buf[32];
  if (unit == 0) {
    snprintf(buf,
             sizeof(buf),
             "%llu %s",
             static_cast<unsigned long long>(bytes),
             kUnits[unit]);
  } else {
    snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
  }
  return String(buf);
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

bool encodeBase64(const uint8_t *data,
                  size_t len,
                  char *out,
                  size_t outCap,
                  size_t *outLen = nullptr) {
  if (!data || len == 0 || !out || outCap < 2U) {
    return false;
  }

  const size_t expected = base64_encode_expected_len(len);
  if (expected == 0 || expected + 1U > outCap || len > static_cast<size_t>(INT_MAX)) {
    out[0] = '\0';
    return false;
  }

  const int written = base64_encode_chars(reinterpret_cast<const char *>(data),
                                          static_cast<int>(len),
                                          out);
  if (written <= 0) {
    out[0] = '\0';
    return false;
  }

  const size_t produced = static_cast<size_t>(written);
  if (produced != expected || produced >= outCap) {
    out[0] = '\0';
    return false;
  }

  out[produced] = '\0';
  if (outLen) {
    *outLen = produced;
  }
  return true;
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

bool listSdDirectory(const String &path,
                     std::vector<SdSelectEntry> &outEntries,
                     String *error = nullptr) {
  outEntries.clear();

  File dir = SD.open(path.c_str(), FILE_READ);
  if (!dir || !dir.isDirectory()) {
    if (error) {
      *error = "Directory open failed";
    }
    if (dir) {
      dir.close();
    }
    return false;
  }

  File entry = dir.openNextFile();
  while (entry) {
    const String rawName = String(entry.name());
    if (!rawName.isEmpty()) {
      SdSelectEntry item;
      item.fullPath = buildChildPath(path, rawName);
      item.isDirectory = entry.isDirectory();
      item.size = entry.size();
      item.label = item.isDirectory ? "[D] " : "[F] ";
      item.label += baseName(item.fullPath);
      if (!item.isDirectory) {
        item.label += " (" + formatBytes(item.size) + ")";
      }
      outEntries.push_back(item);
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  std::sort(outEntries.begin(),
            outEntries.end(),
            [](const SdSelectEntry &a, const SdSelectEntry &b) {
              if (a.isDirectory != b.isDirectory) {
                return a.isDirectory;
              }
              String lhs = a.fullPath;
              String rhs = b.fullPath;
              lhs.toLowerCase();
              rhs.toLowerCase();
              return lhs < rhs;
            });
  return true;
}

bool selectSdFile(AppContext &ctx,
                  const String &title,
                  const std::function<bool(const String &)> &acceptFile,
                  String *selectedPath,
                  const std::function<void()> &backgroundTick) {
  if (!selectedPath) {
    return false;
  }

  String currentPath = "/";
  int selected = 0;

  while (true) {
    std::vector<SdSelectEntry> entries;
    String err;
    if (!listSdDirectory(currentPath, entries, &err)) {
      ctx.uiRuntime->showToast("File",
                               err.isEmpty() ? String("Read failed") : err,
                               1700,
                               backgroundTick);
      return false;
    }

    std::vector<String> menu;
    if (currentPath != "/") {
      menu.push_back(".. (Up)");
    }
    for (std::vector<SdSelectEntry>::const_iterator it = entries.begin();
         it != entries.end();
         ++it) {
      menu.push_back(it->label);
    }
    menu.push_back("Refresh");
    menu.push_back("Cancel");

    const String subtitle = "Path: " + trimMiddle(currentPath, 23);
    const int choice = ctx.uiRuntime->menuLoop(title,
                                               menu,
                                               selected,
                                               backgroundTick,
                                               "OK Select  BACK Cancel",
                                               subtitle);
    if (choice < 0) {
      return false;
    }

    selected = choice;
    int idx = choice;
    if (currentPath != "/") {
      if (idx == 0) {
        currentPath = parentPath(currentPath);
        selected = 0;
        continue;
      }
      idx -= 1;
    }

    const int entryCount = static_cast<int>(entries.size());
    if (idx == entryCount) {
      continue;
    }
    if (idx == entryCount + 1) {
      return false;
    }
    if (idx < 0 || idx >= entryCount) {
      continue;
    }

    const SdSelectEntry selectedEntry = entries[static_cast<size_t>(idx)];
    if (selectedEntry.isDirectory) {
      currentPath = selectedEntry.fullPath;
      selected = 0;
      continue;
    }

    if (acceptFile && !acceptFile(selectedEntry.fullPath)) {
      ctx.uiRuntime->showToast("File", "This file type is not allowed", 1500, backgroundTick);
      continue;
    }

    *selectedPath = selectedEntry.fullPath;
    return true;
  }
}

bool askVoiceRecordSeconds(AppContext &ctx,
                           const std::function<void()> &backgroundTick,
                           uint16_t *secondsOut) {
  if (!secondsOut) {
    return false;
  }

  const int maxSeconds = static_cast<int>(
      std::max<uint32_t>(1U, static_cast<uint32_t>(USER_MIC_MAX_SECONDS)));
  int seconds = static_cast<int>(
      std::max<uint32_t>(1U, static_cast<uint32_t>(USER_MIC_DEFAULT_SECONDS)));
  if (seconds > maxSeconds) {
    seconds = maxSeconds;
  }

  if (!ctx.uiRuntime->numberWheelInput("Record Seconds",
                                       1,
                                       maxSeconds,
                                       1,
                                       seconds,
                                       backgroundTick,
                                       "s")) {
    return false;
  }

  *secondsOut = static_cast<uint16_t>(seconds);
  return true;
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

bool sendMainSessionResetGreeting(AppContext &ctx,
                                  const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return false;
  }

  const String sessionKey = buildMainMessengerSessionKey();
  const String previousSubscribedSessionKey = gSubscribedSessionKey;

  if (!previousSubscribedSessionKey.isEmpty()) {
    sendChatSessionEvent(ctx, "chat.unsubscribe", previousSubscribedSessionKey);
  }
  // Force default main session subscribe to be re-established.
  if (previousSubscribedSessionKey != sessionKey) {
    sendChatSessionEvent(ctx, "chat.unsubscribe", sessionKey);
  }

  gMessengerSessionKey = sessionKey;
  gSubscribedSessionKey = "";
  gSubscribedConnectOkMs = 0;

  if (!ensureMessengerSessionSubscription(ctx, backgroundTick)) {
    return false;
  }

  DynamicJsonDocument payload(2048);
  payload["message"] = "/new";
  payload["sessionKey"] = sessionKey;
  payload["deliver"] = false;

  if (!ctx.gateway->sendNodeEvent("agent.request", payload)) {
    ctx.uiRuntime->showToast("Messenger", "Text send failed", 1500, backgroundTick);
    return false;
  }

  clearMessengerMessages(ctx);
  ctx.uiRuntime->showToast("Messenger", "New session started", 1100, backgroundTick);
  return true;
}

void startMainSessionWithGreeting(AppContext &ctx,
                                  const std::function<void()> &backgroundTick) {
  sendMainSessionResetGreeting(ctx, backgroundTick);
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

bool sendAgentRequestMessage(AppContext &ctx,
                             const String &sessionKey,
                             const String &target,
                             const String &message,
                             const std::function<void()> &backgroundTick,
                             String *errorOut = nullptr) {
  if (message.isEmpty()) {
    if (errorOut) {
      *errorOut = "Message is empty";
    }
    return false;
  }
  if (message.length() > kAgentRequestMessageMaxChars) {
    if (errorOut) {
      *errorOut = "Attachment frame too large";
    }
    return false;
  }

  size_t payloadCap = message.length() + 640U;
  if (payloadCap < 1024U) {
    payloadCap = 1024U;
  }

  DynamicJsonDocument payload(payloadCap);
  payload["message"] = message;
  payload["sessionKey"] = sessionKey;
  if (!target.isEmpty()) {
    payload["to"] = target;
  }
  payload["deliver"] = false;
  payload["thinking"] = "low";
  if (!sendGatewayEventWithRetry(ctx, "agent.request", payload, backgroundTick)) {
    if (errorOut) {
      *errorOut = withGatewayErrorSuffix("Agent request send failed", ctx.gateway);
    }
    return false;
  }
  return true;
}

String hexDigest(const uint8_t *digest, size_t len) {
  static const char kHex[] = "0123456789abcdef";
  if (!digest || len == 0) {
    return "";
  }
  String out;
  out.reserve(len * 2U);
  for (size_t i = 0; i < len; ++i) {
    const uint8_t value = digest[i];
    out += kHex[(value >> 4) & 0x0F];
    out += kHex[value & 0x0F];
  }
  return out;
}

String computeFileSha256Hex(const String &filePath, String *errorOut = nullptr) {
  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (errorOut) {
      *errorOut = "Checksum read failed";
    }
    return "";
  }

  SHA256 hash;
  uint8_t digest[SHA256::HASH_SIZE] = {0};
  uint8_t buffer[1024] = {0};
  bool failed = false;

  hash.reset();
  while (file.available()) {
    const size_t readLen = file.read(buffer, sizeof(buffer));
    if (readLen == 0) {
      failed = true;
      break;
    }
    hash.update(buffer, readLen);
  }
  file.close();

  if (failed) {
    if (errorOut) {
      *errorOut = "Checksum read failed";
    }
    return "";
  }

  hash.finalize(digest, sizeof(digest));
  const String hex = hexDigest(digest, sizeof(digest));
  if (hex.isEmpty() && errorOut) {
    *errorOut = "Checksum encode failed";
  }
  return hex;
}

String readTextFilePreview(const String &filePath,
                           size_t maxChars,
                           bool *truncatedOut = nullptr,
                           String *errorOut = nullptr) {
  if (truncatedOut) {
    *truncatedOut = false;
  }
  if (maxChars == 0) {
    return "";
  }

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    if (errorOut) {
      *errorOut = "Preview read failed";
    }
    return "";
  }

  String preview;
  preview.reserve(std::min<size_t>(maxChars, 512U));
  size_t count = 0;

  while (file.available() && count < maxChars) {
    const int next = file.read();
    if (next < 0) {
      break;
    }
    if (next == '\r') {
      continue;
    }
    char append = ' ';
    if (next == '\n' || next == '\t') {
      append = static_cast<char>(next);
    } else if (next >= 32 || next >= 0x80) {
      append = static_cast<char>(next);
    }
    preview += append;
    ++count;
  }

  if (truncatedOut) {
    *truncatedOut = file.available();
  }
  file.close();
  return preview;
}

String buildAttachmentFallbackMessage(AttachmentKind kind,
                                      const String &messageId,
                                      const String &filePath,
                                      const String &fileName,
                                      const String &mimeType,
                                      uint32_t totalBytes,
                                      const String &caption,
                                      const String &reason,
                                      const String &preview,
                                      bool previewTruncated) {
  String message;
  message.reserve(caption.length() + preview.length() + 640U);
  message += "[ATTACHMENT_TEXT_FALLBACK]\n";
  message += "id:";
  message += messageId;
  message += "\nkind:";
  message += attachmentKindToken(kind);
  message += "\nname:";
  message += fileName;
  message += "\npath:";
  message += filePath;
  message += "\nmime:";
  message += mimeType;
  message += "\nsize:";
  message += String(static_cast<unsigned long>(totalBytes));
  message += "\nts:";
  message += String(static_cast<unsigned long long>(currentUnixMs()));
  if (!caption.isEmpty()) {
    message += "\ncaption:";
    message += caption;
  }
  if (!reason.isEmpty()) {
    message += "\nreason:";
    message += reason;
  }
  message += "\nnote:Node role uses agent.request relay. Binary attachments are limited.";
  if (!preview.isEmpty()) {
    message += "\npreviewTruncated:";
    message += previewTruncated ? "true" : "false";
    message += "\npreview:\n";
    message += preview;
  }
  return message;
}

AttachmentSendResult sendAttachmentTextFallback(
    AppContext &ctx,
    AttachmentKind kind,
    const String &filePath,
    const String &mimeType,
    const String &target,
    const String &caption,
    uint32_t totalBytes,
    const String &reason,
    const std::function<void()> &backgroundTick) {
  AttachmentSendResult result;
  result.ok = false;
  result.route = AttachmentRoute::Failed;
  result.eventName = "agent.request";
  result.mimeType = mimeType;
  result.fileName = baseName(filePath);
  result.totalBytes = totalBytes;
  result.messageId = makeMessageId(attachmentKindToken(kind));

  if (!ensureMessengerSessionSubscription(ctx, backgroundTick)) {
    result.error = "Chat subscribe failed";
    return result;
  }

  String preview;
  bool previewTruncated = false;
  if (kind == AttachmentKind::File && isTextPreviewMimeType(mimeType)) {
    preview = readTextFilePreview(filePath,
                                  kMessengerTextFallbackPreviewMaxChars,
                                  &previewTruncated,
                                  nullptr);
  }
  const String sessionKey = activeMessengerSessionKey();
  const String message = buildAttachmentFallbackMessage(kind,
                                                        result.messageId,
                                                        filePath,
                                                        result.fileName,
                                                        mimeType,
                                                        totalBytes,
                                                        caption,
                                                        reason,
                                                        preview,
                                                        previewTruncated);
  String sendError;
  if (!sendAgentRequestMessage(ctx,
                               sessionKey,
                               target,
                               message,
                               backgroundTick,
                               &sendError)) {
    result.error = sendError.isEmpty() ? String("Text fallback send failed") : sendError;
    return result;
  }

  result.ok = true;
  result.route = AttachmentRoute::TextFallback;
  return result;
}

AttachmentSendResult sendAttachmentViaAgentRequest(
    AppContext &ctx,
    const String &filePath,
    const String &mimeType,
    AttachmentKind kind,
    const String &target,
    const String &caption,
    uint32_t totalBytes,
    const std::function<void()> &backgroundTick) {
  AttachmentSendResult result;
  result.ok = false;
  result.route = AttachmentRoute::Failed;
  result.eventName = "agent.request";
  result.mimeType = mimeType;
  result.fileName = baseName(filePath);
  result.totalBytes = totalBytes;
  result.messageId = makeMessageId(attachmentKindToken(kind));

  if (totalBytes == 0 || totalBytes > kAgentAttachmentMaxBytes) {
    result.error = "Binary attachment exceeds limit";
    return result;
  }
  if (!ensureMessengerSessionSubscription(ctx, backgroundTick)) {
    result.error = "Chat subscribe failed";
    return result;
  }

  String checksumError;
  const String checksum = computeFileSha256Hex(filePath, &checksumError);
  if (checksum.isEmpty()) {
    result.error = checksumError.isEmpty() ? String("Checksum failed") : checksumError;
    return result;
  }

  const String sessionKey = activeMessengerSessionKey();
  const uint16_t totalChunks = static_cast<uint16_t>(
      (totalBytes + static_cast<uint32_t>(kAgentAttachmentChunkBytes) - 1U) /
      static_cast<uint32_t>(kAgentAttachmentChunkBytes));
  if (totalChunks == 0 || totalChunks > kAgentAttachmentMaxChunks) {
    result.error = "Chunk count out of range";
    return result;
  }

  sendChatSessionEvent(ctx, "chat.unsubscribe", sessionKey);
  gSubscribedSessionKey = "";
  gSubscribedConnectOkMs = 0;

  ScopedProgressOverlay progress(ctx.uiRuntime,
                                 attachmentUiTitle(kind),
                                 "Preparing attachment...");

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    ensureMessengerSessionSubscription(ctx, backgroundTick, false);
    result.error = "Attachment open failed";
    return result;
  }

  std::vector<uint8_t> raw(kAgentAttachmentChunkBytes, 0);
  std::vector<char> encoded(kAgentAttachmentBase64ChunkBytes, 0);
  if (raw.empty() || encoded.empty()) {
    file.close();
    ensureMessengerSessionSubscription(ctx, backgroundTick, false);
    result.error = "Out of memory";
    return result;
  }

  String beginMessage;
  beginMessage.reserve(caption.length() + 640U);
  beginMessage += "[ATTACHMENT_BEGIN]\n";
  beginMessage += "id:";
  beginMessage += result.messageId;
  beginMessage += "\nkind:";
  beginMessage += attachmentKindToken(kind);
  beginMessage += "\nname:";
  beginMessage += result.fileName;
  beginMessage += "\nmime:";
  beginMessage += mimeType;
  beginMessage += "\nsize:";
  beginMessage += String(static_cast<unsigned long>(totalBytes));
  beginMessage += "\nchunks:";
  beginMessage += String(static_cast<unsigned long>(totalChunks));
  beginMessage += "\nchecksum:";
  beginMessage += checksum;
  beginMessage += "\nencoding:base64";
  if (!caption.isEmpty()) {
    beginMessage += "\ncaption:";
    beginMessage += caption;
  }
  beginMessage += "\nreply:ignore chunk transport and wait for END";

  String sendError;
  if (!sendAgentRequestMessage(ctx,
                               sessionKey,
                               target,
                               beginMessage,
                               backgroundTick,
                               &sendError)) {
    file.close();
    ensureMessengerSessionSubscription(ctx, backgroundTick, false);
    result.error = sendError.isEmpty() ? String("Attachment begin send failed") : sendError;
    return result;
  }

  uint16_t chunkIndex = 0;
  int lastShownDecile = -1;
  bool failed = false;

  while (file.available() && chunkIndex < totalChunks) {
    const size_t readLen = file.read(raw.data(), raw.size());
    if (readLen == 0) {
      break;
    }

    size_t encodedLen = 0;
    if (!encodeBase64(raw.data(),
                      readLen,
                      encoded.data(),
                      encoded.size(),
                      &encodedLen)) {
      failed = true;
      sendError = "Base64 encode failed";
      break;
    }

    String chunkMessage;
    chunkMessage.reserve(encodedLen + 320U);
    chunkMessage += "[ATTACHMENT_CHUNK]\n";
    chunkMessage += "id:";
    chunkMessage += result.messageId;
    chunkMessage += "\nseq:";
    chunkMessage += String(static_cast<unsigned long>(chunkIndex + 1));
    chunkMessage += "\nchunks:";
    chunkMessage += String(static_cast<unsigned long>(totalChunks));
    chunkMessage += "\nbytes:";
    chunkMessage += String(static_cast<unsigned long>(readLen));
    chunkMessage += "\ndata:";
    chunkMessage += encoded.data();
    chunkMessage += "\nreply:ignore";

    if (!sendAgentRequestMessage(ctx,
                                 sessionKey,
                                 target,
                                 chunkMessage,
                                 backgroundTick,
                                 &sendError)) {
      failed = true;
      break;
    }

    ++chunkIndex;
    const int percent = static_cast<int>((static_cast<uint32_t>(chunkIndex) * 100U) /
                                         static_cast<uint32_t>(totalChunks));
    const int decile = percent / 10;
    if (decile != lastShownDecile) {
      lastShownDecile = decile;
      progress.update("Sending attachment...", percent);
    }
    if (backgroundTick && ((chunkIndex % 4U) == 0U)) {
      backgroundTick();
    }
  }
  file.close();

  if (!failed && chunkIndex != totalChunks) {
    failed = true;
    sendError = "Attachment chunks incomplete";
  }

  if (!ensureMessengerSessionSubscription(ctx, backgroundTick, false)) {
    failed = true;
    if (sendError.isEmpty()) {
      sendError = "Chat resubscribe failed";
    }
  }

  if (failed) {
    result.error = sendError.isEmpty() ? String("Attachment chunk send failed") : sendError;
    return result;
  }

  String endMessage;
  endMessage.reserve(caption.length() + 640U);
  endMessage += "[ATTACHMENT_END]\n";
  endMessage += "id:";
  endMessage += result.messageId;
  endMessage += "\nkind:";
  endMessage += attachmentKindToken(kind);
  endMessage += "\nname:";
  endMessage += result.fileName;
  endMessage += "\nmime:";
  endMessage += mimeType;
  endMessage += "\nsize:";
  endMessage += String(static_cast<unsigned long>(totalBytes));
  endMessage += "\nchunks:";
  endMessage += String(static_cast<unsigned long>(totalChunks));
  endMessage += "\nchecksum:";
  endMessage += checksum;
  if (!caption.isEmpty()) {
    endMessage += "\ncaption:";
    endMessage += caption;
  }
  endMessage +=
      "\nReconstruct ATTACHMENT_CHUNK parts with same id in order and process as one file.";

  if (!sendAgentRequestMessage(ctx,
                               sessionKey,
                               target,
                               endMessage,
                               backgroundTick,
                               &sendError)) {
    result.error = sendError.isEmpty() ? String("Attachment end send failed") : sendError;
    return result;
  }

  progress.update("Attachment sent", 100);
  result.ok = true;
  result.route = AttachmentRoute::Framed;
  return result;
}

const char *chatSendAttachmentType(AttachmentKind kind, const String &mimeType) {
  if (kind == AttachmentKind::Voice) {
    return "audio";
  }
  return isImageMimeType(mimeType) ? "image" : "file";
}

AttachmentSendResult sendAttachmentViaChatSend(
    AppContext &ctx,
    const String &filePath,
    const String &mimeType,
    AttachmentKind kind,
    const String &target,
    const String &caption,
    uint32_t totalBytes,
    const std::function<void()> &backgroundTick) {
  AttachmentSendResult result;
  result.ok = false;
  result.route = AttachmentRoute::Failed;
  result.eventName = "chat.send";
  result.mimeType = mimeType;
  result.fileName = baseName(filePath);
  result.totalBytes = totalBytes;
  result.messageId = makeMessageId(attachmentKindToken(kind));

  if (totalBytes == 0 || totalBytes > kChatSendAttachmentMaxBytes) {
    result.error = "Attachment too large for chat.send";
    return result;
  }
  if (!ensureMessengerSessionSubscription(ctx, backgroundTick)) {
    result.error = "Chat subscribe failed";
    return result;
  }

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    result.error = "Attachment open failed";
    return result;
  }

  std::vector<uint8_t> raw(kMessageChunkBytes, 0);
  std::vector<char> encoded(kBase64ChunkBufferBytes, 0);
  if (raw.empty() || encoded.empty()) {
    file.close();
    result.error = "Out of memory";
    return result;
  }

  String base64;
  const size_t expectedEncoded = base64_encode_expected_len(totalBytes);
  base64.reserve(expectedEncoded + 16U);

  while (file.available()) {
    const size_t readLen = file.read(raw.data(), raw.size());
    if (readLen == 0) {
      file.close();
      result.error = "Attachment read failed";
      return result;
    }
    if (!encodeBase64(raw.data(), readLen, encoded.data(), encoded.size())) {
      file.close();
      result.error = "Base64 encode failed";
      return result;
    }
    base64 += encoded.data();
  }
  file.close();

  String message = caption;
  message.trim();
  if (message.isEmpty()) {
    message = "See attached.";
  }

  size_t payloadCap = base64.length() + message.length() + 1024U;
  if (payloadCap < 2048U) {
    payloadCap = 2048U;
  }

  DynamicJsonDocument payload(payloadCap);
  payload["message"] = message;
  payload["sessionKey"] = activeMessengerSessionKey();
  payload["to"] = target;
  payload["thinking"] = "low";
  JsonArray attachments = payload.createNestedArray("attachments");
  JsonObject attachment = attachments.createNestedObject();
  attachment["type"] = chatSendAttachmentType(kind, mimeType);
  attachment["mimeType"] = mimeType;
  attachment["fileName"] = result.fileName;
  attachment["content"] = base64;

  if (!sendGatewayEventWithRetry(ctx, "chat.send", payload, backgroundTick)) {
    result.error = withGatewayErrorSuffix("chat.send attachment failed", ctx.gateway);
    return result;
  }

  result.ok = true;
  result.route = AttachmentRoute::ApiAttachment;
  return result;
}

AttachmentSendResult sendLegacyAttachmentChunks(
    AppContext &ctx,
    AttachmentKind kind,
    const String &filePath,
    const String &mimeType,
    const String &target,
    const String &caption,
    uint32_t totalBytes,
    const std::function<void()> &backgroundTick,
    const char *metaEventName,
    const char *chunkEventName) {
  AttachmentSendResult result;
  result.ok = false;
  result.route = AttachmentRoute::Failed;
  result.eventName = metaEventName ? String(metaEventName) : String();
  result.mimeType = mimeType;
  result.fileName = baseName(filePath);
  result.totalBytes = totalBytes;
  result.messageId = makeMessageId(attachmentKindToken(kind));

  if (!metaEventName || !chunkEventName) {
    result.error = "Legacy event is invalid";
    return result;
  }
  if (totalBytes == 0) {
    result.error = "Attachment is empty";
    return result;
  }
  const uint32_t legacyMaxBytes =
      kind == AttachmentKind::Voice ? kMaxVoiceBytes : kMaxFileBytes;
  if (totalBytes > legacyMaxBytes) {
    result.error = "Legacy chunk payload too large";
    return result;
  }
  if (!ensureMessengerSessionSubscription(ctx, backgroundTick)) {
    result.error = "Chat subscribe failed";
    return result;
  }

  const uint16_t totalChunks = static_cast<uint16_t>(
      (totalBytes + static_cast<uint32_t>(kMessageChunkBytes) - 1U) /
      static_cast<uint32_t>(kMessageChunkBytes));
  if (totalChunks == 0) {
    result.error = "Chunk count out of range";
    return result;
  }

  const String sessionKey = activeMessengerSessionKey();
  DynamicJsonDocument meta(2048);
  meta["id"] = result.messageId;
  meta["from"] = kMessageSenderId;
  meta["to"] = target;
  meta["sessionKey"] = sessionKey;
  meta["type"] = attachmentKindToken(kind);
  meta["fileName"] = result.fileName;
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

  if (!sendGatewayEventWithRetry(ctx, metaEventName, meta, backgroundTick)) {
    result.error = withGatewayErrorSuffix("Legacy meta send failed", ctx.gateway);
    return result;
  }

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    result.error = "Attachment open failed";
    return result;
  }

  ScopedProgressOverlay progress(ctx.uiRuntime, attachmentUiTitle(kind), "Legacy sending...");
  std::vector<uint8_t> raw(kMessageChunkBytes, 0);
  std::vector<char> encoded(kBase64ChunkBufferBytes, 0);
  if (raw.empty() || encoded.empty()) {
    file.close();
    result.error = "Out of memory";
    return result;
  }

  DynamicJsonDocument chunk(2048);
  uint16_t chunkIndex = 0;
  int lastShownDecile = -1;
  String sendError;
  while (file.available() && chunkIndex < totalChunks) {
    const size_t readLen = file.read(raw.data(), raw.size());
    if (readLen == 0) {
      break;
    }

    if (!encodeBase64(raw.data(), readLen, encoded.data(), encoded.size())) {
      sendError = "Base64 encode failed";
      break;
    }

    chunk.clear();
    chunk["id"] = result.messageId;
    chunk["from"] = kMessageSenderId;
    chunk["to"] = target;
    chunk["sessionKey"] = sessionKey;
    chunk["seq"] = static_cast<uint32_t>(chunkIndex + 1);
    chunk["chunks"] = totalChunks;
    chunk["last"] = (chunkIndex + 1) >= totalChunks;
    chunk["data"] = encoded.data();
    const uint64_t chunkTs = currentUnixMs();
    if (chunkTs > 0) {
      chunk["ts"] = chunkTs;
    }

    if (!sendGatewayEventWithRetry(ctx, chunkEventName, chunk, backgroundTick)) {
      sendError = withGatewayErrorSuffix("Legacy chunk send failed", ctx.gateway);
      break;
    }

    ++chunkIndex;
    const int percent = static_cast<int>((static_cast<uint32_t>(chunkIndex) * 100U) /
                                         static_cast<uint32_t>(totalChunks));
    const int decile = percent / 10;
    if (decile != lastShownDecile) {
      lastShownDecile = decile;
      progress.update("Legacy sending...", percent);
    }
    if (backgroundTick && ((chunkIndex % 8U) == 0U)) {
      backgroundTick();
    }
  }
  file.close();

  if (chunkIndex != totalChunks) {
    if (sendError.isEmpty()) {
      sendError = "Legacy send incomplete";
    }
    result.error = sendError;
    return result;
  }

  progress.update("Legacy attachment sent", 100);
  result.ok = true;
  result.route = AttachmentRoute::LegacyMetaChunk;
  return result;
}

AttachmentSendResult sendLegacyFileChunks(
    AppContext &ctx,
    const String &filePath,
    const String &mimeType,
    const String &target,
    const String &caption,
    uint32_t totalBytes,
    const std::function<void()> &backgroundTick) {
  return sendLegacyAttachmentChunks(ctx,
                                    AttachmentKind::File,
                                    filePath,
                                    mimeType,
                                    target,
                                    caption,
                                    totalBytes,
                                    backgroundTick,
                                    "msg.file.meta",
                                    "msg.file.chunk");
}

AttachmentSendResult sendLegacyVoiceChunks(
    AppContext &ctx,
    const String &filePath,
    const String &mimeType,
    const String &target,
    const String &caption,
    uint32_t totalBytes,
    const std::function<void()> &backgroundTick) {
  return sendLegacyAttachmentChunks(ctx,
                                    AttachmentKind::Voice,
                                    filePath,
                                    mimeType,
                                    target,
                                    caption,
                                    totalBytes,
                                    backgroundTick,
                                    "msg.voice.meta",
                                    "msg.voice.chunk");
}

bool sendAttachmentMessage(AppContext &ctx,
                           AttachmentKind kind,
                           const String &filePath,
                           const String &caption,
                           const std::function<void()> &backgroundTick) {
  const String uiTitle = attachmentUiTitle(kind);
  if (filePath.isEmpty()) {
    ctx.uiRuntime->showToast(uiTitle, "Path is empty", 1300, backgroundTick);
    return false;
  }

  const String target = defaultAgentId();
  const String mimeType =
      kind == AttachmentKind::Voice ? detectAudioMime(filePath) : detectFileMime(filePath);

  File file = SD.open(filePath.c_str(), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) {
      file.close();
    }
    ctx.uiRuntime->showToast(uiTitle,
                             kind == AttachmentKind::Voice ? "Open voice file failed"
                                                           : "Open file failed",
                             1600,
                             backgroundTick);
    return false;
  }

  const uint32_t totalBytes = static_cast<uint32_t>(file.size());
  file.close();
  if (totalBytes == 0) {
    ctx.uiRuntime->showToast(uiTitle,
                             kind == AttachmentKind::Voice ? "Voice file is empty"
                                                           : "File is empty",
                             1500,
                             backgroundTick);
    return false;
  }

  const uint32_t routeMaxBytes =
      kind == AttachmentKind::Voice ? kMaxVoiceBytes : kMaxFileBytes;
  if (totalBytes > routeMaxBytes) {
    ctx.uiRuntime->showToast(uiTitle,
                             kind == AttachmentKind::Voice ? "File too large (max 2MB)"
                                                           : "File too large (max 4MB)",
                             1800,
                             backgroundTick);
    return false;
  }

  bool framedPreferred = false;
  String fallbackReason;
  if (kind == AttachmentKind::Voice) {
    framedPreferred = true;
  } else if (isImageMimeType(mimeType)) {
    framedPreferred = true;
  } else {
    fallbackReason = "Non-image file uses text fallback";
  }

  if (totalBytes > kMessengerBinaryAttachMaxBytes) {
    framedPreferred = false;
    fallbackReason = "Binary size exceeds framed route limit";
  }

  AttachmentSendResult sendResult;
  if (totalBytes <= kChatSendAttachmentMaxBytes) {
    sendResult = sendAttachmentViaChatSend(ctx,
                                           filePath,
                                           mimeType,
                                           kind,
                                           target,
                                           caption,
                                           totalBytes,
                                           backgroundTick);
  }

  if (!sendResult.ok && framedPreferred) {
    sendResult = sendAttachmentViaAgentRequest(ctx,
                                               filePath,
                                               mimeType,
                                               kind,
                                               target,
                                               caption,
                                               totalBytes,
                                               backgroundTick);
    if (!sendResult.ok && kLegacyMediaFallbackEnabled) {
      if (kind == AttachmentKind::Voice) {
        sendResult = sendLegacyVoiceChunks(ctx,
                                           filePath,
                                           mimeType,
                                           target,
                                           caption,
                                           totalBytes,
                                           backgroundTick);
      } else {
        sendResult = sendLegacyFileChunks(ctx,
                                          filePath,
                                          mimeType,
                                          target,
                                          caption,
                                          totalBytes,
                                          backgroundTick);
      }
    }
  }

  if (!sendResult.ok && !framedPreferred) {
    sendResult = sendAttachmentTextFallback(ctx,
                                            kind,
                                            filePath,
                                            mimeType,
                                            target,
                                            caption,
                                            totalBytes,
                                            fallbackReason,
                                            backgroundTick);
  }

  if (!sendResult.ok) {
    String errorMessage = sendResult.error;
    errorMessage.trim();
    if (errorMessage.isEmpty()) {
      errorMessage = "Send failed";
    }
    ctx.uiRuntime->showToast(uiTitle, errorMessage, 1900, backgroundTick);
    return false;
  }

  GatewayInboxMessage sent;
  sent.id = sendResult.messageId.isEmpty() ? makeMessageId(attachmentKindToken(kind))
                                           : sendResult.messageId;
  sent.event = sendResult.eventName.isEmpty() ? "agent.request" : sendResult.eventName;
  sent.type = attachmentKindToken(kind);
  sent.from = kMessageSenderId;
  sent.to = target;
  sent.text = caption;
  sent.fileName = sendResult.fileName.isEmpty() ? baseName(filePath) : sendResult.fileName;
  sent.contentType = sendResult.mimeType.isEmpty() ? mimeType : sendResult.mimeType;
  sent.voiceBytes = sendResult.totalBytes > 0 ? sendResult.totalBytes : totalBytes;
  sent.tsMs = currentUnixMs();
  pushOutbox(sent);

  ctx.uiRuntime->showToast(uiTitle, attachmentRouteToast(sendResult.route), 1300, backgroundTick);
  return true;
}

bool sendVoiceFileMessage(AppContext &ctx,
                          const String &filePath,
                          const String &caption,
                          const std::function<void()> &backgroundTick) {
  if (!ensureGatewayReady(ctx, backgroundTick)) {
    return false;
  }
  return sendAttachmentMessage(ctx, AttachmentKind::Voice, filePath, caption, backgroundTick);
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

  String mountErr;
  if (!ensureSdMountedForVoice(&mountErr)) {
    ctx.uiRuntime->showToast("Voice",
                      mountErr.isEmpty() ? String("SD mount failed") : mountErr,
                      1600,
                      backgroundTick);
    return;
  }

  sendVoiceFileMessage(ctx, filePath, String(), backgroundTick);
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

  uint16_t recordSeconds = 0;
  if (!askVoiceRecordSeconds(ctx, backgroundTick, &recordSeconds)) {
    return;
  }

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

  String recordingMsg = "Recording ";
  recordingMsg += String(static_cast<unsigned long>(recordSeconds));
  recordingMsg += "s...";
  ctx.uiRuntime->showToast("Voice", recordingMsg, 900, backgroundTick);

  String recordErr;
  uint32_t bytesWritten = 0;
  {
    ScopedOkBackBlock guard(ctx.uiRuntime);
    const std::function<void()> noBackgroundTick;
    if (!recordMicWavToSd(voicePath,
                          recordSeconds,
                          noBackgroundTick,
                          std::function<bool()>(),
                          &recordErr,
                          &bytesWritten)) {
      ctx.uiRuntime->showToast("Voice",
                        recordErr.isEmpty() ? String("MIC recording failed")
                                            : recordErr,
                        1800,
                        backgroundTick);
      return;
    }
  }

  if (bytesWritten > kMaxVoiceBytes) {
    SD.remove(voicePath.c_str());
    ctx.uiRuntime->showToast("Voice", "Recording too large for send", 1700, backgroundTick);
    return;
  }

  sendVoiceFileMessage(ctx, voicePath, String(), backgroundTick);
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

  uint16_t recordSeconds = 0;
  if (!askVoiceRecordSeconds(ctx, backgroundTick, &recordSeconds)) {
    return true;
  }

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

  String recordingMsg = "Recording ";
  recordingMsg += String(static_cast<unsigned long>(recordSeconds));
  recordingMsg += "s...";
  ctx.uiRuntime->showToast("BLE", recordingMsg, 900, backgroundTick);

  String recordErr;
  uint32_t bytesWritten = 0;
  {
    ScopedOkBackBlock guard(ctx.uiRuntime);
    if (!ctx.ble->recordAudioStreamWavToSd(voicePath,
                                           recordSeconds,
                                           backgroundTick,
                                           std::function<bool()>(),
                                           &recordErr,
                                           &bytesWritten)) {
      ctx.uiRuntime->showToast("BLE",
                        recordErr.isEmpty() ? String("BLE recording failed")
                                            : recordErr,
                        1800,
                        backgroundTick);
      return true;
    }
  }

  if (bytesWritten > kMaxVoiceBytes) {
    SD.remove(voicePath.c_str());
    ctx.uiRuntime->showToast("Voice", "Recording too large for send", 1700, backgroundTick);
    return true;
  }

  sendVoiceFileMessage(ctx, voicePath, String(), backgroundTick);
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

  String mountErr;
  if (!ensureSdMountedForVoice(&mountErr)) {
    ctx.uiRuntime->showToast("File",
                      mountErr.isEmpty() ? String("SD mount failed") : mountErr,
                      1600,
                      backgroundTick);
    return;
  }

  String filePath;
  if (!selectSdFile(ctx, "Select File", std::function<bool(const String &)>(), &filePath, backgroundTick)) {
    return;
  }

  String caption;
  if (!ctx.uiRuntime->textInput("Message(optional)", caption, false, backgroundTick)) {
    return;
  }
  caption.trim();
  sendAttachmentMessage(ctx, AttachmentKind::File, filePath, caption, backgroundTick);
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

  String label = entry.outgoing ? "Me: " : "Agent: ";
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

  lines.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
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
                                 "Start new session?",
                                 backgroundTick,
                                 "Run",
                                 "Cancel")) {
        startMainSessionWithGreeting(ctx, backgroundTick);
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
                                  effectiveDeviceName(ctx.config),
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
  if (ctx.wifi->hasConnectionError()) {
    lines.push_back("Wi-Fi Error: " + ctx.wifi->lastConnectionError());
  }
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
  lines.push_back("Device Name: " + effectiveDeviceName(ctx.config));
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
  if (!ctx.config.autoConnect) {
    return;
  }

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
