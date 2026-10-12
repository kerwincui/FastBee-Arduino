#pragma once
#include <driver/gpio.h>
#include <driver/dac_oneshot.h>
#include <esp32-hal-periman.h>
#include <array>
#include <cassert>
enum class Op { Config,Level,Read,Install,Type,Add,Remove,Disable,DacNew,DacWrite,DacDelete,Periman,Count };
static std::array<unsigned,static_cast<size_t>(Op::Count)> calls{};
static Op failure=Op::Count;
static unsigned failureCall=0;
static bool partialAllocation=false, serviceInstalled=false;
static int levels[64]{};
static bool handlers[64]{};
static void (*handlerFunctions[64])(void*){};
static void* handlerArguments[64]{};
static peripheral_bus_type_t buses[64]{};
static gpio_config_t lastConfig{};
static MockDacHandle dacHandles[2]{};
static unsigned count(Op op) { return calls[static_cast<size_t>(op)]; }
static unsigned allCalls() { unsigned value=0;for(auto item:calls)value+=item;return value; }
static bool fail(Op op) { ++calls[static_cast<size_t>(op)];return failure==op && (!failureCall || count(op)==failureCall); }
peripheral_bus_type_t perimanGetPinBusType(uint8_t pin) {
    fail(Op::Periman); assert(pin<64); return buses[pin];
}
esp_err_t gpio_config(const gpio_config_t* config) {
    lastConfig=*config;return fail(Op::Config)?ESP_FAIL:ESP_OK;
}
esp_err_t gpio_set_level(gpio_num_t pin,uint32_t value) {
    assert(pin>=0 && pin<64);levels[pin]=static_cast<int>(value);
    return fail(Op::Level)?ESP_FAIL:ESP_OK;
}
int gpio_get_level(gpio_num_t pin) { fail(Op::Read);return levels[pin]; }
esp_err_t gpio_install_isr_service(int) {
    if(fail(Op::Install))return ESP_FAIL;
    if(serviceInstalled)return ESP_ERR_INVALID_STATE;
    serviceInstalled=true;return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t,gpio_int_type_t) { return fail(Op::Type)?ESP_FAIL:ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin,void(*handler)(void*),void* argument) {
    handlerFunctions[pin]=handler;handlerArguments[pin]=argument;
    handlers[pin]=true;return fail(Op::Add)?ESP_FAIL:ESP_OK; // An error may have partial effects.
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) {
    if(fail(Op::Remove))return ESP_FAIL;
    handlers[pin]=false;handlerFunctions[pin]=nullptr;handlerArguments[pin]=nullptr;return ESP_OK;
}
esp_err_t gpio_intr_disable(gpio_num_t) { return fail(Op::Disable)?ESP_FAIL:ESP_OK; }
esp_err_t dac_oneshot_new_channel(const dac_oneshot_config_t* config,dac_oneshot_handle_t* handle) {
    const bool bad=fail(Op::DacNew);
    if(!bad || partialAllocation) {
        *handle=&dacHandles[config->chan_id];(*handle)->alive=true;
    }
    return bad?ESP_FAIL:ESP_OK;
}
esp_err_t dac_oneshot_output_voltage(dac_oneshot_handle_t handle,uint8_t value) {
    assert(handle && handle->alive);handle->value=value;
    return fail(Op::DacWrite)?ESP_FAIL:ESP_OK;
}
esp_err_t dac_oneshot_del_channel(dac_oneshot_handle_t handle) {
    assert(handle && handle->alive);if(fail(Op::DacDelete))return ESP_FAIL;
    handle->alive=false;return ESP_OK;
}
