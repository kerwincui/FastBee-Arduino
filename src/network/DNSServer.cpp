/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:29:43
 */

#include "network/DNSServer.h"
#include <lwip/def.h>

DNSServer::DNSServer() : _port(53), _ttl(60), _errorReplyCode(DNSReplyCode::NonExistentDomain) {
    // 初始化
}

DNSServer::~DNSServer() {
    stop();
}

bool DNSServer::start(const uint16_t &port, const String &domainName, const IPAddress &resolvedIP) {
    _port = port;
    _domainName = domainName;
    _resolvedIP = resolvedIP;
    
    // 开启UDP监听[citation:3]
    if (_udp.begin(_port) != 1) {
        return false;
    }
    return true;
}

void DNSServer::stop() {
    _udp.stop();
}

void DNSServer::processNextRequest() {
    int packetSize = _udp.parsePacket();
    if (packetSize >= sizeof(DNSHeader)) {
        _handleRequest();
    }
}

void DNSServer::setErrorReplyCode(const DNSReplyCode &replyCode) {
    _errorReplyCode = replyCode;
}

void DNSServer::setTTL(const uint32_t &ttl) {
    _ttl = ttl;
}

void DNSServer::_handleRequest() {
    char buffer[512];
    size_t bufferSize = sizeof(buffer);
    
    // 读取UDP数据包
    int len = _udp.read(buffer, bufferSize);
    if (len < sizeof(DNSHeader)) {
        return; // 数据包过小
    }
    
    // 解析DNS头部
    DNSHeader dnsHeader;
    memcpy(&dnsHeader, buffer, sizeof(DNSHeader));
    dnsHeader.ID = ntohs(dnsHeader.ID);
    dnsHeader.QDCOUNT = ntohs(dnsHeader.QDCOUNT);
    dnsHeader.ANCOUNT = ntohs(dnsHeader.ANCOUNT);
    dnsHeader.NSCOUNT = ntohs(dnsHeader.NSCOUNT);
    dnsHeader.ARCOUNT = ntohs(dnsHeader.ARCOUNT);
    
    // 检查是否为查询请求
    if (dnsHeader.QR != DNS_QR_QUERY || dnsHeader.OPCODE != DNS_OPCODE_QUERY) {
        return;
    }
    
    // 暂时只处理单个查询
    if (dnsHeader.QDCOUNT != 1) {
        return;
    }
    
    // 移动到问题部分
    WiFiUDP udpCopy = _udp;
    udpCopy.seek(sizeof(DNSHeader));
    
    // 读取查询的域名
    String questionDomain = _readNameFromBuffer(udpCopy);
    if (questionDomain.isEmpty()) {
        return;
    }
    
    // 读取查询类型和类
    uint16_t questionType, questionClass;
    if (udpCopy.available() < 4) {
        return;
    }
    questionType = udpCopy.read() << 8 | udpCopy.read();
    questionClass = udpCopy.read() << 8 | udpCopy.read();
    
    // 判断是否匹配域名或通配符
    bool domainMatches = (_domainName == "*" || questionDomain.equalsIgnoreCase(_domainName));
    
    // 构建响应
    _buildResponse(&dnsHeader, questionDomain, questionType, questionClass);
}

void DNSServer::_buildResponse(const DNSHeader *dnsHeader, const String &questionDomain, const uint16_t &questionType, const uint16_t &questionClass) {
    // 准备响应缓冲区
    WiFiUDP udpResponse;
    udpResponse.beginPacket(_udp.remoteIP(), _udp.remotePort());
    
    // 填写DNS头部
    DNSHeader responseHeader;
    memset(&responseHeader, 0, sizeof(responseHeader));
    responseHeader.ID = dnsHeader->ID;
    responseHeader.QR = DNS_QR_RESPONSE;
    responseHeader.OPCODE = dnsHeader->OPCODE;
    responseHeader.AA = 0;
    responseHeader.TC = 0;
    responseHeader.RD = dnsHeader->RD;
    responseHeader.RA = 0;
    responseHeader.Z = 0;
    responseHeader.RCODE = (unsigned char)DNSReplyCode::NoError;
    responseHeader.QDCOUNT = htons(1); // 一个问题
    responseHeader.ANCOUNT = htons(1); // 一个答案
    
    // 写入响应头部
    DNSHeader ntohsHeader = responseHeader;
    ntohsHeader.ID = htons(responseHeader.ID);
    ntohsHeader.QDCOUNT = htons(responseHeader.QDCOUNT);
    ntohsHeader.ANCOUNT = htons(responseHeader.ANCOUNT);
    udpResponse.write((unsigned char*)&ntohsHeader, sizeof(DNSHeader));
    
    // 写入问题部分
    _writeNameToBuffer(udpResponse, questionDomain);
    udpResponse.write((uint8_t)0); // 结束符
    udpResponse.write((uint8_t)((questionType >> 8) & 0xFF));
    udpResponse.write((uint8_t)(questionType & 0xFF));
    udpResponse.write((uint8_t)((questionClass >> 8) & 0xFF));
    udpResponse.write((uint8_t)(questionClass & 0xFF));
    
    // 写入答案部分
    _writeNameToBuffer(udpResponse, questionDomain);
    udpResponse.write((uint8_t)0); // 结束符
    udpResponse.write((uint8_t)0); // Type: A记录 (Host Address)
    udpResponse.write((uint8_t)1);
    udpResponse.write((uint8_t)0); // Class: IN (0x0001)
    udpResponse.write((uint8_t)1);
    udpResponse.write((uint8_t)((_ttl >> 24) & 0xFF)); // TTL
    udpResponse.write((uint8_t)((_ttl >> 16) & 0xFF));
    udpResponse.write((uint8_t)((_ttl >> 8) & 0xFF));
    udpResponse.write((uint8_t)(_ttl & 0xFF));
    udpResponse.write((uint8_t)0); // RDATA长度 (4 bytes for IPv4)
    udpResponse.write((uint8_t)4);
    udpResponse.write(_resolvedIP[0]); // IP地址的四个字节[citation:3]
    udpResponse.write(_resolvedIP[1]);
    udpResponse.write(_resolvedIP[2]);
    udpResponse.write(_resolvedIP[3]);
    
    udpResponse.endPacket();
}

void DNSServer::_writeNameToBuffer(WiFiUDP &udp, const String &name) {
    int start = 0;
    int end = name.indexOf('.');
    while (end != -1) {
        udp.write((uint8_t)(end - start)); // 标签长度
        udp.write((const uint8_t*)name.c_str() + start, end - start); // 标签内容
        start = end + 1;
        end = name.indexOf('.', start);
    }
    // 写入最后一个标签
    udp.write((uint8_t)(name.length() - start));
    udp.write((const uint8_t*)name.c_str() + start, name.length() - start);
}

String DNSServer::_readNameFromBuffer(WiFiUDP &udp) {
    String name;
    while (true) {
        uint8_t labelLength = udp.read();
        if (labelLength == 0) {
            break; // 域名结束
        }
        if (labelLength >= 0xC0) {
            // 指针，这里简化处理，实际应跳转
            break;
        }
        if (name.length() > 0) {
            name += ".";
        }
        while (labelLength > 0) {
            name += (char)udp.read();
            labelLength--;
        }
    }
    return name;
}