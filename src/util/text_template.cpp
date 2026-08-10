#include "util/text_template.h"

#include "util/text.h"
#include "util/path_name.h"

#include <toml++/toml.hpp>

#include <array>
#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cha {
namespace {

struct SourceLocation {
    std::filesystem::path path;
    std::size_t line{1};
    std::size_t column{1};
};

struct ExpansionState {
    const TemplateOptions& options;
    std::filesystem::path root_canonical;
    std::vector<SourceLocation> stack;
    std::unordered_map<std::string, TemplateScope> scope_memo;
    std::size_t include_count{0};
    std::size_t output_bytes{0};
};

bool is_variable_name_char(char character) {
    return (character >= 'A' && character <= 'Z')
        || (character >= 'a' && character <= 'z')
        || (character >= '0' && character <= '9')
        || character == '_' || character == '-' || character == '.';
}

bool is_valid_variable_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (const char character : name) {
        if (!is_variable_name_char(character)) {
            return false;
        }
    }
    return true;
}

using PathIterator = std::filesystem::path::const_iterator;

PathIterator next_nonempty(PathIterator it, PathIterator end) {
    while (it != end && it->empty()) {
        ++it;
    }
    return it;
}

bool path_is_under(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    PathIterator root_it = next_nonempty(root.begin(), root.end());
    PathIterator candidate_it = next_nonempty(candidate.begin(), candidate.end());
    const PathIterator root_end = root.end();
    const PathIterator candidate_end = candidate.end();
    while (root_it != root_end) {
        if (candidate_it == candidate_end || *root_it != *candidate_it) {
            return false;
        }
        root_it = next_nonempty(std::next(root_it), root_end);
        candidate_it = next_nonempty(std::next(candidate_it), candidate_end);
    }
    return true;
}

std::string display_path(
    const std::filesystem::path& path,
    const std::filesystem::path& root_canonical) {
    if (path == root_canonical) {
        return ".";
    }
    if (path_is_under(root_canonical, path)) {
        return utf8_path(std::filesystem::relative(path, root_canonical));
    }
    return utf8_path(path);
}

std::string format_location(
    const SourceLocation& location,
    const std::filesystem::path& root_canonical) {
    return display_path(location.path, root_canonical) + ':'
        + std::to_string(location.line) + ':'
        + std::to_string(location.column);
}

std::string format_include_chain(
    const std::vector<SourceLocation>& stack,
    const std::filesystem::path& root_canonical) {
    if (stack.empty()) {
        return {};
    }
    std::string message;
    message += "\n  in " + format_location(stack.back(), root_canonical);
    for (std::size_t index = stack.size() - 1; index > 0; --index) {
        message += "\n  included from "
            + format_location(stack[index - 1], root_canonical);
    }
    return message;
}

[[noreturn]] void throw_expansion_error(
    const ExpansionState& state,
    std::string message) {
    message += format_include_chain(state.stack, state.root_canonical);
    throw std::runtime_error(std::move(message));
}

// Returns nullopt when the file cannot be opened or read. Callers that already
// checked existence (includes) only hit this on permission/IO failures.
std::optional<std::string> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    if (!file.good() && !file.eof()) {
        return std::nullopt;
    }
    return contents.str();
}

std::string render_toml_scalar(
    const toml::node& node,
    std::string_view name) {
    if (const auto* value = node.as_string()) {
        return std::string{value->get()};
    }
    if (const auto* value = node.as_integer()) {
        return std::to_string(value->get());
    }
    if (const auto* value = node.as_floating_point()) {
        const double number = value->get();
        std::array<char, 64> buffer{};
        const std::to_chars_result result = std::to_chars(
            buffer.data(),
            buffer.data() + buffer.size(),
            number);
        if (result.ec != std::errc{}) {
            throw std::runtime_error(
                "variable '" + std::string(name)
                + "' has an unsupported type");
        }
        return std::string(buffer.data(), result.ptr);
    }
    if (const auto* value = node.as_boolean()) {
        return value->get() ? "true" : "false";
    }
    throw std::runtime_error(
        "variable '" + std::string(name) + "' has an unsupported type");
}

