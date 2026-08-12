/**
 * @description: 433MHz RF 遥控学习路由处理器
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 */

#ifndef RF_ROUTE_HANDLER_H
#define RF_ROUTE_HANDLER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_RF_LEARN

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 433MHz RF 遥控学习路由处理器
 *
 * 处理所有 /api/rf/* 端点：
 * - /api/rf/status        - RF 接收器与按键映射状态
 * - /api/rf/learn/start   - 进入限时学习模式（对码）
 * - /api/rf/learn/status  - 查询学习进度/捕获结果
 * - /api/rf/learn/stop    - 取消学习模式
 * - /api/rf/keys          - 列出按键映射（GET）/ 新增或更新（POST）
 * - /api/rf/keys/delete   - 删除单个按键
 * - /api/rf/keys/clear    - 清除全部按键
 */
class RfRouteHandler
{
public:
    explicit RfRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleStatus(AsyncWebServerRequest* request);
    void handleLearnStart(AsyncWebServerRequest* request);
    void handleLearnStatus(AsyncWebServerRequest* request);
    void handleLearnStop(AsyncWebServerRequest* request);
    void handleListKeys(AsyncWebServerRequest* request);
    void handleSaveKey(AsyncWebServerRequest* request);
    void handleDeleteKey(AsyncWebServerRequest* request);
    void handleClearKeys(AsyncWebServerRequest* request);
};

#endif // FASTBEE_ENABLE_RF_LEARN
#endif // RF_ROUTE_HANDLER_H
