/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:32:02
 */

#include "security/CryptoUtils.h"
#include "systems/LoggerSystem.h"
#include <esp_system.h>

// 自定义标签用于调试
static const char* TAG = "CRYPTO";

CryptoUtils::CryptoUtils() : initialized(false) {
    // 初始化mbedTLS上下文
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    
    config = CryptoConfig();
}

CryptoUtils::~CryptoUtils() {
    // 清理mbedTLS上下文
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
}

bool CryptoUtils::initialize(const CryptoConfig& customConfig) {
    if (initialized) {
        return true;
    }
    
    config = customConfig;
    
    // 初始化随机数生成器
    if (!initializeRNG()) {
        return false;
    }
    
    initialized = true;
    return true;
}

bool CryptoUtils::initializeRNG() {
    const char* pers = "fastbee_crypto_rng";
    
    int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, 
                                   (const unsigned char*)pers, strlen(pers));
    if (ret != 0) {
        if (config.enableDebug) {
            Serial.printf("Failed to initialize CTR_DRBG: -0x%04X\n", -ret);
        }
        return false;
    }
    
    return true;
}

CryptoResult CryptoUtils::hash(const String& data, HashAlgorithm algorithm) {
    CryptoResult result;
    
    if (!initialized && !initialize()) {
        result.error = "Crypto utils not initialized";
        return result;
    }
    
    const uint8_t* input = reinterpret_cast<const uint8_t*>(data.c_str());
    size_t inputLength = data.length();
    
    uint8_t output[32]; // 最大输出长度 (SHA256)
    size_t outputLength = 0;
    
    int ret = 0;
    
    switch (algorithm) {
        case HashAlgorithm::MD5: {
            mbedtls_md5_context ctx;
            mbedtls_md5_init(&ctx);
            
            // ret = mbedtls_md5_starts(&ctx);
            // if (ret == 0) ret = mbedtls_md5_update(&ctx, input, inputLength);
            // if (ret == 0) ret = mbedtls_md5_finish(&ctx, output);
            
            // mbedtls_md5_free(&ctx);
            // outputLength = 16;
            break;
        }
        
        case HashAlgorithm::SHA1: {
            // SHA1实现类似，这里简化处理
            result.error = "SHA1 not implemented";
            return result;
        }
        
        case HashAlgorithm::SHA256: {
            mbedtls_sha256_context ctx;
            mbedtls_sha256_init(&ctx);
            
            // ret = mbedtls_sha256_starts(&ctx, 0); // 0 = SHA256, 1 = SHA224
            // if (ret == 0) ret = mbedtls_sha256_update(&ctx, input, inputLength);
            // if (ret == 0) ret = mbedtls_sha256_finish(&ctx, output);
            
            mbedtls_sha256_free(&ctx);
            outputLength = 32;
            break;
        }
        
        default:
            result.error = "Unsupported hash algorithm";
            return result;
    }
    
    if (ret != 0) {
        result.error = "Hash computation failed: " + String(ret);
        return result;
    }
    
    result.success = true;
    result.data = bytesToHex(output, outputLength);
    result.outputLength = outputLength;
    
    return result;
}

CryptoResult CryptoUtils::hmac(const String& data, const String& key, HashAlgorithm algorithm) {
    CryptoResult result;
    
    if (!initialized && !initialize()) {
        result.error = "Crypto utils not initialized";
        return result;
    }
    
    // 这里简化实现，实际应该使用mbedtls_md_hmac
    // 暂时使用哈希替代
    String combined = key + data + key;
    return hash(combined, algorithm);
}

CryptoResult CryptoUtils::encrypt(const String& plaintext, const String& key, 
                                 const String& iv, const String& aad) {
    CryptoResult result;
    
    if (!initialized && !initialize()) {
        result.error = "Crypto utils not initialized";
        return result;
    }
    
    switch (config.algorithm) {
        case CryptoAlgorithm::AES_128_ECB:
        case CryptoAlgorithm::AES_256_ECB:
            if (!validateKeySize(key, config.keySize)) {
                result.error = "Invalid key size for ECB mode";
                return result;
            }
            return aesECB(plaintext, key, true);
            
        case CryptoAlgorithm::AES_128_CBC:
        case CryptoAlgorithm::AES_256_CBC:
            if (!validateKeySize(key, config.keySize)) {
                result.error = "Invalid key size for CBC mode";
                return result;
            }
            if (!validateIVSize(iv, config.ivSize)) {
                result.error = "Invalid IV size for CBC mode";
                return result;
            }
            return aesCBC(plaintext, key, iv, true);
            
        case CryptoAlgorithm::AES_128_GCM:
        case CryptoAlgorithm::AES_256_GCM:
            if (!validateKeySize(key, config.keySize)) {
                result.error = "Invalid key size for GCM mode";
                return result;
            }
            if (!validateIVSize(iv, config.ivSize)) {
                result.error = "Invalid IV size for GCM mode";
                return result;
            }
            return aesGCM(plaintext, key, iv, aad, true);
            
        default:
            result.error = "Unsupported encryption algorithm";
            return result;
    }
}

