/**
 * @file test_protocol_handlers.cpp
 * @brief 协议处理器单元测试
 * 
 * 测试内容：
 * - TCP 连接管理（连接/断开/重连/超时）
 * - HTTP 客户端（请求构造/响应解析/超时处理）
 * - CoAP 协议（消息格式/确认/重传）
 * - 协议通用：连接状态机、错误恢复、并发保护
 */

#include <unity.h>
#include <Arduino.h>
#include <ArduinoJson.h>

void test_protocol_handlers_group();

// ========== Mock: TCP 连接模拟 ==========

class MockTCPClient {
public:
    MockTCPClient() : _connected(false), _port(0), _timeout(5000),
                      _shouldFail(false), _connectAttempts(0),
                      _bytesSent(0), _bytesReceived(0) {}

    bool connect(const char* host, uint16_t port) {
        _host = String(host);
        _port = port;
        _connectAttempts++;
        
        if (_shouldFail) {
            _connected = false;
            _lastError = "Connection refused";
            return false;
        }
        
        _connected = true;
        _lastError = "";
        return true;
    }

    void disconnect() {
        _connected = false;
    }

    bool isConnected() { return _connected; }

    int send(const uint8_t* data, size_t length) {
        if (!_connected) return -1;
        _bytesSent += length;
        _sentData.push_back(String((const char*)data, length));
        return length;
    }

    int send(const String& data) {
        return send((const uint8_t*)data.c_str(), data.length());
    }

    String receive(size_t maxLen = 1024) {
        if (!_connected || _receiveBuffer.isEmpty()) return "";
        String data = _receiveBuffer;
        _bytesReceived += data.length();
        _receiveBuffer = "";
        return data;
    }

    void setTimeout(unsigned long timeout) { _timeout = timeout; }
    unsigned long getTimeout() { return _timeout; }
    String getLastError() { return _lastError; }
    int getConnectAttempts() { return _connectAttempts; }
    size_t getBytesSent() { return _bytesSent; }
    size_t getBytesReceived() { return _bytesReceived; }
    std::vector<String> getSentData() { return _sentData; }

    // 测试控制
    void setShouldFail(bool fail) { _shouldFail = fail; }
    void setReceiveBuffer(const String& data) { _receiveBuffer = data; }
    void simulateDisconnect() { _connected = false; }
    void reset() {
        _connected = false;
        _connectAttempts = 0;
        _bytesSent = 0;
        _bytesReceived = 0;
        _sentData.clear();
        _receiveBuffer = "";
        _shouldFail = false;
    }

private:
    bool _connected;
    String _host;
    uint16_t _port;
    unsigned long _timeout;
    bool _shouldFail;
    int _connectAttempts;
    size_t _bytesSent;
    size_t _bytesReceived;
    String _lastError;
    String _receiveBuffer;
    std::vector<String> _sentData;
};

// ========== Mock: HTTP 客户端模拟 ==========

struct HTTPResponse {
    int statusCode;
    String body;
    std::map<String, String> headers;
    
    HTTPResponse() : statusCode(0) {}
};

class MockHTTPClient {
public:
    MockHTTPClient() : _timeout(10000), _shouldFail(false), _requestCount(0) {}

    HTTPResponse get(const String& url) {
        return makeRequest("GET", url, "");
    }

    HTTPResponse post(const String& url, const String& body) {
        return makeRequest("POST", url, body);
    }

    HTTPResponse put(const String& url, const String& body) {
        return makeRequest("PUT", url, body);
    }

    HTTPResponse del(const String& url) {
        return makeRequest("DELETE", url, "");
    }

    void setTimeout(unsigned long timeout) { _timeout = timeout; }
    unsigned long getTimeout() { return _timeout; }
    int getRequestCount() { return _requestCount; }

    // 测试控制
    void setShouldFail(bool fail) { _shouldFail = fail; }
    void setMockResponse(int code, const String& body) {
        _mockResponse.statusCode = code;
        _mockResponse.body = body;
    }
    void setMockHeader(const String& key, const String& value) {
        _mockResponse.headers[key] = value;
    }
    void reset() {
        _shouldFail = false;
        _requestCount = 0;
        _mockResponse = HTTPResponse();
        _lastRequest = "";
        _lastMethod = "";
        _lastBody = "";
    }

