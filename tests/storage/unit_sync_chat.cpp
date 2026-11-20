#include "app/d1_client.h"
#include "app/d1_sync.h"
#include "runtime/live_session_manager.h"
#include "session/session_open.h"
#include "chat/transcript.h"
#include "storage/note_storage.h"
#include "storage/session_database.h"
#include "storage/session_repository.h"
#include "storage/sqlite_storage.h"
#include "storage/sync_chat.h"
#include "storage/sync_storage.h"
#include "storage/workspace_session_database.h"
#include "support/test_workspace.h"
#include "util/base64.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

constexpr std::string_view sync_machine = "mac";

std::string d1_key_toml() {
    return "display_name = \"D1\"\ntype = \"D1\"\naccount_id = \"a\"\n"
        "database_id = \"d\"\napi_token = \"t\"\n";
}

void add_d1_key(Database& database) {
    database.prepare(
        "INSERT INTO config (name, content) VALUES (?1, ?2)",
        "system/keys/api_key_1/config.toml", d1_key_toml()).run();
}

void make_chat_session(
    Database& database,
    std::string_view forum_id = "f1",
    std::string_view session_id = "20261109-120000-session",
    std::string_view label = "Test Chat") {
    database.prepare("INSERT INTO forums (forum_id) VALUES (?1) "
        "ON CONFLICT(forum_id) DO NOTHING", forum_id).run();
    auto forum = database.prepare("SELECT forum_key FROM forums WHERE forum_id=?1", forum_id);
    (void)forum.step();
    const std::int64_t forum_key = forum.integer(0);
    database.prepare(
        "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
        "history_epoch, next_entry_id, next_request_id, recent_pending, discardable) "
        "VALUES (?1, ?2, ?3, ?4, 1, 1, 1, 0, 0)",
        forum_key, session_id, label,
        static_cast<std::int64_t>(std::time(nullptr))).run();
}

std::int64_t session_key_for(
    Database& database,
    std::string_view forum_id,
    std::string_view session_id) {
    auto query = database.prepare(
        "SELECT s.session_key FROM sessions s JOIN forums f USING(forum_key) "
        "WHERE f.forum_id=?1 AND s.session_id=?2",
        forum_id, session_id);
    if (!query.step()) throw std::runtime_error("missing test session");
    return query.integer(0);
}

nlohmann::json queued_records(Database& database) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& row : sync_queue_list(database)) {
        out.push_back(nlohmann::json::parse(row.record));
    }
    return out;
}

TranscriptEntry human_entry(
    EntryId id, std::string text, std::optional<RequestId> request = std::nullopt) {
    TranscriptEntry entry = make_human_entry({
        .id = id,
        .author = {"user", "User"},
        .addressed_to = {"agent", "Agent"},
        .text = std::move(text),
        .request_id = request,
    });
    return entry;
}

TranscriptEntry reply_entry(
    EntryId id, std::string text, RequestId request) {
    return make_character_entry(id, "agent", "Agent", std::move(text),
        EntryStatus::complete, request);
}

class ChatSyncTest : public testing::Test {
protected:
    void SetUp() override {
        path_ = workspace_.root() / "chat.sqlite3";
        create_empty_workspace_session_database(path_);
        database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
        add_d1_key(*database_);
    }

    test::TestWorkspace workspace_;
    std::filesystem::path path_;
    std::unique_ptr<Database> database_;
};

TEST_F(ChatSyncTest, TemporaryLabelWritesNothing) {
    make_chat_session(*database_, "f1", "s-temp", "New session");
    const auto key = session_key_for(*database_, "f1", "s-temp");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.start_turn(1, human_entry(1, "Hi", 1));
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    EXPECT_TRUE(sync_queue_list(*database_).empty());
}

TEST_F(ChatSyncTest, FirstFinishedTurnPublishesInitialOnce) {
    make_chat_session(*database_, "f1", "s-1", "Test Chat");
    const auto key = session_key_for(*database_, "f1", "s-1");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    // Running turn alone sends nothing.
    journal.start_turn(1, human_entry(1, "Hi", 1));
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[0]["chat"], nlohmann::json::array({"f1", "s-1"}));
    EXPECT_EQ(records[0]["label"], "Test Chat");
    EXPECT_EQ(records[0]["state"], 1);
    ASSERT_EQ(records[0]["entries"].size(), 2u);
    // Second turn sends only its own record.
    journal.start_turn(2, human_entry(3, "Again", 2));
    journal.complete_turn(2, reply_entry(4, "Again!", 2));
    records = queued_records(*database_);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1]["entries"].size(), 2u);
}

TEST_F(ChatSyncTest, TerminalStatesArePreserved) {
    make_chat_session(*database_, "f1", "s-cancel", "Chat");
    auto key = session_key_for(*database_, "f1", "s-cancel");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        auto cancelled = make_character_entry(
            2, "agent", "Agent", "Stopped", EntryStatus::cancelled, 1);
        journal.cancel_turn(1, cancelled);
    }
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["state"], 2);
    // Failed turn.
    make_chat_session(*database_, "f1", "s-fail", "Chat");
    key = session_key_for(*database_, "f1", "s-fail");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.fail_turn(1, make_error_entry(2, "Boom", 1));
    }
    records = queued_records(*database_);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1]["state"], 3);
    // R59: a turn cancelled with no response adds no record.
    make_chat_session(*database_, "f1", "s-empty-cancel", "Chat");
    key = session_key_for(*database_, "f1", "s-empty-cancel");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.cancel_turn(1, std::nullopt);
        journal.start_turn(2, human_entry(2, "Again", 2));
        journal.cancel_turn(2, std::nullopt);
    }
    records = queued_records(*database_);
    ASSERT_EQ(records.size(), 2u);

    // The receiver skips a turn with only its prompt.
    auto prompt_only = records[0];
    prompt_only["chat"][1] = "s-invalid";
    prompt_only["entries"].erase(1);
    for (const int state : {1, 2, 3}) {
        prompt_only["state"] = state;
        EXPECT_THROW(sync_apply_chat_record(*database_, prompt_only), std::runtime_error);
    }
    // A fractional number is skipped; it is not cut to an integer.
    for (const auto* field : {"/state", "/entries/0/kind", "/entries/1/status"}) {
        auto fractional = records[0];
        fractional["chat"][1] = "s-invalid";
        const nlohmann::json::json_pointer pointer(field);
        fractional[pointer] = fractional[pointer].get<int>() + 0.5;
        EXPECT_THROW(sync_apply_chat_record(*database_, fractional), std::runtime_error) << field;
    }
    auto received = database_->prepare(
        "SELECT COUNT(*) FROM sessions WHERE session_id='s-invalid'");
    ASSERT_TRUE(received.step());
    EXPECT_EQ(received.integer(0), 0);
}

TEST_F(ChatSyncTest, RenameFromTemporaryPublishesInitialNotRename) {
    make_chat_session(*database_, "f1", "s-rename", "New session");
    const auto key = session_key_for(*database_, "f1", "s-rename");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.start_turn(1, human_entry(1, "Hi", 1));
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    // Temporary label: still nothing.
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    journal.rename("Real Name");
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[0]["label"], "Real Name");
}

