#include "app/application_internal.h"
#include "media/note_audio.h"
#include "util/private_filesystem.h"
#include <random>
#include "storage/session_lease.h"
#include "util/logging.h"

namespace cha::app {
namespace {
struct TemporaryAudio {
    std::filesystem::path path;
    TemporaryAudio() {
        path = std::filesystem::temp_directory_path() / ("cha-note-audio-" + std::to_string(std::random_device{}())
            + "-" + std::to_string(std::random_device{}()));
        create_private_directory(path);
    }
    ~TemporaryAudio() { std::error_code error; std::filesystem::remove_all(path, error); }
};
std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

nlohmann::json Application::start_note_audio(std::int64_t id, std::string voice,
    std::vector<std::string> blocks, std::uint64_t epoch) {
#if !defined(__APPLE__) && !defined(_WIN32)
    (void)id; (void)voice; (void)blocks; (void)epoch;
    throw ApplicationError(ErrorCode::invalid_argument, "Note audio generation is available on macOS and Windows only.");
#else
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->require_audio_slot(epoch);
    const auto items = impl_->store->list_note_items();
    const auto item = std::find_if(items.begin(), items.end(), [id](const auto& note) { return note.id == id; });
    if (item == items.end() || item->kind != NoteItemKind::note || !item->read_only)
        throw ApplicationError(ErrorCode::invalid_argument, "Make the note read-only to generate audio.");
    if (blocks.empty()) throw ApplicationError(ErrorCode::invalid_argument, "No text to narrate");
    const auto workspace = impl_->store->snapshot();
    NoteAudioService service{workspace->notes().audio_url, {}, std::move(voice)};
    if (service.url.empty() || !impl_->api_keys->find(workspace->notes().audio_api_key_id))
        throw ApplicationError(ErrorCode::invalid_argument, "Configure the audio service URL and API key in Notes settings.");
    service.key = impl_->api_keys->value(workspace->notes().audio_api_key_id);
    std::string jsonl;
    try { jsonl = prepare_note_audio(service, blocks); }
    catch (const NoteAudioSizeError& error) { throw ApplicationError(ErrorCode::body_too_large, error.what()); }
    catch (const std::exception& error) { throw ApplicationError(ErrorCode::invalid_argument, error.what()); }
    impl_->note_audio_cancel = std::make_shared<std::atomic_bool>(false);
    impl_->note_audio_epoch = epoch;
    impl_->note_audio_status = {{"id", id}, {"phase", "uploading"}, {"done", false}, {"error", ""}};
    const auto initial = impl_->note_audio_status;
    try {
        const bool launched = impl_->background_jobs.launch([owner = impl_.get(), id, epoch,
            service = std::move(service), jsonl = std::move(jsonl), count = blocks.size()](std::atomic_bool& cancel) {
            const auto update = [owner, epoch](std::string phase,
                std::optional<std::size_t> completed, std::optional<std::size_t> total) {
                const std::lock_guard lock(owner->lifecycle_mutex);
                owner->require_admitted(epoch);
                owner->note_audio_status["phase"] = std::move(phase);
                for (const auto* field : {"completed", "total"}) owner->note_audio_status.erase(field);
                if (completed) owner->note_audio_status["completed"] = *completed;
                if (total) owner->note_audio_status["total"] = *total;
            };
            std::string error;
            std::int64_t finished_at{};
            try {
                TemporaryAudio temporary;
                auto clips = generate_note_audio(service, jsonl, count, temporary.path, cancel, update);
                const std::lock_guard lock(owner->lifecycle_mutex);
                owner->require_admitted(epoch);
                if (cancel.load()) throw std::runtime_error("Audio generation was interrupted.");
                try { owner->store->save_note_audio(id, clips); }
                catch (const std::exception&) {
                    throw std::runtime_error("Could not save note audio. Check that the note still exists and available disk space.");
                }
                finished_at = now_ms();
                owner->note_audio_status.update({{"phase", "complete"}, {"completed", clips.size()}, {"total", clips.size()}});
            } catch (const nlohmann::json::exception&) {
                error = "The audio service returned an invalid response. Check the service before trying again.";
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
                error = "Note audio generation failed. Check the service and available disk space.";
            }
            if (!finished_at) finished_at = now_ms();
            const std::lock_guard lock(owner->lifecycle_mutex);
            try { owner->require_admitted(epoch); } catch (const ApplicationError&) { return; }
            owner->note_audio_status.update({{"error", error}, {"done", true}, {"finished_at_ms", finished_at}});
        }, impl_->note_audio_cancel);
        if (!launched) throw ApplicationError(ErrorCode::operation_cancelled, "Audio generation was interrupted.");
    } catch (...) {
        impl_->note_audio_status.update({{"done", true}, {"error", "Audio generation could not start."}, {"finished_at_ms", now_ms()}});
        throw;
    }
    return initial;
#endif
}

void Application::Impl::require_audio_slot(std::uint64_t epoch) {
    if (const auto attempt = store->get_note_audio_attempt())
        throw ApplicationError(ErrorCode::invalid_argument, "Audio generation is already running for "
            + note_audio_target(*attempt) + ". Make that note editable to abandon the attempt.");
    if (note_audio_epoch == epoch && note_audio_status.is_object()
        && !note_audio_status.value("done", true))
        throw ApplicationError(ErrorCode::invalid_argument, "Note audio generation is already running.");
}

void Application::set_note_audio_changed(NoteAudioChanged callback) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->note_audio_changed = std::move(callback);
    if (impl_->note_audio_changed) impl_->recover_note_audio();
    else if (const auto cancel = std::atomic_load(&impl_->background_audio_cancel)) cancel->store(true);
}

nlohmann::json Application::start_background_note_audio(std::int64_t id, std::string voice,
    std::vector<std::string> blocks, std::uint64_t epoch) {
#if !defined(__APPLE__) && !defined(_WIN32)
    (void)id; (void)voice; (void)blocks; (void)epoch;
    throw ApplicationError(ErrorCode::invalid_argument, "Note audio generation is available on macOS and Windows only.");
#else
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (!impl_->note_audio_changed || impl_->command.chaweb_host)
        throw ApplicationError(ErrorCode::invalid_argument, "Open the native application to generate note audio.");
    try { impl_->store->require_main(); }
    catch (const SecondaryProcessError&) { throw ApplicationError(ErrorCode::invalid_argument, vault_in_use_message); }
    impl_->require_audio_slot(epoch);
    const auto workspace = impl_->store->snapshot();
    const auto key_id = workspace->notes().audio_api_key_id;
    if (!impl_->api_keys->find(key_id))
        throw ApplicationError(ErrorCode::invalid_argument, "Configure the audio service URL and API key in Notes settings.");
    NoteAudioService service{workspace->notes().audio_url, impl_->api_keys->value(key_id), voice};
    std::string jsonl;
    try { jsonl = prepare_note_audio(service, blocks); }
    catch (const NoteAudioSizeError& error) { throw ApplicationError(ErrorCode::body_too_large, error.what()); }
    catch (const std::exception& error) { throw ApplicationError(ErrorCode::invalid_argument, error.what()); }
    try {
        const auto attempt = impl_->store->accept_note_audio(id, static_cast<std::int64_t>(blocks.size()),
            service.url, key_id, [&](const NoteAudioAttempt& attempt) {
                impl_->background_audio_warned = false;
                impl_->launch_note_audio(attempt, voice, std::move(jsonl));
            });
        return {{"attempt_id", attempt.id}};
    } catch (const NoteError& error) {
        throw ApplicationError(ErrorCode::invalid_argument, error.what());
    } catch (...) {
        // The lifecycle lock is the launch barrier: even a failed commit cannot send HTTP.
        if (const auto cancel = std::atomic_load(&impl_->background_audio_cancel)) cancel->store(true);
        throw;
    }
#endif
}

nlohmann::json Application::background_note_audio_status(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto attempt = impl_->store->get_note_audio_attempt();
    if (attempt && attempt->note_id == id) return {{"pending", true}, {"attempt_id", attempt->id}};
    return {{"pending", false}};
}

void Application::Impl::interrupt_abandoned_audio() {
    const auto cancel = std::atomic_load(&background_audio_cancel);
    if (!cancel) return;
    try {
        if (!store->get_note_audio_attempt()) cancel->store(true);
    } catch (const std::exception&) {
        // The note change has committed. A worker will check its row again.
        log_warn("Could not check audio abandonment after the note change committed.");
    }
}

void Application::Impl::resume_note_audio() {
    if (!note_audio_changed || command.chaweb_host || state.load() != ApplicationState::running || stopping_flag) return;
    try {
        background_jobs.launch([this, epoch = published_epoch.load()](std::atomic_bool& cancel) {
            const std::lock_guard lock(lifecycle_mutex);
            if (!cancel.load() && !admit_locked(epoch)) recover_note_audio();
        });
    } catch (const std::exception&) { log_warn("Could not resume note audio after maintenance."); }
}

void Application::Impl::recover_note_audio() {
#if defined(__APPLE__) || defined(_WIN32)
    if (!note_audio_changed || command.chaweb_host || state.load() != ApplicationState::running || stopping_flag || !store->is_main()) return;
    try {
        const auto attempt = store->get_note_audio_attempt();
        if (!attempt) return;
        const auto cancel = std::atomic_load(&background_audio_cancel);
        if (cancel && !cancel->load()) return;
        launch_note_audio(*attempt);
    } catch (const std::exception&) {
        log_warn("Could not resume note audio. Reopen the vault to try again.");
    }
#endif
}

void Application::Impl::launch_note_audio(NoteAudioAttempt attempt, std::string voice, std::string jsonl) {
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto epoch = published_epoch.load();
    const bool launched = background_jobs.launch([this, attempt, epoch, cancel,
        voice = std::move(voice), jsonl = std::move(jsonl)](std::atomic_bool&) mutable {
        run_note_audio(attempt, epoch, cancel, std::move(voice), std::move(jsonl));
    }, cancel);
    if (!launched) throw std::runtime_error("Could not start note audio worker.");
    std::atomic_store(&background_audio_cancel, std::move(cancel));
}

void Application::Impl::run_note_audio(NoteAudioAttempt attempt, std::uint64_t epoch,
    std::shared_ptr<std::atomic_bool> cancel, std::string voice, std::string jsonl) {
    const auto interrupted = [] { throw NoteAudioError(NoteAudioFailure::interrupted, "Audio generation was interrupted."); };
    // All calls to this guard hold lifecycle_mutex. No old worker can touch a new vault or launch.
    const auto check = [&] {
        if (cancel->load() || std::atomic_load(&background_audio_cancel) != cancel || admit_locked(epoch)) interrupted();
        std::optional<NoteAudioAttempt> current;
        try { current = store->get_note_audio_attempt(); }
        catch (const std::exception&) { throw NoteAudioError(NoteAudioFailure::storage, "Could not read the saved audio attempt."); }
        if (!current || current->id != attempt.id) interrupted();
    };
    const NoteAudioCredentials credentials = [&] {
        const std::lock_guard lock(lifecycle_mutex);
        check();
        if (!api_keys->find(attempt.api_key_id))
            throw NoteAudioError(NoteAudioFailure::invalid,
                "The saved API key was deleted. The remote batch may continue. Configure a key before starting again.");
        auto key = api_keys->value(attempt.api_key_id);
        if (key.empty() || key.find_first_of("\r\n") != std::string::npos)
            throw NoteAudioError(NoteAudioFailure::http, "Replace the audio API key value in Settings → API keys.", 401);
        return NoteAudioService{attempt.service_origin, std::move(key), voice};
    };
    const auto warn = [&](std::string_view reason) {
        const std::lock_guard lock(lifecycle_mutex);
        try {
            check();
            if (background_audio_warned) return;
            store->append_event(EventSeverity::warning, "Audio generation retained for " + note_audio_target(attempt)
                + ": " + std::string(reason) + " Make the note editable to abandon it.");
            background_audio_warned = true;
        } catch (const NoteAudioError& error) {
            if (error.kind == NoteAudioFailure::interrupted) throw;
            log_warn("Could not record the retained audio warning.");
        } catch (const std::exception&) { log_warn("Could not record the retained audio warning."); }
    };
    std::string step = "submission";
    const auto diagnostic = [&](std::string_view reason) {
        log_warn("Audio " + step + " failed for " + note_audio_target(attempt) + ": " + std::string(reason));
    };
    // Retry only the current step. POST submission never enters this loop.
    const auto retry = [&](const auto& operation, bool local) -> decltype(operation()) {
        for (unsigned failures = 0;;) {
            std::string reason;
            bool credential = false;
            try {
                { const std::lock_guard lock(lifecycle_mutex); check(); }
                return operation();
            }
            catch (const NoteAudioError& error) {
                if (error.kind != NoteAudioFailure::storage && error.kind != NoteAudioFailure::transport
                    && !(error.kind == NoteAudioFailure::http && (error.code == 401 || error.code >= 500))) throw;
                credential = error.kind == NoteAudioFailure::http && error.code == 401;
                reason = error.kind == NoteAudioFailure::transport ? "The service connection failed. Check the service address and connection."
                    : error.kind == NoteAudioFailure::storage ? "Local audio storage failed. Check disk space and file access."
                    : "The service returned HTTP " + std::to_string(error.code) + ".";
                if (credential) reason += " Replace the saved key value in Settings → API keys.";
                else if (error.kind == NoteAudioFailure::http) reason += " Check the audio service.";
                diagnostic(error.what());
            } catch (const std::filesystem::filesystem_error& error) {
                reason = "Local audio file access failed (error " + std::to_string(error.code().value()) + "). Check disk space and file access.";
                diagnostic(reason);
            }
            catch (const NoteError&) { throw; }
            catch (const std::exception&) {
                if (!local) throw;
                reason = "The local audio transaction failed. Check disk space and database access.";
                diagnostic(reason); // Do not expose raw SQL or library exception text.
            }
            if (credential || failures >= 3) warn(reason);
            wait_note_audio(10 << std::min(failures, 2u), *cancel, settings.note_audio_wait);
            if (failures < 3) ++failures;
        }
    };
    const auto retire = [&](EventSeverity severity, const std::string& message) {
        retry([&] {
            const std::lock_guard lock(lifecycle_mutex);
            check();
            return store->retire_note_audio(attempt.id, severity, message);
        }, true);
    };
    try {
        retry([&] { const std::lock_guard lock(lifecycle_mutex); check(); return true; }, true);
        if (!attempt.batch_id && jsonl.empty()) {
            retire(EventSeverity::warning, "Audio submission status unknown for " + note_audio_target(attempt)
                + ". Submission was interrupted before the batch ID was saved. Starting again may generate audio twice. Check the service before starting again; do not assume the remote batch was cancelled.");
        } else {
            nlohmann::json operation;
            if (!attempt.batch_id) {
                operation = submit_note_audio(credentials, jsonl, *cancel);
                attempt.batch_id = note_audio_batch_id(operation);
                step = "batch registration";
                retry([&] {
                    const std::lock_guard lock(lifecycle_mutex);
                    check();
                    return store->register_note_audio(attempt.id, *attempt.batch_id, voice);
                }, true);
                std::string{}.swap(jsonl);
                voice.clear();
            } else {
                step = "batch read";
                operation = retry([&] { return read_note_audio_batch(credentials, *attempt.batch_id, *cancel); }, false);
            }
            step = "batch read";
            for (;;) {
                if (operation.contains("error"))
                    throw NoteAudioError(NoteAudioFailure::invalid, "The audio batch failed. Check the service before trying again.");
                if (operation.value("done", false)) break;
                if (!operation.contains("metadata") || !operation.at("metadata").is_object()
                    || !operation.at("metadata").contains("state") || !operation.at("metadata").at("state").is_string())
                    throw NoteAudioError(NoteAudioFailure::invalid, "The audio service returned an invalid batch response. Check the service.");
                wait_note_audio(note_audio_poll_seconds, *cancel, settings.note_audio_wait);
                operation = retry([&] { return read_note_audio_batch(credentials, *attempt.batch_id, *cancel); }, false);
            }
            step = "result download";
            // Temporary files have no persistent cache or lifecycle of their own.
            auto temporary = retry([&] { return std::make_unique<TemporaryAudio>(); }, true);
            retry([&] { download_note_audio(credentials, operation, static_cast<std::size_t>(attempt.clip_count), temporary->path, *cancel); }, false);
            step = "conversion";
            auto clips = retry([&] { return process_note_audio(static_cast<std::size_t>(attempt.clip_count), temporary->path, *cancel); }, false);
            step = "save";
            retry([&] {
                const std::lock_guard lock(lifecycle_mutex);
                check();
                if (store->finish_note_audio(attempt.id, clips) && note_audio_changed)
                    note_audio_changed(attempt.note_id, epoch); // Enqueue only, while commit/source reads are serialized.
                return true;
            }, true);
        }
    } catch (const NoteAudioError& error) {
        if (error.kind != NoteAudioFailure::interrupted) {
            diagnostic(error.what());
            const bool uncertain = error.kind == NoteAudioFailure::uncertain;
            try { retire(uncertain ? EventSeverity::warning : EventSeverity::error,
                "Audio " + step + (uncertain ? " for " : " failed for ") + note_audio_target(attempt) + ": " + error.what()); }
            catch (...) { /* Interruption leaves the durable record for recovery. */ }
        }
    } catch (const NoteError& error) {
        diagnostic(error.what());
        try { retire(EventSeverity::error, "Audio " + step + " failed for " + note_audio_target(attempt) + ": " + error.what()); }
        catch (...) { /* Interruption leaves the durable record for recovery. */ }
    } catch (const std::exception&) {
        diagnostic("The service response or local operation was invalid.");
        try { retire(EventSeverity::error, "Audio " + step + " failed for " + note_audio_target(attempt)
            + ". Check the service response and audio format before starting again."); }
        catch (...) { /* Interruption leaves the durable record for recovery. */ }
    }
    const std::lock_guard lock(lifecycle_mutex);
    if (std::atomic_load(&background_audio_cancel) == cancel)
        std::atomic_store(&background_audio_cancel, std::shared_ptr<std::atomic_bool>{});
}

nlohmann::json Application::cancel_note_audio(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (impl_->note_audio_epoch == epoch && impl_->note_audio_status.is_object()
        && impl_->note_audio_status.value("id", std::int64_t{}) == id
        && !impl_->note_audio_status.value("done", true) && impl_->note_audio_cancel)
        impl_->note_audio_cancel->store(true);
    return nlohmann::json::object();
}

nlohmann::json Application::get_note_audio_status(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (impl_->note_audio_epoch != epoch || !impl_->note_audio_status.is_object()
        || impl_->note_audio_status.value("id", std::int64_t{}) != id)
        throw ApplicationError(ErrorCode::not_found, "There is no audio generation for this note.");
    return impl_->note_audio_status;
}

nlohmann::json Application::get_note_audio_info(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return impl_->store->get_note_audio_info(id);
}

MediaResource Application::note_audio_source(std::string_view connection_id, std::int64_t id,
    std::int64_t ordinal, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    NoteAudioClip clip;
    try { clip = impl_->store->get_note_audio_clip(id, ordinal); }
    catch (const NoteError& e) { throw ApplicationError(ErrorCode::not_found, e.what()); }
    const auto size = clip.bytes.size();
    const auto resource = impl_->media_resources.add(connection_id, epoch, ResourceKind::entry_audio,
        {clip.mime_type, std::move(clip.bytes)});
    return {resource, MediaResources::url_for(resource), std::move(clip.mime_type), size};
}
}
