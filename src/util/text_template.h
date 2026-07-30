#pragma once

#include <toml++/toml.hpp>

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cha {

using TemplateScope = std::map<std::string, std::string, std::less<>>;

struct TemplateLimits {
    std::size_t max_include_depth{16};
    std::size_t max_output_bytes{1024 * 1024};
    std::size_t max_includes{256};
};

struct TemplateOptions {
    std::filesystem::path containment_root;
    std::string scope_file_name{"config.toml"};
    std::string scope_table_name{"prompt"};
    TemplateScope reserved;       // dotted names, cannot be shadowed
    TemplateScope initial_scope;  // shadowable by directory scopes
    TemplateLimits limits;
};

// Converts one TOML table to a template scope. Values in the selected table
// must be scalar; keys outside the template-variable grammar are ignored.
TemplateScope template_scope_from_toml(
    const toml::table& document,
    std::string_view table_name,
    std::string_view file_label);

// Reads one scope table: the named table of one TOML file, values rendered to
// strings. Returns nullopt when the file does not exist; throws on parse errors
// and non-scalar values.
std::optional<TemplateScope> load_template_scope(
    const std::filesystem::path& file,
    std::string_view table_name);

// Expands one file and everything it includes. Throws std::runtime_error with
// the include chain on any failure.
std::string expand_template_file(
    const std::filesystem::path& path,
    const TemplateOptions& options);

} // namespace cha