TEST_F(ChatSyncTest, SelfNoteMovesOnlyWhenShared) {
    make_chat_session(*database_, "f1", "s-self", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-self");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    // @- alone never shares.
    TranscriptEntry self = make_human_entry({
        .id = 1,
        .author = {"user", "User"},
        .addressed_to = {std::string(null_agent_handle), std::string(null_agent_name)},
        .text = "Reminder",
    });
    journal.record_entry(self);
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    // After a finished turn, @- sends its own record.
    journal.start_turn(1, human_entry(2, "Hi", 1));
    journal.complete_turn(1, reply_entry(3, "Hello", 1));
    const auto initial = queued_records(*database_);
    ASSERT_EQ(initial.size(), 2u);
    EXPECT_EQ(initial[0]["entries"][0]["text"], "Reminder");
    EXPECT_EQ(initial[1]["entries"][0]["text"], "Hi");
    EXPECT_FALSE(initial[1]["entries"][0].contains("entry_id"));
    EXPECT_FALSE(initial[1]["entries"][0].contains("request_id"));
    TranscriptEntry self2 = make_human_entry({
        .id = 4,
        .author = {"user", "User"},
        .addressed_to = {std::string(null_agent_handle), std::string(null_agent_name)},
        .text = "Reminder",
    });
    journal.record_entry(self2);
    auto records = queued_records(*database_);
    // Initial includes the first turn and the first @-; then the second @-.
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[1]["kind"], "chat.entries");
    EXPECT_EQ(records[2]["kind"], "chat.entries");
    ASSERT_EQ(records[2]["entries"].size(), 1u);
    // Standalone notices never move.
    journal.record_entry(make_notice_entry(5, "Notice"));
    EXPECT_EQ(queued_records(*database_).size(), 3u);
}

TEST_F(ChatSyncTest, InitialIncludesAttachmentsAndSettingsInOrder) {
    make_chat_session(*database_, "f1", "s-init", "New session");
    const auto key = session_key_for(*database_, "f1", "s-init");
    const auto note = note_create(*database_, std::nullopt, NoteItemKind::note, "N");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
        const NoteText copy{note.id, "N", "body"};
        (void)journal.add_note(copy, 3);
        (void)journal.add_text_file("f.txt", "data", 4);
        journal.set_character_settings("c1", {.provider_id = "p", .reasoning_effort = std::nullopt});
        // Still temporary: nothing.
        EXPECT_TRUE(sync_queue_list(*database_).empty());
        journal.rename("Shared");
    }
    auto records = queued_records(*database_);
    // Turn + note + file + character.
    ASSERT_GE(records.size(), 4u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[1]["kind"], "chat.add_note");
    EXPECT_EQ(records[2]["kind"], "chat.add_file");
    EXPECT_EQ(records[3]["kind"], "chat.character");
}

TEST_F(ChatSyncTest, UnchangedAutomaticNoteCopyWritesNoNoticeOrSyncRecord) {
    make_chat_session(*database_, "f1", "s-note", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-note");
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Plan");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.start_turn(1, human_entry(1, "Hi", 1));
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    NoteText copy{note.id, note.name, "Cancellation terms"};
    ASSERT_TRUE(journal.add_note(copy, 3, true));
    const auto before = queued_records(*database_);
    ASSERT_EQ(before.back()["kind"], "chat.add_note");
    database_->execute("CREATE TRIGGER reject_note_update BEFORE UPDATE ON session_notes "
        "BEGIN SELECT RAISE(ABORT, 'Unexpected note update'); END");
    EXPECT_FALSE(journal.add_note(copy, 4, true));
    EXPECT_EQ(queued_records(*database_), before);
    database_->execute("DROP TRIGGER reject_note_update");
    ASSERT_TRUE(journal.add_note(copy, 4)); // An explicit Ref ID always records a notice.
    EXPECT_EQ(queued_records(*database_).size(), before.size() + 1);
    copy.name = "Hotel";
    const auto updated = journal.add_note(copy, 5, true);
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->text, "Updated this chat’s copy of note “Hotel”.");
    EXPECT_EQ(queued_records(*database_).size(), before.size() + 2);
}

TEST_F(ChatSyncTest, DeletedSourceMakesCopyIneligible) {
    make_chat_session(*database_, "f1", "s-r55", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-r55");
    const auto note = note_create(*database_, std::nullopt, NoteItemKind::note, "Gone");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
        EXPECT_EQ(queued_records(*database_).size(), 1u);
        note_delete(*database_, note.id, std::string(sync_machine));
        const NoteText copy{note.id, "Gone", "body"};
        // Copy stays in the chat locally, but the source is gone.
        // Journal add_note would fail to find the source note text via read?
        // Simulate the R55 path: initial publication skips missing sources.
        // Here the add itself uses the passed copy, so it records; the
        // ineligible path is initial publication after deletion.
        (void)copy;
    }
    // Direct initial-publication check for R55: build a chat with a copy
    // whose source was deleted, then verify initial skips the copy.
    make_chat_session(*database_, "f1", "s-r55b", "New session");
    const auto key2 = session_key_for(*database_, "f1", "s-r55b");
    const auto doomed = note_create(*database_, std::nullopt, NoteItemKind::note, "Doomed");
    {
        SessionJournal journal(path_, key2);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
        const NoteText copy{doomed.id, "Doomed", "text"};
        (void)journal.add_note(copy, 3);
    }
    note_delete(*database_, doomed.id, std::string(sync_machine));
    // Clear queue to isolate the transition.
    {
        storage::SqliteTransaction transaction(*database_);
        database_->prepare("DELETE FROM sync_queue").run();
        transaction.commit();
    }
    {
        SessionJournal journal(path_, key2);
        journal.set_sync_machine(std::string(sync_machine));
        journal.rename("Now Shared");
    }
    auto records = queued_records(*database_);
    // The deleted-source copy is omitted: no chat.add_note for Doomed.
    bool saw_add_note = false;
    int chat_entries = 0;
    for (const auto& record : records) {
        if (record["kind"] == "chat.add_note") saw_add_note = true;
        if (record["kind"] == "chat.entries"
            && record["chat"] == nlohmann::json::array({"f1", "s-r55b"})) {
            ++chat_entries;
        }
    }
    EXPECT_FALSE(saw_add_note);
    EXPECT_EQ(chat_entries, 1);
}

TEST_F(ChatSyncTest, ClearAndLastTurnDeleteKeepEligibility) {
    make_chat_session(*database_, "f1", "s-clear", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-clear");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
        EXPECT_EQ(queued_records(*database_).size(), 1u);
        journal.clear_history();
        auto records = queued_records(*database_);
        ASSERT_EQ(records.size(), 2u);
        EXPECT_EQ(records[1]["kind"], "chat.clear");
        EXPECT_TRUE(sync_chat_is_shared(*database_, key));
    }
    // Restart: reopen and verify still shared (counter retained).
    {
        Database reopened(path_, Database::Mode::read_only);
        EXPECT_TRUE(sync_chat_is_shared(reopened, key));
    }
    {
        storage::SqliteTransaction transaction(*database_);
        database_->prepare("DELETE FROM sync_queue").run();
        transaction.commit();
    }
    // Delete the last turn in a chat with two turns; stays shared.
    make_chat_session(*database_, "f1", "s-del", "Chat");
    const auto key2 = session_key_for(*database_, "f1", "s-del");
    {
        SessionJournal journal(path_, key2);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "One", 1));
        journal.complete_turn(1, reply_entry(2, "One!", 1));
        journal.start_turn(2, human_entry(3, "Two", 2));
        journal.complete_turn(2, reply_entry(4, "Two!", 2));
        EXPECT_EQ(queued_records(*database_).size(), 2u);
        journal.delete_turn(4);
        auto records = queued_records(*database_);
        ASSERT_EQ(records.size(), 3u);
        EXPECT_EQ(records.back()["kind"], "chat.delete_turn");
        EXPECT_EQ(records.back()["turn"], 2);
        EXPECT_TRUE(sync_chat_is_shared(*database_, key2));
    }
}

