#include "ui/text/application_command.h"

#include <cctype>
#include <array>
#include <optional>

namespace cha {
namespace {
bool ascii_space(char value) { return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v'; }
constexpr std::array descriptors{
    CommandDescriptor{"/iam", "/iam <persona>", "Change the current persona.", ApplicationCommandKind::iam, 1},
    CommandDescriptor{"/open", "/open <forum> <session>", "Open a session.", ApplicationCommandKind::open, 2},
    CommandDescriptor{"/create", "/create <forum> <session>", "Create and open a session.", ApplicationCommandKind::create, 2},
    CommandDescriptor{"/forums", "/forums", "List workspace forums.", ApplicationCommandKind::forums, 0},
    CommandDescriptor{"/sessions", "/sessions <forum>", "List stored sessions.", ApplicationCommandKind::sessions, 1},
    CommandDescriptor{"/personas", "/personas", "List workspace personas.", ApplicationCommandKind::personas, 0},
    CommandDescriptor{"/help", "/help", "List commands.", ApplicationCommandKind::help, 0},
    CommandDescriptor{"/clear", "/clear", "Clear the transcript.", std::nullopt, 0},
    CommandDescriptor{"/hide-on", "/hide-on", "Begin an off-record span.", std::nullopt, 0},
    CommandDescriptor{"/hide", "/hide", "Extend an off-record span.", std::nullopt, 0},
    CommandDescriptor{"/hide-off", "/hide-off", "End an off-record span.", std::nullopt, 0},
    CommandDescriptor{"/mcast", "/mcast <targets> <text>", "Send a multicast prompt.", std::nullopt, 0},
    CommandDescriptor{"/info", "/info", "Show session information.", std::nullopt, 0},
    CommandDescriptor{"/agents", "/agents", "List forum agents.", std::nullopt, 0},
    CommandDescriptor{"/@Name", "/@Name", "Set the default agent.", std::nullopt, 0},
    CommandDescriptor{"/stop", "/stop", "Stop generation.", std::nullopt, 0},
    CommandDescriptor{"/exit", "/exit", "Exit the application.", std::nullopt, 0},
};

const CommandDescriptor* application_descriptor(std::string_view name) {
    for (const CommandDescriptor& descriptor : descriptors) {
        if (descriptor.application_kind && descriptor.name == name) return &descriptor;
    }
    return nullptr;
}
}

std::span<const CommandDescriptor> command_descriptors() { return descriptors; }

std::string command_names() {
    std::string result;
    for (const CommandDescriptor& descriptor : descriptors) {
        if (!result.empty()) result += ", ";
        result += descriptor.name;
    }
    return result;
}

std::optional<ApplicationCommandParseResult> parse_application_command(std::string_view input) {
    if (!input.starts_with('/')) return std::nullopt;
    std::size_t pos{};
    while (pos < input.size() && !ascii_space(input[pos])) ++pos;
    const CommandDescriptor* descriptor = application_descriptor(input.substr(0, pos));
    if (descriptor == nullptr) return std::nullopt;
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
    if (names.size() < descriptor->arity) return ApplicationCommandParseError::missing_argument;
    if (names.size() > descriptor->arity) return ApplicationCommandParseError::extra_argument;
    return ApplicationCommand{*descriptor->application_kind, std::move(names)};
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
