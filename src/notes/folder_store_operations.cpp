#include "notes/notes_service_internal.h"

#include "util/logging.h"
#include "util/text.h"
#include "workspace/builtins.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
#include <tuple>
#include <utility>

namespace cha::app {
namespace {

struct Interrupted {};
struct Cancelled {};

std::optional<std::int64_t> parent_of(const std::vector<NoteItem>& items, std::int64_t id) {
    for (const auto& item : items) if (item.id == id) return item.parent_id;
    return {};
}

bool in_subtree(const std::vector<NoteItem>& items, std::int64_t id, std::int64_t root) {
    for (std::optional<std::int64_t> current = id; current; current = parent_of(items, *current)) {
        if (*current == root) return true;
    }
    return false;
}

template<typename Phase>
std::string phase_name(Phase phase) {
    switch (phase) {
    case Phase::preparing: return "preparing";
    case Phase::uploading: return "uploading";
    case Phase::indexing: return "indexing";
    case Phase::cleaning: return "cleaning";
    case Phase::completed: return "completed";
    case Phase::failed: return "failed";
    case Phase::cancelled: return "cancelled";
    case Phase::warning: return "warning";
    }
    return "failed";
}

FolderStoreOperationStatus status_from(const auto& control) {
    // A sent allocation may still be returning its ID after cancellation is accepted.
    return {.present = true, .operation_id = control.operation_id,
        .phase = !control.terminal && control.cancel_forward->load() ? "cleaning" : phase_name(control.phase),
        .completed = control.completed, .total = control.total,
        .error = control.error, .result = control.result,
        .known_cleanup_remains = control.known_cleanup_remains, .terminal = control.terminal};
}

// All application/store access by a worker goes through this guard. Network
// calls use copied inputs after the guard releases the lifecycle lock.
decltype(auto) guarded(auto& self, const auto& control, auto&& work) {
    if (self.settings.test_hooks.folder_store_before_lifetime_lock) self.settings.test_hooks.folder_store_before_lifetime_lock();
    const std::lock_guard lock(self.lifecycle_mutex);
    if (!self.folder_store_lifetime_locked(control)) throw Interrupted{};
    return work();
}

std::string context(const auto& control) {
    return "folder “" + control.folder_name + "” (" + std::to_string(control.root_id)
        + "), service=" + std::string(folder_store_service_name(control.service))
        + " key=" + control.api_key_id;
}

std::string resource_ids(const auto& control) {
    std::string result;
    if (control.store_id) result += " store=" + *control.store_id;
    for (const auto& id : control.file_ids) result += " file=" + id;
    return result;
}

std::string unknown_warning(const auto& control) {
    return "Unconfirmed allocation for " + context(control) + resource_ids(control)
        + " name=" + nlohmann::json(control.store_name).dump()
        + ". Remote data may remain. Check the original provider project for manual cleanup. "
          "An ID that the provider did not return cannot be recovered here.";
}

void record_event(auto& self, EventSeverity severity, const std::string& message) {
    try { self.store->append_event(severity, message); }
    catch (...) { log_warn("Folder store event could not be recorded: " + message); }
}

void log_id(const auto& control, std::string_view kind, std::string_view id, bool late = false) {
    const auto message = "Folder store " + std::string(kind) + " id=" + std::string(id)
        + " vault=" + control.vault_name + " folder=" + std::to_string(control.root_id)
        + " service=" + std::string(folder_store_service_name(control.service))
        + " key_id=" + control.api_key_id;
    if (late) log_warn("Abandoned " + message);
    else log_info(message);
}

std::string current_key(auto& self, const auto& control, bool forward) {
    return guarded(self, control, [&] {
        if (forward && control->cancel_forward->load()) throw Cancelled{};
        const auto info = self.api_keys->find(control->api_key_id);
        if (!info || !info->has_value) {
            throw FolderStoreError("The saved API key is missing or empty.");
        }
        return self.api_keys->value(control->api_key_id);
    });
}

void wait_round(auto& self, const auto& control, int round) {
    if (self.settings.test_hooks.folder_store_wait) {
        self.settings.test_hooks.folder_store_wait(round);
        return;
    }
    std::unique_lock lock(control->wait_mutex);
    control->wait_cv.wait_for(lock, self.settings.folder_store_poll_interval, [&] {
        const bool cancelled = control->cancel_forward->load() || control->cancel_all->load();
        if (self.settings.test_hooks.folder_store_poll_predicate_read) self.settings.test_hooks.folder_store_poll_predicate_read();
        return cancelled;
    });
}

void finish(auto& self, const auto& control, std::string result, std::string error = {}) {
    guarded(self, control, [&] {
        using Phase = decltype(control->phase);
        control->phase = result == "warning" ? Phase::warning
            : result == "cancelled" ? Phase::cancelled
            : result == "success" ? Phase::completed : Phase::failed;
        if (result == "error" && control->warned) {
            if (!error.empty()) error += " ";
            error += "Some remote data could not be confirmed deleted. See Events for manual-cleanup details.";
        }
        control->result = std::move(result);
        control->error = std::move(error);
        control->terminal = true;
    });
}

void require_result(const FolderStoreRemoteResult& result, std::string_view label = {}) {
    if (result.kind == FolderStoreRemoteKind::ok && !result.operation_error) return;
    if (result.kind == FolderStoreRemoteKind::cancelled) throw Cancelled{};
    throw FolderStoreError((label.empty() ? std::string{} : std::string(label) + ": ")
        + (result.message.empty() ? "The provider request failed." : result.message));
}

// Keeps a returned allocation for cleanup. A lost allocation is only logged.
void keep_allocation(auto& self, const auto& control, const FolderStoreRemoteResult& result, bool file) {
    if (result.id.empty()) {
        if (result.unknown_allocation) guarded(self, control, [&] { control->unknown_allocation = true; });
        return;
    }
    log_id(*control, file ? "file" : "store", result.id);
    try {
        guarded(self, control, [&] {
            if (file) control->file_ids.push_back(result.id);
            else control->store_id = result.id;
        });
    } catch (const Interrupted&) {
        log_id(*control, file ? "file" : "store", result.id, true);
        throw;
    }
}

// The same routine serves failed uploads, cancellation and Retry cleanup.
// Each known resource is attempted once; only explicit user work retries failures.
bool cleanup(auto& self, const auto& control, FolderStoreClient& client) {
    const auto [service, store_id, file_ids] = guarded(self, control, [&] {
        control->phase = decltype(control->phase)::cleaning;
        return std::tuple{control->service, control->store_id, control->file_ids};
    });
    std::string errors;
    std::string first_error;
    const auto remove = [&](const std::string& id, bool store_resource) {
        try {
            const auto key = current_key(self, control, false);
            const auto result = service == FolderStoreService::google
                ? client.google_delete_store(id, key, *control->cancel_all)
                : store_resource ? client.openai_delete_store(id, key, *control->cancel_all)
                                 : client.openai_delete_file(id, key, *control->cancel_all);
            guarded(self, control, [&] {
                const bool not_found = result.kind == FolderStoreRemoteKind::not_found;
                if (result.kind != FolderStoreRemoteKind::ok && !not_found) {
                    throw FolderStoreError(result.message.empty() ? "Remote cleanup failed." : result.message);
                }
                if (not_found) {
                    const auto warning = "Resource " + id + " was not found for " + context(*control)
                        + ". The saved key may refer to another project. Manual cleanup may still be needed.";
                    log_warn(warning);
                    record_event(self, EventSeverity::warning, warning);
                    control->warned = true;
                }
                if (store_resource) control->store_id.reset();
                else std::erase(control->file_ids, id);
            });
        } catch (const Interrupted&) { throw; }
        catch (const FolderStoreError& error) {
            if (first_error.empty()) first_error = error.what();
            errors += " " + std::string(error.what()) + " Resource=" + id + ".";
        }
        catch (const std::exception&) {
            if (first_error.empty()) first_error = "Could not confirm remote cleanup.";
            errors += " Could not confirm cleanup of " + id + ".";
        }
    };
    if (store_id) remove(*store_id, true);
    for (const auto& id : file_ids) remove(id, false);
    return guarded(self, control, [&] {
        control->known_cleanup_remains = control->store_id || !control->file_ids.empty();
        if (!control->known_cleanup_remains) return true;
        control->error = first_error.empty() ? "Could not confirm remote cleanup." : first_error;
        const auto diagnostic = context(*control) + errors + resource_ids(*control);
        log_warn(diagnostic);
        record_event(self, EventSeverity::warning, diagnostic);
        return false;
    });
}

struct WorkItem {
    FolderStoreDocument document;
    std::string remote_id;
    bool done{};
};

} // namespace

bool NotesService::Impl::folder_store_reserved(std::int64_t folder_id) {
    const auto found = folder_store_controls.find(folder_id);
    return found != folder_store_controls.end() && !found->second->terminal;
}

bool NotesService::Impl::folder_store_blocks_delete(std::int64_t item_id) {
    if (std::all_of(folder_store_controls.begin(), folder_store_controls.end(), [](const auto& entry) {
        return entry.second->terminal;
    })) return false;
    const auto items = store->list_note_items();
    for (const auto& [id, control] : folder_store_controls) {
        if (!control->terminal && in_subtree(items, id, item_id)) return true;
    }
    return false;
}

std::shared_ptr<NotesService::Impl::FolderStoreControl> NotesService::Impl::folder_store_operation(std::int64_t operation_id) {
    for (const auto& [id, control] : folder_store_controls) {
        if (control->operation_id == operation_id) return control;
    }
    return {};
}

void NotesService::Impl::release_folder_store_control_locked(const std::shared_ptr<FolderStoreControl>& control) {
    const auto found = folder_store_controls.find(control->root_id);
    if (found != folder_store_controls.end() && found->second == control) folder_store_controls.erase(found);
}

bool NotesService::Impl::folder_store_lifetime_locked(const std::shared_ptr<FolderStoreControl>& control) {
    if (admit_locked(control->epoch) || control->cancel_all->load()) return false;
    if (folder_store_operation(control->operation_id) != control) return false;
    store->require_main();
    return true;
}

bool NotesService::Impl::folder_store_lifetime(const std::shared_ptr<FolderStoreControl>& control) {
    const std::lock_guard lock(lifecycle_mutex);
    return folder_store_lifetime_locked(control);
}

void NotesService::Impl::run_folder_store_upload(std::shared_ptr<FolderStoreControl> control) {
    bool remote_started = false;
    FolderStoreClient client;
    try {
        if (settings.test_hooks.folder_store_worker_started) settings.test_hooks.folder_store_worker_started(control->operation_id, control->cancel_all);
        auto capture = guarded(*this, control, [&] {
            if (control->cancel_forward->load()) throw Cancelled{};
            const auto info = api_keys->find(control->api_key_id);
            if (!info || !info->has_value) {
                throw FolderStoreError("The saved API key is missing or empty.");
            }
            const auto transport = folder_store_transport ? folder_store_transport : folder_store_curl_transport;
            client = FolderStoreClient([this, control, transport](const auto& request, const auto& cancel) {
                auto authorized = request;
                const bool forward = &cancel == control->cancel_forward.get();
                const auto key = current_key(*this, control, forward);
                for (auto& header : authorized.headers) {
                    if (header.name == "x-goog-api-key") header.value = key;
                    if (header.name == "Authorization") header.value = "Bearer " + key;
                }
                return transport(authorized, cancel);
            });
            return store->capture_folder_store(control->root_id, control->service);
        });
        guarded(*this, control, [&] {
            if (control->cancel_forward->load()) throw Cancelled{};
            control->folder_name = capture.folder.name;
            control->total = static_cast<std::int64_t>(capture.documents.size());
            control->phase = FolderStorePhase::uploading;
        });
        auto key = current_key(*this, control, true);
        remote_started = true;
        auto created = control->service == FolderStoreService::openai
            ? client.openai_create_store(control->store_name, control->store_description, key, *control->cancel_all)
            : client.google_create_store(control->store_name, key, *control->cancel_all);
        keep_allocation(*this, control, created, false);
        if (created.kind == FolderStoreRemoteKind::rejected) created.message = "The provider rejected the store name or description.";
        require_result(created);
        std::vector<WorkItem> items;
        items.reserve(capture.documents.size() + 1);
        if (control->service == FolderStoreService::google) {
            key = current_key(*this, control, true);
            auto uploaded = client.google_upload_store_metadata(created.id, control->store_name,
                control->store_description, key, *control->cancel_all, *control->cancel_forward);
            if (!uploaded.id.empty()) {
                log_id(*control, "operation", uploaded.id);
                if (!folder_store_lifetime(control)) {
                    log_id(*control, "operation", uploaded.id, true);
                    throw Interrupted{};
                }
            }
            if (uploaded.kind == FolderStoreRemoteKind::rejected) uploaded.message = "The provider rejected the store name or description.";
            require_result(uploaded, "Store metadata");
            items.push_back({.document = {.note_id = 0, .note_name = "Store metadata"},
                .remote_id = uploaded.id, .done = uploaded.done});
        }
        for (auto& document : capture.documents) {
            key = current_key(*this, control, true);
            WorkItem item{.document = std::move(document)};
            if (control->service == FolderStoreService::openai) {
                const auto uploaded = client.openai_upload_file(item.document.filename, item.document.markdown, key, *control->cancel_all);
                keep_allocation(*this, control, uploaded, true);
                require_result(uploaded, "Note “" + item.document.note_name + "”");
                item.remote_id = uploaded.id;
                key = current_key(*this, control, true);
                const auto attached = client.openai_attach_file(created.id, uploaded.id, key, *control->cancel_forward);
                require_result(attached, "Note “" + item.document.note_name + "”");
                if (attached.status == "failed" || attached.status == "cancelled") throw FolderStoreError("Could not index note “" + item.document.note_name + "”.");
                item.done = attached.status == "completed";
            } else {
                const auto uploaded = client.google_upload_document(created.id, item.document.filename,
                    item.document.markdown, key, *control->cancel_all, *control->cancel_forward);
                if (!uploaded.id.empty()) {
                    log_id(*control, "operation", uploaded.id);
                    if (!folder_store_lifetime(control)) {
                        log_id(*control, "operation", uploaded.id, true);
                        throw Interrupted{};
                    }
                }
                require_result(uploaded, "Note “" + item.document.note_name + "”");
                item.remote_id = uploaded.id;
                item.done = uploaded.done;
            }
            items.push_back(std::move(item));
            guarded(*this, control, [&] { ++control->completed; });
        }
        guarded(*this, control, [&] {
            control->phase = FolderStorePhase::indexing;
            control->completed = static_cast<std::int64_t>(std::count_if(items.begin(), items.end(), [](const auto& item) { return item.document.note_id > 0 && item.done; }));
        });
        int round = 0;
        for (;;) {
            bool pending = false;
            for (auto& item : items) {
                if (item.done) continue;
                key = current_key(*this, control, true);
                const auto result = control->service == FolderStoreService::openai
                    ? client.openai_inspect_file(created.id, item.remote_id, key, *control->cancel_forward)
                    : client.google_inspect_operation(item.remote_id, key, *control->cancel_forward);
                require_result(result, item.document.note_id > 0 ? "Note “" + item.document.note_name + "”" : "Store metadata");
                item.done = control->service == FolderStoreService::openai ? result.status == "completed" : result.done;
                if (item.done && item.document.note_id > 0) guarded(*this, control, [&] { ++control->completed; });
                if (!item.done) pending = true;
            }
            if (!pending && control->service == FolderStoreService::google) {
                key = current_key(*this, control, true);
                const auto counts = client.google_inspect_store(created.id, key, *control->cancel_forward);
                require_result(counts);
                if (counts.failed || counts.active > static_cast<std::int64_t>(items.size())) throw FolderStoreError("Google document counts do not match the snapshot.");
                pending = counts.pending != 0 || counts.active < static_cast<std::int64_t>(items.size());
            }
            if (!pending) break;
            wait_round(*this, control, round++);
        }
        guarded(*this, control, [&] {
            if (control->cancel_forward->load()) throw Cancelled{};
            // This Event is the only record of the new store in CHA.
            store->append_event(EventSeverity::success, "Uploaded " + std::to_string(control->total)
                + " notes from " + context(*control) + resource_ids(*control) + ".");
            control->terminal = true;
            control->phase = FolderStorePhase::completed;
            control->result = "success";
        });
        return;
    } catch (const Interrupted&) { return; }
    catch (...) {
        bool cancelled = false;
        std::string message;
        try { throw; }
        catch (const Cancelled&) { cancelled = true; }
        catch (const FolderStoreError& error) { message = error.what(); }
        catch (const std::bad_alloc&) { message = "Could not allocate memory for the folder snapshot."; }
        catch (...) { message = "The folder store operation failed."; }
        try {
            guarded(*this, control, [] {});
            if (remote_started) {
                if (!cleanup(*this, control, client)) message += guarded(*this, control, [&] { return " " + control->error; });
                guarded(*this, control, [&] {
                    if (!control->unknown_allocation) return;
                    control->warned = true;
                    const auto warning = unknown_warning(*control);
                    log_warn(warning);
                    record_event(*this, EventSeverity::warning, warning);
                });
            }
            const auto state = guarded(*this, control, [&] { return status_from(*control); });
            const bool warned = guarded(*this, control, [&] { return control->warned; });
            const auto result = cancelled ? (state.known_cleanup_remains ? "error" : warned ? "warning" : "cancelled")
                : remote_started ? "error" : "preflight";
            finish(*this, control, result, message);
        } catch (const Interrupted&) { return; }
        catch (...) {
            try {
                const auto ids = guarded(*this, control, [&] {
                    control->known_cleanup_remains = control->store_id || !control->file_ids.empty();
                    return resource_ids(*control);
                });
                const auto message = "Cleanup could not be recorded. Remote resources may remain; see Events and logs.";
                log_warn(std::string(message) + ids);
                guarded(*this, control, [&] { record_event(*this, EventSeverity::warning, std::string(message) + ids); });
                finish(*this, control, "error", message);
            }
            catch (...) {}
        }
    }
}

void NotesService::Impl::run_folder_store_cleanup(std::shared_ptr<FolderStoreControl> control) {
    try {
        if (settings.test_hooks.folder_store_worker_started) settings.test_hooks.folder_store_worker_started(control->operation_id, control->cancel_all);
        const auto transport = guarded(*this, control, [&] { return folder_store_transport; });
        FolderStoreClient client(transport);
        const bool ok = cleanup(*this, control, client);
        const auto [warned, error] = guarded(*this, control, [&] { return std::pair{control->warned, control->error}; });
        // A finished cleanup closes the dialog like a finished upload.
        finish(*this, control, ok ? (warned ? "warning" : "success") : "error", ok ? "" : error);
    } catch (const Interrupted&) {}
    catch (...) {
        try { finish(*this, control, "error", "The cleanup failed. Remote resources may remain; see Events and logs."); }
        catch (...) {}
    }
}

bool NotesService::Impl::launch_folder_store_control(const std::shared_ptr<FolderStoreControl>& control, std::function<void()> work) {
    try {
        if (settings.test_hooks.folder_store_before_launch) settings.test_hooks.folder_store_before_launch();
        if (background_jobs.launch([this, control, work = std::move(work)](std::atomic_bool&) {
                work();
                if (settings.test_hooks.folder_store_worker_finished) settings.test_hooks.folder_store_worker_finished(control->operation_id);
            }, control->cancel_all, [control] {
                const std::lock_guard lock(control->wait_mutex);
                control->cancel_forward->store(true);
                control->wait_cv.notify_all();
            })) return true;
    } catch (...) {}
    return false;
}

void NotesService::Impl::interrupt_folder_stores() {
    for (const auto& [id, control] : folder_store_controls) {
        const std::lock_guard lock(control->wait_mutex);
        control->cancel_all->store(true);
        control->cancel_forward->store(true);
        control->wait_cv.notify_all();
    }
    folder_store_controls.clear();
}

std::vector<FolderStoreChoiceView> NotesService::folder_store_choices(std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    std::vector<FolderStoreChoiceView> views;
    for (const auto& key : impl_->api_keys->list()) {
        if (!key.has_value) continue;
        for (const auto service : {FolderStoreService::openai, FolderStoreService::google}) {
            views.push_back({.service = std::string(folder_store_service_name(service)),
                .api_key_id = key.id, .api_key_name = key.display_name});
        }
    }
    return views;
}

FolderStoreStart NotesService::start_folder_store_upload(std::int64_t folder_id,
    std::string service, std::string api_key_id, std::string name, std::string description, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    FolderStoreService parsed;
    try { parsed = parse_folder_store_service(service); }
    catch (const FolderStoreError& error) { throw ApplicationError(ErrorCode::invalid_argument, error.what()); }
    // Validate UTF-8 before counting or sending any user metadata to a provider.
    const auto code_points = [](std::string_view value, std::string_view field) {
        try { (void)nlohmann::json(value).dump(); }
        catch (const nlohmann::json::type_error&) {
            throw ApplicationError(ErrorCode::invalid_argument, std::string(field) + " must be valid UTF-8.");
        }
        return std::count_if(value.begin(), value.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    };
    name = std::string(trim_view(name));
    const auto name_points = code_points(name, "Name");
    const auto description_points = code_points(description, "Description");
    if (name.empty()) throw ApplicationError(ErrorCode::invalid_argument, "Enter a store name.");
    if (parsed == FolderStoreService::google && name.size() > 512)
        throw ApplicationError(ErrorCode::invalid_argument, "Name is too long for Google. Use a shorter name.");
    if (parsed == FolderStoreService::openai && name_points > 512)
        throw ApplicationError(ErrorCode::invalid_argument, "Name must be 512 characters or fewer.");
    if (parsed == FolderStoreService::openai && description_points > 512)
        throw ApplicationError(ErrorCode::invalid_argument, "Description must be 512 characters or fewer for OpenAI.");
    if (folder_id <= 0 || folder_id > note_id_limit) throw ApplicationError(ErrorCode::invalid_argument, "A store can only belong to a real folder.");
    const auto folder = impl_->store->folder_store_folder(folder_id);
    if (!folder) throw ApplicationError(ErrorCode::invalid_argument, "A store can only belong to a real folder.");
    if (folder->forum_id) throw ApplicationError(ErrorCode::invalid_argument, "This folder is managed by CHA.");
    if (impl_->folder_store_reserved(folder_id)) throw ApplicationError(ErrorCode::invalid_argument, "A store operation is already running for this folder.");
    auto control = std::make_shared<Impl::FolderStoreControl>();
    control->operation_id = impl_->next_folder_store_operation++;
    control->epoch = epoch;
    control->root_id = folder_id;
    control->folder_name = folder->name;
    control->vault_name = impl_->current_vault_name();
    control->service = parsed;
    control->api_key_id = std::move(api_key_id);
    control->store_name = std::move(name);
    control->store_description = std::move(description);
    impl_->folder_store_controls[folder_id] = control;
    if (!impl_->launch_folder_store_control(control, [owner = impl_.get(), control] { owner->run_folder_store_upload(control); })) {
        impl_->release_folder_store_control_locked(control);
        throw ApplicationError(ErrorCode::operation_cancelled, "The operation could not be started.");
    }
    return {.operation_id = control->operation_id};
}

FolderStoreStart NotesService::retry_folder_store_cleanup(std::int64_t operation_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    const auto control = impl_->folder_store_operation(operation_id);
    if (!control || !control->terminal || !control->known_cleanup_remains)
        throw ApplicationError(ErrorCode::invalid_argument, "This store cleanup cannot be retried.");
    control->terminal = false;
    control->error.clear();
    control->result.clear();
    control->phase = Impl::FolderStorePhase::cleaning;
    if (!impl_->launch_folder_store_control(control, [owner = impl_.get(), control] { owner->run_folder_store_cleanup(control); })) {
        control->terminal = true;
        control->phase = Impl::FolderStorePhase::failed;
        control->result = "error";
        control->error = "The cleanup retry could not be started. Retry or close this operation.";
        throw ApplicationError(ErrorCode::operation_cancelled, "The operation could not be started.");
    }
    return {.operation_id = control->operation_id};
}

FolderStoreOperationStatus NotesService::folder_store_operation_status(std::int64_t folder_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto found = impl_->folder_store_controls.find(folder_id);
    if (found == impl_->folder_store_controls.end()) return {};
    return status_from(*found->second);
}

void NotesService::cancel_folder_store_upload(std::int64_t operation_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    const auto control = impl_->folder_store_operation(operation_id);
    if (!control) throw ApplicationError(ErrorCode::invalid_argument, "That store operation is no longer current.");
    if (control->terminal) return;
    const std::lock_guard wait_lock(control->wait_mutex);
    control->cancel_forward->store(true);
    control->wait_cv.notify_all();
}

void NotesService::close_folder_store_operation(std::int64_t operation_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    const auto control = impl_->folder_store_operation(operation_id);
    if (!control) throw ApplicationError(ErrorCode::invalid_argument, "That store operation is no longer current.");
    if (!control->terminal) throw ApplicationError(ErrorCode::invalid_argument, "The operation is still running.");
    // Remaining resource IDs are already in Events and the log.
    impl_->release_folder_store_control_locked(control);
}

void NotesService::set_folder_store_transport_for_tests(FolderStoreTransport transport) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->folder_store_transport = std::move(transport);
}

void NotesService::interrupt_folder_stores_locked() {
    impl_->interrupt_folder_stores();
}

} // namespace cha::app
