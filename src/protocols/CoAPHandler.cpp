/**
 * @description: CoAP 协议处理器 - RFC 7252 合规实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:30:40
 *
 * 支持: CON/NON/ACK/RST 消息类型、Token 匹配、Option 编解码、
 *       指数退避重传、消息去重
 */

#include "protocols/CoAPHandler.h"

#if FASTBEE_ENABLE_COAP

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <esp_random.h>

CoAPHandler::CoAPHandler() 
    : isInitialized(false), messageId(0), recentMsgIdIndex(0) {
    memset(pendingMessages, 0, sizeof(pendingMessages));
    memset(recentMessageIds, 0, sizeof(recentMessageIds));
}

CoAPHandler::~CoAPHandler() {
    end();
}

bool CoAPHandler::loadConfigFromLittleFS(const char* configPath) {
    if (!LittleFS.begin()) {
        LOG_ERROR("CoAP: Failed to mount LittleFS");
        return false;
    }
    
    File configFile = LittleFS.open(configPath, "r");
    if (!configFile) {
        LOG_ERRORF("CoAP: Failed to open config: %s", configPath);
        LittleFS.end();
        return false;
    }
    
    size_t size = configFile.size();
    if (size == 0) {
        LOG_WARNING("CoAP: Config file is empty");
        configFile.close();
        LittleFS.end();
        return false;
    }
    
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, configFile);
    configFile.close();
    LittleFS.end();
    
    if (error) {
        LOG_ERRORF("CoAP: Config parse error: %s", error.c_str());
        return false;
    }
    
    config.server = doc["server"] | "coap.me";
    config.port = doc["port"] | 5683;
    config.localPort = doc["localPort"] | 5683;
    config.defaultMethod = doc["defaultMethod"] | "POST";
    config.timeout = doc["timeout"] | 5000;
    config.retransmitCount = doc["retransmitCount"] | 3;
    
    if (doc.containsKey("resources")) {
        JsonObject resources = doc["resources"];
        for (JsonPair resource : resources) {
            config.resourceMap[String(resource.key().c_str())] = String(resource.value().as<const char*>());
        }
    }
    
    LOG_INFO("CoAP: Configuration loaded from LittleFS");
    LOG_INFOF("  Server: %s:%d, Local: %d", config.server.c_str(), config.port, config.localPort);
    LOG_INFOF("  Method: %s, Timeout: %lums, Retransmit: %d",
              config.defaultMethod.c_str(), config.timeout, config.retransmitCount);
    
    return true;
}

bool CoAPHandler::begin(const CoAPConfig& config) {
    this->config = config;
    
    if (udp.begin(config.localPort)) {
        isInitialized = true;
        // 初始化随机 Message ID 起始值
        messageId = (uint16_t)(esp_random() & 0xFFFF);
        LOG_INFOF("CoAP: Initialized on port %d", config.localPort);
        return true;
    }
    
    LOG_ERROR("CoAP: Failed to bind UDP port");
    return false;
}

bool CoAPHandler::beginFromConfig(const char* configPath) {
    if (!loadConfigFromLittleFS(configPath)) {
        return false;
    }
    return begin(config);
}

void CoAPHandler::end() {
    if (isInitialized) {
        udp.stop();
        isInitialized = false;
        memset(pendingMessages, 0, sizeof(pendingMessages));
        LOG_INFO("CoAP: Stopped");
    }
}

bool CoAPHandler::send(const String& resource, const String& data) {
    if (!isInitialized) return false;
    
    CoAPMethod method = CoAPMethod::POST;
    if (config.defaultMethod == "GET") method = CoAPMethod::GET;
    else if (config.defaultMethod == "PUT") method = CoAPMethod::PUT;
    else if (config.defaultMethod == "DELETE") method = CoAPMethod::DELETE;
    
    String actualResource = resource;
    if (config.resourceMap.find(resource) != config.resourceMap.end()) {
        actualResource = config.resourceMap[resource];
    }
    
    return sendCoAPMessage(CoAPType::CON, method, actualResource, data);
}

bool CoAPHandler::get(const String& resource, const String& query) {
    if (!isInitialized) return false;
    
    String fullResource = resource;
    if (!query.isEmpty()) {
        fullResource += "?" + query;
    }
    
    return sendCoAPMessage(CoAPType::CON, CoAPMethod::GET, fullResource, "");
}

bool CoAPHandler::put(const String& resource, const String& data) {
    if (!isInitialized) return false;
    return sendCoAPMessage(CoAPType::CON, CoAPMethod::PUT, resource, data);
}

