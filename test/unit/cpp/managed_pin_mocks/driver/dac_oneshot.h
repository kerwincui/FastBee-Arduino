#pragma once
#include "gpio.h"
struct MockDacHandle { bool alive=false; uint8_t value=0; };
using dac_oneshot_handle_t = MockDacHandle*;
enum dac_channel_t { DAC_CHAN_0, DAC_CHAN_1 };
struct dac_oneshot_config_t { dac_channel_t chan_id=DAC_CHAN_0; };
esp_err_t dac_oneshot_new_channel(const dac_oneshot_config_t*,dac_oneshot_handle_t*);
esp_err_t dac_oneshot_output_voltage(dac_oneshot_handle_t,uint8_t);
esp_err_t dac_oneshot_del_channel(dac_oneshot_handle_t);
