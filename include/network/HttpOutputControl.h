#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/NativeOutputControl.h"
class AsyncWebServerRequest;
class WebHandlerContext;

namespace FastBee {
void writeOrdinaryControlJson(JsonObject out, const OrdinaryControlResult& result);
// Call only after the route's auth/developer checks. This helper revalidates
// the actual session and never accepts actor/source/operation from the client.
bool beginHttpRuleOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                         uint32_t expectedEpoch, OrdinaryControlContext& context);
bool beginHttpPcaOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                       const String& id, uint32_t expectedEpoch, OrdinaryControlContext& context);
bool beginHttpModbusRelayOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
    const String& canonicalTarget, uint32_t expectedEpoch, OrdinaryControlContext& context);
bool beginHttpMotorOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
    const String& canonicalTarget, uint32_t expectedEpoch, OrdinaryControlContext& context);
void handleHttpNativeDigital(WebHandlerContext& web, AsyncWebServerRequest* request,
                             const String& id, uint32_t expectedEpoch);
void handleHttpServoOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                           const String& id, uint32_t expectedEpoch);
void handleHttpNativeScalar(WebHandlerContext& web, AsyncWebServerRequest* request,
                            const String& id, uint32_t expectedEpoch);
void handleHttpRfOutput(WebHandlerContext& web, AsyncWebServerRequest* request,
                        const String& id, uint32_t expectedEpoch);
} // namespace FastBee
