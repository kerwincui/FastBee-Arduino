/**
 * @description: 红外遥控路由处理器
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 */

#ifndef IR_ROUTE_HANDLER_H
#define IR_ROUTE_HANDLER_H

#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_IR_REMOTE

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief 红外遥控路由处理器
 *
 * 处理所有 /api/ir/* 端点：
 * - /api/ir/status        - 红外接收器与按键映射状态
 * - /api/ir/learn/start   - 进入限时学习模式（对码）
 * - /api/ir/learn/status  - 查询学习进度/捕获结果
 * - /api/ir/learn/stop    - 取消学习模式
 * - /api/ir/keys          - 列出按键映射（GET）/ 新增或更新（POST，对码保存）
 * - /api/ir/keys/delete   - 删除单个按键（清码）
 * - /api/ir/keys/clear    - 清除全部按键（清码）
 */
class IrRouteHandler
{
public:
    explicit IrRouteHandler(WebHandlerContext* ctx);

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

#endif // FASTBEE_ENABLE_IR_REMOTE
#endif // IR_ROUTE_HANDLER_H
