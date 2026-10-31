#pragma once

#include <string_view>

namespace cha {

enum class ErrorCode {
    not_found, body_too_large, prompt_too_large,
    internal_error, speech_busy, vault_changed, session_stopping,
    session_limit_reached, session_open_timeout, server_stopping,
    session_not_live, command_timeout,
    command_queue_full, vault_password_required,
    source_vault_password_required,
    invalid_argument, operation_cancelled, application_unavailable,
};

std::string_view to_string(ErrorCode value);

} // namespace cha
