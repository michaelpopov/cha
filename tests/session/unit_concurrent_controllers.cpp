#include "providers/model_backend.h"
#include "characters/character_config.h"
#include "session/session_controller.h"
#include "session/session_database.h"
#include "workspace/session_open.h"
#include "workspace/workspace.h"
#include "session/session_repository.h"
#include "support/test_backends.h"
#include "support/test_controller.h"
#include "support/test_notifier.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cha {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
            / ("cha_concurrent_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

class DeterministicBackend final : public test::DescribedModelBackend {
public:
    DeterministicBackend(std::string id, std::string name, std::string answer)
        : info_{
              .character = {.id = std::move(id), .display_name = std::move(name)},
              .model = "deterministic",
              .api = "test://deterministic",
              .streaming = true,
          },
          answer_(std::move(answer)) {
    }

    RequestPayload prepare(const GenerationRequest& input) override {
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool&) override {
        on_delta({GenerationDeltaKind::answer, answer_});
        return {};
    }

    test::DescribedBackendInfo info() const override { return info_; }

private:
    test::DescribedBackendInfo info_;
    std::string answer_;
};

struct ControllerResult {
    SessionRestore restored;
    std::exception_ptr failure;
};

ControllerResult run_controller(
    const std::filesystem::path& database_path,
    std::string id,
    std::string name,
    std::string prompt,
    std::string answer,
    std::barrier<>& start) {
    try {
        test::TestNotifier notifier;
        start.arrive_and_wait();
        auto controller = test::from_test_backends(
            test::one_backend(std::make_unique<DeterministicBackend>(
                std::move(id), std::move(name), std::move(answer))),
            database_path,
            notifier);
        (void)controller->submit_prompt("operator", std::move(prompt));
        while (controller->is_generating()) {
            const std::size_t observed = notifier.wake_count();
            (void)test::receive_all_events(*controller);
            if (controller->is_generating()
                && !notifier.wait_for_wake(observed)) {
                throw std::runtime_error("Timed out waiting for deterministic backend");
            }
        }
        controller->shutdown();
        return {.restored = load_session_state(database_path)};
    } catch (...) {
        return {.failure = std::current_exception()};
    }
}

void require_database(
    const std::filesystem::path& path,
    std::string id) {
    ASSERT_TRUE(create_session_database(
        path,
        {.id = std::move(id), .forum = "forum", .label = "Concurrent"}));
}

struct WorkspaceLayout {
    std::filesystem::path root;
    std::filesystem::path forum;
};

// The production graph over a hand-built workspace: one loaded model plus the
// repository that owns its storage.
struct SessionGraph {
    explicit SessionGraph(const std::filesystem::path& root)
        : store_(open_store(root)),
          repository(
              store_->database_path(),
              store_->workspace_path(),
              store_->welcome_path(),
              TemporarySessionSeed{{"temporary-forum", "temporary-session"}, "Temporary"}) {}

    WorkspaceConfigStore& config() const { return *store_; }

private:
    static std::unique_ptr<WorkspaceConfigStore> open_store(
        const std::filesystem::path& root) {
        import_workspace_configuration(root, root / "workspace.sqlite3");
        return WorkspaceConfigStore::open(root / "workspace.sqlite3");
    }

    std::unique_ptr<WorkspaceConfigStore> store_;

public:
    SessionRepository repository;
};

WorkspaceLayout make_workspace(const std::filesystem::path& parent) {
    const std::filesystem::path root = parent / "workspace";
    const std::filesystem::path forum = root / "forums" / "forum";
    std::filesystem::create_directories(root / "characters" / "character");
    std::filesystem::create_directories(forum / "members" / "character");
    std::filesystem::create_directories(root / "personas" / "operator");
    {
        const std::filesystem::path provider =
            root / "system" / "providers" / "test";
        std::filesystem::create_directories(provider);
        std::ofstream(provider / "config.toml")
            << "host = \"127.0.0.1\"\nport = 9\nmode = \"test\"\n"
            << "model = \"configured-model\"\n";
        std::filesystem::create_directories(root / "system" / "assistant");
        std::ofstream(root / "system" / "assistant" / "character.toml")
            << "display_name = \"Assistant\"\nprovider = \"test\"\n";
    }
    std::ofstream(forum / "config.toml") << "display_name = \"Forum\"\n";
    std::ofstream(forum / "FORUM.md") << "Forum prompt";
    std::ofstream(forum / "members" / "character" / "character.toml")
        << "# forum membership\n";
    std::ofstream(root / "characters" / "character" / "character.toml")
        << "display_name = \"Worker\"\nprovider = \"test\"\n";
    std::ofstream(root / "characters" / "character" / "CHARACTER.md")
        << "System prompt";
    std::ofstream(root / "personas" / "operator" / "persona.toml")
        << "display_name = \"Reader\"\n";
    return {root, forum};
}

TEST(ConcurrentControllers, KeepTranscriptsJournalsAndRequestIdsIndependent) {
    TemporaryDirectory directory;
    const std::filesystem::path first_path = directory.path() / "first.sqlite3";
    const std::filesystem::path second_path = directory.path() / "second.sqlite3";
    ASSERT_NO_FATAL_FAILURE(require_database(first_path, "first"));
    ASSERT_NO_FATAL_FAILURE(require_database(second_path, "second"));

    std::barrier start(3);
    ControllerResult first;
    ControllerResult second;
    std::thread first_thread([&] {
        first = run_controller(
            first_path, "worker", "Worker", "first prompt", "first answer", start);
    });
    std::thread second_thread([&] {
        second = run_controller(
            second_path, "worker", "Worker", "second prompt", "second answer", start);
    });
    start.arrive_and_wait();
    first_thread.join();
    second_thread.join();

    if (first.failure) std::rethrow_exception(first.failure);
    if (second.failure) std::rethrow_exception(second.failure);

    ASSERT_EQ(first.restored.entries.size(), 2U);
    EXPECT_EQ(first.restored.entries[0].text, "first prompt");
    EXPECT_EQ(first.restored.entries[1].text, "first answer");
    EXPECT_EQ(first.restored.entries[0].request_id, 1);
    EXPECT_EQ(first.restored.entries[1].request_id, 1);
    ASSERT_EQ(second.restored.entries.size(), 2U);
    EXPECT_EQ(second.restored.entries[0].text, "second prompt");
    EXPECT_EQ(second.restored.entries[1].text, "second answer");
    EXPECT_EQ(second.restored.entries[0].request_id, 1);
    EXPECT_EQ(second.restored.entries[1].request_id, 1);
}

TEST(ConcurrentControllers, ConstructSessionLocalProviderClientsConcurrently) {
    // This cross-platform test supplies the portability coverage. Its
    // concurrent first touch of curl_global() is only order-dependent smoke
    // coverage because an earlier test may have initialized the magic static;
    // C++ guarantees thread-safe initialization regardless.
    TemporaryDirectory directory;
    const std::filesystem::path first_path = directory.path() / "first.sqlite3";
    const std::filesystem::path second_path = directory.path() / "second.sqlite3";
    ASSERT_NO_FATAL_FAILURE(require_database(first_path, "first"));
    ASSERT_NO_FATAL_FAILURE(require_database(second_path, "second"));

    std::barrier start(3);
    std::array<std::exception_ptr, 2> failures;
    const auto construct = [&](std::size_t index, const std::filesystem::path& path) {
        try {
            (void)index;
            test::NoopNotifier notifier;
            CharacterDefinition definition;
            definition.character.id = "worker";
            definition.character.display_name = "Worker";
            definition.provider = {.id = "test", .config = {
                .host = "127.0.0.1",
                .port = 9,
                .mode = Mode::net,
                .model = "configured-model",
            }};
            start.arrive_and_wait();
            auto controller = test::from_test_workspace(
                {std::move(definition)}, path, notifier);
            controller->shutdown();
        } catch (...) {
            failures[index] = std::current_exception();
        }
    };
    std::thread first([&] { construct(0, first_path); });
    std::thread second([&] { construct(1, second_path); });
    start.arrive_and_wait();
    first.join();
    second.join();

    for (const std::exception_ptr& failure : failures) {
        if (failure) std::rethrow_exception(failure);
    }
}

TEST(WorkspaceConcurrency, SharesTheModelWhileRepositoryCreatesCollidingSessions) {
    TemporaryDirectory directory;
    const WorkspaceLayout layout = make_workspace(directory.path());
    const SessionGraph graph(layout.root);
    constexpr std::size_t creator_count = 4;
    std::atomic_bool creating{true};
    std::atomic_bool observed_forum_list_mismatch{false};
    std::atomic_bool observed_forum_identity_mismatch{false};
    std::atomic_bool observed_invalid_session{false};
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto record_failure = [&](std::exception_ptr error) {
        std::lock_guard lock(failure_mutex);
        if (!failure) failure = std::move(error);
    };

    std::thread lister([&] {
        try {
            while (creating.load(std::memory_order_acquire)) {
                const std::shared_ptr<const Workspace> workspace = getws();
                const WorkspaceForum* const forum = workspace->find_forum("forum");
                if (forum == nullptr) {
                    observed_forum_list_mismatch.store(
                        true, std::memory_order_release);
                }
                if (forum == nullptr || forum->id != "forum") {
                    observed_forum_identity_mismatch.store(
                        true, std::memory_order_release);
                }
                for (const StoredSession& session : graph.repository.list("forum")) {
                    if (session.identity.forum_id != "forum") {
                        observed_invalid_session.store(true, std::memory_order_release);
                    }
                }
            }
        } catch (...) {
            record_failure(std::current_exception());
        }
    });

    std::barrier start(creator_count + 1);
    std::vector<StoredSession> created(creator_count);
    std::vector<std::thread> creators;
    creators.reserve(creator_count);
    for (std::size_t index = 0; index < creator_count; ++index) {
        creators.emplace_back([&, index] {
            try {
                start.arrive_and_wait();
                created[index] = graph.repository.create(
                    "forum", "Concurrent " + std::to_string(index));
            } catch (...) {
                record_failure(std::current_exception());
            }
        });
    }
    start.arrive_and_wait();
    for (std::thread& creator : creators) creator.join();
    creating.store(false, std::memory_order_release);
    lister.join();

    if (failure) std::rethrow_exception(failure);
    EXPECT_FALSE(
        observed_forum_list_mismatch.load(std::memory_order_acquire));
    EXPECT_FALSE(
        observed_forum_identity_mismatch.load(std::memory_order_acquire));
    EXPECT_FALSE(observed_invalid_session.load(std::memory_order_acquire));

    std::set<std::string> created_ids;
    for (const StoredSession& session : created) {
        EXPECT_FALSE(session.identity.session_id.empty());
        EXPECT_TRUE(created_ids.insert(session.identity.session_id).second)
            << "duplicate published session ID: " << session.identity.session_id;
        EXPECT_EQ(
            graph.repository.prepare(session.identity).label,
            session.label);
    }
    EXPECT_EQ(created_ids.size(), creator_count);

    const std::vector<StoredSession> sessions = graph.repository.list("forum");
    ASSERT_EQ(sessions.size(), creator_count);
    for (const StoredSession& session : sessions) {
        EXPECT_EQ(session.identity.forum_id, "forum");
    }
}

TEST(WorkspaceConcurrency, OpensSessionsOnOwnerThreadsWhileListing) {
    TemporaryDirectory directory;
    const WorkspaceLayout layout = make_workspace(directory.path());
    const SessionGraph graph(layout.root);
    constexpr std::size_t session_count = 4;
    std::vector<StoredSession> created;
    created.reserve(session_count);
    for (std::size_t index = 0; index < session_count; ++index) {
        created.push_back(graph.repository.create(
            "forum", "Session " + std::to_string(index)));
    }
    ASSERT_NE(created[0].identity.session_id, created[1].identity.session_id);

    // The web composition shares this same model and repository between lobby
    // threads and session owner threads. Exercise listing while two owners open distinct
    // stored sessions, then shut each controller down on its owning thread.
    std::mutex failure_mutex;
    std::exception_ptr failure;
    const auto record_failure = [&](std::exception_ptr error) {
        std::lock_guard lock(failure_mutex);
        if (!failure) failure = std::move(error);
    };
    std::barrier open_start(4);
    std::array<bool, 2> opened{};
    std::vector<StoredSession> listed_during_open;
    std::thread opening_first([&] {
        try {
            test::NoopNotifier notifier;
            Providers providers;
            open_start.arrive_and_wait();
            auto controller = open_session(
                graph.repository, {"forum", created[0].identity.session_id}, providers,
                std::shared_ptr<WakeNotifier>(&notifier, [](WakeNotifier*) {}),
                graph.config());
            opened[0] = true;
            controller.controller->shutdown();
        } catch (...) {
            record_failure(std::current_exception());
        }
    });
    std::thread opening_second([&] {
        try {
            test::NoopNotifier notifier;
            Providers providers;
            open_start.arrive_and_wait();
            auto controller = open_session(
                graph.repository, {"forum", created[1].identity.session_id}, providers,
                std::shared_ptr<WakeNotifier>(&notifier, [](WakeNotifier*) {}),
                graph.config());
            opened[1] = true;
            controller.controller->shutdown();
        } catch (...) {
            record_failure(std::current_exception());
        }
    });
    std::thread listing_during_open([&] {
        try {
            open_start.arrive_and_wait();
            listed_during_open = graph.repository.list("forum");
        } catch (...) {
            record_failure(std::current_exception());
        }
    });
    open_start.arrive_and_wait();
    opening_first.join();
    opening_second.join();
    listing_during_open.join();

    if (failure) std::rethrow_exception(failure);
    EXPECT_TRUE(opened[0]);
    EXPECT_TRUE(opened[1]);
    ASSERT_EQ(listed_during_open.size(), session_count);
    for (const StoredSession& session : listed_during_open) {
        EXPECT_EQ(session.identity.forum_id, "forum");
    }
}

} // namespace
} // namespace cha