TEST_F(ChatSyncTest, TitleBeforeResponsePublishesInitial) {
    make_chat_session(*database_, "f1", "s-title-first", "New session");
    const auto key = session_key_for(*database_, "f1", "s-title-first");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.rename("Real Title");
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    journal.start_turn(1, human_entry(1, "Hi", 1));
    journal.complete_turn(1, reply_entry(2, "Hello", 1));
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[0]["label"], "Real Title");
}

TEST_F(ChatSyncTest, InterruptedRepairPublishesInitial) {
    make_chat_session(*database_, "f1", "s-repair", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-repair");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        // Crash before finish: running turn remains.
    }
    {
        // Reopen repair: fail the interrupted turn.
        SessionJournal journal(path_, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.fail_turn(1, make_error_entry(2, "Interrupted", 1));
    }
    auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "chat.entries");
    EXPECT_EQ(records[0]["state"], 3);
}

TEST_F(ChatSyncTest, ClosedChatRenameAndDeleteEmit) {
    const auto path = test::import_test_database(workspace_.root());
    auto store = WorkspaceConfigStore::open(path);
    SessionRepository repository(
        [&] { return store->snapshot(); }, path, store->workspace_path(),
        store->welcome_path(), {{"welcome", "welcome"}, "Welcome"});
    repository.set_sync_machine(std::string(sync_machine));
    const auto chat = repository.create("lobby", "Closed");
    Database database(path, Database::Mode::read_write);
    add_d1_key(database);
    const auto key = session_key_for(database, chat.identity.forum_id, chat.identity.session_id);
    {
        SessionJournal journal(path, key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(1, human_entry(1, "Hi", 1));
        journal.complete_turn(1, reply_entry(2, "Hello", 1));
    }
    (void)repository.rename(chat.identity, "Renamed Closed");
    repository.delete_session(chat.identity);

    const auto records = queued_records(database);
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[1]["kind"], "chat.rename");
    EXPECT_EQ(records[1]["label"], "Renamed Closed");
    EXPECT_EQ(records[2]["kind"], "chat.delete");
    EXPECT_EQ(records[2]["label"], "Renamed Closed");
}

TEST_F(ChatSyncTest, TurnPositionIgnoresLocalCancellationWithoutReply) {
    make_chat_session(*database_, "f1", "s-local-cancel", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-local-cancel");
    SessionJournal journal(path_, key);
    journal.set_sync_machine(std::string(sync_machine));
    journal.start_turn(1, human_entry(1, "Cancelled locally", 1));
    journal.cancel_turn(1, std::nullopt);
    journal.start_turn(2, human_entry(2, "First shared prompt", 2));
    journal.complete_turn(2, reply_entry(3, "First shared reply", 2));
    journal.delete_turn(3);
    // The turn without a reply makes the chat shared but adds no record.
    const auto records = queued_records(*database_);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0]["entries"][0]["text"], "First shared prompt");
    EXPECT_EQ(records[1]["turn"], 1);

    make_chat_session(*database_, "f1", "s-receiver", "Chat");
    const auto receiver = session_key_for(*database_, "f1", "s-receiver");
    SessionJournal local(path_, receiver);
    local.start_turn(1, human_entry(1, "Another local cancellation", 1));
    local.cancel_turn(1, std::nullopt);
    for (auto record : records) {
        record["chat"][1] = "s-receiver";
        sync_apply_chat_record(*database_, record);
    }
    // The local turn without a reply stays; the received turn is deleted.
    auto remaining = database_->prepare(
        "SELECT request_id FROM turns WHERE session_key=?1 ORDER BY request_id", receiver);
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.integer(0), 1);
    EXPECT_FALSE(remaining.step());
}

TEST_F(ChatSyncTest, IncomingCreatesMissingChatWithLocalNumbering) {
    make_chat_session(*database_, "f1", "s-local", "Local");
    const auto local_key = session_key_for(*database_, "f1", "s-local");
    {
        SessionJournal journal(path_, local_key);
        journal.set_sync_machine(std::string(sync_machine));
        journal.start_turn(9, human_entry(1, "Local prompt", 9));
        journal.complete_turn(9, reply_entry(2, "Local reply", 9));
    }
    // Incoming turn for a missing chat uses carried identity/label and
    // receiver-local numbers.
    nlohmann::json record = {
        {"kind", "chat.entries"},
        {"chat", {"f1", "remote-1"}},
        {"label", "Remote"},
        {"state", 1},
        {"entries", {
            {{"kind", 0}, {"participant_id", "u"}, {"display_name", "U"},
             {"addressed_to", "a"}, {"addressed_to_name", "A"},
             {"text", "P"}, {"status", 0}, {"created_at", 0},
             {"input_tokens", nullptr}, {"output_tokens", nullptr},
             {"web_search_used", false}, {"request_id", 99}},
            {{"kind", 1}, {"participant_id", "a"}, {"display_name", "A"},
             {"addressed_to", ""}, {"addressed_to_name", ""},
             {"text", "R"}, {"status", 0}, {"created_at", 0},
             {"input_tokens", nullptr}, {"output_tokens", nullptr},
             {"web_search_used", false}, {"request_id", 99}},
        }},
    };
    const auto before = queued_records(*database_).size();
    EXPECT_EQ(before, 1u); // Local turn only.
    sync_apply_chat_record(*database_, record);
    // No echo: incoming writes no new records.
    EXPECT_EQ(queued_records(*database_).size(), before);
    const auto remote_key = session_key_for(*database_, "f1", "remote-1");
    auto check = database_->prepare(
        "SELECT label, next_entry_id, next_request_id FROM sessions WHERE session_key=?1",
        remote_key);
    ASSERT_TRUE(check.step());
    EXPECT_EQ(check.text(0), "Remote");
    // Local numbers start at 1, not 99.
    EXPECT_EQ(check.integer(1), 3);
    EXPECT_EQ(check.integer(2), 2);
    // No echo: incoming sets no flags.
    auto flags = database_->prepare("SELECT COUNT(*) FROM note_items WHERE changed<>0");
    ASSERT_TRUE(flags.step());
    EXPECT_EQ(flags.integer(0), 0);
}

