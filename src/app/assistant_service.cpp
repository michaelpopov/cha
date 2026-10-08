#include "app/assistant_service.h"

#include "app/application_config.h"
#include "providers/credentials.h"
#include "providers/openai_oauth.h"
#include "runtime/live_session_manager.h"
#include "storage/session_repository.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cha {

std::string_view embedded_assistant_maintenance();
std::string_view embedded_maintainer_guide();
std::string_view embedded_linux_packaging_readme();

namespace {

using Json = nlohmann::json;

std::string tool_error(std::string_view code, std::string_view message) {
    return Json{
        {"error", code},
        {"message", message},
        {"committed", false},
    }.dump();
}

std::string version_text(std::uint64_t revision) {
    return std::to_string(revision);
}

std::optional<std::uint64_t> parse_version(const Json& value) {
    if (!value.is_string()) return std::nullopt;
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty()) return std::nullopt;
    std::uint64_t parsed = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto [pointer, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || pointer != end) return std::nullopt;
    return parsed;
}

enum class LogSeverity { trace, debug, info, warn, error, critical };

std::string_view level_name(LogSeverity level) {
    switch (level) {
    case LogSeverity::trace: return "trace";
    case LogSeverity::debug: return "debug";
    case LogSeverity::info: return "info";
    case LogSeverity::warn: return "warn";
    case LogSeverity::error: return "error";
    case LogSeverity::critical: return "critical";
    }
    return "info";
}

std::optional<LogSeverity> parse_level(std::string_view name) {
    if (name == "trace") return LogSeverity::trace;
    if (name == "debug") return LogSeverity::debug;
    if (name == "info") return LogSeverity::info;
    if (name == "warn") return LogSeverity::warn;
    if (name == "error") return LogSeverity::error;
    if (name == "critical") return LogSeverity::critical;
    return std::nullopt;
}

std::string_view read_status_name(WorkspaceConfigReadStatus status) {
    switch (status) {
    case WorkspaceConfigReadStatus::ok: return "ok";
    case WorkspaceConfigReadStatus::missing: return "missing";
    case WorkspaceConfigReadStatus::too_large: return "too_large";
    case WorkspaceConfigReadStatus::key_metadata: return "key_metadata";
    }
    return "missing";
}

std::string_view apply_error_name(WorkspaceConfigApplyError error) {
    switch (error) {
    case WorkspaceConfigApplyError::none: return {};
    case WorkspaceConfigApplyError::invalid_argument: return "invalid_argument";
    case WorkspaceConfigApplyError::invalid_path: return "invalid_path";
    case WorkspaceConfigApplyError::create_replace_conflict: return "create_replace_conflict";
    case WorkspaceConfigApplyError::protected_path: return "protected_path";
    case WorkspaceConfigApplyError::credential_destination_protected:
        return "credential_destination_protected";
    case WorkspaceConfigApplyError::stale_version: return "stale_version";
    case WorkspaceConfigApplyError::unavailable_undo: return "unavailable_undo";
    case WorkspaceConfigApplyError::validation_failure: return "validation_failure";
    case WorkspaceConfigApplyError::too_large: return "too_large";
    case WorkspaceConfigApplyError::cancelled: return "cancelled";
    case WorkspaceConfigApplyError::restart_required: return "restart_required";
    }
    return "invalid_argument";
}

std::string cap_text(std::string text, std::size_t limit) {
    if (text.size() <= limit) return text;
    text.resize(limit - 3);
    // Drop a cut that lands inside one UTF-8 character. A complete character
    // keeps its continuation bytes.
    std::size_t index = text.size();
    std::size_t continuations = 0;
    while (index > 0 && continuations < 3
        && (static_cast<unsigned char>(text[index - 1]) & 0xC0) == 0x80) {
        --index;
        ++continuations;
    }
    if (index > 0) {
        const unsigned char lead = static_cast<unsigned char>(text[index - 1]);
        std::size_t expected = 0;
        if ((lead & 0xE0) == 0xC0) expected = 1;
        else if ((lead & 0xF0) == 0xE0) expected = 2;
        else if ((lead & 0xF8) == 0xF0) expected = 3;
        if (expected != 0 && continuations < expected) text.resize(index - 1);
    }
    text += "...";
    return text;
}

