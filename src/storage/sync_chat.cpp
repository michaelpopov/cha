#include "storage/sync_chat.h"

#include "storage/session_label.h"
#include "storage/session_timestamp.h"
#include "storage/note_storage.h"
#include "storage/sync_storage.h"
#include "util/logging.h"

#include <algorithm>
#include <stdexcept>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

nlohmann::json chat_ref(const SyncChatIdentity& identity) {
    return {identity.forum_id, identity.session_id};
}

void append_record(
    Database& database,
    const SyncChatIdentity& identity,
    std::string_view kind,
    nlohmann::json fields) {
    fields["chat"] = chat_ref(identity);
    fields["label"] = identity.label;
    sync_queue_append(database, sync_make_record(kind, fields));
}

TranscriptEntry parse_entry_strict(const nlohmann::json& value) {
    TranscriptEntry entry;
    entry.id = 1; // Receiver assigns local numbers; validation needs a positive ID.
    entry.kind = static_cast<EntryKind>(value.at("kind").get<int>());
    entry.participant_id = value.at("participant_id").get<std::string>();
    entry.display_name = value.at("display_name").get<std::string>();
    entry.addressed_to = value.value("addressed_to", std::string{});
    entry.addressed_to_name = value.value("addressed_to_name", std::string{});
    entry.text = value.at("text").get<std::string>();
    entry.status = static_cast<EntryStatus>(value.at("status").get<int>());
    entry.created_at = value.value("created_at", std::int64_t{0});
    if (value.contains("input_tokens") && !value.at("input_tokens").is_null())
        entry.input_tokens = value.at("input_tokens").get<std::uint64_t>();
    if (value.contains("output_tokens") && !value.at("output_tokens").is_null())
        entry.output_tokens = value.at("output_tokens").get<std::uint64_t>();
    entry.web_search_used = value.value("web_search_used", false);
    require_storable_transcript_entry(entry);
    return entry;
}

std::optional<std::int64_t> find_forum_key(Database& database, std::string_view forum_id) {
    auto query = database.prepare(
        "SELECT forum_key FROM forums WHERE forum_id=?1", forum_id);
    if (!query.step()) return std::nullopt;
    return query.integer(0);
}

std::optional<std::int64_t> find_session_key(
    Database& database, std::string_view forum_id, std::string_view session_id) {
    auto query = database.prepare(
        "SELECT s.session_key FROM sessions s JOIN forums f USING(forum_key) "
        "WHERE f.forum_id=?1 AND s.session_id=?2 AND s.archived_at IS NULL",
        forum_id, session_id);
    if (!query.step()) return std::nullopt;
    return query.integer(0);
}

std::int64_t ensure_session(
    Database& database,
    std::string_view forum_id,
    std::string_view session_id,
    std::string_view label) {
    if (auto found = find_session_key(database, forum_id, session_id)) return *found;
    const auto forum_key = find_forum_key(database, forum_id);
    if (!forum_key) throw std::runtime_error("Chat forum does not exist.");
    const std::int64_t now = session_timestamp();
    database.prepare(
        "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
        "history_epoch, next_entry_id, next_request_id, recent_pending, discardable) "
        "VALUES (?1, ?2, ?3, ?4, 1, 1, 1, 0, 0)",
        *forum_key, session_id, label, now).run();
    auto created = find_session_key(database, forum_id, session_id);
    if (!created) throw std::runtime_error("Failed to create incoming chat.");
    return *created;
}

void touch(Database& database, std::int64_t session_key) {
    database.prepare(
        "UPDATE sessions SET updated_at=?1 WHERE session_key=?2",
        session_timestamp(), session_key).run();
}

std::int64_t allocate_entry(Database& database, std::int64_t session_key) {
    auto query = database.prepare(
        "SELECT next_entry_id FROM sessions WHERE session_key=?1",
        session_key);
    if (!query.step()) throw std::runtime_error("Incoming chat is missing.");
    const std::int64_t entry_id = query.integer(0);
    database.prepare(
        "UPDATE sessions SET next_entry_id=?1 WHERE session_key=?2",
        entry_id + 1, session_key).run();
    return entry_id;
}

