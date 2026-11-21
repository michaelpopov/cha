#pragma once

#include "app/application_config.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

namespace cha::app::vault {

[[nodiscard]] std::filesystem::path normalized_vault_path(
    const std::filesystem::path& config_directory,
    const std::filesystem::path& value);
[[nodiscard]] toml::table vault_definition_table(
    const VaultDefinition& vault);
void assign_vault_paths(
    VaultDefinition& vault,
    const ApplicationCommand& command);
[[nodiscard]] std::filesystem::path next_vault_file(
    const std::filesystem::path& config_directory);
void validate_candidate_vaults(
    const std::filesystem::path& config_directory,
    const std::vector<VaultDefinition>& vaults);
void validate_vault_paths(const VaultDefinition& vault);
void require_available_database_path(const std::filesystem::path& data);
void clear_existing_export(const std::filesystem::path& destination);
bool move_modify_directory(
    const std::optional<std::filesystem::path>& from,
    const std::optional<std::filesystem::path>& to);
void restore_modify_directory(
    const std::filesystem::path& from,
    const std::filesystem::path& to) noexcept;
[[nodiscard]] nlohmann::json vault_detail_json(
    const VaultDefinition& vault,
    std::string_view active_name,
    std::size_t vault_count);
void save_file_replace(
    const std::filesystem::path& destination,
    std::string_view contents);

} // namespace cha::app::vault