bool welcome_request(const MaintenanceContext& context) {
    return context.character_id == workspace_assistant_id
        && context.forum_id == workspace_entrance_id
        && context.session_id == welcome_id;
}

// Returns a tool error when the captured epoch is no longer admitted.
using AdmissionCheck = std::function<std::optional<std::string>()>;

// Removes saved key values, OAuth tokens, and private roots from tool text.
class Redactor {
public:
    explicit Redactor(const AssistantService::Links& links) {
        try {
            roots_.push_back(links.config_directory.string());
            if (const auto snapshot = links.store->snapshot()) {
                for (const SavedApiKey& key : snapshot->api_keys()) {
                    if (!key.value.empty()) secrets_.push_back(key.value);
                }
                if (const auto& r2 = snapshot->r2_storage()) {
                    if (!r2->secret_key.empty()) secrets_.push_back(r2->secret_key);
                }
            }
        } catch (const std::exception&) {
        }
        if (links.oauth) {
            for (std::string& secret : links.oauth->redaction_secrets()) {
                if (!secret.empty()) secrets_.push_back(std::move(secret));
            }
        }
        try {
            roots_.push_back(links.store->private_root().string());
            roots_.push_back(links.store->workspace_path().string());
        } catch (const std::exception&) {
        }
        std::erase_if(roots_, [](const std::string& root) { return root.size() < 2; });
        const auto longer_first = [](const std::string& left, const std::string& right) {
            return left.size() > right.size();
        };
        std::ranges::sort(secrets_, longer_first);
        std::ranges::sort(roots_, longer_first);
    }

    std::string operator()(std::string_view text) const {
        std::string result;
        result.reserve(text.size());
        // Match the original text once so short secrets cannot alter the markers.
        while (!text.empty()) {
            const auto matches = [text](const std::string& value) {
                return text.starts_with(value);
            };
            const auto root = std::ranges::find_if(roots_, matches);
            const auto secret = std::ranges::find_if(secrets_, matches);
            if (root != roots_.end()) {
                result += "[path]";
                text.remove_prefix(root->size());
            } else if (secret != secrets_.end()) {
                result += "[REDACTED]";
                text.remove_prefix(secret->size());
            } else {
                result += text.front();
                text.remove_prefix(1);
            }
        }
        return result;
    }

private:
    std::vector<std::string> secrets_;
    std::vector<std::string> roots_;
};

bool host_config_name(std::string_view name) {
    return name.size() > 5 && name.ends_with(".toml")
        && name.find_first_of("/\\:") == std::string_view::npos
        && name.find('\0') == std::string_view::npos;
}

