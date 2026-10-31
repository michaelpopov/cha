#pragma once

#include "app/application_result.h"
#include "notes/notes_types.h"
#include "providers/folder_store_client.h"
#include "storage/folder_store_storage.h"
#include "storage/note_storage.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
class WorkspaceConfigStore;
class SessionRepository;
class LiveSessionManager;
class ApiKeyStore;
class OpenAiOAuth;
struct RuntimeSettings;
}

namespace cha::app {

class BackgroundJobs;

// Borrows the application lifecycle lock and admission policy. Ordinary methods
// acquire that lock; methods ending in _locked require the caller to hold it.
// Admission and current-vault callbacks require that lock. Recovery eligibility
// reads only the application's atomic running/stopping state.
// Construction does not start recovery. Join BackgroundJobs before destroying
// the service or its borrowed dependencies; the service destructor does not join.
class NotesService {
public:
    NotesService(WorkspaceConfigStore& store, ApiKeyStore& api_keys, OpenAiOAuth& oauth,
        SessionRepository& sessions, LiveSessionManager& live_sessions,
        BackgroundJobs& jobs, const RuntimeSettings& settings, std::timed_mutex& lifecycle,
        std::atomic_uint64_t& epoch,
        std::function<std::optional<ErrorCode>(std::uint64_t)> admit,
        std::function<bool()> can_recover, bool native_host,
        std::function<std::string()> current_vault_name);
    ~NotesService();
    NotesService(const NotesService&) = delete;
    NotesService& operator=(const NotesService&) = delete;

    std::vector<NoteItem> list_note_items(std::uint64_t epoch);
    NoteContent get_note(std::int64_t id, std::uint64_t epoch);
    bool save_note(std::int64_t id, const nlohmann::json& document, std::uint64_t epoch);
    void save_note_scroll_position(std::int64_t id, std::int64_t scroll_top, std::uint64_t epoch);
    NoteItem rename_note_item(std::int64_t id, std::string name, std::uint64_t epoch);

    void set_note_compression_changed(NoteAudioChanged callback);
    nlohmann::json compress_note(std::int64_t id, std::uint64_t epoch, bool skip_ineligible = false);
    nlohmann::json note_compression_status(std::int64_t id, std::uint64_t epoch);
    void cancel_note_compression(std::int64_t id, std::uint64_t epoch);

    void cancel_compressions_locked();
    nlohmann::json compression_status_locked(std::int64_t id);
    // Generate returns Markdown and a run ID through its reply. Convert the
    // Markdown, then commit the document or abandon that run ID. Each reservation
    // belongs to one connection and epoch; commit/abandon must use the same pair.
    // Connection close and maintenance use cancel_summaries_locked to cancel and
    // retire runs. Session deletion only signals cancellation via
    // cancel_session_summary_locked.
    std::shared_ptr<OperationReply> generate_session_summary(
        std::string connection_id, const FullSessionId& identity, std::uint64_t epoch);
    NoteItem commit_session_summary(std::string_view connection_id,
        std::int64_t run_id, const nlohmann::json& document, std::uint64_t epoch);
    void abandon_session_summary(std::string_view connection_id,
        std::int64_t run_id, std::string_view reason, std::uint64_t epoch);
    void cancel_summaries_locked(std::optional<std::string_view> connection_id = {});
    // Session deletion signals the run, leaving retirement to its current owner.
    void cancel_session_summary_locked(const FullSessionId& identity);
    std::shared_ptr<OperationReply> read_note_page(std::string url, std::uint64_t epoch);
    std::shared_ptr<OperationReply> read_note_picture(std::string url, std::uint64_t epoch);

    void set_note_audio_changed(NoteAudioChanged callback);
    nlohmann::json start_background_note_audio(std::int64_t id, std::string voice,
        std::vector<std::string> blocks, std::uint64_t epoch, bool only_if_missing = false);
    nlohmann::json background_note_audio_status(std::int64_t id, std::uint64_t epoch);
    nlohmann::json get_note_audio_info(std::int64_t id, std::uint64_t epoch);
    NoteAudioClip get_note_audio_clip_locked(std::int64_t id, std::int64_t ordinal, std::uint64_t epoch);
    nlohmann::json background_audio_status_locked(std::int64_t id);
    // Shutdown and epoch publication must be able to signal audio without the lifecycle lock.
    void interrupt_note_audio();
    void resume_note_audio_locked();
    void clear_audio_warnings_locked();
    NoteItem set_note_read_only(std::int64_t id, bool read_only, std::uint64_t epoch);

    [[nodiscard]] std::vector<FolderStoreChoiceView> folder_store_choices(std::uint64_t epoch);
    [[nodiscard]] FolderStoreStart start_folder_store_upload(
        std::int64_t folder_id,
        std::string service,
        std::string api_key_id,
        std::uint64_t epoch);
    [[nodiscard]] FolderStoreStart start_folder_store_remove(
        std::int64_t folder_id,
        std::int64_t expected_store_id,
        std::uint64_t epoch);
    [[nodiscard]] FolderStoreOperationStatus folder_store_operation_status(
        std::int64_t folder_id, std::uint64_t epoch);
    void cancel_folder_store_upload(std::int64_t operation_id, std::uint64_t epoch);
    void close_folder_store_operation(std::int64_t operation_id, std::uint64_t epoch);
    [[nodiscard]] FolderStoreStart start_folder_delete(
        std::int64_t folder_id, std::int64_t confirmed_store_count, std::uint64_t epoch);
    [[nodiscard]] FolderStoreStart retry_folder_delete(
        std::int64_t operation_id, std::uint64_t epoch);
    void delete_folder_anyway(std::int64_t operation_id, std::uint64_t epoch);
    [[nodiscard]] std::optional<FolderStoreRecord> folder_store_record(
        std::int64_t folder_id, std::uint64_t epoch);
    [[nodiscard]] std::vector<FolderStoreRecord> list_folder_stores(std::uint64_t epoch);
    [[nodiscard]] std::int64_t folder_store_count_in_subtree(
        std::int64_t folder_id, std::uint64_t epoch);
    [[nodiscard]] std::vector<FolderStoreKeyUsage> folder_store_usage(std::uint64_t epoch);
    void set_folder_store_transport_for_tests(FolderStoreTransport transport);
    void interrupt_folder_stores_locked();

    NoteItem create_note_item(std::optional<std::int64_t> parent_id,
        NoteItemKind kind, std::string name, std::uint64_t epoch);
    std::int64_t import_note_book(std::optional<std::int64_t> parent_id,
        std::string name, const std::vector<NoteImportEntry>& notes, std::uint64_t epoch);
    void delete_note_item(std::int64_t id, std::uint64_t epoch);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha::app
