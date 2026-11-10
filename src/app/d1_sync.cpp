#include "app/d1_sync.h"

#include "runtime/live_session_manager.h"
#include "storage/note_storage.h"
#include "storage/session_repository.h"
#include "storage/sync_chat.h"
#include "storage/sqlite_storage.h"
#include "util/logging.h"
#include "workspace/workspace_config_store.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

constexpr int initial_read_limit = 50;

std::optional<SyncD1Credentials> read_credentials(
    const std::filesystem::path& path,
    const std::string& password) {
    try {
        Database database(path, Database::Mode::read_only, password);
        return sync_read_d1(database);
    } catch (const std::exception& error) {
        log_warn(std::string("D1 sync credential read failed: ") + error.what());
        return std::nullopt;
    }
}

} // namespace

D1SyncWorker::D1SyncWorker(Config config) : config_(std::move(config)) {}

D1SyncWorker::~D1SyncWorker() {
    stop();
}

void D1SyncWorker::start(bool immediately) {
    if (thread_.joinable() || stopped()) return;
    thread_ = std::jthread([this, immediately](std::stop_token stop) {
        std::condition_variable_any delay;
        std::mutex mutex;
        std::unique_lock lock(mutex);
        if (!immediately)
            delay.wait_for(lock, stop, std::chrono::seconds(30), [] { return false; });
        while (!interrupted(stop) && !stopped()) {
            try {
                run_loop(stop, true);
            } catch (const std::exception& error) {
                log_warn(std::string("D1 sync loop failed: ") + error.what());
            }
            if (stopped()) break;
            delay.wait_for(lock, stop, std::chrono::seconds(30), [] { return false; });
        }
    });
}

void D1SyncWorker::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

bool D1SyncWorker::interrupted(std::stop_token stop) const {
    return stop.stop_possible() && (stop.stop_requested()
        || (config_.application_stopping && config_.application_stopping()));
}

void D1SyncWorker::halt_with_missing_table() {
    stopped_.store(true);
    log_error(
        "D1 sync stopped: download the vault from R2, or save the D1 credentials again.");
}

void D1SyncWorker::collect_flags() {
    try {
        Database database(
            config_.database_path, Database::Mode::read_write, config_.database_password);
        sync_collect_flagged_notes(database, config_.machine);
    } catch (const std::exception& error) {
        log_warn(std::string("D1 sync flag collection failed: ") + error.what());
    }
}

void D1SyncWorker::run_once(bool notify_notes) {
    std::stop_source stop;
    run_loop(stop.get_token(), notify_notes);
}

void D1SyncWorker::run_loop(std::stop_token stop, bool notify_notes) {
    if (stopped() || config_.machine.empty() || interrupted(stop)) return;
    collect_flags();
    if (send_pass(stop)) receive_pass(stop, notify_notes);
}

void D1SyncWorker::flush_send() {
    if (stopped() || config_.machine.empty()) return;
    collect_flags();
    (void)send_pass();
}

void D1SyncWorker::ensure_local_queue() {
    const auto creds = read_credentials(config_.database_path, config_.database_password);
    if (!creds) return;
    D1Client client(
        {creds->account_id, creds->database_id, creds->api_token}, config_.transport);
    const auto [queues, discovery_error] = client.list_vault_queues(config_.vault, config_.machine);
    if (discovery_error) {
        log_warn("D1 sync discovery failed: " + discovery_error->message);
    } else if (!queues.local) {
        const auto table = sync_queue_table(config_.vault, config_.machine);
        if (const auto error = client.create_queue_table(table))
            log_warn("D1 sync table creation failed: " + error->message);
    }
}

bool D1SyncWorker::send_pass(std::stop_token stop) {
    if (stopped() || interrupted(stop)) return false;
    const auto creds = read_credentials(config_.database_path, config_.database_password);
    if (!creds) return true; // Disconnected vaults never send.
    std::vector<QueuedRecord> local;
    try {
        Database database(
            config_.database_path, Database::Mode::read_only, config_.database_password);
        local = sync_queue_list(database);
    } catch (const std::exception& error) {
        log_warn(std::string("D1 sync queue read failed: ") + error.what());
        return true;
    }
    if (local.empty()) return true;
    D1Client client(
        {creds->account_id, creds->database_id, creds->api_token}, config_.transport);
    auto [queues, list_error] = client.list_vault_queues(config_.vault, config_.machine);
    if (list_error) {
        log_warn("D1 sync send discovery failed: " + list_error->message);
        return true;
    }
    if (!queues.local) {
        halt_with_missing_table();
        return false;
    }
    if (queues.recipients.empty()) {
        try {
            Database database(
                config_.database_path, Database::Mode::read_write, config_.database_password);
            storage::SqliteTransaction transaction(database);
            for (const auto& row : local) sync_queue_delete(database, row.rowid);
            transaction.commit();
        } catch (const std::exception& error) {
            log_warn(std::string("D1 sync local cleanup failed: ") + error.what());
        }
        return true;
    }
    for (const auto& row : local) {
        if (interrupted(stop)) return false;
        const auto pieces = sync_split_record(row.record);
        bool ok = true;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const auto error = client.insert_piece(
                queues.recipients, static_cast<std::int64_t>(i + 1),
                static_cast<std::int64_t>(pieces.size()), pieces[i]);
            if (error) {
                log_warn("D1 sync send failed: " + error->message);
                if (error->failure == D1Client::Failure::full_database)
                    handle_full_database(client);
                ok = false;
                break;
            }
        }
        if (!ok) return true; // Stop sending this loop; retry next loop.
        try {
            Database database(
                config_.database_path, Database::Mode::read_write, config_.database_password);
            storage::SqliteTransaction transaction(database);
            sync_queue_delete(database, row.rowid);
            transaction.commit();
        } catch (const std::exception& error) {
            log_warn(std::string("D1 sync queue delete failed: ") + error.what());
            return true;
        }
    }
    return true;
}

