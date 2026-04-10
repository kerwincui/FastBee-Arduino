/**
 * @file FileUtils.cpp
 * @brief 文件系统工具类的完整实现
 * @author kerwincui
 * @date 2025-12-02
 */

#include "utils/FileUtils.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include <ArduinoJson.h>
#include <vector>
#include <map>

// 静态成员初始化
bool FileUtils::fsInitialized = false;

// ==================== 文件系统初始化 ====================

bool FileUtils::initialize(bool formatIfFailed) {
    if (fsInitialized) {
        return true;
    }
    
    // LittleFS 由 ConfigStorage 统一挂载，此处检测挂载状态
    if (LittleFS.totalBytes() > 0) {
        fsInitialized = true;
        return true;
    }
    
    // 若 ConfigStorage 尚未初始化（单独使用场景），则自行挂载
    fsInitialized = LittleFS.begin(formatIfFailed);
    if (!fsInitialized) {
        LOG_ERROR("[FileUtils] LittleFS mount failed");
        return false;
    }
    
    return fsInitialized;
}

// ==================== 基本文件操作 ====================

bool FileUtils::exists(const String& path) {
    if (!fsInitialized) return false;
    return LittleFS.exists(path);
}

bool FileUtils::isDirectory(const String& path) {
    if (!fsInitialized || !exists(path)) return false;
    
    File file = LittleFS.open(path);
    bool result = file.isDirectory();
    file.close();
    return result;
}

size_t FileUtils::getFileSize(const String& path) {
    if (!fsInitialized || !exists(path) || isDirectory(path)) return 0;
    
    File file = LittleFS.open(path, "r");
    if (!file) return 0;
    
    size_t size = file.size();
    file.close();
    return size;
}

time_t FileUtils::getModifiedTime(const String& path) {
    // LittleFS 默认不支持修改时间
    return 0;
}

// ==================== 文件读写操作 ====================

String FileUtils::readFile(const String& path) {
    if (!fsInitialized || !exists(path) || isDirectory(path)) {
        return "";
    }
    
    File file = LittleFS.open(path, "r");
    if (!file) {
        return "";
    }
    
    String content = file.readString();
    file.close();
    return content;
}

size_t FileUtils::readFileToBuffer(const String& path, uint8_t* buffer, size_t bufferSize) {
    if (!fsInitialized || !exists(path) || isDirectory(path)) {
        return 0;
    }
    
    File file = LittleFS.open(path, "r");
    if (!file) {
        return 0;
    }
    
    size_t fileSize = file.size();
    size_t bytesToRead = (bufferSize < fileSize) ? bufferSize : fileSize;
    
    size_t bytesRead = file.read(buffer, bytesToRead);
    file.close();
    
    return bytesRead;
}

bool FileUtils::writeFile(const String& path, const String& content, bool append) {
    if (!fsInitialized) return false;
    
    String mode = append ? "a" : "w";
    File file = LittleFS.open(path, mode.c_str());
    if (!file) {
        return false;
    }
    
    size_t bytesWritten = file.print(content);
    file.close();
    
    return bytesWritten == content.length();
}

bool FileUtils::writeFileFromBuffer(const String& path, const uint8_t* buffer, size_t size, bool append) {
    if (!fsInitialized) return false;
    
    String mode = append ? "a" : "w";
    File file = LittleFS.open(path, mode.c_str());
    if (!file) {
        return false;
    }
    
    size_t bytesWritten = file.write(buffer, size);
    file.close();
    
    return bytesWritten == size;
}

// ==================== 文件管理操作 ====================

bool FileUtils::deleteFile(const String& path) {
    if (!fsInitialized || !exists(path)) {
        return false;
    }
    
    if (isDirectory(path)) {
        return deleteDirectory(path);
    }
    
    return LittleFS.remove(path);
}

bool FileUtils::renameFile(const String& oldPath, const String& newPath) {
    if (!fsInitialized || !exists(oldPath) || exists(newPath)) {
        return false;
    }
    
    return LittleFS.rename(oldPath, newPath);
}

