#include "support/test_workspace.h"
#include "storage/session_repository.h"
#include "storage/sqlite_storage.h"
#include "storage/workspace_session_database.h"
#include "workspace/workspace_config_store.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace cha {
namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;

class VaultProcess {
public:
    explicit VaultProcess(const std::filesystem::path& database) {
        int input[2]{-1, -1};
        int output[2]{-1, -1};
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions)) throw std::runtime_error("spawn actions");
        try {
            if (pipe(input) || pipe(output)) throw std::runtime_error("pipe");
            for (int fd : {input[0], input[1], output[0], output[1]}) {
                if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) throw std::runtime_error("fcntl");
            }
            if (posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO)
                || posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO)) {
                throw std::runtime_error("spawn descriptors");
            }
            std::string path = database.string();
            char* arguments[]{const_cast<char*>(CHA_MULTI_TEST_HELPER), path.data(), nullptr};
            if (posix_spawn(&process_, CHA_MULTI_TEST_HELPER, &actions, nullptr, arguments, environ)) {
                throw std::runtime_error("spawn helper");
            }
        } catch (...) {
            posix_spawn_file_actions_destroy(&actions);
            for (int fd : {input[0], input[1], output[0], output[1]}) if (fd != -1) (void)close(fd);
            throw;
        }
        posix_spawn_file_actions_destroy(&actions);
        (void)close(input[0]);
        (void)close(output[1]);
        input_ = input[1];
        output_ = output[0];
    }
    ~VaultProcess() { terminate(); (void)close(input_); (void)close(output_); }
    VaultProcess(const VaultProcess&) = delete;
    VaultProcess& operator=(const VaultProcess&) = delete;
    void terminate() {
        if (process_ == -1) return;
        (void)kill(process_, SIGKILL);
        while (waitpid(process_, nullptr, 0) == -1 && errno == EINTR) {}
        process_ = -1;
    }
    void send(const Json& request) {
        std::string data = request.dump() + "\n";
        std::string_view remaining(data);
        while (!remaining.empty()) {
            const auto n = write(input_, remaining.data(), remaining.size());
            if (n == -1 && errno == EINTR) continue;
            if (n <= 0) throw std::runtime_error("helper write");
            remaining.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    Json receive() {
        pollfd fd{output_, POLLIN, 0};
        int ready;
        do { ready = poll(&fd, 1, 10000); } while (ready == -1 && errno == EINTR);
        if (ready <= 0) throw std::runtime_error("helper response timed out");
        std::string line;
        char byte;
        while (true) {
            const auto n = read(output_, &byte, 1);
            if (n == -1 && errno == EINTR) continue;
            if (n != 1) throw std::runtime_error("helper exited");
            if (byte == '\n') return Json::parse(line);
            line.push_back(byte);
        }
    }
    Json call(const Json& request) { send(request); return receive(); }
private:
    pid_t process_{-1};
    int input_{-1};
    int output_{-1};
};

class MultiProcessTest : public testing::Test {
protected:
    void SetUp() override {
        database_ = fixture_.root() / "multi.sqlite3";
        (void)test::import_test_database(fixture_.root(), database_);
    }
    std::unique_ptr<SessionRepository> repository(WorkspaceConfigStore& store) {
        return std::make_unique<SessionRepository>([&store] { return store.snapshot(); },
            store.database_path(), store.workspace_path(), store.welcome_path(),
            TemporarySessionSeed{{std::string(entrance_id), std::string(welcome_id)}, "Welcome"});
    }
    test::TestWorkspace fixture_;
    std::filesystem::path database_;
};

TEST_F(MultiProcessTest, SecondaryKeepsItsRoleWhenMainExitsAndAnotherMainStarts) {
    auto main = WorkspaceConfigStore::open(database_);
    VaultProcess secondary(database_);
    ASSERT_EQ(secondary.call({{"action", "open"}}).at("main"), false);
    EXPECT_EQ(secondary.call({{"action", "settings"}, {"name", "Refused"}}).at("secondary"), true);
    (void)main->apply_character_definition("guide", "New settings");
    EXPECT_EQ(secondary.call({{"action", "role"}}).at("name"), "Guide");
    main.reset();
    EXPECT_EQ(secondary.call({{"action", "role"}}).at("main"), false);
    VaultProcess next(database_);
    EXPECT_EQ(next.call({{"action", "open"}}).at("main"), true);
    EXPECT_EQ(secondary.call({{"action", "role"}}).at("main"), false);
}

TEST_F(MultiProcessTest, CreationHandoffSkipsCleanupAndPreventsAnotherOwner) {
    auto main = WorkspaceConfigStore::open(database_);
    auto sessions = repository(*main);
    const auto pending = sessions->create("lobby", "");
    VaultProcess secondary(database_);
    ASSERT_EQ(secondary.call({{"action", "open"}}).at("main"), false);
    sessions->validate(pending.identity);
    const Json claim{{"action", "claim"}, {"forum", "lobby"}, {"id", pending.identity.session_id}};
    EXPECT_EQ(secondary.call(claim).at("busy"), true);
    auto owned = sessions->prepare(pending.identity);
    EXPECT_EQ(secondary.call(claim).at("busy"), true);
    owned.lease.reset();
    EXPECT_EQ(secondary.call(claim).at("ok"), true);
    secondary.terminate();
    EXPECT_NO_THROW((void)sessions->prepare(pending.identity));
}

TEST_F(MultiProcessTest, IndependentSessionsWriteToOneDatabaseAndCrashReleasesOwnership) {
    auto main = WorkspaceConfigStore::open(database_);
    auto sessions = repository(*main);
    const auto a = sessions->create("lobby", "A");
    const auto b = sessions->create("lobby", "B");
    // Named sessions can be released immediately; no pending empty-session cleanup applies.
    sessions->release_pending();
    auto owned = sessions->prepare(a.identity);
    VaultProcess secondary(database_);
    ASSERT_EQ(secondary.call({{"action", "open"}}).at("main"), false);
    ASSERT_EQ(secondary.call({{"action", "claim"}, {"forum", "lobby"}, {"id", b.identity.session_id}}).at("ok"), true);
    SessionJournal journal(owned.database_path, owned.session_key);
    secondary.send({{"action", "write"}, {"text", "Secondary process"}});
    journal.record_entry(make_notice_entry(1, "Main process"));
    EXPECT_EQ(secondary.receive().at("ok"), true);
    EXPECT_EQ(sessions->history(a.identity).front().text, "Main process");
    EXPECT_EQ(sessions->history(b.identity).front().text, "Secondary process");
    EXPECT_THROW((void)sessions->prepare(b.identity), SessionBusyError);
    secondary.terminate();
    auto restored = sessions->prepare(b.identity);
    EXPECT_EQ(restored.restore.entries.front().text, "Secondary process");
}

TEST_F(MultiProcessTest, StartupDoesNotRepairAnotherOwnersUnfinishedTurn) {
    FullSessionId identity;
    {
        auto store = WorkspaceConfigStore::open(database_);
        auto sessions = repository(*store);
        identity = sessions->create("lobby", "Unfinished").identity;
    }
    VaultProcess owner(database_);
    ASSERT_TRUE(owner.call({{"action", "open"}}).at("ok"));
    ASSERT_TRUE(owner.call({{"action", "claim"}, {"forum", "lobby"}, {"id", identity.session_id}}).at("ok"));
    ASSERT_TRUE(owner.call({{"action", "begin"}}).at("ok"));
    auto secondary = WorkspaceConfigStore::open(database_);
    auto sessions = repository(*secondary);
    EXPECT_THROW((void)sessions->prepare(identity), SessionBusyError);
    EXPECT_EQ(sessions->history(identity).size(), 1U);
    owner.terminate();
    const auto restored = sessions->prepare(identity);
    EXPECT_EQ(restored.restore.interrupted_turns.size(), 1U);
}

TEST_F(MultiProcessTest, MaintenanceRejectsSecondaryAndExcludesNewOpeners) {
    auto main = WorkspaceConfigStore::open(database_);
    VaultProcess secondary(database_);
    ASSERT_EQ(secondary.call({{"action", "open"}}).at("main"), false);
    {
        auto maintenance = main->reserve_maintenance();
        EXPECT_THROW(maintenance.close(), SessionBusyError);
    }
    EXPECT_NE(main->snapshot()->find_character("guide"), nullptr);
    secondary.terminate();
    {
        auto maintenance = main->reserve_maintenance();
        maintenance.close();
        VaultProcess rejected(database_);
        EXPECT_EQ(rejected.call({{"action", "open"}}).at("busy"), true);
        maintenance.reopen();
    }
    VaultProcess admitted(database_);
    EXPECT_EQ(admitted.call({{"action", "open"}}).at("main"), false);
}

TEST_F(MultiProcessTest, ConcurrentCompatibleSchemaInitializationIsSerialized) {
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        db.execute("ALTER TABLE entries DROP COLUMN input_tokens");
        db.execute("ALTER TABLE entries DROP COLUMN output_tokens");
        db.execute("ALTER TABLE entries DROP COLUMN web_search_used");
        db.execute("ALTER TABLE sessions DROP COLUMN recent_pending");
        db.execute("ALTER TABLE sessions DROP COLUMN discardable");
    }
    VaultProcess a(database_);
    VaultProcess b(database_);
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        storage::SqliteTransaction writing(db);
        a.send({{"action", "open"}});
        b.send({{"action", "open"}});
        writing.commit();
    }
    const auto first = a.receive();
    const auto second = b.receive();
    ASSERT_TRUE(first.at("ok")) << first;
    ASSERT_TRUE(second.at("ok")) << second;
    EXPECT_NE(first.at("main"), second.at("main"));
}

TEST_F(MultiProcessTest, NormalRepositoryShutdownDoesNotWaitForAnotherReadersSnapshot) {
    auto main = WorkspaceConfigStore::open(database_);
    auto sessions = repository(*main);
    const auto created = sessions->create("lobby", "Stored");
    VaultProcess secondary(database_);
    ASSERT_EQ(secondary.call({{"action", "open"}}).at("ok"), true);
    ASSERT_EQ(secondary.call({{"action", "reader"}}).at("ok"), true);
    (void)sessions->rename(created.identity, "Renamed after reader started");
    const auto before = std::chrono::steady_clock::now();
    sessions.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 1s);
}

} // namespace
} // namespace cha
