#include "storage/session_repository.h"

#include "storage/folder_store_storage.h"
#include "storage/not_found_error.h"
#include "storage/session_label.h"
#include "storage/session_timestamp.h"
#include "storage/sqlite_storage.h"
#include "storage/workspace_session_database.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "workspace/workspace.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <limits>
#include <set>
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

// Entry and turn codes are stored as integers from 0 through 3.
constexpr std::array<std::string_view, 4> entry_kind_names{
    "human", "character", "notice", "error"};
constexpr std::array<std::string_view, 4> entry_status_names{
    "complete", "streaming", "cancelled", "failed"};
constexpr std::array<std::string_view, 4> turn_state_names{
    "started", "completed", "cancelled", "failed"};

std::string code_name(
    std::int64_t code,
    const std::array<std::string_view, 4>& names) {
    return std::string(names.at(static_cast<std::size_t>(code)));
}

nlohmann::json session_overrides(Database& database, std::int64_t session_key) {
    Statement statement = database.prepare(
        "SELECT character_id, provider_id, reasoning_effort "
        "FROM session_character_settings WHERE session_key = ?1 "
        "ORDER BY character_id",
        session_key);
    auto result = nlohmann::json::array();
    while (statement.step()) {
        result.push_back({
            {"character_id", statement.text(0)},
            {"provider_id", statement.is_null(1)
                ? nlohmann::json(nullptr)
                : nlohmann::json(statement.text(1))},
            {"reasoning_effort", statement.is_null(2)
                ? nlohmann::json(nullptr)
                : nlohmann::json(statement.text(2))},
        });
    }
    return result;
}

nlohmann::json session_folders(Database& database, std::int64_t session_key) {
    Statement statement = database.prepare(
        "SELECT folder_id FROM session_folders WHERE session_key = ?1 "
        "ORDER BY folder_id",
        session_key);
    auto result = nlohmann::json::array();
    while (statement.step()) {
        const std::int64_t folder_id = statement.integer(0);
        result.push_back({
            {"folder_id", folder_id},
            {"ref_id", note_item_ref_id(folder_id)},
        });
    }
    return result;
}

nlohmann::json session_notes(Database& database, std::int64_t session_key) {
    Statement statement = database.prepare(
        "SELECT sn.note_id, sn.name, sn.explicitly_added, "
        "EXISTS (SELECT 1 FROM note_items AS n WHERE n.id = sn.note_id) "
        "FROM session_notes AS sn WHERE sn.session_key = ?1 "
        "ORDER BY sn.note_id",
        session_key);
    auto result = nlohmann::json::array();
    while (statement.step()) {
        const std::int64_t note_id = statement.integer(0);
        auto scopes = nlohmann::json::array();
        Statement scope = database.prepare(
            "SELECT character_id FROM session_note_scopes "
            "WHERE session_key = ?1 AND note_id = ?2 ORDER BY character_id",
            session_key,
            note_id);
        while (scope.step()) scopes.push_back(scope.text(0));
        result.push_back({
            {"note_id", note_id},
            {"ref_id", note_item_ref_id(note_id)},
            {"name", statement.text(1)},
            {"explicitly_added", statement.integer(2) != 0},
            {"source_exists", statement.integer(3) != 0},
            {"scopes", std::move(scopes)},
        });
    }
    return result;
}

