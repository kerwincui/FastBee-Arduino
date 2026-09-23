#pragma once

// PeripheralConfig only exposes JsonObject references in this focused test.
// The planner never serializes JSON, so the full ArduinoJson dependency is not
// needed by the host compiler harness.
class JsonObject {};
class JsonDocument {};