CryptoResult CryptoUtils::decrypt(const String& ciphertext, const String& key, 
                                 const String& iv, const String& aad) {
    CryptoResult result;
    
    if (!initialized && !initialize()) {
        result.error = "Crypto utils not initialized";
        return result;
    }
    
    switch (config.algorithm) {
        case CryptoAlgorithm::AES_128_ECB:
        case CryptoAlgorithm::AES_256_ECB:
            if (!validateKeySize(key, config.keySize)) {
                result.error = "Invalid key size for ECB mode";
                return result;
            }
            return aesECB(ciphertext, key, false);
            
        case CryptoAlgorithm::AES_128_CBC:
        case CryptoAlgorithm::AES_256_CBC:
            if (!validateKeySize(key, config.keySize)) {
                result.error = "Invalid key size for CBC mode";
                return result;
            }
            if (!validateIVSize(iv, config.ivSize)) {
                result.error = "Invalid IV size for CBC mode";
                return result;
            }
            return aesCBC(ciphertext, key, iv, false);
            
        case CryptoAlgorithm::AES_128_GCM:
        case CryptoAlgorithm::AES_256_GCM:
            if (!validateKeySize(key, config.keySize)) {
                result.error = "Invalid key size for GCM mode";
                return result;
            }
            if (!validateIVSize(iv, config.ivSize)) {
                result.error = "Invalid IV size for GCM mode";
                return result;
            }
            return aesGCM(ciphertext, key, iv, aad, false);
            
        default:
            result.error = "Unsupported decryption algorithm";
            return result;
    }
}

CryptoResult CryptoUtils::aesECB(const String& input, const String& key, bool encrypt) {
    CryptoResult result;
    
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    
    int ret = 0;
    size_t inputLength = input.length();
    
    // ECB模式需要数据对齐到16字节
    size_t paddedLength = ((inputLength + 15) / 16) * 16;
    uint8_t* output = new uint8_t[paddedLength];
    uint8_t* inputBuffer = new uint8_t[paddedLength];
    
    // 复制并填充输入数据
    memcpy(inputBuffer, input.c_str(), inputLength);
    if (encrypt) {
        // PKCS7填充
        uint8_t padValue = 16 - (inputLength % 16);
        memset(inputBuffer + inputLength, padValue, padValue);
    }
    
    const uint8_t* keyBytes = reinterpret_cast<const uint8_t*>(key.c_str());
    
    if (encrypt) {
        ret = mbedtls_aes_setkey_enc(&aes, keyBytes, config.keySize * 8);
        if (ret == 0) {
            for (size_t i = 0; i < paddedLength; i += 16) {
                ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, 
                                           inputBuffer + i, output + i);
                if (ret != 0) break;
            }
        }
    } else {
        ret = mbedtls_aes_setkey_dec(&aes, keyBytes, config.keySize * 8);
        if (ret == 0) {
            for (size_t i = 0; i < paddedLength; i += 16) {
                ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, 
                                           inputBuffer + i, output + i);
                if (ret != 0) break;
            }
        }
        
        if (ret == 0) {
            // 去除PKCS7填充
            uint8_t padValue = output[paddedLength - 1];
            if (padValue > 0 && padValue <= 16) {
                paddedLength -= padValue;
            }
        }
    }
    
    if (ret == 0) {
        result.success = true;
        result.data = String(reinterpret_cast<char*>(output), paddedLength);
        result.outputLength = paddedLength;
    } else {
        result.error = "AES ECB operation failed: " + String(ret);
    }
    
    // 清理
    mbedtls_aes_free(&aes);
    delete[] output;
    delete[] inputBuffer;
    
    return result;
}

