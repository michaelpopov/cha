#pragma once

#include "agents/character_config.h"
#include "chat/persona.h"

#include <filesystem>
#include <memory>
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
// the private model-backend configuration used to execute it.
struct CharacterDefinition {
    CharacterMetadata character;
    ProviderSelection provider;
    // The expanded character-level prompt before forum context is appended.
    std::string character_prompt;
    // The public character description shown by the web UI. When the expanded
    // prompt contains a <character_profile> section, this is that section;
    // otherwise it is the full character prompt.
    std::string character_description;
    std::string system_prompt;
};

// Public configured runtime information for one character. It is derived from
// immutable character definitions and is safe to show in diagnostics.
struct CharacterRuntimeInfo {
    CharacterMetadata character;
    std::string model;
    std::string api;
    bool streaming{};
};

// The immutable character snapshot shared by a session, its request-local
// clients, and eventually request-owned provider work.
using SharedCharacterDefinition = std::shared_ptr<const CharacterDefinition>;

// Converts loaded definitions to the shared immutable form while preserving
// the forum roster order.
std::vector<SharedCharacterDefinition> share_character_definitions(
    std::vector<CharacterDefinition> definitions);

// The two filesystem layers that form one forum member's effective character.
struct CharacterDefinitionSource {
    std::filesystem::path definition_directory;
    std::filesystem::path member_directory;
    // The workspace characters/ directory. Empty keeps the legacy behavior
    // for callers that provide only a standalone definition directory.
    std::filesystem::path definition_containment_root;
};

// Returns the public part of an expanded character prompt. When a profile
// section is present, private portrayal instructions outside it are omitted.
std::string character_description_from_prompt(std::string_view prompt);

// Loads the forum's character configurations in order and assembles each effective system prompt.
std::vector<CharacterDefinition> load_character_definitions(
    const std::vector<CharacterDefinitionSource>& sources,
    const std::filesystem::path& forum_directory,
    std::string_view forum_display_name,
    const PersonaRoster& personas,
    std::optional<std::filesystem::path> forum_defaults_path = std::nullopt,
    std::optional<std::filesystem::path> providers_directory = std::nullopt,
    std::optional<std::filesystem::path> styles_directory = std::nullopt);

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