bool FileUtils::copyFile(const String& sourcePath, const String& destPath) {
    if (!fsInitialized || !exists(sourcePath) || exists(destPath) || isDirectory(sourcePath)) {
        return false;
    }
    
    // 读取源文件
    File sourceFile = LittleFS.open(sourcePath, "r");
    if (!sourceFile) {
        return false;
    }
    
    // 创建目标文件
    File destFile = LittleFS.open(destPath, "w");
    if (!destFile) {
        sourceFile.close();
        return false;
    }
    
    // 复制数据
    size_t bufferSize = 512;
    uint8_t buffer[bufferSize];
    
    while (sourceFile.available()) {
        size_t bytesRead = sourceFile.read(buffer, bufferSize);
        destFile.write(buffer, bytesRead);
    }
    
    sourceFile.close();
    destFile.close();
    
    return true;
}

bool FileUtils::moveFile(const String& sourcePath, const String& destPath) {
    if (!copyFile(sourcePath, destPath)) {
        return false;
    }
    
    return deleteFile(sourcePath);
}

// ==================== 目录操作 ====================

bool FileUtils::createDirectory(const String& path) {
    if (!fsInitialized || exists(path)) {
        return false;
    }
    
    return LittleFS.mkdir(path);
}

bool FileUtils::createDirectories(const String& path) {
    if (!fsInitialized || exists(path)) {
        return exists(path);
    }
    
    String normalizedPath = normalizePath(path);
    String currentPath = "";
    
    // 按 '/' 分割路径
    int start = 0;
    while (start < normalizedPath.length()) {
        int end = normalizedPath.indexOf('/', start);
        if (end == -1) {
            end = normalizedPath.length();
        }
        
        String segment = normalizedPath.substring(start, end);
        if (!segment.isEmpty()) {
            currentPath = currentPath.isEmpty() ? segment : currentPath + "/" + segment;
            if (!exists(currentPath)) {
                if (!createDirectory(currentPath)) {
                    return false;
                }
            }
        }
        
        start = end + 1;
    }
    
    return true;
}

bool FileUtils::deleteDirectory(const String& path) {
    if (!fsInitialized || !exists(path) || !isDirectory(path)) {
        return false;
    }
    
    return recursiveDeleteDirectory(path);
}

bool FileUtils::recursiveDeleteDirectory(const String& path) {
    File dir = LittleFS.open(path);
    if (!dir || !dir.isDirectory()) {
        return false;
    }
    
    File file = dir.openNextFile();
    while (file) {
        String fileName = file.name();
        String fullPath = path + "/" + fileName;
        
        if (file.isDirectory()) {
            if (!recursiveDeleteDirectory(fullPath)) {
                dir.close();
                return false;
            }
        } else {
            if (!LittleFS.remove(fullPath)) {
                dir.close();
                return false;
            }
        }
        
        file = dir.openNextFile();
    }
    
    dir.close();
    
    // 删除空目录
    return LittleFS.rmdir(path);
}

// ==================== 目录列表和搜索 ====================

std::vector<FileInfo> FileUtils::listDirectory(const String& path, bool recursive) {
    std::vector<FileInfo> results;
    
    if (!fsInitialized || !exists(path) || !isDirectory(path)) {
        return results;
    }
    
    if (recursive) {
        listDirectoryRecursive(path, results);
    } else {
        File dir = LittleFS.open(path);
        File file = dir.openNextFile();
        
        while (file) {
            FileInfo info;
            info.name = getFileName(file.name());
            info.path = file.name();
            info.size = file.size();
            info.isDirectory = file.isDirectory();
            info.modifiedTime = getModifiedTime(info.path);
            
            results.push_back(info);
            file = dir.openNextFile();
        }
        
        dir.close();
    }
    
    return results;
}

void FileUtils::listDirectoryRecursive(const String& path, std::vector<FileInfo>& results) {
    File dir = LittleFS.open(path);
    if (!dir || !dir.isDirectory()) {
        return;
    }
    
    File file = dir.openNextFile();
    while (file) {
        FileInfo info;
        info.name = getFileName(file.name());
        info.path = file.name();
        info.size = file.size();
        info.isDirectory = file.isDirectory();
        info.modifiedTime = getModifiedTime(info.path);
        
        results.push_back(info);
        
        if (file.isDirectory()) {
            listDirectoryRecursive(info.path, results);
        }
        
        file = dir.openNextFile();
    }
    
    dir.close();
}

std::vector<FileInfo> FileUtils::findFiles(const String& directory, const String& pattern, bool recursive) {
    std::vector<FileInfo> results;
    
    if (!fsInitialized || !exists(directory) || !isDirectory(directory)) {
        return results;
    }
    
    findFilesRecursive(directory, pattern, results, recursive);
    return results;
}