TEST_F(ChatSyncTest, IncomingPositionalDeleteAndMissingAttachmentSkip) {
    make_chat_session(*database_, "f1", "s-pos", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-pos");
    {
        SessionJournal journal(path_, key);
        journal.set_sync_machine("");
        journal.start_turn(1, human_entry(1, "One", 1));
        journal.complete_turn(1, reply_entry(2, "One!", 1));
        journal.start_turn(2, human_entry(3, "Two", 2));
        journal.complete_turn(2, reply_entry(4, "Two!", 2));
    }
    // Delete the first of two turns by position.
    sync_apply_chat_record(*database_, {
        {"kind", "chat.delete_turn"},
        {"chat", {"f1", "s-pos"}},
        {"label", "Chat"},
        {"turn", 1},
    });
    auto remaining = database_->prepare(
        "SELECT COUNT(*) FROM turns WHERE session_key=?1", key);
    ASSERT_TRUE(remaining.step());
    EXPECT_EQ(remaining.integer(0), 1);
    // Missing attachment skips with warning (throws, caller warns).
    EXPECT_THROW(
        sync_apply_chat_record(*database_, {
            {"kind", "chat.add_note"},
            {"chat", {"f1", "s-pos"}},
            {"label", "Chat"},
            {"path", {"Nope"}},
            {"name", "Nope"},
            {"text", "x"},
        }),
        std::runtime_error);
    // Missing forum skips.
    EXPECT_THROW(
        sync_apply_chat_record(*database_, {
            {"kind", "chat.rename"},
            {"chat", {"missing-forum", "s-pos"}},
            {"label", "Chat"},
        }),
        std::runtime_error);
    // Invalid turn position skips.
    EXPECT_THROW(
        sync_apply_chat_record(*database_, {
            {"kind", "chat.delete_turn"},
            {"chat", {"f1", "s-pos"}},
            {"label", "Chat"},
            {"turn", 99},
        }),
        std::runtime_error);
    // A fractional position skips and keeps the remaining turn.
    EXPECT_THROW(
        sync_apply_chat_record(*database_, {
            {"kind", "chat.delete_turn"},
            {"chat", {"f1", "s-pos"}},
            {"label", "Chat"},
            {"turn", 1.5},
        }),
        std::runtime_error);
    auto kept = database_->prepare("SELECT COUNT(*) FROM turns WHERE session_key=?1", key);
    ASSERT_TRUE(kept.step());
    EXPECT_EQ(kept.integer(0), 1);
}

TEST_F(ChatSyncTest, IncomingAttachmentsOverLimitSkip) {
    make_chat_session(*database_, "f1", "s-file", "Chat");
    const auto key = session_key_for(*database_, "f1", "s-file");
    const auto add_file = [&](std::string name, std::size_t size) {
        sync_apply_chat_record(*database_, {
            {"kind", "chat.add_file"},
            {"chat", {"f1", "s-file"}},
            {"label", "Chat"},
            {"name", std::move(name)},
            {"text", std::string(size, 'x')},
        });
    };
    add_file("limit.txt", 256 * 1024);
    EXPECT_THROW(add_file("over.txt", 256 * 1024 + 1), std::runtime_error);
    auto files = database_->prepare(
        "SELECT name FROM session_files WHERE session_key=?1", key);
    ASSERT_TRUE(files.step());
    EXPECT_EQ(files.text(0), "limit.txt");
    EXPECT_FALSE(files.step());

    (void)note_create(*database_, std::nullopt, NoteItemKind::note, "Source");
    const auto add_note = [&](std::size_t size) {
        sync_apply_chat_record(*database_, {
            {"kind", "chat.add_note"},
            {"chat", {"f1", "s-file"}},
            {"label", "Chat"},
            {"path", {"Source"}},
            {"name", "Source"},
            {"text", std::string(size, 'x')},
        });
    };
    EXPECT_THROW(add_note(256 * 1024 + 1), std::runtime_error);
    add_note(256 * 1024);
    auto notes = database_->prepare(
        "SELECT length(text) FROM session_notes WHERE session_key=?1", key);
    ASSERT_TRUE(notes.step());
    EXPECT_EQ(notes.integer(0), 256 * 1024);
    EXPECT_FALSE(notes.step());
    // One notice for the file and one for the note.
    auto entries = database_->prepare("SELECT COUNT(*) FROM entries WHERE session_key=?1", key);
    ASSERT_TRUE(entries.step());
    EXPECT_EQ(entries.integer(0), 2);
}

// --- Worker transport script -------------------------------------------------

nlohmann::json batch_success(std::size_t count) {
    nlohmann::json results = nlohmann::json::array();
    for (std::size_t i = 0; i < count; ++i) {
        results.push_back({{"success", true}, {"results", nlohmann::json::array()}});
    }
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json table_list_success(std::vector<std::string> names) {
    nlohmann::json rows = nlohmann::json::array();
    for (auto& name : names) rows.push_back({{"name", std::move(name)}});
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"success", true}, {"results", std::move(rows)}});
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json queue_rows_success(std::vector<D1Client::Row> rows) {
    nlohmann::json list = nlohmann::json::array();
    for (auto& row : rows) {
        list.push_back(
            {{"rowid", row.rowid}, {"piece", row.piece}, {"pieces", row.pieces},
             {"record", row.record}});
    }
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"success", true}, {"results", std::move(list)}});
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json count_success(std::int64_t count) {
    nlohmann::json rows = nlohmann::json::array({{{"count", count}}});
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"success", true}, {"results", std::move(rows)}});
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json error_response(std::string message) {
    return {
        {"success", false},
        {"result", nlohmann::json::array()},
        {"errors", nlohmann::json::array({{{"message", std::move(message)}}})},
    };
}

struct ScriptedTransport {
    std::vector<std::pair<long, std::string>> responses;
    std::vector<std::string> bodies;
    std::size_t index = 0;

    std::pair<long, std::string> operator()(
        const std::string&, const std::string& body, const std::vector<std::string>&) {
        bodies.push_back(body);
        if (index >= responses.size()) return {500, "{\"success\":false}"};
        return responses[index++];
    }
};

D1SyncWorker::Config worker_config(
    const std::filesystem::path& path,
    std::shared_ptr<ScriptedTransport> scripted) {
    D1SyncWorker::Config config;
    config.database_path = path;
    config.database_password = {};
    config.vault = "Test";
    config.machine = std::string(sync_machine);
    config.desktop_host = true;
    config.transport = [scripted](
                           const std::string& url, const std::string& body,
                           const std::vector<std::string>& headers) {
        return (*scripted)(url, body, headers);
    };
    return config;
}

// Execute the client's SQL against SQLite so uniqueness, interleaving, and row
// ID reuse are covered by the database instead of a scripted response.
struct SqliteD1 {
    Database queue;
    std::vector<nlohmann::json> inserts;
    std::vector<nlohmann::json> deletes;
    std::size_t piece_reads{};
    bool lose_insert_response{};
    bool fail_ack{};
    bool fail_peer_final{};
    bool reduce_page{};

    explicit SqliteD1(const std::filesystem::path& path)
        : queue(path, Database::Mode::read_write_create) {}

    D1Client::Transport transport() {
        return [this](const std::string&, const std::string& body,
                   const std::vector<std::string>&) -> std::pair<long, std::string> {
            const auto batch = nlohmann::json::parse(body).at("batch");
            auto response = batch_success(batch.size());
            for (std::size_t i = 0; i < batch.size(); ++i) {
                const auto& statement = batch[i];
                const auto sql = statement.at("sql").get<std::string>();
                const auto& params = statement.at("params");
                try {
                    const bool insert = sql.starts_with("INSERT INTO");
                    const bool acknowledge = sql.starts_with("DELETE FROM") && sql.find("op_id IN") != std::string::npos;
                    if (insert) {
                        inserts.push_back(statement);
                        if (fail_peer_final && sql.find("/peer\"") != std::string::npos
                            && params[1] == params[2]) {
                            fail_peer_final = false;
                            response["result"][i] = {{"success", false}, {"error", "down"}};
                            continue;
                        }
                    }
                    if (acknowledge && std::exchange(fail_ack, false))
                        return {503, error_response("ack down").dump()};
                    if (sql.starts_with("SELECT seq") && std::exchange(reduce_page, false))
                        return {400, error_response("D1 query result exceeded").dump()};
                    auto query = queue.prepare(sql);
                    for (std::size_t param = 0; param < params.size(); ++param)
                        query.bind(static_cast<int>(param + 1), params[param].get<std::string>());
                    auto& rows = response["result"][i]["results"];
                    if (sql.starts_with("SELECT name")) {
                        while (query.step()) rows.push_back({{"name", query.text(0)}});
                    } else if (sql.starts_with("SELECT seq")) {
                        while (query.step()) {
                            nlohmann::json row{{"seq", query.integer(0)}, {"op_id", query.text(1)},
                                {"pieces", query.integer(2)}, {"record", nullptr}};
                            if (!query.is_null(3)) row["record"] = query.text(3);
                            rows.push_back(std::move(row));
                        }
                    } else if (sql.starts_with("SELECT pieces")) {
                        ++piece_reads;
                        while (query.step())
                            rows.push_back({{"pieces", query.integer(0)}, {"record", query.text(1)}});
                    } else if (sql.starts_with("SELECT rowid")) {
                        while (query.step()) {
                            rows.push_back({{"rowid", query.integer(0)}, {"piece", query.integer(1)},
                                {"pieces", query.integer(2)}, {"record", query.text(3)}});
                        }
                    } else {
                        query.run();
                    }
                    if (acknowledge) deletes.push_back(statement);
                } catch (const storage::SqliteError& error) {
                    return {400, error_response(error.what()).dump()};
                }
            }
            if (batch[0]["sql"].get_ref<const std::string&>().starts_with("INSERT")
                && std::exchange(lose_insert_response, false))
                return {503, error_response("response lost").dump()};
            return {200, response.dump()};
        };
    }

