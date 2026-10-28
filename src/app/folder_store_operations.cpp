#include "app/application_internal.h"

#include "util/logging.h"
#include "workspace/builtins.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <mutex>
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

bool reserved(const auto& control) { return !control.terminal || control.delete_failed; }

template<typename Phase>
std::string phase_name(Phase phase) {
    switch (phase) {
    case Phase::preparing: return "preparing";
    case Phase::uploading: return "uploading";
    case Phase::indexing: return "indexing";
    case Phase::cleaning: return "cleaning";
    case Phase::removing: return "removing";
    case Phase::completed: return "completed";
    case Phase::failed: return "failed";
    case Phase::cancelled: return "cancelled";
    case Phase::warning: return "warning";
    }
    return "failed";
}

template<typename Kind>
std::string kind_name(Kind kind) {
    switch (kind) {
    case Kind::upload: return "upload";
    case Kind::remove: return "remove";
    case Kind::folder_delete: return "folder_delete";
    }
    return "upload";
}

Application::FolderStoreOperationStatus status_from(const auto& control) {
    return {.present = true, .operation_id = control.operation_id,
        .kind = kind_name(control.kind), .phase = phase_name(control.phase),
        .completed = control.completed, .total = control.total,
        .error = control.error, .result = control.result,
        .known_cleanup_remains = control.known_cleanup_remains,
        .folder_deleted = control.folder_deleted, .terminal = control.terminal};
}

// All application/store access by a worker goes through this guard. Network
// calls use copied inputs after the guard releases the lifecycle lock.
decltype(auto) guarded(auto& self, const auto& control, auto&& work) {
    if (self.settings.folder_store_before_lifetime_lock) self.settings.folder_store_before_lifetime_lock();
    const std::lock_guard lock(self.lifecycle_mutex);
    if (!self.folder_store_lifetime_locked(control, control->store_record_id)) throw Interrupted{};
    return work();
}

std::string context(const FolderStoreRecord& record) {
    return "folder “" + record.folder_name + "” (" + std::to_string(record.folder_id)
        + "), service=" + std::string(folder_store_service_name(record.service))
        + " key=" + record.api_key_id + " store_record=" + std::to_string(record.id);
}

std::string resource_ids(const FolderStoreRecord& record) {
    std::string result;
    if (record.remote_store_id) result += " store=" + *record.remote_store_id;
    for (const auto& id : record.file_ids) result += " file=" + id;
    if (record.remote_outcome_unknown) result += " unknown-allocation";
    return result;
}

std::string unknown_warning(const FolderStoreRecord& record) {
    return "Unconfirmed allocation for " + context(record) + resource_ids(record)
        + ". Remote data may remain. Check the original provider project for manual cleanup. "
          "An ID lost by both storage and logging cannot be recovered here.";
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
            throw FolderStoreError("The saved API key '" + control->api_key_id + "' is missing or empty.");
        }
        return self.api_keys->value(control->api_key_id);
    });
}

void wait_round(auto& self, const auto& control, int round) {
    if (self.settings.folder_store_wait) {
        self.settings.folder_store_wait(round);
        return;
    }
    std::unique_lock lock(control->wait_mutex);
    control->wait_cv.wait_for(lock, self.settings.folder_store_poll_interval, [&] {
        const bool cancelled = control->cancel_forward->load() || control->cancel_all->load();
        if (self.settings.folder_store_poll_predicate_read) self.settings.folder_store_poll_predicate_read();
        return cancelled;
    });
}

void finish(auto& self, const auto& control, std::string result, std::string error = {}, bool failed_delete = false) {
    guarded(self, control, [&] {
        using Phase = decltype(control->phase);
        control->phase = result == "warning" ? Phase::warning
            : result == "cancelled" ? Phase::cancelled
            : result == "success" || result == "folder_deleted" ? Phase::completed : Phase::failed;
        control->result = std::move(result);
        control->error = std::move(error);
        control->terminal = true;
        control->delete_failed = failed_delete;
        // Keep the terminal control for status/Close, even after its folder is gone.
    });
}

void require_result(const FolderStoreRemoteResult& result, std::string_view note = {}) {
    if (result.kind == FolderStoreRemoteKind::ok && !result.operation_error) return;
    if (result.kind == FolderStoreRemoteKind::cancelled) throw Cancelled{};
    throw FolderStoreError((note.empty() ? std::string{} : "Note “" + std::string(note) + "”: ")
        + (result.message.empty() ? "The provider request failed." : result.message));
}

