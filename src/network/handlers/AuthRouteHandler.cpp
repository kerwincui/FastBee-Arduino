#include "./network/handlers/AuthRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./security/AuthManager.h"
#include "./security/UserManager.h"
#include "core/ChipConfig.h"
#include "core/MemoryBudget.h"
#include "systems/LoggerSystem.h"
#include <ArduinoJson.h>
#include <new>

// ============ 登录异步化 ============
// PBKDF2 验证需秒级 CPU：若在 lwIP 线程同步执行，整个网络栈
// （TCP ACK/SSE/其他 API）被阻塞数秒，浏览器套件与并发请求表现为
// 连接超时/复位。因此用 ESPAsyncWebServer 的请求延续（pause/weak_ptr）
// 把登录验证移到独立工作任务，完成后再回包；客户端中途断连时
// weak_ptr 自动失效，不会回包到已销毁的请求。
// AuthManager/UserManager 内部的会话表/用户表已由各自的递归互斥量保护。

namespace {

#if CHIP_DUAL_CORE
static constexpr uint32_t LOGIN_TASK_STACK_BYTES = 8192U;
#else
// C3 field evidence: steady-state internal largest block is about 7.1KB, so
// an 8KB transient stack can never be created even with 25KB total DRAM free.
// Keep the worker transient to avoid permanently consuming 6KB on constrained
// targets; the recorded high-water mark below must be checked after flashing.
static constexpr uint32_t LOGIN_TASK_STACK_BYTES = 6144U;
#endif
// xTaskCreate allocates the stack as one block, but the TCB and Web/lwIP
// working memory do not need to be contiguous with it. Keep only allocator
// metadata/alignment overhead on the largest-block gate, and protect the rest
// with an independent total internal-DRAM reserve.
static constexpr uint32_t LOGIN_TASK_STACK_ALLOCATOR_OVERHEAD_BYTES = 256U;
static constexpr uint32_t LOGIN_TASK_MIN_REMAINING_DRAM_BYTES = 12288U;
static constexpr uint32_t LOGIN_TASK_RETRY_AFTER_SECONDS = 3U;

// 并发登录任务槽：防止恶意并发登录请求堆叠多个 PBKDF2 计算
SemaphoreHandle_t loginSlotMutex() {
    // PBKDF2 is CPU and stack heavy. On single-core C3/C6, two concurrent
    // Login tasks can starve lwIP and fragment the small internal DRAM
    // pool; serialize those requests while retaining two slots on dual-core
    // boards where the second core can keep the network responsive.
#if CHIP_DUAL_CORE
    static SemaphoreHandle_t sem = xSemaphoreCreateCounting(2, 2);
#else
    static SemaphoreHandle_t sem = xSemaphoreCreateCounting(1, 1);
#endif
    return sem;
}

struct LoginJob {
    WebHandlerContext* ctx;
    AsyncWebServerRequestPtr held;  // 暂停请求的弱引用，客户端断连即失效
    SemaphoreHandle_t slot;
    String username;
    String password;
    String ipAddress;
    String userAgent;
    bool apiClient = false;
};

void loginTask(void* arg) {
    LoginJob* job = static_cast<LoginJob*>(arg);

    // The task is created before the HTTP request is paused. Waiting for the
    // handler's notification closes the failure window where xTaskCreate
    // could fail after pause() and leave request ownership ambiguous.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    AuthResult result = job->ctx->authManager->login(
        job->username, job->password, job->ipAddress, job->userAgent);

    // 客户端可能已断开：weak_ptr 失效则静默丢弃结果
    if (auto request = job->held.lock()) {
        if (result.success) {
            JsonDocument responseDoc;
            responseDoc["success"] = true;
            if (job->apiClient) responseDoc["sessionId"] = result.sessionId;
            responseDoc["username"] = result.username;

            String jsonStr;
            serializeJson(responseDoc, jsonStr);
            AsyncWebServerResponse* response = request->beginResponse(200, "application/json", jsonStr);

            SecurityConfig securityConfig;
            auto* authManager = static_cast<AuthManager*>(job->ctx->authManager);
            if (authManager) securityConfig = authManager->getSecurityConfig();
            if (securityConfig.cookieName.isEmpty()) securityConfig.cookieName = "sessionId";
            String cookie = securityConfig.cookieName + "=" + result.sessionId +
                            "; Path=/; Max-Age=" + String(securityConfig.cookieMaxAge) +
                            "; SameSite=Strict";
            if (securityConfig.cookieHttpOnly) cookie += "; HttpOnly";
            if (securityConfig.cookieSecure) cookie += "; Secure";
            response->addHeader("Set-Cookie", cookie);
            response->addHeader("Connection", "close");
            request->send(response);
        } else {
            job->ctx->sendError(request.get(), 401, result.errorMessage);
        }
    }

