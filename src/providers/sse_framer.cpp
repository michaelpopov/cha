#include "providers/sse_framer.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace cha {

namespace {

void normalize_newlines(std::string& text) {
    std::size_t index = 0;
    while ((index = text.find("\r\n", index)) != std::string::npos) {
        text.erase(index, 1);
    }
}

} // namespace

void SseFramer::consume(std::string_view bytes, const DataHandler& on_data) {
    pending_.append(bytes);
    normalize_newlines(pending_);

    std::size_t event_end = 0;
    while ((event_end = pending_.find("\n\n")) != std::string::npos) {
        const std::string event = pending_.substr(0, event_end);
        pending_.erase(0, event_end + 2);
        if (!read_event(event, on_data)) {
            pending_.clear();
            return;
        }
    }
}

void SseFramer::finish(const DataHandler& on_data) {
    if (pending_.empty()) {
        return;
    }
    (void)read_event(pending_, on_data);
    pending_.clear();
}

bool SseFramer::read_event(
    std::string_view event,
    const DataHandler& on_data) {
    std::size_t line_start = 0;
    while (line_start <= event.size()) {
        const std::size_t line_end = event.find('\n', line_start);
        const std::string_view line = event.substr(
            line_start,
            line_end == std::string_view::npos
                ? event.size() - line_start
                : line_end - line_start);

        if (line.starts_with("data:")) {
            std::string_view data = line.substr(5);
            while (!data.empty() && data.front() == ' ') {
                data.remove_prefix(1);
            }
            if (!data.empty() && !on_data(data)) {
                return false;
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }
    return true;
}

} // namespace cha
