#include "providers/api_key_store.h"

#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cha {
namespace {

using Json = nlohmann::json;

std::optional<std::uint64_t> id_suffix(std::string_view id) {
    constexpr std::string_view prefix = "api_key_";
    if (!id.starts_with(prefix) || id.size() == prefix.size()) {
        return std::nullopt;
    }
    id.remove_prefix(prefix.size());
    std::uint64_t suffix{};
    const auto [end, error] = std::from_chars(
        id.data(), id.data() + id.size(), suffix);
    if (error != std::errc{} || end != id.data() + id.size() || suffix == 0) {
        return std::nullopt;
    }
    return suffix;
}

ApiKeyInfo info(const SavedApiKey& key) {
    return {
        .id = key.id,
        .display_name = key.display_name,
        .has_value = !key.value.empty(),
    };
}

R2StorageInfo info(const R2StorageKey& key) {
    return {
        .id = key.id,
        .display_name = key.display_name,
        .url = key.url,
        .access_key_id = key.access_key_id,
        .has_secret_key = !key.secret_key.empty(),
    };
}

std::shared_ptr<const Workspace> workspace() {
    std::shared_ptr<const Workspace> result = getws();
    if (!result) throw std::runtime_error("No active workspace");
    return result;
}

ApiKeyInfo published_api_key(std::string_view id) {
    const std::shared_ptr<const Workspace> current = workspace();
    const SavedApiKey* const key = current->find_api_key(id);
    if (key == nullptr) {
        throw std::runtime_error(
            "API key '" + std::string(id) + "' was not published");
    }
    return info(*key);
}

R2StorageInfo published_r2_storage() {
    const std::shared_ptr<const Workspace> current = workspace();
    const std::optional<R2StorageKey>& key = current->r2_storage();
    if (!key) {
        throw std::runtime_error("R2 storage credentials were not published");
    }
    return info(*key);
}

struct LegacyKeys {
    std::vector<SavedApiKey> keys;
    std::uint64_t next_id{1};
};

LegacyKeys read_legacy_keys(
    const std::filesystem::path& path) {
    if (path.empty() || !std::filesystem::exists(path)) return {};
    tighten_private_file(path);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read API key file '" + utf8_path(path) + "'");
    }
    try {
        Json json;
        input >> json;
        if (!json.is_object()
            || !json.contains("keys") || !json["keys"].is_object()) {
            throw std::runtime_error("invalid shape");
        }
        const int version = json.value("version", 0);
        const bool legacy = version == 1 && json.size() == 2;
        const bool current = version == 2 && json.size() == 3
            && json.contains("next_id") && json["next_id"].is_number_unsigned();
        if (!legacy && !current) throw std::runtime_error("invalid shape");

        std::uint64_t highest_id{};
        LegacyKeys result;
        result.keys.reserve(json["keys"].size());
        for (const auto& [id, entry] : json["keys"].items()) {
            const std::optional<std::uint64_t> suffix = id_suffix(id);
            if (!suffix || !entry.is_object() || entry.size() != 2
                || !entry.contains("display_name")
                || !entry["display_name"].is_string()
                || !entry.contains("value") || !entry["value"].is_string()) {
                throw std::runtime_error("invalid entry");
            }
            result.keys.push_back({
                .id = id,
                .display_name = entry["display_name"].get<std::string>(),
                .value = entry["value"].get<std::string>(),
            });
            highest_id = std::max(highest_id, *suffix);
        }
        if (current) {
            const std::uint64_t next_id = json["next_id"].get<std::uint64_t>();
            if (next_id == 0 || next_id <= highest_id) {
                throw std::runtime_error("invalid next ID");
            }
            result.next_id = next_id;
        } else {
            if (highest_id == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("API key ID space exhausted");
            }
            result.next_id = highest_id + 1;
        }
        return result;
    } catch (const std::exception&) {
        throw std::runtime_error(
            "API key file '" + utf8_path(path) + "' is invalid");
    }
}

const char* nonempty_environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : nullptr;
}

} // namespace

ApiKeyStore::ApiKeyStore(
    WorkspaceConfigStore& config,
    std::filesystem::path legacy_path)
    : config_(&config), legacy_path_(std::move(legacy_path)) {
    migrate();
}

std::vector<ApiKeyInfo> ApiKeyStore::list() const {
    const std::shared_ptr<const Workspace> current = workspace();
    std::vector<ApiKeyInfo> result;
    result.reserve(current->api_keys().size());
    for (const SavedApiKey& key : current->api_keys()) {
        result.push_back(info(key));
    }
    return result;
}

std::optional<ApiKeyInfo> ApiKeyStore::find(std::string_view id) const {
    const std::shared_ptr<const Workspace> current = workspace();
    const SavedApiKey* key = current->find_api_key(id);
    return key == nullptr ? std::nullopt : std::optional<ApiKeyInfo>(info(*key));
}

std::optional<ApiKeyInfo> ApiKeyStore::find_by_name(
    std::string_view display_name) const {
    const std::shared_ptr<const Workspace> current = workspace();
    const SavedApiKey* result = nullptr;
    for (const SavedApiKey& key : current->api_keys()) {
        if (key.display_name != display_name) continue;
        if (result != nullptr) return std::nullopt;
        result = &key;
    }
    return result == nullptr
        ? std::nullopt : std::optional<ApiKeyInfo>(info(*result));
}

std::string ApiKeyStore::value(std::string_view id) const {
    const std::shared_ptr<const Workspace> current = workspace();
    const SavedApiKey* key = current->find_api_key(id);
    if (key == nullptr) {
        throw std::runtime_error(
            "API key '" + std::string(id) + "' does not exist");
    }
    return key->value;
}

