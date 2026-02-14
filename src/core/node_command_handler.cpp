#include "node_command_handler.h"

#include <WiFi.h>

#include "cc1101_radio.h"
#include "gateway_client.h"

namespace {

struct ArgList {
  static constexpr size_t kMaxArgs = 8;
  String values[kMaxArgs];
  size_t count = 0;
};

bool parseArgArray(JsonVariantConst value, ArgList &outArgs) {
  if (!value.is<JsonArrayConst>()) {
    return false;
  }

  JsonArrayConst arr = value.as<JsonArrayConst>();
  outArgs.count = 0;

  for (JsonVariantConst item : arr) {
    if (outArgs.count >= ArgList::kMaxArgs) {
      break;
    }

    if (item.is<const char *>()) {
      outArgs.values[outArgs.count++] = String(item.as<const char *>());
    } else if (item.is<long>() || item.is<unsigned long>() || item.is<long long>() ||
               item.is<unsigned long long>() || item.is<float>() || item.is<double>()) {
      outArgs.values[outArgs.count++] = item.as<String>();
    } else {
      return false;
    }
  }

  return outArgs.count > 0;
}

bool parseUInt64Token(const String &token, uint64_t &out) {
  char *endPtr = nullptr;
  const unsigned long long value = strtoull(token.c_str(), &endPtr, 0);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = static_cast<uint64_t>(value);
  return true;
}

bool parseIntToken(const String &token, int &out) {
  char *endPtr = nullptr;
  const long value = strtol(token.c_str(), &endPtr, 10);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

bool parseFloatToken(const String &token, float &out) {
  char *endPtr = nullptr;
  const float value = strtof(token.c_str(), &endPtr);
  if (endPtr == token.c_str() || *endPtr != '\0') {
    return false;
  }
  out = value;
  return true;
}

bool readFloatFromJson(JsonVariantConst v, float &out) {
  if (v.is<float>()) {
    out = v.as<float>();
    return true;
  }
  if (v.is<double>()) {
    out = static_cast<float>(v.as<double>());
    return true;
  }
  if (v.is<const char *>()) {
    return parseFloatToken(String(v.as<const char *>()), out);
  }
  return false;
}

bool readUInt32FromJson(JsonVariantConst v, uint32_t &out) {
  if (v.is<uint32_t>()) {
    out = v.as<uint32_t>();
    return true;
  }
  if (v.is<int>()) {
    const int iv = v.as<int>();
    if (iv < 0) {
      return false;
    }
    out = static_cast<uint32_t>(iv);
    return true;
  }
  if (v.is<const char *>()) {
    char *endPtr = nullptr;
    const unsigned long value = strtoul(String(v.as<const char *>()).c_str(), &endPtr, 0);
    if (!endPtr || *endPtr != '\0') {
      return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
  }
  return false;
}

bool readUInt64FromJson(JsonVariantConst v, uint64_t &out) {
  if (v.is<uint64_t>()) {
    out = v.as<uint64_t>();
    return true;
  }
  if (v.is<unsigned long long>()) {
    out = static_cast<uint64_t>(v.as<unsigned long long>());
    return true;
  }
  if (v.is<const char *>()) {
    return parseUInt64Token(String(v.as<const char *>()), out);
  }
  return false;
}

void buildInfoPayload(JsonObject obj) {
  appendCc1101Info(obj);
  obj["wifiConnected"] = WiFi.status() == WL_CONNECTED;
  obj["wifiRssi"] = (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0;
  obj["ip"] = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("");
  obj["uptimeMs"] = millis();
}

}  // namespace

void NodeCommandHandler::setGatewayClient(GatewayClient *gateway) {
  gateway_ = gateway;
}

void NodeCommandHandler::handleInvoke(const String &invokeId,
                                      const String &nodeId,
                                      const String &command,
                                      JsonObjectConst params) {
  if (!gateway_) {
    return;
  }

  if (command == "system.which") {
    if (handleSystemWhich(invokeId, nodeId, params)) {
      return;
    }
    gateway_->sendInvokeError(invokeId,
                              nodeId,
                              "INVALID_REQUEST",
                              "bins array required");
    return;
  }

  if (command == "system.run") {
    if (handleSystemRun(invokeId, nodeId, params)) {
      return;
    }
    return;
  }

  if (command.startsWith("cc1101.")) {
    if (handleCc1101Command(invokeId, nodeId, command, params)) {
      return;
    }
    return;
  }

  gateway_->sendInvokeError(invokeId,
                            nodeId,
                            "UNAVAILABLE",
                            "command not supported");
}

bool NodeCommandHandler::handleSystemWhich(const String &invokeId,
                                           const String &nodeId,
                                           JsonObjectConst params) {
  if (!params["bins"].is<JsonArrayConst>()) {
    return false;
  }

  DynamicJsonDocument payload(768);
  JsonObject binsOut = payload.createNestedObject("bins");
  JsonArrayConst bins = params["bins"].as<JsonArrayConst>();

  for (JsonVariantConst v : bins) {
    if (!v.is<const char *>()) {
      continue;
    }

    const String bin = v.as<const char *>();
    if (bin == "cc1101.info" || bin == "cc1101.set_freq" || bin == "cc1101.tx") {
      binsOut[bin] = "builtin://t-embed-cc1101";
    }
  }

  gateway_->sendInvokeOk(invokeId, nodeId, payload);
  return true;
}

bool NodeCommandHandler::handleSystemRun(const String &invokeId,
                                         const String &nodeId,
                                         JsonObjectConst params) {
  ArgList args;
  if (!parseArgArray(params["command"], args)) {
    gateway_->sendInvokeError(invokeId,
                              nodeId,
                              "INVALID_REQUEST",
                              "command array required");
    return true;
  }

  String stdoutText;
  String stderrText;
  int exitCode = 0;
  bool success = false;

  DynamicJsonDocument resultPayload(1024);
  JsonObject result = resultPayload.to<JsonObject>();

  const String cmd = args.values[0];

  if (cmd == "cc1101.info") {
    buildInfoPayload(result);
    serializeJson(resultPayload, stdoutText);
    success = true;
  } else if (cmd == "cc1101.set_freq") {
    if (args.count < 2) {
      exitCode = 2;
      stderrText = "usage: cc1101.set_freq <mhz>";
    } else {
      float mhz = 0.0f;
      if (!parseFloatToken(args.values[1], mhz)) {
        exitCode = 2;
        stderrText = "invalid frequency";
      } else {
        setCc1101FrequencyMhz(mhz);
        result["frequencyMhz"] = getCc1101FrequencyMhz();
        result["applied"] = true;
        serializeJson(resultPayload, stdoutText);
        success = true;
      }
    }
  } else if (cmd == "cc1101.tx") {
    if (args.count < 3) {
      exitCode = 2;
      stderrText = "usage: cc1101.tx <code> <bits> [pulseLength] [protocol] [repeat]";
    } else {
      uint64_t code64 = 0;
      int bits = 0;
      int pulseLength = 350;
      int protocol = 1;
      int repeat = 10;

      if (!parseUInt64Token(args.values[1], code64) || code64 > 0xFFFFFFFFULL) {
        exitCode = 2;
        stderrText = "code must be uint32";
      } else if (!parseIntToken(args.values[2], bits)) {
        exitCode = 2;
        stderrText = "invalid bits";
      } else if (args.count >= 4 && !parseIntToken(args.values[3], pulseLength)) {
        exitCode = 2;
        stderrText = "invalid pulseLength";
      } else if (args.count >= 5 && !parseIntToken(args.values[4], protocol)) {
        exitCode = 2;
        stderrText = "invalid protocol";
      } else if (args.count >= 6 && !parseIntToken(args.values[5], repeat)) {
        exitCode = 2;
        stderrText = "invalid repeat";
      } else {
        String txErr;
        if (!transmitCc1101(static_cast<uint32_t>(code64),
                            bits,
                            pulseLength,
                            protocol,
                            repeat,
                            txErr)) {
          exitCode = 1;
          stderrText = txErr;
        } else {
          result["sent"] = true;
          result["code"] = static_cast<uint32_t>(code64);
          result["bits"] = bits;
          result["pulseLength"] = pulseLength;
          result["protocol"] = protocol;
          result["repeat"] = repeat;
          result["frequencyMhz"] = getCc1101FrequencyMhz();
          serializeJson(resultPayload, stdoutText);
          success = true;
        }
      }
    }
  } else {
    exitCode = 127;
    stderrText = "unsupported command: " + cmd;
  }

  DynamicJsonDocument payload(3072);
  payload["exitCode"] = exitCode;
  payload["timedOut"] = false;
  payload["success"] = success;
  payload["stdout"] = stdoutText;
  payload["stderr"] = stderrText;
  if (success) {
    payload["error"] = nullptr;
  } else {
    payload["error"] = stderrText;
  }
  payload["truncated"] = false;
  if (resultPayload.size() > 0) {
    payload["result"] = resultPayload.as<JsonVariantConst>();
  }

  gateway_->sendInvokeOk(invokeId, nodeId, payload);
  return true;
}

bool NodeCommandHandler::handleCc1101Command(const String &invokeId,
                                             const String &nodeId,
                                             const String &command,
                                             JsonObjectConst params) {
  DynamicJsonDocument payload(1024);

  if (command == "cc1101.info") {
    buildInfoPayload(payload.to<JsonObject>());
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.set_freq") {
    float mhz = 0.0f;
    if (!readFloatFromJson(params["mhz"], mhz)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "mhz is required");
      return true;
    }

    setCc1101FrequencyMhz(mhz);
    payload["frequencyMhz"] = getCc1101FrequencyMhz();
    payload["applied"] = true;
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  if (command == "cc1101.tx") {
    uint64_t code64 = 0;
    uint32_t bits = 0;
    uint32_t pulseLength = 350;
    uint32_t protocol = 1;
    uint32_t repeat = 10;

    if (!readUInt64FromJson(params["code"], code64) || code64 > 0xFFFFFFFFULL) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "code must be uint32");
      return true;
    }
    if (!readUInt32FromJson(params["bits"], bits)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "bits is required");
      return true;
    }
    if (!params["pulseLength"].isNull() &&
        !readUInt32FromJson(params["pulseLength"], pulseLength)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "invalid pulseLength");
      return true;
    }
    if (!params["protocol"].isNull() &&
        !readUInt32FromJson(params["protocol"], protocol)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "invalid protocol");
      return true;
    }
    if (!params["repeat"].isNull() &&
        !readUInt32FromJson(params["repeat"], repeat)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "INVALID_REQUEST",
                                "invalid repeat");
      return true;
    }

    String txErr;
    if (!transmitCc1101(static_cast<uint32_t>(code64),
                        static_cast<int>(bits),
                        static_cast<int>(pulseLength),
                        static_cast<int>(protocol),
                        static_cast<int>(repeat),
                        txErr)) {
      gateway_->sendInvokeError(invokeId,
                                nodeId,
                                "UNAVAILABLE",
                                txErr);
      return true;
    }

    payload["sent"] = true;
    payload["code"] = static_cast<uint32_t>(code64);
    payload["bits"] = bits;
    payload["pulseLength"] = pulseLength;
    payload["protocol"] = protocol;
    payload["repeat"] = repeat;
    payload["frequencyMhz"] = getCc1101FrequencyMhz();
    gateway_->sendInvokeOk(invokeId, nodeId, payload);
    return true;
  }

  gateway_->sendInvokeError(invokeId,
                            nodeId,
                            "UNAVAILABLE",
                            "unsupported cc1101 command");
  return true;
}
