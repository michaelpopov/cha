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
#include <atomic>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <limits>
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
    std::string_view forum_id,
    std::string_view password = {}) {
    Database database(path, Database::Mode::read_only, password);
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

void delete_archived_sessions(
    const std::filesystem::path& path,
    std::string_view password) {
    Database database(path, Database::Mode::read_write, password);
    validate_workspace_session_database_identity(database);
    database.execute("DELETE FROM sessions WHERE archived_at IS NOT NULL");
}

std::atomic<bool> forced_forum_sync_failure{false};

} // namespace

void force_next_forum_sync_failure() {
    forced_forum_sync_failure.store(true);
}

SessionRepository::MaintenanceGuard::MaintenanceGuard(
    SessionRepository& repository)
    : repository_(&repository), lock_(repository.operation_mutex_) {
}

void SessionRepository::MaintenanceGuard::checkpoint() const {
    checkpoint_workspace_session_database(
        repository_->database_path_, repository_->database_password_);
}

void SessionRepository::MaintenanceGuard::synchronize_forums(
    const Workspace& workspace) const {
    repository_->synchronize_forums_unlocked(workspace);
}

void SessionRepository::MaintenanceGuard::retarget(
    std::filesystem::path database_path,
    std::string database_password) {
    repository_->database_path_ = std::move(database_path);
    repository_->database_password_ = std::move(database_password);
}

SessionRepository::MaintenanceGuard
SessionRepository::reserve_maintenance() {
    return MaintenanceGuard(*this);
}

std::filesystem::path SessionRepository::database_path() const {
    const std::shared_lock lock(operation_mutex_);
    return database_path_;
}

SessionRepository::SessionRepository(
    std::filesystem::path database_path,
    std::filesystem::path workspace_root,
    std::filesystem::path welcome_directory,
    TemporarySessionSeed temporary,
    std::string database_password)
    : workspace_root_(std::move(workspace_root)),
      database_path_(std::move(database_path)),
      database_password_(std::move(database_password)),
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
    delete_archived_sessions(database_path_, database_password_);
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