    D1SyncWorker::Config config(const std::filesystem::path& path, std::string machine = "mac") {
        return {
            .database_path = path, .vault = "Test", .machine = std::move(machine),
            .desktop_host = true, .transport = transport(), .use_v2 = true,
        };
    }

    std::vector<std::string> ids(std::string_view table) {
        std::vector<std::string> result;
        auto query = queue.prepare("SELECT op_id FROM " + sync_escape_identifier(table) + " ORDER BY seq");
        while (query.step()) result.push_back(query.text(0));
        return result;
    }
};

std::string folder_record(std::string_view name) {
    return sync_make_record("note.create", {{"path", {name}}, {"item", "folder"}});
}

std::string remote_turn() {
    return sync_make_record("chat.entries", {
        {"chat", {"f1", "remote-turn"}}, {"label", "Remote"}, {"state", 1},
        {"entries", {
            {{"kind", 0}, {"participant_id", "u"}, {"display_name", "U"},
             {"addressed_to", "a"}, {"addressed_to_name", "A"}, {"text", "Prompt"}, {"status", 0}},
            {{"kind", 1}, {"participant_id", "a"}, {"display_name", "A"},
             {"text", "Reply"}, {"status", 0}},
        }},
    });
}

TEST(D1WorkerTest, DesktopCapacityFailureRetainsOutboxAndAcknowledgesIncoming) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    const auto first = sync_make_record("note.create", {{"path", {"A"}}, {"item", "folder"}});
    const auto second = sync_make_record("note.create", {{"path", {"B"}}, {"item", "folder"}});
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
        sync_queue_append(database, first);
        sync_queue_append(database, second);
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, table_list_success({"q/Test/mac", "q/Test/other", "q2/Test/mac", "q2/Test/other"}).dump()},
        {200, error_response("Exceeded maximum DB size.").dump()},
        {200, queue_rows_success({{7, 1, 1,
            sync_make_record("note.create", {{"path", {"Incoming"}}, {"item", "folder"}})}}).dump()},
        {200, batch_success(1).dump()},
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, batch_success(1).dump()},
        {200, batch_success(1).dump()},
        {200, queue_rows_success({}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    EXPECT_FALSE(worker.stopped());
    {
        Database database(path, Database::Mode::read_only);
        const auto pending = sync_queue_list(database);
        ASSERT_EQ(pending.size(), 2u);
        EXPECT_EQ(pending[0].record, first);
        EXPECT_EQ(pending[1].record, second);
        EXPECT_TRUE(sync_note_find(database, {"Incoming"}));
    }
    ASSERT_EQ(scripted->bodies.size(), 4u);
    EXPECT_NE(scripted->bodies[1].find("INSERT INTO"), std::string::npos);
    EXPECT_NE(scripted->bodies[2].find("SELECT rowid"), std::string::npos);
    const auto ack = nlohmann::json::parse(scripted->bodies[3])["batch"][0];
    EXPECT_EQ(ack["sql"], "DELETE FROM \"q/Test/mac\" WHERE rowid<=?1");
    EXPECT_EQ(ack["params"], nlohmann::json::array({"7"}));
    for (const auto& body : scripted->bodies) {
        EXPECT_EQ(body.find("DROP TABLE"), std::string::npos);
        EXPECT_EQ(body.find("q2/"), std::string::npos);
    }

    worker.run_once();
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_TRUE(sync_queue_list(database).empty());
    }
}

TEST(D1WorkerTest, NoRecipientsDeletesWithoutRequests) {
    for (const bool v2 : {false, true}) {
        SCOPED_TRACE(v2);
        test::TestWorkspace workspace;
        const auto path = workspace.root() / "w.sqlite3";
        create_empty_workspace_session_database(path);
        std::string id;
        {
            Database database(path, Database::Mode::read_write);
            add_d1_key(database);
            storage::SqliteTransaction transaction(database);
            ensure_sync_schema(database, v2);
            sync_queue_append(database, folder_record("Local"));
            id = sync_queue_list(database).front().op_id;
            transaction.commit();
        }
        SqliteD1 d1(workspace.root() / "d1.sqlite3");
        D1Client client({"a", "d", "t"}, d1.transport());
        ASSERT_FALSE(client.create_queue_table(sync_queue_table("Test", "mac", v2), v2));
        auto config = d1.config(path);
        config.use_v2 = v2;
        D1SyncWorker worker(std::move(config));
        worker.run_once();

        Database database(path, Database::Mode::read_only);
        EXPECT_FALSE(worker.stopped());
        EXPECT_TRUE(sync_queue_list(database).empty());
        EXPECT_TRUE(d1.inserts.empty());
        if (v2) EXPECT_TRUE(sync_seen(database, id));
        else EXPECT_FALSE(sync_has_receipt_schema(database));
    }
}

TEST(D1WorkerTest, MissingOwnTableHaltsAndRetains) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
        sync_queue_append(database, sync_make_record("note.create", {{"path", {"A"}}}));
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, table_list_success({"q/Test/other"}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    EXPECT_TRUE(worker.stopped());
    Database database(path, Database::Mode::read_only);
    EXPECT_EQ(sync_queue_list(database).size(), 1u);
}

TEST(D1WorkerTest, SplitSetAssemblesAcrossPagesAndSkipsBroken) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
    }
    const auto make_note = [](std::string_view name) {
        return sync_make_record("note.create", {{"path", {name}}, {"item", "folder"}});
    };
    const std::string whole = make_note("A");
    const auto first = whole.substr(0, whole.size() / 2);
    const auto second = whole.substr(first.size());
    std::vector<D1Client::Row> page;
    for (int i = 1; i <= 48; ++i) page.push_back({i, 2, 2, "orphan"});
    page.push_back({49, 1, 1, make_note("B")});
    page.push_back({50, 1, 2, first});
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, queue_rows_success(page).dump()},
        {200, batch_success(1).dump()}, // Acknowledge through 49, then read after 50.
        {200, queue_rows_success({
            {51, 2, 2, second},
            {52, 1, 3, "abandoned set"},
            {53, 1, 1, make_note("C")},
            {54, 1, 2, first},
        }).dump()},
        {200, batch_success(1).dump()}, // Leave 54 for the next loop.
        {200, queue_rows_success({
            {54, 1, 2, first}, {55, 2, 2, second},
        }).dump()},
        {200, batch_success(1).dump()},
    };
    auto params = [&](std::size_t request) {
        return nlohmann::json::parse(scripted->bodies.at(request))["batch"][0]["params"];
    };
    int notices = 0;
    auto config = worker_config(path, scripted);
    config.on_notes_changed = [&] { ++notices; };
    D1SyncWorker worker(std::move(config));
    worker.run_once();
    ASSERT_EQ(scripted->bodies.size(), 4u);
    EXPECT_EQ(params(1), nlohmann::json::array({"49"}));
    EXPECT_EQ(params(2), nlohmann::json::array({"50", "50"}));
    EXPECT_EQ(params(3), nlohmann::json::array({"53"}));
    EXPECT_EQ(notices, 2);
    {
        Database database(path, Database::Mode::read_only);
        for (const auto* name : {"A", "B", "C"})
            EXPECT_TRUE(sync_note_find(database, {name})) << name;
    }
    worker.run_once();
    ASSERT_EQ(scripted->bodies.size(), 6u);
    EXPECT_EQ(params(4), nlohmann::json::array({"0", "50"}));
    EXPECT_EQ(params(5), nlohmann::json::array({"55"}));
    EXPECT_EQ(notices, 3);
}

