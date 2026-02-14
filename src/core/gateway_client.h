#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

#include <functional>

#include "runtime_config.h"

struct GatewayStatus {
  bool shouldConnect = false;
  bool wsConnected = false;
  bool gatewayReady = false;
  String lastError;
  unsigned long lastConnectAttemptMs = 0;
  unsigned long lastConnectOkMs = 0;
};

class GatewayClient {
 public:
  using InvokeRequestHandler = std::function<void(const String &invokeId,
                                                  const String &nodeId,
                                                  const String &command,
                                                  JsonObjectConst params)>;

  using TelemetryBuilder = std::function<void(JsonObject payload)>;

  void begin();
  void setInvokeRequestHandler(InvokeRequestHandler handler);
  void setTelemetryBuilder(TelemetryBuilder builder);

  void configure(const RuntimeConfig &config);

  void connectNow();
  void disconnectNow();
  void reconnectNow();
  void tick();

  bool isReady() const;
  String lastError() const;
  GatewayStatus status() const;

  bool sendNodeEvent(const char *eventName, JsonDocument &payloadDoc);
  bool sendInvokeOk(const String &invokeId,
                    const String &nodeId,
                    JsonDocument &payloadDoc);
  bool sendInvokeError(const String &invokeId,
                       const String &nodeId,
                       const char *code,
                       const String &message);

 private:
  struct GatewayEndpoint {
    bool valid = false;
    bool secure = false;
    String host;
    uint16_t port = 0;
    String path;
  };

  RuntimeConfig config_;
  WebSocketsClient ws_;

  bool initialized_ = false;
  bool shouldConnect_ = false;
  bool wsStarted_ = false;
  bool wsConnected_ = false;
  bool gatewayReady_ = false;

  String connectRequestId_;
  uint32_t reqCounter_ = 0;
  String lastError_;

  unsigned long lastConnectAttemptMs_ = 0;
  unsigned long lastConnectOkMs_ = 0;
  unsigned long lastTelemetryMs_ = 0;

  InvokeRequestHandler invokeHandler_;
  TelemetryBuilder telemetryBuilder_;

  void onWsEvent(WStype_t type, uint8_t *payload, size_t length);
  void startWebSocket();
  bool canStartConnection(String *reason = nullptr) const;

  bool sendRequest(const char *method,
                   JsonDocument &paramsDoc,
                   String *requestIdOut = nullptr);
  void sendConnectRequest();

  void handleGatewayFrame(const char *text, size_t len);
  void handleGatewayResponse(JsonObjectConst frame);
  void handleGatewayEvent(JsonObjectConst frame);

  bool parseGatewayUrl(const String &rawUrl, GatewayEndpoint &out) const;
  String nextReqId(const char *prefix);
  void persistGatewayConfigBestEffort();
  bool ensureDeviceIdentity(String *error = nullptr);
  bool decodeBase64Url(const String &in, uint8_t *out, size_t outLen) const;
  String encodeBase64Url(const uint8_t *data, size_t len) const;
  String sha256Hex(const uint8_t *data, size_t len) const;
  String buildDeviceAuthPayload(uint64_t signedAtMs, const String &tokenForSignature) const;
  bool hasSharedCredential() const;
  uint64_t currentUnixMs() const;

  String connectNonce_;
  uint64_t connectChallengeTsMs_ = 0;
  unsigned long connectQueuedAtMs_ = 0;
  bool connectSent_ = false;
  bool connectUsedDeviceToken_ = false;
  bool connectCanFallbackToShared_ = false;
};
