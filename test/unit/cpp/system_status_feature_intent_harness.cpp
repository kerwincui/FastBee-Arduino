#include <Arduino.h>
#undef malloc
#undef free
#include <ArduinoJson.h>
#include "network/FixedJsonField.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
#include "serializer_trace_allocator.h"
#define private public
#include "core/RuntimeFeatureManager.h"
#include "core/FeatureBudget.h"
#undef private

namespace FastBee {
// Tests configure an already loaded registry. Lazy initialization would hide a
// fixture/setup error and must never substitute its defaults for these states.
void RuntimeFeatureManager::generateDefaults() { assert(false); }
void RuntimeFeatureManager::syncBudgetState() { assert(false); }
bool RuntimeFeatureManager::applyDependencyRules() { assert(false); return false; }
#include "system_status_feature_methods.inc"
}

struct RequestProbe { int status = 0; unsigned errors = 0; };
namespace HandlerUtils {
void sendJsonError(RequestProbe* request, int status, const char*) {
    request->status = status;
    ++request->errors;
}
}

static void serializeFeatureBlock(JsonDocument& wrapper, RequestProbe* request) {
    wrapper["success"] = true;
    JsonObject doc = wrapper["data"].to<JsonObject>();
#include "system_status_feature_block.inc"
    // Reaching here means the actual production overflow guard admitted the
    // document; HTTP transport/cache copying is outside this serializer test.
    request->status = 200;
}

static void configure(unsigned state, bool present) {
    assert(state < 8);
    auto& manager = FastBee::RuntimeFeatureManager::instance();
    auto& budget = FastBee::FeatureBudgetRegistry::instance();
    manager.loaded_ = true;
    manager.networkReady_ = true;
    manager.count_ = present ? 4 : 3;
    budget.deniedCount_ = 0;
    static const char* keys[] = {"webServer", "optionalOff", "dependency", "screenPanel"};
    for (size_t i = 0; i < manager.count_; ++i) {
        auto& feature = manager.features_[i];
        feature = {};
        feature.meta = {keys[i], keys[i], "system", 1, 16, true, true, nullptr, nullptr, 0, false};
        feature.userEnabled = i != 1;
        feature.budgetPassed = true;
    }
    if (!present) return;
    auto& panel = manager.features_[3];
    panel.meta.compiled = state != 1 && state != 3;
    panel.userEnabled = state != 2 && state != 3;
    if (state == 4) {
        panel.budgetPassed = false;
        budget.denied_[budget.deniedCount_++] = "screenPanel";
    }
    panel.runtimeDisabled = state == 5;
    if (state == 6) {
        panel.meta.dependsOn = "dependency";
        manager.features_[2].userEnabled = false;
    }
    if (state == 7) panel.budgetPassed = false;
}

static void verifyComplete(const std::string& bytes, unsigned state, bool present) {
    JsonDocument parsed;
    assert(!deserializeJson(parsed, bytes));
    assert(parsed["success"].is<bool>() && parsed["success"].as<bool>());
    JsonObjectConst data = parsed["data"].as<JsonObjectConst>();
    JsonObjectConst features = data["features"].as<JsonObjectConst>();
    assert(features.size() == (present ? 4U : 3U));
    assert(features["webServer"] == "active");
    assert(features["optionalOff"] == "disabled");
    assert(features["dependency"] == (present && state == 6 ? "disabled" : "active"));
    if (!present) {
        assert(data["featureIntent"].isNull());
        assert(features["screenPanel"].isNull());
        return;
    }
    static const char* states[] = {"active", "disabled", "disabled", "disabled",
                                   "no-memory", "no-memory", "disabled", "disabled"};
    assert(features["screenPanel"] == states[state]);
    JsonObjectConst intents = data["featureIntent"].as<JsonObjectConst>();
    assert(intents.size() == 1);
    JsonObjectConst panel = intents["screenPanel"].as<JsonObjectConst>();
    assert(panel.size() == 2);
    assert(panel["compiled"].is<bool>() && panel["userEnabled"].is<bool>());
    assert(panel["compiled"].as<bool>() == (state != 1 && state != 3));
    assert(panel["userEnabled"].as<bool>() == (state != 2 && state != 3));
}

static bool run(unsigned state, bool present, unsigned allocationLimit) {
    configure(state, present);
    SerializerTraceAllocator allocator(allocationLimit);
    bool complete = false;
    {
        JsonDocument wrapper(&allocator);
        RequestProbe request;
        serializeFeatureBlock(wrapper, &request);
        const bool rejectedAllocation = std::any_of(allocator.trace.begin(), allocator.trace.end(),
            [](const auto& entry) { return std::get<2>(entry); });
        if (request.status == 200) {
            assert(!wrapper.overflowed() && !rejectedAllocation && request.errors == 0);
            std::string bytes;
            serializeJson(wrapper, bytes);
            verifyComplete(bytes, state, present);
            complete = true;
        } else {
            // The extracted overflow branch clears the incomplete document
            // before rejecting it. Never validate that partial JSON as status.
            assert(request.status == 503 && request.errors == 1 && rejectedAllocation);
            assert(wrapper.size() == 0);
        }
    }
    assert(allocator.live.empty());
    return complete;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode(argv[1]);
    if (mode == "states") {
        for (unsigned state = 0; state < 8; ++state) assert(run(state, true, 256));
    } else if (mode == "absent") {
        assert(run(0, false, 256));
    } else if (mode == "allocation") {
        for (unsigned state = 0; state < 8; ++state) {
            bool sawFailure = false, sawSuccess = false;
            for (unsigned limit = 0; limit < 256; ++limit) {
                if (run(state, true, limit)) { sawSuccess = true; break; }
                sawFailure = true;
            }
            assert(sawFailure && sawSuccess);
        }
    } else assert(false);
}
