/**
 * @file MockOTA.h
 * @brief OTA升级模拟对象
 * 
 * 提供OTA固件升级功能的模拟实现
 */

#ifndef MOCK_OTA_H
#define MOCK_OTA_H

#include <Arduino.h>
#include <functional>

// OTA状态枚举
enum class OTAState {
    OTA_IDLE = 0,           // 空闲
    OTA_STARTING = 1,       // 开始
    OTA_DOWNLOADING = 2,    // 下载中
    OTA_VERIFYING = 3,      // 验证中
    OTA_WRITING = 4,        // 写入中
    OTA_COMPLETED = 5,      // 完成
    OTA_FAILED = 6          // 失败
};

// OTA错误码
enum class OTAError {
    OTA_OK = 0,
    OTA_ERROR_NO_SPACE = 1,
    OTA_ERROR_WRITE_FAILED = 2,
    OTA_ERROR_VERIFY_FAILED = 3,
    OTA_ERROR_NETWORK = 4,
    OTA_ERROR_SERVER = 5,
    OTA_ERROR_TIMEOUT = 6,
    OTA_ERROR_CANCELED = 7
};

// OTA配置结构
struct OTAConfig {
    bool enabled;
    String serverUrl;
    int checkInterval;      // 检查间隔（小时）
    bool autoUpdate;        // 自动更新
    String currentVersion;
    
    OTAConfig() : enabled(false), checkInterval(24), autoUpdate(false) {}
};

// OTA进度回调类型
typedef std::function<void(int progress, int total)> OTAProgressCallback;
typedef std::function<void(OTAState state, OTAError error)> OTAStateCallback;

// 模拟OTA管理器
class MockOTAManager {
public:
    MockOTAManager(void* client = nullptr) 
        : _client(client), _state(OTAState::OTA_IDLE),
          _error(OTAError::OTA_OK), _progress(0),
          _totalSize(0), _writtenSize(0),
          _inProgress(false), _shouldFail(false),
          _failAtPercent(-1) {}

    bool initialize() {
        _state = OTAState::OTA_IDLE;
        _progress = 0;
        _inProgress = false;
        return true;
    }

    // OTA状态
    bool isOTAInProgress() {
        return _inProgress;
    }

    OTAState getState() {
        return _state;
    }

    OTAError getError() {
        return _error;
    }

    int getProgress() {
        return _progress;
    }

    // 开始OTA
    bool beginOTA(size_t firmwareSize) {
        if (_inProgress) return false;
        
        _totalSize = firmwareSize;
        _writtenSize = 0;
        _progress = 0;
        _state = OTAState::OTA_STARTING;
        _error = OTAError::OTA_OK;
        _inProgress = true;
        
        // 检查空间
        if (firmwareSize > getMaxSketchSpace()) {
            _state = OTAState::OTA_FAILED;
            _error = OTAError::OTA_ERROR_NO_SPACE;
            _inProgress = false;
            return false;
        }
        
        _state = OTAState::OTA_WRITING;
        return true;
    }

    // 写入数据
    size_t writeData(const uint8_t* data, size_t len) {
        if (!_inProgress) return 0;
        if (_state != OTAState::OTA_WRITING) return 0;
        
        // 模拟写入
        _writtenSize += len;
        
        // 更新进度
        if (_totalSize > 0) {
            _progress = (_writtenSize * 100) / _totalSize;
        }
        
        // 检查是否应该在特定百分比失败
        if (_failAtPercent > 0 && _progress >= _failAtPercent) {
            _state = OTAState::OTA_FAILED;
            _error = OTAError::OTA_ERROR_WRITE_FAILED;
            _inProgress = false;
            return 0;
        }
        
        // 调用进度回调
        if (_progressCallback) {
            _progressCallback(_writtenSize, _totalSize);
        }
        
        return len;
    }

    // 结束OTA
    bool endOTA() {
        if (!_inProgress) return false;
        
        if (_shouldFail) {
            _state = OTAState::OTA_FAILED;
            _error = OTAError::OTA_ERROR_VERIFY_FAILED;
            _inProgress = false;
            return false;
        }
        
        // 验证完整性
        if (_writtenSize != _totalSize) {
            _state = OTAState::OTA_FAILED;
            _error = OTAError::OTA_ERROR_VERIFY_FAILED;
            _inProgress = false;
            return false;
        }
        
        _state = OTAState::OTA_COMPLETED;
        _inProgress = false;
        
        // 调用状态回调
        if (_stateCallback) {
            _stateCallback(_state, _error);
        }
        
        return true;
    }