// `file_label` is what appears in diagnostics. Callers under expansion pass a
// path relative to the containment root; direct load_template_scope() callers
// pass utf8_path(file).
TemplateScope scope_from_toml(
    const toml::table& table,
    std::string_view table_name,
    std::string_view file_label) {
    const toml::node* node = table.get(table_name);
    if (node == nullptr) {
        return {};
    }
    const toml::table* scope_table = node->as_table();
    if (scope_table == nullptr) {
        throw std::runtime_error(
            "'" + std::string(table_name) + "' must be a table in '"
            + std::string(file_label) + "'");
    }

    TemplateScope scope;
    for (const auto& [key, value] : *scope_table) {
        const std::string name{key.str()};
        // Quoted keys outside the variable-name grammar are not addressable;
        // skip them rather than failing the load.
        if (!is_valid_variable_name(name)) {
            continue;
        }
        if (value.is_table()) {
            throw std::runtime_error(
                "'[" + std::string(table_name)
                + "]' may contain only scalar values in '"
                + std::string(file_label) + "'");
        }
        try {
            scope.emplace(name, render_toml_scalar(value, name));
        } catch (const std::runtime_error& error) {
            throw std::runtime_error(
                std::string(error.what()) + " in '"
                + std::string(file_label) + "'");
        }
    }
    return scope;
}

TemplateScope read_scope_table(
    const std::filesystem::path& file,
    std::string_view table_name,
    std::string_view file_label) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        throw std::runtime_error(
            "cannot parse '" + std::string(file_label) + "': cannot open file");
    }
    toml::table table;
    try {
        table = toml::parse(stream, std::string(file_label));
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(
            "cannot parse '" + std::string(file_label) + "': "
            + std::string(error.description()));
    }
    return scope_from_toml(table, table_name, file_label);
}

void overlay_scope(TemplateScope& destination, const TemplateScope& source) {
    for (const auto& [key, value] : source) {
        destination.insert_or_assign(key, value);
    }
}

const TemplateScope& directory_scope(
    ExpansionState& state,
    const std::filesystem::path& directory) {
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(directory, error);
    const std::filesystem::path memo_key = error ? directory : canonical;
    const std::string key = utf8_path(memo_key);
    if (const auto found = state.scope_memo.find(key);
        found != state.scope_memo.end()) {
        return found->second;
    }

    TemplateScope loaded;
    const std::filesystem::path scope_file =
        directory / state.options.scope_file_name;
    std::error_code status_error;
    const std::filesystem::file_status scope_status =
        std::filesystem::status(scope_file, status_error);
    if (status_error
        && status_error != std::errc::no_such_file_or_directory) {
        throw_expansion_error(
            state,
            "cannot inspect scope file '"
                + display_path(scope_file, state.root_canonical)
                + "': " + status_error.message());
    }
    if (!status_error && std::filesystem::exists(scope_status)) {
        std::error_code scope_error;
        const std::filesystem::path scope_canonical =
            std::filesystem::weakly_canonical(scope_file, scope_error);
        if (scope_error) {
            throw_expansion_error(
                state,
                "cannot inspect scope file '"
                    + display_path(scope_file, state.root_canonical)
                    + "': " + scope_error.message());
        }
        if (!path_is_under(state.root_canonical, scope_canonical)) {
            throw_expansion_error(
                state, "scope file escapes containment root");
        }
        if (!std::filesystem::is_regular_file(scope_status)) {
            throw_expansion_error(
                state, "scope file is not a regular file");
        }
        const std::string label =
            display_path(scope_canonical, state.root_canonical);
        try {
            loaded = read_scope_table(
                scope_canonical, state.options.scope_table_name, label);
        } catch (const std::runtime_error& read_error) {
            throw_expansion_error(state, read_error.what());
        }
    }
    const auto [iterator, _] =
        state.scope_memo.emplace(key, std::move(loaded));
    return iterator->second;
}

