#pragma once

#include "chat/session_identity.h"
#include "chat/transcript.h"
#include "session/stored_session.h"

#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace cha {

class SessionRepository;

} // namespace cha

namespace cha::web {

// A best-effort runtime projection of persistent sessions into Markdown files.
// Construction performs the initial synchronization and therefore fails when
// the configured mirror cannot be used. Later updates are logged on failure so
// mirroring never changes the outcome of an already-persisted session update.
class SessionMirror final {
public:
    SessionMirror(
        std::filesystem::path root,
        const SessionRepository& sessions);

    SessionMirror(const SessionMirror&) = delete;
    SessionMirror& operator=(const SessionMirror&) = delete;

    void add(const StoredSession& session);
    void update(
        const FullSessionId& identity,
        std::string_view label,
        std::span<const TranscriptEntry> entries);

private:
    struct MirroredSession {
        std::string label;
        std::filesystem::path path;
    };

    [[nodiscard]] std::filesystem::path allocate_session_path(
        const FullSessionId& identity,
        std::string_view label) const;
    void update_locked(
        const FullSessionId& identity,
        std::string_view label,
        std::span<const TranscriptEntry> entries);

    std::filesystem::path root_;
    std::map<std::string, std::filesystem::path, std::less<>> forums_;
    std::map<FullSessionId, MirroredSession, std::less<>> sessions_;
    std::mutex mutex_;
};

// Converts a display name to one safe filesystem component. The same rule is
// used for forum directories and session Markdown filenames.
[[nodiscard]] std::string mirror_path_name(std::string_view display_name);

} // namespace cha::web
