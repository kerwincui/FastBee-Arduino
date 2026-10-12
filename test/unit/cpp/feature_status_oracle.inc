const OracleFeatureManager::FeatureState* OracleFeatureManager::findFeature(const char* key) const {
    if (!key) return nullptr;
    for (size_t i = 0; i < count_; ++i) {
        if (strcmp(features_[i].meta.key, key) == 0) return &features_[i];
    }
    return nullptr;
}

bool OracleFeatureManager::shouldEnable(const char* key) const {
    // 安全回退: 若尚未从文件加载（如早期调用），先在内存中注册默认状态，
    // 避免 count_==0 导致所有功能被误禁用
    if (!loaded_) {
        auto* self = const_cast<OracleFeatureManager*>(this);
        self->generateDefaults();
        self->syncBudgetState();
        self->applyDependencyRules();
        self->loaded_ = true;
    }
    const auto* f = findFeature(key);
    if (!f) return false;
    return strcmp(admissionState(*f), "admitted") == 0;
}

const char* OracleFeatureManager::admissionState(const FeatureState& feature) const {
    const FeatureState* current = &feature;
    bool ownState = true;
    // At most one visit per registered feature. No recursion, allocation, or
    // fail-open result for a missing/cyclic dependency.
    for (size_t depth = 0; depth < count_; ++depth) {
        if (!current->meta.compiled) return ownState ? "not_compiled" : "dependency_blocked";
        const bool effectiveEnabled = strcmp(current->meta.key, "ioExpander") == 0
            ? ioExpanderBootEnabled_ : strcmp(current->meta.key, "i2cSensors") == 0
                ? i2cSensorsBootEnabled_ : current->userEnabled;
        if (!effectiveEnabled) {
            if (!ownState) return "dependency_blocked";
            return current->userEnabled ? "restart_required" : "user_disabled";
        }
        if (!current->budgetPassed) return ownState ? "budget_denied" : "dependency_blocked";
        if (current->runtimeDisabled) return ownState ? "runtime_disabled" : "dependency_blocked";
        if (current->meta.requiresNetwork && !networkReady_) {
            return ownState ? "waiting_for_network" : "dependency_blocked";
        }
        if (!current->meta.dependsOn) return "admitted";
        current = findFeature(current->meta.dependsOn);
        if (!current) return "dependency_missing";
        ownState = false;
    }
    return "dependency_cycle";
}

