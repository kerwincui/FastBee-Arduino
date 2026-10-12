#pragma once
enum class RestartReason { OTA_UPDATE };
struct RestartDiagnostics {
    static void savePreRestartState(RestartReason reason, const char* details);
};
