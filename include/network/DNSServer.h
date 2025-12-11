#ifndef DNSServer_h
#define DNSServer_h

#include <WiFiUdp.h>

#define DNS_QR_QUERY 0
#define DNS_QR_RESPONSE 1
#define DNS_OPCODE_QUERY 0

enum class DNSReplyCode : unsigned char {
    NoError = 0,
    FormError = 1,
    ServerFailure = 2,
    NonExistentDomain = 3,
    NotImplemented = 4,
    Refused = 5,
    YXDomain = 6,
    YXRRSet = 7,
    NXRRSet = 8
};

struct DNSHeader {
    uint16_t ID;               // 事务标识符
    unsigned char RD : 1;      // 期望递归
    unsigned char TC : 1;      // 截断
    unsigned char AA : 1;      // 权威回答
    unsigned char OPCODE : 4;  // 操作码
    unsigned char QR : 1;      // 查询/响应标志
    unsigned char RCODE : 4;   // 响应码
    unsigned char Z : 3;       // 保留
    unsigned char RA : 1;      // 递归可用
    uint16_t QDCOUNT;          // 问题数
    uint16_t ANCOUNT;          // 答案数
    uint16_t NSCOUNT;          // 权威记录数
    uint16_t ARCOUNT;          // 附加记录数
};

class DNSServer {
public:
    DNSServer(); // 构造函数
    ~DNSServer(); // 析构函数
    
    // 启动DNS服务器，默认端口53，domainName为"*"时捕获所有查询[citation:3]
    bool start(const uint16_t &port, const String &domainName, const IPAddress &resolvedIP);
    // 停止DNS服务器[citation:3]
    void stop();
    // 处理下一个DNS请求，需在loop中频繁调用[citation:3]
    void processNextRequest();
    // 设置返回的错误码
    void setErrorReplyCode(const DNSReplyCode &replyCode);
    // 设置特定的域名查询返回自定义IP，其他返回错误码
    void setTTL(const uint32_t &ttl);

private:
    WiFiUDP _udp; // UDP实例
    uint16_t _port; // 端口号
    String _domainName; // 要拦截的域名
    IPAddress _resolvedIP; // 域名解析到的IP地址[citation:3]
    uint32_t _ttl; // DNS记录的生存时间
    DNSReplyCode _errorReplyCode; // 错误响应码
    
    // 向下游DNS服务器转发请求（可选功能）
    bool _forwardQueries = false;
    IPAddress _forwarder;
    
    // 处理收到的DNS请求
    void _handleRequest();
    // 构建DNS响应
    void _buildResponse(const DNSHeader *dnsHeader, const String &questionDomain, const uint16_t &questionType, const uint16_t &questionClass);
    // 向下游DNS服务器转发并获取响应（可选功能）
    void _forwardRequest(const char *buffer, size_t length);
    // 将域名转换为DNS格式
    void _writeNameToBuffer(WiFiUDP &udp, const String &name);
    // 从DNS格式中读取域名
    String _readNameFromBuffer(WiFiUDP &udp);
};

#endif