#include "systems/OtaSelfCheck.h"
#include "systems/RestartDiagnostics.h"
#include <esp_ota_ops.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define CHECK(value) do { if (!(value)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #value); std::exit(86); } } while (false)

TestSerial Serial;
TestEsp ESP;
uint32_t nowMs = 0;
esp_partition_t running{"ota_1"};
bool partitionAvailable = true;
esp_ota_img_states_t currentState = ESP_OTA_IMG_PENDING_VERIFY;
esp_err_t readResult = ESP_OK, validResult = ESP_OK, invalidResult = ESP_OK;
unsigned validCalls = 0, invalidCalls = 0, restarts = 0, snapshots = 0;
std::vector<std::string> events;
std::string snapshotDetails;

unsigned long millis() { return nowMs; }
void delay(unsigned long milliseconds) { nowMs += static_cast<uint32_t>(milliseconds); }
void TestEsp::restart() { ++restarts; events.emplace_back("restart"); }
const esp_partition_t* esp_ota_get_running_partition() { return partitionAvailable ? &running : nullptr; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition, esp_ota_img_states_t* state) {
    CHECK(partition == &running);
    if (readResult == ESP_OK) *state = currentState;
    return readResult;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    ++validCalls; events.emplace_back("valid");
    if (validResult == ESP_OK) currentState = ESP_OTA_IMG_VALID;
    return validResult;
}
esp_err_t esp_ota_mark_app_invalid_rollback() {
    ++invalidCalls; events.emplace_back("invalid");
    CHECK(snapshots == 1); // Evidence must exist before any SDK mutation/reset.
    if (invalidResult == ESP_OK) currentState = ESP_OTA_IMG_INVALID;
    return invalidResult;
}
void RestartDiagnostics::savePreRestartState(RestartReason reason, const char* details) {
    CHECK(reason == RestartReason::OTA_UPDATE && details != nullptr);
    ++snapshots; events.emplace_back("snapshot"); snapshotDetails = details;
}

void tick(uint32_t at) { nowMs = at; OtaSelfCheck::handle(); }
void quietAfter(uint32_t at) {
    const unsigned before = validCalls;
    for (unsigned i = 0; i < 20; ++i) tick(at + i * 60000U);
    CHECK(validCalls == before);
}
void confirmBoundary(uint32_t start) {
    nowMs = start; OtaSelfCheck::begin(); CHECK(OtaSelfCheck::isPending());
    tick(start + 89999U); CHECK(validCalls == 0 && OtaSelfCheck::isPending());
    tick(start + 90000U); CHECK(validCalls == 1 && !OtaSelfCheck::isPending());
    quietAfter(start + 90001U);
    CHECK(invalidCalls == 0 && restarts == 0);
}
void failedConfirmation(uint32_t start) {
    validResult = ESP_FAIL;
    nowMs = start; OtaSelfCheck::begin();
    tick(start + 90000U); CHECK(validCalls == 1 && OtaSelfCheck::isPending());
    tick(start + 94999U); CHECK(validCalls == 1);
    tick(start + 95000U); CHECK(validCalls == 2 && OtaSelfCheck::isPending());
    tick(start + 99999U); CHECK(validCalls == 2);
    tick(start + 100000U); CHECK(validCalls == 3 && OtaSelfCheck::isPending());
    quietAfter(start + 100001U);
    OtaSelfCheck::begin(); quietAfter(start + 2000000U);
    CHECK(validCalls == 3 && restarts == 0 && OtaSelfCheck::isPending());
}
void rollbackSuccess(bool beginFirst = true) {
    if (beginFirst) OtaSelfCheck::begin();
    OtaSelfCheck::markInvalidAndRollback("init fault");
    CHECK(invalidCalls == 1 && snapshots == 1 && restarts == 1);
    CHECK(snapshotDetails == "init fault");
    CHECK(events == std::vector<std::string>({"snapshot", "invalid", "restart"}));
    CHECK(!OtaSelfCheck::isPending());
    currentState = ESP_OTA_IMG_PENDING_VERIFY; // Repeated begin cannot re-arm an attempted rollback.
    OtaSelfCheck::begin();
    OtaSelfCheck::markInvalidAndRollback("repeat");
    quietAfter(100000U);
    CHECK(invalidCalls == 1 && snapshots == 1 && restarts == 1 && validCalls == 0);
}

