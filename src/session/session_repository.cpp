#include "session/session_repository.h"

#include "session/not_found_error.h"
#include "session/session_archive.h"
#include "session/session_catalog.h"
#include "session/session_delete_conflict.h"
#include "session/session_label.h"
#include "session/session_lease.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <chrono>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cha {
namespace {

// A private directory for the temporary session, readable only by this
// process's owner. The caller-supplied identity names the database so one
// ordinary SessionCatalog serves temporary and persistent storage alike.
std::filesystem::path create_private_directory() {
    const std::filesystem::path parent = std::filesystem::temp_directory_path();
    std::mt19937_64 random(std::random_device{}());
    for (std::size_t attempt{}; attempt != 100; ++attempt) {
        const std::filesystem::path candidate = parent
            / ("cha-session-"
               + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
               + "-" + std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            return candidate;
        }
    }
    throw std::runtime_error("Failed to create private temporary session storage");
}

void move_sidecar_if_present(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) noexcept {
    try {
        if (std::filesystem::exists(source)) {
            archive_without_replacement(source, destination);
        }
    } catch (const std::exception& error) {
        log_warn("Failed to archive session database sidecar: path="
            + utf8_path(source) + " reason=" + error.what());
    }
}

} // namespace

SessionRepository::SessionRepository(
    std::vector<ForumSessionDirectory> persistent,
    TemporarySessionSeed temporary)
    : temporary_identity_(std::move(temporary.identity)),
      temporary_label_(std::move(temporary.label)) {
    for (ForumSessionDirectory& forum : persistent) {
        if (forum.forum_id == temporary_identity_.forum_id) {
            throw std::invalid_argument(
                "Forum '" + forum.forum_id + "' collides with the temporary session forum");
        }
        persistent_.emplace(std::move(forum.forum_id), std::move(forum.directory));
    }
    temporary_directory_ = create_private_directory();
    try {
        std::filesystem::permissions(
            temporary_directory_, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace);
        temporary_database_path_ = temporary_directory_
            / path_from_utf8(temporary_identity_.session_id + ".sqlite3");
        if (!create_session_database(
                temporary_database_path_,
                {.id = temporary_identity_.session_id,
                 .forum = temporary_identity_.forum_id,
                 .label = temporary_label_})) {
            throw std::runtime_error("Failed to create the temporary session database");
        }
    } catch (...) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(temporary_directory_, cleanup_error);
        throw;
    }
}

SessionRepository::~SessionRepository() {
    std::error_code error;
    std::filesystem::remove_all(temporary_directory_, error);
    if (error) log_warn("Failed to clean up temporary session storage");
}

SessionCatalog SessionRepository::catalog_for(std::string_view forum_id) const {
    if (forum_id == temporary_identity_.forum_id) {
        return SessionCatalog(temporary_directory_, temporary_identity_.forum_id);
    }
    const auto found = persistent_.find(std::string(forum_id));
    if (found == persistent_.end()) {
        throw ForumNotFoundError("Forum '" + std::string(forum_id) + "' does not exist");
    }
    return SessionCatalog(found->second, std::string(forum_id));
}

std::vector<StoredSession> SessionRepository::list(std::string_view forum_id) const {
    return catalog_for(forum_id).list();
}

void SessionRepository::validate(const SessionIdentity& identity) const {
    (void)catalog_for(identity.forum_id).inspect(identity.session_id);
}

StoredSession SessionRepository::create(
    std::string_view forum_id,
    std::string label) const {
    if (forum_id == temporary_identity_.forum_id) {
        throw ForumNotFoundError(
            "Forum '" + std::string(forum_id) + "' does not store sessions");
    }
    return catalog_for(forum_id).create(std::move(label));
}

StoredSession SessionRepository::rename(
    const SessionIdentity& identity,
    std::string label) const {
    if (identity.forum_id == temporary_identity_.forum_id) {
        throw ForumNotFoundError(
            "Forum '" + identity.forum_id + "' does not store sessions");
    }
    validate_session_label(label);
    const SessionCatalog catalog = catalog_for(identity.forum_id);
    const std::filesystem::path path = catalog.database_path(identity.session_id);
    if (!std::filesystem::is_regular_file(path)) {
        throw missing_session_error(identity.session_id);
    }
    SessionLease lease = SessionLease::acquire(path);
    if (!std::filesystem::is_regular_file(path)) {
        throw missing_session_error(identity.session_id);
    }
    rename_session_database(path, identity, label);
    return catalog.inspect(identity.session_id);
}

void SessionRepository::move_to_deleted(const SessionIdentity& identity) const {
    if (identity.forum_id == temporary_identity_.forum_id) {
        throw ForumNotFoundError(
            "Forum '" + identity.forum_id + "' does not store sessions");
    }
    const SessionCatalog catalog = catalog_for(identity.forum_id);
    const std::filesystem::path source = catalog.database_path(identity.session_id);
    const std::filesystem::path destination =
        catalog.deleted_database_path(identity.session_id);
    if (!std::filesystem::is_regular_file(source)) {
        throw missing_session_error(identity.session_id);
    }
    std::filesystem::create_directories(destination.parent_path());
    if (std::filesystem::exists(destination)) {
        throw SessionDeleteConflictError(
            "A deleted session database already exists at '"
            + utf8_path(destination) + "'");
    }

    SessionLease lease = SessionLease::acquire(source);
    if (!std::filesystem::is_regular_file(source)) {
        throw missing_session_error(identity.session_id);
    }
    archive_without_replacement(source, destination);

    for (const std::string_view suffix : {"-journal", "-wal", "-shm"}) {
        move_sidecar_if_present(
            path_from_utf8(utf8_path(source) + std::string(suffix)),
            path_from_utf8(utf8_path(destination) + std::string(suffix)));
    }
}

PreparedSession SessionRepository::prepare(const SessionIdentity& identity) const {
    const std::filesystem::path database_path =
        catalog_for(identity.forum_id).database_path(identity.session_id);
    // Absence is settled before a lease exists, so asking for a session that
    // was never stored does not leave a companion lock behind for it.
    if (!std::filesystem::is_regular_file(database_path)) {
        throw missing_session_error(identity.session_id);
    }
    SessionLease lease = SessionLease::acquire(database_path);
    // Opening is the one write-capable moment every stored session passes
    // through, still behind its lease, so a schema-migration step here migrates
    // every database exactly once and leaves the restore read itself read-only.
    migrate_session_database(database_path);
    // Nothing observed before the lease is trusted: the one authoritative read
    // of this database happens here, behind it. A file that was leased first
    // therefore reports busy even if it would also have proved invalid.
    LoadedSessionDatabase loaded = load_session_database(database_path, identity);
    return {
        .identity = identity,
        .label = std::move(loaded.metadata.label),
        .database_path = database_path,
        .lease = std::move(lease),
        .restore = std::move(loaded.restore),
    };
}

} // namespace cha
