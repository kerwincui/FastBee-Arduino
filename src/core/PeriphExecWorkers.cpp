/** @file PeriphExecWorkers.cpp @brief Worker admission and lifecycle. */
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#include "core/PeriphExecWorkerPool.h"
#include "core/MemoryBudget.h"
#include "core/FastBeeFramework.h"
#include "core/ChipConfig.h"
#include "core/NativeOutputDispatch.h"
#include "systems/LoggerSystem.h"
#include <new>
#if FASTBEE_WORKER_BUDGETED
#include "network/NetworkManager.h"
#include "core/RuntimeFeatureManager.h"
#endif
#if defined(ESP_PLATFORM)
#include <WiFi.h>
#endif

namespace {
#if FASTBEE_WORKER_BUDGETED
static FastBee::MqttClientLifetime::Read acquireWorkerNetworkBaseline() {
    auto* fw = FastBeeFramework::getInstance();
    if (!fw || !fw->getNetworkManager() ||
        !FastBee::RuntimeFeatureManager::instance().networkReady()) return {};
    // Framework owns this concrete implementation, exposed through its interface.
    auto* network = static_cast<FBNetworkManager*>(fw->getNetworkManager());
    auto lease = network->acquireWorkerBudgetBaselineRead();
    if (!lease) return {};
    const bool ready = network->getNetworkType() == NetworkType::NET_WIFI
        ? WiFi.getMode() != WIFI_OFF : network->getActiveAdapter() != nullptr;
    return ready ? std::move(lease) : FastBee::MqttClientLifetime::Read{};
}
#endif

static bool hasWorkerPoolMemoryHeadroom(bool logDeferred, bool baselineReady = false) {
#if defined(ESP_PLATFORM)
#if FASTBEE_WORKER_BUDGETED
    const bool admitted = FastBee::WorkerPoolBudget::canAdd(
        FastBee::MemoryBudget::dram8Free(), FastBee::MemoryBudget::dram8Largest(),
        PeriphExecWorkerPool::WORKER_STACK, baselineReady, 0, PeriphExecWorkerPool::WORKER_COUNT, false);
    if (!admitted && logDeferred) LOGGER.warning("[PeriphExec] Worker deferred: online Web/diagnostic DRAM budget");
    return admitted;
#else

    (void)baselineReady;
    // Before the WiFi driver finishes starting, boot heap is not the online
    // budget. COM16 measured 61KB consumed after the pool was admitted.
    // Defer that optional allocation until its network baseline exists.
    if (CHIP_IS_CLASSIC_ESP32 && FASTBEE_USE_PSRAM == 0 &&
        !FASTBEE_ENABLE_ETHERNET && !FASTBEE_ENABLE_CELLULAR &&
        FastBeeFramework::getInstance() &&
        FastBeeFramework::getInstance()->getNetworkManager() &&
        WiFi.status() != WL_CONNECTED &&
        !((WiFi.getMode() & WIFI_AP) && WiFi.softAPIP() != IPAddress(0, 0, 0, 0))) {
        if (logDeferred) LOGGER.warning("[PeriphExec] Worker pool admission deferred until WiFi is ready");
        return false;
    }
    const uint32_t workerReserve =
        static_cast<uint32_t>(PeriphExecWorkerPool::WORKER_STACK *
                              PeriphExecWorkerPool::WORKER_COUNT + 4096U);
    const uint32_t webReserveAfterWorker =
        (CHIP_IS_CLASSIC_ESP32 && FASTBEE_USE_PSRAM == 0)
            ? 25600U // Release minimum; guard recovery alone cannot sustain Web access.
            : 0U;
    const uint32_t requiredFree8 = workerReserve + webReserveAfterWorker;
    const uint32_t free8 = FastBee::MemoryBudget::dram8Free();
    const uint32_t largest8 = FastBee::MemoryBudget::dram8Largest();
    const bool admitted =
        free8 >= requiredFree8 && largest8 >= PeriphExecWorkerPool::WORKER_STACK;

    if (!admitted && logDeferred) {
        LOGGER.warningf(
            "[PeriphExec] Worker pool memory admission deferred: free8=%lu largest8=%lu needFree8=%lu needLargest=%lu",
            (unsigned long)free8,
            (unsigned long)largest8,
            (unsigned long)requiredFree8,
            (unsigned long)PeriphExecWorkerPool::WORKER_STACK);
    }
    return admitted;
#endif
#else
    (void)logDeferred;
    (void)baselineReady;
    return true;
#endif
}
} // namespace