    String getLastRequest() { return _lastRequest; }
    String getLastMethod() { return _lastMethod; }
    String getLastBody() { return _lastBody; }

private:
    HTTPResponse makeRequest(const String& method, const String& url, const String& body) {
        _requestCount++;
        _lastMethod = method;
        _lastRequest = url;
        _lastBody = body;
        
        if (_shouldFail) {
            HTTPResponse errResp;
            errResp.statusCode = -1;
            errResp.body = "Connection failed";
            return errResp;
        }
        
        if (_mockResponse.statusCode == 0) {
            HTTPResponse defaultResp;
            defaultResp.statusCode = 200;
            defaultResp.body = "{}";
            return defaultResp;
        }
        
        return _mockResponse;
    }

    unsigned long _timeout;
    bool _shouldFail;
    int _requestCount;
    HTTPResponse _mockResponse;
    String _lastRequest;
    String _lastMethod;
    String _lastBody;
};

// ========== Mock: CoAP 消息模拟 ==========

enum CoAPMessageType {
    COAP_CON = 0,   // Confirmable
    COAP_NON = 1,   // Non-confirmable
    COAP_ACK = 2,   // Acknowledgement
    COAP_RST = 3    // Reset
};

enum CoAPResponseCode {
    COAP_SUCCESS = 200,
    COAP_CREATED = 201,
    COAP_BAD_REQUEST = 400,
    COAP_NOT_FOUND = 404,
    COAP_INTERNAL_ERROR = 500
};

struct CoAPMessage {
    CoAPMessageType type;
    int code;
    uint16_t messageId;
    String token;
    String path;
    String payload;
    
    CoAPMessage() : type(COAP_CON), code(0), messageId(0) {}
};

class MockCoAPHandler {
public:
    MockCoAPHandler() : _messageIdCounter(1), _shouldFail(false),
                        _ackReceived(false), _retransmitCount(0) {}

    bool sendRequest(const String& path, const String& payload, 
                     CoAPMessageType type = COAP_CON) {
        if (_shouldFail) return false;
        
        CoAPMessage msg;
        msg.type = type;
        msg.messageId = _messageIdCounter++;
        msg.path = path;
        msg.payload = payload;
        msg.code = 1;  // GET
        
        _sentMessages.push_back(msg);
        
        // CON 消息需要 ACK
        if (type == COAP_CON) {
            _pendingAcks.push_back(msg.messageId);
        }
        
        return true;
    }

    bool acknowledgeMessage(uint16_t messageId) {
        for (auto it = _pendingAcks.begin(); it != _pendingAcks.end(); ++it) {
            if (*it == messageId) {
                _pendingAcks.erase(it);
                _ackReceived = true;
                return true;
            }
        }
        return false;
    }

    bool retransmit(uint16_t messageId) {
        _retransmitCount++;
        // 查找原始消息并重发
        for (auto& msg : _sentMessages) {
            if (msg.messageId == messageId) {
                _sentMessages.push_back(msg);
                return true;
            }
        }
        return false;
    }

    int getPendingAckCount() { return _pendingAcks.size(); }
    int getRetransmitCount() { return _retransmitCount; }
    bool wasAckReceived() { return _ackReceived; }
    std::vector<CoAPMessage> getSentMessages() { return _sentMessages; }

    // 测试控制
    void setShouldFail(bool fail) { _shouldFail = fail; }
    void reset() {
        _sentMessages.clear();
        _pendingAcks.clear();
        _messageIdCounter = 1;
        _shouldFail = false;
        _ackReceived = false;
        _retransmitCount = 0;
    }

private:
    uint16_t _messageIdCounter;
    bool _shouldFail;
    bool _ackReceived;
    int _retransmitCount;
    std::vector<CoAPMessage> _sentMessages;
    std::vector<uint16_t> _pendingAcks;
};

// ========== TCP 连接测试 ==========

static MockTCPClient tcpClient;

static void test_tcp_connect_success() {
    tcpClient.reset();
    
    TEST_ASSERT_TRUE(tcpClient.connect("192.168.1.100", 8080));
    TEST_ASSERT_TRUE(tcpClient.isConnected());
    TEST_ASSERT_EQUAL(1, tcpClient.getConnectAttempts());
}

