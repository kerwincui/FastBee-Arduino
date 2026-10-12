// The unmodified original helper is a negative control: demonstrate borrowing
// while source storage is still alive, so reproduction never reads dead stack.
static void checkOwnedMcpText(JsonVariantConst field, const char* expected) {
    CHECK(field.is<const char*>());
    const JsonString text = field.as<JsonString>();
    CHECK(!text.isStatic()); // fail before reading a potentially dangling value
    CHECK(text.size() == std::strlen(expected));
    CHECK(std::strcmp(text.c_str(), expected) == 0);
}

static void mcpOwnershipCases() {
    scenario = 0;
    activeCase = "legacy-mcp-array-borrow-and-dependency-reuse";
    evaluate(0, TraceAllocator::never, false, [] {
        auto parent = configFor(PeripheralType::IO_EXPANDER, 0);
        auto dependency = configFor(PeripheralType::GPIO_DIGITAL_OUTPUT, 0);
        TrackedDocument doc;
        Legacy::addGenericPeripheralFields(doc["provider"].to<JsonObject>(), parent);
        Legacy::addGenericPeripheralFields(doc["reset"].to<JsonObject>(), dependency);
        const auto expectBorrowed = [](JsonVariantConst field, const char* source, size_t capacity) {
            const JsonString value = field.as<JsonString>();
            CHECK(value.isStatic());
            CHECK(value.c_str() == source);
            // The const-array adapter carries N-1, but the linked VariantData
            // stores only a pointer and JsonString reconstructs strlen. Do not
            // mistake adapter capacity for serialized text length.
            CHECK(value.size() == std::strlen(source));
            CHECK(value.size() < capacity - 1);
        };
        expectBorrowed(doc["provider"]["params"]["driver"], parent.params.ioExpander.driver,
                       sizeof(parent.params.ioExpander.driver));
        expectBorrowed(doc["provider"]["params"]["busPeripheralId"], parent.params.ioExpander.busPeripheralId,
                       sizeof(parent.params.ioExpander.busPeripheralId));
        expectBorrowed(doc["provider"]["params"]["resetPeripheralId"], parent.params.ioExpander.resetPeripheralId,
                       sizeof(parent.params.ioExpander.resetPeripheralId));
        expectBorrowed(doc["reset"]["params"]["providerId"], dependency.params.gpio.providerId,
                       sizeof(dependency.params.gpio.providerId));
        const Snapshot before = capture(doc);
        CHECK(before.bytes.find("\\u0000") == std::string::npos);
        // This is the same storage reuse as buildMcpGroupPreview's reset -> bus
        // snapshot. The object itself stays alive; no dead-stack access occurs.
        dependency = configFor(PeripheralType::I2C, 0);
        CHECK(!(before == capture(doc)));
        observe(doc);
    });

    for (unsigned version : {0U, 1U, 2U}) {
        activeCase = "mcp-copy-survives-local-return-and-manager-release/" + std::to_string(version);
        evaluate(version, TraceAllocator::never, false, [] {
            PeripheralManager manager;
            auto parent = configFor(PeripheralType::IO_EXPANDER, 0);
            parent.id = "provider"; parent.enabled = false;
            manager.entries["provider"] = parent;
            auto reset = configFor(PeripheralType::GPIO_DIGITAL_OUTPUT, 0); reset.id = "reset";
            manager.entries["reset"] = reset;
            auto bus = configFor(PeripheralType::I2C, 0); bus.id = "bus";
            manager.entries["bus"] = bus;
            auto child = configFor(PeripheralType::GPIO_DIGITAL_OUTPUT, 1); child.id = "channel-0";
            std::strcpy(child.params.gpio.providerId, "provider");
            manager.entries["channel-0"] = child;
            TrackedDocument doc; String error;
            INVOKE_RESULT(returnValue, buildMcpGroupPreview, manager, String("provider"), doc, error);
            CHECK(returnValue); CHECK(error.isEmpty()); CHECK(!doc.overflowed());
            // The real method has returned: its local parent/dependency are
            // already destroyed. None of those locals has a prolonged lifetime.
            const auto assertText = [&] {
                checkOwnedMcpText(doc["data"]["provider"]["params"]["driver"], "MCP23017");
                checkOwnedMcpText(doc["data"]["provider"]["params"]["busPeripheralId"], "bus");
                checkOwnedMcpText(doc["data"]["provider"]["params"]["resetPeripheralId"], "reset");
                checkOwnedMcpText(doc["data"]["reset"]["params"]["providerId"], "provider-with-owned-channel");
                checkOwnedMcpText(doc["data"]["channels"][0]["params"]["providerId"], "provider");
            };
            assertText();
            const Snapshot before = capture(doc);
            CHECK(before.bytes.find("\\u0000") == std::string::npos);
            manager.entries.clear();
            assertText(); CHECK(before == capture(doc));
            observe(doc);
        });
    }

    // Existing resourceCases still exercises 0/1/16/17 channels and both lock
    // states under every persistent/one-shot JSON allocation failure boundary.
}