void PeriphExecManager::initializeWorkerPool() {
    // 启动常驻 Worker 任务池（D 方案）。在内存已经紧张的设备上，
    // 先保留 Web/WiFi 的连续 DRAM；轻量规则仍可同步执行，重规则会被
    // dispatchAsync 的内存门控跳过，避免启动期一次性分配栈导致 OOM。
    // On the no-PSRAM classic F4R0, a resident worker stack competes directly
    // with AsyncTCP/Web/MQTT. Rules remain configured when admission is
    // deferred; dispatchAsync uses its existing safe degradation path.
    {
        MutexGuard lock(_rulesMutex);
        const bool workerDemand = !lock.isLocked() || hasWorkerPoolDemandLocked();
        if (workerDemand) {
#if FASTBEE_WORKER_BUDGETED
            auto networkBaseline = acquireWorkerNetworkBaseline();
#endif
            const bool workerAdmission = hasWorkerPoolMemoryHeadroom(true
#if FASTBEE_WORKER_BUDGETED
            , static_cast<bool>(networkBaseline)
#endif
            );
            if (workerAdmission) _workerPool.reset(new (std::nothrow) PeriphExecWorkerPool());
            if (!workerAdmission || !_workerPool || !_workerPool->begin(FASTBEE_WORKER_BUDGETED ? 1U : PeriphExecWorkerPool::WORKER_COUNT)) {
                LOGGER.warning("[PeriphExec] WorkerPool unavailable at startup; async dispatch will degrade safely");
                const bool retired = !_workerPool || _workerPool->shutdown();
                if (retired) _workerPool.reset();
                _workersSuspendedForMemory = retired;
                _workerSuspensionPending = !retired;
            }
        } else {
            LOGGER.info("[PeriphExec] Worker pool dormant; no configured worker demand");
        }
    }
}

bool PeriphExecManager::ruleRequiresWorkerPool(const PeriphExecRule& rule) const {
    if (shouldAvoidSyncFallback(rule)) return true;
    if (!CHIP_IS_CLASSIC_ESP32 || FASTBEE_USE_PSRAM != 0) return false;
    // Only typed native GPIO commands qualify. Their ordinary-output service
    // owns arbitration and any ticker budget; no driver call or explicit wait
    // is introduced into the synchronous scheduler path.
    if (rule.actions.size() > MAX_ACTIONS_PER_RULE) return true;
    auto& pm = PeripheralManager::getInstance();
    for (const auto& action : rule.actions) {
        if (action.syncDelayMs != 0) return true;
        PeripheralConfig target;
        if (!pm.getPeripheralSnapshot(action.targetPeriphId, target) ||
            target.isExpanderChannel() ||
            (!FastBee::isOrdinaryDigitalTarget(target) &&
             !FastBee::isOrdinaryScalarTarget(target))) return true;
        switch (static_cast<ExecActionType>(action.actionType)) {
            case ExecActionType::ACTION_HIGH:
            case ExecActionType::ACTION_LOW:
            case ExecActionType::ACTION_HIGH_INVERTED:
            case ExecActionType::ACTION_LOW_INVERTED:
            case ExecActionType::ACTION_GPIO_TOGGLE:
            case ExecActionType::ACTION_SET_PWM:
            case ExecActionType::ACTION_SET_DAC:
            case ExecActionType::ACTION_CALL_PERIPHERAL:
                break;
            default:
                return true;
        }
    }
    return false;
}

bool PeriphExecManager::hasWorkerPoolDemandLocked() const {
    if (!CHIP_IS_CLASSIC_ESP32 || FASTBEE_USE_PSRAM != 0 || _workerPoolDemandPending) return true;
    for (const auto& entry : rules) {
        if (entry.second.enabled && ruleRequiresWorkerPool(entry.second)) return true;
    }
    return false;
}

