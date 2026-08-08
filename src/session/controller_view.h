#pragma once

#include "agents/agent.h"
#include "session/generation_status.h"
#include "transcript/transcript.h"

#include <optional>
#include <span>
#include <string_view>

namespace cha {

// The borrowed generation facts a full projection needs. Reasoning text is
// ephemeral presentation state owned by the controller's active response.
struct ControllerGenerationView {
    bool active{};
    std::optional<RequestId> request_id;
    std::string_view agent_id;
    std::string_view agent_name;
    ResponsePhase phase{ResponsePhase::waiting};
    std::string_view reasoning_text;
};

// A borrowed read model of one live controller, sufficient to build a full
// frontend snapshot.
//
// The view may be used only on the controller owner thread and is invalidated
// by the next controller mutation. It must be consumed synchronously: it must
// never be stored in a runtime field, moved into a mailbox, or captured by
// work that can outlive the call that produced it.
struct ControllerView {
    std::span<const CharacterInfo> characters;
    std::string_view default_agent_id;
    TranscriptView transcript;
    ControllerGenerationView generation;
};

} // namespace cha
