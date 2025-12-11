#ifndef CRYPTO_UTILS_H
#define CRYPTO_UTILS_H

#include <Arduino.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <mbedtls/aes.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/gcm.h>
#include <Preferences.h>

/**
 * @brief 哈希算法类型枚举
 */
enum class HashAlgorithm {
    MD5 = 0,
    SHA256 = 1,
    SHA1 = 2
};

/**
 * @brief 加密算法类型枚举
 */
enum class CryptoAlgorithm {
    AES_128_ECB = 0,
    AES_256_ECB = 1,
    AES_128_CBC = 2,
    AES_256_CBC = 3,
    AES_128_GCM = 4,
    AES_256_GCM = 5
};

/**
 * @brief 加密操作结果结构体
 */
struct CryptoResult {
    bool success;
    String data;
    String error;
    size_t outputLength;
    
    CryptoResult() : success(false), outputLength(0) {}
};

/**
 * @brief 加密配置结构体
 */
struct CryptoConfig {
    CryptoAlgorithm algorithm = CryptoAlgorithm::AES_256_CBC;
    bool useHardwareAcceleration = true;
    bool enableDebug = false;
    
    // AES配置
    size_t keySize = 32; // 256-bit
    size_t ivSize = 16;  // 128-bit
    
    // GCM配置
    size_t tagSize = 16; // 128-bit authentication tag
};

/**
 * @brief 加密工具类
 * 
 * 提供各种加密、哈希和编码功能：
 * - 多种哈希算法 (MD5, SHA1, SHA256)
 * - AES加密/解密 (ECB, CBC, GCM模式)
 * - Base64编码/解码
 * - 安全随机数生成
 * - 密码学安全密钥派生
 */
class CryptoUtils {
private:
    // mbedTLS上下文
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    
    // 配置
    CryptoConfig config;
    bool initialized;
    
    /**
     * @brief 初始化mbedTLS随机数生成器
     * @return 初始化是否成功
     */
    bool initializeRNG();
    
    /**
     * @brief 验证密钥长度
     * @param key 密钥
     * @param expectedSize 期望长度
     * @return 是否有效
     */
    bool validateKeySize(const String& key, size_t expectedSize);
    
    /**
     * @brief 验证IV长度
     * @param iv 初始化向量
     * @param expectedSize 期望长度
     * @return 是否有效
     */
    bool validateIVSize(const String& iv, size_t expectedSize);
    
    /**
     * @brief 执行AES ECB加密
     * @param input 输入数据
     * @param key 密钥
     * @param encrypt 加密/解密标志
     * @return 加密结果
     */
    CryptoResult aesECB(const String& input, const String& key, bool encrypt);
    
    /**
     * @brief 执行AES CBC加密
     * @param input 输入数据
     * @param key 密钥
     * @param iv 初始化向量
     * @param encrypt 加密/解密标志
     * @return 加密结果
     */
    CryptoResult aesCBC(const String& input, const String& key, const String& iv, bool encrypt);
    
    /**
     * @brief 执行AES GCM加密
     * @param input 输入数据
     * @param key 密钥
     * @param iv 初始化向量
     * @param aad 附加认证数据
     * @param encrypt 加密/解密标志
     * @return 加密结果
     */
    CryptoResult aesGCM(const String& input, const String& key, const String& iv, 
                       const String& aad, bool encrypt);

public:
    /**
     * @brief 构造函数
     */
    CryptoUtils();
    
    /**
     * @brief 析构函数
     */
    ~CryptoUtils();
    
    /**
     * @brief 初始化加密工具
     * @param customConfig 自定义配置
     * @return 初始化是否成功
     */
    bool initialize(const CryptoConfig& customConfig = CryptoConfig());
    
    /**
     * @brief 计算数据的哈希值
     * @param data 输入数据
     * @param algorithm 哈希算法
     * @return 哈希结果
     */
    CryptoResult hash(const String& data, HashAlgorithm algorithm = HashAlgorithm::SHA256);
    
    /**
     * @brief HMAC计算
     * @param data 输入数据
     * @param key HMAC密钥
     * @param algorithm 哈希算法
     * @return HMAC结果
     */
    CryptoResult hmac(const String& data, const String& key, HashAlgorithm algorithm = HashAlgorithm::SHA256);
    
    /**
     * @brief AES加密
     * @param plaintext 明文
     * @param key 密钥
     * @param iv 初始化向量（CBC/GCM模式需要）
     * @param aad 附加认证数据（GCM模式需要）
     * @return 加密结果
     */
    CryptoResult encrypt(const String& plaintext, const String& key, 
                        const String& iv = "", const String& aad = "");
    
