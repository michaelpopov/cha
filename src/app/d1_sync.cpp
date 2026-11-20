#include "app/d1_sync.h"

#include "characters/character_config.h"
#include "runtime/live_session_manager.h"
#include "storage/note_storage.h"
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

std::string_view request_reason(const D1Client::Error& error) {
    if (error.failure == D1Client::Failure::full_database) return "database is full";
    return "D1 request failed";
}

std::string_view transient_storage_reason(int code) {
    switch (code & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED: return "local storage is busy";
    case SQLITE_IOERR: return "local storage I/O failed";
    case SQLITE_FULL: return "local storage is full";
    case SQLITE_CANTOPEN: return "local storage could not be opened";
    default: return {};
    }
}

bool valid_op_id(std::string_view id) {
    return id.size() == 32 && std::ranges::all_of(id, [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

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
    if (using_v2()) {
        report_v2(EventSeverity::warning,
            "D1 sync stopped. Download the vault from R2 or save the D1 credentials again.");
        return;
    }
    log_error(
        "D1 sync stopped: download the vault from R2, or save the D1 credentials again.");
}

bool D1SyncWorker::using_v2() const noexcept {
    return config_.use_v2;
}

void D1SyncWorker::report_v2(EventSeverity severity, const std::string& message) {
    if (severity == EventSeverity::info) log_info(message);
    else log_warn(message);
    if (config_.store) {
        try {
            config_.store->append_event(severity, message);
        } catch (...) {
            log_warn("D1 Event recording failed.");
        }
    }
}

void D1SyncWorker::pause_v2(bool sending, std::string_view stage, std::string_view reason,
    std::string_view op_id) {
    auto& previous = sending ? send_condition_ : receive_condition_;
    std::string condition = std::string(stage) + " (" + std::string(reason) + ")";
    if (previous == condition) return;
    previous = std::move(condition);
    std::string message = "D1 " + std::string(stage) + " paused";
    if (valid_op_id(op_id)) message += " for operation " + std::string(op_id);
    report_v2(EventSeverity::warning, message + " (" + std::string(reason) + ").");
}

void D1SyncWorker::resume_v2(bool sending) {
    auto& previous = sending ? send_condition_ : receive_condition_;
    if (previous.empty()) return;
    previous.clear();
    report_v2(EventSeverity::info, sending ? "D1 send resumed." : "D1 receive resumed.");
}

bool D1SyncWorker::prepare_v2(std::stop_token stop) {
    if (!using_v2()) return true;
    try {
        std::optional<SyncD1Credentials> creds;
        {
            Database database(
                config_.database_path, Database::Mode::read_only, config_.database_password);
            if (sync_has_receipt_schema(database)) return true;
            creds = sync_read_d1(database);
        }
        if (!creds) return false;
        D1Client client(
            {creds->account_id, creds->database_id, creds->api_token}, config_.transport);
        const auto [tables, error] = client.list_tables();
        if (error) {
            pause_v2(true, "discovery", request_reason(*error));
            return false;
        }
        for (const auto& table : tables) {
            if (interrupted(stop)) return false;
            const auto legacy = sync_parse_queue_table(table);
            if (!legacy || !sync_same_vault(legacy->vault, config_.vault)) continue;
            if (std::ranges::any_of(tables, [&](const auto& candidate) {
                    const auto v2 = sync_parse_queue_table(candidate, true);
                    return v2 && sync_same_vault(v2->vault, legacy->vault)
                        && sync_same_machine(v2->machine, legacy->machine);
                })) continue;
            const auto v2_table = sync_queue_table(legacy->vault, legacy->machine, true);
            if (const auto failure = client.create_queue_table(v2_table, true)) {
                pause_v2(true, "provisioning", request_reason(*failure));
                return false;
            }
        }
        Database database(
            config_.database_path, Database::Mode::read_write, config_.database_password);
        storage::SqliteTransaction transaction(database);
        ensure_sync_schema(database, true);
        transaction.commit();
        return true;
    } catch (const std::exception&) {
        pause_v2(true, "provisioning", "local storage upgrade failed");
        return false;
    }
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
    if (!prepare_v2(stop)) return;
    collect_flags();
    if (send_pass(stop)) receive_pass(stop, notify_notes);
}

void D1SyncWorker::flush_send() {
    if (stopped() || config_.machine.empty()) return;
    if (!prepare_v2()) return;
    collect_flags();
    (void)send_pass();
}

void D1SyncWorker::ensure_local_queue() {
    const auto creds = read_credentials(config_.database_path, config_.database_password);
    if (!creds) return;
    D1Client client(
        {creds->account_id, creds->database_id, creds->api_token}, config_.transport);
    const bool v2 = using_v2();
    const auto [queues, discovery_error] = client.list_vault_queues(config_.vault, config_.machine, v2);
    if (discovery_error) {
        if (v2) pause_v2(true, "discovery", request_reason(*discovery_error));
        else log_warn("D1 sync discovery failed: " + discovery_error->message);
    } else if (!queues.local) {
        const auto table = sync_queue_table(config_.vault, config_.machine, v2);
        if (const auto error = client.create_queue_table(table, v2)) {
            if (v2) pause_v2(true, "provisioning", request_reason(*error));
            else log_warn("D1 sync table creation failed: " + error->message);
        }
    }
}

bool D1SyncWorker::send_pass(std::stop_token stop) {
    if (stopped() || interrupted(stop)) return false;
    const bool v2 = using_v2();
    const auto creds = read_credentials(config_.database_path, config_.database_password);
    if (!creds) return true; // Disconnected vaults never send.
    std::vector<QueuedRecord> local;
    try {
        Database database(
            config_.database_path, Database::Mode::read_only, config_.database_password);
        local = sync_queue_list(database);
    } catch (const std::exception& error) {
        if (v2) pause_v2(true, "send", "local outbox could not be read");
        else log_warn(std::string("D1 sync queue read failed: ") + error.what());
        return true;
    }
    if (local.empty()) {
        if (v2) resume_v2(true);
        return true;
    }
    D1Client client(
        {creds->account_id, creds->database_id, creds->api_token}, config_.transport);
    auto [queues, list_error] = client.list_vault_queues(config_.vault, config_.machine, v2);
    if (list_error) {
        if (v2) pause_v2(true, "discovery", request_reason(*list_error));
        else log_warn("D1 sync send discovery failed: " + list_error->message);
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
            for (const auto& row : local) sync_queue_delete(database, row.rowid, row.op_id);
            transaction.commit();
        } catch (const std::exception& error) {
            if (v2) pause_v2(true, "send", "local outbox could not be updated");
            else log_warn(std::string("D1 sync local cleanup failed: ") + error.what());
            return true;
        }
        if (v2) resume_v2(true);
        return true;
    }
    for (const auto& row : local) {
        if (interrupted(stop)) return false;
        if (v2 && !valid_op_id(row.op_id)) {
            pause_v2(true, "send", "local outbox has an invalid operation ID");
            return true;
        }
        const auto pieces = sync_split_record(row.record);
        bool ok = true;
        for (std::size_t i = 0; i < pieces.size(); ++i) {
            const auto error = client.insert_piece(
                queues.recipients, static_cast<std::int64_t>(i + 1),
                static_cast<std::int64_t>(pieces.size()), pieces[i],
                v2 ? std::string_view(row.op_id) : std::string_view{});
            if (error) {
                if (v2) pause_v2(true, "send", request_reason(*error), row.op_id);
                else log_warn("D1 sync send failed: " + error->message);
                if (!config_.desktop_host && error->failure == D1Client::Failure::full_database)
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
            sync_queue_delete(database, row.rowid, row.op_id);
            transaction.commit();
        } catch (const std::exception& error) {
            if (v2) pause_v2(true, "send", "local outbox could not be updated", row.op_id);
            else log_warn(std::string("D1 sync queue delete failed: ") + error.what());
            return true;
        }
    }
    if (v2) resume_v2(true);
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

enum class ApplyOutcome { applied, skipped, retry };
struct ApplyResult {
    ApplyOutcome outcome;
    std::string_view reason = {};
};

// Changed chats and their current labels.
using ChangedChats = std::map<FullSessionId, std::string>;

// The ordinary validation of SessionController::set_character_settings.
bool valid_character_record(
    const Workspace& workspace, std::string_view forum_id, const nlohmann::json& record) {
    if (!workspace.find_forum_member(forum_id, record.at("character_id").get<std::string>()))
        return false;
    const auto text = [&](const char* name) {
        const auto found = record.find(name);
        return found == record.end() || found->is_null()
            ? std::string{} : found->get<std::string>();
    };
    const std::string provider = text("provider_id");
    const std::string effort = text("reasoning_effort");
    return (provider.empty() || workspace.find_provider(provider))
        && (effort.empty() || valid_reasoning_effort(effort));
}

ApplyResult apply_complete_record(
    const std::string& record_text,
    const D1SyncWorker::Config& config, bool& notes_changed, ChangedChats& chats_changed,
    std::string_view op_id = {}) {
    const auto skip = [&](std::string_view reason, std::string_view legacy_warning) -> ApplyResult {
        if (op_id.empty()) log_warn(std::string(legacy_warning));
        return {ApplyOutcome::skipped, reason};
    };
    nlohmann::json envelope;
    try {
        envelope = nlohmann::json::parse(record_text);
    } catch (...) {
        return skip("invalid record", "Skipped incoming sync record with invalid JSON.");
    }
    std::string kind;
    try {
        kind = envelope.at("kind").get<std::string>();
    } catch (...) {
        return skip("invalid record", "Skipped incoming sync record with no kind.");
    }
    try {
        if (kind == "chat.add_folder" || kind == "chat.detach_folder") {
            if (!op_id.empty()) {
                Database database(config.database_path, Database::Mode::read_write, config.database_password);
                storage::SqliteTransaction transaction(database);
                if (!sync_seen(database, op_id)) sync_record_receipt(database, op_id);
                transaction.commit();
            }
            log_warn("Ignoring obsolete D1 chat record: " + kind);
            return {ApplyOutcome::applied};
        }
        if (kind.rfind("note.", 0) == 0) {
            Database database(config.database_path, Database::Mode::read_write, config.database_password);
            if (note_apply_record(database, envelope, op_id)) notes_changed = true;
            return {ApplyOutcome::applied};
        }
        if (kind == "config.edit") {
            if (!config.store) {
                return skip("no vault is open", "Skipped incoming configuration change: no vault is open.");
            }
            std::vector<ConfigFile> set_rows;
            std::vector<std::string> delete_names;
            const auto& set = envelope.at("set");
            const auto& remove = envelope.at("delete");
            if (!set.is_array() || !remove.is_array()) {
                return skip("invalid record", "Skipped incoming configuration change with invalid row lists.");
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
            }, op_id);
            if (outcome.retry) return {ApplyOutcome::retry, "configuration is busy"};
            if (!outcome.applied) return {ApplyOutcome::skipped, "configuration change rejected"};
            if (outcome.notes_changed) notes_changed = true;
            return {ApplyOutcome::applied};
        }
        if (kind.rfind("chat.", 0) == 0) {
            const auto chat = sync_parse_chat_identity(envelope);
            const FullSessionId identity{chat.forum_id, chat.session_id};
            Database database(config.database_path, Database::Mode::read_write, config.database_password);
            if (!sync_chat_forum_exists(database, identity.forum_id)) {
                return skip("chat forum does not exist", "Skipped incoming chat change: chat forum does not exist.");
            }
            if (kind == "chat.character" && (!config.store
                || !valid_character_record(*config.store->snapshot(), identity.forum_id, envelope))) {
                return skip("invalid record", "Skipped incoming chat character settings that are not valid.");
            }
            std::optional<LiveSessionMaintenanceReservation> reservation;
            if (config.live_sessions) {
                auto reserved = config.live_sessions->reserve_for_sync(
                    identity, std::chrono::seconds(5));
                if (std::holds_alternative<MaintenanceFailure>(reserved)) {
                    return {ApplyOutcome::retry, "chat is busy"};
                }
                reservation.emplace(
                    std::move(std::get<LiveSessionMaintenanceReservation>(reserved)));
            }
            if (sync_apply_chat_record(database, envelope, op_id)) {
                if (kind == "chat.delete") chats_changed.erase(identity);
                else chats_changed[identity] = chat.label;
            }
            return {ApplyOutcome::applied};
        }
        return skip("invalid record", "Skipped incoming sync record with unknown kind.");
    } catch (const NoteError& error) {
        if (!op_id.empty()) {
            if (const auto reason = transient_storage_reason(error.sqlite_code); !reason.empty())
                return {ApplyOutcome::retry, reason};
        }
        return skip("note change rejected", std::string("Skipped incoming sync record: ") + error.what());
    } catch (const storage::SqliteError& error) {
        if (!op_id.empty()) {
            if (const auto reason = transient_storage_reason(error.code); !reason.empty())
                return {ApplyOutcome::retry, reason};
        }
        return skip("storage rejected the change", std::string("Skipped incoming sync record: ") + error.what());
    } catch (const std::exception& error) {
        return skip("invalid record", std::string("Skipped incoming sync record: ") + error.what());
    }
}

void notify_changes(const D1SyncWorker::Config& config, bool notify_notes, bool notes_changed,
    const ChangedChats& chats_changed, bool v2 = false) {
    if (notify_notes && notes_changed && config.on_notes_changed) {
        try {
            config.on_notes_changed();
        } catch (...) {
            log_warn("Could not deliver the D1 notes change notification.");
        }
    }
    if (config.on_chat_changed) {
        for (const auto& [identity, label] : chats_changed) {
            try {
                config.on_chat_changed(identity, label);
            } catch (const std::exception& error) {
                if (v2) log_warn("Could not update the session mirror after D1 sync.");
                else log_warn(std::string("Could not update the session mirror: ") + error.what());
            }
        }
    }
}

} // namespace

void D1SyncWorker::receive_pass(std::stop_token stop, bool notify_notes) {
    if (stopped() || interrupted(stop)) return;
    const auto creds = read_credentials(config_.database_path, config_.database_password);
    if (!creds) return;
    D1Client client(
        {creds->account_id, creds->database_id, creds->api_token}, config_.transport);
    if (receive_legacy(client, stop, notify_notes) && using_v2())
        receive_v2(client, stop, notify_notes);
}

bool D1SyncWorker::receive_legacy(D1Client& client, std::stop_token stop, bool notify_notes) {
    const bool v2 = using_v2();
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
            if (read_error->failure == D1Client::Failure::missing_table) {
                if (v2) return true;
                halt_with_missing_table();
            } else if (v2)
                pause_v2(false, "receive", request_reason(*read_error));
            else
                log_warn("D1 sync receive failed: " + read_error->message);
            return false;
        }
        if (rows.empty()) return true;
        bool notes_changed = false;
        ChangedChats chats_changed;
        std::string_view retry_reason;
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
                if (outcome.outcome == ApplyOutcome::retry) {
                    retry_reason = outcome.reason;
                    break;
                }
                pending.reset();
            } else {
                log_warn("Skipped an invalid sync record piece.");
            }
            handled = row.rowid;
        }
        notify_changes(config_, notify_notes, notes_changed, chats_changed);
        if (handled > acknowledged) {
            if (const auto error = client.delete_through(local_table, handled)) {
                if (error->failure == D1Client::Failure::missing_table) {
                    if (v2) return true;
                    halt_with_missing_table();
                } else if (v2)
                    pause_v2(false, "acknowledgement", request_reason(*error));
                else
                    log_warn("D1 sync acknowledgement failed: " + error->message);
                return false;
            }
            acknowledged = handled;
        }
        if (!retry_reason.empty()) {
            if (v2) pause_v2(false, "receive", retry_reason);
            return false;
        }
        if (static_cast<int>(rows.size()) < limit) return true;
    }
    return false;
}

