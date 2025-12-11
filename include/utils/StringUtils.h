#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <Arduino.h>
#include <vector>
#include <map>
#include <algorithm>
#include <cctype>

/**
 * @brief 字符串工具类
 * 
 * 提供各种字符串处理功能：
 * - 字符串分割和连接
 * - 大小写转换
 * - 空白字符处理
 * - 编码转换
 * - 格式化输出
 * - 正则匹配（简化版）
 * - JSON/XML转义
 */
class StringUtils {
public:
    // 禁止实例化，所有方法都是静态的
    StringUtils() = delete;
    StringUtils(const StringUtils&) = delete;
    StringUtils& operator=(const StringUtils&) = delete;

    /**
     * @brief 分割字符串
     * @param str 输入字符串
     * @param delimiter 分隔符
     * @return 分割后的字符串向量
     */
    static std::vector<String> split(const String& str, char delimiter);
    
    /**
     * @brief 分割字符串（字符串分隔符）
     * @param str 输入字符串
     * @param delimiter 分隔符字符串
     * @return 分割后的字符串向量
     */
    static std::vector<String> split(const String& str, const String& delimiter);
    
    /**
     * @brief 连接字符串向量
     * @param strings 字符串向量
     * @param delimiter 连接分隔符
     * @return 连接后的字符串
     */
    static String join(const std::vector<String>& strings, const String& delimiter = "");
    
    /**
     * @brief 去除字符串两端的空白字符
     * @param str 输入字符串
     * @return 处理后的字符串
     */
    static String trim(const String& str);
    
    /**
     * @brief 去除字符串左端的空白字符
     * @param str 输入字符串
     * @return 处理后的字符串
     */
    static String trimLeft(const String& str);
    
    /**
     * @brief 去除字符串右端的空白字符
     * @param str 输入字符串
     * @return 处理后的字符串
     */
    static String trimRight(const String& str);
    
    /**
     * @brief 转换为小写
     * @param str 输入字符串
     * @return 小写字符串
     */
    static String toLower(const String& str);
    
    /**
     * @brief 转换为大写
     * @param str 输入字符串
     * @return 大写字符串
     */
    static String toUpper(const String& str);
    
    /**
     * @brief 检查字符串是否以指定前缀开头
     * @param str 输入字符串
     * @param prefix 前缀
     * @param caseSensitive 是否区分大小写
     * @return 是否匹配
     */
    static bool startsWith(const String& str, const String& prefix, bool caseSensitive = true);
    
    /**
     * @brief 检查字符串是否以指定后缀结尾
     * @param str 输入字符串
     * @param suffix 后缀
     * @param caseSensitive 是否区分大小写
     * @return 是否匹配
     */
    static bool endsWith(const String& str, const String& suffix, bool caseSensitive = true);
    
    /**
     * @brief 检查字符串是否包含子串
     * @param str 输入字符串
     * @param substring 子串
     * @param caseSensitive 是否区分大小写
     * @return 是否包含
     */
    static bool contains(const String& str, const String& substring, bool caseSensitive = true);
    
    /**
     * @brief 替换字符串中的子串
     * @param str 输入字符串
     * @param from 要替换的子串
     * @param to 替换为的子串
     * @param caseSensitive 是否区分大小写
     * @return 替换后的字符串
     */
    static String replace(const String& str, const String& from, const String& to, bool caseSensitive = true);
    
    /**
     * @brief 替换所有匹配的子串
     * @param str 输入字符串
     * @param from 要替换的子串
     * @param to 替换为的子串
     * @param caseSensitive 是否区分大小写
     * @return 替换后的字符串
     */
    static String replaceAll(const String& str, const String& from, const String& to, bool caseSensitive = true);
    
    /**
     * @brief 字符串格式化（类似printf）
     * @param format 格式字符串
     * @param ... 参数
     * @return 格式化后的字符串
     */
    static String format(const char* format, ...);
    
    /**
     * @brief URL编码
     * @param str 输入字符串
     * @return URL编码后的字符串
     */
    static String urlEncode(const String& str);
    
    /**
     * @brief URL解码
     * @param str URL编码字符串
     * @return 解码后的字符串
     */
    static String urlDecode(const String& str);
    
    /**
     * @brief HTML实体编码
     * @param str 输入字符串
     * @return HTML编码后的字符串
     */
    static String htmlEncode(const String& str);
    
    /**
     * @brief HTML实体解码
     * @param str HTML编码字符串
     * @return 解码后的字符串
     */
    static String htmlDecode(const String& str);
    