    /**
     * @brief AES解密
     * @param ciphertext 密文
     * @param key 密钥
     * @param iv 初始化向量（CBC/GCM模式需要）
     * @param aad 附加认证数据（GCM模式需要）
     * @return 解密结果
     */
    CryptoResult decrypt(const String& ciphertext, const String& key, 
                        const String& iv = "", const String& aad = "");
    
    /**
     * @brief Base64编码
     * @param data 原始数据
     * @return 编码结果
     */
    CryptoResult base64Encode(const String& data);
    
    /**
     * @brief Base64解码
     * @param data Base64编码数据
     * @return 解码结果
     */
    CryptoResult base64Decode(const String& data);
    
    /**
     * @brief 生成安全随机字节
     * @param length 字节长度
     * @return 随机字节
     */
    CryptoResult generateRandomBytes(size_t length);
    
    /**
     * @brief 生成随机字符串
     * @param length 字符串长度
     * @param charset 字符集
     * @return 随机字符串
     */
    CryptoResult generateRandomString(size_t length, const String& charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    
    /**
     * @brief 密码派生函数 (PBKDF2)
     * @param password 密码
     * @param salt 盐值
     * @param iterations 迭代次数
     * @param keyLength 派生密钥长度
     * @param algorithm 哈希算法
     * @return 派生密钥
     */
    CryptoResult pbkdf2(const String& password, const String& salt, 
                       uint32_t iterations = 10000, size_t keyLength = 32,
                       HashAlgorithm algorithm = HashAlgorithm::SHA256);
    
    /**
     * @brief 验证密码哈希
     * @param password 密码
     * @param hash 存储的哈希值
     * @param salt 盐值
     * @param iterations 迭代次数
     * @param algorithm 哈希算法
     * @return 验证是否成功
     */
    bool verifyPassword(const String& password, const String& hash, 
                       const String& salt, uint32_t iterations = 10000,
                       HashAlgorithm algorithm = HashAlgorithm::SHA256);
    
    /**
     * @brief 生成安全的密码哈希（包含盐值）
     * @param password 密码
     * @param iterations 迭代次数
     * @param algorithm 哈希算法
     * @return 包含哈希和盐值的结果
     */
    CryptoResult createPasswordHash(const String& password, uint32_t iterations = 10000,
                                   HashAlgorithm algorithm = HashAlgorithm::SHA256);
    
    /**
     * @brief 数据完整性校验
     * @param data 数据
     * @param checksum 校验和
     * @param algorithm 哈希算法
     * @return 校验是否成功
     */
    bool verifyChecksum(const String& data, const String& checksum, 
                       HashAlgorithm algorithm = HashAlgorithm::SHA256);
    
    /**
     * @brief 获取加密配置
     * @return 当前配置
     */
    CryptoConfig getConfig() const;
    
    /**
     * @brief 更新加密配置
     * @param newConfig 新配置
     * @return 更新是否成功
     */
    bool updateConfig(const CryptoConfig& newConfig);
    
    // 静态工具方法
public:
    /**
     * @brief 字节数组转十六进制字符串
     * @param data 字节数组
     * @param length 数据长度
     * @return 十六进制字符串
     */
    static String bytesToHex(const uint8_t* data, size_t length);
    
    /**
     * @brief 十六进制字符串转字节数组
     * @param hex 十六进制字符串
     * @param output 输出缓冲区
     * @param maxLength 最大长度
     * @return 实际转换长度
     */
    static size_t hexToBytes(const String& hex, uint8_t* output, size_t maxLength);
    
    /**
     * @brief 简单的哈希函数（用于兼容性）
     * @param input 输入字符串
     * @return 哈希值
     */
    static String simpleHash(const String& input);
    
    /**
     * @brief 生成UUID
     * @return UUID字符串
     */
    static String generateUUID();
    
    /**
     * @brief 计算字符串的CRC32校验和
     * @param data 输入数据
     * @return CRC32值
     */
    static uint32_t crc32(const String& data);
    
    /**
     * @brief 安全内存清零
     * @param data 数据指针
     * @param length 数据长度
     */
    static void secureZero(void* data, size_t length);
    
    /**
     * @brief 常量时间比较（防止时序攻击）
     * @param a 数据A
     * @param b 数据B
     * @param length 数据长度
     * @return 是否相等
     */
    static bool constantTimeCompare(const uint8_t* a, const uint8_t* b, size_t length);
};

#endif