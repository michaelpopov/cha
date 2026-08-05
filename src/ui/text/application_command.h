#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <optional>
#include <variant>
#include <vector>

namespace cha {

// Typed grammar for fixed-arity terminal application commands.  It deliberately
// stays separate from free-form controller commands such as /mcast.
enum class ApplicationCommandKind { iam, open, create, forums, sessions, members, personas, help };
struct CommandDescriptor {
    std::string_view name;
    std::string_view syntax;
    std::string_view description;
    std::optional<ApplicationCommandKind> application_kind;
    std::size_t arity;
};
struct ApplicationCommand { ApplicationCommandKind kind; std::vector<std::string> names; };
enum class ApplicationCommandParseError { unmatched_quote, unsupported_escape, empty_name, missing_argument, extra_argument };
using ApplicationCommandParseResult = std::variant<ApplicationCommand, ApplicationCommandParseError>;

// Returns nullopt when input is not an application command.
std::optional<ApplicationCommandParseResult> parse_application_command(std::string_view input);
std::string_view application_command_parse_error_message(ApplicationCommandParseError error);
std::span<const CommandDescriptor> command_descriptors();
std::string command_names();
} // namespace cha
