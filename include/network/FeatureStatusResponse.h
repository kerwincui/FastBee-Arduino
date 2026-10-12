#pragma once

#include "network/FeatureStatusCatalog.h"
#include <ESPAsyncWebServer.h>

namespace FastBee {

class FeatureStatusResponse final : public AsyncAbstractResponse {
public:
    explicit FeatureStatusResponse(const RuntimeFeatureManager& manager) : catalog_(manager) {
        _code = 200;
        _contentType = "application/json";
        _contentLength = catalog_.measureLength();
        _sendContentLength = true;
    }
    bool _sourceValid() const override { return catalog_.valid() && _contentLength > 0; }
    size_t _fillBuffer(uint8_t* destination, size_t capacity) override {
        return catalog_.read(destination, capacity);
    }
private:
    FeatureStatusCatalog catalog_;
};

} // namespace FastBee
