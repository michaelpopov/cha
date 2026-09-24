#pragma once

#include "chat/session_identity.h"
#include "chat/transcript.h"
#include "media/audio_stream.h"

#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cha::app {

enum class ResourceKind { speech, entry_audio };

using ResourceBytes = AudioChunk;

struct MediaResource {
    std::string resource_id;
    std::string url;
    std::string mime_type;
    std::uint64_t byte_length{};
    bool streaming{};
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
        std::string_view resource_id,
        std::uint64_t context_epoch) const;

    [[nodiscard]] std::string add_stream(
        std::string_view connection_id, std::uint64_t context_epoch,
        ResourceKind kind, std::shared_ptr<AudioStream> stream,
        std::optional<FullSessionId> session = {}, std::optional<EntryId> entry = {});
    [[nodiscard]] std::optional<AudioChunk> read_chunk(
        std::string_view connection_id, std::string_view resource_id,
        std::uint64_t context_epoch, std::uint64_t offset) const;

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
        std::shared_ptr<AudioStream> stream;
    };

    mutable std::mutex mutex_;
    std::uint64_t next_id_{1};
    std::unordered_map<std::string, Stored> entries_;
};

} // namespace cha::app
