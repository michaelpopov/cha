#include "providers/api_key_store.h"

#include "util/path_name.h"
#include "util/private_filesystem.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>

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

} // namespace

ApiKeyStore::ApiKeyStore(std::filesystem::path path)
    : path_(std::move(path)) {
    if (!std::filesystem::exists(path_)) return;
    tighten_private_file(path_);
    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read API key file '" + utf8_path(path_) + "'");
    }
    Json json;
    try {
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
        for (const auto& [id, entry] : json["keys"].items()) {
            const std::optional<std::uint64_t> suffix = id_suffix(id);
            if (!suffix || !entry.is_object() || entry.size() != 2
                || !entry.contains("display_name")
                || !entry["display_name"].is_string()
                || !entry.contains("value") || !entry["value"].is_string()) {
                throw std::runtime_error("invalid entry");
            }
            Record record{
                .display_name = entry["display_name"].get<std::string>(),
                .value = entry["value"].get<std::string>(),
            };
            validate_display_name(record.display_name);
            validate_secret(record.value);
            records_.emplace(id, std::move(record));
            highest_id = std::max(highest_id, *suffix);
        }
        if (legacy) {
            if (highest_id == std::numeric_limits<std::uint64_t>::max()) {
                throw std::runtime_error("API key ID space exhausted");
            }
            next_id_ = highest_id + 1;
        } else {
            next_id_ = json["next_id"].get<std::uint64_t>();
            if (next_id_ == 0 || next_id_ <= highest_id) {
                throw std::runtime_error("invalid next ID");
            }
        }
    } catch (const std::exception&) {
        throw std::runtime_error(
            "API key file '" + utf8_path(path_) + "' is invalid");
    }
}

void ApiKeyStore::validate_display_name(std::string_view value) {
    const bool only_space = std::ranges::all_of(value, [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    if (value.empty() || value.size() > 100 || only_space
        || value.find_first_of("\r\n") != std::string_view::npos) {
        throw std::invalid_argument("Invalid API key name");
    }
}

void ApiKeyStore::validate_secret(std::string_view value) {
    if (value.empty() || value.size() > 16 * 1024
        || value.find_first_of("\r\n") != std::string_view::npos) {
        throw std::invalid_argument("Invalid API key value");
    }
}

ApiKeyInfo ApiKeyStore::info(std::string_view id, const Record& record) {
    return {
        .id = std::string(id),
        .display_name = record.display_name,
        .has_value = !record.value.empty(),
    };
}

std::vector<ApiKeyInfo> ApiKeyStore::list() const {
    const std::lock_guard lock(mutex_);
    std::vector<ApiKeyInfo> result;
    result.reserve(records_.size());
    for (const auto& [id, record] : records_) result.push_back(info(id, record));
    std::ranges::sort(result, {}, [](const ApiKeyInfo& key) {
        std::string name = key.display_name;
        std::ranges::transform(name, name.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        return name;
    });
    return result;
}

std::optional<ApiKeyInfo> ApiKeyStore::find(std::string_view id) const {
    const std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) return std::nullopt;
    return info(found->first, found->second);
}

std::string ApiKeyStore::value(std::string_view id) const {
    const std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) {
        throw std::runtime_error(
            "API key '" + std::string(id) + "' does not exist");
    }
    return found->second.value;
}

ApiKeyInfo ApiKeyStore::create(
    std::string_view display_name,
    std::string_view value) {
    validate_display_name(display_name);
    validate_secret(value);
    const std::lock_guard lock(mutex_);
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("API key ID space exhausted");
    }
    const std::uint64_t assigned_id = next_id_;
    const std::string id = "api_key_" + std::to_string(assigned_id);
    auto [position, inserted] = records_.emplace(
        id, Record{std::string(display_name), std::string(value)});
    if (!inserted) throw std::logic_error("API key ID was already assigned");
    ++next_id_;
    try {
        save_unlocked();
    } catch (...) {
        records_.erase(position);
        next_id_ = assigned_id;
        throw;
    }
    return info(position->first, position->second);
}

ApiKeyInfo ApiKeyStore::rename(
    std::string_view id,
    std::string_view display_name) {
    validate_display_name(display_name);
    const std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) throw std::out_of_range("Unknown API key");
    const std::string previous = found->second.display_name;
    found->second.display_name = display_name;
    try {
        save_unlocked();
    } catch (...) {
        found->second.display_name = previous;
        throw;
    }
    return info(found->first, found->second);
}

ApiKeyInfo ApiKeyStore::replace(
    std::string_view id,
    std::string_view value) {
    validate_secret(value);
    const std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) throw std::out_of_range("Unknown API key");
    const std::string previous = found->second.value;
    found->second.value = value;
    try {
        save_unlocked();
    } catch (...) {
        found->second.value = previous;
        throw;
    }
    return info(found->first, found->second);
}

void ApiKeyStore::remove(std::string_view id) {
    const std::lock_guard lock(mutex_);
    const auto found = records_.find(id);
    if (found == records_.end()) throw std::out_of_range("Unknown API key");
    const std::string removed_id = found->first;
    const Record removed = found->second;
    records_.erase(found);
    try {
        save_unlocked();
    } catch (...) {
        records_.emplace(removed_id, removed);
        throw;
    }
}

void ApiKeyStore::save_unlocked() const {
    Json keys = Json::object();
    for (const auto& [id, record] : records_) {
        keys[id] = {
            {"display_name", record.display_name},
            {"value", record.value},
        };
    }
    const Json json = {
        {"version", 2},
        {"next_id", next_id_},
        {"keys", std::move(keys)},
    };
    create_private_file(path_, json.dump(2) + '\n');
}

} // namespace cha