    xSemaphoreGive(job->slot);
    delete job;

    // ESP-IDF reports the high-water mark in bytes (unlike vanilla FreeRTOS).
    // Emit it before deleting this transient task so the 6KB single-core size
    // can be validated on hardware without relying on a periodic sampler.
    const UBaseType_t freeStackBytes = uxTaskGetStackHighWaterMark(nullptr);
    const uint32_t usedStackBytes = freeStackBytes < LOGIN_TASK_STACK_BYTES
        ? LOGIN_TASK_STACK_BYTES - static_cast<uint32_t>(freeStackBytes)
        : 0U;
    Serial.printf("[LOGIN_METRICS] stack=%lu free=%lu used=%lu bytes\n",
                  (unsigned long)LOGIN_TASK_STACK_BYTES,
                  (unsigned long)freeStackBytes,
                  (unsigned long)usedStackBytes);
    vTaskDelete(nullptr);
}

}  // namespace

AuthRouteHandler::AuthRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
}

void AuthRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/api/auth/login", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleLogin(request);
    });

    server->on("/api/auth/logout", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleLogout(request);
    });

    server->on("/api/auth/session", HTTP_GET,
              [this](AsyncWebServerRequest* request) {
        handleVerifySession(request);
    });

    server->on("/api/auth/change-password", HTTP_POST,
              [this](AsyncWebServerRequest* request) {
        handleChangePassword(request);
    });
}

void AuthRouteHandler::handleLogin(AsyncWebServerRequest* request) {
    if (request->method() != HTTP_POST) {
        ctx->sendError(request, 405, "Method not allowed");
        return;
    }

    String username = ctx->getParamValue(request, "username", "");
    String password = ctx->getParamValue(request, "password", "");

    LOG_INFOF("[LOGIN] username='%s' password_len=%d params=%d",
              username.c_str(), password.length(), request->params());

    if (username.isEmpty() || password.isEmpty()) {
        ctx->sendBadRequest(request, "Username and password are required");
        return;
    }

    String ipAddress = ctx->getClientIP(request);
    String userAgent = ctx->getUserAgent(request);

    if (!ctx->authManager) {
        ctx->sendError(request, 500, "Authentication service unavailable");
        return;
    }

    SemaphoreHandle_t slot = loginSlotMutex();
    if (!slot) {
        HandlerUtils::sendJsonError(request, 503,
                                    "Authentication scheduler unavailable",
                                    LOGIN_TASK_RETRY_AFTER_SECONDS);
        return;
    }

    // 登录槽满：直接 503，避免 PBKDF2 工作任务堆叠。
    if (xSemaphoreTake(slot, 0) != pdTRUE) {
        HandlerUtils::sendJsonError(request, 503,
                                    "Login already in progress, please retry",
                                    LOGIN_TASK_RETRY_AFTER_SECONDS);
        return;
    }

    LoginJob* job = new (std::nothrow) LoginJob();
    if (!job) {
        xSemaphoreGive(slot);
        HandlerUtils::sendJsonError(request, 503,
                                    "Insufficient memory for login request",
                                    LOGIN_TASK_RETRY_AFTER_SECONDS);
        return;
    }
    job->ctx = ctx;
    job->slot = slot;
    job->username = username;
    job->password = password;
    job->ipAddress = ipAddress;
    job->userAgent = userAgent;
    job->apiClient = request->hasHeader("X-FastBee-Api-Client");

    // xTaskCreate allocates the stack as one internal block. Check the same
    // capability pool immediately before pausing the request so a fragmented
    // target gets an actionable 503 instead of an abandoned paused request.
    const uint32_t freeDram = FastBee::MemoryBudget::dram8Free();
    const uint32_t largestBlock = FastBee::MemoryBudget::dram8Largest();
    const uint32_t requiredBlock =
        LOGIN_TASK_STACK_BYTES + LOGIN_TASK_STACK_ALLOCATOR_OVERHEAD_BYTES;
    const uint32_t requiredFree =
        LOGIN_TASK_MIN_REMAINING_DRAM_BYTES + requiredBlock;
    if (largestBlock < requiredBlock || freeDram < requiredFree) {
        char message[176];
        snprintf(message, sizeof(message),
                 "Login needs largest>=%lu and free>=%lu DRAM bytes; available largest=%lu free=%lu",
                 (unsigned long)requiredBlock, (unsigned long)requiredFree,
                 (unsigned long)largestBlock, (unsigned long)freeDram);
        xSemaphoreGive(slot);
        delete job;
        HandlerUtils::sendJsonError(request, 503, message,
                                    LOGIN_TASK_RETRY_AFTER_SECONDS);
        return;
    }

    // 双核芯片钉到 APP 核（核 1），把核 0 留给 WiFi/lwIP；
    // 单核 C3/C6 普通创建：PBKDF2 任务优先级低于 lwIP tcpip 任务，
    // 网络栈仍可及时响应（此前同步执行时正是 lwIP 线程自身被占住）
    BaseType_t ok;
    TaskHandle_t taskHandle = nullptr;
#if CHIP_DUAL_CORE
    ok = xTaskCreatePinnedToCore(loginTask, "fb_login", LOGIN_TASK_STACK_BYTES,
                                 job, 1, &taskHandle, 1);
#else
    ok = xTaskCreate(loginTask, "fb_login", LOGIN_TASK_STACK_BYTES,
                     job, 1, &taskHandle);
#endif
    if (ok != pdPASS) {
        const uint32_t freeAfter = FastBee::MemoryBudget::dram8Free();
        const uint32_t largestAfter = FastBee::MemoryBudget::dram8Largest();
        char message[144];
        snprintf(message, sizeof(message),
                 "Login task allocation failed (free=%lu, largest=%lu); please retry",
                 (unsigned long)freeAfter, (unsigned long)largestAfter);
        xSemaphoreGive(slot);
        delete job;
        HandlerUtils::sendJsonError(request, 503, message,
                                    LOGIN_TASK_RETRY_AFTER_SECONDS);
        return;
    }

    job->held = request->pause();  // handler 返回后不触发 501 兜底
    xTaskNotifyGive(taskHandle);
    // 回包由 loginTask 在验证完成后发出。
}

