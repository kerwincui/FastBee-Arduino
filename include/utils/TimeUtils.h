#ifndef TIME_UTILS_H
#define TIME_UTILS_H

#include <Arduino.h>
#include <time.h>

/**
 * @brief 时间工具类
 * 
 * 提供各种时间处理功能：
 * - 时间戳转换
 * - 日期时间格式化
 * - 时区处理
 * - 时间计算
 * - 定时器功能
 * - NTP时间同步
 */
class TimeUtils {
public:
    // 禁止实例化，所有方法都是静态的
    TimeUtils() = delete;
    TimeUtils(const TimeUtils&) = delete;
    TimeUtils& operator=(const TimeUtils&) = delete;


    /**
     * @brief 时间格式枚举
     */
    enum TimeFormat {
        ISO8601 = 0,        // YYYY-MM-DDTHH:MM:SSZ
        RFC822 = 1,         // Wed, 02 Oct 2002 08:00:00 EST
        HUMAN_READABLE = 2, // 2002-10-02 08:00:00
        TIME_ONLY = 3,      // 08:00:00
        DATE_ONLY = 4       // 2002-10-02
    };

    /**
     * @brief 时区结构体
     */
    struct TimeZone {
        String name;
        int offset; // 分钟偏移
        bool dst;   // 是否使用夏令时
        
        TimeZone() : name("UTC"), offset(0), dst(false) {}
        TimeZone(const String& n, int o, bool d = false) : name(n), offset(o), dst(d) {}
    };

    /**
     * @brief 初始化时间系统
     * @param timezone 时区配置
     * @param ntpServer NTP服务器
     * @return 初始化是否成功
     */
    static bool initialize(const TimeZone& timezone = TimeZone(), const String& ntpServer = "pool.ntp.org");
    
    /**
     * @brief 同步NTP时间
     * @param timeout 超时时间（毫秒）
     * @return 同步是否成功
     */
    static bool syncNTP(unsigned long timeout = 10000);
    
    /**
     * @brief 通过 HTTP 接口同步时间（支持 FastBee NTP HTTP 接口）
     * 使用 NTP 四报文算法: now = (serverRecvTime + serverSendTime + deviceRecvTime - deviceSendTime) / 2
     * @param url HTTP/HTTPS NTP 服务器 URL（末尾不需要带时间戳，会自动拼接）
     * @param timeout 超时时间（毫秒）
     * @return 同步是否成功
     */
    static bool syncNTPFromHTTP(const String& url, unsigned long timeout = 10000);

    /**
     * @brief 通过 HTTP 接口同步时间并返回计算后的毫秒时间戳
     * @param url HTTP/HTTPS NTP 服务器 URL
     * @param outTimestampMs 输出：计算后的服务器时间（毫秒，Unix时间戳）
     * @param timeout 超时时间（毫秒）
     * @return 同步是否成功
     */
    static bool syncNTPFromHTTPWithTimestamp(const String& url, long long& outTimestampMs, unsigned long timeout = 10000);
    
    /**
     * @brief 获取当前时间戳（秒）
     * @return Unix时间戳
     */
    static time_t getTimestamp();
    
    /**
     * @brief 获取当前时间戳（毫秒）
     * @return 毫秒时间戳
     */
    static unsigned long getTimestampMs();
    
    /**
     * @brief 获取系统运行时间（毫秒）
     * @return 运行时间
     */
    static unsigned long getUptime();
    
    /**
     * @brief 格式化时间
     * @param timestamp 时间戳
     * @param format 时间格式
     * @return 格式化后的时间字符串
     */
    static String formatTime(time_t timestamp, TimeFormat format = ISO8601);
    
    /**
     * @brief 格式化当前时间
     * @param format 时间格式
     * @return 格式化后的时间字符串
     */
    static String formatCurrentTime(TimeFormat format = ISO8601);
    
    /**
     * @brief 解析时间字符串
     * @param timeString 时间字符串
     * @param format 时间格式
     * @return 时间戳
     */
    static time_t parseTime(const String& timeString, TimeFormat format = ISO8601);
    
    /**
     * @brief 获取时间结构体
     * @param timestamp 时间戳
     * @return tm结构体
     */
    static struct tm getTimeStruct(time_t timestamp = 0);
    
    /**
     * @brief 设置时区
     * @param timezone 时区配置
     * @return 设置是否成功
     */
    static bool setTimeZone(const TimeZone& timezone);
    
