#include "app/application_internal.h"

#include "util/logging.h"

namespace cha::app {

void Application::record_event(EventSeverity severity,
    std::string_view message, std::uint64_t epoch) noexcept {
    try {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        if (impl_->admit_locked(epoch)) {
            log_warn("Event dropped: originating context unavailable");
            return;
        }
        impl_->store->append_event(severity, message);
    } catch (...) {
        log_warn("Event recording failed");
    }
}

std::vector<ActivityEvent> Application::list_events(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->list_events();
    } catch (...) {
        log_warn("Event history read failed");
        throw ApplicationError(ErrorCode::internal_error, "Could not load events");
    }
}

} // namespace cha::app