void AuthRouteHandler::handleLogout(AsyncWebServerRequest* request) {
    // 登出只需要有效会话即可
    AuthResult authResult = ctx->authenticateRequest(request);
    if (!authResult.success) {
        ctx->sendUnauthorized(request);
        return;
    }

    String sessionId = AuthManager::extractSessionIdFromRequest(request);

    if (sessionId.isEmpty()) {
        ctx->sendError(request, 400, "No session found");
        return;
    }

    if (ctx->authManager && ctx->authManager->logout(sessionId)) {
        JsonDocument doc;
        doc["success"] = true;
        doc["message"] = "Logged out successfully";
        String body;
        serializeJson(doc, body);
        auto* response = request->beginResponse(200, "application/json", body);
        auto* authManager = static_cast<AuthManager*>(ctx->authManager);
        SecurityConfig securityConfig = authManager ? authManager->getSecurityConfig() : SecurityConfig();
        if (securityConfig.cookieName.isEmpty()) securityConfig.cookieName = "sessionId";
        String expiredCookie = securityConfig.cookieName + "=; Path=/; Max-Age=0; SameSite=Strict";
        if (securityConfig.cookieHttpOnly) expiredCookie += "; HttpOnly";
        if (securityConfig.cookieSecure) expiredCookie += "; Secure";
        response->addHeader("Set-Cookie", expiredCookie);
        response->addHeader("Connection", "close");
        request->send(response);
    } else {
        ctx->sendError(request, 400, "Logout failed");
    }
}

void AuthRouteHandler::handleVerifySession(AsyncWebServerRequest* request) {
    AuthResult authResult = ctx->authenticateRequest(request);

    if (authResult.success) {
        JsonDocument responseDoc;
        responseDoc["username"] = authResult.username;
        responseDoc["sessionValid"] = true;
        responseDoc["timestamp"] = millis();

        ctx->sendSuccess(request, responseDoc);
    } else {
        // This endpoint is a browser bootstrap probe, not a protected
        // resource. An absent/expired session is normal state and should not
        // create a console-level 401 resource error. Protected APIs retain
        // their normal unauthorized response semantics.
        JsonDocument responseDoc;
        responseDoc["sessionValid"] = false;
        responseDoc["timestamp"] = millis();
        ctx->sendSuccess(request, responseDoc);
    }
}

void AuthRouteHandler::handleChangePassword(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;

    String oldPassword = ctx->getParamValue(request, "oldPassword", "");
    String newPassword = ctx->getParamValue(request, "newPassword", "");

    if (oldPassword.isEmpty() || newPassword.isEmpty()) {
        ctx->sendBadRequest(request, "Old password and new password are required");
        return;
    }

    AuthResult authResult = ctx->authenticateRequest(request);
    if (!authResult.success) {
        ctx->sendUnauthorized(request);
        return;
    }

    if (!ctx->userManager) {
        ctx->sendError(request, 500, "User service unavailable");
        return;
    }

    if (ctx->userManager->changePassword(authResult.username, oldPassword, newPassword)) {
        if (ctx->authManager) {
            ctx->authManager->forceLogout(authResult.username);
        }
        ctx->sendSuccess(request, "Password changed successfully");
    } else {
        ctx->sendError(request, 400, "Password change failed");
    }
}