void persist_id(auto& self, const auto& control, std::string id, bool file) {
    log_id(*control, file ? "file" : "store", id);
    try {
        guarded(self, control, [&] {
            // Retain the returned ID even if its transaction fails.
            if (file) control->unsaved_file_ids.push_back(id);
            else control->unsaved_store_id = id;
            if (file) self.store->add_folder_store_file_id(*control->store_record_id, id);
            else self.store->save_folder_store_remote_id(*control->store_record_id, id);
            if (file) control->unsaved_file_ids.pop_back();
            else control->unsaved_store_id.reset();
        });
    } catch (const Interrupted&) {
        log_id(*control, file ? "file" : "store", id, true);
        throw;
    }
}

// Caller holds the lifecycle lock. Retain each unsaved ID until its write succeeds.
void save_unsaved_ids(WorkspaceConfigStore& store, auto& control, std::int64_t record_id) {
    if (control.unsaved_store_id) {
        store.save_folder_store_remote_id(record_id, *control.unsaved_store_id);
        control.unsaved_store_id.reset();
    }
    while (!control.unsaved_file_ids.empty()) {
        store.add_folder_store_file_id(record_id, control.unsaved_file_ids.back());
        control.unsaved_file_ids.pop_back();
    }
}

// The same routine serves Remove, failed uploads, cancellation and subtree delete.
// Each known resource is attempted once; only explicit user work retries failures.
bool cleanup(auto& self, const auto& control, FolderStoreClient& client, FolderStoreRecord record, bool finalize) {
    guarded(self, control, [&] {
        using Phase = decltype(control->phase);
        control->phase = kind_name(control->kind) == "upload" ? Phase::cleaning : Phase::removing;
        // Reuse finite Events history to keep earlier not-found outcomes visible
        // across an explicit retry or reopen. No second warning store is needed.
        const auto marker = "store_record=" + std::to_string(record.id);
        for (const auto& event : self.store->list_events()) {
            const auto at = event.message.find(marker);
            const auto end = at == std::string::npos ? 0 : at + marker.size();
            if (at != std::string::npos && (end == event.message.size() || event.message[end] < '0' || event.message[end] > '9')
                && event.message.find("was not found for") != std::string::npos) control->warned = true;
        }
        if (control->unsaved_store_id) record.remote_store_id = control->unsaved_store_id;
        for (const auto& id : control->unsaved_file_ids) {
            if (std::find(record.file_ids.begin(), record.file_ids.end(), id) == record.file_ids.end()) record.file_ids.push_back(id);
        }
    });
    bool ok = true;
    std::string errors;
    const auto remove = [&](const std::string& id, bool store_resource) {
        try {
            const auto key = current_key(self, control, false);
            const auto result = record.service == FolderStoreService::google
                ? client.google_delete_store(id, key, *control->cancel_all)
                : store_resource ? client.openai_delete_store(id, key, *control->cancel_all)
                                 : client.openai_delete_file(id, key, *control->cancel_all);
            guarded(self, control, [&] {
                const bool not_found = result.kind == FolderStoreRemoteKind::not_found;
                if (result.kind != FolderStoreRemoteKind::ok && !not_found) {
                    throw FolderStoreError(result.message.empty() ? "Remote cleanup failed." : result.message);
                }
                const auto warning = not_found
                    ? "Resource " + id + " was not found for " + context(record)
                        + ". The saved key may refer to another project. Manual cleanup may still be needed."
                    : std::string{};
                if (not_found) log_warn(warning);
                if (store_resource) self.store->clear_folder_store_remote_id(record.id, warning);
                else self.store->remove_folder_store_file_id(record.id, id, warning);
                if (not_found) control->warned = true;
                if (store_resource && control->unsaved_store_id == id) control->unsaved_store_id.reset();
                if (!store_resource) std::erase(control->unsaved_file_ids, id);
            });
        } catch (const Interrupted&) { throw; }
        catch (const FolderStoreError& error) {
            ok = false;
            errors += " " + std::string(error.what()) + " Resource=" + id + ".";
        }
        catch (const std::exception&) {
            ok = false;
            errors += " Could not confirm cleanup of " + id + ".";
        }
    };
    if (record.remote_store_id) remove(*record.remote_store_id, true);
    for (const auto& id : record.file_ids) remove(id, false);
    return guarded(self, control, [&] {
        // A failed cleanup must keep learned IDs available to a later explicit
        // Remove. Retry only local bookkeeping; never replay an allocation.
        try {
            save_unsaved_ids(*self.store, *control, record.id);
        } catch (...) { log_warn("Folder store ownership bookkeeping still failed; returned IDs remain in the operation and logs."); }
        auto latest = self.store->get_folder_store(record.id);
        if (!latest) throw FolderStoreError("That folder store was not found.");
        control->known_cleanup_remains = !ok || latest->remote_store_id || !latest->file_ids.empty()
            || control->unsaved_store_id || !control->unsaved_file_ids.empty();
        if (control->known_cleanup_remains) {
            auto remaining = *latest;
            if (control->unsaved_store_id) remaining.remote_store_id = control->unsaved_store_id;
            for (const auto& id : control->unsaved_file_ids) {
                if (std::find(remaining.file_ids.begin(), remaining.file_ids.end(), id) == remaining.file_ids.end()) remaining.file_ids.push_back(id);
            }
            control->error = context(remaining) + errors + resource_ids(remaining);
            log_warn(control->error);
            try { self.store->append_event(EventSeverity::warning, control->error); }
            catch (...) { log_warn("Folder store cleanup warning could not be recorded."); }
            return false;
        }
        // If the only uncertain allocation returned an ID that failed to save,
        // confirmed cleanup still cannot prove the original storage commit. Keep
        // the precommitted uncertainty warning instead of claiming full deletion.
        if (latest->remote_outcome_unknown) control->warned = true;
        if (finalize) {
            self.store->delete_folder_store_record(record.id,
                latest->remote_outcome_unknown ? unknown_warning(*latest)
                    : "Removed the local store association for " + context(*latest)
                        + ". Cleanup of known resources finished; earlier Events may contain warnings.",
                control->warned ? EventSeverity::warning : EventSeverity::success);
            control->store_record_id.reset();
        }
        return true;
    });
}

