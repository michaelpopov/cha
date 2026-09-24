#include "media/pending_media_registry.h"

#include <utility>

namespace cha::app {

std::shared_ptr<PendingMediaRegistry::PendingMedia> PendingMediaRegistry::remember(
    std::string connection_id,
    std::uint64_t request_id) {
    auto pending = std::make_shared<PendingMedia>();
    pending->connection_id = std::move(connection_id);
    pending->request_id = request_id;
    pending->cancelled = std::make_shared<std::atomic_bool>(false);
    std::lock_guard lock(mutex_);
    pending_[{pending->connection_id, request_id}] = pending;
    return pending;
}

void PendingMediaRegistry::forget(
    const std::shared_ptr<PendingMedia>& pending,
    bool keep_resource) {
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(
        {pending->connection_id, pending->request_id});
    if (found != pending_.end() && found->second == pending
        && (!keep_resource || pending->resource_id.empty())) {
        pending_.erase(found);
    }
}

bool PendingMediaRegistry::set_resource(
    const std::shared_ptr<PendingMedia>& pending,
    std::string resource_id) {
    std::lock_guard lock(mutex_);
    const auto found = pending_.find(
        {pending->connection_id, pending->request_id});
    if (found == pending_.end() || found->second != pending
        || pending->cancelled->load()) {
        return false;
    }
    pending->resource_id = std::move(resource_id);
    return true;
}

void PendingMediaRegistry::cancel(
    std::string_view connection_id,
    std::uint64_t request_id) {
    std::shared_ptr<PendingMedia> pending;
    std::string resource_id;
    {
        std::lock_guard lock(mutex_);
        const auto found = pending_.find(
            {std::string(connection_id), request_id});
        if (found == pending_.end()) return;
        pending = found->second;
        pending->cancelled->store(true);
        resource_id = std::move(pending->resource_id);
        pending_.erase(found);
    }
    if (!resource_id.empty()) {
        resources_.release(connection_id, resource_id);
    }
}

void PendingMediaRegistry::cancel_connection(std::string_view connection_id) {
    {
        std::lock_guard lock(mutex_);
        for (auto item = pending_.begin();
             item != pending_.end();) {
            if (item->second->connection_id != connection_id) {
                ++item;
                continue;
            }
            item->second->cancelled->store(true);
            item = pending_.erase(item);
        }
    }
    resources_.revoke_connection(connection_id);
}

void PendingMediaRegistry::cancel_all() {
    std::lock_guard lock(mutex_);
    for (auto& [key, item] : pending_) {
        item->cancelled->store(true);
    }
    pending_.clear();
}

void PendingMediaRegistry::release_resource(
    std::string_view connection_id, std::string_view resource_id) {
    {
        std::lock_guard lock(mutex_);
        std::erase_if(pending_, [&](const auto& item) {
            const bool matches = item.second->connection_id == connection_id
                && item.second->resource_id == resource_id;
            if (matches) item.second->cancelled->store(true);
            return matches;
        });
    }
    resources_.release(connection_id, resource_id);
}

} // namespace cha::app