std::string resolve_variable(
    ExpansionState& state,
    std::string_view name,
    const TemplateScope& scope) {
    if (const auto found = state.options.reserved.find(name);
        found != state.options.reserved.end()) {
        return found->second;
    }
    if (const auto found = scope.find(name); found != scope.end()) {
        return found->second;
    }
    throw_expansion_error(
        state, "unknown variable '" + std::string(name) + "'");
}

void append_output(
    ExpansionState& state,
    std::string& output,
    std::string_view text) {
    if (text.empty()) {
        return;
    }
    if (state.output_bytes > state.options.limits.max_output_bytes
        || text.size() > state.options.limits.max_output_bytes - state.output_bytes) {
        throw_expansion_error(
            state,
            "expanded prompt exceeds "
                + std::to_string(state.options.limits.max_output_bytes)
                + " bytes");
    }
    output.append(text);
    state.output_bytes += text.size();
}

std::filesystem::path resolve_include_path(
    ExpansionState& state,
    std::string_view raw_path,
    const std::filesystem::path& current_directory) {
    const std::filesystem::path relative = path_from_utf8(raw_path);
    if (relative.empty()) {
        throw_expansion_error(state, "empty include path");
    }
    if (relative.is_absolute()) {
        throw_expansion_error(state, "include path must be relative");
    }

    std::error_code error;
    const std::filesystem::path joined = current_directory / relative;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(joined, error);
    if (error) {
        // Prefer the author-written relative path over an absolute join.
        throw_expansion_error(
            state,
            "cannot read included file '" + std::string(raw_path) + "'");
    }
    if (!path_is_under(state.root_canonical, canonical)) {
        throw_expansion_error(state, "include path escapes the forum");
    }
    std::error_code status_error;
    const std::filesystem::file_status status =
        std::filesystem::status(canonical, status_error);
    if (status_error || !std::filesystem::exists(status)) {
        throw_expansion_error(
            state,
            "cannot read included file '"
                + display_path(canonical, state.root_canonical) + "'");
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw_expansion_error(state, "included path is not a regular file");
    }
    return canonical;
}

void expand_path(
    ExpansionState& state,
    const std::filesystem::path& path,
    const TemplateScope& inherited_scope,
    std::string& output);

