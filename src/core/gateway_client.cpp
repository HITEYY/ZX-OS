#include "gateway_client.h"

#include <WiFi.h>

#include "user_config.h"

namespace {

constexpr const char *OPENCLAW_CLIENT_ID = "node-host";
constexpr const char *OPENCLAW_CLIENT_MODE = "node";
constexpr int OPENCLAW_PROTOCOL_VERSION = 1;

constexpr unsigned long kReconnectRetryMs = 2000UL;

}  // namespace

void GatewayClient::begin() {
  if (initialized_) {
    return;
  }
  ws_.onEvent([this](WStype_t type, uint8_t *payload, size_t length) {
    onWsEvent(type, payload, length);
  });
  ws_.setReconnectInterval(5000);
  ws_.enableHeartbeat(15000, 3000, 2);

  initialized_ = true;
}

void GatewayClient::setInvokeRequestHandler(InvokeRequestHandler handler) {
  invokeHandler_ = handler;
}

void GatewayClient::setTelemetryBuilder(TelemetryBuilder builder) {
  telemetryBuilder_ = builder;
}

void GatewayClient::configure(const RuntimeConfig &config) {
  config_ = config;
}

void GatewayClient::connectNow() {
  shouldConnect_ = true;
  if (!wsStarted_) {
    startWebSocket();
  }
}

void GatewayClient::disconnectNow() {
  shouldConnect_ = false;
  gatewayReady_ = false;
  wsConnected_ = false;
  connectRequestId_ = "";

  if (wsStarted_) {
    ws_.disconnect();
    wsStarted_ = false;
  }
}

void GatewayClient::reconnectNow() {
  disconnectNow();
  shouldConnect_ = true;
  startWebSocket();
}

void GatewayClient::tick() {
  if (!initialized_) {
    return;
  }

  if (wsStarted_) {
    ws_.loop();
  }

  if (shouldConnect_ && !wsStarted_) {
    const unsigned long now = millis();
    if (now - lastConnectAttemptMs_ >= kReconnectRetryMs) {
      startWebSocket();
    }
  }

  if (gatewayReady_ && telemetryBuilder_) {
    const unsigned long now = millis();
    if (now - lastTelemetryMs_ >= USER_TELEMETRY_INTERVAL_MS) {
      lastTelemetryMs_ = now;
      DynamicJsonDocument payload(1024);
      JsonObject obj = payload.to<JsonObject>();
      telemetryBuilder_(obj);
      sendNodeEvent("cc1101.telemetry", payload);
    }
  }
}

bool GatewayClient::isReady() const {
  return gatewayReady_;
}

String GatewayClient::lastError() const {
  return lastError_;
}

GatewayStatus GatewayClient::status() const {
  GatewayStatus s;
  s.shouldConnect = shouldConnect_;
  s.wsConnected = wsConnected_;
  s.gatewayReady = gatewayReady_;
  s.lastError = lastError_;
  s.lastConnectAttemptMs = lastConnectAttemptMs_;
  s.lastConnectOkMs = lastConnectOkMs_;
  return s;
}

bool GatewayClient::sendNodeEvent(const char *eventName, JsonDocument &payloadDoc) {
  DynamicJsonDocument params(2048);
  params["event"] = eventName;
  params["payload"] = payloadDoc.as<JsonVariantConst>();
  return sendRequest("node.event", params, nullptr);
}

bool GatewayClient::sendInvokeOk(const String &invokeId,
                                 const String &nodeId,
                                 JsonDocument &payloadDoc) {
  DynamicJsonDocument params(4096);
  params["id"] = invokeId;
  params["nodeId"] = nodeId;
  params["ok"] = true;
  params["payload"] = payloadDoc.as<JsonVariantConst>();
  return sendRequest("node.invoke.result", params, nullptr);
}

bool GatewayClient::sendInvokeError(const String &invokeId,
                                    const String &nodeId,
                                    const char *code,
                                    const String &message) {
  DynamicJsonDocument params(1024);
  params["id"] = invokeId;
  params["nodeId"] = nodeId;
  params["ok"] = false;
  JsonObject error = params.createNestedObject("error");
  error["code"] = code;
  error["message"] = message;
  return sendRequest("node.invoke.result", params, nullptr);
}