bool PeriphExecManager::ensureWorkerPoolForRule(const PeriphExecRule& rule) {
    if (!FASTBEE_WORKER_BUDGETED &&
        (!CHIP_IS_CLASSIC_ESP32 || FASTBEE_USE_PSRAM != 0 || !ruleRequiresWorkerPool(rule))) return true;
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;
    if (!_workersSuspendedForMemory && !_workerSuspensionPending &&
        _workerPool && _workerPool->isStarted()) {
#if FASTBEE_WORKER_BUDGETED
        // A queued job proves concurrent demand; rule count alone does not.
        // A declined second worker keeps the bounded queue and first worker.
        auto networkBaseline = acquireWorkerNetworkBaseline();
        _workerPool->tryGrow(_workerPool->pendingCount() > 0, static_cast<bool>(networkBaseline));
#endif
        return true;
    }
    _workerPoolDemandPending = true;
    // HealthMonitor owns restoration once a real memory suspension is pending.
    // Never delete an active pool from an incoming request.
    if (_workersSuspendedForMemory || _workerSuspensionPending) return false;
#if FASTBEE_WORKER_BUDGETED
    // Keep the active network baseline alive through stack/task allocation.
    auto networkBaseline = acquireWorkerNetworkBaseline();
#endif
    if (!hasWorkerPoolMemoryHeadroom(true
#if FASTBEE_WORKER_BUDGETED
            , static_cast<bool>(networkBaseline)
#endif
            )) {
        _workersSuspendedForMemory = true;
        return false;
    }
    std::unique_ptr<PeriphExecWorkerPool> candidate(new (std::nothrow) PeriphExecWorkerPool());
    if (!candidate || !candidate->begin(FASTBEE_WORKER_BUDGETED ? 1U : PeriphExecWorkerPool::WORKER_COUNT)) {
        if (candidate && !candidate->shutdown()) {
            _workerPool = std::move(candidate);
            _workerSuspensionPending = true;
        }
        _workersSuspendedForMemory = true;
        return false;
    }
    _workerPool = std::move(candidate);
    _workerPoolDemandPending = false;
    return true;
}

bool PeriphExecManager::suspendWorkersForMemory() {
    // Gate new dispatches before asking workers to drain. A timed-out
    // shutdown is only "pending"; the fully-suspended flag is set after every
    // worker acknowledges exit so health reporting and retry logic remain
    // truthful.
    {
        MutexGuard lock(_rulesMutex);
        if (!lock.isLocked()) return false;
        if (_workersSuspendedForMemory) return true;
        _workerSuspensionPending = true;
    }
    if (_workerPool) {
        // Do not destroy the pool until every worker has acknowledged exit.
        // A long-running rule may outlive the first wait window; retaining the
        // object lets the next health sample retry safely.
        if (!_workerPool->shutdown()) {
            LOGGER.warning("[PeriphExec] Worker pool shutdown deferred; worker still running");
            return false;
        }
        _workerPool.reset();
    }
    _workerSuspensionPending = false;
    _workersSuspendedForMemory = true;
    LOGGER.warning("[PeriphExec] Worker pool suspended for memory pressure");
    return true;
}

