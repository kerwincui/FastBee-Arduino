#ifndef BLE_PROVISION_TYPES_H
#define BLE_PROVISION_TYPES_H

#include <Arduino.h>

/**
 * @brief BLE 配网载荷结构体
 *
 * 手机端通过 BLE UART RX 特征值下发的 JSON 配网数据：
 *   {"ssid":"...","password":"...","userId":"...","deviceNum":"...","extra":"..."}
 * 字段语义与 AP 配网接口 /api/wifi/config 保持一致：
 *   - ssid/password：目标 WiFi 凭证（ssid 必填）
 *   - userId       → device.json.userId
 *   - deviceNum    → device.json.deviceId
 *   - extra        → device.json.productNumber（仅有效正整数时写入）
 */
struct BleProvisionPayload {
    String ssid;
    String password;
    String userId;
    String deviceNum;
    String extra;

    bool hasUserId   = false;   // userId 字段存在且非空
    bool hasDeviceNum = false;  // deviceNum 字段存在且非空
    bool hasExtra    = false;   // extra 字段存在且非空
};

/**
 * @brief 解析 BLE 配网 JSON 载荷
 * @param json 原始 JSON 字符串
 * @param out  解析成功时写入的载荷
 * @return ssid 有效且 JSON 解析成功返回 true；否则 false
 * @note 兼容旧版示例协议：password 缺失或为字符串 "null" 时按空密码处理；
 *       ssid 为字符串 "null" 视为无效（与历史蓝牙配网示例行为一致）
 */
bool parseBleProvisionPayload(const String& json, BleProvisionPayload& out);

/**
 * @brief 构建配网结果通知内容（TX 特征值 notify 值）
 * @param success 配网是否成功
 * @return "true" 或 "false"（与历史蓝牙配网示例及手机端 App 协议兼容）
 */
const char* buildBleProvisionResult(bool success);

#endif // BLE_PROVISION_TYPES_H