struct WorkItem {
    FolderStoreDocument document;
    std::string remote_id;
    bool done{};
};

} // namespace

bool Application::Impl::folder_store_reserved(std::int64_t folder_id) {
    const auto found = folder_store_controls.find(folder_id);
    return found != folder_store_controls.end() && reserved(*found->second);
}

bool Application::Impl::folder_in_delete_subtree(std::int64_t folder_id) {
    if (std::none_of(folder_store_controls.begin(), folder_store_controls.end(), [](const auto& entry) {
        return entry.second->kind == FolderStoreOpKind::folder_delete && reserved(*entry.second);
    })) return false;
    const auto items = store->list_note_items();
    for (const auto& [id, control] : folder_store_controls) {
        if (control->kind == FolderStoreOpKind::folder_delete && reserved(*control)
            && in_subtree(items, folder_id, id)) return true;
    }
    return false;
}

bool Application::Impl::folder_store_blocks_delete(std::int64_t item_id) {
    if (std::none_of(folder_store_controls.begin(), folder_store_controls.end(), [](const auto& entry) {
        return reserved(*entry.second);
    })) return false;
    const auto items = store->list_note_items();
    for (const auto& [id, control] : folder_store_controls) {
        if (!reserved(*control)) continue;
        if (in_subtree(items, id, item_id)
            || (control->kind == FolderStoreOpKind::folder_delete && in_subtree(items, item_id, id))) return true;
    }
    return false;
}

std::shared_ptr<Application::Impl::FolderStoreControl> Application::Impl::folder_store_operation(std::int64_t operation_id) {
    for (const auto& [id, control] : folder_store_controls) {
        if (control->operation_id == operation_id) return control;
    }
    return {};
}

void Application::Impl::release_folder_store_control_locked(const std::shared_ptr<FolderStoreControl>& control) {
    const auto found = folder_store_controls.find(control->root_id);
    if (found != folder_store_controls.end() && found->second == control) folder_store_controls.erase(found);
}

bool Application::Impl::folder_store_lifetime_locked(const std::shared_ptr<FolderStoreControl>& control,
    std::optional<std::int64_t> store_id) {
    if (stopping_flag.load() || unusable.load() || admit_locked(control->epoch) || control->cancel_all->load()) return false;
    if (folder_store_operation(control->operation_id) != control) return false;
    store->require_main();
    if (store_id) {
        if (control->store_record_id != store_id) return false;
        const auto record = store->get_folder_store(*store_id);
        if (!record) return false;
    }
    return true;
}

bool Application::Impl::folder_store_lifetime(const std::shared_ptr<FolderStoreControl>& control,
    std::optional<std::int64_t> store_id) {
    const std::lock_guard lock(lifecycle_mutex);
    return folder_store_lifetime_locked(control, store_id);
}

