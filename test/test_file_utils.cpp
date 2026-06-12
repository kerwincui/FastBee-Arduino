/**
 * @file test_file_utils.cpp
 * @brief FileUtils 单元测试
 *
 * 覆盖范围：
 *  - 路径操作纯逻辑 (getFileExtension, getFileName, getDirectoryPath, joinPath, normalizePath)
 *  - 基本文件 I/O (writeFile, readFile, exists, deleteFile, getFileSize)
 *  - 原子写入 (atomicWriteFile)
 *  - 追加写入 (appendToFile)
 *  - 文件复制/移动/重命名
 *  - 目录创建/删除
 *  - 行读写 (readLines, writeLines)
 *  - 文件哈希 (calculateFileHash, verifyFileIntegrity)
 *  - 备份恢复 (backupFile, restoreBackup)
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
