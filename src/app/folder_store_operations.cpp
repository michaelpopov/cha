#include "app/application_internal.h"

namespace cha::app {

std::vector<Application::FolderStoreChoiceView> Application::folder_store_choices(std::uint64_t epoch) {
    return impl_->notes->folder_store_choices(epoch);
}

Application::FolderStoreStart Application::start_folder_store_upload(std::int64_t folder_id,
    std::string service, std::string api_key_id, std::string name, std::string description, std::uint64_t epoch) {
    return impl_->notes->start_folder_store_upload(folder_id, std::move(service), std::move(api_key_id), std::move(name), std::move(description), epoch);
}

Application::FolderStoreStart Application::retry_folder_store_cleanup(std::int64_t operation_id, std::uint64_t epoch) {
    return impl_->notes->retry_folder_store_cleanup(operation_id, epoch);
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

void Application::set_folder_store_transport_for_tests(FolderStoreTransport transport) {
    impl_->notes->set_folder_store_transport_for_tests(std::move(transport));
}

} // namespace cha::app
