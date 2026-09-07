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
    // A character not yet assigned to a forum may remain a draft until its
    // provider is selected in Settings.
    std::optional<std::string> provider_id;
    std::optional<std::string> style_id;
    std::optional<std::string> reasoning_effort;
    std::optional<WebSearchMode> web_search;
    WorkspacePromptVariables prompt_variables;
    std::string prompt_template;
    std::string markdown;
    std::string editable_markdown;
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

enum class HandleMatch { resolved, unknown, ambiguous };

struct HandleResolution {
    HandleMatch match{HandleMatch::unknown};
    const CharacterMetadata* character{};
    std::vector<const CharacterMetadata*> candidates;
};

// A self-contained, eagerly loaded and validated view of workspace/. It owns
// every value it publishes and never exposes filesystem-backed references.
class Workspace final {
public:
    static Workspace load(std::filesystem::path root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
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
    [[nodiscard]] const CharacterMetadata* find_forum_character(
        std::string_view forum_id,
        std::string_view character_id) const noexcept;
    [[nodiscard]] HandleResolution resolve_forum_handle(
        std::string_view forum_id,
        std::string_view handle) const;
    [[nodiscard]] std::string forum_handle_list(
        std::string_view forum_id) const;
    [[nodiscard]] CharacterDefinition character_definition(
        std::string_view forum_id,
        std::string_view character_id) const;
    [[nodiscard]] bool character_is_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool persona_is_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool forum_is_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool provider_is_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool style_is_writable(
        std::string_view id) const noexcept;

    void write_provider(
        std::string_view provider_id,
        std::string_view display_name,
        const ModelBackendConfig& config) const;
    void create_provider(
        std::string_view provider_id,
        std::string_view display_name) const;
    void delete_provider(std::string_view provider_id) const;
    void write_style(
        std::string_view style_id,
        std::string_view display_name,
        const CharacterAppearance& appearance) const;
    void create_style(
        std::string_view style_id,
        std::string_view display_name) const;
    void delete_style(std::string_view style_id) const;

    void write_character_settings(
        std::string_view character_id,
        std::string_view provider_id,
        std::optional<std::string_view> style_id,
        std::optional<std::string_view> reasoning_effort = std::nullopt,
        std::optional<WebSearchMode> web_search = std::nullopt) const;
    void write_character_definition(
        std::string_view character_id,
        std::string_view display_name,
        std::optional<std::string_view> markdown = std::nullopt) const;
    void delete_character(std::string_view character_id) const;
    void write_persona(
        std::string_view persona_id,
        std::string_view display_name,
        std::string_view markdown) const;
    void delete_persona(std::string_view persona_id) const;
    void create_persona(
        std::string_view persona_id,
        std::string_view display_name) const;
    void create_character(
        std::string_view character_id,
        std::string_view display_name,
        std::string_view description) const;
    void create_forum(
        std::string_view forum_id,
        std::string_view display_name,
        std::string_view persona_id) const;
    void write_forum(
        std::string_view forum_id,
        std::string_view display_name,
        std::string_view markdown) const;
    void delete_forum(std::string_view forum_id) const;
    void write_forum_members(
        std::string_view forum_id,
        std::span<const std::string> character_ids) const;
    void write_forum_default_character(
        std::string_view forum_id,
        std::string_view character_id) const;
    void write_forum_default_persona(
        std::string_view forum_id,
        std::string_view persona_id) const;

private:
    std::filesystem::path root_;
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
        persona_directories_;
    std::unordered_map<std::string, std::filesystem::path>
        forum_config_paths_;
    std::unordered_map<std::string, std::filesystem::path>
        provider_config_paths_;
    std::unordered_map<std::string, std::filesystem::path>
        style_config_paths_;
};

[[nodiscard]] std::shared_ptr<const Workspace> getws();
void loadws(const std::filesystem::path& root);
void loadws(Workspace workspace);

} // namespace cha
