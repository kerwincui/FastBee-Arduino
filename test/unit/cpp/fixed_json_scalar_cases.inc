// Independent reference uses ArduinoJson's original proxies at all six
// document/object depths; compare exact allocator calls and conversion results.
template<class Value>
static void scalarPut(Pair& pair, unsigned layout, Value value) {
    bool was = false, now = false;
    switch (layout) {
        case 0:
            was = pair.expected["field"].set(value);
            now = FastBee::setFixedJsonField(pair.actual, "field", value); break;
        case 1:
            was = pair.expected["field"]["child"].set(value);
            now = FastBee::setFixedJsonField(pair.actual, "field", "child", value); break;
        case 2:
            was = pair.expected["field"]["child"]["leaf"].set(value);
            now = FastBee::setFixedJsonField(pair.actual, "field", "child", "leaf", value); break;
        case 3:
            was = pair.expected.as<JsonObject>()["field"].set(value);
            now = FastBee::setFixedJsonField(pair.actual.as<JsonObject>(), "field", value); break;
        case 4:
            was = pair.expected.as<JsonObject>()["field"]["child"].set(value);
            now = FastBee::setFixedJsonField(pair.actual.as<JsonObject>(), "field", "child", value); break;
        case 5:
            was = pair.expected.as<JsonObject>()["field"]["child"]["leaf"].set(value);
            now = FastBee::setFixedJsonField(pair.actual.as<JsonObject>(), "field", "child", "leaf", value); break;
        default: assert(false);
    }
    assert(was == now);
    pair.check();
}

template<class T>
static void scalarLimits(Pair& pair, unsigned layout) {
    for (T value : {std::numeric_limits<T>::min(), T(0), T(1), std::numeric_limits<T>::max()})
        scalarPut(pair, layout, value);
}

static void scalarValues(Pair& pair, unsigned layout) {
    scalarPut(pair, layout, true); scalarPut(pair, layout, false);
    scalarLimits<signed char>(pair, layout); scalarLimits<unsigned char>(pair, layout);
    scalarLimits<short>(pair, layout); scalarLimits<unsigned short>(pair, layout);
    scalarLimits<int>(pair, layout); scalarLimits<unsigned int>(pair, layout);
    scalarLimits<long>(pair, layout); scalarLimits<unsigned long>(pair, layout);
    scalarLimits<long long>(pair, layout); scalarLimits<unsigned long long>(pair, layout);
    for (int64_t value : {INT64_C(-2147483649), INT64_C(-2147483648), INT64_C(2147483647), INT64_C(2147483648)})
        scalarPut(pair, layout, value);
    for (uint64_t value : {UINT64_C(4294967295), UINT64_C(4294967296), UINT64_MAX})
        scalarPut(pair, layout, value);
    for (float value : {0.0f, -0.0f, 0.125f, std::numeric_limits<float>::min(),
            std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::quiet_NaN()}) scalarPut(pair, layout, value);
    for (double value : {0.0, -0.0, 0.125, 0.10000000000000001,
            std::numeric_limits<double>::min(), std::numeric_limits<double>::denorm_min(),
            std::numeric_limits<double>::max(), std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        scalarPut(pair, layout, value);
    scalarPut(pair, layout, nullptr);
}

static void checkScalarWidths(bool faults) {
    size_t exercisedFaults = 0;
    for (unsigned layout = 0; layout < 6; ++layout) {
        for (size_t root = 0; root < sizeof(Roots) / sizeof(*Roots); ++root) {
            size_t allocations;
            {
                Pair pair; pair.seed(root); scalarValues(pair, layout);
                assert(!pair.actual.overflowed()); allocations = pair.before.eligible;
            }
            if (!faults) continue;
            for (bool persistent : {false, true}) for (size_t fault = 0; fault < allocations; ++fault) {
                Pair pair(fault, persistent); pair.seed(root); scalarValues(pair, layout);
                assert(pair.before.failures); ++exercisedFaults;
            }
        }
    }
    if (faults) assert(exercisedFaults > 0);
}