void GatewayClient::onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      wsConnected_ = false;
      gatewayReady_ = false;
      connectRequestId_ = "";
      wsStarted_ = false;
      if (shouldConnect_ && !lastError_.length()) {
        lastError_ = "Gateway disconnected";
      }
      break;

    case WStype_CONNECTED:
      wsConnected_ = true;
      gatewayReady_ = false;
      lastError_ = "";
      sendConnectRequest();
      break;

    case WStype_TEXT:
      handleGatewayFrame(reinterpret_cast<const char *>(payload), length);
      break;

    case WStype_ERROR:
      lastError_ = "WebSocket error";
      break;

    default:
      break;
  }
}

void GatewayClient::startWebSocket() {
  String reason;
  if (!canStartConnection(&reason)) {
    lastError_ = reason;
    return;
  }

  GatewayEndpoint endpoint;
  if (!parseGatewayUrl(config_.gatewayUrl, endpoint)) {
    lastError_ = "Invalid gateway URL";
    return;
  }

  const bool wasStarted = wsStarted_;
  if (wasStarted) {
    ws_.disconnect();
  }

  if (endpoint.secure) {
    ws_.beginSSL(endpoint.host.c_str(), endpoint.port, endpoint.path.c_str(), "");
  } else {
    ws_.begin(endpoint.host.c_str(), endpoint.port, endpoint.path.c_str());
  }

  wsStarted_ = true;
  wsConnected_ = false;
  gatewayReady_ = false;
  connectRequestId_ = "";
  lastConnectAttemptMs_ = millis();
}

bool GatewayClient::canStartConnection(String *reason) const {
  if (config_.gatewayUrl.isEmpty()) {
    if (reason) {
      *reason = "Gateway URL is empty";
    }
    return false;
  }

  if (!hasGatewayCredentials(config_)) {
    if (reason) {
      *reason = "Gateway credential is missing";
    }
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (reason) {
      *reason = "Wi-Fi is not connected";
    }
    return false;
  }

  return true;
}

bool GatewayClient::sendRequest(const char *method,
                                JsonDocument &paramsDoc,
                                String *requestIdOut) {
  if (!wsConnected_) {
    return false;
  }

  DynamicJsonDocument frame(4096);
  const String reqId = nextReqId("req");

  frame["type"] = "req";
  frame["id"] = reqId;
  frame["method"] = method;
  frame["params"] = paramsDoc.as<JsonVariantConst>();

  String body;
  serializeJson(frame, body);

  const bool sent = ws_.sendTXT(body);
  if (sent && requestIdOut) {
    *requestIdOut = reqId;
  }
  return sent;
}

void GatewayClient::sendConnectRequest() {
  DynamicJsonDocument params(3072);
  params["minProtocol"] = OPENCLAW_PROTOCOL_VERSION;
  params["maxProtocol"] = OPENCLAW_PROTOCOL_VERSION;

  JsonObject client = params.createNestedObject("client");
  client["id"] = OPENCLAW_CLIENT_ID;
  client["displayName"] = USER_OPENCLAW_DISPLAY_NAME;
  client["version"] = "0.2.0";
  client["platform"] = "esp32s3";
  client["deviceFamily"] = "lilygo-t-embed-cc1101";
  client["modelIdentifier"] = "T_EMBED_1101";
  client["mode"] = OPENCLAW_CLIENT_MODE;
  client["instanceId"] = USER_OPENCLAW_INSTANCE_ID;

  params["role"] = "node";
  params.createNestedArray("scopes");

  JsonArray caps = params.createNestedArray("caps");
  caps.add("rf");
  caps.add("cc1101");

  JsonArray commands = params.createNestedArray("commands");
  commands.add("system.which");
  commands.add("system.run");
  commands.add("cc1101.info");
  commands.add("cc1101.set_freq");
  commands.add("cc1101.tx");

  JsonObject auth = params.createNestedObject("auth");
  if (config_.gatewayAuthMode == GatewayAuthMode::Password) {
    auth["password"] = config_.gatewayPassword;
  } else {
    auth["token"] = config_.gatewayToken;
  }

  if (!sendRequest("connect", params, &connectRequestId_)) {
    lastError_ = "Failed to send connect request";
  }
}

