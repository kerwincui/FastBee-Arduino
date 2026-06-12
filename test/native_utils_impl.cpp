#ifdef UNIT_TEST

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>

#include <Arduino.h>
#include <LittleFS.h>
#include <MD5Builder.h>

#include "utils/FileUtils.h"
#include "utils/TimeUtils.h"

bool FileUtils::fsInitialized = false;

static std::string fbToStdString(const String& value) {
    return static_cast<std::string>(value);
}

static String fbFromStdString(const std::string& value) {
    return String(value);
}

static std::string fbTrimLineEnd(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
        value.pop_back();
    }
    return value;
}

static tm fbLocalTime(time_t timestamp) {
    tm out = {};
#if defined(_WIN32)
    localtime_s(&out, &timestamp);
#else
    localtime_r(&timestamp, &out);
#endif
    return out;
}

bool FileUtils::initialize(bool formatIfFailed) {
    if (fsInitialized) {
        return true;
    }
    fsInitialized = LittleFS.begin(formatIfFailed);
    return fsInitialized;
}

bool FileUtils::exists(const String& path) {
    return fsInitialized && LittleFS.exists(path);
}

bool FileUtils::isDirectory(const String& path) {
    if (!fsInitialized) {
        return false;
    }
    File file = LittleFS.open(path);
    bool result = file && file.isDirectory();
    file.close();
    return result;
}

size_t FileUtils::getFileSize(const String& path) {
    if (!exists(path) || isDirectory(path)) {
        return 0;
    }
    File file = LittleFS.open(path, "r");
    size_t result = file ? file.size() : 0;
    file.close();
    return result;
}

time_t FileUtils::getModifiedTime(const String&) {
    return 0;
}

String FileUtils::readFile(const String& path) {
    if (!exists(path) || isDirectory(path)) {
        return "";
    }
    File file = LittleFS.open(path, "r");
    String result = file ? file.readString() : String("");
    file.close();
    return result;
}

size_t FileUtils::readFileToBuffer(const String& path, uint8_t* buffer, size_t bufferSize) {
    if (!buffer || !exists(path) || isDirectory(path)) {
        return 0;
    }
    File file = LittleFS.open(path, "r");
    size_t result = file ? file.read(buffer, bufferSize) : 0;
    file.close();
    return result;
}

bool FileUtils::writeFile(const String& path, const String& content, bool append) {
    if (!fsInitialized) {
        return false;
    }
    File file = LittleFS.open(path, append ? "a" : "w");
    if (!file) {
        return false;
    }
    size_t written = file.print(content);
    file.close();
    return written == content.length();
}

bool FileUtils::writeFileFromBuffer(const String& path, const uint8_t* buffer, size_t size, bool append) {
    if (!fsInitialized || !buffer) {
        return false;
    }
    File file = LittleFS.open(path, append ? "a" : "w");
    if (!file) {
        return false;
    }
    size_t written = file.write(buffer, size);
    file.close();
    return written == size;
}

bool FileUtils::deleteFile(const String& path) {
    if (!exists(path)) {
        return false;
    }
    return LittleFS.remove(path);
}

bool FileUtils::renameFile(const String& oldPath, const String& newPath) {
    if (!exists(oldPath) || exists(newPath)) {
        return false;
    }
    return LittleFS.rename(oldPath, newPath);
}

bool FileUtils::atomicWriteFile(const String& path, const String& content) {
    if (!fsInitialized) {
        return false;
    }
    String tmpPath = path + ".tmp";
    if (!writeFile(tmpPath, content, false)) {
        LittleFS.remove(tmpPath);
        return false;
    }
    LittleFS.remove(path);
    return LittleFS.rename(tmpPath, path);
}

bool FileUtils::copyFile(const String& sourcePath, const String& destPath) {
    if (!exists(sourcePath) || exists(destPath) || isDirectory(sourcePath)) {
        return false;
    }
    return writeFile(destPath, readFile(sourcePath), false);
}

bool FileUtils::moveFile(const String& sourcePath, const String& destPath) {
    if (!copyFile(sourcePath, destPath)) {
        return false;
    }
    return deleteFile(sourcePath);
}

bool FileUtils::createDirectory(const String&) {
    return fsInitialized;
}

bool FileUtils::createDirectories(const String&) {
    return fsInitialized;
}

bool FileUtils::deleteDirectory(const String& path) {
    return fsInitialized && LittleFS.rmdir(path);
}