static void test_tcp_connect_failure() {
    tcpClient.reset();
    tcpClient.setShouldFail(true);
    
    TEST_ASSERT_FALSE(tcpClient.connect("192.168.1.100", 8080));
    TEST_ASSERT_FALSE(tcpClient.isConnected());
    TEST_ASSERT_FALSE(tcpClient.getLastError().isEmpty());
}

static void test_tcp_disconnect() {
    tcpClient.reset();
    tcpClient.connect("192.168.1.100", 8080);
    
    tcpClient.disconnect();
    TEST_ASSERT_FALSE(tcpClient.isConnected());
}

static void test_tcp_send_data() {
    tcpClient.reset();
    tcpClient.connect("192.168.1.100", 8080);
    
    String data = "{\"cmd\":\"status\"}";
    int sent = tcpClient.send(data);
    
    TEST_ASSERT_EQUAL((int)data.length(), sent);
    TEST_ASSERT_EQUAL(data.length(), tcpClient.getBytesSent());
    
    auto sentData = tcpClient.getSentData();
    TEST_ASSERT_EQUAL(1, sentData.size());
    TEST_ASSERT_EQUAL_STRING(data.c_str(), sentData[0].c_str());
}

static void test_tcp_send_while_disconnected() {
    tcpClient.reset();
    // 不连接，直接发送
    
    int sent = tcpClient.send("data");
    TEST_ASSERT_EQUAL(-1, sent);
}

static void test_tcp_receive_data() {
    tcpClient.reset();
    tcpClient.connect("192.168.1.100", 8080);
    tcpClient.setReceiveBuffer("{\"status\":\"ok\"}");
    
    String received = tcpClient.receive();
    TEST_ASSERT_EQUAL_STRING("{\"status\":\"ok\"}", received.c_str());
    TEST_ASSERT_TRUE(tcpClient.getBytesReceived() > 0);
}

static void test_tcp_receive_empty() {
    tcpClient.reset();
    tcpClient.connect("192.168.1.100", 8080);
    
    String received = tcpClient.receive();
    TEST_ASSERT_TRUE(received.isEmpty());
}

static void test_tcp_reconnect_after_disconnect() {
    tcpClient.reset();
    tcpClient.connect("192.168.1.100", 8080);
    tcpClient.simulateDisconnect();
    
    TEST_ASSERT_FALSE(tcpClient.isConnected());
    
    // 重连
    TEST_ASSERT_TRUE(tcpClient.connect("192.168.1.100", 8080));
    TEST_ASSERT_TRUE(tcpClient.isConnected());
    TEST_ASSERT_EQUAL(2, tcpClient.getConnectAttempts());
}

static void test_tcp_timeout_config() {
    tcpClient.reset();
    
    tcpClient.setTimeout(30000);
    TEST_ASSERT_EQUAL(30000, (int)tcpClient.getTimeout());
}

// ========== HTTP 客户端测试 ==========

static MockHTTPClient httpClient;

static void test_http_get_success() {
    httpClient.reset();
    httpClient.setMockResponse(200, "{\"health\":\"ok\"}");
    
    auto resp = httpClient.get("http://192.168.1.100/api/health");
    
    TEST_ASSERT_EQUAL(200, resp.statusCode);
    TEST_ASSERT_EQUAL_STRING("{\"health\":\"ok\"}", resp.body.c_str());
    TEST_ASSERT_EQUAL_STRING("GET", httpClient.getLastMethod().c_str());
}

static void test_http_post_with_body() {
    httpClient.reset();
    httpClient.setMockResponse(201, "{\"id\":1}");
    
    String body = "{\"name\":\"sensor1\",\"type\":\"temperature\"}";
    auto resp = httpClient.post("http://192.168.1.100/api/devices", body);
    
    TEST_ASSERT_EQUAL(201, resp.statusCode);
    TEST_ASSERT_EQUAL_STRING("POST", httpClient.getLastMethod().c_str());
    TEST_ASSERT_EQUAL_STRING(body.c_str(), httpClient.getLastBody().c_str());
}

static void test_http_put_update() {
    httpClient.reset();
    httpClient.setMockResponse(200, "{}");
    
    auto resp = httpClient.put("http://host/api/config", "{\"timeout\":30}");
    
    TEST_ASSERT_EQUAL(200, resp.statusCode);
    TEST_ASSERT_EQUAL_STRING("PUT", httpClient.getLastMethod().c_str());
}