nlohmann::json session_files(Database& database, std::int64_t session_key) {
    Statement statement = database.prepare(
        "SELECT name, length(CAST(text AS BLOB)) FROM session_files "
        "WHERE session_key = ?1 ORDER BY name",
        session_key);
    auto result = nlohmann::json::array();
    while (statement.step()) {
        result.push_back({
            {"name", statement.text(0)},
            {"bytes", statement.integer(1)},
        });
    }
    return result;
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

void SessionRepository::MaintenanceGuard::recover_sessions() const {
    repository_->recover_sessions();
}

void SessionRepository::recover_sessions() const {
    Database database(database_path_, Database::Mode::read_write, database_password_);
    validate_workspace_session_database_identity(database);
    database.execute("DELETE FROM sessions WHERE archived_at IS NOT NULL");
    std::vector<FullSessionId> candidates;
    {
        auto rows = database.prepare(
            "SELECT f.forum_id, s.session_id FROM sessions s JOIN forums f USING(forum_key) "
            "WHERE s.recent_pending = 1 AND s.archived_at IS NULL");
        while (rows.step()) candidates.push_back({rows.text(0), rows.text(1)});
    }
    for (const auto& identity : candidates) {
        try {
            const auto lease = acquire_session(identity, false, true);
            Transaction transaction(database);
            database.prepare(
                "DELETE FROM sessions WHERE recent_pending = 1 AND archived_at IS NULL "
                "AND forum_key = (SELECT forum_key FROM forums WHERE forum_id = ?1) "
                "AND session_id = ?2 AND NOT EXISTS "
                "(SELECT 1 FROM entries e WHERE e.session_key = sessions.session_key)",
                identity.forum_id, identity.session_id).run();
            database.prepare(
                "UPDATE sessions SET recent_pending = 0 WHERE recent_pending = 1 "
                "AND forum_key = (SELECT forum_key FROM forums WHERE forum_id = ?1) "
                "AND session_id = ?2 AND EXISTS "
                "(SELECT 1 FROM entries e WHERE e.session_key = sessions.session_key)",
                identity.forum_id, identity.session_id).run();
            transaction.commit();
        } catch (const SessionBusyError&) { /* Another owner is still working. */ }
    }
}

std::shared_ptr<SessionLease> SessionRepository::acquire_session(
    const FullSessionId& identity, bool opening, bool skip_local) const {
    if (identity == temporary_identity_) return {};
    const std::lock_guard lock(leases_mutex_);
    std::erase_if(leases_, [](const auto& item) { return item.second.expired(); });
    const auto found = leases_.find(identity);
    if (found != leases_.end()) {
        if (auto lease = found->second.lock()) {
            if (skip_local) throw SessionBusyError(session_in_use_message);
            if (opening) pending_.erase(identity);
            return lease;
        }
        leases_.erase(found);
    }
    auto lease = std::make_shared<SessionLease>(SessionLease::acquire_file(
        SessionLease::session_path(database_path_, identity.forum_id, identity.session_id),
        session_in_use_message));
    leases_[identity] = lease;
    return lease;
}

void SessionRepository::release_pending(const FullSessionId& identity) const {
    const std::lock_guard lock(leases_mutex_);
    pending_.erase(identity);
}

void SessionRepository::release_pending() {
    const std::lock_guard lock(leases_mutex_);
    pending_.clear();
}

void SessionRepository::MaintenanceGuard::retarget(
    std::filesystem::path database_path,
    std::string database_password) {
    const bool path_changed = repository_->database_path_ != database_path;
    repository_->release_pending();
    if (path_changed) {
        SessionJournal welcome(repository_->temporary_database_path_);
        welcome.clear_vault_context();
    }
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

std::shared_ptr<const Workspace> SessionRepository::workspace() const {
    auto result = read_workspace_();
    if (!result || result->root() != workspace_root_) {
        throw std::runtime_error("Session repository has no matching loaded workspace");
    }
    return result;
}

SessionRepository::SessionRepository(
    WorkspaceReader read_workspace,
    std::filesystem::path database_path,
    std::filesystem::path workspace_root,
    std::filesystem::path welcome_directory,
    TemporarySessionSeed temporary,
    std::string database_password)
    : read_workspace_(std::move(read_workspace)),
      workspace_root_(std::move(workspace_root)),
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
    recover_sessions();
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
        checkpoint_workspace_session_database(database_path_, database_password_, true);
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
    const std::shared_ptr<const Workspace> workspace = this->workspace();
    if (workspace->find_forum(forum_id) == nullptr) {
        throw ForumNotFoundError(
            "Forum '" + std::string(forum_id) + "' does not exist");
    }
}

void SessionRepository::synchronize_forums() const {
    const std::shared_lock operation(operation_mutex_);
    const std::shared_ptr<const Workspace> workspace = this->workspace();
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

nlohmann::json SessionRepository::inspect_sessions(std::string_view forum_id) const {
    const std::shared_lock operation(operation_mutex_);
    require_persistent_forum(forum_id);
    Database database(database_path_, Database::Mode::read_only, database_password_);
    validate_workspace_session_database_identity(database);
    Statement sessions = database.prepare(
        "SELECT s.session_key, s.session_id, s.label, s.updated_at, "
        "s.recent_pending, s.discardable, s.history_epoch, "
        "(SELECT COUNT(*) FROM entries AS e WHERE e.session_key = s.session_key), "
        "(SELECT COALESCE(SUM(e.input_tokens), 0) FROM entries AS e "
        "WHERE e.session_key = s.session_key), "
        "(SELECT COALESCE(SUM(e.output_tokens), 0) FROM entries AS e "
        "WHERE e.session_key = s.session_key), "
        "(SELECT COUNT(*) FROM entry_audio AS a WHERE a.session_key = s.session_key), "
        "(SELECT n.note_id FROM note_summaries AS n WHERE n.session_key = s.session_key) "
        "FROM sessions AS s JOIN forums AS f USING (forum_key) "
        "WHERE f.forum_id = ?1 AND s.archived_at IS NULL "
        "ORDER BY s.session_id",
        forum_id);
    auto result = nlohmann::json::array();
    while (sessions.step()) {
        const std::int64_t session_key = sessions.integer(0);
        result.push_back({
            {"session_id", sessions.text(1)},
            {"label", sessions.text(2)},
            {"updated_at", sessions.integer(3)},
            {"recent_pending", sessions.integer(4) != 0},
            {"discardable", sessions.integer(5) != 0},
            {"history_epoch", sessions.integer(6)},
            {"entry_count", sessions.integer(7)},
            {"input_tokens", sessions.integer(8)},
            {"output_tokens", sessions.integer(9)},
            {"cached_audio_count", sessions.integer(10)},
            {"summary_note_id", sessions.is_null(11)
                ? nlohmann::json(nullptr)
                : nlohmann::json(sessions.integer(11))},
            {"overrides", session_overrides(database, session_key)},
            {"folders", session_folders(database, session_key)},
            {"notes", session_notes(database, session_key)},
            {"files", session_files(database, session_key)},
        });
    }
    return result;
}

nlohmann::json SessionRepository::inspect_entries(const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    require_persistent_forum(identity.forum_id);
    Database database(database_path_, Database::Mode::read_only, database_password_);
    validate_workspace_session_database_identity(database);
    require_active(database, identity);
    Statement entries = database.prepare(
        "SELECT e.entry_id, e.epoch, e.request_id, t.state, e.kind, "
        "e.participant_id, e.display_name, e.addressed_to, e.addressed_to_name, "
        "e.status, e.created_at, e.input_tokens, e.output_tokens, "
        "e.web_search_used, length(CAST(e.text AS BLOB)), a.content_type "
        "FROM entries AS e "
        "JOIN sessions AS s USING (session_key) "
        "JOIN forums AS f USING (forum_key) "
        "LEFT JOIN turns AS t "
        "ON t.session_key = e.session_key AND t.request_id = e.request_id "
        "LEFT JOIN entry_audio AS a "
        "ON a.session_key = e.session_key AND a.entry_id = e.entry_id "
        "WHERE f.forum_id = ?1 AND s.session_id = ?2 AND s.archived_at IS NULL "
        "ORDER BY e.entry_id",
        std::string_view(identity.forum_id),
        std::string_view(identity.session_id));
    // Request IDs, token counts, the turn, and cached audio can be absent.
    const auto optional_integer = [&entries](int column) {
        return entries.is_null(column)
            ? nlohmann::json(nullptr)
            : nlohmann::json(entries.integer(column));
    };
    auto result = nlohmann::json::array();
    while (entries.step()) {
        result.push_back({
            {"entry_id", entries.integer(0)},
            {"epoch", entries.integer(1)},
            {"request_id", optional_integer(2)},
            {"state", entries.is_null(3)
                ? nlohmann::json(nullptr)
                : nlohmann::json(code_name(entries.integer(3), turn_state_names))},
            {"kind", code_name(entries.integer(4), entry_kind_names)},
            {"participant_id", entries.text(5)},
            {"display_name", entries.text(6)},
            {"addressed_to", entries.text(7)},
            {"addressed_to_name", entries.text(8)},
            {"status", code_name(entries.integer(9), entry_status_names)},
            {"created_at", entries.integer(10)},
            {"input_tokens", optional_integer(11)},
            {"output_tokens", optional_integer(12)},
            {"web_search_used", entries.integer(13) != 0},
            {"text_bytes", entries.integer(14)},
            {"content_type", entries.is_null(15)
                ? nlohmann::json(nullptr)
                : nlohmann::json(entries.text(15))},
        });
    }
    return result;
}

std::vector<StoredSession> SessionRepository::recent() const {
    const std::shared_lock operation(operation_mutex_);
    const std::shared_ptr<const Workspace> workspace = this->workspace();
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
        "WHERE s.archived_at IS NULL AND s.recent_pending = 0 "
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
    const std::string base_id = session_timestamp_name(std::time(nullptr)) + "-session";
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
        const FullSessionId identity{std::string(forum_id), id};
        std::shared_ptr<SessionLease> lease;
        try { lease = acquire_session(identity, false, true); }
        catch (const SessionBusyError&) { continue; }
        const std::int64_t updated_at = session_timestamp();
        const std::string effective_label = label.empty()
            ? std::string(temporary_session_label) : label;
        Statement insert = database.prepare(
            "INSERT INTO sessions (forum_key, session_id, label, "
            "updated_at, history_epoch, next_entry_id, next_request_id, recent_pending, discardable) "
            "VALUES (?1, ?2, ?3, ?4, 1, 1, 1, ?5, ?5)",
            forum_key,
            std::string_view(id),
            std::string_view(effective_label),
            updated_at,
            static_cast<std::int64_t>(label.empty()));
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
        {
            const std::lock_guard lock(leases_mutex_);
            pending_[identity] = std::move(lease);
        }
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
    const auto lease = acquire_session(identity);
    require_persistent_forum(identity.forum_id);
    validate_session_label(label);
    Database database(
        database_path_, Database::Mode::read_write, database_password_);
    validate_workspace_session_database_identity(database);
    Transaction transaction(database);
    const std::int64_t updated_at = session_timestamp();
    Statement update = database.prepare(
        "UPDATE sessions SET label = ?1, updated_at = ?2, recent_pending = 0, discardable = 0 "
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

bool SessionRepository::discard_unused(const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    const auto lease = acquire_session(identity);
    require_persistent_forum(identity.forum_id);
    Database database(database_path_, Database::Mode::read_write, database_password_);
    validate_workspace_session_database_identity(database);
    auto remove = database.prepare(
        "DELETE FROM sessions WHERE discardable = 1 AND archived_at IS NULL "
        "AND session_key = (SELECT s.session_key FROM sessions s JOIN forums f USING (forum_key) "
        "WHERE f.forum_id = ?1 AND s.session_id = ?2) "
        "AND NOT EXISTS (SELECT 1 FROM entries e WHERE e.session_key = sessions.session_key)",
        std::string_view(identity.forum_id), std::string_view(identity.session_id));
    remove.run();
    const bool discarded = database.changes() == 1;
    release_pending(identity);
    return discarded;
}

void SessionRepository::delete_session(const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    const auto lease = acquire_session(identity);
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
    release_pending(identity);
}

PreparedSession SessionRepository::prepare(
    const FullSessionId& identity) const {
    const std::shared_lock operation(operation_mutex_);
    auto lease = acquire_session(identity, true);
    const auto& path = session_database_path(identity);
    const bool temporary = identity == temporary_identity_;
    LoadedSessionDatabase loaded =
        load_session_database(
            path, identity, temporary ? std::string_view{} : database_password_);
    return {
        .lease = std::move(lease),
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
        "SELECT e.session_key, e.text, e.kind, e.participant_id FROM entries e "
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
        .participant_id = entry.text(3),
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
    const auto lease = acquire_session(entry.identity);
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
    const auto lease = acquire_session(identity);
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
