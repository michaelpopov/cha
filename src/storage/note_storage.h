#pragma once

#include "storage/sqlite_storage.h"
#include "chat/session_identity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <exception>
#include <functional>
#include "storage/event_storage.h"
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha {

inline constexpr std::int64_t note_id_limit = 9007199254740991LL;
inline constexpr std::size_t note_document_byte_limit = 12582912;
inline constexpr std::string_view empty_note_document =
    R"({"type":"doc","content":[{"type":"paragraph"}]})";

enum class NoteItemKind { folder, note, annotation };

enum class NoteFailure {
    invalid,
    not_found,
    too_large,
    opening,
    saving,
};

class NoteError : public std::exception {
public:
    NoteError(NoteFailure failure, std::string message);
    NoteFailure failure;
    [[nodiscard]] const char* what() const noexcept override;

private:
    std::string message_;
};

struct NoteItem {
    std::int64_t id{};
    std::optional<std::int64_t> parent_id;
    NoteItemKind kind{NoteItemKind::folder};
    std::string name;
    bool has_compressed{};
    bool has_audio{};
    bool read_only{};
    std::optional<std::string> forum_id;
    bool is_summary{};
    bool summary_linked{};
    bool has_l1{};
    bool has_l2{};
    bool summary_excluded{};
    bool folder_summary{};
};

enum class NoteTextMode { full_text, compressed, l1, l2 };

struct NoteContent {
    std::int64_t id{};
    std::int64_t document_id{};
    nlohmann::json document = nlohmann::json::object();
    std::int64_t scroll_top{};
};

struct NoteImportEntry {
    std::string name;
    nlohmann::json document;
};

struct NoteText {
    std::int64_t id{};
    std::string name;
    std::string text;
};

void to_json(nlohmann::json& json, const NoteItem& item);
void to_json(nlohmann::json& json, const NoteContent& content);

// Caller owns the transaction. The statements are idempotent.
void create_note_items_schema(storage::SqliteDatabase& database);
// Owns the migration transaction. Call before creating the runtime schema.
void migrate_compressed_notes(storage::SqliteDatabase& database);

// Caller owns the transaction; names come from a validated configured catalog.
void note_sync_forums(storage::SqliteDatabase& database,
    const std::map<std::string, std::string>& forum_names);

struct NoteSummaryTarget {
    std::int64_t parent_id{};
    std::optional<std::int64_t> target_note_id;
};
[[nodiscard]] NoteSummaryTarget note_summary_target(
    storage::SqliteDatabase& database, const FullSessionId& identity);
[[nodiscard]] NoteItem note_commit_summary(storage::SqliteDatabase& database,
    const FullSessionId& identity, const NoteSummaryTarget& target,
    const nlohmann::json& document,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now(),
    std::string_view sync_machine = {});
struct SummaryChapters {
    std::string short_markdown;
    std::string long_markdown;
};
[[nodiscard]] SummaryChapters note_summary_chapters(const nlohmann::json& document);
// Owns a dedicated read-only connection and transaction for this call.
[[nodiscard]] std::string note_summary_tool(const std::filesystem::path& path,
    std::string_view password, std::string_view forum_id, std::string_view tool_name,
    std::string_view arguments, const std::atomic_bool& cancellation);

// Complete bounded snapshot, or nullopt on cancellation/database failure.
[[nodiscard]] std::optional<nlohmann::json> note_short_summary_catalogue(
    const std::filesystem::path& path, std::string_view password, std::string_view forum_id,
    std::int64_t session_key, const std::atomic_bool& cancellation);

// Markdown for chat context, retaining headings, lists, links, emphasis, and code blocks.
[[nodiscard]] std::string note_document_markdown(const nlohmann::json& document);
[[nodiscard]] nlohmann::json note_document_from_text(std::string_view text);
[[nodiscard]] nlohmann::json note_parse_saved_document(
    std::string_view bytes, std::int64_t id);
[[nodiscard]] NoteText note_get_text(
    storage::SqliteDatabase& database, std::int64_t id, bool prefer_compressed = false);

[[nodiscard]] std::optional<NoteItem> note_find_item(storage::SqliteDatabase& database, std::int64_t id);
[[nodiscard]] std::vector<NoteItem> note_list(storage::SqliteDatabase& database);
[[nodiscard]] std::int64_t note_import_book(
    storage::SqliteDatabase& database,
    std::optional<std::int64_t> parent_id,
    std::string_view name,
    const std::vector<NoteImportEntry>& notes,
    std::string_view sync_machine = {});
[[nodiscard]] NoteItem note_create(
    storage::SqliteDatabase& database,
    std::optional<std::int64_t> parent_id,
    NoteItemKind kind,
    std::string_view name,
    std::string_view sync_machine = {});
[[nodiscard]] NoteContent note_get(
    storage::SqliteDatabase& database,
    std::int64_t id, NoteTextMode mode = NoteTextMode::full_text);
bool note_save(
    storage::SqliteDatabase& database,
    std::int64_t id,
    const nlohmann::json& document,
    std::string_view sync_machine = {});
void note_save_compressed(
    storage::SqliteDatabase& database, std::int64_t id, std::string_view text,
    std::string_view sync_machine = {});
// Checks the saved Markdown and commits the result and success event together.
bool note_commit_compressed(storage::SqliteDatabase& database, std::int64_t id,
    std::string_view source, std::string_view text, std::string_view success_message,
    std::string_view sync_machine = {});
// Writes the L1 or L2 row, the success event, and the sync record together.
void note_commit_level(storage::SqliteDatabase& database, std::int64_t id,
    int level, const nlohmann::json& document, std::string_view success_message,
    std::string_view sync_machine = {});
[[nodiscard]] NoteItem note_commit_folder_summary(storage::SqliteDatabase& database,
    std::int64_t folder_id, const nlohmann::json& document, std::string_view sync_machine = {});
[[nodiscard]] NoteItem note_set_summary_excluded(storage::SqliteDatabase& database,
    std::int64_t id, bool excluded, std::string_view sync_machine = {});
void note_save_scroll_position(
    storage::SqliteDatabase& database, std::int64_t id, std::int64_t scroll_top);
[[nodiscard]] NoteItem note_set_read_only(
    storage::SqliteDatabase& database, std::int64_t id, bool read_only,
    std::string_view sync_machine = {});
[[nodiscard]] NoteItem note_rename(
    storage::SqliteDatabase& database,
    std::int64_t id,
    std::string_view name,
    std::string_view sync_machine = {});
void note_delete(storage::SqliteDatabase& database, std::int64_t id,
    std::string_view sync_machine = {});
// Caller owns the transaction.
void note_delete_in_transaction(storage::SqliteDatabase& database, std::int64_t id);
struct NoteAudioAttempt {
    std::int64_t id{}, note_id{}, clip_count{};
    std::string note_name, service_origin, api_key_id;
    std::optional<std::string> batch_id;
};
std::string note_audio_target(const NoteAudioAttempt& attempt);
std::optional<NoteAudioAttempt> note_audio_attempt_by_id(storage::SqliteDatabase& database, std::int64_t id);
std::optional<NoteAudioAttempt> note_audio_attempt_for_note(storage::SqliteDatabase& database, std::int64_t id);
std::vector<NoteAudioAttempt> note_audio_attempts(storage::SqliteDatabase& database);
std::optional<NoteAudioAttempt> note_audio_accept(storage::SqliteDatabase& database, std::int64_t note_id,
    std::int64_t count, std::string_view origin, std::string_view key_id,
    const std::function<void(const NoteAudioAttempt&)>& launch, bool only_if_missing = false);
bool note_audio_register(storage::SqliteDatabase& database, std::int64_t attempt_id,
    std::string_view batch_id, std::string_view voice);
bool note_audio_finish(storage::SqliteDatabase& database, std::int64_t attempt_id,
    const std::vector<std::filesystem::path>& clips);
bool note_audio_retire(storage::SqliteDatabase& database, std::int64_t attempt_id,
    EventSeverity severity, std::string_view message);
struct NoteAudioClip { std::string mime_type, bytes; };
nlohmann::json note_audio_info(storage::SqliteDatabase& database, std::int64_t id);
NoteAudioClip note_audio_clip(storage::SqliteDatabase& database, std::int64_t id, std::int64_t ordinal);
void note_save_audio(storage::SqliteDatabase& database, std::int64_t id,
    const std::vector<std::filesystem::path>& clips);

// Applies one incoming sync record in its own transaction. Returns true when
// notes changed. Throws NoteError when the record must be skipped with a
// warning (missing parent, managed item, invalid candidate). Never writes an
// outgoing record or sets a changed flag.
[[nodiscard]] bool note_apply_record(
    storage::SqliteDatabase& database, const nlohmann::json& record);
// Exports the folder, its subfolders, notes and audio to a new SQLite file.
// Annotations, summary links and forum links are left out.
void note_export_folder(storage::SqliteDatabase& database, std::int64_t folder_id,
    const std::filesystem::path& destination);
// Copies an exported folder into parent_id and returns the new folder ID.
[[nodiscard]] std::int64_t note_import_folder(storage::SqliteDatabase& database,
    std::optional<std::int64_t> parent_id, const std::filesystem::path& source,
    std::string_view sync_machine = {});

} // namespace cha
