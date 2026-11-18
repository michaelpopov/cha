#include "app/application_internal.h"
#include "app/media_operations.h"
#include "media/note_audio.h"
#include "util/text.h"

namespace cha::app {

MediaResource Application::note_audio_source(std::string_view connection_id, std::int64_t id,
    std::int64_t ordinal, std::uint64_t epoch) {
    const std::lock_guard lock(impl_->lifecycle_mutex);
    auto clip = impl_->notes->get_note_audio_clip_locked(id, ordinal, epoch);
    const auto size = clip.bytes.size();
    const auto resource = impl_->media_resources.add(connection_id, epoch, ResourceKind::entry_audio,
        {clip.mime_type, std::move(clip.bytes)});
    return {resource, MediaResources::url_for(resource), std::move(clip.mime_type), size};
}

void Application::set_note_audio_changed(NoteAudioChanged callback) {
    impl_->notes->set_note_audio_changed(std::move(callback));
}

std::shared_ptr<OperationReply> Application::list_note_audio_voices(std::uint64_t epoch) {
    return impl_->notes->list_note_audio_voices(epoch);
}

std::shared_ptr<OperationReply> Application::preview_note_audio(std::string_view connection_id,
    std::uint64_t request_id, std::string voice, std::string text, std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    NoteAudioService service;
    std::shared_ptr<PendingMediaRegistry::PendingMedia> pending;
    {
        const std::lock_guard lock(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        if (impl_->command.chaweb_host || trim_view(voice).empty() || trim_view(text).empty() || text.size() > 512)
            throw ApplicationError(ErrorCode::invalid_argument);
        service = impl_->notes->note_audio_service_locked(std::move(voice));
        pending = impl_->pending_media.remember(std::string(connection_id), request_id);
    }

    try {
        if (impl_->background_jobs.launch([owner = impl_.get(), reply, pending, epoch,
                service = std::move(service), text = std::move(text)](std::atomic_bool& cancel) {
                PendingMediaRegistry::Cleanup cleanup{owner->pending_media, pending};
                try {
                    auto wav = synthesize_note_audio_preview(service, text, cancel);
                    const auto size = wav.size();
                    std::string id;
                    {
                        const std::lock_guard lock(owner->lifecycle_mutex);
                        owner->require_admitted(epoch);
                        if (cancel.load()) throw ApplicationError(ErrorCode::operation_cancelled);
                        id = owner->media_resources.add(pending->connection_id, epoch, ResourceKind::speech,
                            {"audio/wav", std::move(wav)});
                        if (!owner->pending_media.set_resource(pending, id)) {
                            (void)owner->media_resources.release(pending->connection_id, id);
                            throw ApplicationError(ErrorCode::operation_cancelled);
                        }
                    }
                    if (reply->complete(media_resource_json(id, "audio/wav", size))) return;
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (const std::exception&) {
                    if (cancel.load()) reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
                    else reply->fail(ErrorCode::internal_error, "Could not preview this voice. Check the audio service in Notes settings.");
                }
                owner->pending_media.cancel(pending->connection_id, pending->request_id);
            }, pending->cancelled)) return reply;
    } catch (...) {
        impl_->pending_media.forget(pending);
        throw;
    }
    impl_->pending_media.forget(pending);
    reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
    return reply;
}

nlohmann::json Application::start_background_note_audio(std::int64_t id, std::string voice, std::vector<std::string> blocks, std::uint64_t epoch, bool only_if_missing) {
    return impl_->notes->start_background_note_audio(id, std::move(voice), std::move(blocks), epoch, only_if_missing);
}

nlohmann::json Application::background_note_audio_status(std::int64_t id, std::uint64_t epoch) {
    return impl_->notes->background_note_audio_status(id, epoch);
}

nlohmann::json Application::get_note_audio_info(std::int64_t id, std::uint64_t epoch) {
    return impl_->notes->get_note_audio_info(id, epoch);
}

} // namespace cha::app