TEST(D1WorkerTest, MissingOwnTableDuringReadHalts) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, error_response("no such table: q/Test/mac").dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    EXPECT_TRUE(worker.stopped());
    worker.run_once();
    EXPECT_EQ(scripted->bodies.size(), 1u);
}

TEST(D1WorkerTest, OversizedResultHalvesLimitAndResets) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, error_response("D1 query result exceeded the maximum response size limit of 50 MB").dump()},
        {200, queue_rows_success({}).dump()},
        {200, queue_rows_success({}).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    worker.run_once();
    ASSERT_EQ(scripted->bodies.size(), 3u);
    const auto limit = [&](std::size_t i) {
        return nlohmann::json::parse(scripted->bodies.at(i))["batch"][0]["params"][1];
    };
    EXPECT_EQ(limit(0), "50");
    EXPECT_EQ(limit(1), "25");
    EXPECT_EQ(limit(2), "50");
}

TEST(D1WorkerTest, SplitSendRetriesFromFirstPiece) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    const auto record = sync_make_record("note.create",
        {{"path", {"A"}}, {"item", "folder"}, {"text", std::string(sync_piece_bytes, 'x')}});
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
        sync_queue_append(database, record);
    }
    SqliteD1 d1(workspace.root() / "d1.sqlite3");
    D1Client client({"a", "d", "t"}, d1.transport());
    for (const auto* name : {"q2/Test/mac", "q2/TEST/other", "q2/Test/peer", "q2/Elsewhere/other"})
        ASSERT_FALSE(client.create_queue_table(name, true));
    d1.fail_peer_final = true;
    D1SyncWorker worker(d1.config(path));
    worker.run_once();

    std::string id;
    {
        Database database(path, Database::Mode::read_only);
        const auto pending = sync_queue_list(database);
        ASSERT_EQ(pending.size(), 1u);
        id = pending.front().op_id;
        EXPECT_EQ(pending.front().record, record);
        EXPECT_TRUE(sync_seen(database, id));
    }
    EXPECT_EQ(d1.ids("q2/TEST/other"), (std::vector{id, id}));
    EXPECT_EQ(d1.ids("q2/Test/peer"), (std::vector{id}));
    ASSERT_EQ(d1.inserts.size(), 4u);

    worker.run_once();
    ASSERT_EQ(d1.inserts.size(), 8u);
    for (std::size_t i = 0; i < 4; ++i) EXPECT_TRUE(d1.inserts[i] == d1.inserts[i + 4]);
    EXPECT_EQ(d1.inserts[4]["params"][1], "1");
    EXPECT_EQ(d1.inserts[4]["params"][0], id);
    EXPECT_EQ(d1.ids("q2/TEST/other"), (std::vector{id, id}));
    EXPECT_EQ(d1.ids("q2/Test/peer"), (std::vector{id, id}));
    EXPECT_TRUE(d1.ids("q2/Elsewhere/other").empty());
    Database database(path, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(database).empty());
    EXPECT_TRUE(sync_seen(database, id));
}

TEST(D1WorkerTest, ObsoleteFolderRecordsAreAcknowledgedWithoutAnEvent) {
    test::TestWorkspace workspace;
    const auto path = test::import_test_database(workspace.root());
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
    }
    auto store = WorkspaceConfigStore::open(path);
    SqliteD1 d1(workspace.root() / "d1.sqlite3");
    D1Client client({"a", "d", "t"}, d1.transport());
    const std::string inbox = "q2/Test/mac";
    const std::string legacy = "q/Test/mac";
    const std::string id(32, 'a');
    const nlohmann::json fields{
        {"chat", {"lobby", "old-chat"}}, {"label", "Old chat"}, {"path", {"Missing folder"}},
    };
    ASSERT_FALSE(client.create_queue_table(inbox, true));
    ASSERT_FALSE(client.create_queue_table(legacy));
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 1, sync_make_record("chat.add_folder", fields), id));
    ASSERT_FALSE(client.insert_piece({legacy}, 1, 1, sync_make_record("chat.detach_folder", fields)));

    auto config = d1.config(path);
    config.store = store.get();
    D1SyncWorker worker(std::move(config));
    worker.run_once();

    EXPECT_TRUE(d1.ids(inbox).empty());
    EXPECT_FALSE(d1.queue.prepare("SELECT 1 FROM \"q/Test/mac\"").step());
    EXPECT_TRUE(store->list_events().empty());
    Database database(path, Database::Mode::read_only);
    EXPECT_TRUE(sync_seen(database, id));
    EXPECT_FALSE(database.prepare("SELECT 1 FROM sessions WHERE session_id='old-chat'").step());
}

TEST(D1WorkerTest, InvalidRecordsSkippedAndAcked) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
    }
    SqliteD1 d1(workspace.root() / "d1.sqlite3");
    D1Client client({"a", "d", "t"}, d1.transport());
    const std::string inbox = "q2/Test/mac";
    const std::string mismatched_id(32, 'a');
    const std::string missing_id(32, 'b');
    const std::string valid_id(32, 'c');
    ASSERT_FALSE(client.create_queue_table(inbox, true));
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 3, "wrong piece count", mismatched_id));
    ASSERT_FALSE(client.insert_piece({inbox}, 2, 2, "final piece", mismatched_id));
    ASSERT_FALSE(client.insert_piece({inbox}, 2, 2, "final piece", missing_id));
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 1, folder_record("After invalid"), valid_id));

    D1SyncWorker worker(d1.config(path));
    worker.run_once();
    EXPECT_TRUE(d1.ids(inbox).empty());
    ASSERT_EQ(d1.deletes.size(), 1u);
    EXPECT_EQ(d1.deletes[0]["params"], nlohmann::json::array({mismatched_id, missing_id, valid_id}));
    Database database(path, Database::Mode::read_only);
    EXPECT_FALSE(sync_seen(database, mismatched_id));
    EXPECT_FALSE(sync_seen(database, missing_id));
    EXPECT_TRUE(sync_seen(database, valid_id));
    EXPECT_TRUE(sync_note_find(database, {"After invalid"}));
    EXPECT_TRUE(sync_queue_list(database).empty());
}

TEST(D1WorkerTest, UnorderedReadAppliesAndAcknowledgesNothing) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, queue_rows_success({
            {-1, 1, 1, sync_make_record("note.create", {{"path", {"A"}}, {"item", "note"}})},
            {6, 1, 1, sync_make_record("note.create", {{"path", {"B"}}, {"item", "note"}})},
        }).dump()},
    };
    D1SyncWorker worker(worker_config(path, scripted));
    worker.run_once();
    EXPECT_EQ(scripted->bodies.size(), 1u); // No DELETE request.
    Database database(path, Database::Mode::read_only);
    EXPECT_FALSE(sync_note_find(database, {"A"}));
    EXPECT_FALSE(sync_note_find(database, {"B"}));
}