void D1SyncWorker::receive_v2(D1Client& client, std::stop_token stop, bool notify_notes) {
    const auto table = sync_queue_table(config_.vault, config_.machine, true);
    int limit = initial_read_limit;
    const auto request_failed = [&](const D1Client::Error& error, std::string_view stage,
        std::string_view op_id = {}) {
        if (error.failure == D1Client::Failure::missing_table) halt_with_missing_table();
        else pause_v2(false, stage, request_reason(error), op_id);
    };
    while (!interrupted(stop)) {
        auto [operations, read_error] = client.read_ready(table, limit);
        if (read_error) {
            if (read_error->failure == D1Client::Failure::result_too_large && limit > 1) {
                limit = std::max(1, limit / 2);
                continue;
            }
            request_failed(*read_error, "receive");
            return;
        }
        bool notes_changed = false;
        ChangedChats chats_changed;
        std::vector<std::string> handled;
        std::string_view retry_reason;
        std::string_view retry_id;
        // One handle per page: each open of a protected vault derives its key.
        std::optional<Database> receipts;
        for (auto& operation : operations) {
            if (interrupted(stop)) break;
            const auto& id = operation.op_id;
            ApplyResult result{ApplyOutcome::skipped, "invalid record"};
            if (valid_op_id(id)) {
                bool receipts_read = false;
                try {
                    if (!receipts) {
                        receipts.emplace(config_.database_path, Database::Mode::read_only,
                            config_.database_password);
                    }
                    if (sync_seen(*receipts, id)) {
                        handled.push_back(id);
                        continue;
                    }
                    receipts_read = true;
                } catch (const std::exception& error) {
                    if (const auto* sqlite = dynamic_cast<const storage::SqliteError*>(&error)) {
                        retry_reason = transient_storage_reason(sqlite->code);
                        if (!retry_reason.empty()) {
                            retry_id = id;
                            break;
                        }
                    }
                    result.reason = "local receipts could not be read";
                }
                if (receipts_read && operation.pieces > 1) {
                    operation.record.emplace();
                    for (std::int64_t piece = 1; !interrupted(stop); ++piece) {
                        auto [part, error] = client.read_piece(table, operation, piece);
                        if (error) {
                            if (error->failure == D1Client::Failure::missing_table)
                                halt_with_missing_table();
                            retry_reason = request_reason(*error);
                            retry_id = id;
                            break;
                        }
                        if (!part) {
                            operation.record.reset();
                            result.reason = "missing or inconsistent pieces";
                            break;
                        }
                        *operation.record += *part;
                        if (piece == operation.pieces) break;
                    }
                    if (!retry_reason.empty() || interrupted(stop)) break;
                }
                if (receipts_read && operation.pieces > 0 && operation.record)
                    result = apply_complete_record(*operation.record, config_, notes_changed, chats_changed, id);
            }
            if (result.outcome == ApplyOutcome::retry) {
                retry_reason = result.reason;
                retry_id = id;
                break;
            }
            if (result.outcome == ApplyOutcome::skipped) {
                std::string message = "D1 skipped operation";
                if (valid_op_id(id)) message += " " + id;
                report_v2(EventSeverity::warning, message + " (" + std::string(result.reason) + ").");
            }
            handled.push_back(id);
        }
        notify_changes(config_, notify_notes, notes_changed, chats_changed, true);
        if (!stopped()) {
            if (const auto error = client.delete_operations(table, handled)) {
                request_failed(*error, "acknowledgement",
                    handled.size() == 1 ? std::string_view(handled.front()) : std::string_view{});
                return;
            }
        }
        if (stopped() || interrupted(stop)) return;
        if (!retry_reason.empty()) {
            pause_v2(false, "receive", retry_reason, retry_id);
            return;
        }
        if (static_cast<int>(operations.size()) < limit) {
            resume_v2(false);
            return;
        }
    }
}

} // namespace cha