void FileUtils::findFilesRecursive(const String& directory, const String& pattern, 
                                  std::vector<FileInfo>& results, bool recursive) {
    File dir = LittleFS.open(directory);
    if (!dir || !dir.isDirectory()) {
        return;
    }
    
    File file = dir.openNextFile();
    while (file) {
        String fileName = getFileName(file.name());
        String fullPath = file.name();
        
        if (file.isDirectory()) {
            if (recursive) {
                findFilesRecursive(fullPath, pattern, results, recursive);
            }
        } else {
            // 使用模式匹配函数
            if (patternMatches(fileName, pattern)) {
                FileInfo info;
                info.name = fileName;
                info.path = fullPath;
                info.size = file.size();
                info.isDirectory = false;
                info.modifiedTime = getModifiedTime(fullPath);
                
                results.push_back(info);
            }
        }
        
        file = dir.openNextFile();
    }
    
    dir.close();
}

// ✅ 新增：模式匹配函数实现
bool FileUtils::patternMatches(const String& fileName, const String& pattern) {
    if (pattern == "*") return true;
    if (pattern.isEmpty()) return fileName.isEmpty();
    
    // 简单的通配符匹配实现
    // 支持 *（任意字符）和 ?（单个字符）
    int i = 0, j = 0;
    int starPos = -1, matchPos = -1;
    
    while (i < fileName.length()) {
        if (j < pattern.length() && (pattern[j] == fileName[i] || pattern[j] == '?')) {
            i++;
            j++;
        } else if (j < pattern.length() && pattern[j] == '*') {
            starPos = j;
            matchPos = i;
            j++;
        } else if (starPos != -1) {
            j = starPos + 1;
            i = ++matchPos;
        } else {
            return false;
        }
    }
    
    while (j < pattern.length() && pattern[j] == '*') {
        j++;
    }
    
    return j == pattern.length();
}

// ==================== 文件信息 ====================

FileInfo FileUtils::getFileInfo(const String& path) {
    FileInfo info;
    
    if (!fsInitialized || !exists(path)) {
        return info;
    }
    
    info.name = getFileName(path);
    info.path = path;
    info.isDirectory = isDirectory(path);
    info.size = info.isDirectory ? 0 : getFileSize(path);
    info.modifiedTime = getModifiedTime(path);
    
    return info;
}

// ==================== 目录大小计算（核心功能） ====================

size_t FileUtils::getDirectorySize(const String& path) {
    return calculateFolderSizeOptimized(path);
}

size_t FileUtils::calculateFolderSize(const String& folderPath) {
    if (!fsInitialized || !exists(folderPath)) {
        return 0;
    }
    
    return calculateFolderSizeRecursive(folderPath);
}

size_t FileUtils::calculateFolderSizeRecursive(const String& folderPath) {
    size_t totalSize = 0;
    
    File dir = LittleFS.open(folderPath);
    if (!dir || !dir.isDirectory()) {
        return 0;
    }
    
    File file = dir.openNextFile();
    while (file) {
        String fileName = file.name();
        String fullPath = fileName; // 已经是完整路径
        
        if (file.isDirectory()) {
            totalSize += calculateFolderSizeRecursive(fullPath);
        } else {
            totalSize += file.size();
        }
        
        file = dir.openNextFile();
    }
    
    dir.close();
    return totalSize;
}

size_t FileUtils::calculateFolderSizeOptimized(const String& folderPath) {
    if (!fsInitialized || !exists(folderPath)) {
        return 0;
    }
    
    size_t totalSize = 0;
    std::vector<String> directories;
    directories.push_back(folderPath);
    
    while (!directories.empty()) {
        String currentDir = directories.back();
        directories.pop_back();
        
        File dir = LittleFS.open(currentDir);
        if (!dir || !dir.isDirectory()) {
            continue;
        }
        
        File file = dir.openNextFile();
        while (file) {
            String fileName = file.name();
            String fullPath = fileName; // LittleFS 返回完整路径
            
            if (file.isDirectory()) {
                directories.push_back(fullPath);
            } else {
                totalSize += file.size();
            }
            
            file = dir.openNextFile();
        }
        dir.close();
    }
    
    return totalSize;
}

// ==================== 存储空间信息 ====================