TEST(D1WorkerTest, AckFailureEndsPassAndRetainsForReplay) {
    test::TestWorkspace workspace;
    const auto sender_path = workspace.root() / "sender.sqlite3";
    const auto receiver_path = workspace.root() / "receiver.sqlite3";
    for (const auto& path : {sender_path, receiver_path}) {
        create_empty_workspace_session_database(path);
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
        database.prepare("INSERT INTO forums (forum_id) VALUES ('f1')").run();
        if (path == sender_path) sync_queue_append(database, remote_turn());
    }
    SqliteD1 d1(workspace.root() / "d1.sqlite3");
    D1Client client({"a", "d", "t"}, d1.transport());
    const std::string inbox = "q2/Test/other";
    ASSERT_FALSE(client.create_queue_table("q2/Test/mac", true));
    ASSERT_FALSE(client.create_queue_table(inbox, true));
    D1SyncWorker sender(d1.config(sender_path));
    D1SyncWorker receiver(d1.config(receiver_path, "other"));

    const std::string multipart_id(32, 'a');
    const std::string incomplete_id(32, 'b');
    const auto multipart = sync_make_record("note.create",
        {{"path", {"Parent", "Child"}}, {"item", "folder"}});
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 2, multipart.substr(0, 16), multipart_id));
    d1.lose_insert_response = true;
    sender.run_once();
    std::string turn_id;
    {
        Database database(sender_path, Database::Mode::read_only);
        const auto pending = sync_queue_list(database);
        ASSERT_EQ(pending.size(), 1u);
        turn_id = pending.front().op_id;
        EXPECT_EQ(pending.front().record, remote_turn());
    }
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 1, folder_record("Parent"), std::string(32, 'c')));
    ASSERT_FALSE(client.insert_piece({inbox}, 2, 2, multipart.substr(16), multipart_id));
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 2, "incomplete", incomplete_id));

    d1.reduce_page = true;
    d1.fail_ack = true;
    receiver.run_once();
    ASSERT_EQ(d1.piece_reads, 2u);
    EXPECT_TRUE(d1.deletes.empty());
    EXPECT_EQ(d1.ids(inbox), (std::vector{multipart_id, turn_id, std::string(32, 'c'), multipart_id, incomplete_id}));

    sender.run_once();
    receiver.run_once();
    EXPECT_EQ(d1.piece_reads, 2u);
    ASSERT_EQ(d1.deletes.size(), 1u);
    EXPECT_EQ(d1.deletes.front()["params"],
        nlohmann::json::array({turn_id, std::string(32, 'c'), multipart_id}));
    EXPECT_EQ(d1.ids(inbox), (std::vector{incomplete_id}));

    // An overlapping retry leaves just a final piece for an already seen ID.
    // Receiving that header must avoid fetching its incomplete contents.
    ASSERT_FALSE(client.insert_piece({inbox}, 2, 2, multipart.substr(16), multipart_id));
    receiver.run_once();
    EXPECT_EQ(d1.piece_reads, 2u);
    ASSERT_EQ(d1.deletes.size(), 2u);
    EXPECT_EQ(d1.deletes.back()["params"], nlohmann::json::array({multipart_id}));
    EXPECT_EQ(d1.ids(inbox), (std::vector{incomplete_id}));

    // Empty the queue, then exercise reuse of seq=1 (no receive cursor).
    ASSERT_FALSE(client.delete_operations(inbox, {incomplete_id}));
    const std::string later_id(32, 'd');
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 1, folder_record("After reuse"), later_id));
    {
        auto seq = d1.queue.prepare("SELECT seq FROM " + sync_escape_identifier(inbox));
        ASSERT_TRUE(seq.step());
        EXPECT_EQ(seq.integer(0), 1);
    }
    receiver.run_once();
    EXPECT_EQ(d1.deletes.back()["params"], nlohmann::json::array({later_id}));
    EXPECT_TRUE(d1.ids(inbox).empty());
    Database database(receiver_path, Database::Mode::read_only);
    auto turns = database.prepare("SELECT count(*) FROM turns");
    ASSERT_TRUE(turns.step());
    EXPECT_EQ(turns.integer(0), 1);
    EXPECT_TRUE(sync_note_find(database, {"Parent", "Child"}));
    EXPECT_TRUE(sync_note_find(database, {"After reuse"}));
    EXPECT_TRUE(sync_seen(database, turn_id));
    EXPECT_TRUE(sync_queue_list(database).empty());
    Database sender_db(sender_path, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(sender_db).empty());
    EXPECT_TRUE(sync_seen(sender_db, turn_id));
}

TEST(D1WorkerTest, ReservationFailurePausesAndRetainsRows) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
        database.prepare("INSERT INTO forums (forum_id) VALUES ('f1')").run();
    }
    RuntimeSettings settings;
    LiveSessionManager live(settings, [](const FullSessionId&, std::shared_ptr<WakeNotifier>, std::uint64_t) -> OpenedSession {
        throw std::runtime_error("no opener in test");
    });
    SqliteD1 d1(workspace.root() / "d1.sqlite3");
    D1Client client({"a", "d", "t"}, d1.transport());
    const std::string inbox = "q2/Test/mac";
    const std::string before(32, 'a');
    const std::string held(32, 'b');
    const std::string later(32, 'c');
    ASSERT_FALSE(client.create_queue_table(inbox, true));
    ASSERT_FALSE(client.create_queue_table("q2/Test/other", true));
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 1, folder_record("Before"), before));
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 1, remote_turn(), held));
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 1, folder_record("Later"), later));
    auto config = d1.config(path);
    config.live_sessions = &live;
    D1SyncWorker worker(std::move(config));
    {
        auto hold = live.reserve_for_sync({"f1", "remote-turn"}, std::chrono::seconds(1));
        ASSERT_TRUE(std::holds_alternative<LiveSessionMaintenanceReservation>(hold));
        worker.run_once();
        EXPECT_EQ(d1.ids(inbox), (std::vector{held, later}));
        ASSERT_EQ(d1.deletes.size(), 1u);
        EXPECT_EQ(d1.deletes[0]["params"], nlohmann::json::array({before}));
        Database database(path, Database::Mode::read_write);
        EXPECT_TRUE(sync_note_find(database, {"Before"}));
        EXPECT_FALSE(sync_note_find(database, {"Later"}));
        EXPECT_FALSE(sync_seen(database, held));
        storage::SqliteTransaction transaction(database);
        sync_queue_append(database, folder_record("Local"));
        transaction.commit();
        worker.run_once();
        EXPECT_EQ(d1.ids("q2/Test/other").size(), 1u);
        EXPECT_EQ(d1.ids(inbox), (std::vector{held, later}));
    }
    worker.run_once();
    EXPECT_TRUE(d1.ids(inbox).empty());
    Database database(path, Database::Mode::read_only);
    EXPECT_TRUE(sync_seen(database, held));
    EXPECT_TRUE(sync_note_find(database, {"Later"}));
    EXPECT_TRUE(sync_queue_list(database).empty());
}