void GatewayClient::handleGatewayFrame(const char *text, size_t len) {
  DynamicJsonDocument doc(4096);
  const auto err = deserializeJson(doc, text, len);
  if (err) {
    lastError_ = "Invalid gateway frame";
    return;
  }

  const char *type = doc["type"] | "";
  if (strcmp(type, "res") == 0) {
    handleGatewayResponse(doc.as<JsonObjectConst>());
  } else if (strcmp(type, "event") == 0) {
    handleGatewayEvent(doc.as<JsonObjectConst>());
  }
}

void GatewayClient::handleGatewayResponse(JsonObjectConst frame) {
  const String id = frame["id"].as<String>();
  if (id != connectRequestId_) {
    return;
  }

  const bool ok = frame["ok"] | false;
  if (!ok) {
    gatewayReady_ = false;
    lastError_ = String(static_cast<const char *>(frame["error"]["message"] |
                                                   "Gateway connect rejected"));
    return;
  }

  gatewayReady_ = true;
  lastError_ = "";
  lastConnectOkMs_ = millis();

  if (telemetryBuilder_) {
    DynamicJsonDocument payload(1024);
    JsonObject obj = payload.to<JsonObject>();
    telemetryBuilder_(obj);
    sendNodeEvent("cc1101.telemetry", payload);
    lastTelemetryMs_ = millis();
  }
}

void GatewayClient::handleGatewayEvent(JsonObjectConst frame) {
  const String eventName = frame["event"].as<String>();
  if (eventName == "shutdown") {
    gatewayReady_ = false;
    lastError_ = "Gateway shutdown";
    return;
  }

  if (eventName != "node.invoke.request") {
    return;
  }

  if (!frame["payload"].is<JsonObjectConst>()) {
    return;
  }

  const JsonObjectConst payload = frame["payload"].as<JsonObjectConst>();
  const String invokeId = payload["id"].as<String>();
  const String nodeId = payload["nodeId"].as<String>();
  const String command = payload["command"].as<String>();
  const char *paramsJson = payload["paramsJSON"] | nullptr;

  DynamicJsonDocument paramsDoc(2048);
  if (!paramsJson || !paramsJson[0] || strcmp(paramsJson, "null") == 0) {
    paramsDoc.to<JsonObject>();
  } else {
    const auto parseErr = deserializeJson(paramsDoc, paramsJson);
    if (parseErr || !paramsDoc.is<JsonObject>()) {
      sendInvokeError(invokeId, nodeId, "INVALID_REQUEST", "invalid paramsJSON");
      return;
    }
  }

  if (!invokeHandler_) {
    sendInvokeError(invokeId, nodeId, "UNAVAILABLE", "invoke handler is not configured");
    return;
  }

  invokeHandler_(invokeId,
                 nodeId,
                 command,
                 paramsDoc.as<JsonObjectConst>());
}

bool GatewayClient::parseGatewayUrl(const String &rawUrl,
                                    GatewayEndpoint &out) const {
  String url = rawUrl;
  url.trim();
  if (url.length() == 0) {
    return false;
  }

  bool secure = false;
  String rest;
  if (url.startsWith("wss://")) {
    secure = true;
    rest = url.substring(6);
  } else if (url.startsWith("ws://")) {
    secure = false;
    rest = url.substring(5);
  } else {
    return false;
  }

  const int slashIndex = rest.indexOf('/');
  const String hostPort = slashIndex >= 0 ? rest.substring(0, slashIndex) : rest;
  const String path = slashIndex >= 0 ? rest.substring(slashIndex) : "/";

  if (hostPort.length() == 0) {
    return false;
  }

  String host = hostPort;
  uint16_t port = secure ? 443 : 80;

  const int colon = hostPort.lastIndexOf(':');
  if (colon > 0 && hostPort.indexOf(']') < 0) {
    host = hostPort.substring(0, colon);
    const String portStr = hostPort.substring(colon + 1);
    const int parsedPort = portStr.toInt();
    if (parsedPort <= 0 || parsedPort > 65535) {
      return false;
    }
    port = static_cast<uint16_t>(parsedPort);
  }

  out.valid = true;
  out.secure = secure;
  out.host = host;
  out.port = port;
  out.path = path;
  return true;
}

String GatewayClient::nextReqId(const char *prefix) {
  ++reqCounter_;
  String id(prefix);
  id += "-";
  id += String(reqCounter_);
  return id;
}