std::vector<FileInfo> FileUtils::listDirectory(const String&, bool) {
    return {};
}

std::vector<FileInfo> FileUtils::findFiles(const String&, const String&, bool) {
    return {};
}

FileInfo FileUtils::getFileInfo(const String& path) {
    FileInfo info;
    if (exists(path)) {
        info.name = getFileName(path);
        info.path = path;
        info.size = getFileSize(path);
        info.isDirectory = isDirectory(path);
        info.modifiedTime = getModifiedTime(path);
    }
    return info;
}

size_t FileUtils::getDirectorySize(const String&) {
    return 0;
}

size_t FileUtils::getFreeSpace() {
    if (!fsInitialized) {
        return 0;
    }
    return LittleFS.totalBytes() - LittleFS.usedBytes();
}

size_t FileUtils::getTotalSpace() {
    return fsInitialized ? LittleFS.totalBytes() : 0;
}

float FileUtils::getSpaceUsage() {
    size_t total = getTotalSpace();
    if (total == 0) {
        return 0.0f;
    }
    return static_cast<float>(LittleFS.usedBytes()) / static_cast<float>(total);
}

String FileUtils::backupFile(const String& sourcePath, const String& backupDir) {
    if (!exists(sourcePath)) {
        return "";
    }
    String backupPath = joinPath(backupDir, getFileName(sourcePath) + ".bak");
    return copyFile(sourcePath, backupPath) ? backupPath : String("");
}

bool FileUtils::restoreBackup(const String& backupPath, const String& restorePath) {
    if (!exists(backupPath)) {
        return false;
    }
    if (exists(restorePath)) {
        deleteFile(restorePath);
    }
    return copyFile(backupPath, restorePath);
}

int FileUtils::cleanupOldBackups(const String&, int) {
    return 0;
}

String FileUtils::calculateFileHash(const String& path) {
    if (!exists(path) || isDirectory(path)) {
        return "";
    }
    MD5Builder md5;
    md5.begin();
    md5.add(readFile(path));
    md5.calculate();
    return md5.toString();
}

bool FileUtils::verifyFileIntegrity(const String& path, const String& expectedHash) {
    String hash = calculateFileHash(path);
    return !hash.isEmpty() && hash == expectedHash;
}

bool FileUtils::appendToFile(const String& path, const String& content) {
    return writeFile(path, content, true);
}

std::vector<String> FileUtils::readLines(const String& path) {
    std::vector<String> lines;
    std::string content = fbToStdString(readFile(path));
    std::stringstream input(content);
    std::string line;
    while (std::getline(input, line)) {
        line = fbTrimLineEnd(line);
        if (!line.empty()) {
            lines.push_back(fbFromStdString(line));
        }
    }
    return lines;
}

bool FileUtils::writeLines(const String& path, const std::vector<String>& lines) {
    std::string content;
    for (size_t i = 0; i < lines.size(); i += 1) {
        if (i > 0) {
            content += '\n';
        }
        content += fbToStdString(lines[i]);
    }
    return writeFile(path, fbFromStdString(content), false);
}

String FileUtils::createTempFile(const String& prefix, const String& content) {
    static int counter = 0;
    String path = joinPath("/tmp", prefix + "_" + String(counter++));
    if (!content.isEmpty()) {
        writeFile(path, content, false);
    }
    return path;
}

int FileUtils::cleanupTempFiles(const String&, time_t) {
    return 0;
}

String FileUtils::getFileExtension(const String& path) {
    std::string value = fbToStdString(path);
    size_t slash = value.find_last_of('/');
    size_t dot = value.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && slash > dot)) {
        return "";
    }
    return fbFromStdString(value.substr(dot + 1));
}

String FileUtils::getFileName(const String& path) {
    std::string value = fbToStdString(path);
    if (value.empty()) {
        return "";
    }
    while (value.size() > 1 && value.back() == '/') {
        value.pop_back();
    }
    size_t slash = value.find_last_of('/');
    return slash == std::string::npos ? fbFromStdString(value) : fbFromStdString(value.substr(slash + 1));
}

String FileUtils::getDirectoryPath(const String& path) {
    std::string value = fbToStdString(path);
    size_t slash = value.find_last_of('/');
    if (slash == std::string::npos) {
        return "";
    }
    return fbFromStdString(value.substr(0, slash));
}