bool CoAPHandler::post(const String& resource, const String& data) {
    if (!isInitialized) return false;
    return sendCoAPMessage(CoAPType::CON, CoAPMethod::POST, resource, data);
}

bool CoAPHandler::del(const String& resource) {
    if (!isInitialized) return false;
    return sendCoAPMessage(CoAPType::CON, CoAPMethod::DELETE, resource, "");
}

void CoAPHandler::handle() {
    if (!isInitialized) return;
    
    // 处理接收的数据包
    int packetSize = udp.parsePacket();
    if (packetSize) {
        processCoAPPacket();
    }
    
    // 检查重传
    checkRetransmissions();
}

String CoAPHandler::getStatus() const {
    if (!isInitialized) return "Not initialized";
    
    int activePending = 0;
    for (int i = 0; i < MAX_PENDING_MESSAGES; i++) {
        if (pendingMessages[i].active) activePending++;
    }
    
    char buf[64];
    snprintf(buf, sizeof(buf), "Initialized, pending: %d/%d", activePending, MAX_PENDING_MESSAGES);
    return String(buf);
}

void CoAPHandler::setMessageCallback(std::function<void(const String&, const String&)> callback) {
    this->messageCallback = callback;
}

const CoAPConfig& CoAPHandler::getConfig() const {
    return config;
}

// ============ RFC 7252 Option 编码 ============

uint16_t CoAPHandler::encodeOption(uint8_t* buffer, uint16_t prevOptionNumber,
                                    uint16_t optionNumber, const uint8_t* value, uint16_t valueLength) {
    uint16_t delta = optionNumber - prevOptionNumber;
    uint16_t index = 0;
    
    // 编码 delta 和 length 的高低4位
    uint8_t deltaField, lengthField;
    
    // Delta 编码
    if (delta < 13) {
        deltaField = (uint8_t)delta;
    } else if (delta < 269) {
        deltaField = 13;
    } else {
        deltaField = 14;
    }
    
    // Length 编码
    if (valueLength < 13) {
        lengthField = (uint8_t)valueLength;
    } else if (valueLength < 269) {
        lengthField = 13;
    } else {
        lengthField = 14;
    }
    
    // 写入首字节
    buffer[index++] = (deltaField << 4) | lengthField;
    
    // Delta 扩展字段
    if (delta >= 13 && delta < 269) {
        buffer[index++] = (uint8_t)(delta - 13);
    } else if (delta >= 269) {
        uint16_t extDelta = delta - 269;
        buffer[index++] = (uint8_t)(extDelta >> 8);
        buffer[index++] = (uint8_t)(extDelta & 0xFF);
    }
    
    // Length 扩展字段
    if (valueLength >= 13 && valueLength < 269) {
        buffer[index++] = (uint8_t)(valueLength - 13);
    } else if (valueLength >= 269) {
        uint16_t extLen = valueLength - 269;
        buffer[index++] = (uint8_t)(extLen >> 8);
        buffer[index++] = (uint8_t)(extLen & 0xFF);
    }
    
    // 写入 value
    if (valueLength > 0 && value != nullptr) {
        memcpy(&buffer[index], value, valueLength);
        index += valueLength;
    }
    
    return index;
}

// ============ Token 管理 ============

void CoAPHandler::generateToken(uint8_t* token, uint8_t length) {
    uint32_t r = esp_random();
    for (uint8_t i = 0; i < length && i < COAP_MAX_TOKEN_SIZE; i++) {
        token[i] = (uint8_t)(r >> (i * 8));
        if (i == 3 && length > 4) {
            r = esp_random();  // 需要更多随机字节
        }
    }
}

// ============ Message ID ============

uint16_t CoAPHandler::generateMessageId() {
    messageId++;
    if (messageId == 0) messageId = 1;
    return messageId;
}

// ============ 消息发送 (RFC 7252 合规) ============

