#include "storage/event_storage.h"

#include "storage/session_timestamp.h"
#include "util/json_serialization.h"

#include <stdexcept>

namespace cha {
namespace {

std::string_view severity_name(EventSeverity severity) {
    switch (severity) {
    case EventSeverity::info: return "info";
    case EventSeverity::success: return "success";
    case EventSeverity::warning: return "warning";
    case EventSeverity::error: return "error";
    }
    throw std::invalid_argument("Invalid event severity");
}

EventSeverity read_severity(std::string_view severity) {
    if (severity == "info") return EventSeverity::info;
    if (severity == "success") return EventSeverity::success;
    if (severity == "warning") return EventSeverity::warning;
    if (severity == "error") return EventSeverity::error;
    throw std::runtime_error("Invalid stored event severity");
}

void validate_message(std::string_view message) {
    if (message.empty()) throw std::invalid_argument("Empty event message");
    (void)dump_json(nlohmann::json(message), "Event message");
}

} // namespace

void create_event_history_schema(storage::SqliteDatabase& database) {
    database.execute(R"(
        CREATE TABLE IF NOT EXISTS event_history (
            id INTEGER PRIMARY KEY CHECK (id > 0 AND id <= 9007199254740991),
            recorded_at INTEGER NOT NULL CHECK (recorded_at >= 0),
            severity TEXT NOT NULL CHECK (severity IN ('info', 'success', 'warning', 'error')),
            message TEXT NOT NULL CHECK (length(message) > 0)
        ) STRICT;
    )");
}

void event_append(storage::SqliteDatabase& database,
    EventSeverity severity, std::string_view message) {
    storage::SqliteTransaction transaction(database);
    event_append_in_transaction(database, severity, message);
    transaction.commit();
}

void event_append_in_transaction(storage::SqliteDatabase& database,
    EventSeverity severity, std::string_view message) {
    const auto name = severity_name(severity);
    validate_message(message);
    database.prepare(
        "INSERT INTO event_history (recorded_at, severity, message) VALUES (?1, ?2, ?3)",
        session_timestamp(), name, message).run();
    database.prepare(
        "DELETE FROM event_history WHERE id NOT IN ("
        "SELECT id FROM event_history ORDER BY id DESC LIMIT ?1)",
        event_history_limit).run();
}

std::vector<ActivityEvent> event_list(storage::SqliteDatabase& database) {
    auto statement = database.prepare(
        "SELECT id, recorded_at, severity, message FROM event_history "
        "ORDER BY id DESC LIMIT ?1", event_history_limit);
    std::vector<ActivityEvent> events;
    while (statement.step()) {
        ActivityEvent event{
            .id = statement.integer(0),
            .recorded_at = statement.integer(1),
            .severity = read_severity(statement.text(2)),
            .message = statement.text(3),
        };
        if (event.id <= 0 || event.id > 9007199254740991LL || event.recorded_at < 0) {
            throw std::runtime_error("Invalid stored event record");
        }
        validate_message(event.message);
        events.push_back(std::move(event));
    }
    return events;
}

void to_json(nlohmann::json& json, const ActivityEvent& event) {
    json = {{"id", event.id}, {"recorded_at", event.recorded_at},
        {"severity", severity_name(event.severity)}, {"message", event.message}};
}

} // namespace cha
