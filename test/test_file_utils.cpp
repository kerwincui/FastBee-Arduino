/**
 * @file test_file_utils.cpp
 * @brief FileUtils 单元测试
 *
 * 覆盖范围：
 *  - 路径操作纯逻辑 (getFileExtension, getFileName, getDirectoryPath, joinPath, normalizePath)
 *  - 基本文件 I/O (writeFile, readFile, exists, deleteFile, getFileSize)
 *  - 原子写入 (atomicWriteFile)
 *  - JSON 原子写入 (atomicWriteJsonFile：流式序列化 + 截断校验)
 *  - 追加写入 (appendToFile)
 *  - 文件复制/移动/重命名
 *  - 目录创建/删除
 *  - 行读写 (readLines, writeLines)
 *  - 文件哈希 (calculateFileHash, verifyFileIntegrity)
 *  - 存储空间信息
 */

#include <unity.h>
#include <Arduino.h>
#include <LittleFS.h>
#include "utils/FileUtils.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_file_utils_group();

// 测试目录常量
static const char* TEST_DIR = "/test_fu";

// 辅助：清理测试目录
static void cleanupTestDir() {
    if (LittleFS.exists(TEST_DIR)) {
        File dir = LittleFS.open(TEST_DIR);
        if (dir && dir.isDirectory()) {
            File f = dir.openNextFile();
            while (f) {
                String name = f.name();
                if (!f.isDirectory()) LittleFS.remove(name);
                else LittleFS.rmdir(name);
                f = dir.openNextFile();
            }
        }
        dir.close();
        LittleFS.rmdir(TEST_DIR);
    }
}

// ========== 路径操作纯逻辑 ==========

void test_get_file_extension() {
    TestLog::testStart("Path: getFileExtension");

    TEST_ASSERT_EQUAL_STRING("json", FileUtils::getFileExtension("/config/device.json").c_str());
    TEST_ASSERT_EQUAL_STRING("gz", FileUtils::getFileExtension("/www/index.html.gz").c_str());
    TEST_ASSERT_EQUAL_STRING("", FileUtils::getFileExtension("/config/README").c_str());
    TEST_ASSERT_EQUAL_STRING("", FileUtils::getFileExtension("").c_str());
    // 目录路径中斜杠在点号之后不应返回扩展名
    TEST_ASSERT_EQUAL_STRING("", FileUtils::getFileExtension("/dir.ext/file").c_str());
    TestLog::step("Extension extraction verified for 5 cases");

    TestLog::testEnd(true);
}

void test_get_file_name() {
    TestLog::testStart("Path: getFileName");

    TEST_ASSERT_EQUAL_STRING("device.json", FileUtils::getFileName("/config/device.json").c_str());
    TEST_ASSERT_EQUAL_STRING("index.html", FileUtils::getFileName("/www/index.html").c_str());
    TEST_ASSERT_EQUAL_STRING("file.txt", FileUtils::getFileName("file.txt").c_str());
    TEST_ASSERT_EQUAL_STRING("", FileUtils::getFileName("").c_str());
    TestLog::step("File name extraction verified for 4 cases");

    TestLog::testEnd(true);
}

void test_get_directory_path() {
    TestLog::testStart("Path: getDirectoryPath");

    TEST_ASSERT_EQUAL_STRING("/config", FileUtils::getDirectoryPath("/config/device.json").c_str());
    TEST_ASSERT_EQUAL_STRING("/www/css", FileUtils::getDirectoryPath("/www/css/style.css").c_str());
    TEST_ASSERT_EQUAL_STRING("", FileUtils::getDirectoryPath("file.txt").c_str());
    TestLog::step("Directory path extraction verified");

    TestLog::testEnd(true);
}

void test_join_path() {
    TestLog::testStart("Path: joinPath");

    String r1 = FileUtils::joinPath("/config", "device.json");
    TEST_ASSERT_EQUAL_STRING("/config/device.json", r1.c_str());
    TestLog::step("joinPath('/config', 'device.json') = '/config/device.json'");

    String r2 = FileUtils::joinPath("/config/", "device.json");
    TEST_ASSERT_EQUAL_STRING("/config/device.json", r2.c_str());
    TestLog::step("joinPath with trailing slash handled");

    String r3 = FileUtils::joinPath("/config", "/device.json");
    TEST_ASSERT_EQUAL_STRING("/config/device.json", r3.c_str());
    TestLog::step("joinPath with leading slash on part handled");

    String r4 = FileUtils::joinPath("", "file.txt");
    TEST_ASSERT_EQUAL_STRING("file.txt", r4.c_str());
    TestLog::step("Empty base returns part");

    String r5 = FileUtils::joinPath("/config", "");
    TEST_ASSERT_EQUAL_STRING("/config", r5.c_str());
    TestLog::step("Empty part returns base");

    TestLog::testEnd(true);
}

