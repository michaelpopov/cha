#pragma once

#include "providers/folder_store_types.h"
#include "storage/note_storage.h"
#include "storage/session_database.h"
#include "storage/sqlite_storage.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// OpenAI documents 512 MB without defining the byte unit; use decimal MB
// conservatively. Google's v1beta Discovery document specifies 104857600 bytes.
inline constexpr std::size_t folder_store_openai_file_byte_limit =
    512ull * 1000ull * 1000ull;
inline constexpr std::size_t folder_store_google_document_byte_limit =
    104857600ull;

class FolderStoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct FolderStoreDocument {
    std::int64_t note_id{};
    std::string note_name;
    std::string folder_path;
    std::string filename;
    std::string markdown;
};

struct FolderStoreCapture {
    NoteItem folder;
    std::string folder_path;
    std::vector<FolderStoreDocument> documents;
};

// Caller owns the schema transaction. The statements are idempotent and drop
// the old folder store tables.
void create_folder_store_schema(storage::SqliteDatabase& database);

[[nodiscard]] std::string_view folder_store_service_name(FolderStoreService service);
[[nodiscard]] FolderStoreService parse_folder_store_service(std::string_view value);
[[nodiscard]] std::string note_item_ref_id(std::int64_t id);
[[nodiscard]] std::string folder_store_filename(std::string_view name, std::int64_t note_id);
[[nodiscard]] bool folder_store_document_nonempty(const nlohmann::json& document);
[[nodiscard]] std::size_t folder_store_document_byte_limit(FolderStoreService service);
// Validates final UTF-8 bytes, including metadata, without allocating a second copy.
void folder_store_validate_document_size(FolderStoreService service, std::size_t bytes,
    std::string_view note_name, std::int64_t note_id);

[[nodiscard]] std::optional<NoteItem> folder_store_require_folder(
    storage::SqliteDatabase& database, std::int64_t folder_id);
[[nodiscard]] std::vector<std::int64_t> folder_store_folder_ids_in_subtree(
    storage::SqliteDatabase& database, std::int64_t folder_id);
[[nodiscard]] std::vector<std::int64_t> session_folder_ids(
    storage::SqliteDatabase& database, SessionKey session_key);

// Caller owns the transaction.
void folder_store_delete_session_folders_in_subtree(
    storage::SqliteDatabase& database, std::int64_t folder_id);
[[nodiscard]] FolderStoreCapture folder_store_capture(
    storage::SqliteDatabase& database,
    std::int64_t folder_id,
    FolderStoreService service);

void session_folder_insert(
    storage::SqliteDatabase& database, SessionKey session_key, std::int64_t folder_id);
void session_folder_delete(
    storage::SqliteDatabase& database, SessionKey session_key, std::int64_t folder_id);
void session_folder_clear(
    storage::SqliteDatabase& database, SessionKey session_key);
void session_folder_prune(
    storage::SqliteDatabase& database,
    SessionKey session_key,
    std::span<const std::int64_t> folder_ids);

} // namespace cha
