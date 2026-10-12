#pragma once
#include <cstdint>
using esp_err_t = int;
constexpr esp_err_t ESP_OK=0, ESP_FAIL=-1, ESP_ERR_INVALID_STATE=0x103;
using gpio_num_t = int;
enum gpio_mode_t { GPIO_MODE_INPUT=1, GPIO_MODE_OUTPUT=2 };
enum gpio_pullup_t { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE };
enum gpio_pulldown_t { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE };
enum gpio_int_type_t { GPIO_INTR_DISABLE, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE };
struct gpio_config_t {
    uint64_t pin_bit_mask=0;
    gpio_mode_t mode=GPIO_MODE_INPUT;
    gpio_pullup_t pull_up_en=GPIO_PULLUP_DISABLE;
    gpio_pulldown_t pull_down_en=GPIO_PULLDOWN_DISABLE;
    gpio_int_type_t intr_type=GPIO_INTR_DISABLE;
};
esp_err_t gpio_config(const gpio_config_t*);
esp_err_t gpio_set_level(gpio_num_t,uint32_t);
int gpio_get_level(gpio_num_t);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_set_intr_type(gpio_num_t,gpio_int_type_t);
esp_err_t gpio_isr_handler_add(gpio_num_t,void(*)(void*),void*);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
esp_err_t gpio_intr_disable(gpio_num_t);