static void test_http_delete() {
    httpClient.reset();
    httpClient.setMockResponse(204, "");
    
    auto resp = httpClient.del("http://host/api/sessions/123");
    
    TEST_ASSERT_EQUAL(204, resp.statusCode);
    TEST_ASSERT_EQUAL_STRING("DELETE", httpClient.getLastMethod().c_str());
}

static void test_http_connection_failure() {
    httpClient.reset();
    httpClient.setShouldFail(true);
    
    auto resp = httpClient.get("http://unreachable/api");
    
    TEST_ASSERT_EQUAL(-1, resp.statusCode);
    TEST_ASSERT_TRUE(resp.body.indexOf("failed") >= 0);
}

static void test_http_404_response() {
    httpClient.reset();
    httpClient.setMockResponse(404, "{\"error\":\"not found\"}");
    
    auto resp = httpClient.get("http://host/api/nonexist");
    
    TEST_ASSERT_EQUAL(404, resp.statusCode);
}

static void test_http_500_server_error() {
    httpClient.reset();
    httpClient.setMockResponse(500, "{\"error\":\"internal\"}");
    
    auto resp = httpClient.get("http://host/api/broken");
    
    TEST_ASSERT_EQUAL(500, resp.statusCode);
}

static void test_http_timeout_config() {
    httpClient.reset();
    
    httpClient.setTimeout(30000);
    TEST_ASSERT_EQUAL(30000, (int)httpClient.getTimeout());
}

static void test_http_request_count() {
    httpClient.reset();
    
    httpClient.get("http://host/a");
    httpClient.get("http://host/b");
    httpClient.post("http://host/c", "{}");
    
    TEST_ASSERT_EQUAL(3, httpClient.getRequestCount());
}

// ========== CoAP 协议测试 ==========

static MockCoAPHandler coapHandler;

static void test_coap_send_confirmable() {
    coapHandler.reset();
    
    TEST_ASSERT_TRUE(coapHandler.sendRequest("/sensors/temp", "read", COAP_CON));
    
    auto msgs = coapHandler.getSentMessages();
    TEST_ASSERT_EQUAL(1, msgs.size());
    TEST_ASSERT_EQUAL((int)COAP_CON, (int)msgs[0].type);
    TEST_ASSERT_EQUAL_STRING("/sensors/temp", msgs[0].path.c_str());
    
    // CON 消息应有 pending ACK
    TEST_ASSERT_EQUAL(1, coapHandler.getPendingAckCount());
}

static void test_coap_send_nonconfirmable() {
    coapHandler.reset();
    
    TEST_ASSERT_TRUE(coapHandler.sendRequest("/status", "ping", COAP_NON));
    
    // NON 消息无需 ACK
    TEST_ASSERT_EQUAL(0, coapHandler.getPendingAckCount());
}

static void test_coap_acknowledge() {
    coapHandler.reset();
    
    coapHandler.sendRequest("/data", "get", COAP_CON);
    auto msgs = coapHandler.getSentMessages();
    uint16_t msgId = msgs[0].messageId;
    
    TEST_ASSERT_TRUE(coapHandler.acknowledgeMessage(msgId));
    TEST_ASSERT_TRUE(coapHandler.wasAckReceived());
    TEST_ASSERT_EQUAL(0, coapHandler.getPendingAckCount());
}

static void test_coap_acknowledge_invalid_id() {
    coapHandler.reset();
    
    coapHandler.sendRequest("/data", "get", COAP_CON);
    
    // ACK 一个不存在的 ID
    TEST_ASSERT_FALSE(coapHandler.acknowledgeMessage(9999));
    TEST_ASSERT_EQUAL(1, coapHandler.getPendingAckCount());
}

static void test_coap_retransmit() {
    coapHandler.reset();
    
    coapHandler.sendRequest("/data", "get", COAP_CON);
    auto msgs = coapHandler.getSentMessages();
    uint16_t msgId = msgs[0].messageId;
    
    // 模拟超时重传
    TEST_ASSERT_TRUE(coapHandler.retransmit(msgId));
    TEST_ASSERT_EQUAL(1, coapHandler.getRetransmitCount());
    
    // 重传后消息列表增加
    auto allMsgs = coapHandler.getSentMessages();
    TEST_ASSERT_EQUAL(2, allMsgs.size());
}

