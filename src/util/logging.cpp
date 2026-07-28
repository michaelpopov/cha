#include "util/logging.h"

#include "util/utf8_path.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

namespace cha {

void initialize_diagnostic_logging() {
    const char* configured_path = std::getenv("CHA_LOG_FILE");
    if (!configured_path || configured_path[0] == '\0') {
        return;
    }

    auto logger = spdlog::basic_logger_mt(
        "cha",
        path_from_utf8(configured_path).native(),
        false);
    logger->set_level(spdlog::level::trace);
    logger->set_pattern(
        "[%Y-%m-%d %H:%M:%S.%f] [thread %t] [%l] %v");
    logger->set_error_handler([](const std::string&) {
        // A diagnostic sink must not write to the application's terminal.
    });
    logger->flush_on(spdlog::level::trace);
    logger->info("diagnostic logging enabled");
}

} // namespace cha