    // 取消OTA
    void cancelOTA() {
        _state = OTAState::OTA_FAILED;
        _error = OTAError::OTA_ERROR_CANCELED;
        _inProgress = false;
    }

    // URL升级
    bool startOTAFromURL(const String& url) {
        if (_inProgress) return false;
        
        _url = url;
        _state = OTAState::OTA_DOWNLOADING;
        _inProgress = true;
        
        // 模拟下载过程
        if (_shouldFail) {
            _state = OTAState::OTA_FAILED;
            _error = OTAError::OTA_ERROR_NETWORK;
            _inProgress = false;
            return false;
        }
        
        // 假设固件大小为100KB
        return beginOTA(102400);
    }

    // 文件上传升级
    bool handleUploadChunk(const uint8_t* data, size_t len, bool isLast) {
        if (!_inProgress) {
            // 首次上传，开始OTA
            if (!beginOTA(0)) return false;  // 大小未知
        }
        
        size_t written = writeData(data, len);
        
        if (isLast) {
            return endOTA();
        }
        
        return written == len;
    }

    // 设置回调
    void setProgressCallback(OTAProgressCallback callback) {
        _progressCallback = callback;
    }

    void setStateCallback(OTAStateCallback callback) {
        _stateCallback = callback;
    }

    // 获取状态JSON
    String getStatusJSON() {
        String json = "{";
        json += "\"state\":" + String((int)_state) + ",";
        json += "\"progress\":" + String(_progress) + ",";
        json += "\"error\":" + String((int)_error) + ",";
        json += "\"inProgress\":" + String(_inProgress ? "true" : "false") + ",";
        json += "\"written\":" + String(_writtenSize) + ",";
        json += "\"total\":" + String(_totalSize);
        json += "}";
        return json;
    }

    // 配置
    void setConfig(const OTAConfig& config) {
        _config = config;
    }

    OTAConfig getConfig() {
        return _config;
    }

    // 版本检查（模拟）
    bool checkForUpdate(String& newVersion, String& releaseNotes) {
        if (!_config.enabled) return false;
        
        // 模拟发现新版本
        newVersion = "2.0.0";
        releaseNotes = "Bug fixes and improvements";
        return true;
    }

    // 获取最大可用空间
    size_t getMaxSketchSpace() {
        return 1310720;  // 1.25MB（ESP32典型值）
    }

    // 测试控制方法
    void setShouldFail(bool fail) {
        _shouldFail = fail;
    }

    void setFailAtPercent(int percent) {
        _failAtPercent = percent;
    }

    void setProgress(int progress) {
        _progress = constrain(progress, 0, 100);
    }

    void reset() {
        _state = OTAState::OTA_IDLE;
        _error = OTAError::OTA_OK;
        _progress = 0;
        _inProgress = false;
        _writtenSize = 0;
        _totalSize = 0;
    }

private:
    void* _client;
    OTAState _state;
    OTAError _error;
    int _progress;
    size_t _totalSize;
    size_t _writtenSize;
    bool _inProgress;
    bool _shouldFail;
    int _failAtPercent;
    String _url;
    OTAConfig _config;
    OTAProgressCallback _progressCallback;
    OTAStateCallback _stateCallback;
};

// 模拟OTA路由处理器
class MockOTARouteHandler {
public:
    MockOTARouteHandler(MockOTAManager* otaMgr = nullptr)
        : _otaMgr(otaMgr) {}

    void setOTAManager(MockOTAManager* otaMgr) {
        _otaMgr = otaMgr;
    }

    // 处理OTA状态查询
    String handleOtaStatus() {
        if (!_otaMgr) return "{\"error\":\"OTA manager not available\"}";
        return _otaMgr->getStatusJSON();
    }

    // 处理OTA URL请求
    bool handleOtaUrl(const String& url) {
        if (!_otaMgr) return false;
        return _otaMgr->startOTAFromURL(url);
    }

    // 处理OTA上传
    bool handleOtaUpload(const uint8_t* data, size_t len, bool isLast) {
        if (!_otaMgr) return false;
        return _otaMgr->handleUploadChunk(data, len, isLast);
    }

private:
    MockOTAManager* _otaMgr;
};

#endif // MOCK_OTA_H
