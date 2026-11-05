#pragma once

#include "notes/notes_service.h"
#include "app/background_jobs.h"
#include "providers/api_key_store.h"
#include "providers/openai_oauth.h"
#include "providers/providers.h"
#include "runtime/live_session_manager.h"
#include "runtime/runtime_settings.h"
#include "storage/session_repository.h"
#include "workspace/workspace_config_store.h"

#include <condition_variable>
#include <map>
#include <set>

namespace cha::app {

[[noreturn]] void rethrow_note(const NoteError& error);

struct NotesService::Impl {
    Impl(WorkspaceConfigStore& store, ApiKeyStore& api_keys, OpenAiOAuth& oauth,
        SessionRepository& sessions, LiveSessionManager& live_sessions,
        BackgroundJobs& jobs, const RuntimeSettings& settings, std::timed_mutex& lifecycle,
        std::atomic_uint64_t& epoch,
        std::function<std::optional<ErrorCode>(std::uint64_t)> admit,
        std::function<bool()> can_recover, bool native_host,
        std::function<std::string()> current_vault_name);

    WorkspaceConfigStore* store;
    ApiKeyStore* api_keys;
    OpenAiOAuth* openai_auth;
    SessionRepository* sessions;
    LiveSessionManager* live_sessions;
    BackgroundJobs& background_jobs;
    const RuntimeSettings& settings;
    std::atomic_uint64_t& published_epoch;
    std::timed_mutex& lifecycle_mutex;
    std::function<std::optional<ErrorCode>(std::uint64_t)> admit_locked;

    std::function<bool()> can_recover;
    const bool native_host;
    std::function<std::string()> current_vault_name;

    // Network work waits outside lifecycle_mutex and observes its cancellation flag.
    std::timed_mutex audio_submission_mutex;
    std::mutex background_audio_mutex;
    std::map<std::int64_t, std::shared_ptr<std::atomic_bool>> background_audio;
    // Guarded by lifecycle_mutex; retained across worker replacement.
    std::set<std::int64_t> background_audio_warned;
    NoteAudioChanged note_audio_changed;
    void interrupt_note_audio();
    void remove_note_audio_control(std::int64_t id, const std::shared_ptr<std::atomic_bool>& cancel);
    void resume_note_audio_locked(); // Schedule after maintenance releases its store guard.
    void recover_note_audio(); // Caller holds lifecycle_mutex.
    nlohmann::json background_note_audio_status_locked(std::int64_t id); // Caller holds lifecycle_mutex.
    std::shared_ptr<std::atomic_bool> launch_note_audio(NoteAudioAttempt attempt, std::string voice = {}, std::string jsonl = {});
    void run_note_audio(NoteAudioAttempt attempt, std::uint64_t epoch,
        std::shared_ptr<std::atomic_bool> cancel, std::string voice, std::string jsonl);
    void interrupt_abandoned_audio();
    std::timed_mutex compression_mutex;
    struct CompressionControl {
        std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
        std::string name;
    };
    // All compression state and retirement use lifecycle_mutex.
    std::map<std::int64_t, std::shared_ptr<CompressionControl>> compressions;
    NoteAudioChanged note_compression_changed;
    void notify_compression(std::int64_t id, std::uint64_t epoch) noexcept;
    void retire_compression(std::int64_t id, const std::shared_ptr<CompressionControl>& control,
        std::uint64_t epoch, EventSeverity severity, const std::string& message) noexcept;

    struct SummaryRun {
        std::int64_t run_id{};
        std::string connection_id;
        std::uint64_t epoch{};
        NoteSummaryTarget target;
        std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
        bool ready{};
        std::string label;
    };
    std::int64_t next_summary_run = 1;
    std::map<FullSessionId, std::shared_ptr<SummaryRun>> summaries;
    void retire_summary(const FullSessionId& identity, const std::shared_ptr<SummaryRun>& run,
        EventSeverity severity, std::string_view message, std::string_view reason = {}) noexcept;

    enum class FolderStorePhase {
        preparing,
        uploading,
        indexing,
        cleaning,
        completed,
        failed,
        cancelled,
        warning
    };
    struct FolderStoreControl {
        std::int64_t operation_id{};
        std::uint64_t epoch{};
        std::int64_t root_id{};
        FolderStorePhase phase{FolderStorePhase::preparing};
        std::int64_t completed{};
        std::int64_t total{};
        std::string error;
        std::string result;
        bool terminal{};
        // Retained across cleanup retries, even after a warned resource is gone.
        bool warned{};
        bool known_cleanup_remains{};
        std::shared_ptr<std::atomic_bool> cancel_forward =
            std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> cancel_all =
            std::make_shared<std::atomic_bool>(false);
        std::mutex wait_mutex;
        std::condition_variable wait_cv;
        std::string vault_name;
        std::string store_name;
        std::string store_description;
        std::string folder_name;
        FolderStoreService service{FolderStoreService::openai};
        std::string api_key_id;
        // Remote resources of this upload. CHA keeps them only in memory, the
        // log and Events.
        std::optional<std::string> store_id;
        std::vector<std::string> file_ids;
        bool unknown_allocation{};
    };
    std::map<std::int64_t, std::shared_ptr<FolderStoreControl>> folder_store_controls;
    std::int64_t next_folder_store_operation{1};
    FolderStoreTransport folder_store_transport;
    void interrupt_folder_stores();
    [[nodiscard]] bool folder_store_reserved(std::int64_t folder_id);
    [[nodiscard]] bool folder_store_blocks_delete(std::int64_t item_id);
    [[nodiscard]] std::shared_ptr<FolderStoreControl> folder_store_operation(std::int64_t operation_id);
    void release_folder_store_control_locked(const std::shared_ptr<FolderStoreControl>& control);
    [[nodiscard]] bool folder_store_lifetime_locked(const std::shared_ptr<FolderStoreControl>& control);
    [[nodiscard]] bool folder_store_lifetime(const std::shared_ptr<FolderStoreControl>& control);
    void run_folder_store_upload(std::shared_ptr<FolderStoreControl> control);
    void run_folder_store_cleanup(std::shared_ptr<FolderStoreControl> control);
    using CloudStoreWork = std::function<nlohmann::json(
        FolderStoreClient&, FolderStoreService, const std::string& key, const std::atomic_bool& cancel)>;
    // Resolves the key under lifecycle_mutex, then runs the work as a background job.
    std::shared_ptr<OperationReply> launch_cloud_store(
        std::string_view api_key_id, std::uint64_t epoch, bool writing, CloudStoreWork work);
    bool launch_folder_store_control(
        const std::shared_ptr<FolderStoreControl>& control, std::function<void()> work);

    // Deletion captures targets before the store commit and retires only after it.
    std::vector<std::int64_t> collect_compressions_locked(std::int64_t root);
    void cancel_note_compression_locked(std::int64_t id, std::uint64_t epoch);
    void retire_deleted_compressions_locked(const std::vector<std::int64_t>& ids, std::uint64_t epoch);

    void require_admitted(std::uint64_t epoch) const {
        if (const auto error = admit_locked(epoch)) throw ApplicationError(*error);
    }
};

} // namespace cha::app
