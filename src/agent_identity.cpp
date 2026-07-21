#include "agent_identity.h"

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
    if (name.size() == 4
        && std::tolower(static_cast<unsigned char>(name[0])) == 'u'
        && std::tolower(static_cast<unsigned char>(name[1])) == 's'
        && std::tolower(static_cast<unsigned char>(name[2])) == 'e'
        && std::tolower(static_cast<unsigned char>(name[3])) == 'r') {
        throw std::invalid_argument("Agent name 'User' is reserved");
    }
}

std::string fold_ascii(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 128) {
            character = static_cast<char>(std::tolower(byte));
        }
    }
    return result;
}

} // namespace cha
