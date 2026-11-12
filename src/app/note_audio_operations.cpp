#include "app/application_internal.h"

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
