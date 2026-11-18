#include "app/application_internal.h"

namespace cha::app {

std::vector<NoteItem> Application::list_note_items(std::uint64_t epoch) {
    return impl_->notes->list_note_items(epoch);
}

NoteContent Application::get_note(std::int64_t id, std::uint64_t epoch, NoteTextMode mode) {
    return impl_->notes->get_note(id, epoch, mode);
}

bool Application::save_note(std::int64_t id, const nlohmann::json& document, std::uint64_t epoch) {
    return impl_->notes->save_note(id, document, epoch);
}

void Application::save_note_scroll_position(std::int64_t id, std::int64_t scroll_top, std::uint64_t epoch) {
    impl_->notes->save_note_scroll_position(id, scroll_top, epoch);
}

NoteItem Application::rename_note_item(std::int64_t id, std::string name, std::uint64_t epoch) {
    return impl_->notes->rename_note_item(id, std::move(name), epoch);
}

nlohmann::json Application::start_summary_batch(std::string connection_id, std::int64_t id,
    SummaryBatchOptions options, std::uint64_t epoch) {
    return impl_->notes->start_summary_batch(std::move(connection_id), id, std::move(options), epoch);
}

void Application::summary_line(std::string connection_id, std::int64_t id, bool finished, std::uint64_t epoch) {
    impl_->notes->summary_line(std::move(connection_id), id, finished, epoch);
}

std::shared_ptr<OperationReply> Application::generate_summary_part(std::string connection_id,
    std::int64_t id, SummaryPart part, std::string text, std::uint64_t epoch) {
    return impl_->notes->generate_summary_part(std::move(connection_id), id, part, std::move(text), epoch);
}

void Application::commit_summary_part(std::int64_t id, SummaryPart part,
    const nlohmann::json& document, std::uint64_t epoch, std::optional<std::string_view> connection_id) {
    impl_->notes->commit_summary_part(id, part, document, epoch, connection_id);
}

void Application::abandon_summary_part(std::string_view connection_id,
    std::int64_t id, SummaryPart part, std::uint64_t epoch, std::optional<std::int64_t> source_id) {
    impl_->notes->abandon_summary_part(connection_id, id, part, epoch, source_id);
}

void Application::finish_summary_batch(std::string_view connection_id, std::uint64_t epoch) {
    impl_->notes->finish_summary_batch(connection_id, epoch);
}

NoteItem Application::set_note_summary_excluded(std::int64_t id, bool excluded, std::uint64_t epoch) {
    return impl_->notes->set_note_summary_excluded(id, excluded, epoch);
}

std::shared_ptr<OperationReply> Application::test_notes_flex(std::string provider_id, std::uint64_t epoch) {
    return impl_->notes->test_flex(std::move(provider_id), epoch);
}

NoteItem Application::commit_session_summary(std::string_view connection_id, std::int64_t run_id,
    const nlohmann::json& document, std::uint64_t epoch) {
    return impl_->notes->commit_session_summary(connection_id, run_id, document, epoch);
}

std::shared_ptr<OperationReply> Application::generate_session_summary(std::string connection_id, const FullSessionId& identity, std::uint64_t epoch) {
    return impl_->notes->generate_session_summary(std::move(connection_id), identity, epoch);
}

void Application::abandon_session_summary(std::string_view connection_id, std::int64_t run_id, std::string_view reason, std::uint64_t epoch) {
    impl_->notes->abandon_session_summary(connection_id, run_id, reason, epoch);
}

std::shared_ptr<OperationReply> Application::read_note_page(std::string url, std::uint64_t epoch,
    std::function<void(std::string_view)> progress) {
    return impl_->notes->read_note_page(std::move(url), epoch, std::move(progress));
}

std::shared_ptr<OperationReply> Application::read_note_picture(std::string url, std::uint64_t epoch) {
    return impl_->notes->read_note_picture(std::move(url), epoch);
}

NoteItem Application::set_note_read_only(std::int64_t id, bool read_only, std::uint64_t epoch) {
    return impl_->notes->set_note_read_only(id, read_only, epoch);
}

NoteItem Application::create_note_item(
    std::optional<std::int64_t> parent_id,
    NoteItemKind kind,
    std::string name,
    std::uint64_t epoch) {
    return impl_->notes->create_note_item(parent_id, kind, std::move(name), epoch);
}

std::int64_t Application::import_note_book(
    std::optional<std::int64_t> parent_id,
    std::string name, const std::vector<NoteImportEntry>& notes, std::uint64_t epoch) {
    return impl_->notes->import_note_book(parent_id, std::move(name), notes, epoch);
}

void Application::export_note_folder(
    std::int64_t folder_id, const std::filesystem::path& destination, std::uint64_t epoch) {
    impl_->notes->export_note_folder(folder_id, destination, epoch);
}

std::int64_t Application::import_note_folder(
    std::optional<std::int64_t> parent_id, const std::filesystem::path& source, std::uint64_t epoch) {
    return impl_->notes->import_note_folder(parent_id, source, epoch);
}

void Application::delete_note_item(std::int64_t id, std::uint64_t epoch) {
    impl_->notes->delete_note_item(id, epoch);
}

} // namespace cha::app
