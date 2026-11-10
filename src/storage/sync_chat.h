#pragma once

#include "chat/session_identity.h"
#include "chat/transcript.h"
#include "storage/sqlite_storage.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha {

// Shared-chat rule: non-temporary label, next_request_id > 1, and no
// running turn with request ID 1. Clear and turn deletion never reset the
// counter, so eligibility is derived from stored data.
[[nodiscard]] bool sync_chat_is_shared(
    storage::SqliteDatabase& database, std::int64_t session_key);

struct SyncChatIdentity {
    std::string forum_id;
    std::string session_id;
    std::string label;
};

[[nodiscard]] std::optional<SyncChatIdentity> sync_chat_identity(
    storage::SqliteDatabase& database, std::int64_t session_key);

// Enqueues one chat record with chat/label filled in. Caller owns the
// transaction and has already checked eligibility and connectivity.
void sync_enqueue_chat(
    storage::SqliteDatabase& database,
    const SyncChatIdentity& identity,
    std::string_view kind,
    nlohmann::json fields);

// Emits the initial shared records for a chat that just became shared:
// finished turns and @- messages in entry order, then attachments, then
// character settings. Caller owns the transaction.
void sync_record_chat_initial(
    storage::SqliteDatabase& database,
    std::int64_t session_key,
    const SyncChatIdentity& identity);

// One finished turn (prompt, response, and state) or one @- entry.
[[nodiscard]] nlohmann::json sync_chat_entries_fields(
    const std::vector<TranscriptEntry>& entries,
    std::optional<int> turn_state);

[[nodiscard]] bool sync_chat_forum_exists(
    storage::SqliteDatabase& database, std::string_view forum_id);
[[nodiscard]] SyncChatIdentity sync_parse_chat_identity(const nlohmann::json& record);

// Request IDs of finished turns with a response or error, in chat order.
[[nodiscard]] std::vector<std::int64_t> sync_chat_turns(
    storage::SqliteDatabase& database, std::int64_t session_key);

// Applies one record in a transaction without recording outgoing work.
void sync_apply_chat_record(
    storage::SqliteDatabase& database, const nlohmann::json& record);

} // namespace cha