    /**
     * @brief 获取当前时区
     * @return 时区配置
     */
    static TimeZone getCurrentTimeZone();
    
    /**
     * @brief 时间加法
     * @param timestamp 基础时间戳
     * @param seconds 增加的秒数
     * @return 新的时间戳
     */
    static time_t addSeconds(time_t timestamp, long seconds);
    
    /**
     * @brief 时间加法（分钟）
     * @param timestamp 基础时间戳
     * @param minutes 增加的分钟数
     * @return 新的时间戳
     */
    static time_t addMinutes(time_t timestamp, long minutes);
    
    /**
     * @brief 时间加法（小时）
     * @param timestamp 基础时间戳
     * @param hours 增加的小时数
     * @return 新的时间戳
     */
    static time_t addHours(time_t timestamp, long hours);
    
    /**
     * @brief 时间加法（天数）
     * @param timestamp 基础时间戳
     * @param days 增加的天数
     * @return 新的时间戳
     */
    static time_t addDays(time_t timestamp, long days);
    
    /**
     * @brief 时间差计算（秒）
     * @param time1 时间1
     * @param time2 时间2
     * @return 时间差（秒）
     */
    static long timeDifference(time_t time1, time_t time2);
    
    /**
     * @brief 检查是否为同一天
     * @param time1 时间1
     * @param time2 时间2
     * @return 是否为同一天
     */
    static bool isSameDay(time_t time1, time_t time2);
    
    /**
     * @brief 获取当天的开始时间（00:00:00）
     * @param timestamp 时间戳
     * @return 当天开始时间戳
     */
    static time_t getDayStart(time_t timestamp = 0);
    
    /**
     * @brief 获取当天的结束时间（23:59:59）
     * @param timestamp 时间戳
     * @return 当天结束时间戳
     */
    static time_t getDayEnd(time_t timestamp = 0);
    
    /**
     * @brief 获取星期几
     * @param timestamp 时间戳
     * @return 星期几（0-6，0=周日）
     */
    static int getDayOfWeek(time_t timestamp = 0);
    
    /**
     * @brief 获取月份中的第几天
     * @param timestamp 时间戳
     * @return 天数（1-31）
     */
    static int getDayOfMonth(time_t timestamp = 0);
    
    /**
     * @brief 获取年份中的第几天
     * @param timestamp 时间戳
     * @return 天数（1-366）
     */
    static int getDayOfYear(time_t timestamp = 0);
    
    /**
     * @brief 格式化时间间隔
     * @param milliseconds 毫秒数
     * @param showMilliseconds 是否显示毫秒
     * @return 格式化的时间间隔
     */
    static String formatDuration(unsigned long milliseconds, bool showMilliseconds = false);
    
    /**
     * @brief 延迟执行（非阻塞）
     * @param milliseconds 延迟毫秒数
     * @param callback 回调函数
     * @return 定时器ID
     */
    static int setTimeout(unsigned long milliseconds, void (*callback)());
    
    /**
     * @brief 间隔执行（非阻塞）
     * @param milliseconds 间隔毫秒数
     * @param callback 回调函数
     * @return 定时器ID
     */
    static int setInterval(unsigned long milliseconds, void (*callback)());
    
    /**
     * @brief 清除定时器
     * @param timerId 定时器ID
     */
    static void clearTimer(int timerId);
    
    /**
     * @brief 检查时间是否在指定范围内
     * @param checkTime 检查的时间
     * @param startTime 开始时间
     * @param endTime 结束时间
     * @return 是否在范围内
     */
    static bool isTimeInRange(time_t checkTime, time_t startTime, time_t endTime);
    
    /**
     * @brief 获取预定义的时区
     * @param name 时区名称
     * @return 时区配置
     */
    static TimeZone getTimeZoneByName(const String& name);
    
    /**
     * @brief 检查是否为闰年
     * @param year 年份
     * @return 是否为闰年
     */
    static bool isLeapYear(int year);
    
    /**
     * @brief 获取月份的天数
     * @param year 年份
     * @param month 月份（1-12）
     * @return 天数
     */
    static int getDaysInMonth(int year, int month);
    
    /**
     * @brief 获取CPU时间（微秒）
     * @return CPU时间
     */
    static unsigned long getCPUTime();
};

#endif