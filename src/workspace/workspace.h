#pragma once

#include "characters/character.h"
#include "chat/persona.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cha {

inline constexpr std::string_view workspace_guest_id = "builtin-guest";
inline constexpr std::string_view workspace_assistant_id = "builtin-assistant";
inline constexpr std::string_view workspace_entrance_id = "builtin-entrance";

using WorkspacePromptVariables =
    std::map<std::string, std::string, std::less<>>;

struct WorkspaceSettings {
    std::filesystem::path log_file;
    std::string log_level;
};

// One complete provider configuration. Defaults have already been applied.
struct WorkspaceProvider {
    std::string id;
    std::string label;
    ModelBackendConfig config;
};

struct WorkspaceStyle {
    std::string id;
    std::string label;
    CharacterAppearance appearance;
};

using WorkspacePersona = Persona;

// A user-defined or built-in character. Forum-specific overrides and prompt
// expansion are stored on WorkspaceForumMember.
struct WorkspaceCharacter {
    CharacterMetadata character;
    std::string provider_id;
    std::optional<std::string> style_id;
    WorkspacePromptVariables prompt_variables;
    std::string prompt_template;
    std::string markdown;
};

struct WorkspaceForumMember {
    std::string character_id;
    WorkspacePromptVariables prompt_variables;
    std::optional<std::string> prompt_override;
    std::string character_prompt;
    std::string system_prompt;
};

struct WorkspaceForum {
    std::string id;
    std::string display_name;
    std::optional<std::string> description;
    std::string default_character_id;
    std::string default_persona_id;
    std::string prompt_template;
    std::vector<WorkspaceForumMember> members;
};

// A self-contained, eagerly loaded and validated view of workspace/. It owns
// every value it publishes and never exposes filesystem-backed references.
class Workspace final {
public:
    static Workspace load(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }
    [[nodiscard]] const WorkspaceSettings& settings() const noexcept {
        return settings_;
    }
    [[nodiscard]] std::span<const WorkspaceProvider> providers() const noexcept {
        return providers_;
    }
    [[nodiscard]] std::span<const WorkspaceStyle> styles() const noexcept {
        return styles_;
    }
    [[nodiscard]] std::span<const WorkspacePersona> personas() const noexcept {
        return personas_;
    }
    [[nodiscard]] std::span<const WorkspaceCharacter> characters() const noexcept {
        return characters_;
    }
    [[nodiscard]] std::span<const WorkspaceForum> forums() const noexcept {
        return forums_;
    }

    [[nodiscard]] const WorkspaceProvider* find_provider(
        std::string_view id) const noexcept;
    [[nodiscard]] const WorkspaceStyle* find_style(
        std::string_view id) const noexcept;
    [[nodiscard]] const WorkspacePersona* find_persona(
        std::string_view id) const noexcept;
    [[nodiscard]] const WorkspaceCharacter* find_character(
        std::string_view id) const noexcept;
    [[nodiscard]] const WorkspaceForum* find_forum(
        std::string_view id) const noexcept;
    [[nodiscard]] const WorkspaceForumMember* find_forum_member(
        std::string_view forum_id,
        std::string_view character_id) const noexcept;
    [[nodiscard]] CharacterDefinition character_definition(
        std::string_view forum_id,
        std::string_view character_id) const;
    [[nodiscard]] std::optional<std::filesystem::path> forum_session_directory(
        std::string_view forum_id) const;
    [[nodiscard]] bool character_is_writable(
        std::string_view id) const noexcept;

    void write_character_settings(
        std::string_view character_id,
        std::string_view provider_id,
        std::optional<std::string_view> style_id) const;
    void write_forum_default_character(
        std::string_view forum_id,
        std::string_view character_id) const;
    void write_forum_default_persona(
        std::string_view forum_id,
        std::string_view persona_id) const;

private:
    std::filesystem::path root_;
    WorkspaceSettings settings_;
    std::vector<WorkspaceProvider> providers_;
    std::vector<WorkspaceStyle> styles_;
    std::vector<WorkspacePersona> personas_;
    std::vector<WorkspaceCharacter> characters_;
    std::vector<WorkspaceForum> forums_;
    std::unordered_map<std::string, std::size_t> provider_index_;
    std::unordered_map<std::string, std::size_t> style_index_;
    std::unordered_map<std::string, std::size_t> persona_index_;
    std::unordered_map<std::string, std::size_t> character_index_;
    std::unordered_map<std::string, std::size_t> forum_index_;
    std::unordered_map<std::string, std::filesystem::path>
        character_config_paths_;
    std::unordered_map<std::string, std::filesystem::path>
        forum_config_paths_;
};

[[nodiscard]] std::shared_ptr<const Workspace> getws();
void loadws(const std::filesystem::path& root);
void loadws(Workspace workspace);

} // namespace cha