    /**
     * @brief JSON字符串转义
     * @param str 输入字符串
     * @return JSON转义后的字符串
     */
    static String jsonEscape(const String& str);
    
    /**
     * @brief 检查字符串是否为空或仅包含空白字符
     * @param str 输入字符串
     * @return 是否为空
     */
    static bool isEmpty(const String& str);
    
    /**
     * @brief 检查字符串是否为数字
     * @param str 输入字符串
     * @return 是否为数字
     */
    static bool isNumeric(const String& str);
    
    /**
     * @brief 检查字符串是否为整数
     * @param str 输入字符串
     * @return 是否为整数
     */
    static bool isInteger(const String& str);
    
    /**
     * @brief 检查字符串是否为浮点数
     * @param str 输入字符串
     * @return 是否为浮点数
     */
    static bool isFloat(const String& str);
    
    /**
     * @brief 字符串转换为整数
     * @param str 输入字符串
     * @param defaultValue 默认值
     * @return 整数值
     */
    static int toInt(const String& str, int defaultValue = 0);
    
    /**
     * @brief 字符串转换为浮点数
     * @param str 输入字符串
     * @param defaultValue 默认值
     * @return 浮点数值
     */
    static float toFloat(const String& str, float defaultValue = 0.0f);
    
    /**
     * @brief 字符串转换为布尔值
     * @param str 输入字符串
     * @param defaultValue 默认值
     * @return 布尔值
     */
    static bool toBool(const String& str, bool defaultValue = false);
    
    /**
     * @brief 填充字符串到指定长度
     * @param str 输入字符串
     * @param length 目标长度
     * @param padChar 填充字符
     * @param left 是否在左侧填充
     * @return 填充后的字符串
     */
    static String pad(const String& str, size_t length, char padChar = ' ', bool left = true);
    
    /**
     * @brief 重复字符串
     * @param str 输入字符串
     * @param times 重复次数
     * @return 重复后的字符串
     */
    static String repeat(const String& str, size_t times);
    
    /**
     * @brief 字符串反转
     * @param str 输入字符串
     * @return 反转后的字符串
     */
    static String reverse(const String& str);
    
    /**
     * @brief 获取字符串的子串
     * @param str 输入字符串
     * @param start 起始位置
     * @param length 子串长度（如果为0则到末尾）
     * @return 子串
     */
    static String substring(const String& str, int start, int length = 0);
    
    /**
     * @brief 计算字符串的MD5哈希（简化实现）
     * @param str 输入字符串
     * @return MD5哈希值
     */
    static String md5(const String& str);
    
    /**
     * @brief 计算字符串的SHA256哈希（简化实现）
     * @param str 输入字符串
     * @return SHA256哈希值
     */
    static String sha256(const String& str);
    
    /**
     * @brief Base64编码
     * @param str 输入字符串
     * @return Base64编码字符串
     */
    static String base64Encode(const String& str);
    
    /**
     * @brief Base64解码
     * @param str Base64编码字符串
     * @return 解码后的字符串
     */
    static String base64Decode(const String& str);
    
    /**
     * @brief 生成随机字符串
     * @param length 字符串长度
     * @param charset 字符集
     * @return 随机字符串
     */
    static String randomString(size_t length, const String& charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
    
    /**
     * @brief 字符串比较（忽略大小写）
     * @param str1 字符串1
     * @param str2 字符串2
     * @return 比较结果
     */
    static int compareIgnoreCase(const String& str1, const String& str2);
    
    /**
     * @brief 移除所有空白字符
     * @param str 输入字符串
     * @return 处理后的字符串
     */
    static String removeWhitespace(const String& str);
    
    /**
     * @brief 计算字符串的字符数（考虑多字节字符）
     * @param str 输入字符串
     * @return 字符数
     */
    static size_t charCount(const String& str);
    
    /**
     * @brief 计算字符串的字节数
     * @param str 输入字符串
     * @return 字节数
     */
    static size_t byteCount(const String& str);
    
    /**
     * @brief 字符串转换为驼峰命名
     * @param str 输入字符串
     * @param capitalizeFirst 是否首字母大写
     * @return 驼峰命名字符串
     */
    static String toCamelCase(const String& str, bool capitalizeFirst = false);
    
    /**
     * @brief 字符串转换为下划线命名
     * @param str 输入字符串
     * @return 下划线命名字符串
     */
    static String toSnakeCase(const String& str);

    /**
     * 构建json响应信息
     */
    static String buildJsonResponse(int status, const String& msg, const String& data = "\"\"");
};

#endif