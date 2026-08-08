#pragma once

#include "agents/character_config.h"
#include "chat/persona.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// Participant names no configured party may claim.
inline constexpr std::string_view reserved_participant_names[] = {
    "persona", "system", "error", "human", "assistant", "agent", "character", "you", "guest",
};

// A character loaded and ready to run. Public identity remains separate from
// the private completion-provider configuration used to execute it.
struct CharacterDefinition {
    CharacterMetadata character;
    CompletionConfig completion;
    std::string system_prompt;
};

// The two filesystem layers that form one forum member's effective character.
struct CharacterDefinitionSource {
    std::filesystem::path definition_directory;
    std::filesystem::path member_directory;
};

// Loads the forum's character configurations in order and assembles each effective system prompt.
std::vector<CharacterDefinition> load_character_definitions(
    const std::vector<CharacterDefinitionSource>& sources,
    const std::filesystem::path& forum_directory,
    std::string_view forum_display_name,
    const PersonaRoster& personas,
    std::optional<std::filesystem::path> forum_defaults_path = std::nullopt,
    std::optional<ProviderConfig> application_provider = std::nullopt);

// Adds the standard participant and forum-context sections to definitions
// assembled by trusted non-filesystem factories.
void append_standard_prompt_context(
    std::vector<CharacterDefinition>& definitions,
    const PersonaRoster& personas);

void validate_character_id(std::string_view id);
// Validates the transport/runtime shape only. Workspace definitions must use
// validate_character_display_name(), which additionally enforces reserved names.
void validate_character_display_name_syntax(std::string_view display_name);
void validate_character_display_name(std::string_view display_name);
void validate_persona_character_collisions(
    const PersonaRoster& personas,
    const std::vector<CharacterMetadata>& definitions);

} // namespace cha