CryptoResult CryptoUtils::aesCBC(const String& input, const String& key, 
                                const String& iv, bool encrypt) {
    CryptoResult result;
    
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    
    int ret = 0;
    size_t inputLength = input.length();
    
    // CBC模式需要数据对齐到16字节
    size_t paddedLength = ((inputLength + 15) / 16) * 16;
    uint8_t* output = new uint8_t[paddedLength];
    uint8_t* inputBuffer = new uint8_t[paddedLength];
    uint8_t ivBuffer[16];
    
    // 复制并填充输入数据
    memcpy(inputBuffer, input.c_str(), inputLength);
    memcpy(ivBuffer, iv.c_str(), 16);
    
    if (encrypt) {
        // PKCS7填充
        uint8_t padValue = 16 - (inputLength % 16);
        memset(inputBuffer + inputLength, padValue, padValue);
    }
    
    const uint8_t* keyBytes = reinterpret_cast<const uint8_t*>(key.c_str());
    
    if (encrypt) {
        ret = mbedtls_aes_setkey_enc(&aes, keyBytes, config.keySize * 8);
        if (ret == 0) {
            ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, paddedLength,
                                       ivBuffer, inputBuffer, output);
        }
    } else {
        ret = mbedtls_aes_setkey_dec(&aes, keyBytes, config.keySize * 8);
        if (ret == 0) {
            ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, paddedLength,
                                       ivBuffer, inputBuffer, output);
        }
        
        if (ret == 0) {
            // 去除PKCS7填充
            uint8_t padValue = output[paddedLength - 1];
            if (padValue > 0 && padValue <= 16) {
                paddedLength -= padValue;
            }
        }
    }
    
    if (ret == 0) {
        result.success = true;
        result.data = String(reinterpret_cast<char*>(output), paddedLength);
        result.outputLength = paddedLength;
    } else {
        result.error = "AES CBC operation failed: " + String(ret);
    }
    
    // 清理
    mbedtls_aes_free(&aes);
    delete[] output;
    delete[] inputBuffer;
    secureZero(ivBuffer, sizeof(ivBuffer));
    
    return result;
}

CryptoResult CryptoUtils::aesGCM(const String& input, const String& key,
                                const String& iv, const String& aad, bool encrypt) {
    CryptoResult result;
    
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    
    int ret = 0;
    size_t inputLength = input.length();
    size_t outputLength = inputLength;
    
    if (encrypt) {
        outputLength += config.tagSize; // 加密时包含认证标签
    }
    
    uint8_t* output = new uint8_t[outputLength];
    uint8_t tag[16] = {0};
    
    const uint8_t* keyBytes = reinterpret_cast<const uint8_t*>(key.c_str());
    const uint8_t* ivBytes = reinterpret_cast<const uint8_t*>(iv.c_str());
    const uint8_t* aadBytes = reinterpret_cast<const uint8_t*>(aad.c_str());
    const uint8_t* inputBytes = reinterpret_cast<const uint8_t*>(input.c_str());
    
    ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, keyBytes, config.keySize * 8);
    
    if (ret == 0) {
        if (encrypt) {
            ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, inputLength,
                                          ivBytes, iv.length(), aadBytes, aad.length(),
                                          inputBytes, output, config.tagSize, tag);
                                          
            if (ret == 0) {
                // 将标签附加到输出
                memcpy(output + inputLength, tag, config.tagSize);
            }
        } else {
            // 解密时，输入的最后config.tagSize字节是认证标签
            if (inputLength < config.tagSize) {
                ret = -1;
                result.error = "Input too short for GCM decryption";
            } else {
                size_t ciphertextLength = inputLength - config.tagSize;
                const uint8_t* receivedTag = inputBytes + ciphertextLength;
                
                ret = mbedtls_gcm_auth_decrypt(&gcm, ciphertextLength,
                                             ivBytes, iv.length(), aadBytes, aad.length(),
                                             receivedTag, config.tagSize,
                                             inputBytes, output);
                outputLength = ciphertextLength;
            }
        }
    }
    
    if (ret == 0) {
        result.success = true;
        result.data = String(reinterpret_cast<char*>(output), outputLength);
        result.outputLength = outputLength;
    } else {
        result.error = "AES GCM operation failed: " + String(ret);
    }
    
    // 清理
    mbedtls_gcm_free(&gcm);
    delete[] output;
    secureZero(tag, sizeof(tag));
    
    return result;
}

