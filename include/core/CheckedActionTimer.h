#pragma once

#include <stdint.h>
#include <esp_timer.h>

// Checked adapter for PeripheralManager::ActionTickerData. It does not
// replace SDK/Arduino Ticker globally. PM retains this object on any failure.
// Caller must hold PM's existing lock for create/start/stop/delete; callback
// receives a never-reused integer token and acquires PM zero-wait before I/O.
namespace FastBee {

class CheckedActionTimer {
public:
    CheckedActionTimer() = default;
    CheckedActionTimer(const CheckedActionTimer&) = delete;
    CheckedActionTimer& operator=(const CheckedActionTimer&) = delete;

    // Deliberately no implicit SDK cleanup: deleting a context whose timer is
    // retained is forbidden by its PM owner. A destructor cannot report failure.
    // Every ActionTickerData deletion/erase must check releaseConfirmed().
    ~CheckedActionTimer() = default;

    bool create(esp_timer_cb_t callback, uint32_t token) {
        if (_timer || _fault || !callback || !token) return false;
        esp_timer_create_args_t args{};
        args.callback = callback;
        args.arg = reinterpret_cast<void*>(static_cast<uintptr_t>(token));
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "fb_action";
        esp_timer_handle_t created = nullptr;
        const esp_err_t result = esp_timer_create(&args, &created);
        // Retain a non-null returned handle even on an unusual SDK failure.
        _timer = created;
        _lastError = result;
        _fault = result != ESP_OK || !_timer;
        return !_fault;
    }

    bool startPeriodic(uint32_t intervalMs) {
        if (!_timer || _fault || !intervalMs) return false;
        _lastError = esp_timer_start_periodic(
            _timer, static_cast<uint64_t>(intervalMs) * 1000ULL);
        _fault = _lastError != ESP_OK;
        return !_fault;
    }

    bool stopAndDelete() {
        if (!_timer) {
            // No handle was published; there is nothing to free. Fault history
            // is kept outside this adapter; do not erase PM failure counters.
            _fault = false;
            return true;
        }
        // With a valid exclusively owned handle, INVALID_STATE means already
        // stopped (documented esp_timer contract), not evidence of deletion.
        _lastError = esp_timer_stop(_timer);
        if (_lastError != ESP_OK && _lastError != ESP_ERR_INVALID_STATE) {
            _fault = true;
            return false;
        }
        _lastError = esp_timer_delete(_timer);
        if (_lastError != ESP_OK) {
            _fault = true;
            return false;
        }
        _timer = nullptr; // Only successful SDK delete releases ownership.
        _fault = false;
        return true;
    }

    bool releaseConfirmed() const { return !_timer && !_fault; }
    bool fault() const { return _fault; }
    esp_err_t lastError() const { return _lastError; }

private:
    esp_timer_handle_t _timer = nullptr;
    esp_err_t _lastError = ESP_OK;
    bool _fault = false;
};

} // namespace FastBee

