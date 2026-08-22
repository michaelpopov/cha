#pragma once

#include "chat/character.h"
#include "chat/session_identity.h"
#include "chat/transcript.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

inline constexpr std::string_view shared_history_heading =
    "Shared chat history (JSONL):";

using SharedModelHistory = std::shared_ptr<const ModelHistory>;

// One logical model run. Durable transcript entry IDs are assigned only when
// the controller activates the run.
struct RunSpec {
    SessionIdentity session;
    RequestId request_id{};
    CharacterMetadata target;
    EntryIdentity author;
    std::string prompt_text;
    // Provider-side prompt-cache affinity for this forum/session/character
    // projection. An empty value leaves caching disabled for the request.
    std::string prompt_cache_key;
    // The prompt's UTC Unix-seconds timestamp. It is copied into the durable
    // transcript entry when this run becomes active.
    std::int64_t created_at{};
};

// Complete immutable input for backend preparation. Ordinary requests and
// multicast children use the same representation.
struct GenerationRequest {
    SharedModelHistory history;
    RunSpec run;
};

enum class ModelRole {
    system,
    persona,
    assistant,
};

struct ModelMessage {
    ModelRole role{};
    std::string content;

    bool operator==(const ModelMessage&) const = default;
};

std::vector<ModelMessage> project_model_context(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    OffrecordSpan offrecord_span,
    std::string_view system_prompt,
    std::string_view character_id);

std::vector<ModelMessage> project_model_context(
    const GenerationRequest& input,
    std::string_view system_prompt);

} // namespace cha
