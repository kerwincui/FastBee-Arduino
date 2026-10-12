#include "core/ScriptEngine.h"
#include "core/ScriptRandomExpansion.h"
#include <cassert>
#include <string>
#include <vector>

static std::vector<uint32_t> draws;
static size_t drawIndex = 0;
uint32_t esp_random() {
    assert(drawIndex < draws.size()); return draws[drawIndex++];
}
// @production@

static void success(const std::string& input, const char* expected,
                    std::vector<uint32_t> random = {}) {
    draws = random; drawIndex = 0;
    String output("old");
    assert(ScriptEngine::processRandomExpressions(String(input), output));
    assert(std::string(output.c_str(), output.length()) == expected);
    assert(drawIndex == draws.size());
}
static void failure(const std::string& input, std::vector<uint32_t> random = {}) {
    draws = random; drawIndex = 0;
    String output("old");
    assert(!ScriptEngine::processRandomExpressions(String(input), output));
    assert(output.length() == 0); // Never leave a partial prefix publishable.
}
int main() {
    success("", "");
    success("普通文本 ${sensor.value}", "普通文本 ${sensor.value}");
    success("RANDOM(-2147483648,2147483647)", "-2147483648", {0});
    success("RANDOM(-2147483648,2147483647)", "2147483647", {UINT32_MAX});
    success("RANDOM(2147483647,-2147483648)", "0", {UINT32_C(2147483648)});
    success("RANDOM(-2147483648,-2147483648)", "-2147483648", {UINT32_MAX});
    success("RANDOM(2147483647,2147483647)", "2147483647", {0});
    success("RANDOM( +2 , -2 )", "2", {4});
    success("[RANDOM(0,9),RANDOMF(1,2,2),RANDOM(8,8)]", "[7,1.50,8]", {7,500000,123});
    success("RANDOMF(2,1,2)", "1.00", {0});
    success("RANDOMF(-1e2,+1e2,3)", "0.000", {500000});
    success("RANDOMF(.5,.5,99)", "0.500000", {1});
    success("RANDOMF(2,2,-4)", "2", {1});
    success(std::string(1024, 'a'), std::string(1024, 'a').c_str());
    for (const char* bad : {"RANDOM(", "RANDOM()", "RANDOM(1)", "RANDOM(1,2,3)",
            "RANDOM(,2)", "RANDOM(1,)", "RANDOM(foo,1)", "RANDOM(1.2,3)",
            "RANDOM(2147483648,0)", "RANDOM(-2147483649,0)",
            "RANDOM(1,RANDOM(2,3))", "RANDOMF(1,2)", "RANDOMF(1,2,3,4)",
            "RANDOMF(NaN,1,2)", "RANDOMF(1,inf,2)", "RANDOMF(0x10,20,2)",
            "RANDOMF(1e,2,2)", "RANDOMF(1e999,2,2)", "RANDOMF(1e-999,2,2)",
            "RANDOMF(1,2,1.2)", "RANDOMF(1,2,foo)"}) failure(bad);
    failure(std::string(1025, 'x'));
    failure(std::string("abc\0def", 7));
    failure("prefix RANDOM(1,1) suffix RANDOM(bad,2)", {0});
    failure("RANDOMF(1e100,1e100,2)", {0}); // snprintf length must not silently truncate.
    const std::string expanding = "RANDOMF(1e30,1e30,6)";
    std::string large;
    for (int i = 0; i < 30; ++i) large += expanding;
    failure(large, std::vector<uint32_t>(30, 0));
    String::rejectReserve = true;
    failure("payload");
    String::rejectReserve = false;
    String::appendBudget = 3;
    failure("prefix RANDOM(1,2)");
    String::appendBudget = 9;
    failure("a RANDOM(2147483647,2147483647) z", {0});
    String::appendBudget = -1;
    success("payload", "payload"); // Allocation failure cannot poison the next call.
    String alias("input");
    assert(!ScriptEngine::processRandomExpressions(alias, alias));
    assert(std::string(alias.c_str()) == "input");
}
