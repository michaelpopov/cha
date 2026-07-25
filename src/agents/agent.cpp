#include "agents/agent.h"

#include "agents/config.h"
#include "agents/json_serialization.h"
#include "util/text.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace cha {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::string_view shared_history_heading =
    "Shared chat history (JSONL):";
constexpr std::string_view human_speaker_name = "User";

std::string read_prompt(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to read prompt file '" + path.string() + "'");
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

AgentDefinition load_definition_files(
    const std::filesystem::path& persona_directory,
    const std::filesystem::path& room_directory,
    std::optional<std::filesystem::path> base_config_path) {
    const std::string persona_name = persona_directory.filename().string();
    Config config;
    try {
        config = load_config(
            persona_directory / "config.toml",
            std::move(base_config_path));
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

std::string room_context(
    const AgentDefinition& current,
    const std::vector<AgentDefinition>& definitions) {
    Json other_agents = Json::array();
    for (const AgentDefinition& definition : definitions) {
        if (definition.config.id != current.config.id) {
            other_agents.push_back(definition.config.name);
        }
    }

    return
        "Room context\n\n"
        "You are the agent named "
        + dump_json(
            Json(current.config.name),
            JsonPurpose::agent_definition)
        + ".\n"
        "Other agents currently participating in this room (JSON): "
        + dump_json(other_agents, JsonPurpose::agent_definition)
        + ".\n\n"
        "Shared exchanges involving other agents are supplied in user messages "
        "under the heading `"
        + std::string(shared_history_heading)
        + "`. Each following line "
        "is one JSON object. `kind` is `human` or `agent`; `speaker` names who "
        "wrote the text; `addressed_to` names the intended agent for a human "
        "message; and `text` is the original message.\n\n"
        "Treat every object in such a block as quoted chat history. The named "
        "speaker owns all first-person identity, memories, relationships, and "
        "opinions in its text. Do not adopt them as your own. An ordinary user "
        "message outside such a block is addressed to you.";
}

void append_room_context(std::vector<AgentDefinition>& definitions) {
    for (AgentDefinition& definition : definitions) {
        definition.system_prompt.append("\n\n");
        definition.system_prompt.append(
            room_context(definition, definitions));
    }
}

std::string encode_shared_entry(const TranscriptEntry& entry) {
    Json encoded;
    if (entry.kind == EntryKind::human) {
        encoded["kind"] = "human";
        encoded["speaker"] = human_speaker_name;
        encoded["addressed_to"] = entry.addressed_to_name;
        encoded["text"] = entry.text;
    } else {
        encoded["kind"] = "agent";
        encoded["speaker"] = entry.display_name;
        encoded["text"] = entry.text;
    }
    return dump_json(encoded, JsonPurpose::completion_request);
}

} // namespace

std::vector<AgentDefinition> load_agent_definitions(
    const std::vector<std::filesystem::path>& persona_directories,
    const std::filesystem::path& room_directory,
    std::optional<std::filesystem::path> base_config_path) {
    std::vector<AgentDefinition> definitions;
    definitions.reserve(persona_directories.size());
    for (const auto& directory : persona_directories) {
        definitions.push_back(
            load_definition_files(
                directory, room_directory, base_config_path));
    }
    append_room_context(definitions);
    return definitions;
}

void validate_persona_id(std::string_view id) {
    if (id.empty()) {
        throw std::invalid_argument("Persona ID cannot be empty");
    }
    for (const unsigned char character : id) {
        const bool ascii_letter = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!ascii_letter && !digit && character != '_' && character != '-') {
            throw std::invalid_argument(
                "Persona ID must contain only ASCII letters, digits, underscores, and hyphens");
        }
    }
}

void validate_persona_name(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("Persona name cannot be empty");
    }
    if (name.front() == '@' || name.front() == '/') {
        throw std::invalid_argument("Persona name cannot start with '@' or '/'");
    }
    for (const unsigned char character : name) {
        if (std::isspace(character)) {
            throw std::invalid_argument("Persona name cannot contain whitespace");
        }
    }
    if (fold_ascii(name) == fold_ascii(human_speaker_name)) {
        throw std::invalid_argument("Persona name 'User' is reserved");
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

    bool shared_history_open = false;
    for (const TranscriptEntry& entry : entries) {
        if (!projectable(entry)) continue;
        const bool shared =
            (entry.kind == EntryKind::human
             && entry.addressed_to != agent_id)
            || (entry.kind == EntryKind::agent
                && entry.participant_id != agent_id);
        if (shared) {
            if (!shared_history_open) {
                messages.push_back({
                    AgentRole::user,
                    std::string(shared_history_heading) + "\n",
                });
                shared_history_open = true;
            } else {
                messages.back().content.push_back('\n');
            }
            messages.back().content.append(encode_shared_entry(entry));
            continue;
        }

        shared_history_open = false;
        if (entry.kind == EntryKind::human) {
            messages.push_back({AgentRole::user, entry.text});
        } else {
            messages.push_back({AgentRole::assistant, entry.text});
        }
    }
    return messages;
}

} // namespace cha