std::int64_t allocate_request(Database& database, std::int64_t session_key) {
    auto query = database.prepare(
        "SELECT next_request_id FROM sessions WHERE session_key=?1", session_key);
    if (!query.step()) throw std::runtime_error("Incoming chat is missing.");
    const std::int64_t request_id = query.integer(0);
    database.prepare(
        "UPDATE sessions SET next_request_id=?1 WHERE session_key=?2",
        request_id + 1, session_key).run();
    return request_id;
}

std::int64_t current_epoch(Database& database, std::int64_t session_key) {
    auto query = database.prepare(
        "SELECT history_epoch FROM sessions WHERE session_key=?1", session_key);
    if (!query.step()) throw std::runtime_error("Incoming chat is missing.");
    return query.integer(0);
}

void insert_entry_row(
    Database& database,
    std::int64_t session_key,
    std::int64_t entry_id,
    std::optional<std::int64_t> request_id,
    const TranscriptEntry& entry) {
    const std::int64_t epoch = current_epoch(database, session_key);
    auto insert = database.prepare(
        "INSERT INTO entries (session_key, entry_id, epoch, request_id, kind, "
        "participant_id, display_name, addressed_to, addressed_to_name, text, "
        "status, created_at, input_tokens, output_tokens, web_search_used) VALUES "
        "(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15)",
        session_key, entry_id, epoch);
    if (request_id) insert.bind(4, *request_id);
    else insert.bind_null(4);
    insert.bind(5, static_cast<std::int64_t>(entry.kind));
    insert.bind(6, entry.participant_id);
    insert.bind(7, entry.display_name);
    insert.bind(8, entry.addressed_to);
    insert.bind(9, entry.addressed_to_name);
    insert.bind(10, entry.text);
    insert.bind(11, static_cast<std::int64_t>(entry.status));
    insert.bind(12, entry.created_at);
    if (entry.input_tokens) insert.bind(13, static_cast<std::int64_t>(*entry.input_tokens));
    else insert.bind_null(13);
    if (entry.output_tokens) insert.bind(14, static_cast<std::int64_t>(*entry.output_tokens));
    else insert.bind_null(14);
    insert.bind(15, entry.web_search_used ? 1 : 0);
    insert.run();
}

bool is_self_note(const TranscriptEntry& entry) {
    return entry.kind == EntryKind::human && entry.addressed_to == null_agent_handle;
}

} // namespace

bool sync_chat_is_shared(Database& database, std::int64_t session_key) {
    auto session = database.prepare(
        "SELECT label, next_request_id FROM sessions "
        "WHERE session_key=?1 AND archived_at IS NULL",
        session_key);
    if (!session.step()) return false;
    const std::string label = session.text(0);
    const std::int64_t next_request = session.integer(1);
    if (is_temporary_session_label(label)) return false;
    if (next_request <= 1) return false;
    auto running = database.prepare(
        "SELECT 1 FROM turns WHERE session_key=?1 AND request_id=1 AND state=0 LIMIT 1",
        session_key);
    if (running.step()) return false;
    return true;
}

std::optional<SyncChatIdentity> sync_chat_identity(
    Database& database, std::int64_t session_key) {
    auto query = database.prepare(
        "SELECT f.forum_id, s.session_id, s.label FROM sessions s "
        "JOIN forums f USING(forum_key) WHERE s.session_key=?1 "
        "AND s.archived_at IS NULL",
        session_key);
    if (!query.step()) return std::nullopt;
    return SyncChatIdentity{query.text(0), query.text(1), query.text(2)};
}

void sync_enqueue_chat(
    Database& database,
    const SyncChatIdentity& identity,
    std::string_view kind,
    nlohmann::json fields) {
    append_record(database, identity, kind, std::move(fields));
}

static nlohmann::json chat_entry_json(const TranscriptEntry& entry) {
    nlohmann::json value;
    value["kind"] = static_cast<int>(entry.kind);
    value["participant_id"] = entry.participant_id;
    value["display_name"] = entry.display_name;
    value["addressed_to"] = entry.addressed_to;
    value["addressed_to_name"] = entry.addressed_to_name;
    value["text"] = entry.text;
    value["status"] = static_cast<int>(entry.status);
    value["created_at"] = entry.created_at;
    if (entry.input_tokens) value["input_tokens"] = *entry.input_tokens;
    else value["input_tokens"] = nullptr;
    if (entry.output_tokens) value["output_tokens"] = *entry.output_tokens;
    else value["output_tokens"] = nullptr;
    value["web_search_used"] = entry.web_search_used;
    return value;
}