void Application::Impl::run_folder_store_upload(std::shared_ptr<FolderStoreControl> control) {
    bool registered = false;
    bool remote_started = false;
    FolderStoreClient client;
    try {
        if (settings.folder_store_worker_started) settings.folder_store_worker_started(control->operation_id, control->cancel_all);
        auto capture = guarded(*this, control, [&] {
            if (control->cancel_forward->load()) throw Cancelled{};
            const auto snapshot = store->snapshot();
            std::vector<ModelBackendConfig> configs;
            for (const auto& provider : snapshot->providers()) configs.push_back(provider.config);
            const auto choices = folder_store_choices_from_providers(configs, *api_keys);
            if (std::none_of(choices.begin(), choices.end(), [&](const auto& choice) {
                    return choice.service == control->service && choice.api_key_id == control->api_key_id;
                })) throw FolderStoreError("The selected service and saved API key are not eligible.");
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
            control->store_record_id = store->register_folder_store(control->root_id,
                control->service, control->api_key_id, control->total);
            registered = true;
            control->phase = FolderStorePhase::uploading;
        });
        auto key = current_key(*this, control, true);
        guarded(*this, control, [&] {
            if (control->cancel_forward->load()) throw Cancelled{};
            store->set_folder_store_uncertainty(*control->store_record_id, true);
            remote_started = true;
        });
        const auto name = capture.folder.name + " " + note_item_ref_id(capture.folder.id);
        const auto created = control->service == FolderStoreService::openai
            ? client.openai_create_store(name, key, *control->cancel_all)
            : client.google_create_store(name, key, *control->cancel_all);
        if (!created.id.empty()) persist_id(*this, control, created.id, false);
        else guarded(*this, control, [&] {
            if (!created.unknown_allocation) store->set_folder_store_uncertainty(*control->store_record_id, false);
        });
        require_result(created);
        std::vector<WorkItem> items;
        items.reserve(capture.documents.size());
        for (auto& document : capture.documents) {
            key = current_key(*this, control, true);
            WorkItem item{.document = std::move(document)};
            if (control->service == FolderStoreService::openai) {
                guarded(*this, control, [&] {
                    if (control->cancel_forward->load()) throw Cancelled{};
                    store->set_folder_store_uncertainty(*control->store_record_id, true);
                });
                const auto uploaded = client.openai_upload_file(item.document.filename, item.document.markdown, key, *control->cancel_all);
                if (!uploaded.id.empty()) persist_id(*this, control, uploaded.id, true);
                else guarded(*this, control, [&] {
                    if (!uploaded.unknown_allocation) store->set_folder_store_uncertainty(*control->store_record_id, false);
                });
                require_result(uploaded, item.document.note_name);
                item.remote_id = uploaded.id;
                key = current_key(*this, control, true);
                const auto attached = client.openai_attach_file(created.id, uploaded.id, key, *control->cancel_forward);
                require_result(attached, item.document.note_name);
                if (attached.status == "failed" || attached.status == "cancelled") throw FolderStoreError("Could not index note “" + item.document.note_name + "”.");
                item.done = attached.status == "completed";
            } else {
                const auto uploaded = client.google_upload_document(created.id, item.document.filename,
                    item.document.markdown, key, *control->cancel_all);
                if (!uploaded.id.empty()) {
                    log_id(*control, "operation", uploaded.id);
                    if (!folder_store_lifetime(control, control->store_record_id)) {
                        log_id(*control, "operation", uploaded.id, true);
                        throw Interrupted{};
                    }
                }
                require_result(uploaded, item.document.note_name);
                item.remote_id = uploaded.id;
                item.done = uploaded.done;
            }
            items.push_back(std::move(item));
            guarded(*this, control, [&] { ++control->completed; });
        }
        guarded(*this, control, [&] {
            control->phase = FolderStorePhase::indexing;
            control->completed = static_cast<std::int64_t>(std::count_if(items.begin(), items.end(), [](const auto& item) { return item.done; }));
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
                require_result(result, item.document.note_name);
                item.done = control->service == FolderStoreService::openai ? result.status == "completed" : result.done;
                if (item.done) guarded(*this, control, [&] { ++control->completed; });
                else pending = true;
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
            store->mark_folder_store_ready(*control->store_record_id,
                "Uploaded " + std::to_string(control->total) + " notes from folder “" + control->folder_name
                    + "” (" + std::to_string(control->root_id) + "), service="
                    + std::string(folder_store_service_name(control->service)) + " key=" + control->api_key_id + ".");
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
            if (registered) {
                const auto record = guarded(*this, control, [&] { return store->get_folder_store(*control->store_record_id); });
                if (record) {
                    const bool cleaned = cleanup(*this, control, client, *record, true);
                    if (!cleaned) message += guarded(*this, control, [&] { return " " + control->error; });
                }
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
                    std::string ids;
                    if (control->unsaved_store_id) ids += " store=" + *control->unsaved_store_id;
                    for (const auto& id : control->unsaved_file_ids) ids += " file=" + id;
                    if (control->store_record_id) {
                        if (const auto record = store->get_folder_store(*control->store_record_id)) {
                            ids += resource_ids(*record);
                            control->known_cleanup_remains = record->remote_store_id || !record->file_ids.empty();
                        }
                    }
                    control->known_cleanup_remains = control->known_cleanup_remains
                        || control->unsaved_store_id || !control->unsaved_file_ids.empty();
                    return ids;
                });
                finish(*this, control, "error", "Cleanup could not be recorded. Remote resources may remain; see Events and logs." + ids);
            }
            catch (...) {}
        }
    }
}

