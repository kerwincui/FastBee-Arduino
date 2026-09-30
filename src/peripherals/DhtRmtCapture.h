#pragma once
// ESP32 hardware capture for DHT frames. Caller holds the SensorDriver bus mutex.
// RX buffers have static lifetime: Arduino's async RMT callback can outlive a
// timeout. A failed channel teardown quarantines this buffer until teardown works.
static bool decodeDhtRmtSymbols(const rmt_data_t* symbols, size_t count,
                                uint8_t (&data)[5], DhtReadDiagnostic* diagnostic) {
    auto level = [&](size_t n) -> unsigned { return (n & 1) ? symbols[n / 2].level1 : symbols[n / 2].level0; };
    auto duration = [&](size_t n) -> unsigned { return (n & 1) ? symbols[n / 2].duration1 : symbols[n / 2].duration0; };
    auto fail = [&](const char* phase, int bit = -1, unsigned low = 0, unsigned high = 0) {
        if (diagnostic) { diagnostic->phase=phase; diagnostic->bit=bit; diagnostic->lowUs=low; diagnostic->highUs=high; }
        return false;
    };
    if (count == 0 || count > 48) return fail("rmt_symbol_count");
    size_t preamble = count * 2;
    // At most host-low + release-high precede the sensor's low/high response.
    // Search only that prefix, never an arbitrary later data bit.
    for (size_t n = 0; n + 1 < count * 2 && n < 4; ++n) {
        if (level(n)==0 && level(n+1)==1 && duration(n)>=60 && duration(n)<=120 &&
            duration(n+1)>=60 && duration(n+1)<=120) { preamble=n; break; }
    }
    if (preamble == count * 2) return fail("rmt_preamble");
    if (preamble + 82 > count * 2) return fail("rmt_short_frame");
    for (unsigned n=0;n<5;++n) data[n]=0;
    for (unsigned bit=0;bit<40;++bit) {
        const size_t n=preamble+2+bit*2;
        const unsigned low=duration(n), high=duration(n+1);
        if (level(n)!=0 || level(n+1)!=1 || low==0 || high==0 || low>500 || high>500)
            return fail("rmt_bit_pulse",bit,low,high);
        data[bit/8]=static_cast<uint8_t>((data[bit/8]<<1) | (high>low ? 1 : 0));
    }
    return true;
}

static bool readDhtRmtFrame(uint8_t pin, bool dht11, uint8_t (&data)[5],
                            DhtReadDiagnostic* diagnostic) {
    static rmt_data_t symbols[48];
    static size_t count=0;
    static int retainedPin=-1;
    const uint32_t started=micros();
    auto fail = [&](const char* phase) {
        if (diagnostic) {
            diagnostic->phase=phase;
            diagnostic->elapsedUs=static_cast<uint32_t>(micros()-started);
            diagnostic->finalLevel=gpio_get_level(static_cast<gpio_num_t>(pin));
        }
        return false;
    };
    if (retainedPin>=0) {
        if (!rmtDeinit(retainedPin)) return fail("rmt_cleanup");
        pinMode(retainedPin,INPUT_PULLUP);
        retainedPin=-1;
    }
    pinMode(pin,INPUT_PULLUP);
    if (digitalRead(pin)!=HIGH) return fail("idle_low");
    if (!rmtInit(pin,RMT_RX_MODE,RMT_MEM_NUM_BLOCKS_1,1000000)) return fail("rmt_init");
    retainedPin=pin;
    auto cleanup = [&]() {
        gpio_set_level(static_cast<gpio_num_t>(pin),1); // open-drain release
        if (!rmtDeinit(pin)) return false;
        retainedPin=-1;
        pinMode(pin,INPUT_PULLUP);
        return true;
    };
    auto abortCapture = [&](const char* phase) {
        const bool released=cleanup();
        return fail(released ? phase : "rmt_cleanup");
    };
    if (!rmtSetRxMaxThreshold(pin,10000) || !rmtSetRxMinThreshold(pin,1) ||
        gpio_set_direction(static_cast<gpio_num_t>(pin),GPIO_MODE_INPUT_OUTPUT_OD)!=ESP_OK ||
        gpio_pullup_en(static_cast<gpio_num_t>(pin))!=ESP_OK)
        return abortCapture("rmt_config");
    // Do not call Arduino pinMode/digitalWrite with an active RMT pin: its
    // peripheral manager would detach the receiver. GPIO calls retain RX routing.
    gpio_set_level(static_cast<gpio_num_t>(pin),0);
    delay(dht11 ? 20 : 2);
    count=sizeof(symbols)/sizeof(symbols[0]);
    if (!rmtReadAsync(pin,symbols,&count)) return abortCapture("rmt_arm");
    // RX starts while the host still holds low. A descheduled task can outlive
    // the RX idle threshold before releasing the sensor, leaving only a host-low
    // symbol. Rearm once while low, restoring the capacity changed by the callback.
    // Repeated expiry remains a bounded failure; do not hide it with a retry loop.
    if (rmtReceiveCompleted(pin)) {
        count=sizeof(symbols)/sizeof(symbols[0]);
        if (!rmtReadAsync(pin,symbols,&count)) return abortCapture("rmt_arm");
        if (rmtReceiveCompleted(pin)) return abortCapture("rmt_arm_expired");
    }
    gpio_set_level(static_cast<gpio_num_t>(pin),1);
    const uint32_t waitStarted=millis();
    bool ready=rmtReceiveCompleted(pin);
    while (!ready && static_cast<uint32_t>(millis()-waitStarted)<40) {
        delay(1); // Yield to Wi-Fi/RTOS; pulses are captured in hardware.
        ready=rmtReceiveCompleted(pin);
    }
    if (!ready) return abortCapture("rmt_timeout");
    if (!cleanup()) return fail("rmt_cleanup");
    if (!decodeDhtRmtSymbols(symbols,count,data,diagnostic)) {
        if (diagnostic) {
            diagnostic->elapsedUs=static_cast<uint32_t>(micros()-started);
            diagnostic->finalLevel=digitalRead(pin);
        }
        return false;
    }
    return true;
}
