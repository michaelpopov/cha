#pragma once

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

enum class WorkspaceProviderMode { net, test };
enum class WorkspaceProviderApi { chat_completions, responses };
enum class WorkspaceWebSearch { off, automatic, required };
enum class WorkspaceCacheRetention { off, short_, long_ };
enum class WorkspaceReasoningFormat {
    automatic,
    none,
    reasoning_content,
    reasoning,
};

enum class WorkspaceFont { sans, serif, mono };
enum class WorkspaceSlant { normal, italic };
enum class WorkspaceWeight { light, normal, medium, semibold, bold };
enum class WorkspaceTextSize { small, normal, large };
enum class WorkspaceTextColor { normal, muted, accent };

struct WorkspaceSettings {
    std::filesystem::path log_file;
    std::string log_level;
};

// One complete provider configuration. Defaults have already been applied.
struct WorkspaceProvider {
    std::string id;
    std::string label;
    std::string host;
    int port{};
    std::string base_path;
    WorkspaceProviderMode mode{WorkspaceProviderMode::test};
    std::string model;
    bool stream{true};
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    int timeout_s{600};
    int idle_timeout_s{60};
    std::string api_key_env;
    std::string reasoning_effort;
    WorkspaceReasoningFormat reasoning_format{
        WorkspaceReasoningFormat::automatic};
    bool https{};
    WorkspaceProviderApi api{WorkspaceProviderApi::responses};
    WorkspaceWebSearch web_search{WorkspaceWebSearch::required};
    WorkspaceCacheRetention cache_retention{
        WorkspaceCacheRetention::short_};
};

struct WorkspaceAppearance {
    WorkspaceFont font{WorkspaceFont::sans};
    WorkspaceSlant slant{WorkspaceSlant::normal};
    WorkspaceWeight weight{WorkspaceWeight::normal};
    WorkspaceTextSize size{WorkspaceTextSize::normal};
    WorkspaceTextColor text_color{WorkspaceTextColor::normal};
};

struct WorkspaceStyle {
    std::string id;
    std::string label;
    WorkspaceAppearance appearance;
};

struct WorkspacePersona {
    std::string id;
    std::string display_name;
    std::optional<std::string> description;
    std::string prompt;
};

// A user-defined or built-in character. Forum-specific overrides and prompt
// expansion are stored on WorkspaceForumMember.
struct WorkspaceCharacter {
    std::string id;
    std::string display_name;
    std::optional<std::string> description;
    std::vector<std::string> tags;
    std::string provider_id;
    std::optional<std::string> style_id;
    WorkspaceAppearance appearance;
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

} // namespace cha