void Application::Impl::run_folder_store_remove(std::shared_ptr<FolderStoreControl> control) {
    try {
        if (settings.folder_store_worker_started) settings.folder_store_worker_started(control->operation_id, control->cancel_all);
        const auto record = guarded(*this, control, [&] {
            store->mark_folder_store_incomplete(*control->store_record_id);
            return *store->get_folder_store(*control->store_record_id);
        });
        const auto transport = guarded(*this, control, [&] { return folder_store_transport; });
        FolderStoreClient client(transport);
        const bool ok = cleanup(*this, control, client, record, true);
        const auto status = guarded(*this, control, [&] { return status_from(*control); });
        const bool warned = guarded(*this, control, [&] { return control->warned; });
        finish(*this, control, ok ? (warned ? "warning" : "success") : "error", ok ? "" : status.error);
    } catch (const Interrupted&) {}
    catch (...) {
        try { finish(*this, control, "error", "The folder store could not be removed. Known resources remain recorded."); }
        catch (...) {}
    }
}

void Application::Impl::retire_deleted_note_work(const std::vector<std::int64_t>& note_ids, std::uint64_t epoch) {
    for (const auto id : note_ids) {
        if (const auto found = compressions.find(id); found != compressions.end()) {
            const auto control = found->second;
            control->cancel->store(true);
            retire_compression(id, control, epoch, EventSeverity::info, "Compression cancelled: note deleted.");
        }
    }
    interrupt_abandoned_audio();
}

// Caller owns lifecycle_mutex. Capture folder IDs and compression targets before
// the commit; deleted rows cannot be queried afterwards.
void delete_locally(auto& self, const auto& control, const std::vector<std::string>& warnings,
    const std::vector<std::int64_t>& store_ids) {
    const auto folders = self.store->folder_ids_in_subtree(control->root_id);
    const auto items = self.store->list_note_items();
    std::vector<std::int64_t> notes;
    for (const auto& item : items) if (in_subtree(items, item.id, control->root_id)) notes.push_back(item.id);
    self.store->delete_folder_subtree_locally(control->root_id, warnings, store_ids);
    control->folder_deleted = true;
    self.retire_deleted_note_work(notes, control->epoch);
    try {
        if (!self.live_sessions->post_welcome_folder_prune(control->epoch, folders)) log_warn("Could not enqueue Welcome folder prune.");
    } catch (...) { log_warn("Could not enqueue Welcome folder prune after folder deletion committed."); }
}

void Application::Impl::run_folder_store_delete(std::shared_ptr<FolderStoreControl> control, bool retry) {
    try {
        if (settings.folder_store_worker_started) settings.folder_store_worker_started(control->operation_id, control->cancel_all);
        const auto records = guarded(*this, control, [&] {
            if (!retry) store->mark_folder_stores_incomplete_in_subtree(control->root_id);
            auto records = store->folder_stores_in_subtree(control->root_id);
            control->total = static_cast<std::int64_t>(records.size());
            control->completed = 0;
            return records;
        });
        const auto transport = guarded(*this, control, [&] { return folder_store_transport; });
        FolderStoreClient client(transport);
        bool failed = false;
        std::string failures;
        for (const auto& record : records) {
            guarded(*this, control, [&] {
                control->store_record_id = record.id;
                control->service = record.service;
                control->api_key_id = record.api_key_id;
            });
            const bool ok = cleanup(*this, control, client, record, false);
            guarded(*this, control, [&] {
                auto latest = store->get_folder_store(record.id);
                if (ok && latest && !latest->remote_outcome_unknown) {
                    store->delete_folder_store_record(record.id, "Removed local store association for " + context(*latest)
                        + ". Cleanup of known resources finished; earlier Events may contain warnings.");
                }
                control->store_record_id.reset();
                if (ok) ++control->completed;
            });
            if (!ok) failures += guarded(*this, control, [&] { return control->error + "\n"; });
            failed = failed || !ok;
        }
        if (failed) {
            const auto error = guarded(*this, control, [&] {
                control->known_cleanup_remains = true;
                return failures;
            });
            finish(*this, control, "error", error, true);
            return;
        }
        guarded(*this, control, [&] {
            const auto remaining = store->folder_stores_in_subtree(control->root_id);
            std::vector<std::string> warnings;
            std::vector<std::int64_t> ids;
            for (const auto& record : remaining) {
                ids.push_back(record.id);
                if (record.remote_outcome_unknown) warnings.push_back(unknown_warning(record));
            }
            const bool warning = !warnings.empty() || control->warned;
            delete_locally(*this, control, warnings, ids);
            control->known_cleanup_remains = false;
            control->terminal = true;
            control->phase = warning ? FolderStorePhase::warning : FolderStorePhase::completed;
            control->result = warning ? "warning" : "folder_deleted";
        });
    } catch (const Interrupted&) {}
    catch (...) {
        try { finish(*this, control, "error", "The folder could not be deleted. Notes and remaining ownership were retained.", true); }
        catch (...) {}
    }
}

