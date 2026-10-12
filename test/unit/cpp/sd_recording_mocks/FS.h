#pragma once
#include <Arduino.h>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
constexpr const char* FILE_READ = "r";
constexpr const char* FILE_WRITE = "w";
constexpr const char* FILE_APPEND = "a";
struct RecordedFile { bool directory = false; std::string bytes; };
inline std::map<std::string, std::shared_ptr<RecordedFile>> recordedFiles;
inline size_t recordingWriteLimit = static_cast<size_t>(-1);
inline bool recordingFlushError = false, recordingDeleteOk = true, recordingOpenOk = true;
inline unsigned recordingWrites = 0, recordingDeletes = 0;
class File {
public:
    File() = default;
    File(std::shared_ptr<RecordedFile> data, const std::string& path, bool append = false)
        : data_(std::move(data)), path_(path), position_(append && data_ ? data_->bytes.size() : 0) {}
    explicit operator bool() const { return static_cast<bool>(data_); }
    bool isDirectory() const { return data_ && data_->directory; }
    size_t size() const { return data_ ? data_->bytes.size() : 0; }
    const char* path() const { return path_.c_str(); }
    void close() { data_.reset(); }
    void flush() {}
    int getWriteError() const { return recordingFlushError ? 1 : 0; }
    int available() const { return data_ && position_ < data_->bytes.size(); }
    int read() { return available() ? static_cast<unsigned char>(data_->bytes[position_++]) : -1; }
    size_t write(const uint8_t* data, size_t length) {
        ++recordingWrites;
        if (!data_ || data_->directory) return 0;
        length = std::min(length, recordingWriteLimit);
        if (position_ + length > data_->bytes.size()) data_->bytes.resize(position_ + length);
        std::copy(data, data + length, data_->bytes.begin() + position_); position_ += length;
        return length;
    }
    File openNextFile() {
        if (!isDirectory()) return {};
        size_t i = 0;
        for (const auto& entry : recordedFiles) {
            if (entry.first == path_ || entry.first.rfind(path_ + "/", 0) != 0) continue;
            if (i++ == child_) { ++child_; return File(entry.second, entry.first); }
        }
        return {};
    }
private:
    std::shared_ptr<RecordedFile> data_;
    std::string path_;
    size_t position_ = 0, child_ = 0;
};
