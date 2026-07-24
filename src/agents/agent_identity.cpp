#include "agents/agent_identity.h"

#include "util/text.h"

#include <cctype>
#include <stdexcept>

namespace cha {

void validate_agent_id(std::string_view id) {
    if (id.empty()) {
        throw std::invalid_argument("Agent ID cannot be empty");
    }
    for (const unsigned char character : id) {
        const bool ascii_letter = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!ascii_letter && !digit && character != '_' && character != '-') {
            throw std::invalid_argument(
                "Agent ID must contain only ASCII letters, digits, underscores, and hyphens");
        }
    }
}

void validate_agent_name(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("Agent name cannot be empty");
    }
    if (name.front() == '@' || name.front() == '/') {
        throw std::invalid_argument("Agent name cannot start with '@' or '/'");
    }
    for (const unsigned char character : name) {
        if (std::isspace(character)) {
            throw std::invalid_argument("Agent name cannot contain whitespace");
        }
    }
    if (fold_ascii(name) == "user") {
        throw std::invalid_argument("Agent name 'User' is reserved");
    }
}

} // namespace cha