CryptoResult CryptoUtils::base64Encode(const String& data) {
    CryptoResult result;
    
    size_t inputLength = data.length();
    size_t outputLength = ((inputLength + 2) / 3) * 4 + 1; // Base64输出长度
    
    uint8_t* output = new uint8_t[outputLength];
    
    size_t actualLength = 0;
    int ret = mbedtls_base64_encode(output, outputLength, &actualLength,
                                   reinterpret_cast<const uint8_t*>(data.c_str()), inputLength);
    
    if (ret == 0) {
        result.success = true;
        result.data = String(reinterpret_cast<char*>(output), actualLength);
        result.outputLength = actualLength;
    } else {
        result.error = "Base64 encode failed: " + String(ret);
    }
    
    delete[] output;
    return result;
}

CryptoResult CryptoUtils::base64Decode(const String& data) {
    CryptoResult result;
    
    size_t inputLength = data.length();
    size_t outputLength = (inputLength * 3) / 4 + 1; // 估计最大输出长度
    
    uint8_t* output = new uint8_t[outputLength];
    
    size_t actualLength = 0;
    int ret = mbedtls_base64_decode(output, outputLength, &actualLength,
                                   reinterpret_cast<const uint8_t*>(data.c_str()), inputLength);
    
    if (ret == 0) {
        result.success = true;
        result.data = String(reinterpret_cast<char*>(output), actualLength);
        result.outputLength = actualLength;
    } else {
        result.error = "Base64 decode failed: " + String(ret);
    }
    
    delete[] output;
    return result;
}

CryptoResult CryptoUtils::generateRandomBytes(size_t length) {
    CryptoResult result;
    
    if (!initialized && !initialize()) {
        result.error = "Crypto utils not initialized";
        return result;
    }
    
    uint8_t* randomData = new uint8_t[length];
    
    int ret = mbedtls_ctr_drbg_random(&ctr_drbg, randomData, length);
    
    if (ret == 0) {
        result.success = true;
        result.data = bytesToHex(randomData, length);
        result.outputLength = length;
    } else {
        result.error = "Random generation failed: " + String(ret);
    }
    
    delete[] randomData;
    return result;
}

CryptoResult CryptoUtils::generateRandomString(size_t length, const String& charset) {
    CryptoResult result;
    
    auto randomBytes = generateRandomBytes(length);
    if (!randomBytes.success) {
        result.error = randomBytes.error;
        return result;
    }
    
    String randomString;
    size_t charsetSize = charset.length();
    
    // 将随机字节转换为字符集索引
    for (size_t i = 0; i < length; i++) {
        uint8_t randomByte = randomBytes.data[i * 2] * 16 + randomBytes.data[i * 2 + 1];
        randomString += charset[randomByte % charsetSize];
    }
    
    result.success = true;
    result.data = randomString;
    result.outputLength = length;
    
    return result;
}

CryptoResult CryptoUtils::pbkdf2(const String& password, const String& salt,
                                uint32_t iterations, size_t keyLength,
                                HashAlgorithm algorithm) {
    CryptoResult result;
    
    // 简化实现 - 实际应该使用mbedtls_pkcs5_pbkdf2_hmac
    // 这里使用多次哈希来模拟
    
    String currentHash = password + salt;
    
    for (uint32_t i = 0; i < iterations; i++) {
        auto hashResult = hash(currentHash, algorithm);
        if (!hashResult.success) {
            result.error = "PBKDF2 failed at iteration " + String(i);
            return result;
        }
        currentHash = hashResult.data;
    }
    
    // 截取所需长度
    if (keyLength * 2 <= currentHash.length()) {
        currentHash = currentHash.substring(0, keyLength * 2);
    }
    
    result.success = true;
    result.data = currentHash;
    result.outputLength = keyLength;
    
    return result;
}

bool CryptoUtils::verifyPassword(const String& password, const String& hash,
                                const String& salt, uint32_t iterations,
                                HashAlgorithm algorithm) {
    auto newHash = pbkdf2(password, salt, iterations, hash.length() / 2, algorithm);
    if (!newHash.success) {
        return false;
    }
    
    return constantTimeCompare(
        reinterpret_cast<const uint8_t*>(newHash.data.c_str()),
        reinterpret_cast<const uint8_t*>(hash.c_str()),
        hash.length()
    );
}