void test_normalize_path() {
    TestLog::testStart("Path: normalizePath");

    TEST_ASSERT_EQUAL_STRING("/", FileUtils::normalizePath("").c_str());
    TEST_ASSERT_EQUAL_STRING("/", FileUtils::normalizePath("/").c_str());
    TEST_ASSERT_EQUAL_STRING("/config", FileUtils::normalizePath("config").c_str());
    TEST_ASSERT_EQUAL_STRING("/config", FileUtils::normalizePath("/config/").c_str());
    TEST_ASSERT_EQUAL_STRING("/a/b/c", FileUtils::normalizePath("/a/b/c").c_str());
    TestLog::step("Path normalization verified for 5 cases");

    TestLog::testEnd(true);
}

// ========== 基本文件 I/O ==========

void test_file_write_read_delete() {
    TestLog::testStart("File I/O: Write, Read, Delete");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String path = String(TEST_DIR) + "/test1.txt";
    String content = "Hello, FastBee!";

    // 写入
    TEST_ASSERT_TRUE(FileUtils::writeFile(path, content));
    TestLog::step("writeFile succeeded");

    // 存在性
    TEST_ASSERT_TRUE(FileUtils::exists(path));
    TestLog::step("exists() returns true");

    // 读取
    String read = FileUtils::readFile(path);
    TEST_ASSERT_EQUAL_STRING(content.c_str(), read.c_str());
    TestLog::step("readFile content matches");

    // 大小
    TEST_ASSERT_EQUAL(content.length(), FileUtils::getFileSize(path));
    TestLog::step("getFileSize matches content length");

    // 删除
    TEST_ASSERT_TRUE(FileUtils::deleteFile(path));
    TEST_ASSERT_FALSE(FileUtils::exists(path));
    TestLog::step("deleteFile succeeded, file no longer exists");

    cleanupTestDir();
    TestLog::testEnd(true);
}

void test_file_append() {
    TestLog::testStart("File I/O: Append");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String path = String(TEST_DIR) + "/append.txt";
    FileUtils::writeFile(path, "Hello");
    FileUtils::appendToFile(path, " World");

    String content = FileUtils::readFile(path);
    TEST_ASSERT_EQUAL_STRING("Hello World", content.c_str());
    TestLog::step("appendToFile verified: 'Hello World'");

    cleanupTestDir();
    TestLog::testEnd(true);
}

// ========== 原子写入 ==========

void test_atomic_write() {
    TestLog::testStart("Atomic Write");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String path = String(TEST_DIR) + "/atomic.txt";

    // 第一次写入
    TEST_ASSERT_TRUE(FileUtils::atomicWriteFile(path, "version1"));
    TEST_ASSERT_EQUAL_STRING("version1", FileUtils::readFile(path).c_str());
    TestLog::step("First atomic write: 'version1'");

    // 覆盖写入
    TEST_ASSERT_TRUE(FileUtils::atomicWriteFile(path, "version2_longer_content"));
    TEST_ASSERT_EQUAL_STRING("version2_longer_content", FileUtils::readFile(path).c_str());
    TestLog::step("Second atomic write overwrites correctly");

    // 确保没有 .tmp 残留
    String tmpPath = path + ".tmp";
    TEST_ASSERT_FALSE(FileUtils::exists(tmpPath));
    TestLog::step("No .tmp file residual");

    cleanupTestDir();
    TestLog::testEnd(true);
}

// ========== JSON 原子写入 ==========

