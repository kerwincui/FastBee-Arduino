#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <vector>
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

struct FileInfo {
    String name;
    String path;
    size_t size;
    bool isDirectory;
    time_t modifiedTime;
};

class FileUtils {
public:
    // 初始化文件系统
    static bool initialize(bool formatIfFailed = true);
    
    // 文件系统状态
    static bool isInitialized() { return fsInitialized; }
    
    // 基本文件操作
    static bool exists(const String& path);
    static bool isDirectory(const String& path);
    static size_t getFileSize(const String& path);
    static time_t getModifiedTime(const String& path);
    
    // 文件读写
    static String readFile(const String& path);
    static size_t readFileToBuffer(const String& path, uint8_t* buffer, size_t bufferSize);
    static bool writeFile(const String& path, const String& content, bool append = false);
    static bool writeFileFromBuffer(const String& path, const uint8_t* buffer, size_t size, bool append = false);
    
    // 文件管理
    static bool deleteFile(const String& path);
    static bool renameFile(const String& oldPath, const String& newPath);
    static bool copyFile(const String& sourcePath, const String& destPath);
    static bool moveFile(const String& sourcePath, const String& destPath);
    
    // 目录操作
    static bool createDirectory(const String& path);
    static bool createDirectories(const String& path);
    static bool deleteDirectory(const String& path);
    static std::vector<FileInfo> listDirectory(const String& path, bool recursive = false);
    
    // 文件查找
    static std::vector<FileInfo> findFiles(const String& directory, const String& pattern, bool recursive = false);
    
    // 文件信息
    static FileInfo getFileInfo(const String& path);
    static size_t getDirectorySize(const String& path);
    
    // 存储空间信息
    static size_t getFreeSpace();
    static size_t getTotalSpace();
    static float getSpaceUsage();
    
    // 文件完整性
    static String calculateFileHash(const String& path);
    static bool verifyFileIntegrity(const String& path, const String& expectedHash);
    
    // 原子写入（先写临时文件，再 rename，防止断电导致文件损坏）
    static bool atomicWriteFile(const String& path, const String& content);

    // 原子写入 JSON：流式序列化 JsonDocument 到临时文件后 rename 原子替换。
    // 相比 atomicWriteFile(String) 省去大体积 payload 的连续堆分配；
    // 通过校验序列化字节数与 measureJson 一致来检测截断，截断时绝不覆盖已有文件。
    static bool atomicWriteJsonFile(const String& path, JsonDocument& doc);

    // 流式原子写入：打开 path + ".tmp" 临时文件供调用方流式写入（逐段 print/serializeJson），
    // 内部处理父目录创建与残留 .tmp 清理；失败返回空 File。
    // 写完后调用 commitAtomicWrite 原子替换目标文件，峰值内存不随整体配置体积增长。
    static File openAtomicWrite(const String& path);

    // 提交流式原子写入：rename 原子替换目标文件（失败兜底 remove+rename），
    // 全部失败时删除临时文件并保留原目标文件内容。
    static bool commitAtomicWrite(const String& tmpPath, const String& path);

    // 文本文件操作
    static bool appendToFile(const String& path, const String& content);
    static std::vector<String> readLines(const String& path);
    static bool writeLines(const String& path, const std::vector<String>& lines);
    
    // 临时文件管理
    static String createTempFile(const String& prefix, const String& content = "");
    static int cleanupTempFiles(const String& tempDir, time_t olderThan = 0);
    
    // 路径操作
    static String getFileExtension(const String& path);
    static String getFileName(const String& path);
    static String getDirectoryPath(const String& path);
    static String joinPath(const String& base, const String& part);
    static String normalizePath(const String& path);
    
    // 计算文件夹大小（显式栈实现，避免递归栈深度限制）
    static size_t calculateFolderSize(const String& folderPath);
    // 兼容旧调用方的公开入口，内部与 calculateFolderSize 共用显式栈实现；
    // 新代码应使用 calculateFolderSize，待外部集成迁移后再删除此符号。
    static size_t calculateFolderSizeOptimized(const String& folderPath);
    
    // 流式JSON解析：直接从文件读取并反序列化到 JsonDocument
    static bool readJsonFile(const String& path, JsonDocument& doc);

    // 文件系统监控
    static String getFileSystemInfoJSON();
    // 兼容调试入口，仅输出日志且不返回结构化结果；新代码应使用
    // listDirectory/findFiles，保留此 API 以兼容旧版诊断工具。
    static void listAllFiles(const String& path = "/", int depth = 0);
    
private:
    static bool fsInitialized;
    
    // 私有辅助方法
    static bool recursiveDeleteDirectory(const String& path);
    static void findFilesRecursive(const String& directory, const String& pattern, 
                                   std::vector<FileInfo>& results, bool recursive);
    static void listDirectoryRecursive(const String& path, std::vector<FileInfo>& results);
    static bool patternMatches(const String& fileName, const String& pattern);
    static String ensureAbsolutePath(const String& path, const String& baseDir = "/");
};

#endif
