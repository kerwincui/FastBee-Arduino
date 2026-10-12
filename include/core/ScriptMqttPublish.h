#pragma once
#include <cstddef>
#include <cstdint>

namespace FastBee {
enum class ScriptPublishResult : uint8_t { Accepted, Unavailable, UnknownInstance, InstanceChanged, Rejected, Queued };
inline const char* scriptPublishResultName(ScriptPublishResult result) {
    switch (result) {
        case ScriptPublishResult::Accepted: return "accepted_locally";
        case ScriptPublishResult::Queued: return "queued_locally_delivery_unverified";
        case ScriptPublishResult::Unavailable: return "unavailable";
        case ScriptPublishResult::UnknownInstance: return "unknown_instance";
        case ScriptPublishResult::InstanceChanged: return "instance_changed";
        case ScriptPublishResult::Rejected: return "not_accepted";
    }
    return "unknown";
}

// The caller retains a SHORT current-client lease throughout this call. Never
// rebind a script to a new instance. Typed queue entries retain the configured
// topic row/retain semantics and transformed bytes, separately from telemetry.
template<class Client, class Message>
ScriptPublishResult publishBoundScriptTopic(Client* client, uint32_t origin,
                                           size_t topicIndex, const Message& message) {
    if (!origin) return ScriptPublishResult::UnknownInstance;
    if (!client) return ScriptPublishResult::Unavailable;
    const uint32_t current = client->getReportInstanceId();
    if (!current) return ScriptPublishResult::UnknownInstance;
    if (current != origin) return ScriptPublishResult::InstanceChanged;
    return client->queueTopicMessage(topicIndex, message) ?
        ScriptPublishResult::Queued : ScriptPublishResult::Rejected;
}
} // namespace FastBee