void test_atomic_write_json() {
    TestLog::testStart("Atomic Write JSON");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String path = String(TEST_DIR) + "config.json";

    // 构建文档并写入
    JsonDocument doc;
    doc["version"] = 3;
    JsonArray arr = doc["rules"].to<JsonArray>();
    JsonObject rule = arr.add<JsonObject>();
    rule["id"] = "r1";
    rule["name"] = "测试规则";
    rule["enabled"] = true;

    TEST_ASSERT_TRUE(FileUtils::atomicWriteJsonFile(path, doc));
    TestLog::step("First atomic JSON write succeeded");

    // 读回反序列化，字段逐一比对
    File f = LittleFS.open(path, "r");
    TEST_ASSERT_TRUE((bool)f);
    JsonDocument readBack;
    DeserializationError err = deserializeJson(readBack, f);
    f.close();
    TEST_ASSERT_EQUAL((int)DeserializationError::Ok, (int)err.code());
    TEST_ASSERT_EQUAL_INT(3, readBack["version"].as<int>());
    TEST_ASSERT_EQUAL_INT(1, readBack["rules"].size());
    TEST_ASSERT_EQUAL_STRING("r1", readBack["rules"][0]["id"].as<const char*>());
    TEST_ASSERT_TRUE(readBack["rules"][0]["enabled"].as<bool>());
    TestLog::step("Read-back parse matches original document");

    // 文件字节数必须与 measureJson 一致（无截断、无多余字节）
    TEST_ASSERT_EQUAL(measureJson(doc), FileUtils::getFileSize(path));
    TestLog::step("File size equals measureJson (no truncation)");

    // 覆盖写入：旧内容必须被完整替换
    JsonDocument doc2;
    doc2["version"] = 4;
    doc2["note"] = "overwritten";
    TEST_ASSERT_TRUE(FileUtils::atomicWriteJsonFile(path, doc2));
    File f2 = LittleFS.open(path, "r");
    JsonDocument readBack2;
    DeserializationError err2 = deserializeJson(readBack2, f2);
    f2.close();
    TEST_ASSERT_EQUAL((int)DeserializationError::Ok, (int)err2.code());
    TEST_ASSERT_EQUAL_INT(4, readBack2["version"].as<int>());
    TEST_ASSERT_TRUE(readBack2["rules"].isNull());
    TEST_ASSERT_EQUAL(measureJson(doc2), FileUtils::getFileSize(path));
    TestLog::step("Overwrite replaces previous content completely");

    // 无 .tmp 残留
    TEST_ASSERT_FALSE(FileUtils::exists(path + ".tmp"));
    TestLog::step("No .tmp file residual");

    cleanupTestDir();
    TestLog::testEnd(true);
}

void test_atomic_write_json_empty_document() {
    TestLog::testStart("Atomic Write JSON: Empty Document");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String path = String(TEST_DIR) + "empty.json";
    JsonDocument doc;

    // 空文档也是合法输入：ArduinoJson 7 序列化为 "null"，且同样可被正常解析
    TEST_ASSERT_TRUE(FileUtils::atomicWriteJsonFile(path, doc));
    TEST_ASSERT_EQUAL_STRING("null", FileUtils::readFile(path).c_str());
    TEST_ASSERT_FALSE(FileUtils::exists(path + ".tmp"));
    TestLog::step("Empty document writes 'null', no .tmp residual");

    cleanupTestDir();
    TestLog::testEnd(true);
}

