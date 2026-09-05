#include "session/session_repository.h"

#include "session/not_found_error.h"
#include "session/session_label.h"
#include "session/session_timestamp.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;
using Transaction = storage::SqliteTransaction;

constexpr std::size_t max_session_id_attempts = 100;

bool local_time(std::time_t now, std::tm& result) {
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    const std::tm* const local = std::localtime(&now);
    if (local == nullptr) return false;
    result = *local;
    return true;
}

std::string timestamp_name(std::time_t now) {
    std::tm local{};
    if (!local_time(now, local)) {
        throw std::runtime_error("Failed to read local time for session name");
    }
    std::ostringstream result;
    result << std::put_time(&local, "%Y-%m-%d-%H-%M-%S") << "-session";
    return result.str();
}

void require_active(
    Database& database,
    const FullSessionId& identity) {
    Statement statement = database.prepare(
        "SELECT 1 "
        "FROM sessions AS s JOIN forums AS f USING (forum_key) "
        "WHERE f.forum_id = ?1 AND s.session_id = ?2 "
        "AND s.archived_at IS NULL",
        std::string_view(identity.forum_id),
        std::string_view(identity.session_id));
    if (!statement.step()) throw missing_session_error(identity.session_id);
}

std::vector<StoredSession> list_forum(
    const std::filesystem::path& path,
    std::string_view forum_id) {
    Database database(path, Database::Mode::read_only);
    validate_workspace_session_database_identity(database);
    Statement statement = database.prepare(
        "SELECT s.session_id, s.label, s.updated_at "
        "FROM sessions AS s JOIN forums AS f USING (forum_key) "
        "WHERE f.forum_id = ?1 AND s.archived_at IS NULL "
        "ORDER BY s.session_id",
        forum_id);
    std::vector<StoredSession> result;
    while (statement.step()) {
        result.push_back({
            .identity = {std::string(forum_id), statement.text(0)},
            .label = statement.text(1),
            .updated_at = statement.integer(2),
        });
    }
    return result;
}

void delete_archived_sessions(const std::filesystem::path& path) {
    Database database(path, Database::Mode::read_write);
    validate_workspace_session_database_identity(database);
    database.execute("DELETE FROM sessions WHERE archived_at IS NOT NULL");
}

} // namespace

SessionRepository::MaintenanceGuard::MaintenanceGuard(
    const SessionRepository& repository)
    : repository_(&repository), lock_(repository.operation_mutex_) {
}

void SessionRepository::MaintenanceGuard::checkpoint() const {
    checkpoint_workspace_session_database(repository_->database_path_);
}

void SessionRepository::MaintenanceGuard::synchronize_forums(
    const Workspace& workspace) const {
    repository_->synchronize_forums_unlocked(workspace);
}

SessionRepository::MaintenanceGuard
SessionRepository::reserve_maintenance() const {
    return MaintenanceGuard(*this);
}

SessionRepository::SessionRepository(
    std::filesystem::path database_path,
    std::filesystem::path workspace_root,
    std::filesystem::path welcome_directory,
    TemporarySessionSeed temporary)
    : workspace_root_(std::move(workspace_root)),
      database_path_(std::move(database_path)),
      temporary_identity_(std::move(temporary.identity)),
      temporary_label_(std::move(temporary.label)) {
    if (!std::filesystem::is_regular_file(database_path_)) {
        throw std::runtime_error(
            "Workspace session database path '" + utf8_path(database_path_)
            + "' is not a regular file");
    }
    if (!std::filesystem::is_directory(welcome_directory)) {
        throw std::runtime_error(
            "Welcome directory '" + utf8_path(welcome_directory)
            + "' is not a directory");
    }
    delete_archived_sessions(database_path_);
    synchronize_forums();

    temporary_database_path_ = welcome_directory / "sessions.sqlite3";
    if (!create_session_database(
            temporary_database_path_,
            {.id = temporary_identity_.session_id,
             .forum = temporary_identity_.forum_id,
             .label = temporary_label_})) {
        throw std::runtime_error(
            "Failed to create the temporary session database");
    }
    initialize_workspace_session_database_runtime(temporary_database_path_);
}

SessionRepository::~SessionRepository() {
    try {
        const MaintenanceGuard maintenance = reserve_maintenance();
        maintenance.checkpoint();
    } catch (const std::exception& error) {
        log_warn(
            "Failed to checkpoint workspace session database: "
            + std::string(error.what()));
    }
}

void SessionRepository::require_persistent_forum(
    std::string_view forum_id) const {
    if (forum_id == temporary_identity_.forum_id) {
        throw ForumNotFoundError(
            "Forum '" + std::string(forum_id) + "' does not store sessions");
    }
    const std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace || workspace->root() != workspace_root_) {
        throw std::runtime_error(
            "Session repository has no matching loaded workspace");
    }
    if (workspace->find_forum(forum_id) == nullptr) {
        throw ForumNotFoundError(
            "Forum '" + std::string(forum_id) + "' does not exist");
    }
}

