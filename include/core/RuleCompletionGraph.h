#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "core/PeripheralExecution.h"
#include "core/ResourceProfile.h"

namespace FastBee {

// Scratch-only graph. Capacity follows the actual target profile rather than a
// hardcoded 32-bit node mask. No recursion, heap allocation or retained state.
class RuleCompletionGraph {
public:
    static constexpr size_t Capacity = ResourceProfile::MAX_PERIPH_EXEC_RULES;
    static constexpr size_t Words = (Capacity + 31) / 32;
    static_assert(Capacity > 0, "Completion graph requires a nonempty profile");

    explicit RuleCompletionGraph(size_t count = Capacity) : count_(count) {}
    bool setEnabled(size_t node, bool enabled) {
        if (node >= count_ || node >= Capacity) { invalid_ = true; return false; }
        enabled_[node] = enabled;
        return true;
    }
    bool addEdge(size_t source, size_t target) {
        if (source >= count_ || target >= count_ || source >= Capacity || target >= Capacity) {
            invalid_ = true; return false;
        }
        edges_[source][target / 32] |= uint32_t(1) << (target % 32);
        return true;
    }
    const char* error() const {
        if (invalid_ || count_ > Capacity) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
        size_t incoming[Capacity] = {};
        size_t active = 0;
        for (size_t source = 0; source < count_; ++source) {
            if (!enabled_[source]) continue;
            ++active;
            for (size_t target = 0; target < count_; ++target)
                if (enabled_[target] && edge(source, target)) ++incoming[target];
        }
        for (size_t removed = 0; removed < active; ++removed) {
            size_t source = 0;
            while (source < count_ && (!enabled_[source] || incoming[source] != 0)) ++source;
            if (source == count_) return "CONFIG_RULE_EVENT_CYCLE";
            incoming[source] = SIZE_MAX; // Already removed; cannot be selected twice.
            for (size_t target = 0; target < count_; ++target)
                if (enabled_[target] && incoming[target] != SIZE_MAX && edge(source, target)) --incoming[target];
        }
        return nullptr;
    }

private:
    bool edge(size_t source, size_t target) const {
        return (edges_[source][target / 32] & (uint32_t(1) << (target % 32))) != 0;
    }
    uint32_t edges_[Capacity][Words] = {};
    bool enabled_[Capacity] = {};
    size_t count_;
    bool invalid_ = false;
};

// Only action 21 emits a local rule event. New saves require an explicit target;
// legacy fixed fallback IDs remain analyzable. Received-value fallback with no
// target is dynamic and deliberately not guessed as a wildcard.
inline const char* fixedRuleActionEvent(uint8_t type, const char* target,
        const char* value, bool useReceivedValue, const char* ruleId) {
    if (type != static_cast<uint8_t>(ExecActionType::ACTION_TRIGGER_EVENT)) return nullptr;
    if (target && *target) return target;
    if (useReceivedValue) return nullptr;
    return value && *value ? value : ruleId;
}

inline bool hasDynamicRuleEventTarget(uint8_t type, const char* target, bool useReceivedValue) {
    return type == static_cast<uint8_t>(ExecActionType::ACTION_TRIGGER_EVENT) &&
        (!target || !*target) && useReceivedValue;
}

// Caller holds the rule mutex. Candidate replaces only the explicitly supplied
// ID (update IDs are canonicalized by Manager), or appends on ADD. Enabling a
// saved draft uses enableId and does not need to copy its strings/actions.
template<class RuleMap>
const char* validateRuleCompletionGraph(const RuleMap& rules,
        const PeriphExecRule* candidate = nullptr, const String* candidateId = nullptr,
        const String* enableId = nullptr) {
    constexpr size_t capacity = RuleCompletionGraph::Capacity;
    const PeriphExecRule* nodes[capacity] = {};
    const String* ids[capacity] = {};
    size_t count = 0;
    if (rules.size() > capacity) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
    const String* replacement = candidate ? (candidateId ? candidateId : &candidate->id) : nullptr;
    for (const auto& entry : rules) {
        if (replacement && entry.first == *replacement) continue;
        if (count == capacity) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
        ids[count] = &entry.first; nodes[count++] = &entry.second;
    }
    if (candidate) {
        if (count == capacity) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
        ids[count] = replacement; nodes[count++] = candidate;
    }
    RuleCompletionGraph graph(count);
    for (size_t target = 0; target < count; ++target) {
        if (nodes[target]->triggers.size() > MAX_TRIGGERS_PER_RULE ||
            nodes[target]->actions.size() > MAX_ACTIONS_PER_RULE) return "CONFIG_RULE_EVENT_GRAPH_LIMIT";
        const bool enabled = nodes[target]->enabled || (enableId && *ids[target] == *enableId);
        graph.setEnabled(target, enabled);
        if (enabled) for (const auto& action : nodes[target]->actions)
            if (hasDynamicRuleEventTarget(action.actionType, action.targetPeriphId.c_str(), action.useReceivedValue))
                return "CONFIG_RULE_EVENT_DYNAMIC_TARGET";
        for (const auto& trigger : nodes[target]->triggers) {
            if (trigger.triggerType != static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER) || trigger.eventId.isEmpty()) continue;
            for (size_t source = 0; source < count; ++source) {
                if (trigger.eventId == *ids[source]) graph.addEdge(source, target);
                // Potential exact-ID fanout: do not query PeripheralManager
                // under the rules mutex. Even an unknown ID may later become a
                // DEVICE_EVENT; payload filters are not proof that an edge can
                // never fire after its input/configuration changes.
                for (const auto& action : nodes[source]->actions) {
                    const char* emitted = fixedRuleActionEvent(action.actionType,
                        action.targetPeriphId.c_str(), action.actionValue.c_str(),
                        action.useReceivedValue, ids[source]->c_str());
                    if (emitted && trigger.eventId == emitted) graph.addEdge(source, target);
                }
            }
        }
    }
    return graph.error();
}

} // namespace FastBee