String FileUtils::joinPath(const String& base, const String& part) {
    std::string left = fbToStdString(base);
    std::string right = fbToStdString(part);
    if (left.empty()) {
        return part;
    }
    if (right.empty()) {
        return base;
    }
    while (!left.empty() && left.back() == '/') {
        left.pop_back();
    }
    while (!right.empty() && right.front() == '/') {
        right.erase(right.begin());
    }
    return normalizePath(fbFromStdString(left + "/" + right));
}

String FileUtils::normalizePath(const String& path) {
    std::string value = fbToStdString(path);
    if (value.empty() || value == "/") {
        return "/";
    }
    if (value.front() != '/') {
        value.insert(value.begin(), '/');
    }
    std::string normalized;
    normalized.reserve(value.size());
    bool previousSlash = false;
    for (char ch : value) {
        if (ch == '/') {
            if (!previousSlash) {
                normalized.push_back(ch);
            }
            previousSlash = true;
        } else {
            normalized.push_back(ch);
            previousSlash = false;
        }
    }
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return fbFromStdString(normalized);
}

size_t FileUtils::calculateFolderSize(const String&) {
    return 0;
}

size_t FileUtils::calculateFolderSizeOptimized(const String&) {
    return 0;
}

bool FileUtils::readJsonFile(const String&, JsonDocument&) {
    return false;
}

String FileUtils::getFileSystemInfoJSON() {
    return "{}";
}

void FileUtils::listAllFiles(const String&, int) {}

bool FileUtils::recursiveDeleteDirectory(const String&) {
    return true;
}

void FileUtils::findFilesRecursive(const String&, const String&, std::vector<FileInfo>&, bool) {}

void FileUtils::listDirectoryRecursive(const String&, std::vector<FileInfo>&) {}

size_t FileUtils::calculateFolderSizeRecursive(const String&) {
    return 0;
}

bool FileUtils::patternMatches(const String& fileName, const String& pattern) {
    return pattern == "*" || fileName == pattern;
}

String FileUtils::ensureAbsolutePath(const String& path, const String& baseDir) {
    if (path.isEmpty()) {
        return normalizePath(baseDir);
    }
    std::string value = fbToStdString(path);
    if (!value.empty() && value.front() == '/') {
        return normalizePath(path);
    }
    return joinPath(baseDir, path);
}

bool TimeUtils::initialize(const TimeZone& timezone, const String&) {
    return setTimeZone(timezone);
}

bool TimeUtils::syncNTP(unsigned long) {
    return true;
}

bool TimeUtils::syncNTPFromHTTP(const String&, unsigned long) {
    return false;
}

bool TimeUtils::syncNTPFromHTTPWithTimestamp(const String&, long long& outTimestampMs, unsigned long) {
    outTimestampMs = 0;
    return false;
}

time_t TimeUtils::getTimestamp() {
    return std::time(nullptr);
}

unsigned long TimeUtils::getTimestampMs() {
    return getTimestamp() * 1000UL;
}

unsigned long TimeUtils::getUptime() {
    return millis();
}

String TimeUtils::formatTime(time_t timestamp, TimeFormat format) {
    if (timestamp == 0) {
        timestamp = getTimestamp();
    }
    tm value = fbLocalTime(timestamp);
    char buffer[64] = {};
    switch (format) {
        case ISO8601:
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &value);
            break;
        case RFC822:
            std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S", &value);
            break;
        case HUMAN_READABLE:
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &value);
            break;
        case TIME_ONLY:
            std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &value);
            break;
        case DATE_ONLY:
            std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &value);
            break;
    }
    return String(buffer);
}

String TimeUtils::formatCurrentTime(TimeFormat format) {
    return formatTime(getTimestamp(), format);
}

time_t TimeUtils::parseTime(const String& timeString, TimeFormat format) {
    tm value = {};
    if (format == ISO8601) {
        std::sscanf(timeString.c_str(), "%d-%d-%dT%d:%d:%dZ",
            &value.tm_year, &value.tm_mon, &value.tm_mday,
            &value.tm_hour, &value.tm_min, &value.tm_sec);
        value.tm_year -= 1900;
        value.tm_mon -= 1;
    } else if (format == HUMAN_READABLE || format == DATE_ONLY) {
        std::sscanf(timeString.c_str(), "%d-%d-%d",
            &value.tm_year, &value.tm_mon, &value.tm_mday);
        value.tm_year -= 1900;
        value.tm_mon -= 1;
    } else if (format == TIME_ONLY) {
        time_t now = getTimestamp();
        value = fbLocalTime(now);
        std::sscanf(timeString.c_str(), "%d:%d:%d", &value.tm_hour, &value.tm_min, &value.tm_sec);
    }
    return std::mktime(&value);
}

