#pragma once

#include "workspace/workspace.h"
#include <toml++/toml.hpp>

namespace cha {

// Edits a disposable copy of configuration rows. The store validates and commits
// the result; neither the published Workspace nor the filesystem is modified.
class WorkspaceConfigEditor {
public:
    WorkspaceConfigEditor(const Workspace& workspace, TextFiles& files)
        : workspace_(workspace), files_(files), source_(workspace.root(), files) {}

    [[nodiscard]] bool exists(const std::filesystem::path& path) const {
        return source_.exists(path);
    }
    TextFiles& files() { return files_; }

    void write_provider(
        std::string_view provider_id,
        std::string_view display_name,
        const ModelBackendConfig& config);
    void create_provider(
        std::string_view provider_id,
        std::string_view display_name,
        std::string_view copy_from = {});
    void delete_provider(std::string_view provider_id);
    void write_style(
        std::string_view style_id,
        std::string_view display_name,
        const CharacterAppearance& appearance);
    void create_style(
        std::string_view style_id,
        std::string_view display_name);
    void delete_style(std::string_view style_id);
    void write_voice(
        std::string_view voice_id,
        std::string_view display_name,
        std::string_view description,
        std::string_view elevenlabs_voice_id,
        const VoiceSettings& settings);
    void create_voice(
        std::string_view voice_id,
        std::string_view display_name,
        std::string_view description,
        std::string_view elevenlabs_voice_id);
    void delete_voice(std::string_view voice_id);
    void write_jev(const std::optional<WorkspaceJev>& settings);
    void write_web_search(const WorkspaceWebSearch& settings);
    void write_voice_input(const WorkspaceVoiceInput& settings);
    void write_voice_output(const WorkspaceVoiceOutput& settings);
    void create_api_key(
        std::string_view id,
        std::string_view display_name,
        std::string_view value);
    void write_api_key(
        std::string_view id,
        std::string_view display_name,
        std::string_view value);
    void delete_api_key(std::string_view id);
    void create_r2_storage(const R2StorageKey& key);
    void write_r2_storage(const R2StorageKey& key);
    void delete_r2_storage();
    void write_next_api_key_id(std::uint64_t next_id);

    void write_character_settings(
        std::string_view character_id,
        std::string_view provider_id,
        std::optional<std::string_view> style_id,
        std::optional<std::string_view> voice_id = std::nullopt,
        std::optional<std::string_view> reasoning_effort = std::nullopt,
        std::optional<WebSearchMode> web_search = std::nullopt);
    void write_character_definition(
        std::string_view character_id,
        std::string_view display_name,
        std::optional<std::string_view> markdown = std::nullopt);
    void delete_character(std::string_view character_id);
    void write_character_file(
        std::string_view character_id,
        std::string_view filename,
        std::optional<std::string_view> content,
        bool create = false);
    void write_persona(
        std::string_view persona_id,
        std::string_view display_name,
        std::string_view markdown,
        std::optional<std::string_view> style_id,
        std::optional<std::string_view> voice_id);
    void delete_persona(std::string_view persona_id);
    void create_persona(
        std::string_view persona_id,
        std::string_view display_name);
    void create_character(
        std::string_view character_id,
        std::string_view display_name,
        std::string_view description);
    void create_forum(
        std::string_view forum_id,
        std::string_view display_name,
        std::string_view persona_id);
    void write_forum(
        std::string_view forum_id,
        std::string_view display_name,
        std::string_view markdown);
    void write_forum_file(
        std::string_view forum_id,
        std::string_view filename,
        std::optional<std::string_view> content,
        bool create = false);
    void delete_forum(std::string_view forum_id);
    void write_forum_members(
        std::string_view forum_id,
        std::span<const std::string> character_ids);
    void write_forum_default_character(
        std::string_view forum_id,
        std::string_view character_id);
    void write_forum_default_persona(
        std::string_view forum_id,
        std::string_view persona_id);

private:
    void write_file(const std::filesystem::path& path, std::string_view content);
    void write_toml(const std::filesystem::path& path, const toml::table& table);
    void rewrite_toml(const std::filesystem::path& path,
                      const std::function<void(toml::table&)>& edit);
    void remove_directory(const std::filesystem::path& path);
    void write_markdown_file(
        const std::filesystem::path& config_path,
        const std::map<std::string, std::string, std::less<>>& markdown_files,
        std::string_view filename, std::optional<std::string_view> content,
        bool create, std::string_view required_filename, std::string_view subject);

    const Workspace& workspace_;
    TextFiles& files_;
    TextSource source_;
};

} // namespace cha