nlohmann::json sync_chat_entries_fields(
    const std::vector<TranscriptEntry>& entries,
    std::optional<int> turn_state) {
    nlohmann::json fields;
    if (turn_state) fields["state"] = *turn_state;
    nlohmann::json list = nlohmann::json::array();
    for (const auto& entry : entries) list.push_back(chat_entry_json(entry));
    fields["entries"] = std::move(list);
    return fields;
}

void sync_record_chat_initial(
    Database& database,
    std::int64_t session_key,
    const SyncChatIdentity& identity) {
    // Use the prompt's entry number to order a finished turn among self-notes.
    auto heads = database.prepare(
        "SELECT min(e.entry_id), e.request_id, t.state FROM entries e "
        "JOIN turns t USING(session_key, request_id) "
        "WHERE e.session_key=?1 AND t.state<>0 GROUP BY e.request_id HAVING count(*)<=2 "
        "UNION ALL SELECT entry_id, NULL, NULL FROM entries "
        "WHERE session_key=?1 AND request_id IS NULL AND kind=0 AND addressed_to=?2 "
        "ORDER BY 1", session_key, null_agent_handle);
    while (heads.step()) {
        auto rows = heads.is_null(1)
            ? database.prepare(
                "SELECT kind, participant_id, display_name, addressed_to, "
                "addressed_to_name, text, status, created_at, input_tokens, "
                "output_tokens, web_search_used FROM entries "
                "WHERE session_key=?1 AND entry_id=?2", session_key, heads.integer(0))
            : database.prepare(
                "SELECT kind, participant_id, display_name, addressed_to, "
                "addressed_to_name, text, status, created_at, input_tokens, "
                "output_tokens, web_search_used FROM entries "
                "WHERE session_key=?1 AND request_id=?2 ORDER BY entry_id",
                session_key, heads.integer(1));
        std::vector<TranscriptEntry> entries;
        while (rows.step()) {
            TranscriptEntry entry{
                .kind = static_cast<EntryKind>(rows.integer(0)),
                .participant_id = rows.text(1),
                .display_name = rows.text(2),
                .addressed_to = rows.text(3),
                .addressed_to_name = rows.text(4),
                .text = rows.text(5),
                .status = static_cast<EntryStatus>(rows.integer(6)),
                .created_at = rows.integer(7),
            };
            if (!rows.is_null(8)) entry.input_tokens = static_cast<std::uint64_t>(rows.integer(8));
            if (!rows.is_null(9)) entry.output_tokens = static_cast<std::uint64_t>(rows.integer(9));
            entry.web_search_used = rows.integer(10) != 0;
            entries.push_back(std::move(entry));
        }
        const auto state = heads.is_null(2) ? std::optional<int>{}
                                          : static_cast<int>(heads.integer(2));
        append_record(database, identity, "chat.entries", sync_chat_entries_fields(entries, state));
    }
    // Attachments.
    {
        auto notes = database.prepare(
            "SELECT note_id, name, text FROM session_notes WHERE session_key=?1 ORDER BY note_id",
            session_key);
        while (notes.step()) {
            const std::int64_t note_id = notes.integer(0);
            std::vector<std::string> path;
            try {
                path = sync_note_path(database, note_id);
            } catch (...) {
                continue; // R55: deleted source makes the copy ineligible.
            }
            nlohmann::json fields;
            fields["path"] = path;
            fields["name"] = notes.text(1);
            fields["text"] = notes.text(2);
            append_record(database, identity, "chat.add_note", std::move(fields));
        }
    }
    {
        auto files = database.prepare(
            "SELECT name, text FROM session_files WHERE session_key=?1 ORDER BY name",
            session_key);
        while (files.step()) {
            nlohmann::json fields;
            fields["name"] = files.text(0);
            fields["text"] = files.text(1);
            append_record(database, identity, "chat.add_file", std::move(fields));
        }
    }
    {
        auto folders = database.prepare(
            "SELECT folder_id FROM session_folders WHERE session_key=?1 ORDER BY folder_id",
            session_key);
        while (folders.step()) {
            const std::int64_t folder_id = folders.integer(0);
            std::vector<std::string> path;
            try {
                path = sync_note_path(database, folder_id);
            } catch (...) {
                continue;
            }
            nlohmann::json fields;
            fields["path"] = path;
            append_record(database, identity, "chat.add_folder", std::move(fields));
        }
    }
    {
        auto settings = database.prepare(
            "SELECT character_id, provider_id, reasoning_effort "
            "FROM session_character_settings WHERE session_key=?1 ORDER BY character_id",
            session_key);
        while (settings.step()) {
            nlohmann::json fields;
            fields["character_id"] = settings.text(0);
            if (settings.is_null(1)) fields["provider_id"] = nullptr;
            else fields["provider_id"] = settings.text(1);
            if (settings.is_null(2)) fields["reasoning_effort"] = nullptr;
            else fields["reasoning_effort"] = settings.text(2);
            append_record(database, identity, "chat.character", std::move(fields));
        }
    }
}

