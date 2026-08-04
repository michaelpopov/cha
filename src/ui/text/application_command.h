#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <vector>

namespace cha {

// Typed grammar for fixed-arity terminal application commands.  It deliberately
// stays separate from free-form controller commands such as /mcast.
enum class ApplicationCommandKind { iam, open, create, forums, sessions, personas, help };
struct ApplicationCommand { ApplicationCommandKind kind; std::vector<std::string> names; };
enum class ApplicationCommandParseError { unmatched_quote, unsupported_escape, empty_name, missing_argument, extra_argument };
using ApplicationCommandParseResult = std::variant<ApplicationCommand, ApplicationCommandParseError>;

// Returns nullopt when input is not an application command.
std::optional<ApplicationCommandParseResult> parse_application_command(std::string_view input);
std::string_view application_command_parse_error_message(ApplicationCommandParseError error);
} // namespace cha
