#pragma once

#include "systems/RunningImageSize.h"
#include "network/FixedJsonField.h"
#include "network/handlers/HandlerUtils.h"
#include <cstdio>

namespace FastBee {

inline void writeRunningImageSizeFailure(JsonObject out, const RunningImageSizeResult& result) {
    setFixedJsonField(out, "success", false);
    setFixedJsonField(out, "error", "Running image size unavailable");
    setFixedJsonField(out, "errorCode", "RUNNING_IMAGE_SIZE_UNAVAILABLE");
    setFixedJsonField(out, "detail", runningImageSizeErrorCode(result.error));
    setFixedJsonField(out, "code", 503);
    setFixedJsonField(out, "espError", static_cast<int>(result.readError));
}

inline bool readRunningImageSizeForResponse(AsyncWebServerRequest* request, RunningImageSizeResult& result) {
    result = readRunningImageSize();
    if (result.known()) return true;
    char json[256];
    snprintf(json, sizeof(json),
             "{\"success\":false,\"error\":\"Running image size unavailable\","
             "\"errorCode\":\"RUNNING_IMAGE_SIZE_UNAVAILABLE\",\"detail\":\"%s\","
             "\"code\":503,\"espError\":%d}",
             runningImageSizeErrorCode(result.error), static_cast<int>(result.readError));
    AsyncWebServerResponse* response = request->beginResponse(503, "application/json", json);
    if (!response) {
        // The shared sendWithClose helper ignores null responses. Close this
        // failed request explicitly so response allocation cannot leave it hung.
        AsyncClient* client = request->client();
        if (client) client->close(true);
        return false;
    }
    HandlerUtils::sendWithClose(request, response);
    return false;
}

} // namespace FastBee