void D1SyncWorker::handle_full_database(D1Client& client) {
    auto [tables, list_error] = client.list_tables();
    if (list_error) {
        log_warn("D1 sync eviction discovery failed: " + list_error->message);
        return;
    }
    std::erase_if(tables, [this](const auto& table) {
        const auto parsed = sync_parse_queue_table(table);
        return !parsed || sync_same_machine(parsed->machine, config_.machine);
    });
    if (tables.empty()) return;
    auto [counts, count_error] = client.count_rows(tables);
    if (count_error) {
        log_warn("D1 sync eviction count failed: " + count_error->message);
        return;
    }
    const auto largest = std::distance(counts.begin(), std::ranges::max_element(counts));
    if (const auto drop_error = client.drop_table(tables[static_cast<std::size_t>(largest)])) {
        log_warn("D1 sync eviction drop failed: " + drop_error->message);
        return;
    }
    log_warn("D1 sync dropped the largest queue table to free space.");
}

namespace {

struct PendingSet {
    std::string text;
    std::int64_t pieces{};
    std::int64_t next_piece{2};
    std::int64_t last_rowid{};
};

enum class ApplyOutcome { applied, skipped, reservation_failed };

// Changed chats and their current labels.
using ChangedChats = std::map<FullSessionId, std::string>;

ApplyOutcome apply_complete_record(
    const std::string& record_text,
    const D1SyncWorker::Config& config, bool& notes_changed, ChangedChats& chats_changed) {
    nlohmann::json envelope;
    try {
        envelope = nlohmann::json::parse(record_text);
    } catch (...) {
        log_warn("Skipped incoming sync record with invalid JSON.");
        return ApplyOutcome::skipped;
    }
    std::string kind;
    try {
        kind = envelope.at("kind").get<std::string>();
    } catch (...) {
        log_warn("Skipped incoming sync record with no kind.");
        return ApplyOutcome::skipped;
    }
    try {
        if (kind.rfind("note.", 0) == 0) {
            Database database(config.database_path, Database::Mode::read_write, config.database_password);
            if (note_apply_record(database, envelope)) notes_changed = true;
            return ApplyOutcome::applied;
        }
        if (kind == "config.edit") {
            if (!config.store) {
                log_warn("Skipped incoming configuration change: no vault is open.");
                return ApplyOutcome::skipped;
            }
            std::vector<ConfigFile> set_rows;
            std::vector<std::string> delete_names;
            const auto& set = envelope.at("set");
            const auto& remove = envelope.at("delete");
            if (!set.is_array() || !remove.is_array()) {
                log_warn("Skipped incoming configuration change with invalid row lists.");
                return ApplyOutcome::skipped;
            }
            for (const auto& row : set)
                set_rows.push_back({row.at("name").get<std::string>(), row.at("content").get<std::string>()});
            for (const auto& name : remove) delete_names.push_back(name.get<std::string>());
            // Validate first. Hold the reservation through commit so live
            // chats cannot write while forums are pruned.
            std::optional<LiveSessionGlobalMaintenance> global;
            const auto outcome = config.store->apply_sync_config_edit(set_rows, delete_names, [&] {
                if (!config.live_sessions) return true;
                auto reserved = config.live_sessions->reserve_global_maintenance(std::chrono::seconds(5));
                if (std::holds_alternative<MaintenanceFailure>(reserved)) return false;
                global.emplace(std::move(std::get<LiveSessionGlobalMaintenance>(reserved)));
                return true;
            });
            if (outcome.retry) return ApplyOutcome::reservation_failed;
            if (!outcome.applied) return ApplyOutcome::skipped;
            if (outcome.notes_changed) notes_changed = true;
            return ApplyOutcome::applied;
        }
        if (kind.rfind("chat.", 0) == 0) {
            const auto chat = sync_parse_chat_identity(envelope);
            const FullSessionId identity{chat.forum_id, chat.session_id};
            Database database(config.database_path, Database::Mode::read_write, config.database_password);
            if (!sync_chat_forum_exists(database, identity.forum_id)) {
                log_warn("Skipped incoming chat change: chat forum does not exist.");
                return ApplyOutcome::skipped;
            }
            std::optional<LiveSessionMaintenanceReservation> reservation;
            if (config.live_sessions) {
                auto reserved = config.live_sessions->reserve_for_sync(
                    identity, std::chrono::seconds(5));
                if (std::holds_alternative<MaintenanceFailure>(reserved)) {
                    return ApplyOutcome::reservation_failed;
                }
                reservation.emplace(
                    std::move(std::get<LiveSessionMaintenanceReservation>(reserved)));
            }
            // The runtime reservation covers this process. The existing
            // session lease also excludes a controller in another process.
            const auto lease = config.sessions
                ? config.sessions->reserve_for_sync(identity) : nullptr;
            sync_apply_chat_record(database, envelope);
            if (kind == "chat.delete") chats_changed.erase(identity);
            else chats_changed[identity] = chat.label;
            return ApplyOutcome::applied;
        }
        log_warn("Skipped incoming sync record with unknown kind.");
        return ApplyOutcome::skipped;
    } catch (const SessionBusyError&) {
        return ApplyOutcome::reservation_failed;
    } catch (const std::exception& error) {
        log_warn(std::string("Skipped incoming sync record: ") + error.what());
        return ApplyOutcome::skipped;
    }
}

} // namespace