std::string ApiKeyStore::value_by_name(std::string_view display_name) const {
    const std::shared_ptr<const Workspace> current = workspace();
    const SavedApiKey* result = nullptr;
    for (const SavedApiKey& key : current->api_keys()) {
        if (key.display_name != display_name) continue;
        if (result != nullptr) {
            throw std::runtime_error(
                "API key name '" + std::string(display_name) + "' is ambiguous");
        }
        result = &key;
    }
    if (result == nullptr) {
        throw std::runtime_error(
            "API key named '" + std::string(display_name) + "' does not exist");
    }
    return result->value;
}

std::optional<R2StorageKey> ApiKeyStore::r2() const {
    const std::shared_ptr<const Workspace> current = workspace();
    return current->r2_storage();
}

std::optional<R2StorageInfo> ApiKeyStore::r2_info() const {
    const std::shared_ptr<const Workspace> current = workspace();
    const std::optional<R2StorageKey>& key = current->r2_storage();
    return key ? std::optional<R2StorageInfo>(info(*key)) : std::nullopt;
}

ApiKeyInfo ApiKeyStore::create(
    std::string_view display_name,
    std::string_view value) {
    const std::lock_guard lock(mutex_);
    const std::shared_ptr<const Workspace> current = workspace();
    const std::uint64_t assigned_id = current->next_api_key_id();
    if (assigned_id
        >= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("API key ID space exhausted");
    }
    const std::string id = "api_key_" + std::to_string(assigned_id);
    config_->apply_api_key_create(id, display_name, value);
    return published_api_key(id);
}

ApiKeyInfo ApiKeyStore::rename(
    std::string_view id,
    std::string_view display_name) {
    const std::lock_guard lock(mutex_);
    const std::shared_ptr<const Workspace> current = workspace();
    const SavedApiKey* key = current->find_api_key(id);
    if (key == nullptr) throw std::out_of_range("Unknown API key");
    config_->apply_api_key_update(id, display_name, key->value);
    return published_api_key(id);
}

ApiKeyInfo ApiKeyStore::replace(
    std::string_view id,
    std::string_view value) {
    const std::lock_guard lock(mutex_);
    const std::shared_ptr<const Workspace> current = workspace();
    const SavedApiKey* key = current->find_api_key(id);
    if (key == nullptr) throw std::out_of_range("Unknown API key");
    config_->apply_api_key_update(id, key->display_name, value);
    return published_api_key(id);
}

void ApiKeyStore::remove(std::string_view id) {
    const std::lock_guard lock(mutex_);
    const std::shared_ptr<const Workspace> current = workspace();
    if (current->find_api_key(id) == nullptr) {
        throw std::out_of_range("Unknown API key");
    }
    config_->apply_api_key_delete(id);
}

R2StorageInfo ApiKeyStore::save_r2(
    std::string_view display_name,
    std::string_view url,
    std::string_view access_key_id,
    std::optional<std::string_view> secret_key) {
    const std::lock_guard lock(mutex_);
    const std::shared_ptr<const Workspace> current = workspace();
    const std::optional<R2StorageKey>& saved = current->r2_storage();
    if (!saved && !secret_key) {
        throw std::invalid_argument("R2 secret key is required");
    }

    std::string id;
    if (saved) {
        id = saved->id;
    } else {
        const std::uint64_t assigned_id = current->next_api_key_id();
        if (assigned_id
            >= static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("API key ID space exhausted");
        }
        id = "api_key_" + std::to_string(assigned_id);
    }

    R2StorageKey key{
        .id = std::move(id),
        .display_name = std::string(display_name),
        .url = std::string(url),
        .access_key_id = std::string(access_key_id),
        .secret_key = secret_key
            ? std::string(*secret_key) : saved->secret_key,
    };
    if (saved) {
        config_->apply_r2_storage_update(key);
    } else {
        config_->apply_r2_storage_create(key);
    }
    return published_r2_storage();
}

void ApiKeyStore::remove_r2() {
    const std::lock_guard lock(mutex_);
    const std::shared_ptr<const Workspace> current = workspace();
    if (!current->r2_storage()) {
        throw std::out_of_range("R2 storage credentials are not configured");
    }
    config_->apply_r2_storage_delete();
}

void ApiKeyStore::migrate_vault() {
    const std::lock_guard lock(mutex_);
    migrate();
}

void ApiKeyStore::migrate() {
    const std::shared_ptr<const Workspace> current = workspace();
    if (!current->api_keys().empty() || current->r2_storage()) return;

    LegacyKeys legacy = read_legacy_keys(legacy_path_);

    std::optional<R2StorageKey> r2_storage;
    const char* url = nonempty_environment("CHA_R2_URL");
    const char* access_key = nonempty_environment("CHA_R2_ACCESS_KEY_ID");
    const char* secret_key = nonempty_environment("CHA_R2_SECRET_ACCESS_KEY");
    if (url != nullptr && access_key != nullptr && secret_key != nullptr) {
        if (legacy.next_id
            >= static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("API key ID space exhausted");
        }
        r2_storage = R2StorageKey{
            .id = "api_key_" + std::to_string(legacy.next_id++),
            .display_name = "R2",
            .url = url,
            .access_key_id = access_key,
            .secret_key = secret_key,
        };
    }
    if (!legacy.keys.empty() || r2_storage) {
        config_->apply_key_migration(
            legacy.keys, r2_storage, legacy.next_id);
    }
}

} // namespace cha