SyncChatIdentity sync_parse_chat_identity(const nlohmann::json& record) {
    const auto chat = record.at("chat");
    if (!chat.is_array() || chat.size() != 2)
        throw std::runtime_error("Incoming chat record has no chat.");
    SyncChatIdentity identity;
    identity.forum_id = chat.at(0).get<std::string>();
    identity.session_id = chat.at(1).get<std::string>();
    identity.label = record.at("label").get<std::string>();
    if (identity.forum_id.empty() || identity.session_id.empty() || identity.label.empty())
        throw std::runtime_error("Incoming chat record has no chat.");
    return identity;
}

namespace {

std::vector<std::string> parse_path(const nlohmann::json& value) {
    if (!value.is_array()) throw std::runtime_error("Incoming chat record has no path.");
    std::vector<std::string> path;
    for (const auto& item : value) path.push_back(item.get<std::string>());
    if (path.empty()) throw std::runtime_error("Incoming chat record has no path.");
    return path;
}

std::int64_t require_attachment(
    Database& database, const nlohmann::json& path, NoteItemKind kind) {
    const auto found = sync_note_find(database, parse_path(path));
    if (found) {
        auto query = database.prepare("SELECT kind FROM note_items WHERE id=?1", *found);
        if (query.step()) {
            const auto stored_kind = query.text(0);
            if (kind == NoteItemKind::folder ? stored_kind == "folder"
                : stored_kind == "note" || stored_kind == "annotation") {
                return *found;
            }
        }
    }
    throw std::runtime_error("Incoming chat attachment was not found.");
}

} // namespace

bool sync_chat_forum_exists(Database& database, std::string_view forum_id) {
    return find_forum_key(database, forum_id).has_value();
}

std::vector<std::int64_t> sync_chat_turns(Database& database, std::int64_t session_key) {
    auto rows = database.prepare(
        "SELECT t.request_id FROM turns t JOIN entries e USING(session_key, request_id) "
        "WHERE t.session_key=?1 AND t.state<>0 AND e.kind IN (1, 3) ORDER BY t.request_id",
        session_key);
    std::vector<std::int64_t> turns;
    while (rows.step()) turns.push_back(rows.integer(0));
    return turns;
}