// Bounded reads return complete text, never a partial replacement base.
std::string read_host_config(const std::filesystem::path& file) {
    require_regular_file(file);
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read host configuration.");
    std::string content(workspace_config_file_size_limit + 1, '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (input.bad()) throw std::runtime_error("Cannot read host configuration.");
    content.resize(static_cast<std::size_t>(input.gcount()));
    return content;
}

std::string host_config_tool(
    std::string_view name,
    const Json& parsed,
    const AssistantService::Links& links,
    const MaintenanceContext& context,
    std::unique_lock<std::timed_mutex>& lifecycle,
    const AdmissionCheck& admission_error,
    const WorkspaceConfigCancelCheck& cancel_check,
    const Redactor& redact) {
    if (links.config_directory.empty()) {
        return tool_error("unavailable", "Host configuration tools are not available.");
    }
    if (name == "host_config_list") {
        if (!parsed.empty()) return tool_error("invalid_argument", "host_config_list takes no arguments.");
        std::vector<std::string> paths;
        for (const auto& entry : std::filesystem::directory_iterator(links.config_directory)) {
            const auto path = utf8_path(entry.path().filename());
            if (host_config_name(path) && std::filesystem::is_regular_file(entry.symlink_status())) {
                paths.push_back(path);
            }
        }
        std::ranges::sort(paths);
        const bool truncated = paths.size() > workspace_config_list_limit;
        if (truncated) paths.resize(workspace_config_list_limit);
        Json entries = Json::array();
        for (const auto& path : paths) {
            entries.push_back({{"path", path},
                {"bytes", std::filesystem::file_size(links.config_directory / path_from_utf8(path))}});
        }
        if (const auto error = admission_error()) return *error;
        const std::string result = Json{{"entries", entries}, {"truncated", truncated}}.dump();
        if (result.size() > maintenance_call_result_limit) {
            return tool_error("too_large", "The host configuration listing is too large.");
        }
        return result;
    }
    const bool write = name == "host_config_write";
    if (!parsed.contains("path") || !parsed["path"].is_string()
        || parsed.size() != (write ? 3 : 1)
        || (write && (!parsed.contains("content") || !parsed["content"].is_string()
            || !parsed.contains("expected_content")
            || (!parsed["expected_content"].is_string() && !parsed["expected_content"].is_null())))) {
        return tool_error("invalid_argument", write
            ? "host_config_write requires path, content, and expected_content."
            : "host_config_read requires path.");
    }
    const auto path = parsed["path"].get<std::string>();
    if (!host_config_name(path)) {
        return tool_error("invalid_path", "Use a TOML filename in the host configuration directory.");
    }
    const auto file = links.config_directory / path_from_utf8(path);
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(file, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("Cannot inspect host configuration.");
    }
    const bool exists = status.type() != std::filesystem::file_type::not_found;
    if (exists && !std::filesystem::is_regular_file(status)) {
        return tool_error("invalid_path", "Host configuration must be a regular file, not a link.");
    }
    std::string current;
    if (exists) current = read_host_config(file);
    if (current.size() > workspace_config_file_size_limit) {
        return tool_error("too_large", "The configuration file exceeds 64 KiB.");
    }
    if (const auto error = admission_error()) return *error;
    if (!write) {
        const std::string result = Json{{"path", path}, {"status", exists ? "ok" : "missing"},
            {"content", exists ? Json(current) : Json(nullptr)}}.dump();
        if (result.size() > maintenance_call_result_limit) {
            return tool_error("too_large", "The configuration result is too large.");
        }
        return result;
    }
    const auto content = parsed["content"].get<std::string>();
    if (content.size() > workspace_config_file_size_limit) {
        return tool_error("too_large", "The configuration file exceeds 64 KiB.");
    }
    const auto& expected = parsed["expected_content"];
    if (exists != expected.is_string() || (exists && current != expected.get<std::string>())) {
        return tool_error("stale_content", "The file changed. Read it again before writing.");
    }
    if (exists && current == content) {
        return Json{{"committed", false}, {"restart_required", false}, {"path", path},
            {"message", "No configuration changes were necessary."}}.dump();
    }
    // Reuse startup validation with one in-memory replacement. Nothing is
    // written until the whole configuration directory has loaded successfully.
    const auto candidate = load_configuration_directory(links.config_directory, path, content);
    if (cancel_check()) return tool_error("cancelled", "The maintenance tool was cancelled.");
    if (const auto error = admission_error()) return *error;
    // Check again after validation, which also reads the other registrations.
    if ((exists && read_host_config(file) != current)
        || (!exists && std::filesystem::exists(std::filesystem::symlink_status(file)))) {
        return tool_error("stale_content", "The file changed. Read it again before writing.");
    }
    create_private_file(file, content);
    for (const auto& warning : candidate.warnings) log_warn(warning);
    lifecycle.unlock();
    const std::string notice = "Host configuration was saved. Restart is required.";
    bool delivered = true;
    if (links.sessions) {
        delivered = links.sessions->post_maintenance_result(context.context_epoch,
            {context.forum_id, context.session_id}, notice);
    }
    Json warnings = Json::array();
    for (const auto& warning : candidate.warnings) warnings.push_back(redact(warning));
    Json result{{"committed", true}, {"restart_required", true}, {"path", path},
        {"warnings", warnings}, {"message", delivered ? notice : notice + " Session refresh was not delivered."}};
    if (result.dump().size() > maintenance_call_result_limit) {
        result["warnings"] = Json::array();
        result["message"] = notice + " Warnings were omitted because the result is too large.";
    }
    return result.dump();
}

std::string list_tool(
    const Json& parsed,
    WorkspaceConfigStore& store,
    const AdmissionCheck& admission_error) {
    if (parsed.size() != 1 || !parsed.contains("prefix")) {
        return tool_error("invalid_argument", "vault_config_list requires prefix.");
    }
    std::string prefix;
    if (!parsed["prefix"].is_null()) {
        if (!parsed["prefix"].is_string()) {
            return tool_error("invalid_argument", "prefix must be a string or null.");
        }
        prefix = parsed["prefix"].get<std::string>();
    }
    const auto listed = store.list_config(prefix);
    if (const auto error = admission_error()) return *error;
    Json entries = Json::array();
    for (const auto& item : listed.entries) {
        Json entry{
            {"path", item.path},
            {"bytes", item.bytes},
            {"readable", item.readable},
            {"writable", item.writable},
        };
        if (!item.protection_reason.empty()) {
            entry["protection_reason"] = item.protection_reason;
        }
        entries.push_back(std::move(entry));
    }
    return Json{
        {"version", version_text(listed.revision)},
        {"truncated", listed.truncated},
        {"entries", std::move(entries)},
    }.dump();
}

std::string read_tool(
    const Json& parsed,
    WorkspaceConfigStore& store,
    const AdmissionCheck& admission_error) {
    if (parsed.size() != 1 || !parsed.contains("paths") || !parsed["paths"].is_array()) {
        return tool_error("invalid_argument", "vault_config_read requires paths.");
    }
    std::vector<std::string> paths;
    for (const auto& path : parsed["paths"]) {
        if (!path.is_string()) {
            return tool_error("invalid_argument", "Each path must be a string.");
        }
        paths.push_back(path.get<std::string>());
    }
    const auto read = store.read_config(paths);
    if (const auto error = admission_error()) return *error;
    // JSON escaping makes text larger. A file that does not fit in the
    // encoded result is too_large, so the other files still arrive.
    Json files = Json::array();
    std::size_t total = Json{
        {"version", version_text(read.revision)},
        {"files", Json::array()},
    }.dump().size();
    for (const auto& item : read.files) {
        Json file{{"path", item.path}, {"status", read_status_name(item.status)}};
        if (item.status == WorkspaceConfigReadStatus::ok) file["content"] = item.content;
        if (item.key) {
            file["key"] = {
                {"id", item.key->id},
                {"display_name", item.key->display_name},
                {"type", item.key->type},
                {"credential_present", item.key->credential_present},
            };
        }
        std::size_t size = file.dump().size() + 1;
        if (file.contains("content") && total + size > maintenance_call_result_limit) {
            file.erase("content");
            file["status"] = read_status_name(WorkspaceConfigReadStatus::too_large);
            size = file.dump().size() + 1;
        }
        total += size;
        files.push_back(std::move(file));
    }
    if (total > maintenance_call_result_limit) {
        return tool_error("too_large", "Too many paths. Read fewer paths at once.");
    }
    return Json{
        {"version", version_text(read.revision)},
        {"files", std::move(files)},
    }.dump();
}

std::string logging_tool(const Json& parsed, const AdmissionCheck& admission_error) {
    if (parsed.size() != 1 || !parsed.contains("level") || !parsed["level"].is_string()) {
        return tool_error("invalid_argument", "assistant_logging requires level.");
    }
    const auto level = parsed["level"].get<std::string>();
    if (level != "off" && !parse_level(level)) {
        return tool_error("invalid_argument", "level is not a known level.");
    }
    if (const auto error = admission_error()) return *error;
    set_diagnostic_log_level(level);
    return Json{{"level", diagnostic_log_level()}}.dump();
}

std::string logs_tool(
    const Json& parsed,
    const Redactor& redact,
    const AdmissionCheck& admission_error) {
    if (!parsed.contains("minimum_level")
        || !parsed.contains("contains") || !parsed.contains("limit")
        || parsed.size() != 3) {
        return tool_error("invalid_argument", "assistant_logs arguments are incomplete.");
    }
    LogSeverity minimum = LogSeverity::info;
    if (!parsed["minimum_level"].is_null()) {
        if (!parsed["minimum_level"].is_string()) {
            return tool_error("invalid_argument", "minimum_level must be a string or null.");
        }
        const auto parsed_level = parse_level(parsed["minimum_level"].get<std::string>());
        if (!parsed_level) {
            return tool_error("invalid_argument", "minimum_level is not a known level.");
        }
        minimum = *parsed_level;
    }
    std::string contains;
    if (!parsed["contains"].is_null()) {
        if (!parsed["contains"].is_string()) {
            return tool_error("invalid_argument", "contains must be a string or null.");
        }
        contains = parsed["contains"].get<std::string>();
    }
    if (!parsed["limit"].is_number_unsigned()) {
        return tool_error("invalid_argument", "limit must be a positive integer.");
    }
    const auto limit = parsed["limit"].get<std::uint64_t>();
    if (limit == 0 || limit > maintenance_log_entry_limit) {
        return tool_error("invalid_argument", "limit must be between 1 and "
            + std::to_string(maintenance_log_entry_limit) + ".");
    }
    if (const auto error = admission_error()) return *error;
    const auto path = diagnostic_log_file();
    if (path.empty()) return tool_error("unavailable", "Diagnostic logging is not initialized.");
    // spdlog renames the previous file to "name.1.ext". Read it first to keep time order.
    std::filesystem::path rotated = path.stem();
    rotated += ".1";
    rotated += path.extension();
    rotated = path.parent_path() / rotated;
    // One large payload line must not use the whole result.
    constexpr std::size_t entry_text_limit = 4096;
    bool missing = true;
    bool limited = false;
    std::deque<Json> matches;
    for (const auto& file : {rotated, path}) {
        std::ifstream input(file);
        if (!input) {
            if (std::filesystem::exists(file)) {
                return tool_error("unavailable", "Cannot read the diagnostic log file.");
            }
            continue;
        }
        missing = false;
        std::string line;
        while (std::getline(input, line)) {
            // Parse only the native header, before redaction or substring filtering.
            if (!line.starts_with("[")) continue;
            const auto thread = line.find("] [thread ");
            if (thread == std::string::npos) continue;
            const auto start = line.find("] [", thread + 3);
            if (start == std::string::npos) continue;
            const auto end = line.find("] ", start + 3);
            if (end == std::string::npos) continue;
            const auto name = std::string_view(line).substr(start + 3, end - start - 3);
            const auto level = parse_level(name == "warning" ? "warn" : name);
            if (!level || *level < minimum) continue;
            auto text = redact(std::move(line));
            if (!contains.empty() && text.find(contains) == std::string::npos) continue;
            matches.push_back({
                {"level", level_name(*level)},
                {"text", cap_text(std::move(text), entry_text_limit)},
            });
            if (matches.size() > limit) {
                matches.pop_front();
                limited = true;
            }
        }
        if (input.bad()) return tool_error("unavailable", "Cannot read the diagnostic log file.");
    }
    // Log text is untrusted and can hold invalid UTF-8.
    const auto dump = [](const Json& value) {
        return value.dump(-1, ' ', false, Json::error_handler_t::replace);
    };
    const auto encode = [&](Json items, bool shortened) {
        return dump(Json{
            {"entries", std::move(items)},
            {"status", missing ? "missing" : "ok"},
            {"level", diagnostic_log_level()},
            {"limited", shortened},
        });
    };
    std::size_t total = encode(Json::array(), false).size();
    std::vector<std::size_t> sizes;
    for (const Json& entry : matches) {
        sizes.push_back(dump(entry).size() + 1);
        total += sizes.back();
    }
    std::size_t dropped = 0;
    while (total > maintenance_call_result_limit && dropped < sizes.size()) {
        total -= sizes[dropped++];
    }
    if (dropped > 0) limited = true;
    Json entries = Json::array();
    for (std::size_t index = dropped; index < matches.size(); ++index) {
        entries.push_back(std::move(matches[index]));
    }
    const std::string encoded = encode(std::move(entries), limited);
    if (const auto error = admission_error()) return *error;
    if (encoded.size() > maintenance_call_result_limit) {
        return tool_error("too_large", "The log result is too large.");
    }
    return encoded;
}

// The short transcript notice for a committed apply or undo.
std::string save_notice(
    const WorkspaceConfigApplyResult& applied,
    bool restart,
    const std::string& refresh_error,
    const Redactor& redact) {
    std::string notice = "Configuration was saved.";
    if (!restart && !applied.changed.empty()) {
        notice += " Changed:";
        const std::size_t shown = std::min<std::size_t>(applied.changed.size(), 5);
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& item = applied.changed[index];
            notice += " " + item.path + " (" + std::to_string(item.new_bytes) + " bytes)";
        }
        if (applied.changed.size() > shown) notice += " ...";
        notice += ".";
    }
    notice += applied.undo_available && !restart
        ? " Undo is available."
        : " Undo is not available.";
    if (!restart && !applied.warnings.empty()) {
        notice += " Warning: " + redact(applied.warnings.front().message);
    }
    if (!refresh_error.empty()) {
        notice += " Refresh failed: " + refresh_error;
    }
    if (restart) notice += " Restart is required.";
    return cap_text(std::move(notice), 500);
}