void SessionRepository::synchronize_forums() const {
    const std::shared_lock operation(operation_mutex_);
    const std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace || workspace->root() != workspace_root_) {
        throw std::runtime_error(
            "Session repository has no matching loaded workspace");
    }
    synchronize_forums_unlocked(*workspace);
}

void SessionRepository::synchronize_forums(const Workspace& workspace) const {
    const std::shared_lock operation(operation_mutex_);
    synchronize_forums_unlocked(workspace);
}

void SessionRepository::synchronize_forums_unlocked(
    const Workspace& workspace) const {
    if (workspace.root() != workspace_root_) {
        throw std::runtime_error(
            "Cannot synchronize forums from a different workspace");
    }
    Database database(database_path_, Database::Mode::read_write);
    validate_workspace_session_database_identity(database);
    Transaction transaction(database);
    for (const WorkspaceForum& forum : workspace.forums()) {
        if (forum.id == temporary_identity_.forum_id) continue;
        Statement insert = database.prepare(
            "INSERT OR IGNORE INTO forums (forum_id) VALUES (?1)",
            std::string_view(forum.id));
        insert.run();
    }
    transaction.commit();
}

std::vector<StoredSession> SessionRepository::list(
    std::string_view forum_id) const {
    const std::shared_lock operation(operation_mutex_);
    if (forum_id == temporary_identity_.forum_id) {
        return list_forum(temporary_database_path_, forum_id);
    }
    require_persistent_forum(forum_id);
    return list_forum(database_path_, forum_id);
}

std::vector<StoredSession> SessionRepository::recent() const {
    const std::shared_lock operation(operation_mutex_);
    const std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace || workspace->root() != workspace_root_) {
        throw std::runtime_error(
            "Session repository has no matching loaded workspace");
    }
    std::set<std::string, std::less<>> current_forums;
    for (const WorkspaceForum& forum : workspace->forums()) {
        if (forum.id == temporary_identity_.forum_id) continue;
        current_forums.insert(forum.id);
    }
    Database database(database_path_, Database::Mode::read_only);
    validate_workspace_session_database_identity(database);
    Statement statement = database.prepare(
        "SELECT f.forum_id, s.session_id, s.label, s.updated_at "
        "FROM sessions AS s JOIN forums AS f USING (forum_key) "
        "WHERE s.archived_at IS NULL "
        "ORDER BY s.updated_at DESC, f.forum_id, s.session_id");
    std::vector<StoredSession> result;
    while (statement.step()) {
        const std::string forum_id = statement.text(0);
        if (!current_forums.contains(forum_id)) continue;
        result.push_back({
            .identity = {forum_id, statement.text(1)},
            .label = statement.text(2),
            .updated_at = statement.integer(3),
        });
    }
    for (StoredSession& temporary :
         list_forum(temporary_database_path_, temporary_identity_.forum_id)) {
        const auto position = std::lower_bound(
            result.begin(), result.end(), temporary,
            [](const StoredSession& left, const StoredSession& right) {
                if (left.updated_at != right.updated_at) {
                    return left.updated_at > right.updated_at;
                }
                return left.identity < right.identity;
            });
        result.insert(position, std::move(temporary));
    }
    return result;
}

void SessionRepository::validate(const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    if (identity.forum_id == temporary_identity_.forum_id) {
        if (identity != temporary_identity_) {
            throw missing_session_error(identity.session_id);
        }
        Database database(
            temporary_database_path_, Database::Mode::read_only);
        validate_workspace_session_database_identity(database);
        require_active(database, identity);
        return;
    }
    require_persistent_forum(identity.forum_id);
    Database database(database_path_, Database::Mode::read_only);
    validate_workspace_session_database_identity(database);
    require_active(database, identity);
}