void sync_apply_chat_record(Database& database, const nlohmann::json& record) {
    const std::string kind = record.at("kind").get<std::string>();
    const SyncChatIdentity identity = sync_parse_chat_identity(record);
    storage::SqliteTransaction transaction(database);
    const std::int64_t session_key =
        ensure_session(database, identity.forum_id, identity.session_id, identity.label);

    if (kind == "chat.entries") {
        const auto list = record.at("entries");
        if (!list.is_array() || list.empty())
            throw std::runtime_error("Incoming chat record has no entries.");
        std::vector<TranscriptEntry> entries;
        for (const auto& item : list) entries.push_back(parse_entry_strict(item));
        const bool has_state = record.contains("state") && !record.at("state").is_null();
        if (has_state) {
            const int state = record.at("state").get<int>();
            if (state < 1 || state > 3)
                throw std::runtime_error("Incoming chat record is invalid.");
            // A turn that was stopped before any response has only its prompt.
            if (entries.size() > 2 || (entries.size() == 1 && state != 2))
                throw std::runtime_error("Incoming chat record is invalid.");
            const auto expected_kind = state == 3 ? EntryKind::error : EntryKind::character;
            const auto expected_status = state == 3 ? EntryStatus::failed
                : state == 2 ? EntryStatus::cancelled : EntryStatus::complete;
            if (entries[0].kind != EntryKind::human
                || (entries.size() == 2 && (entries[1].kind != expected_kind
                    || entries[1].status != expected_status))) {
                throw std::runtime_error("Incoming chat turn has invalid entries.");
            }
            const std::int64_t request_id = allocate_request(database, session_key);
            database.prepare(
                "INSERT INTO turns (session_key, request_id, epoch, state) VALUES (?1, ?2, ?3, ?4)",
                session_key, request_id, current_epoch(database, session_key), state).run();
            for (const auto& entry : entries) {
                TranscriptEntry stored = entry;
                stored.request_id = static_cast<std::uint64_t>(request_id);
                const std::int64_t entry_id = allocate_entry(database, session_key);
                stored.id = static_cast<std::uint64_t>(entry_id);
                insert_entry_row(database, session_key, entry_id, request_id, stored);
            }
        } else {
            if (entries.size() != 1 || !is_self_note(entries.front()))
                throw std::runtime_error("Incoming chat record is invalid.");
            TranscriptEntry stored = entries.front();
            stored.request_id = std::nullopt;
            const std::int64_t entry_id = allocate_entry(database, session_key);
            stored.id = static_cast<std::uint64_t>(entry_id);
            insert_entry_row(database, session_key, entry_id, std::nullopt, stored);
        }
        touch(database, session_key);
    } else if (kind == "chat.rename") {
        database.prepare(
            "UPDATE sessions SET label=?1, updated_at=?2, recent_pending=0, discardable=0 "
            "WHERE session_key=?3",
            identity.label, session_timestamp(), session_key).run();
    } else if (kind == "chat.delete") {
        database.prepare("DELETE FROM sessions WHERE session_key=?1", session_key).run();
    } else if (kind == "chat.clear") {
        database.prepare("DELETE FROM entries WHERE session_key=?1", session_key).run();
        database.prepare("DELETE FROM turns WHERE session_key=?1", session_key).run();
        database.prepare("DELETE FROM session_notes WHERE session_key=?1", session_key).run();
        database.prepare("DELETE FROM session_files WHERE session_key=?1", session_key).run();
        database.prepare("DELETE FROM session_folders WHERE session_key=?1", session_key).run();
        touch(database, session_key);
    } else if (kind == "chat.delete_turn") {
        const auto position = record.at("turn").get<std::int64_t>();
        const auto turns = sync_chat_turns(database, session_key);
        if (position < 1 || static_cast<std::uint64_t>(position) > turns.size())
            throw std::runtime_error("Incoming chat turn was not found.");
        const auto target = turns[static_cast<std::size_t>(position - 1)];
        database.prepare(
            "DELETE FROM entries WHERE session_key=?1 AND request_id=?2",
            session_key, target).run();
        database.prepare(
            "DELETE FROM turns WHERE session_key=?1 AND request_id=?2",
            session_key, target).run();
        touch(database, session_key);
    } else if (kind == "chat.add_note") {
        const auto note = require_attachment(database, record.at("path"), NoteItemKind::note);
        const std::string name = record.at("name").get<std::string>();
        const std::string text = record.at("text").get<std::string>();
        // The notices use the same text as SessionJournal.
        const bool refresh = database.prepare(
            "SELECT 1 FROM session_notes WHERE session_key=?1 AND note_id=?2",
            session_key, note).step();
        database.prepare(
            "INSERT INTO session_notes (session_key, note_id, name, text) VALUES (?1, ?2, ?3, ?4) "
            "ON CONFLICT(session_key, note_id) DO UPDATE SET name=excluded.name, text=excluded.text",
            session_key, note, name, text).run();
        TranscriptEntry notice = make_notice_entry(1, refresh
            ? "Updated this chat\u2019s copy of note \u201c" + name + "\u201d."
            : "Added note \u201c" + name + "\u201d to this chat.");
        const std::int64_t entry_id = allocate_entry(database, session_key);
        notice.id = static_cast<std::uint64_t>(entry_id);
        insert_entry_row(database, session_key, entry_id, std::nullopt, notice);
        touch(database, session_key);
    } else if (kind == "chat.add_file") {
        const std::string name = record.at("name").get<std::string>();
        const std::string text = record.at("text").get<std::string>();
        if (name.empty()) throw std::runtime_error("Incoming chat record is invalid.");
        const bool refresh = database.prepare(
            "SELECT 1 FROM session_files WHERE session_key=?1 AND name=?2",
            session_key, name).step();
        database.prepare(
            "INSERT INTO session_files (session_key, name, text) VALUES (?1, ?2, ?3) "
            "ON CONFLICT(session_key, name) DO UPDATE SET text=excluded.text",
            session_key, name, text).run();
        TranscriptEntry notice = make_notice_entry(1, refresh
            ? "Updated this chat\u2019s copy of file \u201c" + name + "\u201d."
            : "Added file \u201c" + name + "\u201d to this chat.");
        const std::int64_t entry_id = allocate_entry(database, session_key);
        notice.id = static_cast<std::uint64_t>(entry_id);
        insert_entry_row(database, session_key, entry_id, std::nullopt, notice);
        touch(database, session_key);
    } else if (kind == "chat.add_folder") {
        const auto path = parse_path(record.at("path"));
        const auto folder = require_attachment(database, record.at("path"), NoteItemKind::folder);
        database.prepare(
            "INSERT INTO session_folders (session_key, folder_id) VALUES (?1, ?2) "
            "ON CONFLICT DO NOTHING",
            session_key, folder).run();
        TranscriptEntry notice = make_notice_entry(
            1, "Added folder \u201c" + path.back() + "\u201d to this chat.");
        const std::int64_t entry_id = allocate_entry(database, session_key);
        notice.id = static_cast<std::uint64_t>(entry_id);
        insert_entry_row(database, session_key, entry_id, std::nullopt, notice);
        touch(database, session_key);
    } else if (kind == "chat.detach_folder") {
        const auto path = parse_path(record.at("path"));
        const auto folder = require_attachment(database, record.at("path"), NoteItemKind::folder);
        database.prepare(
            "DELETE FROM session_folders WHERE session_key=?1 AND folder_id=?2",
            session_key, folder).run();
        TranscriptEntry notice = make_notice_entry(
            1, "Removed folder \u201c" + path.back() + "\u201d from this chat.");
        const std::int64_t entry_id = allocate_entry(database, session_key);
        notice.id = static_cast<std::uint64_t>(entry_id);
        insert_entry_row(database, session_key, entry_id, std::nullopt, notice);
        touch(database, session_key);
    } else if (kind == "chat.character") {
        const std::string character_id = record.at("character_id").get<std::string>();
        if (character_id.empty() || character_id == "*" || character_id == "-")
            throw std::runtime_error("Incoming chat record is invalid.");
        const bool has_provider = record.contains("provider_id")
            && !record.at("provider_id").is_null()
            && !record.at("provider_id").get<std::string>().empty();
        const bool has_effort = record.contains("reasoning_effort")
            && !record.at("reasoning_effort").is_null()
            && !record.at("reasoning_effort").get<std::string>().empty();
        if (!has_provider && !has_effort) {
            database.prepare(
                "DELETE FROM session_character_settings WHERE session_key=?1 AND character_id=?2",
                session_key, character_id).run();
        } else {
            auto insert = database.prepare(
                "INSERT INTO session_character_settings "
                "(session_key, character_id, provider_id, reasoning_effort) "
                "VALUES (?1, ?2, ?3, ?4) "
                "ON CONFLICT(session_key, character_id) DO UPDATE SET "
                "provider_id=excluded.provider_id, reasoning_effort=excluded.reasoning_effort",
                session_key, character_id);
            if (has_provider) insert.bind(3, record.at("provider_id").get<std::string>());
            else insert.bind_null(3);
            if (has_effort) insert.bind(4, record.at("reasoning_effort").get<std::string>());
            else insert.bind_null(4);
            insert.run();
        }
        touch(database, session_key);
    } else {
        throw std::runtime_error("Incoming chat record is invalid.");
    }
    transaction.commit();
}

} // namespace cha
