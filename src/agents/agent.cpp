#include "agents/agent.h"

#include "agents/config.h"
#include "util/text.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace cha {
namespace {

std::string read_prompt(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to read prompt file '" + path.string() + "'");
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

AgentDefinition load_agent_definition(
    const std::filesystem::path& persona_directory,
    const std::filesystem::path& room_directory) {
    const std::string persona_name = persona_directory.filename().string();
    Config config;
    try {
        config = load_config(persona_directory / "config.toml");
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Persona '" + persona_name
            + "' has invalid configuration: " + error.what());
    }
    std::string persona_prompt;
    try {
        persona_prompt = read_prompt(persona_directory / "SYSTEM.md");
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Persona '" + persona_name
            + "' failed to read SYSTEM.md: " + error.what());
    }
    std::string room_prompt;
    try {
        room_prompt = read_prompt(room_directory / "USER.md");
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Room '" + room_directory.filename().string()
            + "' failed to read USER.md: " + error.what());
    }
    return {
        .config = std::move(config),
        .system_prompt = std::move(persona_prompt)
            + "\n\n" + std::move(room_prompt),
    };
}

std::vector<AgentDefinition> load_agent_definitions(
    const std::vector<std::filesystem::path>& persona_directories,
    const std::filesystem::path& room_directory) {
    std::vector<AgentDefinition> definitions;
    definitions.reserve(persona_directories.size());
    for (const auto& directory : persona_directories) {
        definitions.push_back(
            load_agent_definition(directory, room_directory));
    }
    return definitions;
}

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

std::vector<AgentMessage> project_agent_context(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    std::string_view system_prompt,
    std::string_view agent_id) {
    std::vector<AgentMessage> messages;
    if (!system_prompt.empty()) {
        messages.push_back({AgentRole::system, std::string(system_prompt)});
    }

    std::unordered_set<RequestId> failed_requests;
    for (const TranscriptEntry& entry : entries) {
        if (entry.kind == EntryKind::error && entry.request_id) {
            failed_requests.insert(*entry.request_id);
        }
    }

    const auto projectable = [&failed_requests, open_entry_id](const TranscriptEntry& entry) {
        if (open_entry_id && *open_entry_id == entry.id) return false;
        if (entry.kind == EntryKind::notice || entry.kind == EntryKind::error) return false;
        if (entry.kind == EntryKind::human) return !entry.request_id || !failed_requests.contains(*entry.request_id);
        return entry.status == EntryStatus::complete && !entry.text.empty();
    };

    bool attributed = false;
    for (const TranscriptEntry& entry : entries) {
        if (!projectable(entry)) continue;
        attributed = attributed || (entry.kind == EntryKind::agent && entry.participant_id != agent_id)
            || (entry.kind == EntryKind::human && entry.addressed_to != agent_id);
    }

    bool previous_foreign = false;
    for (const TranscriptEntry& entry : entries) {
        if (!projectable(entry)) continue;
        const bool foreign = entry.kind == EntryKind::agent && entry.participant_id != agent_id;
        const AgentRole role = entry.kind == EntryKind::human || foreign
            ? AgentRole::user : AgentRole::assistant;
        const bool coalesce = !messages.empty()
            && role == AgentRole::user
            && messages.back().role == AgentRole::user
            && (previous_foreign || foreign);
        if (!coalesce) {
            messages.push_back({role, {}});
        } else {
            messages.back().content.append("\n\n");
        }

        std::string& content = messages.back().content;
        if (entry.kind == EntryKind::human) {
            if (attributed) {
                content.append("User: ");
                if (entry.addressed_to != agent_id) {
                    content.append("[to ");
                    content.append(entry.addressed_to_name);
                    content.append("] ");
                }
            }
            content.append(entry.text);
        } else if (foreign) {
            content.append(entry.display_name);
            content.append(": ");
            content.append(entry.text);
        } else {
            content.append(entry.text);
        }
        previous_foreign = foreign;
    }
    return messages;
}

} // namespace cha