bool OracleFeatureManager::getFeatureStatusJson(size_t index, JsonObject obj) const {
    if (index >= count_ || obj.isNull()) return false;

    const auto& st = features_[index];
    obj["key"] = st.meta.key;
    obj["name"] = st.meta.displayName;
    obj["group"] = st.meta.group;
    obj["compiled"] = st.meta.compiled;
    obj["userEnabled"] = st.userEnabled;
    obj["budgetPassed"] = st.budgetPassed;
    if (strcmp(st.meta.key, "ioExpander") == 0 || strcmp(st.meta.key, "i2cSensors") == 0) {
        const bool bootEnabled = strcmp(st.meta.key, "ioExpander") == 0
            ? ioExpanderBootEnabled_ : i2cSensorsBootEnabled_;
        obj["requiresRestart"] = true;
        obj["bootEnabled"] = bootEnabled;
        obj["pendingRestart"] = st.userEnabled != bootEnabled;
        obj["onDemand"] = true;
    }
    // Report the same effective state that the initialization gates use.
    // A feature can be compiled and user-enabled while still being held
    // back by the startup budget, a dependency, network deferral, or a
    // runtime memory-recovery override. Counting it as active in that
    // situation makes the Web resource summary overstate live usage.
    const bool active = shouldEnable(st.meta.key);
    obj["active"] = active;
    // Keep the legacy field for menu/API compatibility, but make its scope
    // explicit. No driver observation means unknown, not initialized or idle.
    obj["activeMeaning"] = "admission_only";
    obj["admissionState"] = admissionState(st);
    auto observed = FeatureInstanceRegistry::instance().observe(st.meta.key);
    if (observed.state == FeatureInstanceState::Unobserved && observed.reason == FeatureInstanceReason::ObserverMissing)
        observed.scope = missingFeatureInstanceScope(st.meta.key);
    obj["instanceState"] = observed.name();
    obj["instanceObserved"] = observed.observed();
    obj["instanceScope"] = observed.scopeName();
    obj["instanceReason"] = observed.state == FeatureInstanceState::Unobserved &&
            observed.reason == FeatureInstanceReason::ObserverMissing
        ? missingFeatureInstanceReason(st.meta.key, st.meta.compiled) : observed.reasonName();
    if (strcmp(st.meta.key, "otaFs") == 0) obj["instanceReason"] = "implementation_missing";
    obj.remove("initializedInstances"); // Objects may be reused for a different feature.
    if (observed.countKnown()) obj["initializedInstances"] = observed.initializedCount;
    obj["runtimeDisabled"] = st.runtimeDisabled;
    if (st.runtimeDisabled && st.runtimeReason[0] != '\0') {
        obj["runtimeDisableReason"] = st.runtimeReason;
    }
    uint32_t displayFlashKB = st.meta.flashKB;
    uint32_t displayRamB = st.meta.ramB;
    if (!st.meta.displayName && st.meta.dependsOn) {
        // 附属项仍保留在状态 API 中供诊断，但资源归入可见父项，避免前端
        // 在组合功能行中重复统计。
        displayFlashKB = 0;
        displayRamB = 0;
    } else if (st.meta.displayName) {
        for (size_t i = 0; i < count_; ++i) {
            const auto& child = features_[i];
            if (!child.meta.displayName && child.meta.dependsOn &&
                strcmp(child.meta.dependsOn, st.meta.key) == 0 && child.meta.compiled) {
                displayFlashKB += child.meta.flashKB;
                displayRamB += child.meta.ramB;
            }
        }
    }
    obj["flashKB"] = displayFlashKB;
    obj["ramB"] = displayRamB;
    if (st.meta.hwDep) obj["hwDep"] = st.meta.hwDep;
    if (st.meta.dependsOn) obj["dependsOn"] = st.meta.dependsOn;
    bool bundleRequired = false;
    for (const auto& bundle : kFeatureBundles)
        if (strcmp(st.meta.key, bundle.member) == 0) bundleRequired = true;
    obj["bundleRequired"] = bundleRequired;
    // 延迟初始化标记：true 表示该功能在网络就绪后才激活（错峰启动）
    if (st.meta.requiresNetwork) obj["deferred"] = true;
    if (!st.meta.compiled) {
        obj["locked"] = true;
        // OTA 在 4MB 单 app 分区中是硬件/分区能力限制，不是统一代码
        // 缺失；将原因透传给页面，避免误导用户重新编译无效固件。
        if (st.meta.key && strcmp(st.meta.key, "ota") == 0) {
            obj["lockReason"] = "当前分区不支持 OTA（需要带 ota_1 的 A/B 双分区）";
        } else if (strcmp(st.meta.key, "otaFs") == 0) {
            obj["lockReason"] = "未实现文件系统 OTA 写入";
        } else {
            obj["lockReason"] = "Not compiled into firmware";
        }
    }
    return true;
}

void OracleFeatureManager::getStatusSummaryJson(JsonObject summary) const {
    if (summary.isNull()) return;

    uint32_t totalFlashKB = 0;
    uint32_t activeFlashKB = 0;
    uint32_t activeRamB = 0;
    for (size_t i = 0; i < count_; ++i) {
        const auto& st = features_[i];
        if (st.meta.compiled) totalFlashKB += st.meta.flashKB;
        const bool active = shouldEnable(st.meta.key);
        if (active) {
            activeFlashKB += st.meta.flashKB;
            activeRamB += st.meta.ramB;
        }
    }

    summary["totalFlashKB"] = totalFlashKB;
    summary["activeFlashKB"] = activeFlashKB;
    summary["activeRamB"] = activeRamB;
    summary["resourceAccounting"] = "static_estimate";
    summary["instanceMemoryMeasured"] = false;
    summary["runtimeDegraded"] = runtimeDegraded_;
#if defined(ESP_PLATFORM)
    summary["free8KB"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
    summary["largest8KB"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
#endif
}