void D1SyncWorker::receive_pass(std::stop_token stop, bool notify_notes) {
    if (stopped() || interrupted(stop)) return;
    const auto creds = read_credentials(config_.database_path, config_.database_password);
    if (!creds) return;
    D1Client client(
        {creds->account_id, creds->database_id, creds->api_token}, config_.transport);
    // No table list here: a read of a missing table gives the missing-table error.
    const std::string local_table = sync_queue_table(config_.vault, config_.machine);
    int limit = initial_read_limit;
    std::int64_t read_through = 0;
    std::int64_t acknowledged = 0;
    std::int64_t handled = 0;
    std::optional<PendingSet> pending;
    while (!interrupted(stop)) {
        auto [rows, read_error] = client.read_queue(local_table, limit, read_through);
        if (read_error) {
            if (read_error->failure == D1Client::Failure::result_too_large && limit > 1) {
                limit = std::max(1, limit / 2);
                continue;
            }
            if (read_error->failure == D1Client::Failure::missing_table)
                halt_with_missing_table();
            else
                log_warn("D1 sync receive failed: " + read_error->message);
            break;
        }
        if (rows.empty()) break;
        bool notes_changed = false;
        ChangedChats chats_changed;
        bool paused = false;
        for (const auto& row : rows) {
            if (interrupted(stop)) break;
            read_through = row.rowid;
            const std::string* complete = &row.record;
            if (pending) {
                if (row.piece == pending->next_piece && row.pieces == pending->pieces) {
                    pending->text += row.record;
                    pending->last_rowid = row.rowid;
                    ++pending->next_piece;
                    if (row.piece < row.pieces) continue;
                    complete = &pending->text;
                } else {
                    log_warn("Skipped an incomplete sync record set.");
                    handled = pending->last_rowid;
                    pending.reset();
                }
            }
            if (!pending && row.piece == 1 && row.pieces > 1) {
                pending = PendingSet{row.record, row.pieces, 2, row.rowid};
                continue;
            }
            if (pending || (row.piece == 1 && row.pieces == 1)) {
                const auto outcome = apply_complete_record(
                    *complete, config_, notes_changed, chats_changed);
                if (outcome == ApplyOutcome::reservation_failed) {
                    paused = true;
                    break;
                }
                pending.reset();
            } else {
                log_warn("Skipped an invalid sync record piece.");
            }
            handled = row.rowid;
        }
        if (notify_notes && notes_changed && config_.on_notes_changed) {
            try {
                config_.on_notes_changed();
            } catch (...) {
                log_warn("Could not deliver the D1 notes change notification.");
            }
        }
        if (config_.on_chat_changed) {
            for (const auto& [identity, label] : chats_changed) {
                try {
                    config_.on_chat_changed(identity, label);
                } catch (const std::exception& error) {
                    log_warn(std::string("Could not update the session mirror: ") + error.what());
                }
            }
        }
        if (handled > acknowledged) {
            if (const auto error = client.delete_through(local_table, handled)) {
                if (error->failure == D1Client::Failure::missing_table)
                    halt_with_missing_table();
                else
                    log_warn("D1 sync acknowledgement failed: " + error->message);
                break;
            }
            acknowledged = handled;
        }
        if (paused || static_cast<int>(rows.size()) < limit) break;
    }
}

} // namespace cha