const std::filesystem::path& SessionRepository::session_database_path(
    const FullSessionId& identity) const {
    if (identity.forum_id == temporary_identity_.forum_id) {
        if (identity != temporary_identity_) throw missing_session_error(identity.session_id);
        return temporary_database_path_;
    }
    require_persistent_forum(identity.forum_id);
    return database_path_;
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
    if (forced_forum_sync_failure.exchange(false)) {
        throw std::runtime_error("Forced forum synchronization failure");
    }
    if (workspace.root() != workspace_root_) {
        throw std::runtime_error(
            "Cannot synchronize forums from a different workspace");
    }
    Database database(
        database_path_, Database::Mode::read_write, database_password_);
    validate_workspace_session_database_identity(database);
    try {
        create_entry_audio_table(database);
    } catch (const std::exception& error) {
        log_warn("Could not initialize audio cache: " + std::string(error.what()));
    }
    std::set<std::string> stored_forums;
    {
        Statement select = database.prepare("SELECT forum_id FROM forums");
        while (select.step()) stored_forums.insert(select.text(0));
    }
    std::vector<std::string> missing_forums;
    for (const WorkspaceForum& forum : workspace.forums()) {
        if (forum.id != temporary_identity_.forum_id
            && !stored_forums.contains(forum.id)) {
            missing_forums.push_back(forum.id);
        }
    }
    if (missing_forums.empty()) return;

    Transaction transaction(database);
    for (const std::string& forum_id : missing_forums) {
        Statement insert = database.prepare(
            "INSERT OR IGNORE INTO forums (forum_id) VALUES (?1)",
            std::string_view(forum_id));
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
    return list_forum(database_path_, forum_id, database_password_);
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
    Database database(
        database_path_, Database::Mode::read_only, database_password_);
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
    const auto& path = session_database_path(identity);
    Database database(path, Database::Mode::read_only,
        identity == temporary_identity_ ? std::string_view{} : database_password_);
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
    Database database(
        database_path_, Database::Mode::read_write, database_password_);
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
    Database database(
        database_path_, Database::Mode::read_write, database_password_);
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
    Database database(
        database_path_, Database::Mode::read_write, database_password_);
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
    const auto& path = session_database_path(identity);
    const bool temporary = identity == temporary_identity_;
    LoadedSessionDatabase loaded =
        load_session_database(
            path, identity, temporary ? std::string_view{} : database_password_);
    return {
        .identity = identity,
        .label = std::move(loaded.metadata.label),
        .database_path = path,
        .database_password = temporary ? std::string{} : database_password_,
        .session_key = loaded.session_key,
        .restore = std::move(loaded.restore),
    };
}

std::vector<TranscriptEntry> SessionRepository::history(
    const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    const auto& path = session_database_path(identity);
    const bool temporary = identity == temporary_identity_;
    return load_session_history(
        path, identity, temporary ? std::string_view{} : database_password_);
}

std::optional<EntryAudioLookup> SessionRepository::lookup_entry_audio(
    const FullSessionId& identity, EntryId entry_id, bool load_audio) const {
    const std::shared_lock operation(operation_mutex_);
    if (entry_id == 0
        || entry_id > static_cast<EntryId>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    const auto& path = session_database_path(identity);
    Database database(path, Database::Mode::read_only,
        identity == temporary_identity_ ? std::string_view{} : database_password_);
    validate_workspace_session_database_identity(database);
    auto entry = database.prepare(
        "SELECT e.session_key, e.text, e.kind FROM entries e "
        "JOIN sessions s ON s.session_key = e.session_key "
        "JOIN forums f ON f.forum_key = s.forum_key "
        "WHERE f.forum_id = ?1 AND s.session_id = ?2 "
        "AND s.archived_at IS NULL AND e.epoch = s.history_epoch "
        "AND e.entry_id = ?3 AND e.status = 0 AND e.kind IN (0, 1, 2)",
        identity.forum_id, identity.session_id, static_cast<std::int64_t>(entry_id));
    if (!entry.step()) return std::nullopt;
    EntryAudioLookup result{
        .session_key = entry.integer(0),
        .entry_id = entry_id,
        .database_path = path,
        .identity = identity,
        .entry_text = entry.text(1),
        .entry_kind = static_cast<EntryKind>(entry.integer(2)),
        .cached = std::nullopt,
    };
    try {
        auto cached = database.prepare(
            load_audio ? "SELECT audio, content_type FROM entry_audio WHERE session_key = ?1 AND entry_id = ?2"
                : "SELECT 1 FROM entry_audio WHERE session_key = ?1 AND entry_id = ?2",
            result.session_key, static_cast<std::int64_t>(entry_id));
        if (cached.step()) {
            result.has_cached_audio = true;
            if (load_audio) result.cached = EntryAudio{cached.blob(0), cached.text(1)};
        }
    } catch (const std::exception& error) {
        log_warn("Could not read cached audio: " + std::string(error.what()));
    }
    return result;
}

void SessionRepository::save_entry_audio(const EntryAudioLookup& entry, const EntryAudio& audio,
    const std::function<bool()>& cancelled) const {
    const std::shared_lock operation(operation_mutex_);
    if (cancelled()) return;
    const auto& path = session_database_path(entry.identity);
    if (entry.database_path != path || audio.audio.empty()) return;
    Database database(path, Database::Mode::read_write,
        entry.identity == temporary_identity_ ? std::string_view{} : database_password_);
    validate_workspace_session_database_identity(database);
    Transaction transaction(database);
    if (cancelled()) return;
    // An entry may have been deleted while synthesis was running. Never recreate it.
    auto insert = database.prepare(
        "INSERT INTO entry_audio (session_key, entry_id, audio, content_type) "
        "SELECT e.session_key, e.entry_id, ?1, ?2 FROM entries e "
        "JOIN sessions s ON s.session_key = e.session_key "
        "JOIN forums f ON f.forum_key = s.forum_key "
        "WHERE e.session_key = ?3 AND e.entry_id = ?4 AND e.text = ?5 "
        "AND f.forum_id = ?6 AND s.session_id = ?7 AND s.archived_at IS NULL "
        "AND e.epoch = s.history_epoch AND e.status = 0 AND e.kind IN (0, 1, 2) ON CONFLICT DO NOTHING");
    insert.bind_blob(1, audio.audio);
    insert.bind(2, audio.content_type);
    insert.bind(3, entry.session_key);
    insert.bind(4, static_cast<std::int64_t>(entry.entry_id));
    insert.bind(5, entry.entry_text);
    insert.bind(6, entry.identity.forum_id);
    insert.bind(7, entry.identity.session_id);
    insert.run();
    transaction.commit();
}

std::set<EntryId> SessionRepository::cached_audio_entries(const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    const auto& path = session_database_path(identity);
    Database database(path, Database::Mode::read_only,
        identity == temporary_identity_ ? std::string_view{} : database_password_);
    validate_workspace_session_database_identity(database);
    auto select = database.prepare(
        "SELECT a.entry_id FROM entry_audio a "
        "JOIN entries e USING (session_key, entry_id) "
        "JOIN sessions s USING (session_key) JOIN forums f USING (forum_key) "
        "WHERE f.forum_id = ?1 AND s.session_id = ?2 AND s.archived_at IS NULL "
        "AND e.epoch = s.history_epoch", identity.forum_id, identity.session_id);
    std::set<EntryId> result;
    while (select.step()) result.insert(static_cast<EntryId>(select.integer(0)));
    return result;
}

void SessionRepository::clear_session_audio(const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    const auto& path = session_database_path(identity);
    Database database(path, Database::Mode::read_write,
        identity == temporary_identity_ ? std::string_view{} : database_password_);
    validate_workspace_session_database_identity(database);
    Transaction transaction(database);
    require_active(database, identity);
    database.prepare(
        "DELETE FROM entry_audio WHERE session_key IN ("
        "SELECT s.session_key FROM sessions s JOIN forums f USING (forum_key) "
        "WHERE f.forum_id = ?1 AND s.session_id = ?2)",
        identity.forum_id, identity.session_id).run();
    transaction.commit();
}

} // namespace cha
