#pragma once

#include "chat/session_identity.h"
#include "chat/transcript.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cha::app {

enum class ResourceKind { speech, entry_audio };

struct ResourceBytes {
    std::string mime_type;
    std::string body;
};

struct MediaResource {
    std::string resource_id;
    std::string url;
    std::string mime_type;
    std::uint64_t byte_length{};
};

// Connection-scoped in-memory media. Handles are opaque; never path names.
class MediaResources {
public:
    static constexpr std::string_view kPathPrefix = "/media/";

    [[nodiscard]] static bool valid_id(std::string_view id) noexcept;
    [[nodiscard]] static std::string url_for(std::string_view id);

    [[nodiscard]] std::string add(
        std::string_view connection_id,
        std::uint64_t context_epoch,
        ResourceKind kind,
        ResourceBytes bytes,
        std::optional<FullSessionId> session = {},
        std::optional<EntryId> entry = {});

    [[nodiscard]] std::optional<ResourceBytes> read(
        std::string_view connection_id,
        std::string_view resource_id) const;

    bool release(std::string_view connection_id, std::string_view resource_id);
    void revoke_connection(std::string_view connection_id);
    void revoke_session(const FullSessionId& session);
    void revoke_all();

private:
    struct Stored {
        std::string connection_id;
        std::uint64_t context_epoch{};
        ResourceKind kind{ResourceKind::speech};
        ResourceBytes bytes;
        std::optional<FullSessionId> session;
        std::optional<EntryId> entry;
    };

    mutable std::mutex mutex_;
    std::uint64_t next_id_{1};
    std::unordered_map<std::string, Stored> entries_;
};

} // namespace cha::app
