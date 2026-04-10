#ifndef MODBUS_ROUTE_HANDLER_H
#define MODBUS_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>

class WebHandlerContext;

/**
 * @brief Modbus协议路由处理器
 * 
 * 处理所有 /api/modbus/* 端点：
 * - /api/modbus/status - 获取Modbus状态
 * - /api/modbus/write - 写单个寄存器
 * - /api/modbus/coil/control - 单个线圈控制
 * - /api/modbus/coil/batch - 批量线圈控制
 * - /api/modbus/coil/delay - 线圈延时控制
 * - /api/modbus/coil/status - 读取线圈状态
 * - /api/modbus/device/address - 读取/设置从站地址
 * - /api/modbus/device/baudrate - 设置波特率
 * - /api/modbus/device/inputs - 读取离散输入
 * - /api/modbus/register/read - 读寄存器
 * - /api/modbus/register/write - 写单个寄存器
 * - /api/modbus/register/batch-write - 批量写寄存器
 */
class ModbusRouteHandler {
public:
    explicit ModbusRouteHandler(WebHandlerContext* ctx);

    void setupRoutes(AsyncWebServer* server);

private:
    WebHandlerContext* ctx;

    void handleGetModbusStatus(AsyncWebServerRequest* request);
    void handleModbusWrite(AsyncWebServerRequest* request);
    
    // Modbus 通用控制 API
    void handleModbusCoilControl(AsyncWebServerRequest* request);
    void handleModbusCoilBatch(AsyncWebServerRequest* request);
    void handleModbusCoilDelay(AsyncWebServerRequest* request);
    void handleModbusCoilStatus(AsyncWebServerRequest* request);
    void handleModbusDeviceAddress(AsyncWebServerRequest* request);
    void handleModbusDeviceBaudrate(AsyncWebServerRequest* request);
    void handleModbusDiscreteInputs(AsyncWebServerRequest* request);
    
    // Modbus 通用寄存器读写 API
    void handleModbusRegisterRead(AsyncWebServerRequest* request);
    void handleModbusRegisterWrite(AsyncWebServerRequest* request);
    void handleModbusRegisterBatchWrite(AsyncWebServerRequest* request);
};

#endif // MODBUS_ROUTE_HANDLER_H
