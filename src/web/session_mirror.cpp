#include "web/session_mirror.h"

#include "session/session_repository.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "web/session_markdown.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"

#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

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

SessionMirror::SessionMirror(
    std::filesystem::path root,
    const SessionRepository& repository)
    : root_(std::move(root)) {
    require_directory(root_);

    const std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");

    std::set<std::filesystem::path> used_forums;
    for (const WorkspaceForum& forum : workspace->forums()) {
        if (forum.id == entrance_id) continue;
        const std::string base = mirror_path_name(forum.display_name);
        std::filesystem::path path;
        for (std::size_t suffix{};; ++suffix) {
            path = root_ / numbered_name(base, suffix);
            if (used_forums.insert(path).second) break;
        }
        require_directory_or_create(path);
        forums_.emplace(forum.id, std::move(path));
    }

    for (const auto& [forum_id, directory] : forums_) {
        (void)directory;
        for (const StoredSession& stored : repository.list(forum_id)) {
            const std::filesystem::path path =
                allocate_session_path(stored.identity, stored.label);
            sessions_.emplace(stored.identity, MirroredSession{
                stored.label, path});
            create_private_file(
                path,
                session_markdown(
                    stored.label,
                    repository.history(stored.identity)));
        }
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
        if (!forums_.contains(identity.forum_id)) return;
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