std::string apply_tool(
    const Json& parsed,
    const AssistantService::Links& links,
    const MaintenanceContext& context,
    std::unique_lock<std::timed_mutex>& lifecycle,
    const WorkspaceConfigCancelCheck& cancel_check,
    const Redactor& redact) {
    if (!parsed.contains("action") || !parsed.contains("version")
        || !parsed.contains("changes") || parsed.size() != 3
        || !parsed["action"].is_string()) {
        return tool_error("invalid_argument", "vault_config_apply arguments are incomplete.");
    }
    const auto version = parse_version(parsed["version"]);
    if (!version) {
        return tool_error("invalid_argument", "version must be the decimal configuration version.");
    }
    const std::string action = parsed["action"].get<std::string>();
    const bool undo = action == "undo";
    if (action != "apply" && !undo) {
        return tool_error("invalid_argument", "action must be apply or undo.");
    }
    std::vector<WorkspaceConfigChange> changes;
    if (undo) {
        if (!parsed["changes"].is_null()) {
            return tool_error("invalid_argument", "undo requires changes to be null.");
        }
    } else if (!parsed["changes"].is_array() || parsed["changes"].empty()) {
        return tool_error("invalid_argument", "apply requires a non-empty changes array.");
    } else {
        for (const auto& change : parsed["changes"]) {
            if (!change.is_object() || change.size() != 3
                || !change.contains("path") || !change["path"].is_string()
                || !change.contains("operation") || !change["operation"].is_string()
                || !change.contains("content") || !change["content"].is_string()) {
                return tool_error("invalid_argument", "Each change requires path, operation, and content.");
            }
            const std::string operation = change["operation"].get<std::string>();
            WorkspaceConfigOperation kind = WorkspaceConfigOperation::replace;
            if (operation == "create") kind = WorkspaceConfigOperation::create;
            else if (operation != "replace") {
                return tool_error("invalid_argument", "operation must be create or replace.");
            }
            changes.push_back({
                .path = change["path"].get<std::string>(),
                .operation = kind,
                .content = change["content"].get<std::string>(),
            });
        }
    }

    WorkspaceConfigApplyResult applied;
    bool restart = false;
    std::string restart_message;
    try {
        applied = undo
            ? links.store->undo_config(*version, cancel_check)
            : links.store->apply_config(*version, changes, cancel_check);
    } catch (const WorkspaceRestartRequiredError& error) {
        // The store throws this only after it committed this call's rows.
        restart = true;
        restart_message = redact(error.what());
    }
    const bool saved = restart || (
        applied.error == WorkspaceConfigApplyError::none && !applied.changed.empty());
    std::string refresh_error;
    bool delivered = true;
    if (saved) {
        lifecycle.unlock();
        if (links.repository && !restart) {
            try {
                links.repository->synchronize_forums(*links.store->snapshot());
            } catch (const std::exception& error) {
                refresh_error = redact(error.what());
            }
        }
        if (links.sessions) {
            delivered = links.sessions->post_maintenance_result(
                context.context_epoch,
                {context.forum_id, context.session_id},
                save_notice(applied, restart, refresh_error, redact));
        }
    }

    if (restart) {
        std::string message = "Configuration was saved. Restart is required.";
        if (!restart_message.empty()) message += " " + restart_message;
        if (!delivered) message += " Session refresh was not delivered.";
        return Json{
            {"committed", true},
            {"restart_required", true},
            {"version", nullptr},
            {"changed", Json::array()},
            {"warnings", Json::array()},
            {"undo_available", false},
            {"error", nullptr},
            {"message", std::move(message)},
        }.dump();
    }

    Json changed = Json::array();
    for (const auto& item : applied.changed) {
        changed.push_back({
            {"path", item.path},
            {"old_bytes", item.old_bytes ? Json(*item.old_bytes) : Json(nullptr)},
            {"new_bytes", item.new_bytes},
        });
    }
    Json warnings = Json::array();
    for (const auto& warning : applied.warnings) {
        warnings.push_back({
            {"path", warning.path},
            {"message", redact(warning.message)},
        });
    }
    std::string message = redact(applied.error_message);
    if (applied.error == WorkspaceConfigApplyError::none) {
        message = applied.changed.empty()
            ? "No configuration changes were necessary."
            : "Configuration was saved.";
        if (!refresh_error.empty()) message += " Refresh failed: " + refresh_error;
        if (!delivered) message += " Session refresh was not delivered.";
    }
    const auto code = apply_error_name(applied.error);
    Json result{
        {"committed", applied.committed},
        {"version", version_text(applied.revision)},
        {"changed", std::move(changed)},
        {"warnings", std::move(warnings)},
        {"undo_available", applied.undo_available},
        {"error", code.empty() ? Json(nullptr) : Json(std::string(code))},
        {"message", std::move(message)},
    };
    std::string encoded = result.dump();
    if (encoded.size() > maintenance_call_result_limit) {
        // The provider loop never replaces an apply result. Keep it in bounds.
        result["warnings"] = Json::array();
        result["message"] = result["message"].get<std::string>()
            + " Warnings were omitted because the result is too large.";
        encoded = result.dump();
    }
    return encoded;
}

} // namespace