int main(int argc, char** argv) {
    CHECK(argc == 2);
    const std::string name = argv[1];
    if (name == "new_confirm") { currentState = ESP_OTA_IMG_NEW; confirmBoundary(10); }
    else if (name == "pending_confirm") confirmBoundary(10);
    else if (name == "window_wrap") confirmBoundary(UINT32_MAX - 500U);
    else if (name == "confirmation_failure_limit") failedConfirmation(0);
    else if (name == "retry_wrap") failedConfirmation(UINT32_MAX - 91000U);
    else if (name == "confirmation_retry_success") {
        validResult = ESP_FAIL; OtaSelfCheck::begin(); tick(90000);
        CHECK(validCalls == 1 && OtaSelfCheck::isPending());
        validResult = ESP_OK; tick(94999); CHECK(validCalls == 1);
        tick(95000); CHECK(validCalls == 2 && !OtaSelfCheck::isPending());
        quietAfter(100000); CHECK(restarts == 0);
    } else if (name == "repeat_begin") {
        nowMs = 100; OtaSelfCheck::begin();
        nowMs = 50000; OtaSelfCheck::begin();
        tick(90099); CHECK(validCalls == 0);
        tick(90100); CHECK(validCalls == 1 && !OtaSelfCheck::isPending());
        nowMs = 100000; OtaSelfCheck::begin(); quietAfter(190000);
    } else if (name == "rollback_new") { currentState = ESP_OTA_IMG_NEW; rollbackSuccess(); }
    else if (name == "rollback_pending") rollbackSuccess();
    else if (name == "rollback_before_begin") rollbackSuccess(false);
    else if (name == "rollback_failure") {
        OtaSelfCheck::begin(); invalidResult = ESP_FAIL;
        OtaSelfCheck::markInvalidAndRollback(nullptr);
        CHECK(invalidCalls == 1 && snapshots == 1 && restarts == 0 && OtaSelfCheck::isPending());
        CHECK(!snapshotDetails.empty());
        OtaSelfCheck::begin(); OtaSelfCheck::markInvalidAndRollback("repeat"); quietAfter(90000);
        CHECK(invalidCalls == 1 && snapshots == 1 && validCalls == 0 && restarts == 0);
    } else if (name == "rollback_state_error" || name == "rollback_no_partition") {
        OtaSelfCheck::begin();
        if (name == "rollback_state_error") readResult = ESP_FAIL;
        else partitionAvailable = false;
        OtaSelfCheck::markInvalidAndRollback("cannot observe");
        readResult = ESP_OK; partitionAvailable = true;
        OtaSelfCheck::begin(); OtaSelfCheck::markInvalidAndRollback("repeat"); quietAfter(90000);
        CHECK(invalidCalls == 0 && snapshots == 0 && validCalls == 0 && restarts == 0);
        CHECK(OtaSelfCheck::isPending());
    } else if (name == "rollback_fresh_state") {
        OtaSelfCheck::begin(); currentState = ESP_OTA_IMG_VALID;
        OtaSelfCheck::markInvalidAndRollback("already valid"); quietAfter(90000);
        CHECK(!OtaSelfCheck::isPending() && invalidCalls == 0 && snapshots == 0 && restarts == 0 && validCalls == 0);
    } else if (name == "rollback_after_confirmed") {
        confirmBoundary(0); OtaSelfCheck::markInvalidAndRollback("late failure");
        CHECK(invalidCalls == 0 && snapshots == 0 && restarts == 0);
    } else if (name == "rollback_after_confirmation_exhausted") {
        failedConfirmation(0); OtaSelfCheck::markInvalidAndRollback("fatal after confirmation failed");
        CHECK(invalidCalls == 1 && snapshots == 1 && restarts == 1 && validCalls == 3);
    } else {
        if (name == "no_partition") partitionAvailable = false;
        else if (name == "state_error") readResult = ESP_FAIL;
        else if (name == "valid_image") currentState = ESP_OTA_IMG_VALID;
        else if (name == "invalid_image") currentState = ESP_OTA_IMG_INVALID;
        else if (name == "aborted_image") currentState = ESP_OTA_IMG_ABORTED;
        else if (name == "undefined_image") currentState = ESP_OTA_IMG_UNDEFINED;
        else CHECK(false);
        OtaSelfCheck::begin(); CHECK(!OtaSelfCheck::isPending()); quietAfter(90000);
        OtaSelfCheck::markInvalidAndRollback("nonpending or unknown");
        CHECK(validCalls == 0 && invalidCalls == 0 && snapshots == 0 && restarts == 0);
    }
}