bool Application::Impl::launch_folder_store_control(const std::shared_ptr<FolderStoreControl>& control, std::function<void()> work) {
    try {
        if (settings.folder_store_before_launch) settings.folder_store_before_launch();
        if (background_jobs.launch([this, control, work = std::move(work)](std::atomic_bool&) {
                work();
                if (settings.folder_store_worker_finished) settings.folder_store_worker_finished(control->operation_id);
            }, control->cancel_all, [control] {
                const std::lock_guard lock(control->wait_mutex);
                control->cancel_forward->store(true);
                control->wait_cv.notify_all();
            })) return true;
    } catch (...) {}
    return false;
}

void Application::Impl::interrupt_folder_stores() {
    for (const auto& [id, control] : folder_store_controls) {
        const std::lock_guard lock(control->wait_mutex);
        control->cancel_all->store(true);
        control->cancel_forward->store(true);
        control->wait_cv.notify_all();
    }
    folder_store_controls.clear();
}

std::vector<Application::FolderStoreChoiceView> Application::folder_store_choices(std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto snapshot = impl_->store->snapshot();
    std::vector<ModelBackendConfig> configs;
    for (const auto& provider : snapshot->providers()) configs.push_back(provider.config);
    std::vector<FolderStoreChoiceView> views;
    for (const auto& choice : folder_store_choices_from_providers(configs, *impl_->api_keys)) {
        views.push_back({.service = std::string(folder_store_service_name(choice.service)),
            .api_key_id = choice.api_key_id, .api_key_name = choice.api_key_name});
    }
    return views;
}

Application::FolderStoreStart Application::start_folder_store_upload(std::int64_t folder_id,
    std::string service, std::string api_key_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    FolderStoreService parsed;
    try { parsed = parse_folder_store_service(service); }
    catch (const FolderStoreError& error) { throw ApplicationError(ErrorCode::invalid_argument, error.what()); }
    if (folder_id <= 0 || folder_id > note_id_limit) throw ApplicationError(ErrorCode::invalid_argument, "A store can only belong to a real folder.");
    const auto folder = impl_->store->folder_store_folder(folder_id);
    if (!folder) throw ApplicationError(ErrorCode::invalid_argument, "A store can only belong to a real folder.");
    if (impl_->store->get_folder_store_by_folder(folder_id)) throw ApplicationError(ErrorCode::invalid_argument, "This folder already has a store.");
    if (impl_->folder_store_reserved(folder_id) || impl_->folder_in_delete_subtree(folder_id)) throw ApplicationError(ErrorCode::invalid_argument, "A store operation is already running for this folder.");
    auto control = std::make_shared<Impl::FolderStoreControl>();
    control->operation_id = impl_->next_folder_store_operation++;
    control->epoch = epoch;
    control->kind = Impl::FolderStoreOpKind::upload;
    control->root_id = folder_id;
    control->folder_name = folder->name;
    control->vault_name = impl_->current_vault_.get().name;
    control->service = parsed;
    control->api_key_id = std::move(api_key_id);
    impl_->folder_store_controls[folder_id] = control;
    if (!impl_->launch_folder_store_control(control, [owner = impl_.get(), control] { owner->run_folder_store_upload(control); })) {
        impl_->release_folder_store_control_locked(control);
        throw ApplicationError(ErrorCode::operation_cancelled, "The operation could not be started.");
    }
    return {.operation_id = control->operation_id};
}