void test_streaming_atomic_write() {
    TestLog::testStart("Atomic Write: Streaming open/commit (per-segment write)");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String path = String(TEST_DIR) + "stream.json";

    // 首次写入：逐段流式写入（模拟逐规则 serializeJson 直写）
    File out = FileUtils::openAtomicWrite(path);
    TEST_ASSERT_TRUE((bool)out);
    size_t written = out.print("{\"rules\":[");
    written += out.print("{\"id\":\"r1\"}");
    written += out.print(",{\"id\":\"r2\"}");
    written += out.print("]}");
    out.flush();
    out.close();
    TEST_ASSERT_TRUE(FileUtils::commitAtomicWrite(path + ".tmp", path));
    TestLog::step("Streaming segments written and committed");

    // 读回内容必须与逐段写入的总字节完全一致
    String content = FileUtils::readFile(path);
    TEST_ASSERT_EQUAL(written, content.length());
    TEST_ASSERT_EQUAL_STRING("{\"rules\":[{\"id\":\"r1\"},{\"id\":\"r2\"}]}", content.c_str());
    TEST_ASSERT_FALSE(FileUtils::exists(path + ".tmp"));
    TestLog::step("Read-back matches streamed content, no .tmp residual");

    // 覆盖写入：openAtomicWrite 需清理旧内容，commit 后目标被完整替换
    File out2 = FileUtils::openAtomicWrite(path);
    TEST_ASSERT_TRUE((bool)out2);
    out2.print("{\"replaced\":true}");
    out2.flush();
    out2.close();
    TEST_ASSERT_TRUE(FileUtils::commitAtomicWrite(path + ".tmp", path));
    TEST_ASSERT_EQUAL_STRING("{\"replaced\":true}", FileUtils::readFile(path).c_str());
    TestLog::step("Commit atomically replaces existing target");

    // 中途放弃：不 commit 时目标文件保持旧内容，再次 openAtomicWrite 清理残留 tmp
    File out3 = FileUtils::openAtomicWrite(path);
    TEST_ASSERT_TRUE((bool)out3);
    out3.print("partial");
    out3.close();
    TEST_ASSERT_TRUE(FileUtils::exists(path + ".tmp"));
    TEST_ASSERT_EQUAL_STRING("{\"replaced\":true}", FileUtils::readFile(path).c_str());
    File out4 = FileUtils::openAtomicWrite(path);
    TEST_ASSERT_TRUE((bool)out4);
    out4.close();
    // 重新打开后残留 tmp 已被清理（替换为空 tmp），目标文件不受影响
    TEST_ASSERT_EQUAL_STRING("", FileUtils::readFile(path + ".tmp").c_str());
    TEST_ASSERT_EQUAL_STRING("{\"replaced\":true}", FileUtils::readFile(path).c_str());
    LittleFS.remove(path + ".tmp");
    TestLog::step("Abandoned tmp never touches target; next open cleans residual");

    // 无 tmp 文件时 commit 必须失败且不破坏目标
    TEST_ASSERT_FALSE(FileUtils::commitAtomicWrite(path + ".tmp", path));
    TEST_ASSERT_EQUAL_STRING("{\"replaced\":true}", FileUtils::readFile(path).c_str());
    TestLog::step("Commit without tmp fails safely");

    cleanupTestDir();
    TestLog::testEnd(true);
}

// ========== 文件复制/移动/重命名 ==========

void test_file_copy_move_rename() {
    TestLog::testStart("File: Copy, Move, Rename");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String src = String(TEST_DIR) + "/src.txt";
    String dst = String(TEST_DIR) + "/dst.txt";
    FileUtils::writeFile(src, "copy me");

    // 复制
    TEST_ASSERT_TRUE(FileUtils::copyFile(src, dst));
    TEST_ASSERT_TRUE(FileUtils::exists(src));
    TEST_ASSERT_TRUE(FileUtils::exists(dst));
    TEST_ASSERT_EQUAL_STRING("copy me", FileUtils::readFile(dst).c_str());
    TestLog::step("copyFile: source preserved, destination created");

    // 重命名
    String renamed = String(TEST_DIR) + "/renamed.txt";
    TEST_ASSERT_TRUE(FileUtils::renameFile(dst, renamed));
    TEST_ASSERT_FALSE(FileUtils::exists(dst));
    TEST_ASSERT_TRUE(FileUtils::exists(renamed));
    TestLog::step("renameFile: old gone, new exists");

    // 移动
    String moved = String(TEST_DIR) + "/moved.txt";
    FileUtils::deleteFile(renamed);  // 清理目标，copyFile 要求目标不存在
    // 重新创建 renamed
    FileUtils::writeFile(renamed, "move me");
    TEST_ASSERT_TRUE(FileUtils::moveFile(renamed, moved));
    TEST_ASSERT_FALSE(FileUtils::exists(renamed));
    TEST_ASSERT_TRUE(FileUtils::exists(moved));
    TEST_ASSERT_EQUAL_STRING("move me", FileUtils::readFile(moved).c_str());
    TestLog::step("moveFile: source gone, destination has correct content");

    cleanupTestDir();
    TestLog::testEnd(true);
}

// ========== 行读写 ==========

void test_read_write_lines() {
    TestLog::testStart("File: ReadLines / WriteLines");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String path = String(TEST_DIR) + "/lines.txt";
    std::vector<String> lines = {"line1", "line2", "line3"};

    TEST_ASSERT_TRUE(FileUtils::writeLines(path, lines));
    TestLog::step("writeLines: 3 lines written");

    std::vector<String> readBack = FileUtils::readLines(path);
    TEST_ASSERT_EQUAL(3, readBack.size());
    TEST_ASSERT_EQUAL_STRING("line1", readBack[0].c_str());
    TEST_ASSERT_EQUAL_STRING("line2", readBack[1].c_str());
    TEST_ASSERT_EQUAL_STRING("line3", readBack[2].c_str());
    TestLog::step("readLines: 3 lines read, content matches");

    cleanupTestDir();
    TestLog::testEnd(true);
}

