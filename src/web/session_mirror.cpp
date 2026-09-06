#include "web/session_mirror.h"

#include "session/session_repository.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "web/session_markdown.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"

#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cha::web {
namespace {

bool forbidden_path_character(char character) {
    switch (character) {
    case '*':
    case '"':
    case '\\':
    case '/':
    case '<':
    case '>':
    case ':':
    case '|':
    case '?':
    case '#':
    case '^':
    case '[':
    case ']':
        return true;
    default:
        return static_cast<unsigned char>(character) < 0x20;
    }
}

std::string numbered_name(
    std::string_view base,
    std::size_t suffix) {
    if (suffix == 0) return std::string(base);
    return std::string(base) + " (" + std::to_string(suffix) + ')';
}

void require_directory_or_create(const std::filesystem::path& path) {
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path);
    if (!std::filesystem::exists(status)) {
        create_private_directory(path);
        return;
    }
    require_directory(path);
}

void replace_by_rename(
    const std::filesystem::path& from,
    const std::filesystem::path& to) {
    if (from == to || !std::filesystem::exists(from)) return;

    const std::filesystem::file_status target =
        std::filesystem::symlink_status(to);
    if (std::filesystem::exists(target)) {
        if (!std::filesystem::is_regular_file(target)) {
            throw std::runtime_error(
                "Mirror target '" + utf8_path(to)
                + "' is not a regular file");
        }
        if (!std::filesystem::remove(to)) {
            throw std::runtime_error(
                "Failed to replace mirror file '" + utf8_path(to) + "'");
        }
    }
    std::filesystem::rename(from, to);
}

} // namespace

std::string mirror_path_name(std::string_view display_name) {
    std::string result;
    result.reserve(display_name.size());
    for (const char character : display_name) {
        result.push_back(forbidden_path_character(character) ? '-' : character);
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
    }
    return result.empty() ? "session" : result;
}

namespace {

template<typename List, typename History>
MirrorRebuildInput collect_mirror_input(List list, History history) {
    const std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");

    MirrorRebuildInput input;
    for (const WorkspaceForum& forum : workspace->forums()) {
        if (forum.id == entrance_id) continue;
        input.forums.push_back({forum.id, forum.display_name});
        for (const StoredSession& stored : list(forum.id)) {
            input.sessions.push_back({stored, history(stored.identity)});
        }
    }
    return input;
}

} // namespace

MirrorRebuildInput mirror_rebuild_input(const SessionRepository& sessions) {
    return collect_mirror_input(
        [&sessions](std::string_view forum_id) {
            return sessions.list(forum_id);
        },
        [&sessions](const FullSessionId& identity) {
            return sessions.history(identity);
        });
}

MirrorRebuildInput mirror_rebuild_input(
    const SessionRepository::MaintenanceGuard& sessions) {
    return collect_mirror_input(
        [&sessions](std::string_view forum_id) {
            return sessions.list(forum_id);
        },
        [&sessions](const FullSessionId& identity) {
            return sessions.history(identity);
        });
}

SessionMirror::SessionMirror() = default;

SessionMirror::SessionMirror(
    std::filesystem::path root,
    const SessionRepository& repository) {
    retarget(std::move(root), mirror_rebuild_input(repository));
}

void SessionMirror::retarget(
    std::optional<std::filesystem::path> root,
    MirrorRebuildInput input) {
    const std::lock_guard lock(mutex_);
    forums_.clear();
    sessions_.clear();
    root_.reset();
    if (!root) return;

    try {
        require_directory(*root);
        std::set<std::filesystem::path> used_forums;
        for (const MirrorRebuildInput::Forum& forum : input.forums) {
            const std::string base = mirror_path_name(forum.display_name);
            std::filesystem::path path;
            for (std::size_t suffix{};; ++suffix) {
                path = *root / numbered_name(base, suffix);
                if (used_forums.insert(path).second) break;
            }
            require_directory_or_create(path);
            forums_.emplace(forum.id, std::move(path));
        }

        for (const MirrorRebuildInput::Session& session : input.sessions) {
            const std::filesystem::path path =
                allocate_session_path(session.stored.identity, session.stored.label);
            sessions_.emplace(session.stored.identity, MirroredSession{
                session.stored.label, path});
            create_private_file(
                path,
                session_markdown(session.stored.label, session.history));
        }
        root_ = std::move(root);
    } catch (...) {
        forums_.clear();
        sessions_.clear();
        root_.reset();
        throw;
    }
}

std::filesystem::path SessionMirror::allocate_session_path(
    const FullSessionId& identity,
    std::string_view label) const {
    const auto forum = forums_.find(identity.forum_id);
    if (forum == forums_.end()) {
        throw std::runtime_error(
            "Session mirror has no forum '" + identity.forum_id + "'");
    }

    std::set<std::filesystem::path> used;
    for (const auto& [key, mirrored] : sessions_) {
        if (key != identity && key.forum_id == identity.forum_id) {
            used.insert(mirrored.path);
        }
    }

    const std::string base = mirror_path_name(label);
    for (std::size_t suffix{};; ++suffix) {
        std::filesystem::path candidate =
            forum->second / (numbered_name(base, suffix) + ".md");
        if (!used.contains(candidate)) return candidate;
    }
}

void SessionMirror::add(const StoredSession& session) {
    update(session.identity, session.label, {});
}

void SessionMirror::update(
    const FullSessionId& identity,
    std::string_view label,
    std::span<const TranscriptEntry> entries) {
    try {
        const std::lock_guard lock(mutex_);
        if (!root_ || !forums_.contains(identity.forum_id)) return;
        update_locked(identity, label, entries);
    } catch (const std::exception& error) {
        log_warn(
            "Session mirror update failed forum_id=" + identity.forum_id
            + " session_id=" + identity.session_id
            + " reason=" + error.what());
    }
}

void SessionMirror::update_locked(
    const FullSessionId& identity,
    std::string_view label,
    std::span<const TranscriptEntry> entries) {
    auto found = sessions_.find(identity);
    if (found == sessions_.end()) {
        const std::filesystem::path path =
            allocate_session_path(identity, label);
        found = sessions_.emplace(
            identity,
            MirroredSession{std::string(label), path}).first;
    } else if (found->second.label != label) {
        const std::filesystem::path path =
            allocate_session_path(identity, label);
        replace_by_rename(found->second.path, path);
        found->second.label = label;
        found->second.path = path;
    }

    create_private_file(
        found->second.path,
        session_markdown(label, entries));
}

} // namespace cha::web
