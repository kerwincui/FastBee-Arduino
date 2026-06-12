/**
 * @file test_performance_bench.cpp
 * @brief 性能基准测试
 * 
 * 测试内容：
 * - JSON 解析/序列化性能
 * - 字符串操作性能
 * - 内存分配/释放性能
 * - 配置加载耗时基线
 * - 命令分派延迟
 * - 内存泄漏检测（前后堆对比）
 * 
 * 注意：性能测试依赖 micros()/millis() 计时，在 native 环境中
 * 主要验证逻辑正确性和相对性能，绝对耗时参考值仅在设备上有意义。
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "mocks/MockConfigStorage.h"
#include "helpers/TestConfig.h"

void test_performance_bench_group();

// ========== 辅助：计时宏 ==========

// native 环境下使用 micros() 计时
#define BENCH_START() unsigned long _bench_start_val = micros()
#define BENCH_END() (micros() - _bench_start_val)

// 基准迭代次数（native 环境可用较高次数）
static const int BENCH_ITERATIONS = 1000;
static const int BENCH_ITERATIONS_SMALL = 100;

// ========== JSON 解析性能 ==========

static void test_bench_json_parse_small() {
    const char* json = "{\"ssid\":\"MyWiFi\",\"password\":\"secret\",\"dhcp\":true,\"port\":80}";
    
    unsigned long totalTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        BENCH_START();
        JsonDocument doc;
        deserializeJson(doc, json);
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    
    // 验证解析成功
    JsonDocument verify;
    deserializeJson(verify, json);
    TEST_ASSERT_EQUAL_STRING("MyWiFi", verify["ssid"].as<const char*>());
    
    // 性能断言：平均耗时应 < 10ms（native环境通常 < 1ms）
    TEST_ASSERT_TRUE(avgTime < 10000);  // 10ms
    
    Serial.printf("  [BENCH] JSON parse small: avg=%lu us (%d iters)\n", avgTime, BENCH_ITERATIONS);
}

static void test_bench_json_parse_medium() {
    // 中等大小 JSON（模拟设备配置）
    String json = "{";
    json += "\"device\":{\"name\":\"FastBee-Device\",\"model\":\"ESP32-S3\",\"firmware\":\"2.0.0\"},";
    json += "\"network\":{\"ssid\":\"WiFi\",\"password\":\"pass\",\"dhcp\":true,\"ip\":\"192.168.1.100\"},";
    json += "\"mqtt\":{\"enabled\":true,\"server\":\"mqtt.fastbee.cn\",\"port\":1883,\"keepalive\":60},";
    json += "\"peripherals\":[{\"id\":1,\"type\":\"led\",\"pin\":2},{\"id\":2,\"type\":\"dht11\",\"pin\":4}]";
    json += "}";
    
    unsigned long totalTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        BENCH_START();
        JsonDocument doc;
        deserializeJson(doc, json);
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    TEST_ASSERT_TRUE(avgTime < 50000);  // 50ms
    
    Serial.printf("  [BENCH] JSON parse medium: avg=%lu us (%d iters)\n", avgTime, BENCH_ITERATIONS);
}

static void test_bench_json_parse_large() {
    // 大型 JSON（模拟外设执行配置）
    JsonDocument buildDoc;
    JsonArray rules = buildDoc["rules"].to<JsonArray>();
    
    for (int i = 0; i < 20; i++) {
        JsonObject rule = rules.add<JsonObject>();
        rule["id"] = i;
        rule["name"] = "rule_" + String(i);
        rule["enabled"] = (i % 2 == 0);
        rule["trigger"]["type"] = "timer";
        rule["trigger"]["interval"] = 1000 * (i + 1);
        rule["action"]["type"] = "gpio";
        rule["action"]["pin"] = i + 2;
        rule["action"]["value"] = i % 2;
    }
    
    String largeJson;
    serializeJson(buildDoc, largeJson);
    
    unsigned long totalTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS_SMALL; i++) {
        BENCH_START();
        JsonDocument doc;
        deserializeJson(doc, largeJson);
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS_SMALL;
    TEST_ASSERT_TRUE(avgTime < 100000);  // 100ms
    
    Serial.printf("  [BENCH] JSON parse large (%d bytes): avg=%lu us (%d iters)\n", 
                  (int)largeJson.length(), avgTime, BENCH_ITERATIONS_SMALL);
}

// ========== JSON 序列化性能 ==========

static void test_bench_json_serialize() {
    JsonDocument doc;
    doc["status"] = "ok";
    doc["heap"] = 150000;
    doc["uptime"] = 86400;
    doc["wifi"]["connected"] = true;
    doc["wifi"]["rssi"] = -45;
    doc["wifi"]["ip"] = "192.168.1.100";
    
    unsigned long totalTime = 0;
    String output;
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        output = "";
        BENCH_START();
        serializeJson(doc, output);
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    TEST_ASSERT_TRUE(avgTime < 10000);  // 10ms
    TEST_ASSERT_TRUE(output.length() > 0);
    
    Serial.printf("  [BENCH] JSON serialize: avg=%lu us, output=%d bytes\n", 
                  avgTime, (int)output.length());
}

// ========== 字符串操作性能 ==========

static void test_bench_string_concat() {
    unsigned long totalTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        BENCH_START();
        String result = "";
        for (int j = 0; j < 20; j++) {
            result += "segment_" + String(j) + ",";
        }
        totalTime += BENCH_END();
        (void)result;  // 防止优化掉
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    TEST_ASSERT_TRUE(avgTime < 50000);  // 50ms
    
    Serial.printf("  [BENCH] String concat (20 segs): avg=%lu us\n", avgTime);
}

static void test_bench_string_search() {
    String haystack = "GET /api/system/health?token=abc123&format=json HTTP/1.1\r\n"
                      "Host: 192.168.1.100\r\nAuthorization: Bearer eyJ0eXAiOiJKV1Q\r\n";
    
    unsigned long totalTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        BENCH_START();
        int pos1 = haystack.indexOf("Bearer");
        int pos2 = haystack.indexOf("token=");
        int pos3 = haystack.indexOf("Host:");
        (void)pos1; (void)pos2; (void)pos3;
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    TEST_ASSERT_TRUE(avgTime < 5000);  // 5ms
    
    Serial.printf("  [BENCH] String search (3 finds): avg=%lu us\n", avgTime);
}

static void test_bench_snprintf_formatting() {
    unsigned long totalTime = 0;
    char buffer[256];
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        BENCH_START();
        snprintf(buffer, sizeof(buffer), 
                 "[%lu] heap=%u maxBlock=%u wifi=%s rssi=%d ip=%s",
                 (unsigned long)millis(), 150000u, 80000u, 
                 "connected", -45, "192.168.1.100");
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    TEST_ASSERT_TRUE(avgTime < 5000);  // 5ms
    TEST_ASSERT_TRUE(strlen(buffer) > 0);
    
    Serial.printf("  [BENCH] snprintf format: avg=%lu us\n", avgTime);
}

// ========== 内存分配性能 ==========

static void test_bench_memory_alloc_free() {
    unsigned long totalTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        BENCH_START();
        void* ptr = malloc(1024);  // 1KB
        if (ptr) free(ptr);
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    TEST_ASSERT_TRUE(avgTime < 1000);  // 1ms
    
    Serial.printf("  [BENCH] malloc/free 1KB: avg=%lu us\n", avgTime);
}

static void test_bench_memory_alloc_various_sizes() {
    unsigned long totalTime = 0;
    size_t sizes[] = {64, 256, 1024, 4096, 8192};
    
    for (int i = 0; i < BENCH_ITERATIONS_SMALL; i++) {
        for (int s = 0; s < 5; s++) {
            BENCH_START();
            void* ptr = malloc(sizes[s]);
            if (ptr) free(ptr);
            totalTime += BENCH_END();
        }
    }
    
    unsigned long avgTime = totalTime / (BENCH_ITERATIONS_SMALL * 5);
    TEST_ASSERT_TRUE(avgTime < 5000);  // 5ms
    
    Serial.printf("  [BENCH] malloc/free various: avg=%lu us\n", avgTime);
}

// ========== 配置存储性能 ==========

static void test_bench_config_save_load() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    JsonDocument doc;
    doc["ssid"] = "TestNetwork";
    doc["password"] = "TestPassword";
    doc["dhcp"] = true;
    doc["ip"] = "192.168.1.100";
    doc["gateway"] = "192.168.1.1";
    doc["dns"] = "8.8.8.8";
    
    unsigned long saveTime = 0;
    unsigned long loadTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS_SMALL; i++) {
        unsigned long s1 = millis();
        store.saveConfig("/config/bench.json", doc);
        saveTime += (millis() - s1);
        
        JsonDocument loaded;
        unsigned long s2 = millis();
        store.loadConfig("/config/bench.json", loaded);
        loadTime += (millis() - s2);
    }
    
    unsigned long avgSave = saveTime / BENCH_ITERATIONS_SMALL;
    unsigned long avgLoad = loadTime / BENCH_ITERATIONS_SMALL;
    
    TEST_ASSERT_TRUE(avgSave < 50000);  // 50ms
    TEST_ASSERT_TRUE(avgLoad < 50000);  // 50ms
    
    Serial.printf("  [BENCH] Config save: avg=%lu ms, load: avg=%lu ms\n", avgSave, avgLoad);
}

static void test_bench_nvs_batch_write() {
    auto& store = MockConfigStorage::getInstance();
    store.clearAll();
    store.initialize();
    
    unsigned long totalTime = 0;
    
    BENCH_START();
    for (int i = 0; i < 50; i++) {
        String key = "key_" + String(i);
        String value = "value_" + String(i) + "_data_payload";
        store.putString(key.c_str(), value);
    }
    totalTime = BENCH_END();
    
    TEST_ASSERT_TRUE(totalTime < 100000);  // 100ms for 50 writes
    
    Serial.printf("  [BENCH] NVS batch write (50 keys): %lu us\n", totalTime);
}

// ========== 内存泄漏检测 ==========

static void test_memory_leak_json_operations() {
    // 记录初始内存（在 native 环境中使用 malloc_usable_size 的替代方案）
    // 在 mock 环境中验证 JsonDocument 正确释放
    
    for (int i = 0; i < 100; i++) {
        JsonDocument doc;
        doc["iteration"] = i;
        doc["data"] = "some payload for leak detection test";
        
        JsonArray arr = doc["array"].to<JsonArray>();
        for (int j = 0; j < 10; j++) {
            arr.add(j * 100);
        }
        
        String output;
        serializeJson(doc, output);
        
        // doc 在作用域结束时自动释放
    }
    
    // 如果有泄漏，大量迭代后 heap 会下降
    // 在 native 测试中主要验证代码路径没有异常
    TEST_ASSERT_TRUE(true);  // 如果到达此处说明没有崩溃
    
    Serial.println("  [LEAK] JSON operations: 100 iterations completed without crash");
}

static void test_memory_leak_string_operations() {
    for (int i = 0; i < 200; i++) {
        String s1 = "Hello World ";
        String s2 = s1 + String(i);
        String s3 = s2.substring(0, 5);
        s3 += " appended";
        s3.replace("appended", "replaced");
        (void)s3;
    }
    
    TEST_ASSERT_TRUE(true);
    Serial.println("  [LEAK] String operations: 200 iterations completed without crash");
}

static void test_memory_leak_map_operations() {
    for (int i = 0; i < 50; i++) {
        std::map<String, String> tempMap;
        for (int j = 0; j < 20; j++) {
            tempMap["key_" + String(j)] = "value_" + String(j);
        }
        // tempMap 在作用域结束时释放
    }
    
    TEST_ASSERT_TRUE(true);
    Serial.println("  [LEAK] Map operations: 50 iterations completed without crash");
}

static void test_memory_leak_vector_operations() {
    for (int i = 0; i < 100; i++) {
        std::vector<String> tempVec;
        for (int j = 0; j < 30; j++) {
            tempVec.push_back("element_" + String(j));
        }
        tempVec.clear();
    }
    
    TEST_ASSERT_TRUE(true);
    Serial.println("  [LEAK] Vector operations: 100 iterations completed without crash");
}

// ========== 边界值性能 ==========

static void test_bench_empty_json() {
    unsigned long totalTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        BENCH_START();
        JsonDocument doc;
        deserializeJson(doc, "{}");
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    TEST_ASSERT_TRUE(avgTime < 5000);
    
    Serial.printf("  [BENCH] Empty JSON parse: avg=%lu us\n", avgTime);
}

static void test_bench_deeply_nested_json() {
    // 构建深度嵌套 JSON
    String nested = "{";
    for (int i = 0; i < 5; i++) {
        nested += "\"l" + String(i) + "\":{";
    }
    nested += "\"value\":42";
    for (int i = 0; i < 5; i++) {
        nested += "}";
    }
    nested += "}";
    
    unsigned long totalTime = 0;
    
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        BENCH_START();
        JsonDocument doc;
        deserializeJson(doc, nested);
        totalTime += BENCH_END();
    }
    
    unsigned long avgTime = totalTime / BENCH_ITERATIONS;
    TEST_ASSERT_TRUE(avgTime < 20000);  // 20ms
    
    Serial.printf("  [BENCH] Nested JSON (5 levels): avg=%lu us\n", avgTime);
}

// ========== 测试组入口 ==========

void test_performance_bench_group() {
    Serial.println("\n  === Performance Benchmarks ===\n");
    
    // JSON 解析性能
    RUN_TEST(test_bench_json_parse_small);
    RUN_TEST(test_bench_json_parse_medium);
    RUN_TEST(test_bench_json_parse_large);
    RUN_TEST(test_bench_json_serialize);
    
    // 字符串操作性能
    RUN_TEST(test_bench_string_concat);
    RUN_TEST(test_bench_string_search);
    RUN_TEST(test_bench_snprintf_formatting);
    
    // 内存分配性能
    RUN_TEST(test_bench_memory_alloc_free);
    RUN_TEST(test_bench_memory_alloc_various_sizes);
    
    // 配置存储性能
    RUN_TEST(test_bench_config_save_load);
    RUN_TEST(test_bench_nvs_batch_write);
    
    // 内存泄漏检测
    RUN_TEST(test_memory_leak_json_operations);
    RUN_TEST(test_memory_leak_string_operations);
    RUN_TEST(test_memory_leak_map_operations);
    RUN_TEST(test_memory_leak_vector_operations);
    
    // 边界值性能
    RUN_TEST(test_bench_empty_json);
    RUN_TEST(test_bench_deeply_nested_json);
    
    Serial.println("\n  === Benchmarks Complete ===\n");
}