bool CoAPHandler::sendCoAPMessage(CoAPType type, CoAPMethod method,
                                   const String& resource, const String& payload) {
    uint8_t buffer[Protocols::COAP_BUFFER_SIZE];
    uint16_t index = 0;
    uint8_t tokenLen = COAP_DEFAULT_TOKEN_SIZE;
    
    // 1. Header (4 bytes)
    uint16_t msgId = generateMessageId();
    buffer[0] = (COAP_VERSION << 6) | ((uint8_t)type << 4) | tokenLen;
    buffer[1] = (uint8_t)method;
    buffer[2] = (msgId >> 8) & 0xFF;
    buffer[3] = msgId & 0xFF;
    index = 4;
    
    // 2. Token
    uint8_t token[COAP_DEFAULT_TOKEN_SIZE];
    generateToken(token, tokenLen);
    memcpy(&buffer[index], token, tokenLen);
    index += tokenLen;
    
    // 3. Options
    // 分离 path 和 query
    String path = resource;
    String query = "";
    int qPos = resource.indexOf('?');
    if (qPos >= 0) {
        path = resource.substring(0, qPos);
        query = resource.substring(qPos + 1);
    }
    
    // 去除开头的 '/'
    if (path.startsWith("/")) {
        path = path.substring(1);
    }
    
    uint16_t prevOptionNumber = 0;
    
    // Uri-Path options (option 11): 按 "/" 拆分，每段一个 option
    if (path.length() > 0) {
        int startPos = 0;
        while (startPos <= (int)path.length()) {
            int slashPos = path.indexOf('/', startPos);
            String segment;
            if (slashPos < 0) {
                segment = path.substring(startPos);
                startPos = path.length() + 1;
            } else {
                segment = path.substring(startPos, slashPos);
                startPos = slashPos + 1;
            }
            
            if (segment.length() > 0) {
                if (index + segment.length() + 5 >= Protocols::COAP_BUFFER_SIZE) break;
                uint16_t written = encodeOption(&buffer[index], prevOptionNumber,
                                                COAP_OPTION_URI_PATH,
                                                (const uint8_t*)segment.c_str(),
                                                segment.length());
                index += written;
                prevOptionNumber = COAP_OPTION_URI_PATH;
            }
        }
    }
    
    // Content-Format option (option 12): 有 payload 时添加
    if (payload.length() > 0) {
        uint8_t ctValue = COAP_CT_TEXT_PLAIN;
        // 如果 payload 看起来像 JSON
        if (payload.startsWith("{") || payload.startsWith("[")) {
            ctValue = COAP_CT_APP_JSON;
        }
        if (index + 4 < Protocols::COAP_BUFFER_SIZE) {
            uint16_t written = encodeOption(&buffer[index], prevOptionNumber,
                                            COAP_OPTION_CONTENT_FORMAT,
                                            &ctValue, 1);
            index += written;
            prevOptionNumber = COAP_OPTION_CONTENT_FORMAT;
        }
    }
    
    // Uri-Query options (option 15): 按 "&" 拆分
    if (query.length() > 0) {
        int startPos = 0;
        while (startPos <= (int)query.length()) {
            int ampPos = query.indexOf('&', startPos);
            String param;
            if (ampPos < 0) {
                param = query.substring(startPos);
                startPos = query.length() + 1;
            } else {
                param = query.substring(startPos, ampPos);
                startPos = ampPos + 1;
            }
            
            if (param.length() > 0) {
                if (index + param.length() + 5 >= Protocols::COAP_BUFFER_SIZE) break;
                uint16_t written = encodeOption(&buffer[index], prevOptionNumber,
                                                COAP_OPTION_URI_QUERY,
                                                (const uint8_t*)param.c_str(),
                                                param.length());
                index += written;
                prevOptionNumber = COAP_OPTION_URI_QUERY;
            }
        }
    }
    
    // 4. Payload marker + Payload
    if (payload.length() > 0) {
        if (index + 1 + payload.length() < Protocols::COAP_BUFFER_SIZE) {
            buffer[index++] = 0xFF;  // Payload marker
            uint16_t copyLen = min((uint16_t)payload.length(),
                                   (uint16_t)(Protocols::COAP_BUFFER_SIZE - index));
            memcpy(&buffer[index], payload.c_str(), copyLen);
            index += copyLen;
        }
    }
    
    // 5. 发送 UDP
    if (!udp.beginPacket(config.server.c_str(), config.port)) {
        LOG_ERROR("CoAP: Failed to begin UDP packet");
        return false;
    }
    
    udp.write(buffer, index);
    int result = udp.endPacket();
    
    if (result != 1) {
        LOG_ERRORF("CoAP: Failed to send packet, error: %d", result);
        return false;
    }
    
    LOG_INFOF("CoAP: Sent %s %s (MID=%d, %dB)",
              getMethodString(method).c_str(), resource.c_str(), msgId, index);
    
    // 6. CON 消息注册重传追踪
    if (type == CoAPType::CON) {
        addPendingMessage(msgId, token, tokenLen, buffer, index);
    }
    
    return true;
}

// ============ 消息接收与解析 ============