TEST(D1WorkerTest, BusyNoteWriteRetriesWithoutAcknowledging) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "busy.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        add_d1_key(database);
        storage::SqliteTransaction transaction(database);
        ensure_sync_schema(database, true);
        transaction.commit();
    }
    SqliteD1 d1(workspace.root() / "d1.sqlite3");
    D1Client client({"a", "d", "t"}, d1.transport());
    const std::string inbox = "q2/Test/mac";
    const std::string id(32, '1');
    ASSERT_FALSE(client.create_queue_table(inbox, true));
    ASSERT_FALSE(client.insert_piece({inbox}, 1, 1, folder_record("Retry"), id));
    D1SyncWorker worker(d1.config(path));
    {
        Database locking_db(path, Database::Mode::read_write);
        storage::SqliteTransaction lock(locking_db);
        worker.run_once();
        EXPECT_FALSE(worker.stopped());
        EXPECT_EQ(d1.ids(inbox), (std::vector{id}));
        EXPECT_FALSE(sync_seen(locking_db, id));
        EXPECT_FALSE(sync_note_find(locking_db, {"Retry"}));
    }
    worker.run_once();
    EXPECT_TRUE(d1.ids(inbox).empty());
    Database database(path, Database::Mode::read_only);
    EXPECT_TRUE(sync_seen(database, id));
    EXPECT_TRUE(sync_note_find(database, {"Retry"}));
}

TEST(D1WorkerTest, ProtectedUpgradePreservesOutboxAndProvisionsLegacyPeersOnlyOnce) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "protected.sqlite3";
    const auto peer = workspace.root() / "old-peer.sqlite3";
    const std::string password = "protected test vault";
    create_empty_workspace_session_database(path, password);
    create_empty_workspace_session_database(peer);
    const auto first = folder_record("From pending work");
    const auto second = folder_record("Also pending");
    {
        Database database(path, Database::Mode::read_write, password);
        add_d1_key(database);
        sync_queue_append(database, first);
        sync_queue_append(database, second);
        EXPECT_FALSE(sync_has_receipt_schema(database));
    }
    {
        Database database(peer, Database::Mode::read_write);
        add_d1_key(database);
    }
    SqliteD1 d1(workspace.root() / "d1.sqlite3");
    D1Client client({"a", "d", "t"}, d1.transport());
    ASSERT_FALSE(client.create_queue_table("q/Test/mac"));
    ASSERT_FALSE(client.create_queue_table("q/TEST/peer"));
    ASSERT_FALSE(client.create_queue_table("q/Elsewhere/ignored"));
    auto config = d1.config(path);
    config.database_password = password;
    d1.lose_insert_response = true;
    {
        D1SyncWorker worker(config);
        worker.run_once();
        EXPECT_FALSE(worker.stopped());
    }
    std::vector<QueuedRecord> original;
    {
        Database database(path, Database::Mode::read_only, password);
        original = sync_queue_list(database);
        ASSERT_EQ(original.size(), 2u);
        EXPECT_EQ(original[0].record, first);
        EXPECT_EQ(original[1].record, second);
        EXPECT_EQ(original[0].op_id.size(), 32u);
        EXPECT_EQ(original[1].op_id.size(), 32u);
        EXPECT_NE(original[0].op_id, original[1].op_id);
        for (const auto& row : original) {
            EXPECT_EQ(row.op_id.find_first_not_of("0123456789abcdef"), std::string::npos);
            EXPECT_TRUE(sync_seen(database, row.op_id));
        }
    }
    EXPECT_EQ(d1.ids("q2/TEST/peer"), (std::vector{original[0].op_id}));
    EXPECT_TRUE(d1.ids("q2/Test/mac").empty());

    const std::string new_id(32, 'a');
    ASSERT_FALSE(client.insert_piece({"q/Test/mac"}, 1, 1, folder_record("Legacy parent")));
    ASSERT_FALSE(client.insert_piece({"q2/Test/mac"}, 1, 1, sync_make_record("note.create",
        {{"path", {"Legacy parent", "V2 child"}}, {"item", "folder"}}), new_id));
    D1SyncWorker reopened(config);
    d1.lose_insert_response = true;
    reopened.run_once();
    {
        Database database(path, Database::Mode::read_only, password);
        const auto pending = sync_queue_list(database);
        ASSERT_EQ(pending.size(), original.size());
        for (std::size_t i = 0; i < original.size(); ++i) {
            EXPECT_EQ(pending[i].op_id, original[i].op_id);
            EXPECT_EQ(pending[i].record, original[i].record);
        }
        EXPECT_TRUE(sync_note_find(database, {"Legacy parent", "V2 child"}));
        EXPECT_TRUE(sync_seen(database, new_id));
        auto receipts = database.prepare("SELECT count(*) FROM sync_seen");
        ASSERT_TRUE(receipts.step());
        EXPECT_EQ(receipts.integer(0), 3);
    }
    reopened.run_once();
    D1SyncWorker upgraded_peer(d1.config(peer, "peer"));
    upgraded_peer.run_once();
    {
        Database database(peer, Database::Mode::read_only);
        EXPECT_TRUE(sync_note_find(database, {"From pending work"}));
        EXPECT_TRUE(sync_note_find(database, {"Also pending"}));
        for (const auto& row : original) EXPECT_TRUE(sync_seen(database, row.op_id));
    }
    ASSERT_FALSE(client.drop_table("q2/Test/mac"));
    D1SyncWorker missing(config);
    missing.run_once();
    EXPECT_TRUE(missing.stopped());
    const auto [tables, error] = client.list_tables();
    ASSERT_FALSE(error);
    EXPECT_NE(std::ranges::find(tables, "q/Test/mac"), tables.end());
    EXPECT_NE(std::ranges::find(tables, "q/Elsewhere/ignored"), tables.end());
    EXPECT_EQ(std::ranges::find(tables, "q2/Test/mac"), tables.end());
    EXPECT_EQ(std::ranges::find(tables, "q2/Elsewhere/ignored"), tables.end());
}

TEST(D1WorkerTest, EvictionDropsAtMostOneEligibleTable) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "w.sqlite3";
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            "system/keys/api_key_1/config.toml", d1_key_toml()).run();
        sync_queue_append(database, sync_make_record("note.create", {{"path", {"A"}}}));
    }
    auto scripted = std::make_shared<ScriptedTransport>();
    scripted->responses = {
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, error_response("Exceeded maximum DB size.").dump()},
        {200, table_list_success(
            {"q/Test/mac", "q/Test/other", "q/Archive/other", "q/Archive/MAC", "config"}).dump()},
        {200, nlohmann::json{
            {"success", true},
            {"result", {count_success(10)["result"][0], count_success(20)["result"][0]}}
        }.dump()},
        {200, batch_success(1).dump()},
        {200, queue_rows_success({}).dump()},
        {200, table_list_success({"q/Test/mac", "q/Test/other"}).dump()},
        {200, batch_success(1).dump()},
        {200, queue_rows_success({}).dump()},
    };
    auto config = worker_config(path, scripted);
    config.desktop_host = false;
    D1SyncWorker worker(std::move(config));
    worker.run_once(); // Triggers eviction, retains work.
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_EQ(sync_queue_list(database).size(), 1u);
    }
    ASSERT_GE(scripted->bodies.size(), 5u);
    const auto counts = nlohmann::json::parse(scripted->bodies[3])["batch"];
    ASSERT_EQ(counts.size(), 2u);
    EXPECT_NE(counts[0]["sql"].get<std::string>().find("q/Test/other"), std::string::npos);
    EXPECT_NE(counts[1]["sql"].get<std::string>().find("q/Archive/other"), std::string::npos);
    EXPECT_NE(scripted->bodies[4].find("q/Archive/other"), std::string::npos);
    worker.run_once(); // Sends and deletes.
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_TRUE(sync_queue_list(database).empty());
    }
}

} // namespace
} // namespace cha