Application::FolderStoreStart Application::start_folder_store_remove(std::int64_t folder_id,
    std::int64_t expected_store_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    const auto record = impl_->store->get_folder_store_by_folder(folder_id);
    if (!record || record->id != expected_store_id) throw ApplicationError(ErrorCode::invalid_argument, "That folder store was not found.");
    if (impl_->folder_store_reserved(folder_id) || impl_->folder_in_delete_subtree(folder_id)) throw ApplicationError(ErrorCode::invalid_argument, "A store operation is already running for this folder.");
    auto control = std::make_shared<Impl::FolderStoreControl>();
    control->operation_id = impl_->next_folder_store_operation++;
    control->epoch = epoch;
    control->kind = Impl::FolderStoreOpKind::remove;
    control->root_id = folder_id;
    control->store_record_id = expected_store_id;
    control->folder_name = record->folder_name;
    control->vault_name = impl_->current_vault_.get().name;
    control->service = record->service;
    control->api_key_id = record->api_key_id;
    control->phase = Impl::FolderStorePhase::removing;
    if (const auto previous = impl_->folder_store_controls.find(folder_id);
        previous != impl_->folder_store_controls.end() && previous->second->store_record_id == expected_store_id) {
        control->unsaved_store_id = previous->second->unsaved_store_id;
        control->unsaved_file_ids = previous->second->unsaved_file_ids;
    }
    impl_->folder_store_controls[folder_id] = control;
    if (!impl_->launch_folder_store_control(control, [owner = impl_.get(), control] { owner->run_folder_store_remove(control); })) {
        impl_->release_folder_store_control_locked(control);
        throw ApplicationError(ErrorCode::operation_cancelled, "The operation could not be started.");
    }
    return {.operation_id = control->operation_id};
}

Application::FolderStoreOperationStatus Application::folder_store_operation_status(std::int64_t folder_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto found = impl_->folder_store_controls.find(folder_id);
    if (found == impl_->folder_store_controls.end()) return {};
    auto status = status_from(*found->second);
    if (status.terminal && status.phase == "failed") {
        // Delete owns its reserved subtree; upload and Remove own one attempt.
        // Recheck retained IDs after exceptional local bookkeeping failures.
        if (found->second->kind == Impl::FolderStoreOpKind::folder_delete) {
            for (const auto& record : impl_->store->folder_stores_in_subtree(folder_id))
                status.known_cleanup_remains |= record.remote_store_id.has_value() || !record.file_ids.empty();
        } else if (found->second->store_record_id) {
            if (const auto record = impl_->store->get_folder_store(*found->second->store_record_id))
                status.known_cleanup_remains |= record->remote_store_id.has_value() || !record->file_ids.empty();
        }
        status.known_cleanup_remains |= found->second->unsaved_store_id.has_value()
            || !found->second->unsaved_file_ids.empty();
    }
    return status;
}

void Application::cancel_folder_store_upload(std::int64_t operation_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    const auto control = impl_->folder_store_operation(operation_id);
    if (!control) throw ApplicationError(ErrorCode::invalid_argument, "That store operation is no longer current.");
    if (control->kind != Impl::FolderStoreOpKind::upload)
        throw ApplicationError(ErrorCode::invalid_argument, "Only an upload can be cancelled.");
    if (control->terminal) return;
    const std::lock_guard wait_lock(control->wait_mutex);
    control->cancel_forward->store(true);
    control->wait_cv.notify_all();
}

void Application::close_folder_store_operation(std::int64_t operation_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    const auto control = impl_->folder_store_operation(operation_id);
    if (!control) throw ApplicationError(ErrorCode::invalid_argument, "That store operation is no longer current.");
    if (!control->terminal) throw ApplicationError(ErrorCode::invalid_argument, "The operation is still running.");
    // Close must not discard ownership IDs retained after a failed local write.
    if (control->store_record_id) {
        try {
            save_unsaved_ids(*impl_->store, *control, *control->store_record_id);
        } catch (...) {
            throw ApplicationError(ErrorCode::invalid_argument, "Cleanup IDs could not be saved. Keep this operation open and try Remove again.");
        }
    }
    impl_->release_folder_store_control_locked(control);
}

