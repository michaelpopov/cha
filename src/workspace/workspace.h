#pragma once

#include "util/text_source.h"

#include "characters/character.h"
#include "chat/persona.h"
#include "providers/credentials.h"

#include <cstdint>
#include <filesystem>
#include <functional>
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

struct VoiceSettings {
    std::optional<double> speed;

    bool operator==(const VoiceSettings&) const = default;
};

struct WorkspaceVoice {
    std::string id;
    std::string label;
    std::string description;
    // Legacy configuration field name; contains a FishAudio reference ID.
    std::string elevenlabs_voice_id;
    VoiceSettings settings;
};

inline constexpr std::string_view openai_voice_input_url_message =
    "OpenAI voice input requires an absolute HTTP or HTTPS URL";
inline constexpr std::string_view xai_voice_input_url_message =
    "xAI voice input requires an absolute WS or WSS URL";

struct WorkspaceVoiceInput {
    std::string provider{"openai"};
    std::string url;
    std::string model;
    std::string api_key_id;
    std::string delay{"low"};
    std::string prompt;
    std::string send_phrase{"over to you"};
};

// xAI stores delay for old files and does not send it. An invalid value becomes low.
void normalize_unused_voice_input_delay(
    std::string_view provider,
    std::string& delay);

struct WorkspaceJev {
    std::string url{"https://openrouter.ai/api/alpha/decisions"};
    std::string model{"typesafe/jev-1.13"};
    std::string api_key_id;
};

void validate_jev_config(const WorkspaceJev& config);

struct WorkspaceVoiceOutput {
    std::string url;
    std::string model;
    std::string api_key_id;
    std::string output_format;
    std::string default_voice;
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
    std::optional<std::string> voice_id;
    std::optional<std::string> reasoning_effort;
    std::optional<WebSearchMode> web_search;
    WorkspacePromptVariables prompt_variables;
    std::string prompt_template;
    std::string markdown;
    std::string editable_markdown;
    std::map<std::string, std::string, std::less<>> markdown_files;
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
    std::map<std::string, std::string, std::less<>> markdown_files;
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
    static Workspace load(std::filesystem::path root, const TextFiles& files);

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }
    [[nodiscard]] std::span<const WorkspaceProvider> providers() const noexcept {
        return providers_;
    }
    [[nodiscard]] std::span<const WorkspaceStyle> styles() const noexcept {
        return styles_;
    }
    [[nodiscard]] std::span<const WorkspaceVoice> voices() const noexcept {
        return voices_;
    }
    [[nodiscard]] const std::optional<WorkspaceVoiceInput>& voice_input()
        const noexcept {
        return voice_input_;
    }
    [[nodiscard]] const std::optional<WorkspaceVoiceOutput>& voice_output()
        const noexcept {
        return voice_output_;
    }
    [[nodiscard]] const std::optional<WorkspaceJev>& jev() const noexcept {
        return jev_;
    }
    [[nodiscard]] std::span<const SavedApiKey> api_keys() const noexcept {
        return api_keys_;
    }
    [[nodiscard]] const std::optional<R2StorageKey>& r2_storage() const noexcept {
        return r2_storage_;
    }
    [[nodiscard]] std::uint64_t next_api_key_id() const noexcept {
        return next_api_key_id_;
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
    [[nodiscard]] const WorkspaceVoice* find_voice(
        std::string_view id) const noexcept;
    [[nodiscard]] const WorkspaceVoice* find_voice_by_name(
        std::string_view name) const noexcept;
    [[nodiscard]] const SavedApiKey* find_api_key(
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
    [[nodiscard]] bool character_settings_are_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool persona_is_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool forum_is_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool provider_is_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool style_is_writable(
        std::string_view id) const noexcept;
    [[nodiscard]] bool voice_is_writable(
        std::string_view id) const noexcept;


private:
    friend class WorkspaceConfigEditor;
    static Workspace load(std::filesystem::path root, const TextSource& source);
    std::filesystem::path root_;
    std::vector<WorkspaceProvider> providers_;
    std::vector<WorkspaceStyle> styles_;
    std::vector<WorkspaceVoice> voices_;
    std::optional<WorkspaceVoiceInput> voice_input_;
    std::optional<WorkspaceVoiceOutput> voice_output_;
    std::optional<WorkspaceJev> jev_;
    std::vector<SavedApiKey> api_keys_;
    std::optional<R2StorageKey> r2_storage_;
    std::uint64_t next_api_key_id_{1};
    std::vector<WorkspacePersona> personas_;
    std::vector<WorkspaceCharacter> characters_;
    std::vector<WorkspaceForum> forums_;
    std::unordered_map<std::string, std::size_t> provider_index_;
    std::unordered_map<std::string, std::size_t> style_index_;
    std::unordered_map<std::string, std::size_t> voice_index_;
    std::unordered_map<std::string, std::size_t> api_key_index_;
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
    std::unordered_map<std::string, std::filesystem::path>
        voice_config_paths_;
};

// Returns the owner’s latest snapshot. Retain it while using references into it.
using WorkspaceReader = std::function<std::shared_ptr<const Workspace>()>;

} // namespace cha
