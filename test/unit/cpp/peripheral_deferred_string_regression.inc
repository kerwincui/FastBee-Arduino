static std::vector<std::pair<std::string, std::string>> deferredTextFor(PeripheralType type, unsigned variant) {
    if (type == PeripheralType::SCREEN_DISPLAY) return {
        {"backend", "rgb"}, {"driver", "st7701"}, {"touchBackend", "gt911"},
        {"rgbInitProfile", "panel-profile"}, {"controlBackend", "ch32v003"}};
    if (type == PeripheralType::SENSOR_GENERIC) return {
        {"driver", variant & 1U ? "MPU6050" : "ADS1115"}, {"busPeripheralId", "sensor-bus"}};
    return {{"busPeripheralId", "bus"}, {"oePeripheralId", "oe"}};
}

static std::string drainDeferredResponse(FastBee::JsonDocumentResponse& response) {
    CHECK(response._sourceValid());
    std::string actual;
    uint8_t buffer[127];
    const size_t capacities[] = {1, 7, 127, 13, 31};
    size_t iteration = 0;
    while (response._sentLength < response._contentLength) {
        const size_t capacity = capacities[iteration++ % 5];
        const size_t copied = response._fillBuffer(buffer, capacity);
        CHECK(copied > 0 && copied <= capacity);
        actual.append(reinterpret_cast<const char*>(buffer), copied);
        response._sentLength += copied;
    }
    CHECK(actual.size() == response._contentLength);
    return actual;
}

static void deferredStringCases() {
    const PeripheralType types[] = {PeripheralType::SCREEN_DISPLAY,
        PeripheralType::SENSOR_GENERIC, PeripheralType::PCA9685_PWM};
    for (auto type : types) {
        scenario = 0;
        activeCase = "legacy-response-borrows-live-config/" + std::to_string(static_cast<int>(type));
        evaluate(0, TraceAllocator::never, false, [=] {
            auto config = configFor(type, 0);
            TrackedDocument doc;
            JsonObject params = doc["params"].to<JsonObject>();
            if (type == PeripheralType::SCREEN_DISPLAY) Legacy::addDisplayParams(params, config);
            else if (type == PeripheralType::SENSOR_GENERIC) LegacySupport::serializeSensorConfig(params, config);
            else LegacySupport::serializePca9685Config(params, config);
            for (const auto& text : deferredTextFor(type, 0)) {
                const JsonString original = params[text.first.c_str()].as<JsonString>();
                CHECK(original.isStatic());
                CHECK(original.size() == text.second.size());
                CHECK(std::strcmp(original.c_str(), text.second.c_str()) == 0);
            }
            const std::string expected = capture(doc).bytes;
            FastBee::JsonDocumentResponse response(std::move(doc));
            // Mutate equal-length text while storage is still alive. This
            // proves delayed transport reads the borrowed storage without UB.
            if (type == PeripheralType::SCREEN_DISPLAY) config.params.display.driver[0] = 'X';
            else if (type == PeripheralType::SENSOR_GENERIC) config.params.sensor.driver[0] = 'X';
            else config.params.pca9685.busPeripheralId[0] = 'X';
            CHECK(drainDeferredResponse(response) != expected);
        });

        for (unsigned variant : {0U, 7U}) {
            const auto callback = [=] {
                deferredExpectedText = deferredTextFor(type, variant);
                deferJsonSend = true;
                std::string expected;
                {
                    // Actual detail response suffix, with the same local
                    // snapshot lifetime as handleGetPeripheral. It moves its
                    // document into the real delayed response at send time.
                    auto config = configFor(type, variant);
                    PeripheralManager manager; fillRuntime(manager, type);
                    INVOKE(detailResponse, &request, &config, config.id, manager);
                    CHECK(pendingJsonResponse != nullptr);
                    CHECK(request.status == 200 && request.sends == 1);
                    CHECK(observations.size() == 1);
                    expected = observations.front().bytes;
                }
                // Both configuration and manager are now destroyed. The fake
                // send boundary asserted all nine applicable values are owned
                // before moving; fragmented reads below use the real class.
                CHECK(pendingJsonResponse->_contentLength == expected.size());
                request.body = drainDeferredResponse(*pendingJsonResponse);
                CHECK(request.body == expected);
                delete pendingJsonResponse; pendingJsonResponse = nullptr;
                deferJsonSend = false; deferredExpectedText.clear();
            };
            scenario = variant;
            activeCase = "deferred-detail-after-local-return/" + std::to_string(static_cast<int>(type)) + '/' + std::to_string(variant);
            const Outcome expected = evaluate(0, TraceAllocator::never, false, callback);
            CHECK(expected == evaluate(1, TraceAllocator::never, false, callback));
            CHECK(expected == evaluate(2, TraceAllocator::never, false, callback));
        }
    }
    // All pre-existing config/status/ownership comparisons still inject each
    // persistent and one-shot allocator failure, now against explicit corrected
    // display/support references. No transport/admission success is fabricated.
}
