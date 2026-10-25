#pragma once

#include "storage/sqlite_storage.h"

#include <cstdint>
#include <filesystem>
#include <exception>
#include <optional>
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
    bool read_only{};
};

struct NoteContent {
    std::int64_t id{};
    nlohmann::json document = nlohmann::json::object();
    std::optional<std::string> compressed_text;
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

// Markdown for chat context, retaining headings, lists, links, emphasis, and code blocks.
[[nodiscard]] std::string note_document_markdown(const nlohmann::json& document);
[[nodiscard]] NoteText note_get_text(
    storage::SqliteDatabase& database, std::int64_t id, bool prefer_compressed = false);

[[nodiscard]] std::vector<NoteItem> note_list(storage::SqliteDatabase& database);
[[nodiscard]] std::int64_t note_import_book(
    storage::SqliteDatabase& database,
    std::optional<std::int64_t> parent_id,
    std::string_view name,
    const std::vector<NoteImportEntry>& notes);
[[nodiscard]] NoteItem note_create(
    storage::SqliteDatabase& database,
    std::optional<std::int64_t> parent_id,
    NoteItemKind kind,
    std::string_view name);
[[nodiscard]] NoteContent note_get(
    storage::SqliteDatabase& database,
    std::int64_t id);
bool note_save(
    storage::SqliteDatabase& database,
    std::int64_t id,
    const nlohmann::json& document);
void note_save_compressed(
    storage::SqliteDatabase& database, std::int64_t id, std::string_view text);
void note_save_scroll_position(
    storage::SqliteDatabase& database, std::int64_t id, std::int64_t scroll_top);
[[nodiscard]] NoteItem note_set_read_only(
    storage::SqliteDatabase& database, std::int64_t id, bool read_only);
[[nodiscard]] NoteItem note_rename(
    storage::SqliteDatabase& database,
    std::int64_t id,
    std::string_view name);
void note_delete(storage::SqliteDatabase& database, std::int64_t id);
struct NoteAudioClip { std::string mime_type, bytes; };
nlohmann::json note_audio_info(storage::SqliteDatabase& database, std::int64_t id);
NoteAudioClip note_audio_clip(storage::SqliteDatabase& database, std::int64_t id, std::int64_t ordinal);
void note_save_audio(storage::SqliteDatabase& database, std::int64_t id,
    const std::vector<std::filesystem::path>& clips);

} // namespace cha