size_t FileUtils::getFreeSpace() {
    if (!fsInitialized) return 0;
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

size_t FileUtils::getTotalSpace() {
    if (!fsInitialized) return 0;
    return LittleFS.totalBytes();
}

float FileUtils::getSpaceUsage() {
    if (!fsInitialized) return 0.0f;
    size_t total = LittleFS.totalBytes();
    if (total == 0) return 0.0f;
    return static_cast<float>(LittleFS.usedBytes()) / total;
}

// ==================== 文本文件操作 ====================

bool FileUtils::appendToFile(const String& path, const String& content) {
    return writeFile(path, content, true);
}

std::vector<String> FileUtils::readLines(const String& path) {
    std::vector<String> lines;
    
    if (!fsInitialized || !exists(path) || isDirectory(path)) {
        return lines;
    }
    
    File file = LittleFS.open(path, "r");
    if (!file) {
        return lines;
    }
    
    String line;
    while (file.available()) {
        line = file.readStringUntil('\n');
        line.trim(); // 去除换行符和空格
        if (!line.isEmpty()) {
            lines.push_back(line);
        }
    }
    
    file.close();
    return lines;
}

bool FileUtils::writeLines(const String& path, const std::vector<String>& lines) {
    if (!fsInitialized) return false;
    
    File file = LittleFS.open(path, "w");
    if (!file) {
        return false;
    }
    
    for (size_t i = 0; i < lines.size(); i++) {
        file.println(lines[i]);
    }
    
    file.close();
    return true;
}

// ==================== 路径操作 ====================

String FileUtils::getFileExtension(const String& path) {
    int dotIndex = path.lastIndexOf('.');
    if (dotIndex == -1) return "";
    
    int slashIndex = path.lastIndexOf('/');
    if (slashIndex > dotIndex) return "";
    
    return path.substring(dotIndex + 1);
}

String FileUtils::getDirectoryPath(const String& path) {
    int slashIndex = path.lastIndexOf('/');
    if (slashIndex == -1) return "";
    
    return path.substring(0, slashIndex);
}

String FileUtils::joinPath(const String& base, const String& part) {
    if (base.isEmpty()) return part;
    if (part.isEmpty()) return base;
    
    // 预分配足够空间: base + "/" + part
    String result;
    result.reserve(base.length() + part.length() + 2);
    result = base;
    
    // 确保 base 以 / 结尾
    if (!result.endsWith("/")) {
        result += "/";
    }
    
    // 如果 part 以 / 开头，去掉开头的 /
    if (part.startsWith("/")) {
        result += part.substring(1);
    } else {
        result += part;
    }
    
    // 规范化路径（移除多余的 //）
    return normalizePath(result);
}

String FileUtils::normalizePath(const String& path) {
    if (path.length() == 0 || path == "/") {
        return "/";
    }
    
    String result = path;
    
    // 确保以 '/' 开头
    if (result.charAt(0) != '/') {
        result = "/" + result;
    }
    
    // 移除末尾的 '/'
    if (result.length() > 1 && result.charAt(result.length() - 1) == '/') {
        result = result.substring(0, result.length() - 1);
    }
    
    return result;
}

String FileUtils::getFileName(const String& path) {
    if (path.length() == 0) {
        return "";
    }
    
    int lastSlash = path.lastIndexOf('/');
    if (lastSlash == -1) {
        return path;
    }
    
    // 如果以斜杠结尾，可能是目录，尝试获取上一级
    if (lastSlash == path.length() - 1) {
        String parent = path.substring(0, path.length() - 1);
        int prevSlash = parent.lastIndexOf('/');
        if (prevSlash == -1) {
            return parent;
        }
        return parent.substring(prevSlash + 1);
    }
    
    return path.substring(lastSlash + 1);
}

String FileUtils::ensureAbsolutePath(const String& path, const String& baseDir) {
    if (path.length() == 0) {
        return baseDir;
    }
    
    // 如果已经是绝对路径
    if (path.charAt(0) == '/') {
        return normalizePath(path);
    }
    
    // 相对路径，基于 baseDir 构建绝对路径
    String base = normalizePath(baseDir);
    if (base == "/") {
        return "/" + path;
    }
    return base + "/" + path;
}

// ==================== 文件系统信息输出 ====================
void FileUtils::listAllFiles(const String& path, int depth) {
    if (!fsInitialized) {
        LOG_ERROR("[FileUtils] File system not initialized");
        return;
    }

    String absPath = normalizePath(path);
    File dir = LittleFS.open(absPath);
    if (!dir || !dir.isDirectory()) {
        char buf[80];
        snprintf(buf, sizeof(buf), "[FileUtils] Cannot open directory: %s", absPath.c_str());
        LOG_ERROR(buf);
        return;
    }

    File file = dir.openNextFile();
    while (file) {
        String filePath    = file.name();
        String displayName = getFileName(filePath);
        char buf[96];

        if (file.isDirectory()) {
            snprintf(buf, sizeof(buf), "%*s[DIR] %s/", depth * 2, "", displayName.c_str());
            LOG_INFO(buf);
            String childPath = ensureAbsolutePath(filePath, absPath);
            FileUtils::listAllFiles(childPath, depth + 1);
        } else {
            snprintf(buf, sizeof(buf), "%*s%s (%u bytes)", depth * 2, "",
                     displayName.c_str(), (unsigned)file.size());
            LOG_INFO(buf);
        }
        file = dir.openNextFile();
    }
    dir.close();
}

String FileUtils::getFileSystemInfoJSON() {
    if (!fsInitialized) {
        return "{\"error\": \"File system not initialized\"}";
    }

    FastBeeJsonDocLarge doc;

    // 文件系统空间信息
    size_t total = getTotalSpace();
    size_t used  = total - getFreeSpace();
    doc["fs"]["total"]        = total;
    doc["fs"]["used"]         = used;
    doc["fs"]["free"]         = getFreeSpace();
    doc["fs"]["percent_used"] = getSpaceUsage() * 100;

    // 关键目录大小
    JsonObject sizes = doc["folder_sizes"].to<JsonObject>();
    const char* commonDirs[] = {"/www", "/config", "/logs", "/backup", "/tmp"};
    for (const char* dir : commonDirs) {
        if (exists(dir)) {
            sizes[String(dir).substring(1)] = calculateFolderSize(dir);
        }
    }

    // 文件统计
    std::vector<FileInfo> allFiles = listDirectory("/", true);
    int fileCount = 0, dirCount = 0;
    size_t totalSize = 0;
    for (const auto& f : allFiles) {
        if (f.isDirectory) { dirCount++; }
        else { fileCount++; totalSize += f.size; }
    }
    JsonObject stats     = doc["statistics"].to<JsonObject>();
    stats["files"]       = fileCount;
    stats["directories"] = dirCount;
    stats["total_size"]  = totalSize;
    stats["avg_file_size"] = fileCount > 0 ? totalSize / fileCount : 0;

    // 文件系统健康状态
    JsonObject health       = doc["health"].to<JsonObject>();
    health["initialized"]   = fsInitialized;
    health["space_warning"] = getSpaceUsage() > 0.8f;
    health["space_critical"]= getSpaceUsage() > 0.9f;
    health["has_www"]       = exists("/www");
    health["has_config"]    = exists("/config");

    // 最多列出 10 个文件
    JsonArray recentFiles = doc["recent_files"].to<JsonArray>();
    int count = 0;
    for (const auto& f : allFiles) {
        if (!f.isDirectory && count < 10) {
            JsonObject fi = recentFiles.add<JsonObject>();
            fi["name"] = f.name;
            fi["size"] = f.size;
            fi["path"] = f.path;
            count++;
        }
    }

    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

// ==================== 暂未实现的方法（占位符） ====================

String FileUtils::calculateFileHash(const String& path) {
    return "";
}

bool FileUtils::verifyFileIntegrity(const String& path, const String& expectedHash) {
    return calculateFileHash(path) == expectedHash;
}

String FileUtils::createTempFile(const String& prefix, const String& content) {
    static int counter = 0;
    String tempPath = "/tmp/" + prefix + "_" + String(millis()) + "_" + String(counter++);
    if (!content.isEmpty()) {
        writeFile(tempPath, content);
    }
    return tempPath;
}

int FileUtils::cleanupTempFiles(const String& tempDir, time_t olderThan) {
    return 0;
}

String FileUtils::backupFile(const String& sourcePath, const String& backupDir) {
    if (!exists(sourcePath)) return "";
    String backupName = getFileName(sourcePath) + "." + String(millis()) + ".bak";
    String backupPath = joinPath(backupDir, backupName);
    return copyFile(sourcePath, backupPath) ? backupPath : "";
}

bool FileUtils::restoreBackup(const String& backupPath, const String& restorePath) {
    return copyFile(backupPath, restorePath);
}

int FileUtils::cleanupOldBackups(const String& backupDir, int keepCount) {
    return 0;
}