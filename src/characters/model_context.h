#pragma once

#include "chat/character_metadata.h"
#include "chat/session_identity.h"
#include "chat/transcript.h"

#include <atomic>
#include <functional>
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
using WebReadTool = std::function<std::string(std::string_view, const std::atomic_bool&,
    const std::function<void()>&)>;

// Immutable Library credentials and scope for one ordinary generation.
struct LibrarySearchScope {
    std::string service;
    std::vector<std::string> store_ids;
    std::string key;
    int timeout_s{};
};

using LibrarySearchTool = std::function<std::string(
    std::string_view, const std::atomic_bool&)>;

using SummaryTool = std::function<std::string(
    std::string_view, std::string_view, const std::atomic_bool&)>;

// One logical model run. Durable transcript entry IDs are assigned only when
// the controller activates the run.
struct RunSpec {
    FullSessionId session;
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
    std::function<std::string(std::string_view, const std::atomic_bool&)> web_search_tool;
    WebReadTool web_read_tool;
    SummaryTool summary_tool;
    std::string summary_context;
    // Bound maintenance tool. Empty for ordinary conversations.
    std::function<std::string(
        std::string_view, std::string_view, const std::atomic_bool&)> maintenance_tool;
    // Auxiliary requests such as session naming do not need conversation tool rules.
    bool include_tool_instructions{true};
    // Native File Search store IDs. Empty for application retrieval.
    std::vector<std::string> library_store_ids;
    std::optional<LibrarySearchScope> library_search;
    LibrarySearchTool library_search_tool;
};

enum class ModelRole {
    system,
    user,
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
    std::optional<EntryId> covered_until,
    std::string_view system_prompt,
    std::string_view character_id);

std::vector<ModelMessage> project_model_context(
    const GenerationRequest& input,
    std::string_view system_prompt);

} // namespace cha
