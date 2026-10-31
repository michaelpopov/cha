#include "app/application_internal.h"

namespace cha::app {

std::vector<Application::FolderStoreChoiceView> Application::folder_store_choices(std::uint64_t epoch) {
    return impl_->notes->folder_store_choices(epoch);
}

Application::FolderStoreStart Application::start_folder_store_upload(std::int64_t folder_id,
    std::string service, std::string api_key_id, std::uint64_t epoch) {
    return impl_->notes->start_folder_store_upload(folder_id, std::move(service), std::move(api_key_id), epoch);
}

Application::FolderStoreStart Application::start_folder_store_remove(std::int64_t folder_id,
    std::int64_t expected_store_id, std::uint64_t epoch) {
    return impl_->notes->start_folder_store_remove(folder_id, expected_store_id, epoch);
}

Application::FolderStoreOperationStatus Application::folder_store_operation_status(std::int64_t folder_id, std::uint64_t epoch) {
    return impl_->notes->folder_store_operation_status(folder_id, epoch);
}

void Application::cancel_folder_store_upload(std::int64_t operation_id, std::uint64_t epoch) {
    impl_->notes->cancel_folder_store_upload(operation_id, epoch);
}

void Application::close_folder_store_operation(std::int64_t operation_id, std::uint64_t epoch) {
    impl_->notes->close_folder_store_operation(operation_id, epoch);
}

Application::FolderStoreStart Application::start_folder_delete(std::int64_t folder_id,
    std::int64_t confirmed_store_count, std::uint64_t epoch) {
    return impl_->notes->start_folder_delete(folder_id, confirmed_store_count, epoch);
}

Application::FolderStoreStart Application::retry_folder_delete(std::int64_t operation_id, std::uint64_t epoch) {
    return impl_->notes->retry_folder_delete(operation_id, epoch);
}

void Application::delete_folder_anyway(std::int64_t operation_id, std::uint64_t epoch) {
    impl_->notes->delete_folder_anyway(operation_id, epoch);
}

std::optional<FolderStoreRecord> Application::folder_store_record(std::int64_t folder_id, std::uint64_t epoch) {
    return impl_->notes->folder_store_record(folder_id, epoch);
}

std::vector<FolderStoreRecord> Application::list_folder_stores(std::uint64_t epoch) {
    return impl_->notes->list_folder_stores(epoch);
}

std::int64_t Application::folder_store_count_in_subtree(std::int64_t folder_id, std::uint64_t epoch) {
    return impl_->notes->folder_store_count_in_subtree(folder_id, epoch);
}

std::vector<FolderStoreKeyUsage> Application::folder_store_usage(std::uint64_t epoch) {
    return impl_->notes->folder_store_usage(epoch);
}

void Application::set_folder_store_transport_for_tests(FolderStoreTransport transport) {
    impl_->notes->set_folder_store_transport_for_tests(std::move(transport));
}

} // namespace cha::app
