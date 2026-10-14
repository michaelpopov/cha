#pragma once

#include "workspace/workspace.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <toml++/toml.hpp>
#include <utility>
#include <variant>

namespace cha::workspace_detail {

// Helpers shared by workspace loading and configuration editing.
struct LoadWarnings {
    const std::filesystem::path& root;
    LoadWarningCollector* collector{};

    std::string logical(const std::filesystem::path& path) const {
        auto name = generic_utf8_path(path.lexically_relative(root));
        if (name.starts_with("./")) name.erase(0, 2);
        if (name == ".") return {};
        return name;
    }

    void emit(const std::filesystem::path& path, std::string message) const {
        emit_logical(logical(path), std::move(message));
    }

    void emit_logical(std::string_view logical_path, std::string message) const {
        if (collector) {
            collector->push_back({std::string(logical_path), message});
        }
        log_warn(message);
    }
};

using LoadedKey = std::variant<SavedApiKey, R2StorageKey>;

bool is_reserved_id(std::string_view id);

toml::table read_toml(
    const TextSource& source, const std::filesystem::path& path, std::string_view kind);

WorkspaceProvider load_provider(
    const TextSource& source,
    const std::filesystem::path& directory,
    const LoadWarnings& warnings);

bool known_voice_input_provider(std::string_view provider);

bool valid_voice_input_url(std::string_view provider, std::string_view url);

std::string_view voice_input_url_message(std::string_view provider);

bool valid_voice_input_delay(std::string_view delay);

void validate_saved_key_text(
    std::string_view value,
    std::size_t maximum,
    std::string_view field,
    const std::filesystem::path& path,
    bool reject_only_space = true);

std::uint64_t saved_key_suffix(
    std::string_view id,
    const std::filesystem::path& path);

LoadedKey load_saved_key(
    const TextSource& source,
    const std::filesystem::path& directory,
    const LoadWarnings& warnings);

WorkspaceStyle load_style(
    const TextSource& source,
    const std::filesystem::path& directory,
    const LoadWarnings& warnings);

WorkspaceVoice load_voice(
    const TextSource& source,
    const std::filesystem::path& directory,
    const LoadWarnings& warnings);

bool is_persona_id(std::string_view id);

bool is_reserved_participant(std::string_view name);

void validate_workspace_character_id(std::string_view id);

bool valid_web_reader_url(std::string_view value);

} // namespace cha::workspace_detail
