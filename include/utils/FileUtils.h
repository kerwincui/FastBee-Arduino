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
    
    // 备份管理
    static String backupFile(const String& sourcePath, const String& backupDir);
    static bool restoreBackup(const String& backupPath, const String& restorePath);
    static int cleanupOldBackups(const String& backupDir, int keepCount);
    
    // 文件完整性
    static String calculateFileHash(const String& path);
    static bool verifyFileIntegrity(const String& path, const String& expectedHash);
    
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
    
    // 递归计算文件夹大小（多个实现版本）
    static size_t calculateFolderSize(const String& folderPath);
    static size_t calculateFolderSizeOptimized(const String& folderPath); // 使用栈的优化版本
    
    // 文件系统监控
    static String getFileSystemInfoJSON();
    static void listAllFiles(const String& path = "/", int depth = 0);
    
private:
    static bool fsInitialized;
    
    // 私有辅助方法
    static bool recursiveDeleteDirectory(const String& path);
    static bool recursiveCopyDirectory(const String& source, const String& dest);
    static void findFilesRecursive(const String& directory, const String& pattern, 
                                   std::vector<FileInfo>& results, bool recursive);
    static void listDirectoryRecursive(const String& path, std::vector<FileInfo>& results);
    static size_t calculateFolderSizeRecursive(const String& folderPath);
    static void addFileInfoToJson(const String& path, JsonObject& parent);
    static bool patternMatches(const String& fileName, const String& pattern);
    static String ensureAbsolutePath(const String& path, const String& baseDir = "/");
};

#endif