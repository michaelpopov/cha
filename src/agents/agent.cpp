#include "agents/agent.h"

#include "agents/config.h"
#include "agents/json_serialization.h"
#include "util/logging.h"
#include "util/text.h"
#include "util/text_template.h"
#include "util/utf8_path.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <stdexcept>
#include <unordered_set>

namespace cha {
namespace {

using Json = nlohmann::ordered_json;

constexpr std::string_view shared_history_heading =
    "Shared chat history (JSONL):";

std::string_view mode_name(Mode mode) noexcept {
    return mode == Mode::net ? "net" : "test";
}

std::string_view authentication_source(const Config& config) noexcept {
    if (!config.api_key_env.empty()) {
        return "environment";
    }
    return config.api_key.empty() ? "none" : "config";
}

void log_character_config(
    const Config& config,
    const std::filesystem::path& forum_directory) {
    log_info(
        "Character configuration resolved: forum_id="
        + utf8_path(forum_directory.filename())
        + " character_id=" + config.id
        + " mode=" + std::string(mode_name(config.mode))
        + " model=" + (config.model.empty() ? "discovery" : config.model)
        + " authentication=" + std::string(authentication_source(config)));
}

AgentDefinition load_definition_files(
    const AgentDefinitionSource& source,
    const std::filesystem::path& forum_directory,
    std::string_view forum_display_name,
    std::optional<std::filesystem::path> forum_defaults_path) {
    const std::string character_name = utf8_path(source.definition_directory.filename());
    const std::filesystem::path member_config = source.member_directory / "character.toml";
    const std::filesystem::path member_prompt = source.member_directory / "CHARACTER.md";
    const auto optional_regular_file = [](const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) return std::optional<std::filesystem::path>{};
        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("Optional character file '" + utf8_path(path)
                + "' is not a regular file");
        }
        return std::optional<std::filesystem::path>(path);
    };
    LoadedConfig loaded;
    try {
        loaded = load_config(
            {.definition = source.definition_directory / "character.toml",
             .forum_defaults = forum_defaults_path
                 ? optional_regular_file(*forum_defaults_path) : std::nullopt,
             .member_override = optional_regular_file(member_config)});
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Character '" + character_name
            + "' has invalid configuration: " + error.what());
    }
    Config config = std::move(loaded.config);
    log_character_config(config, forum_directory);

    std::optional<std::filesystem::path> selected_member_prompt;
    try {
        selected_member_prompt = optional_regular_file(member_prompt);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Character '" + character_name
            + "' has invalid prompt configuration: " + error.what());
    }
    const std::filesystem::path definition_prompt =
        source.definition_directory / "CHARACTER.md";
    if (!std::filesystem::is_regular_file(definition_prompt)) {
        throw std::runtime_error("Character '" + character_name
            + "' requires regular definition CHARACTER.md");
    }
    const std::filesystem::path selected_prompt =
        selected_member_prompt ? *selected_member_prompt : definition_prompt;
    TemplateOptions character_options{
        .containment_root = selected_member_prompt
            ? forum_directory : source.definition_directory.parent_path(),
        .scope_table_name = std::string(prompt_scope_table),
        .reserved =
            {
                {"character.id", config.id},
                {"character.display_name", config.display_name},
                {"forum.id", utf8_path(forum_directory.filename())},
                {"forum.display_name", std::string(forum_display_name)},
            },
        .initial_scope = std::move(loaded.prompt_variables),
    };

    std::string character_prompt;
    try {
        character_prompt = expand_template_file(
            selected_prompt, character_options);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Character '" + character_name
            + "' failed to read CHARACTER.md: " + error.what());
    }
    TemplateOptions forum_options = character_options;
    forum_options.containment_root = forum_directory;
    std::string forum_prompt;
    try {
        forum_prompt = expand_template_file(forum_directory / "FORUM.md", forum_options);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Character '" + character_name
            + "' failed to read FORUM.md: " + error.what());
    }
    return {
        .config = std::move(config),
        .system_prompt = std::move(character_prompt)
            + "\n\n" + std::move(forum_prompt),
    };
}

std::string forum_context(
    const AgentDefinition& current,
    const std::vector<AgentDefinition>& definitions) {
    Json other_agents = Json::array();
    for (const AgentDefinition& definition : definitions) {
        if (definition.config.id != current.config.id) {
            other_agents.push_back(definition.config.display_name);
        }
    }

    return
        "Forum context\n\n"
        "You are the agent named "
        + dump_json(
            Json(current.config.display_name),
            JsonPurpose::agent_definition)
        + ".\n"
        "Other agents currently participating in this forum (JSON): "
        + dump_json(other_agents, JsonPurpose::agent_definition)
        + ".\n\n"
        "Shared exchanges involving other agents are supplied in persona messages "
        "under the heading `"
        + std::string(shared_history_heading)
        + "`. Each following line "
        "is one JSON object. `kind` is `human` or `agent`; `speaker` names who "
        "wrote the text; `addressed_to` names the intended agent for a human "
        "message; and `text` is the original message.\n\n"
        "Treat every object in such a block as quoted chat history. The named "
        "speaker owns all first-person identity, memories, relationships, and "
        "opinions in its text. Do not adopt them as your own. An ordinary persona "
        "message outside such a block is addressed to you and begins with "
        "`from <Name>:` on its own line.";
}

