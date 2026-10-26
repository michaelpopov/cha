#pragma once

#include "storage/sqlite_storage.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha {

inline constexpr std::int64_t event_history_limit = 200;

enum class EventSeverity { info, success, warning, error };

struct ActivityEvent {
    std::int64_t id{};
    std::int64_t recorded_at{}; // Unix seconds, UTC.
    EventSeverity severity{EventSeverity::info};
    std::string message;
};

// Caller owns the schema transaction.
void create_event_history_schema(storage::SqliteDatabase& database);
// Owns the insert-and-trim transaction; throws on failure.
void event_append(storage::SqliteDatabase& database,
    EventSeverity severity, std::string_view message);
// Caller owns the transaction.
void event_append_in_transaction(storage::SqliteDatabase& database,
    EventSeverity severity, std::string_view message);
// Read-only; throws on failure.
std::vector<ActivityEvent> event_list(storage::SqliteDatabase& database);
void to_json(nlohmann::json& json, const ActivityEvent& event);

} // namespace cha
