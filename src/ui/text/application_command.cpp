#include "ui/text/application_command.h"

#include <cctype>
#include <optional>

namespace cha {
namespace {
bool ascii_space(char value) { return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v'; }
std::optional<ApplicationCommandKind> kind_for(std::string_view name) {
    if (name == "/iam") return ApplicationCommandKind::iam;
    if (name == "/open") return ApplicationCommandKind::open;
    if (name == "/create") return ApplicationCommandKind::create;
    if (name == "/forums") return ApplicationCommandKind::forums;
    if (name == "/sessions") return ApplicationCommandKind::sessions;
    if (name == "/personas") return ApplicationCommandKind::personas;
    if (name == "/help") return ApplicationCommandKind::help;
    return std::nullopt;
}
std::size_t arity(ApplicationCommandKind kind) {
    switch (kind) { case ApplicationCommandKind::iam: case ApplicationCommandKind::sessions: return 1; case ApplicationCommandKind::open: case ApplicationCommandKind::create: return 2; default: return 0; }
}
}

std::optional<ApplicationCommandParseResult> parse_application_command(std::string_view input) {
    if (!input.starts_with('/')) return std::nullopt;
    std::size_t pos{};
    while (pos < input.size() && !ascii_space(input[pos])) ++pos;
    const auto kind = kind_for(input.substr(0, pos));
    if (!kind) return std::nullopt;
    std::vector<std::string> names;
    while (pos < input.size()) {
        while (pos < input.size() && ascii_space(input[pos])) ++pos;
        if (pos == input.size()) break;
        std::string value;
        if (input[pos] == '"') {
            ++pos;
            bool closed{};
            while (pos < input.size()) {
                const char current = input[pos++];
                if (current == '"') { closed = true; break; }
                if (current == '\\') {
                    if (pos == input.size()) return ApplicationCommandParseError::unsupported_escape;
                    const char escaped = input[pos++];
                    if (escaped != '"' && escaped != '\\') return ApplicationCommandParseError::unsupported_escape;
                    value.push_back(escaped);
                } else value.push_back(current);
            }
            if (!closed) return ApplicationCommandParseError::unmatched_quote;
            if (pos < input.size() && !ascii_space(input[pos])) return ApplicationCommandParseError::extra_argument;
            if (value.empty()) return ApplicationCommandParseError::empty_name;
        } else {
            while (pos < input.size() && !ascii_space(input[pos])) value.push_back(input[pos++]);
        }
        names.push_back(std::move(value));
    }
    if (names.size() < arity(*kind)) return ApplicationCommandParseError::missing_argument;
    if (names.size() > arity(*kind)) return ApplicationCommandParseError::extra_argument;
    return ApplicationCommand{*kind, std::move(names)};
}
std::string_view application_command_parse_error_message(ApplicationCommandParseError error) {
    switch (error) {
    case ApplicationCommandParseError::unmatched_quote: return "Unmatched double quote";
    case ApplicationCommandParseError::unsupported_escape: return "Only \\\" and \\\\ escapes are supported";
    case ApplicationCommandParseError::empty_name: return "Quoted names cannot be empty";
    case ApplicationCommandParseError::missing_argument: return "Command is missing a required name";
    case ApplicationCommandParseError::extra_argument: return "Command has too many arguments";
    }
    return "Invalid command";
}
} // namespace cha