void expand_text(
    ExpansionState& state,
    const std::filesystem::path& path,
    const std::string& text,
    const TemplateScope& scope,
    std::string& output) {
    std::size_t line = 1;
    std::size_t column = 1;
    std::size_t index = 0;

    const auto advance = [&](std::size_t count) {
        for (std::size_t step = 0; step < count; ++step) {
            if (text[index + step] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        index += count;
    };

    const auto set_stack_location = [&](std::size_t at_line, std::size_t at_column) {
        state.stack.back().line = at_line;
        state.stack.back().column = at_column;
    };

    while (index < text.size()) {
        if (text[index] != '$') {
            const std::size_t start = index;
            std::size_t end = start;
            while (end < text.size() && text[end] != '$') {
                ++end;
            }
            append_output(
                state, output, std::string_view(text).substr(start, end - start));
            advance(end - start);
            continue;
        }

        const std::size_t macro_line = line;
        const std::size_t macro_column = column;

        if (index + 1 >= text.size() || text[index + 1] != '$') {
            append_output(state, output, "$");
            advance(1);
            continue;
        }

        if (index + 2 >= text.size()) {
            append_output(state, output, "$$");
            advance(2);
            continue;
        }

        const char third = text[index + 2];
        if (third == '$') {
            append_output(state, output, "$$");
            advance(3);
            continue;
        }
        if (third != '(' && third != '{') {
            append_output(state, output, "$$");
            advance(2);
            continue;
        }

        const char closer = third == '(' ? ')' : '}';
        const std::size_t body_start = index + 3;
        std::size_t cursor = body_start;
        while (cursor < text.size() && text[cursor] != closer) {
            if (text[cursor] == '\n') {
                set_stack_location(macro_line, macro_column);
                throw_expansion_error(state, "unterminated macro");
            }
            ++cursor;
        }
        if (cursor >= text.size()) {
            set_stack_location(macro_line, macro_column);
            throw_expansion_error(state, "unterminated macro");
        }

        const std::string_view body = trim_view(
            std::string_view(text).substr(body_start, cursor - body_start));
        set_stack_location(macro_line, macro_column);

        // Consume through the closer so line/column match the scan.
        advance(cursor + 1 - index);

        if (third == '(') {
            if (body.empty()) {
                throw_expansion_error(state, "empty include path");
            }
            if (state.include_count >= state.options.limits.max_includes) {
                throw_expansion_error(
                    state,
                    "maximum include count ("
                        + std::to_string(state.options.limits.max_includes)
                        + ") exceeded");
            }
            ++state.include_count;
            const std::filesystem::path target = resolve_include_path(
                state, body, path.parent_path());
            expand_path(state, target, scope, output);
        } else {
            if (body.empty()) {
                throw_expansion_error(state, "empty variable name");
            }
            if (!is_valid_variable_name(body)) {
                throw_expansion_error(
                    state,
                    "invalid variable name '" + std::string(body) + "'");
            }
            append_output(state, output, resolve_variable(state, body, scope));
        }
    }
}

void expand_path(
    ExpansionState& state,
    const std::filesystem::path& path,
    const TemplateScope& inherited_scope,
    std::string& output) {
    std::error_code error;
    const std::filesystem::path canonical =
        std::filesystem::weakly_canonical(path, error);
    if (error) {
        throw_expansion_error(
            state,
            "cannot read included file '"
                + display_path(path, state.root_canonical) + "'");
    }
    if (!path_is_under(state.root_canonical, canonical)) {
        throw_expansion_error(
            state,
            state.stack.empty()
                ? "template path escapes containment root"
                : "include path escapes the forum");
    }

    for (const SourceLocation& frame : state.stack) {
        if (frame.path == canonical) {
            std::string message = "include cycle\n  "
                + display_path(state.stack.front().path, state.root_canonical);
            for (std::size_t index = 1; index < state.stack.size(); ++index) {
                message += "\n  -> "
                    + display_path(state.stack[index].path, state.root_canonical);
            }
            message += "\n  -> " + display_path(canonical, state.root_canonical);
            throw std::runtime_error(std::move(message));
        }
    }
    if (state.stack.size() >= state.options.limits.max_include_depth) {
        throw_expansion_error(
            state,
            "maximum include depth ("
                + std::to_string(state.options.limits.max_include_depth)
                + ") exceeded");
    }

    // Seed at 1:1 — pre-scan failures (read, scope parse) belong to the file
    // itself, not the includer's macro position. expand_text() overwrites the
    // location before any error it raises.
    state.stack.push_back({canonical, 1, 1});

    try {
        const std::optional<std::string> text = read_file_bytes(canonical);
        if (!text) {
            throw_expansion_error(
                state,
                "cannot read included file '"
                    + display_path(canonical, state.root_canonical) + "'");
        }

        TemplateScope scope = inherited_scope;
        overlay_scope(scope, directory_scope(state, canonical.parent_path()));
        expand_text(state, canonical, *text, scope, output);
    } catch (...) {
        state.stack.pop_back();
        throw;
    }
    state.stack.pop_back();
}

} // namespace

TemplateScope template_scope_from_toml(
    const toml::table& document,
    std::string_view table_name,
    std::string_view file_label) {
    return scope_from_toml(document, table_name, file_label);
}

std::optional<TemplateScope> load_template_scope(
    const std::filesystem::path& file,
    std::string_view table_name) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::status(file, error);
    if (error == std::errc::no_such_file_or_directory
        || (!error && !std::filesystem::exists(status))) {
        return std::nullopt;
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect '" + utf8_path(file) + "': " + error.message());
    }
    return read_scope_table(file, table_name, utf8_path(file));
}

std::string expand_template_file(
    const std::filesystem::path& path,
    const TemplateOptions& options) {
    std::error_code error;
    const std::filesystem::path root_canonical =
        std::filesystem::weakly_canonical(options.containment_root, error);
    if (error) {
        throw std::runtime_error("cannot resolve containment root");
    }

    ExpansionState state{
        .options = options,
        .root_canonical = root_canonical,
    };

    std::string output;
    expand_path(state, path, options.initial_scope, output);
    return output;
}

} // namespace cha
