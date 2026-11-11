#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "storage/workspace_session_database.h"

namespace cha {

inline constexpr std::string_view prompt_config_prefix = "system/prompts/";

struct PromptCatalogEntry {
    std::string_view id;
    std::string_view display_name;
    std::string_view (*embedded_text)();

    [[nodiscard]] std::string path() const;
};

extern const std::array<PromptCatalogEntry, 9> prompt_catalog;
[[nodiscard]] const PromptCatalogEntry* find_prompt_catalog_entry(std::string_view id);
[[nodiscard]] std::string normalize_prompt_text(std::string_view text);
[[nodiscard]] std::vector<ConfigFile> default_prompt_config_files();

} // namespace cha
