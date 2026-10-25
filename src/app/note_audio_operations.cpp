#include "app/application_internal.h"
#include "media/note_audio.h"
#include "util/private_filesystem.h"
#include <random>

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
#ifndef __APPLE__
    (void)id; (void)voice; (void)blocks; (void)epoch;
    throw ApplicationError(ErrorCode::invalid_argument, "Note audio generation is available on macOS only.");
#else
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (impl_->note_audio_epoch == epoch && impl_->note_audio_status.is_object()
        && !impl_->note_audio_status.value("done", true))
        throw ApplicationError(ErrorCode::invalid_argument, "Note audio generation is already running.");
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
    catch (const std::exception& error) { throw ApplicationError(ErrorCode::invalid_argument, error.what()); }
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
        });
        if (!launched) throw ApplicationError(ErrorCode::operation_cancelled, "Audio generation was interrupted.");
    } catch (...) {
        impl_->note_audio_status.update({{"done", true}, {"error", "Audio generation could not start."}, {"finished_at_ms", now_ms()}});
        throw;
    }
    return initial;
#endif
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
