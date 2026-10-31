#include "notes/notes_service_internal.h"
#include "util/logging.h"

namespace cha::app {
namespace {

ErrorCode note_error_code(NoteFailure failure) {
    switch (failure) {
    case NoteFailure::invalid: return ErrorCode::invalid_argument;
    case NoteFailure::not_found: return ErrorCode::not_found;
    case NoteFailure::too_large: return ErrorCode::body_too_large;
    case NoteFailure::opening:
    case NoteFailure::saving:
        return ErrorCode::internal_error;
    }
    return ErrorCode::internal_error;
}

} // namespace

[[noreturn]] void rethrow_note(const NoteError& error) {
    throw ApplicationError(note_error_code(error.failure), error.what());
}

NotesService::Impl::Impl(WorkspaceConfigStore& store, ApiKeyStore& api_keys, OpenAiOAuth& oauth,
    SessionRepository& sessions, LiveSessionManager& live_sessions,
    BackgroundJobs& jobs, const RuntimeSettings& settings, std::timed_mutex& lifecycle,
    std::atomic_uint64_t& epoch,
    std::function<std::optional<ErrorCode>(std::uint64_t)> admit,
    std::function<bool()> can_recover, bool native_host,
    std::function<std::string()> current_vault_name)
    : store(&store), api_keys(&api_keys), openai_auth(&oauth), sessions(&sessions),
      live_sessions(&live_sessions), background_jobs(jobs),
      settings(settings), published_epoch(epoch), lifecycle_mutex(lifecycle), admit_locked(std::move(admit)),
      can_recover(std::move(can_recover)), native_host(native_host), current_vault_name(std::move(current_vault_name)) {}

NotesService::NotesService(WorkspaceConfigStore& store, ApiKeyStore& api_keys, OpenAiOAuth& oauth,
    SessionRepository& sessions, LiveSessionManager& live_sessions,
    BackgroundJobs& jobs, const RuntimeSettings& settings, std::timed_mutex& lifecycle,
    std::atomic_uint64_t& epoch,
    std::function<std::optional<ErrorCode>(std::uint64_t)> admit,
    std::function<bool()> can_recover, bool native_host,
    std::function<std::string()> current_vault_name)
    : impl_(std::make_unique<Impl>(store, api_keys, oauth, sessions, live_sessions, jobs,
        settings, lifecycle, epoch, std::move(admit), std::move(can_recover), native_host,
        std::move(current_vault_name))) {}

NotesService::~NotesService() = default;

std::vector<NoteItem> NotesService::list_note_items(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        auto items = impl_->store->list_note_items();
        const auto stores = impl_->store->list_folder_stores();
        for (auto& item : items) {
            if (item.kind != NoteItemKind::folder) continue;
            for (const auto& store : stores) {
                if (store.folder_id == item.id) {
                    item.store = NoteStoreSummary{std::string(folder_store_service_name(store.service)), store.id,
                        store.state == FolderStoreState::ready ? "ready" : "incomplete"};
                    break;
                }
            }
        }
        return items;
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

NoteContent NotesService::get_note(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->get_note(id);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

bool NotesService::save_note(
    std::int64_t id,
    const nlohmann::json& document,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->save_note(id, document);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

void NotesService::save_note_scroll_position(
    std::int64_t id, std::int64_t scroll_top, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        impl_->store->save_note_scroll_position(id, scroll_top);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

NoteItem NotesService::rename_note_item(
    std::int64_t id,
    std::string name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->rename_note_item(id, name);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

NoteItem NotesService::set_note_read_only(std::int64_t id, bool read_only, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        auto item = impl_->store->set_note_read_only(id, read_only);
        if (!read_only) impl_->interrupt_abandoned_audio();
        return item;
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

NoteItem NotesService::create_note_item(
    std::optional<std::int64_t> parent_id,
    NoteItemKind kind,
    std::string name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (parent_id && impl_->folder_in_delete_subtree(*parent_id)) {
        throw ApplicationError(ErrorCode::invalid_argument, "This folder is being deleted.");
    }
    try {
        return impl_->store->create_note_item(parent_id, kind, name);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

std::int64_t NotesService::import_note_book(
    std::optional<std::int64_t> parent_id,
    std::string name, const std::vector<NoteImportEntry>& notes, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (parent_id && impl_->folder_in_delete_subtree(*parent_id)) {
        throw ApplicationError(ErrorCode::invalid_argument, "This folder is being deleted.");
    }
    try {
        return impl_->store->import_note_book(parent_id, name, notes);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

void NotesService::delete_note_item(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        if (impl_->store->folder_store_owns_subtree(id) || impl_->folder_store_blocks_delete(id)) {
            throw ApplicationError(ErrorCode::invalid_argument,
                "This folder has a store or an active operation. Use folder delete after that operation finishes.");
        }
        const auto folder_ids = impl_->store->folder_ids_in_subtree(id);
        // Capture affected controls before the delete. After it commits, retirement
        // must not depend on another fallible list read.
        const auto deleted = impl_->collect_compressions_locked(id);
        impl_->store->delete_note_item(id);
        for (const auto note_id : deleted) impl_->cancel_note_compression_locked(note_id, epoch);
        impl_->interrupt_abandoned_audio();
        if (!folder_ids.empty() && !impl_->live_sessions->post_welcome_folder_prune(epoch, folder_ids)) {
            log_warn("Could not enqueue Welcome folder prune.");
        }
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

} // namespace cha::app
