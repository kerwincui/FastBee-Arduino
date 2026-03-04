/**
 * config.js — HTTP 请求配置入口
 *
 * axios 已替换为原生 fetch 封装（fetch-api.js），节省约 54KB LittleFS 空间。
 * 全局 apiGet / apiPost / apiPut / apiDelete 由 fetch-api.js 提供，接口不变。
 *
 * baseURL 自适应：
 *   - localhost / 127.0.0.1 → http://fastbee.local（开发调试）
 *   - 其他来源 → 当前页面 origin（生产设备访问）
 *
 * 超时：8 秒（ESP32 响应较慢时留足余量）
 * Content-Type：application/x-www-form-urlencoded（POST/PUT）
 * 认证：自动注入 Authorization: Bearer <auth_token>
 * 错误处理：统一 Notification 提示（见 fetch-api.js）
 */

// 此文件无需额外代码，fetch-api.js 已在 index.html 中于本文件之前加载。
// 如需扩展全局请求配置，在此处添加即可。