void CoAPHandler::processCoAPPacket() {
    uint8_t buffer[Protocols::COAP_BUFFER_SIZE];
    int len = udp.read(buffer, sizeof(buffer));
    
    if (len < 4) return;  // 最小 CoAP 报文: 4字节 header
    
    // 解析 Header
    uint8_t ver       = (buffer[0] >> 6) & 0x03;
    uint8_t typeByte  = (buffer[0] >> 4) & 0x03;
    uint8_t tokenLen  = buffer[0] & 0x0F;
    uint8_t code      = buffer[1];
    uint16_t msgId    = (buffer[2] << 8) | buffer[3];
    
    if (ver != COAP_VERSION) {
        LOG_WARNINGF("CoAP: Invalid version %d, expected %d", ver, COAP_VERSION);
        return;
    }
    
    if (tokenLen > COAP_MAX_TOKEN_SIZE || 4 + tokenLen > len) {
        LOG_WARNING("CoAP: Invalid token length");
        return;
    }
    
    CoAPType msgType = (CoAPType)typeByte;
    
    // 消息去重
    if (isDuplicateMessage(msgId)) {
        LOG_DEBUGF("CoAP: Duplicate message MID=%d, ignoring", msgId);
        return;
    }
    recordMessageId(msgId);
    
    // 提取 Token
    uint8_t token[COAP_MAX_TOKEN_SIZE] = {0};
    memcpy(token, &buffer[4], tokenLen);
    
    // 解析 Options (正确遍历)
    String resourcePath = "";
    String payloadStr = "";
    uint16_t contentFormat = 0xFFFF;
    
    uint16_t offset = 4 + tokenLen;
    uint16_t prevOptionNumber = 0;
    
    while (offset < (uint16_t)len) {
        if (buffer[offset] == 0xFF) {
            // Payload marker
            offset++;
            // 提取 payload
            if (offset < (uint16_t)len) {
                payloadStr = String((const char*)&buffer[offset], len - offset);
            }
            break;
        }
        
        // 解析 option delta 和 length
        uint8_t optByte = buffer[offset++];
        uint16_t delta = (optByte >> 4) & 0x0F;
        uint16_t optLength = optByte & 0x0F;
        
        // Delta 扩展
        if (delta == 13) {
            if (offset >= (uint16_t)len) break;
            delta = buffer[offset++] + 13;
        } else if (delta == 14) {
            if (offset + 1 >= (uint16_t)len) break;
            delta = ((uint16_t)buffer[offset] << 8 | buffer[offset + 1]) + 269;
            offset += 2;
        } else if (delta == 15) {
            break;  // 保留值，视为格式错误
        }
        
        // Length 扩展
        if (optLength == 13) {
            if (offset >= (uint16_t)len) break;
            optLength = buffer[offset++] + 13;
        } else if (optLength == 14) {
            if (offset + 1 >= (uint16_t)len) break;
            optLength = ((uint16_t)buffer[offset] << 8 | buffer[offset + 1]) + 269;
            offset += 2;
        } else if (optLength == 15) {
            break;
        }
        
        uint16_t optionNumber = prevOptionNumber + delta;
        prevOptionNumber = optionNumber;
        
        if (offset + optLength > (uint16_t)len) break;
        
        // 处理已知 option
        if (optionNumber == COAP_OPTION_URI_PATH) {
            if (resourcePath.length() > 0) resourcePath += "/";
            resourcePath += String((const char*)&buffer[offset], optLength);
        } else if (optionNumber == COAP_OPTION_CONTENT_FORMAT) {
            if (optLength == 1) contentFormat = buffer[offset];
            else if (optLength == 2) contentFormat = (buffer[offset] << 8) | buffer[offset + 1];
        }
        
        offset += optLength;
    }
    
    // 响应码格式化
    uint8_t codeClass = (code >> 5) & 0x07;
    uint8_t codeDetail = code & 0x1F;
    
    LOG_INFOF("CoAP: Received %d.%02d MID=%d Type=%d TKL=%d",
              codeClass, codeDetail, msgId, typeByte, tokenLen);
    
    // 处理 ACK: 从 pendingMessages 中移除对应条目
    if (msgType == CoAPType::ACK || msgType == CoAPType::RST) {
        // 通过 token 或 MID 匹配
        for (int i = 0; i < MAX_PENDING_MESSAGES; i++) {
            if (!pendingMessages[i].active) continue;
            
            if (pendingMessages[i].messageId == msgId) {
                pendingMessages[i].active = false;
                if (msgType == CoAPType::ACK) {
                    LOG_DEBUGF("CoAP: ACK received for MID=%d", msgId);
                } else {
                    LOG_WARNINGF("CoAP: RST received for MID=%d", msgId);
                }
                break;
            }
        }
    }
    
    // 对收到的 CON 消息发送 ACK
    if (msgType == CoAPType::CON) {
        sendAck(msgId);
    }
    
    // 回调通知
    if (messageCallback) {
        String topicInfo = "coap/" + resourcePath;
        if (resourcePath.isEmpty()) {
            char codeBuf[8];
            snprintf(codeBuf, sizeof(codeBuf), "%d.%02d", codeClass, codeDetail);
            topicInfo = String("coap/") + codeBuf;
        }
        messageCallback(topicInfo, payloadStr);
    }
}