tm TimeUtils::getTimeStruct(time_t timestamp) {
    if (timestamp == 0) {
        timestamp = getTimestamp();
    }
    return fbLocalTime(timestamp);
}

bool TimeUtils::setTimeZone(const TimeZone&) {
    return true;
}

TimeUtils::TimeZone TimeUtils::getCurrentTimeZone() {
    return TimeZone("UTC", 0, false);
}

time_t TimeUtils::addSeconds(time_t timestamp, long seconds) {
    return timestamp + seconds;
}

time_t TimeUtils::addMinutes(time_t timestamp, long minutes) {
    return timestamp + minutes * 60;
}

time_t TimeUtils::addHours(time_t timestamp, long hours) {
    return timestamp + hours * 3600;
}

time_t TimeUtils::addDays(time_t timestamp, long days) {
    return timestamp + days * 86400;
}

long TimeUtils::timeDifference(time_t time1, time_t time2) {
    return static_cast<long>(std::difftime(time1, time2));
}

bool TimeUtils::isSameDay(time_t time1, time_t time2) {
    tm left = getTimeStruct(time1);
    tm right = getTimeStruct(time2);
    return left.tm_year == right.tm_year && left.tm_mon == right.tm_mon && left.tm_mday == right.tm_mday;
}

time_t TimeUtils::getDayStart(time_t timestamp) {
    tm value = getTimeStruct(timestamp);
    value.tm_hour = 0;
    value.tm_min = 0;
    value.tm_sec = 0;
    return std::mktime(&value);
}

time_t TimeUtils::getDayEnd(time_t timestamp) {
    tm value = getTimeStruct(timestamp);
    value.tm_hour = 23;
    value.tm_min = 59;
    value.tm_sec = 59;
    return std::mktime(&value);
}

int TimeUtils::getDayOfWeek(time_t timestamp) {
    return getTimeStruct(timestamp).tm_wday;
}

int TimeUtils::getDayOfMonth(time_t timestamp) {
    return getTimeStruct(timestamp).tm_mday;
}

int TimeUtils::getDayOfYear(time_t timestamp) {
    return getTimeStruct(timestamp).tm_yday + 1;
}

String TimeUtils::formatDuration(unsigned long milliseconds, bool showMilliseconds) {
    unsigned long seconds = milliseconds / 1000;
    unsigned long minutes = seconds / 60;
    unsigned long hours = minutes / 60;
    unsigned long days = hours / 24;
    seconds %= 60;
    minutes %= 60;
    hours %= 24;

    std::ostringstream out;
    if (days > 0) {
        out << days << "d ";
    }
    if (hours > 0 || days > 0) {
        out << hours << "h ";
    }
    if (minutes > 0 || hours > 0 || days > 0) {
        out << minutes << "m ";
    }
    if (showMilliseconds) {
        out << seconds << "." << std::setw(3) << std::setfill('0') << (milliseconds % 1000) << "s";
    } else {
        out << seconds << "s";
    }
    return fbFromStdString(out.str());
}

int TimeUtils::setTimeout(unsigned long, void (*)()) {
    static int nextId = 1;
    return nextId++;
}

int TimeUtils::setInterval(unsigned long, void (*)()) {
    static int nextId = 1000;
    return nextId++;
}

void TimeUtils::clearTimer(int) {}

bool TimeUtils::isTimeInRange(time_t checkTime, time_t startTime, time_t endTime) {
    return checkTime >= startTime && checkTime <= endTime;
}

TimeUtils::TimeZone TimeUtils::getTimeZoneByName(const String& name) {
    if (name == "UTC") return TimeZone("UTC", 0);
    if (name == "EST") return TimeZone("EST", -5 * 60);
    if (name == "CST") return TimeZone("CST", -6 * 60);
    if (name == "PST") return TimeZone("PST", -8 * 60);
    if (name == "CET") return TimeZone("CET", 60, true);
    if (name == "CST8") return TimeZone("CST8", 8 * 60);
    return TimeZone("UTC", 0);
}

bool TimeUtils::isLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int TimeUtils::getDaysInMonth(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) {
        return 0;
    }
    if (month == 2 && isLeapYear(year)) {
        return 29;
    }
    return days[month - 1];
}

unsigned long TimeUtils::getCPUTime() {
    return micros();
}

#endif
