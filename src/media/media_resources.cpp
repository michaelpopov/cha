#include "media/media_resources.h"

#include <cctype>

namespace cha::app {

bool MediaResources::valid_id(std::string_view id) noexcept {
    if (id.size() < 2 || id.size() > 20 || id.front() != 'r') return false;
    for (std::size_t i = 1; i < id.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(id[i]);
        if (!std::isdigit(ch)) return false;
    }
    return true;
}

std::string MediaResources::url_for(std::string_view id) {
    return std::string(kPathPrefix) + std::string(id);
}

std::string MediaResources::add(
    std::string_view connection_id,
    std::uint64_t context_epoch,
    ResourceKind kind,
    ResourceBytes bytes,
    std::optional<FullSessionId> session,
    std::optional<EntryId> entry) {
    std::lock_guard lock(mutex_);
    const std::string id = "r" + std::to_string(next_id_++);
    entries_.emplace(
        id,
        Stored{
            std::string(connection_id),
            context_epoch,
            kind,
            std::move(bytes),
            std::move(session),
            entry});
    return id;
}

std::optional<ResourceBytes> MediaResources::read(
    std::string_view connection_id,
    std::string_view resource_id,
    std::uint64_t context_epoch) const {
    if (!valid_id(resource_id)) return std::nullopt;
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(std::string(resource_id));
    if (found == entries_.end()) return std::nullopt;
    if (found->second.connection_id != connection_id) return std::nullopt;
    if (found->second.context_epoch != context_epoch) return std::nullopt;
    return found->second.bytes;
}

bool MediaResources::release(
    std::string_view connection_id,
    std::string_view resource_id) {
    if (!valid_id(resource_id)) return false;
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(std::string(resource_id));
    if (found == entries_.end()) return false;
    if (found->second.connection_id != connection_id) return false;
    entries_.erase(found);
    return true;
}

void MediaResources::revoke_connection(std::string_view connection_id) {
    std::lock_guard lock(mutex_);
    std::erase_if(entries_, [&](const auto& item) {
        return item.second.connection_id == connection_id;
    });
}

void MediaResources::revoke_session(const FullSessionId& session) {
    std::lock_guard lock(mutex_);
    std::erase_if(entries_, [&](const auto& item) {
        return item.second.session && *item.second.session == session;
    });
}

void MediaResources::revoke_all() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

} // namespace cha::app