std::string maintenance_reference(bool chaweb_host) {
    std::string text = chaweb_host
        ? "Host: cha-daemon (ChaWeb)\n\n"
        : "Host: desktop application\n\n";
    text.append(embedded_assistant_maintenance());
    text.push_back('\n');
    text.append(embedded_maintainer_guide());
    text.push_back('\n');
    text.append(embedded_linux_packaging_readme());
    return text;
}

AssistantService::AssistantService(Links links) : links_(std::move(links)) {}

void AssistantService::bind_runtime(
    LiveSessionManager& sessions,
    SessionRepository& repository) {
    links_.sessions = &sessions;
    links_.repository = &repository;
}

std::string AssistantService::execute(
    std::string_view name,
    std::string_view arguments,
    const MaintenanceContext& context,
    const std::atomic_bool& cancelled) {
    if (arguments.size() > maintenance_argument_limit) {
        return tool_error("too_large", "Tool arguments are too large.");
    }
    if (!welcome_request(context)) {
        return tool_error(
            "unavailable",
            "This maintenance tool is not available for this request.");
    }
    const auto stopping = links_.stopping;
    const auto cancel_check = [&cancelled, stopping] {
        return cancelled.load(std::memory_order_acquire)
            || (stopping && stopping->load(std::memory_order_acquire));
    };
    if (cancel_check()) {
        return tool_error("cancelled", "The maintenance tool was cancelled.");
    }
    if (!links_.admit || !links_.store || !links_.lifecycle) {
        return tool_error("unavailable", "Maintenance tools are not available.");
    }

    Json parsed = Json::parse(arguments, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return tool_error("invalid_argument", "Tool arguments are not valid JSON.");
    }

    // A blocking lock here can deadlock vault switch: that path holds the mutex
    // while it waits for this worker. Leave when the request is cancelled.
    std::unique_lock<std::timed_mutex> lifecycle(*links_.lifecycle, std::defer_lock);
    while (!lifecycle.try_lock_for(std::chrono::milliseconds(50))) {
        if (cancel_check()) {
            return tool_error("cancelled", "The maintenance tool was cancelled.");
        }
    }
    if (cancel_check()) {
        return tool_error("cancelled", "The maintenance tool was cancelled.");
    }
    // Admission reads lifecycle state, so it runs only while the lock is held.
    const AdmissionCheck admission_error = [&]() -> std::optional<std::string> {
        const auto denied = links_.admit(context.context_epoch);
        if (!denied) return std::nullopt;
        if (*denied == ErrorCode::vault_changed) {
            return tool_error(
                "stale_context",
                "The vault context changed. Read the configuration again.");
        }
        return tool_error("unavailable", "The application is unavailable.");
    };
    if (const auto error = admission_error()) return *error;

    // Collect redaction values once per call.
    const Redactor redact(links_);
    try {
        if (name == "host_config_list" || name == "host_config_read" || name == "host_config_write") {
            return host_config_tool(name, parsed, links_, context, lifecycle,
                admission_error, cancel_check, redact);
        }
        if (name == "vault_config_list") return list_tool(parsed, *links_.store, admission_error);
        if (name == "vault_config_read") return read_tool(parsed, *links_.store, admission_error);
        if (name == "assistant_logging") return logging_tool(parsed, admission_error);
        if (name == "assistant_logs") return logs_tool(parsed, redact, admission_error);
        if (name == "vault_config_apply") {
            return apply_tool(parsed, links_, context, lifecycle, cancel_check, redact);
        }
    } catch (const WorkspaceRestartRequiredError& error) {
        // Only apply and undo can save, and they report their own restart.
        return tool_error("restart_required", redact(error.what()));
    } catch (const std::exception& error) {
        log_warn(std::string("Maintenance tool failed: ") + std::string(name));
        return tool_error("validation_failure", redact(error.what()));
    }

    return tool_error("unknown_tool", "Unknown maintenance tool.");
}

} // namespace cha
