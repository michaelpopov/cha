#pragma once

#include <string>
#include <variant>

namespace cha {

class WakeNotifier;

// Controls transcript styling only; machine-readable listings remain plain.
enum class ColorMode {
    automatic,
    always,
    never,
};

// The filesystem-independent result of parsing chacon's remaining frontend
// flags. Entity navigation belongs to ChatApplication slash commands.
struct ConsoleOptions {
    bool check{};
    ColorMode color{ColorMode::automatic};
};

// A command-line usage failure kept distinct from runtime workspace errors so
// console_main can preserve the exit-code contract.
struct ArgumentError {
    std::string message;
    int exit_code{2};
};

// Parses argv without touching the workspace, returning exit-code-2 failures
// as values rather than throwing.
std::variant<ConsoleOptions, ArgumentError> parse_console_arguments(
    int argc,
    const char* const* argv);

} // namespace cha