bool PeriphExecManager::resumeWorkersAfterMemory() {
    if (!_workersSuspendedForMemory && !_workerSuspensionPending) return true;

    // A prior suspend can time out while a long-running rule is finishing.
    // Do not allocate a replacement pool until the old workers are confirmed
    // exited; shutdown() is idempotent and safely retries the wait.
    if (_workerPool) {
        if (!_workerPool->shutdown()) {
            LOGGER.warning("[PeriphExec] Worker pool restore deferred (old worker still active)");
            return false;
        }
        _workerPool.reset();
    }

    // A pool that finished its deferred shutdown is now fully suspended. Keep
    // the state explicit before attempting allocation of the replacement.
    _workersSuspendedForMemory = true;

    // shutdown must stay outside _rulesMutex: retiring jobs query the rule
    // revision. Creation and demand inspection share the existing rule lock.
    MutexGuard lock(_rulesMutex);
    if (!lock.isLocked()) return false;
    if (!hasWorkerPoolDemandLocked()) {
        _workersSuspendedForMemory = false;
        _workerSuspensionPending = false;
        LOGGER.info("[PeriphExec] Worker pool dormant; configured GPIO rules use bounded sync execution");
        return true;
    }

    // Recovery must preserve the same Web/WiFi reserve as startup.  The
    // HealthMonitor's aggregate internal-heap level can be NORMAL while the
    // allocatable 8-bit pool is still below the worker stack requirement on a
    // classic no-PSRAM target.  Repeated xTaskCreate attempts in that state
    // fragment the remaining heap and keep WebMemoryGuard active.
#if FASTBEE_WORKER_BUDGETED
    // Keep the active network baseline alive through stack/task allocation.
    auto networkBaseline = acquireWorkerNetworkBaseline();
#endif
    if (!hasWorkerPoolMemoryHeadroom(true
#if FASTBEE_WORKER_BUDGETED
            , static_cast<bool>(networkBaseline)
#endif
            )) {
        return false;
    }

    std::unique_ptr<PeriphExecWorkerPool> candidate(new (std::nothrow) PeriphExecWorkerPool());
    if (!candidate || !candidate->begin(FASTBEE_WORKER_BUDGETED ? 1U : PeriphExecWorkerPool::WORKER_COUNT)) {
        if (candidate && !candidate->shutdown()) {
            _workerPool = std::move(candidate);
            _workerSuspensionPending = true;
        }
        LOGGER.warning("[PeriphExec] Worker pool restore deferred (insufficient memory)");
        return false;
    }
    _workerPool = std::move(candidate);
    _workerPoolDemandPending = false;
    _workersSuspendedForMemory = false;
    _workerSuspensionPending = false;
    LOGGER.info("[PeriphExec] Worker pool restored after memory recovery");
    return true;
}

void PeriphExecManager::cancelWorkerJob(AsyncExecContext* ctx) {
    if (!ctx) return;

    // The manager pointer is normally this object. If a malformed context was
    // queued, still return its task slot instead of leaking an async permit.
    PeriphExecManager* owner = ctx->manager ? ctx->manager : this;
    if (owner->_runningRulesMutex) {
        MutexGuard runLock(owner->_runningRulesMutex);
        owner->_runningRuleIds.erase(ctx->ruleCopy.id);
        owner->_runningStartTime.erase(ctx->ruleCopy.id);
    }

    AsyncExecResult result;
    result.operationId = ctx->operationId;
    result.peripheralConfigEpoch = ctx->peripheralConfigEpoch;
    result.uartOrigin = ctx->uartOrigin;
    result.ordinaryControl = ctx->ordinaryControl;
    result.ruleId = ctx->ruleCopy.id;
    result.ruleName = ctx->ruleCopy.name;
    result.status = AsyncExecStatus::CANCELLED;
    result.errorCode = AsyncExecError::CANCELLED_BY_SYSTEM;
    result.queuedAt = ctx->queuedAt;
    result.startTime = 0;
    result.endTime = millis();
    result.deadlineAt = ctx->deadlineAt;
    result.queuePosition = ctx->queuePosition;
    result.asyncExecution = true;
    result.actionCount = static_cast<uint8_t>(
        ctx->ruleCopy.actions.size() > 255 ? 255 : ctx->ruleCopy.actions.size());
    owner->recordResult(result);
    if (FastBee::MemoryBudget::dramFree() > 10000 && owner->isRuleExecutionCurrent(ctx->ruleCopy)) {
        owner->dispatchPeriphExecEvent(ctx->ruleCopy.id, "cancelled", ctx->uartOrigin);
    }
    if (ctx->taskSlot) xSemaphoreGive(ctx->taskSlot);
    owner->_contextPool.release(ctx);
}

#endif // FASTBEE_ENABLE_PERIPH_EXEC