Application::FolderStoreStart Application::start_folder_delete(std::int64_t folder_id,
    std::int64_t confirmed_store_count, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    if (folder_id <= 0 || folder_id > note_id_limit) throw ApplicationError(ErrorCode::invalid_argument, "A store can only belong to a real folder.");
    const auto folder = impl_->store->folder_store_folder(folder_id);
    if (!folder) throw ApplicationError(ErrorCode::invalid_argument, "That folder was not found.");
    const auto count = impl_->store->folder_store_count_in_subtree(folder_id);
    if (count != confirmed_store_count) throw ApplicationError(ErrorCode::invalid_argument, "The folder store count changed. Refresh and try again.");
    if (!count) throw ApplicationError(ErrorCode::invalid_argument, "This folder has no store.");
    if (impl_->folder_store_blocks_delete(folder_id)) throw ApplicationError(ErrorCode::invalid_argument, "A store operation is already running for this folder.");
    // Preserve any returned IDs whose earlier upload could not save ownership
    // before replacing its terminal control with the subtree operation.
    for (const auto& record : impl_->store->folder_stores_in_subtree(folder_id)) {
        const auto previous = impl_->folder_store_controls.find(record.folder_id);
        if (previous == impl_->folder_store_controls.end() || previous->second->store_record_id != record.id) continue;
        auto& previous_control = *previous->second;
        try {
            save_unsaved_ids(*impl_->store, previous_control, record.id);
        } catch (...) {
            throw ApplicationError(ErrorCode::invalid_argument, "Returned resource IDs could not be saved. Use Remove or check the operation's cleanup details.");
        }
    }
    auto control = std::make_shared<Impl::FolderStoreControl>();
    control->operation_id = impl_->next_folder_store_operation++;
    control->epoch = epoch;
    control->kind = Impl::FolderStoreOpKind::folder_delete;
    control->root_id = folder_id;
    control->folder_name = folder->name;
    control->vault_name = impl_->current_vault_.get().name;
    control->phase = Impl::FolderStorePhase::removing;
    impl_->folder_store_controls[folder_id] = control;
    if (!impl_->launch_folder_store_control(control, [owner = impl_.get(), control] { owner->run_folder_store_delete(control, false); })) {
        impl_->release_folder_store_control_locked(control);
        throw ApplicationError(ErrorCode::operation_cancelled, "The operation could not be started.");
    }
    return {.operation_id = control->operation_id};
}

Application::FolderStoreStart Application::retry_folder_delete(std::int64_t operation_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    const auto control = impl_->folder_store_operation(operation_id);
    if (!control || control->kind != Impl::FolderStoreOpKind::folder_delete || !control->terminal || !control->delete_failed) throw ApplicationError(ErrorCode::invalid_argument, "This folder delete cannot be retried.");
    control->terminal = false;
    control->delete_failed = false;
    control->error.clear();
    control->result.clear();
    control->phase = Impl::FolderStorePhase::removing;
    if (!impl_->launch_folder_store_control(control, [owner = impl_.get(), control] { owner->run_folder_store_delete(control, true); })) {
        control->terminal = true;
        control->delete_failed = true;
        control->phase = Impl::FolderStorePhase::failed;
        control->result = "error";
        control->error = "The folder delete retry could not be started. Retry or close this operation.";
        throw ApplicationError(ErrorCode::operation_cancelled, "The operation could not be started.");
    }
    return {.operation_id = control->operation_id};
}

void Application::delete_folder_anyway(std::int64_t operation_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->store->require_main();
    const auto control = impl_->folder_store_operation(operation_id);
    if (!control || control->kind != Impl::FolderStoreOpKind::folder_delete || !control->terminal || !control->delete_failed) throw ApplicationError(ErrorCode::invalid_argument, "Delete anyway is only available for a failed folder delete.");
    const auto records = impl_->store->folder_stores_in_subtree(control->root_id);
    std::vector<std::string> warnings;
    std::vector<std::int64_t> ids;
    for (const auto& record : records) {
        ids.push_back(record.id);
        warnings.push_back("Deleted local notes without confirmed cleanup for " + context(record)
            + resource_ids(record) + ". Remote data may remain; manual cleanup is required.");
    }
    delete_locally(*impl_, control, warnings, ids);
    control->phase = Impl::FolderStorePhase::warning;
    control->warned = true;
    control->result = "warning";
    control->error.clear();
    control->known_cleanup_remains = false;
    control->delete_failed = false;
}

std::optional<FolderStoreRecord> Application::folder_store_record(std::int64_t folder_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return impl_->store->get_folder_store_by_folder(folder_id);
}

std::vector<FolderStoreRecord> Application::list_folder_stores(std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return impl_->store->list_folder_stores();
}

std::int64_t Application::folder_store_count_in_subtree(std::int64_t folder_id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return impl_->store->folder_store_count_in_subtree(folder_id);
}

std::vector<FolderStoreKeyUsage> Application::folder_store_usage(std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return impl_->store->folder_store_usage();
}

void Application::set_folder_store_transport_for_tests(FolderStoreTransport transport) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->folder_store_transport = std::move(transport);
}

} // namespace cha::app
