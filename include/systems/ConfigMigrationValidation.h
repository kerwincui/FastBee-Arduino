#pragma once
#include "systems/ConfigMigrationStorage.h"
#include "core/PeripheralResourcePlanner.h"
#include <ArduinoJson.h>

namespace FastBee { namespace ConfigMigration {
struct ReviewContext {
    PreflightContext resources;
    JsonDocument* report = nullptr;
    bool requireAcknowledgements = false;
};
// Uses the real persisted parsers and action validators; no driver is created,
// no live configuration is replaced and no peripheral action is executed.
bool validateGraph(const Manifest& manifest, void* context, String& error);
} }