// ========== 文件哈希 ==========

void test_file_hash() {
    TestLog::testStart("File: Hash & Integrity");

    FileUtils::initialize();
    cleanupTestDir();
    LittleFS.mkdir(TEST_DIR);

    String path = String(TEST_DIR) + "/hash.txt";
    FileUtils::writeFile(path, "integrity check");

    String hash = FileUtils::calculateFileHash(path);
    TEST_ASSERT_FALSE(hash.isEmpty());
    TestLog::step("calculateFileHash returned non-empty hash");

    // 用正确哈希验证
    TEST_ASSERT_TRUE(FileUtils::verifyFileIntegrity(path, hash));
    TestLog::step("verifyFileIntegrity with correct hash: true");

    // 用错误哈希验证
    TEST_ASSERT_FALSE(FileUtils::verifyFileIntegrity(path, "deadbeef"));
    TestLog::step("verifyFileIntegrity with wrong hash: false");

    // 相同内容应该产生相同哈希
    String path2 = String(TEST_DIR) + "/hash2.txt";
    FileUtils::writeFile(path2, "integrity check");
    String hash2 = FileUtils::calculateFileHash(path2);
    TEST_ASSERT_EQUAL_STRING(hash.c_str(), hash2.c_str());
    TestLog::step("Same content produces same hash");

    cleanupTestDir();
    TestLog::testEnd(true);
}

// ========== 存储空间信息 ==========

void test_storage_space_info() {
    TestLog::testStart("Storage Space Info");

    FileUtils::initialize();

    size_t total = FileUtils::getTotalSpace();
    size_t free  = FileUtils::getFreeSpace();
    float usage  = FileUtils::getSpaceUsage();

    TEST_ASSERT_GREATER_THAN(0, (int)total);
    TEST_ASSERT_LESS_OR_EQUAL(total, free);
    TEST_ASSERT_TRUE(usage >= 0.0f && usage <= 1.0f);
    TestLog::step("Total > 0, free <= total, usage in [0, 1]");

    TestLog::testEnd(true);
}

// ========== 不存在文件的边界处理 ==========

void test_nonexistent_file_handling() {
    TestLog::testStart("Edge: Nonexistent File Handling");

    FileUtils::initialize();

    TEST_ASSERT_FALSE(FileUtils::exists("/nonexistent_file_12345.txt"));
    TEST_ASSERT_EQUAL_STRING("", FileUtils::readFile("/nonexistent_file_12345.txt").c_str());
    TEST_ASSERT_EQUAL(0, (int)FileUtils::getFileSize("/nonexistent_file_12345.txt"));
    TEST_ASSERT_FALSE(FileUtils::deleteFile("/nonexistent_file_12345.txt"));
    TEST_ASSERT_EQUAL_STRING("", FileUtils::calculateFileHash("/nonexistent_file_12345.txt").c_str());
    TestLog::step("All operations on nonexistent file return safe defaults");

    TestLog::testEnd(true);
}

// ========== 测试组入口 ==========

void test_file_utils_group() {
    TestLog::groupStart("FileUtils Tests");

    // 路径操作纯逻辑
    RUN_TEST(test_get_file_extension);
    RUN_TEST(test_get_file_name);
    RUN_TEST(test_get_directory_path);
    RUN_TEST(test_join_path);
    RUN_TEST(test_normalize_path);

    // 文件 I/O
    RUN_TEST(test_file_write_read_delete);
    RUN_TEST(test_file_append);
    RUN_TEST(test_atomic_write);
    RUN_TEST(test_atomic_write_json);
    RUN_TEST(test_atomic_write_json_empty_document);
    RUN_TEST(test_streaming_atomic_write);
    RUN_TEST(test_file_copy_move_rename);

    // 行读写
    RUN_TEST(test_read_write_lines);

    // 哈希与完整性
    RUN_TEST(test_file_hash);

    // 存储空间
    RUN_TEST(test_storage_space_info);

    // 边界条件
    RUN_TEST(test_nonexistent_file_handling);

    TestLog::groupEnd();
}
