#pragma once

#include "runtime/protocol.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cha {

class SessionController;

enum class CommandKind {
    text,
    mcast,
    unknown,
};

struct Command {
    CommandKind kind{CommandKind::text};
    std::string argument;
};

struct AddressedPrompt {
    std::string handle;
    std::string text;
};

struct MulticastInput {
    std::vector<std::string> handles;
    std::string text;

    bool operator==(const MulticastInput&) const = default;
};

enum class MulticastParseError {
    empty_prompt,
    empty_handle,
    unexpected_comma,
    missing_separator,
};

using MulticastParseResult =
    std::variant<MulticastInput, MulticastParseError>;

[[nodiscard]] Command parse_command(std::string_view input);
[[nodiscard]] std::string command_names();
[[nodiscard]] AddressedPrompt parse_addressed_prompt(std::string_view input);
[[nodiscard]] MulticastParseResult parse_multicast_input(
    std::string_view argument);
[[nodiscard]] std::string_view multicast_parse_error_message(
    MulticastParseError error);

// Translates the web chat grammar into typed session-layer calls and the
// command result returned to the bridge.
[[nodiscard]] CommandResult handle_text_input(
    SessionController& controller,
    std::string_view author_id,
    std::string input);

} // namespace cha