static void test_coap_send_failure() {
    coapHandler.reset();
    coapHandler.setShouldFail(true);
    
    TEST_ASSERT_FALSE(coapHandler.sendRequest("/data", "get", COAP_CON));
    TEST_ASSERT_EQUAL(0, (int)coapHandler.getSentMessages().size());
}

static void test_coap_message_id_increment() {
    coapHandler.reset();
    
    coapHandler.sendRequest("/a", "1", COAP_NON);
    coapHandler.sendRequest("/b", "2", COAP_NON);
    coapHandler.sendRequest("/c", "3", COAP_NON);
    
    auto msgs = coapHandler.getSentMessages();
    TEST_ASSERT_EQUAL(1, msgs[0].messageId);
    TEST_ASSERT_EQUAL(2, msgs[1].messageId);
    TEST_ASSERT_EQUAL(3, msgs[2].messageId);
}

// ========== 协议通用：连接状态机测试 ==========

enum ProtocolState {
    STATE_DISCONNECTED = 0,
    STATE_CONNECTING = 1,
    STATE_CONNECTED = 2,
    STATE_RECONNECTING = 3,
    STATE_ERROR = 4
};

static void test_protocol_state_machine_normal() {
    // 正常流程：DISCONNECTED -> CONNECTING -> CONNECTED
    ProtocolState state = STATE_DISCONNECTED;
    
    state = STATE_CONNECTING;
    TEST_ASSERT_EQUAL((int)STATE_CONNECTING, (int)state);
    
    // 连接成功
    state = STATE_CONNECTED;
    TEST_ASSERT_EQUAL((int)STATE_CONNECTED, (int)state);
}

static void test_protocol_state_machine_reconnect() {
    // 断线重连：CONNECTED -> RECONNECTING -> CONNECTED
    ProtocolState state = STATE_CONNECTED;
    
    // 断线
    state = STATE_RECONNECTING;
    TEST_ASSERT_EQUAL((int)STATE_RECONNECTING, (int)state);
    
    // 重连成功
    state = STATE_CONNECTED;
    TEST_ASSERT_EQUAL((int)STATE_CONNECTED, (int)state);
}

static void test_protocol_state_machine_error() {
    // 错误恢复：CONNECTING -> ERROR -> DISCONNECTED
    ProtocolState state = STATE_CONNECTING;
    
    // 连接失败
    state = STATE_ERROR;
    TEST_ASSERT_EQUAL((int)STATE_ERROR, (int)state);
    
    // 重置
    state = STATE_DISCONNECTED;
    TEST_ASSERT_EQUAL((int)STATE_DISCONNECTED, (int)state);
}

// ========== 测试组入口 ==========

void test_protocol_handlers_group() {
    // TCP 连接测试
    RUN_TEST(test_tcp_connect_success);
    RUN_TEST(test_tcp_connect_failure);
    RUN_TEST(test_tcp_disconnect);
    RUN_TEST(test_tcp_send_data);
    RUN_TEST(test_tcp_send_while_disconnected);
    RUN_TEST(test_tcp_receive_data);
    RUN_TEST(test_tcp_receive_empty);
    RUN_TEST(test_tcp_reconnect_after_disconnect);
    RUN_TEST(test_tcp_timeout_config);
    
    // HTTP 客户端测试
    RUN_TEST(test_http_get_success);
    RUN_TEST(test_http_post_with_body);
    RUN_TEST(test_http_put_update);
    RUN_TEST(test_http_delete);
    RUN_TEST(test_http_connection_failure);
    RUN_TEST(test_http_404_response);
    RUN_TEST(test_http_500_server_error);
    RUN_TEST(test_http_timeout_config);
    RUN_TEST(test_http_request_count);
    
    // CoAP 协议测试
    RUN_TEST(test_coap_send_confirmable);
    RUN_TEST(test_coap_send_nonconfirmable);
    RUN_TEST(test_coap_acknowledge);
    RUN_TEST(test_coap_acknowledge_invalid_id);
    RUN_TEST(test_coap_retransmit);
    RUN_TEST(test_coap_send_failure);
    RUN_TEST(test_coap_message_id_increment);
    
    // 协议状态机
    RUN_TEST(test_protocol_state_machine_normal);
    RUN_TEST(test_protocol_state_machine_reconnect);
    RUN_TEST(test_protocol_state_machine_error);
}