// ============ ACK 发送 ============

void CoAPHandler::sendAck(uint16_t msgId) {
    uint8_t ack[4];
    ack[0] = (COAP_VERSION << 6) | ((uint8_t)CoAPType::ACK << 4) | 0; // 无 token
    ack[1] = 0;  // Empty code
    ack[2] = (msgId >> 8) & 0xFF;
    ack[3] = msgId & 0xFF;
    
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(ack, 4);
    udp.endPacket();
    
    LOG_DEBUGF("CoAP: Sent ACK for MID=%d", msgId);
}

// ============ 重传引擎 ============

bool CoAPHandler::addPendingMessage(uint16_t msgId, const uint8_t* token, uint8_t tokenLen,
                                     const uint8_t* packet, uint16_t packetLen) {
    for (int i = 0; i < MAX_PENDING_MESSAGES; i++) {
        if (!pendingMessages[i].active) {
            pendingMessages[i].messageId = msgId;
            memcpy(pendingMessages[i].token, token, min((uint8_t)tokenLen, COAP_DEFAULT_TOKEN_SIZE));
            pendingMessages[i].tokenLength = tokenLen;
            memcpy(pendingMessages[i].packet, packet, min(packetLen, (uint16_t)Protocols::COAP_BUFFER_SIZE));
            pendingMessages[i].packetLength = packetLen;
            pendingMessages[i].sentTime = millis();
            pendingMessages[i].retryCount = 0;
            pendingMessages[i].nextRetryTime = millis() + COAP_ACK_TIMEOUT;
            pendingMessages[i].active = true;
            return true;
        }
    }
    LOG_WARNING("CoAP: Pending message queue full");
    return false;
}

void CoAPHandler::removePendingMessage(uint16_t msgId) {
    for (int i = 0; i < MAX_PENDING_MESSAGES; i++) {
        if (pendingMessages[i].active && pendingMessages[i].messageId == msgId) {
            pendingMessages[i].active = false;
            return;
        }
    }
}

void CoAPHandler::checkRetransmissions() {
    unsigned long now = millis();
    
    for (int i = 0; i < MAX_PENDING_MESSAGES; i++) {
        if (!pendingMessages[i].active) continue;
        
        if (now < pendingMessages[i].nextRetryTime) continue;
        
        if (pendingMessages[i].retryCount >= config.retransmitCount) {
            // 超过最大重传次数
            LOG_WARNINGF("CoAP: Message MID=%d timed out after %d retries",
                         pendingMessages[i].messageId, pendingMessages[i].retryCount);
            pendingMessages[i].active = false;
            continue;
        }
        
        // 重传
        if (udp.beginPacket(config.server.c_str(), config.port)) {
            udp.write(pendingMessages[i].packet, pendingMessages[i].packetLength);
            udp.endPacket();
            
            pendingMessages[i].retryCount++;
            // 指数退避: timeout * 2^retryCount
            unsigned long backoff = COAP_ACK_TIMEOUT * (1 << pendingMessages[i].retryCount);
            pendingMessages[i].nextRetryTime = now + backoff;
            
            LOG_INFOF("CoAP: Retransmit MID=%d (attempt %d/%d, next in %lums)",
                      pendingMessages[i].messageId, pendingMessages[i].retryCount,
                      config.retransmitCount, backoff);
        }
    }
}

// ============ 消息去重 ============

bool CoAPHandler::isDuplicateMessage(uint16_t msgId) {
    for (int i = 0; i < 16; i++) {
        if (recentMessageIds[i] == msgId && msgId != 0) {
            return true;
        }
    }
    return false;
}

void CoAPHandler::recordMessageId(uint16_t msgId) {
    recentMessageIds[recentMsgIdIndex] = msgId;
    recentMsgIdIndex = (recentMsgIdIndex + 1) % 16;
}

// ============ 工具函数 ============

String CoAPHandler::getMethodString(CoAPMethod method) {
    switch (method) {
        case CoAPMethod::GET:    return "GET";
        case CoAPMethod::POST:   return "POST";
        case CoAPMethod::PUT:    return "PUT";
        case CoAPMethod::DELETE: return "DELETE";
        default:                 return "UNKNOWN";
    }
}

#endif // FASTBEE_ENABLE_COAP
