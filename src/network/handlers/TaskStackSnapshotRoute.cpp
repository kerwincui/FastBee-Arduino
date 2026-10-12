#include "network/handlers/TaskStackSnapshotRoute.h"
#include "network/TaskStackSnapshotJson.h"
#include "network/WebHandlerContext.h"
#include "network/handlers/HandlerUtils.h"
#include "core/FastBeeFramework.h"
#include "systems/HealthMonitor.h"

namespace FastBee {
void setupTaskStackSnapshotRoute(AsyncWebServer* server, WebHandlerContext* context) {
    if (!server || !context) return;
    server->on("/api/system/task-stack-snapshot", HTTP_POST,
        [context](AsyncWebServerRequest* request) {
            if (!context->requireAuth(request)) return;
            if (!context->requireDeveloperMode(request)) return;
            bool requestIpcIdentity = true;
            if (request->params() > 1) {
                context->sendError(request, 400, "Invalid task snapshot parameters");
                return;
            }
            if (request->params() == 1) {
                const auto* mode = request->getParam(size_t{0});
                if (!mode || mode->isFile() || mode->name() != "ipcMode" || mode->value() != "observe-only") {
                    context->sendError(request, 400, "Invalid task snapshot ipcMode");
                    return;
                }
                requestIpcIdentity = false;
            }
            auto* framework = FastBeeFramework::getInstance();
            auto* monitor = framework ? framework->getHealthMonitor() : nullptr;
            // No PSRAM exception: this debugging call may suspend scheduling
            // and uses an internal-DRAM SDK scratch array.
            auto sample = captureTaskStackSnapshot(!monitor || monitor->isMemorySevere(), requestIpcIdentity);
            auto doc = makeJsonDocument(8192);
            doc["success"] = sample.status == TaskStackCaptureStatus::Observed;
            writeTaskStackSnapshotJson(doc["data"].to<JsonObject>(), sample);
            // Retire the fixed scalar copy before allocating response storage.
            sample.entries.reset();
            if (doc.overflowed() || measureJson(doc) > 8192) {
                HandlerUtils::sendJsonError(request, 503, "Task snapshot response allocation failed", 30);
                return;
            }
            // An unavailable diagnostic has an explicit unknown payload.
            // Auth and developer denial retain their existing HTTP errors.
            HandlerUtils::sendJsonStream(request, doc);
        });
}
} // namespace FastBee