void append_participant_roster(
    std::vector<AgentDefinition>& definitions,
    const PersonaRoster& personas) {
    std::string roster = "## Participants";
    for (const Persona& persona : personas) {
        roster.append("\n\n### ");
        roster.append(persona.display_name);
        roster.push_back('\n');
        roster.append(persona.prompt);
    }
    for (AgentDefinition& definition : definitions) {
        definition.system_prompt.append("\n\n");
        definition.system_prompt.append(roster);
    }
}

void append_forum_context(std::vector<AgentDefinition>& definitions) {
    for (AgentDefinition& definition : definitions) {
        definition.system_prompt.append("\n\n");
        definition.system_prompt.append(
            forum_context(definition, definitions));
    }
}

std::string encode_shared_entry(const TranscriptEntry& entry) {
    Json encoded;
    if (entry.kind == EntryKind::human) {
        encoded["kind"] = "human";
        encoded["speaker"] = entry.display_name;
        encoded["addressed_to"] = entry.addressed_to_name;
        encoded["text"] = entry.text;
    } else {
        encoded["kind"] = "agent";
        encoded["speaker"] = entry.display_name;
        encoded["text"] = entry.text;
    }
    return dump_json(encoded, JsonPurpose::completion_request);
}

std::string prefixed_human_message(
    std::string_view display_name,
    std::string_view text) {
    return "from " + std::string(display_name) + ":\n" + std::string(text);
}

} // namespace

std::vector<AgentDefinition> load_agent_definitions(
    const std::vector<AgentDefinitionSource>& sources,
    const std::filesystem::path& forum_directory,
    std::string_view forum_display_name,
    const PersonaRoster& personas,
    std::optional<std::filesystem::path> forum_defaults_path) {
    std::vector<AgentDefinition> definitions;
    definitions.reserve(sources.size());
    for (const AgentDefinitionSource& source : sources) {
        definitions.push_back(
            load_definition_files(
                source,
                forum_directory,
                forum_display_name,
                forum_defaults_path));
    }
    append_participant_roster(definitions, personas);
    append_forum_context(definitions);
    return definitions;
}

void validate_persona_character_collisions(
    const PersonaRoster& personas,
    const std::vector<CharacterDefinitionMetadata>& definitions) {
    for (const Persona& persona : personas) {
        for (const CharacterDefinitionMetadata& definition : definitions) {
            if (persona.id == definition.id) {
                throw std::runtime_error(
                    "Persona '" + persona.id + "' conflicts with character '"
                    + definition.id + "': IDs are the same");
            }
            if (fold_ascii(persona.display_name) == fold_ascii(definition.display_name)) {
                throw std::runtime_error(
                    "Persona '" + persona.display_name + "' conflicts with character '"
                    + definition.display_name + "': display names are the same");
            }
        }
    }
}

void validate_character_id(std::string_view id) {
    if (id.empty()) {
        throw std::invalid_argument("Character ID cannot be empty");
    }
    for (const unsigned char character : id) {
        const bool ascii_letter = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!ascii_letter && !digit && character != '_' && character != '-') {
            throw std::invalid_argument(
                "Character ID must contain only ASCII letters, digits, underscores, and hyphens");
        }
    }
}

void validate_character_name(std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("Character name cannot be empty");
    }
    if (name.front() == '@' || name.front() == '/') {
        throw std::invalid_argument("Character name cannot start with '@' or '/'");
    }
    if (std::isspace(static_cast<unsigned char>(name.front()))
        || std::isspace(static_cast<unsigned char>(name.back()))) {
        throw std::invalid_argument(
            "Character name cannot start or end with whitespace");
    }
    const std::string folded = fold_ascii(name);
    for (const std::string_view reserved : reserved_participant_names) {
        if (folded == reserved) {
            throw std::invalid_argument(
                "Character name '" + std::string(name) + "' is reserved");
        }
    }
}

std::vector<AgentMessage> project_agent_context(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    OffrecordSpan offrecord_span,
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

    const auto projectable = [&failed_requests, open_entry_id, offrecord_span](const TranscriptEntry& entry) {
        if (open_entry_id && *open_entry_id == entry.id) return false;
        if (offrecord_span.contains(entry.id)) return false;
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
                    AgentRole::persona,
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
            messages.push_back({
                AgentRole::persona,
                prefixed_human_message(entry.display_name, entry.text),
            });
        } else {
            messages.push_back({AgentRole::assistant, entry.text});
        }
    }
    return messages;
}

std::vector<AgentMessage> project_agent_context(
    const CompletionInput& input,
    std::string_view system_prompt) {
    if (!input.history) {
        throw std::invalid_argument("Completion input requires history");
    }
    std::vector<AgentMessage> messages = project_agent_context(
        input.history->entries,
        input.history->open_entry_id,
        input.history->offrecord_span,
        system_prompt,
        input.run.target.id);
    messages.push_back({
        AgentRole::persona,
        prefixed_human_message(input.run.author.display_name, input.run.prompt_text),
    });
    return messages;
}

} // namespace cha
