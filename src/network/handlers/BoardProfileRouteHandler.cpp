#include "network/handlers/PeripheralRouteHandler.h"
#include "network/handlers/HandlerUtils.h"
#include "network/WebHandlerContext.h"
#include "network/BoardProfileCatalog.h"
#include "core/ChipConfig.h"
#include <new>

namespace {
class BoardProfilesResponse final : public AsyncAbstractResponse {
public:
    explicit BoardProfilesResponse(const FastBee::BoardProfile* active)
        : catalog_(active, CHIP_NAME) {
        _code = 200;
        _contentType = "application/json";
        _contentLength = catalog_.measureLength();
        _sendContentLength = true;
    }
    bool _sourceValid() const override { return catalog_.valid() && _contentLength != 0; }
    size_t _fillBuffer(uint8_t* destination, size_t capacity) override {
        return catalog_.read(destination, capacity);
    }
private:
    FastBee::BoardProfileCatalog catalog_;
};
} // namespace

void PeripheralRouteHandler::handleGetBoardProfiles(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    const FastBee::BoardProfile* active = nullptr;
#if CHIP_IS_CLASSIC_ESP32 || CHIP_IS_ESP32C3 || CHIP_IS_ESP32C6 || CHIP_IS_ESP32S3
    active = &FastBee::getCompiledBoardProfile();
#endif
    auto* response = new (std::nothrow) BoardProfilesResponse(active);
    if (!response || !response->_sourceValid()) {
        delete response;
        HandlerUtils::sendJsonError(request, 503, "Board profile response unavailable", 5);
        return;
    }
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}
