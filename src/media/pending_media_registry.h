#pragma once

#include "media/media_resources.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace cha::app {

// Keeps a completed reply cancellable until its resource is released, even
// when the reply is still queued for delivery by the bridge.
class PendingMediaRegistry {
public:
    explicit PendingMediaRegistry(MediaResources& resources) : resources_(resources) {}

    struct PendingMedia {
        std::string connection_id;
        std::uint64_t request_id{};
        std::shared_ptr<std::atomic_bool> cancelled;
        std::string resource_id;
    };
    struct Cleanup {
        PendingMediaRegistry& owner;
        std::shared_ptr<PendingMedia> pending;

        ~Cleanup() {
            // A completed speech reply can still be queued in the bridge.
            // Keep its cancellation association until release or cancellation.
            owner.forget(pending, true);
        }
    };

    std::shared_ptr<PendingMedia> remember(std::string connection_id, std::uint64_t request_id);
    void forget(const std::shared_ptr<PendingMedia>& pending, bool keep_resource = false);
    bool set_resource(const std::shared_ptr<PendingMedia>& pending, std::string resource_id);
    void cancel(std::string_view connection_id, std::uint64_t request_id);
    void cancel_connection(std::string_view connection_id);
    void cancel_all();
    void release_resource(std::string_view connection_id, std::string_view resource_id);

private:
    MediaResources& resources_;
    std::mutex mutex_;
    std::map<std::pair<std::string, std::uint64_t>, std::shared_ptr<PendingMedia>>
        pending_;
};

} // namespace cha::app