StoredSession SessionRepository::create(
    std::string_view forum_id,
    std::string label) const {
    const std::shared_lock operation(operation_mutex_);
    require_persistent_forum(forum_id);
    if (!label.empty()) validate_session_label(label);
    const std::string base_id = timestamp_name(std::time(nullptr));
    Database database(database_path_, Database::Mode::read_write);
    validate_workspace_session_database_identity(database);
    Transaction transaction(database);
    Statement add_forum = database.prepare(
        "INSERT INTO forums (forum_id) VALUES (?1) "
        "ON CONFLICT(forum_id) DO NOTHING",
        forum_id);
    add_forum.run();
    Statement forum = database.prepare(
        "SELECT forum_key FROM forums WHERE forum_id = ?1", forum_id);
    if (!forum.step()) {
        throw std::runtime_error(
            "Configured forum is missing from the session database");
    }
    const std::int64_t forum_key = forum.integer(0);
    for (std::size_t suffix = 1;
         suffix <= max_session_id_attempts;
         ++suffix) {
        const std::string id = suffix == 1
            ? base_id : base_id + "-" + std::to_string(suffix);
        const std::string effective_label = label.empty() ? id : label;
        const std::int64_t updated_at = session_timestamp();
        Statement insert = database.prepare(
            "INSERT INTO sessions (forum_key, session_id, label, "
            "updated_at, history_epoch, next_entry_id, next_request_id) "
            "VALUES (?1, ?2, ?3, ?4, 1, 1, 1)",
            forum_key,
            std::string_view(id),
            std::string_view(effective_label),
            updated_at);
        try {
            insert.run();
        } catch (const std::runtime_error&) {
            if (sqlite3_extended_errcode(database.handle())
                != SQLITE_CONSTRAINT_UNIQUE) {
                throw;
            }
            // Retry only the generated identity collision. A future UNIQUE
            // constraint on another column must remain a storage error.
            Statement collision = database.prepare(
                "SELECT 1 FROM sessions "
                "WHERE forum_key = ?1 AND session_id = ?2",
                forum_key,
                std::string_view(id));
            if (!collision.step()) throw;
            continue;
        }
        transaction.commit();
        return {
            .identity = {std::string(forum_id), id},
            .label = effective_label,
            .updated_at = updated_at,
        };
    }
    throw std::runtime_error(
        "Failed to allocate a unique session ID for forum '"
        + std::string(forum_id) + "' after "
        + std::to_string(max_session_id_attempts) + " attempts");
}

StoredSession SessionRepository::rename(
    const FullSessionId& identity,
    std::string label) const {
    const std::shared_lock operation(operation_mutex_);
    require_persistent_forum(identity.forum_id);
    validate_session_label(label);
    Database database(database_path_, Database::Mode::read_write);
    validate_workspace_session_database_identity(database);
    Transaction transaction(database);
    const std::int64_t updated_at = session_timestamp();
    Statement update = database.prepare(
        "UPDATE sessions SET label = ?1, updated_at = ?2 "
        "WHERE session_key = (SELECT s.session_key FROM sessions AS s "
        "JOIN forums AS f USING (forum_key) WHERE f.forum_id = ?3 "
        "AND s.session_id = ?4) AND archived_at IS NULL",
        std::string_view(label),
        updated_at,
        std::string_view(identity.forum_id),
        std::string_view(identity.session_id));
    update.run();
    if (database.changes() != 1) {
        throw missing_session_error(identity.session_id);
    }
    transaction.commit();
    return {
        .identity = identity,
        .label = std::move(label),
        .updated_at = updated_at,
    };
}

void SessionRepository::delete_session(const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    require_persistent_forum(identity.forum_id);
    Database database(database_path_, Database::Mode::read_write);
    validate_workspace_session_database_identity(database);
    Transaction transaction(database);
    Statement remove = database.prepare(
        "DELETE FROM sessions WHERE session_key = ("
        "SELECT s.session_key FROM sessions AS s "
        "JOIN forums AS f USING (forum_key) WHERE f.forum_id = ?1 "
        "AND s.session_id = ?2) AND archived_at IS NULL",
        std::string_view(identity.forum_id),
        std::string_view(identity.session_id));
    remove.run();
    if (database.changes() != 1) {
        throw missing_session_error(identity.session_id);
    }
    transaction.commit();
}

PreparedSession SessionRepository::prepare(
    const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    const bool temporary = identity.forum_id == temporary_identity_.forum_id;
    if (temporary && identity != temporary_identity_) {
        throw missing_session_error(identity.session_id);
    }
    if (!temporary) require_persistent_forum(identity.forum_id);
    const std::filesystem::path& path = temporary
        ? temporary_database_path_ : database_path_;
    LoadedSessionDatabase loaded =
        load_session_database(path, identity);
    return {
        .identity = identity,
        .label = std::move(loaded.metadata.label),
        .database_path = path,
        .session_key = loaded.session_key,
        .restore = std::move(loaded.restore),
    };
}

std::vector<TranscriptEntry> SessionRepository::history(
    const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    const bool temporary = identity.forum_id == temporary_identity_.forum_id;
    if (temporary && identity != temporary_identity_) {
        throw missing_session_error(identity.session_id);
    }
    if (!temporary) require_persistent_forum(identity.forum_id);
    const std::filesystem::path& path = temporary
        ? temporary_database_path_ : database_path_;
    return load_session_history(path, identity);
}

} // namespace cha
