#pragma once

#include "session/session_storage_layout.h"
#include "session/sqlite_storage.h"

#include <cstdint>
#include <string>

namespace cha {

struct ValidatedLegacySession {
    LegacySessionSource source;
    std::string label;
    std::int64_t version{};
    std::int64_t history_epoch{};
    std::int64_t next_entry_id{};
    std::int64_t next_request_id{};
    std::int64_t updated_at{};
    std::uint64_t turns{};
    std::uint64_t entries{};
};

ValidatedLegacySession validate_legacy_session_source(
    LegacySessionSource source);
void import_legacy_session(
    storage::SqliteDatabase& target,
    const ValidatedLegacySession& source,
    std::int64_t forum_key,
    std::int64_t archived_at);

} // namespace cha