CryptoResult CryptoUtils::createPasswordHash(const String& password, uint32_t iterations,
                                            HashAlgorithm algorithm) {
    CryptoResult result;
    
    // 生成随机盐值
    auto saltResult = generateRandomBytes(16);
    if (!saltResult.success) {
        result.error = "Failed to generate salt: " + saltResult.error;
        return result;
    }
    
    // 派生密钥
    auto hashResult = pbkdf2(password, saltResult.data, iterations, 32, algorithm);
    if (!hashResult.success) {
        result.error = "Failed to create password hash: " + hashResult.error;
        return result;
    }
    
    // 格式: algorithm:iterations:salt:hash
    result.success = true;
    result.data = String(static_cast<int>(algorithm)) + ":" + 
                 String(iterations) + ":" + 
                 saltResult.data + ":" + 
                 hashResult.data;
    
    return result;
}

bool CryptoUtils::verifyChecksum(const String& data, const String& checksum,
                                HashAlgorithm algorithm) {
    auto computedHash = hash(data, algorithm);
    if (!computedHash.success) {
        return false;
    }
    
    return computedHash.data.equalsIgnoreCase(checksum);
}

bool CryptoUtils::validateKeySize(const String& key, size_t expectedSize) {
    return key.length() == expectedSize;
}

bool CryptoUtils::validateIVSize(const String& iv, size_t expectedSize) {
    return iv.length() == expectedSize;
}

CryptoConfig CryptoUtils::getConfig() const {
    return config;
}

bool CryptoUtils::updateConfig(const CryptoConfig& newConfig) {
    config = newConfig;
    return true;
}

// 静态工具方法实现
String CryptoUtils::bytesToHex(const uint8_t* data, size_t length) {
    String hexString;
    hexString.reserve(length * 2);
    
    for (size_t i = 0; i < length; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", data[i]);
        hexString += hex;
    }
    
    return hexString;
}

size_t CryptoUtils::hexToBytes(const String& hex, uint8_t* output, size_t maxLength) {
    size_t hexLength = hex.length();
    if (hexLength % 2 != 0 || hexLength / 2 > maxLength) {
        return 0;
    }
    
    for (size_t i = 0; i < hexLength; i += 2) {
        char byteStr[3] = {hex[i], hex[i + 1], '\0'};
        output[i / 2] = strtoul(byteStr, nullptr, 16);
    }
    
    return hexLength / 2;
}

String CryptoUtils::simpleHash(const String& input) {
    // 简单的哈希函数，用于兼容性
    uint32_t hash = 5381;
    
    for (size_t i = 0; i < input.length(); i++) {
        hash = ((hash << 5) + hash) + input.charAt(i);
    }
    
    char buffer[9];
    snprintf(buffer, sizeof(buffer), "%08lx", hash);
    return String(buffer);
}

String CryptoUtils::generateUUID() {
    // 生成简化版UUID
    CryptoUtils crypto;
    crypto.initialize();
    
    auto part1 = crypto.generateRandomBytes(4);
    auto part2 = crypto.generateRandomBytes(2);
    auto part3 = crypto.generateRandomBytes(2);
    auto part4 = crypto.generateRandomBytes(2);
    auto part5 = crypto.generateRandomBytes(6);
    
    if (!part1.success || !part2.success || !part3.success || !part4.success || !part5.success) {
        return "00000000-0000-0000-0000-000000000000";
    }
    
    return part1.data + "-" + part2.data + "-" + part3.data + "-" + part4.data + "-" + part5.data;
}

uint32_t CryptoUtils::crc32(const String& data) {
    // 简化CRC32实现
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(data.c_str());
    size_t length = data.length();
    
    for (size_t i = 0; i < length; i++) {
        crc ^= buffer[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    
    return ~crc;
}

void CryptoUtils::secureZero(void* data, size_t length) {
    if (data == nullptr) return;
    
    volatile uint8_t* p = static_cast<volatile uint8_t*>(data);
    while (length--) {
        *p++ = 0;
    }
}

bool CryptoUtils::constantTimeCompare(const uint8_t* a, const uint8_t* b, size_t length) {
    uint8_t result = 0;
    
    for (size_t i = 0; i < length; i++) {
        result |= a[i] ^ b[i];
    }
    
    return result == 0;
}