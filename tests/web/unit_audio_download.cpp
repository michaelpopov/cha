#include "web/audio_download.h"
#include "web/current_vault.h"
#include "providers/api_key_store.h"
#include "session/sqlite_storage.h"
#include "support/test_workspace.h"
#include "support/test_transcript.h"
#include "workspace/workspace_config_store.h"
#include <gtest/gtest.h>
#include <future>

namespace cha {
namespace {
using namespace std::chrono_literals;
using Json = nlohmann::json;
bool eventually(const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}
struct ReleaseOnExit {
    std::atomic_bool& release;
    ~ReleaseOnExit() { release = true; }
};
class AudioDownloads : public ::testing::Test {
protected:
    void SetUp() override {
        path = test::import_test_database(workspace.root());
        config = WorkspaceConfigStore::open(path);
        keys = std::make_unique<ApiKeyStore>(*config);
        const auto api = keys->create("FishAudio", "secret");
        config->create_voice("Reader", "", "voice");
        config->apply_voice_output_update({.url = "https://api.fish.audio/v1/tts", .model = "s2.1-pro",
            .api_key_id = api.id, .output_format = "mp3", .default_voice = "Reader"});
        sessions = std::make_unique<SessionRepository>(
            [this] { return config->snapshot(); }, path, config->workspace_path(), config->welcome_path(),
            TemporarySessionSeed{{"temporary", "welcome"}, "Welcome"});
        session = sessions->create("lobby", "Audio").identity;
        const auto prepared = sessions->prepare(session);
        SessionJournal journal(path, prepared.session_key);
        for (EntryId id = 1; id <= 5; ++id)
            journal.record_entry(test::human_entry(id, {"human", "You"}, {"guide", "Guide"}, std::to_string(id)));
        vault = std::make_unique<CurrentVault>(VaultDefinition{.name = "Test", .data = path});
    }
    AudioDownloadRequest input() { return {"Test", {.reference_id = "voice"}}; }
    Json http_input() { return {{"vault_name", "Test"}, {"reference_id", "voice"}}; }
    std::unique_ptr<AudioDownloadManager> make(AudioDownloadManager::Transport transfer) {
        return std::make_unique<AudioDownloadManager>(*sessions, *vault, true, std::move(transfer));
    }
    test::TestWorkspace workspace;
    std::filesystem::path path;
    std::unique_ptr<WorkspaceConfigStore> config;
    std::unique_ptr<ApiKeyStore> keys;
    std::unique_ptr<SessionRepository> sessions;
    std::unique_ptr<CurrentVault> vault;
    FullSessionId session;
};

TEST_F(AudioDownloads, BatchAcceptanceQueuesThreeWorkersAndDeduplicatesExistingJobs) {
    std::atomic_int started{}, attempts{};
    std::atomic_bool release{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto& cancel) -> std::optional<EntryAudio> {
        ++attempts; ++started;
        while (!release && !cancel()) std::this_thread::sleep_for(2ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{"audio", "audio/mpeg"}};
    });
    ReleaseOnExit cleanup{release};
    AudioDownloadBatchRequest batch{"Test", {}};
    for (EntryId id = 1; id <= 4; ++id) batch.entries.push_back({id, {.reference_id = "voice"}});
    const auto accepted = downloads->submit_batch(session, batch);
    EXPECT_EQ(accepted.size(), 4);
    ASSERT_TRUE(eventually([&] { return started == 3; }));
    EXPECT_EQ(downloads->status(session, "Test").downloads.back().state, AudioJobState::queued);
    EXPECT_EQ(downloads->submit_batch(session, batch).size(), 4);
    EXPECT_EQ(downloads->status(session, "Test").downloads.size(), 4);
    release = true;
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).size() == 4; }));
    EXPECT_EQ(attempts, 4);
    const auto cached = downloads->submit_batch(session, batch);
    for (const auto& item : cached) EXPECT_EQ(item.kind, AudioAcceptanceKind::cached);
}

TEST_F(AudioDownloads, InvalidBatchAdmitsNoNewJobs) {
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        ++attempts; return EntryAudio{"audio", "audio/mpeg"};
    });
    const AudioDownloadBatchRequest batch{"Test", {
        {1, {.reference_id = "voice"}}, {99, {.reference_id = "voice"}}
    }};
    EXPECT_THROW(downloads->submit_batch(session, batch), AudioDownloadError);
    EXPECT_TRUE(downloads->status(session, "Test").downloads.empty());
    EXPECT_EQ(attempts, 0);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
}

TEST_F(AudioDownloads, ThreeWorkersQueueFourthAndDeduplicate) {
    std::mutex mutex;
    std::condition_variable cv;
    std::set<std::string> started;
    std::set<std::string> released;
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel) -> std::optional<EntryAudio> {
        const auto text = request.body.at("text").template get<std::string>();
        std::unique_lock lock(mutex);
        started.insert(text); cv.notify_all();
        while (!released.contains(text) && !cancel()) cv.wait_for(lock, 5ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{text, "audio/mpeg"}};
    });
    for (EntryId id = 1; id <= 4; ++id) downloads->submit(session, id, input());
    ASSERT_TRUE(eventually([&] { std::lock_guard lock(mutex); return started.size() == 3; }));
    const auto duplicate = downloads->submit(session, 4, {"Test", {}}); // Existing job wins over new inputs.
    EXPECT_EQ(duplicate.kind, AudioAcceptanceKind::queued);
    {
        std::lock_guard lock(mutex);
        EXPECT_FALSE(started.contains("4"));
        released.insert("1"); cv.notify_all();
    }
    ASSERT_TRUE(eventually([&] { std::lock_guard lock(mutex); return started.contains("4"); }));
    downloads->submit(session, 2, input());
    {
        std::lock_guard lock(mutex);
        released = {"1", "2", "3", "4"}; cv.notify_all();
    }
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).size() == 4; }));
    EXPECT_EQ(downloads->submit(session, 1, input()).kind, AudioAcceptanceKind::cached);
}
TEST_F(AudioDownloads, FourAttemptsWithFiftyMillisecondWaits) {
    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> attempts;
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        std::lock_guard lock(mutex);
        attempts.push_back(std::chrono::steady_clock::now());
        throw std::runtime_error("upstream failed");
    });
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] {
        const auto status = downloads->status(session, "Test");
        return !status.downloads.empty() && status.downloads[0].state == AudioJobState::failed;
    }));
    std::lock_guard lock(mutex);
    ASSERT_EQ(attempts.size(), 4);
    for (std::size_t i = 1; i < attempts.size(); ++i) EXPECT_GE(attempts[i] - attempts[i-1], 50ms);
}
TEST_F(AudioDownloads, RetryWaitDoesNotConsumeQueueWakeup) {
    std::atomic_bool first_started{}, second_started{}, release_first{}, fourth_started{};
    std::atomic_int third_attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel) -> std::optional<EntryAudio> {
        const auto text = request.body.at("text").template get<std::string>();
        if (text == "3" && ++third_attempts == 1) throw std::runtime_error("Retry this transfer");
        if (text == "4") { fourth_started = true; return EntryAudio{"fourth", "audio/mpeg"}; }
        if (text == "1") first_started = true;
        if (text == "2") second_started = true;
        while (!cancel() && !(text == "1" && release_first)) std::this_thread::sleep_for(1ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{"audio", "audio/mpeg"}};
    });
    downloads->submit(session, 1, input());
    downloads->submit(session, 2, input());
    ASSERT_TRUE(eventually([&] { return first_started && second_started; }));
    downloads->submit(session, 3, input());
    ASSERT_TRUE(eventually([&] { return third_attempts == 1; }));
    // Let worker 3 enter its retry wait before worker 1 becomes idle.
    std::this_thread::sleep_for(10ms);
    release_first = true;
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    std::this_thread::sleep_for(5ms);
    downloads->submit(session, 4, input());
    ASSERT_TRUE(eventually([&] { return fourth_started.load(); }));
    EXPECT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(4); }));
}

TEST_F(AudioDownloads, OpusAudioWithParametersIsSavedWithoutRetry) {
    config->apply_voice_output_update({.url = "https://api.fish.audio/v1/tts", .model = "s2.1-pro",
        .api_key_id = config->snapshot()->voice_output()->api_key_id, .output_format = "opus", .default_voice = "Reader"});
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto&) -> std::optional<EntryAudio> {
        ++attempts;
        EXPECT_EQ(request.body.at("format"), "opus");
        return EntryAudio{"opus-audio", "audio/opus; codecs=opus"};
    });
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    const auto saved = downloads->audio(session, 1, "Test");
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->content_type, "audio/opus; codecs=opus");
    EXPECT_EQ(attempts, 1);
}

TEST_F(AudioDownloads, DeletedKeyReportsNotConfiguredButCachedAudioStillWorks) {
    std::atomic_int transfers{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        ++transfers;
        return EntryAudio{"audio", "audio/mpeg"};
    });
    keys->remove(config->snapshot()->voice_output()->api_key_id);
    try {
        downloads->submit(session, 1, input());
        FAIL() << "An uncached entry needs a configured API key";
    } catch (const AudioDownloadError& error) {
        EXPECT_EQ(error.status, 404);
        EXPECT_EQ(error.code, "not_found");
        EXPECT_STREQ(error.what(), "Voice output is not configured.");
    }
    const auto entry = sessions->lookup_entry_audio(session, 1, false);
    ASSERT_TRUE(entry);
    sessions->save_entry_audio(*entry, {"cached", "audio/mpeg"});
    EXPECT_EQ(downloads->submit(session, 1, input()).kind, AudioAcceptanceKind::cached);
    EXPECT_EQ(downloads->audio(session, 1, "Test")->audio, "cached");
    EXPECT_EQ(transfers, 0);
}
TEST_F(AudioDownloads, DisabledDownloadsReportEmptyStatusButRejectWork) {
    AudioDownloadManager downloads(*sessions, *vault, false);
    for (const std::string name : {"Test", "Other"}) {
        const auto status = downloads.status(session, name);
        EXPECT_TRUE(status.cached_entry_ids.empty());
        EXPECT_TRUE(status.downloads.empty());
    }
    EXPECT_THROW(downloads.submit(session, 1, input()), AudioDownloadError);
}
TEST_F(AudioDownloads, DeletedQueuedEntryIsDroppedBeforeTransfer) {
    std::mutex mutex;
    std::set<std::string> started;
    std::atomic_bool release{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel) -> std::optional<EntryAudio> {
        {
            std::lock_guard lock(mutex);
            started.insert(request.body.at("text").template get<std::string>());
        }
        while (!release && !cancel()) std::this_thread::sleep_for(2ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{"audio", "audio/mpeg"}};
    });
    for (EntryId id = 1; id <= 4; ++id) downloads->submit(session, id, input());
    ASSERT_TRUE(eventually([&] { std::lock_guard lock(mutex); return started.size() == 3; }));
    {
        storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
        database.execute("DELETE FROM entries WHERE entry_id = 4");
    }
    release = true;
    ASSERT_TRUE(eventually([&] { return downloads->status(session, "Test").downloads.empty(); }));
    std::lock_guard lock(mutex);
    EXPECT_EQ(started.size(), 3);
    EXPECT_FALSE(started.contains("4"));
    EXPECT_EQ(sessions->cached_audio_entries(session).size(), 3);
}
TEST_F(AudioDownloads, FourthAttemptCanSucceed) {
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        if (++attempts < 4) throw std::runtime_error("upstream failed");
        return EntryAudio{"audio", "audio/mpeg"};
    });
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    EXPECT_EQ(attempts, 4);
}
TEST_F(AudioDownloads, CancellationStopsStalledTransferAndPreservesNewWork) {
    std::atomic_bool started{}, canceled{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel) -> std::optional<EntryAudio> {
        if (request.body.at("text") == "2") return EntryAudio{"new audio", "audio/mpeg"};
        started = true;
        while (!cancel()) std::this_thread::sleep_for(2ms);
        canceled = true;
        return std::nullopt;
    });
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] { return started.load(); }));
    downloads->pause(); downloads->resume();
    ASSERT_TRUE(eventually([&] { return canceled.load(); }));
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    downloads->submit(session, 2, input());
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(2); }));
    EXPECT_FALSE(sessions->cached_audio_entries(session).contains(1));
    downloads->request_stop();
    EXPECT_TRUE(downloads->join_until(std::chrono::steady_clock::now() + 2s));
}
TEST_F(AudioDownloads, CancellationWakesRetryWait) {
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        ++attempts;
        throw std::runtime_error("upstream failed");
    });
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] { return attempts == 1; }));
    downloads->request_stop();
    EXPECT_TRUE(downloads->join_until(std::chrono::steady_clock::now() + 1s));
    EXPECT_EQ(attempts, 1);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
}
TEST_F(AudioDownloads, PersistenceFailureDoesNotRepeatTransferOrSave) {
    storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
    database.execute("CREATE TRIGGER reject_audio BEFORE INSERT ON entry_audio BEGIN SELECT RAISE(FAIL, 'save failed'); END");
    std::atomic_int transfers{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        ++transfers; return EntryAudio{"audio", "audio/mpeg"};
    });
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] {
        const auto status = downloads->status(session, "Test");
        return !status.downloads.empty() && status.downloads[0].state == AudioJobState::failed;
    }));
    EXPECT_EQ(transfers, 1);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    database.execute("DROP TRIGGER reject_audio");
    const auto retry = downloads->submit(session, 1, input());
    EXPECT_EQ(retry.kind, AudioAcceptanceKind::queued);
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    EXPECT_EQ(transfers, 2);
}
TEST_F(AudioDownloads, ClearCancelsOldCompletionWithoutReplacingNewJob) {
    std::atomic_int transfers{};
    std::atomic_bool release{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        ++transfers;
        while (!release) std::this_thread::sleep_for(2ms);
        return EntryAudio{"audio", "audio/mpeg"}; // Simulate a result racing cancellation.
    });
    ReleaseOnExit cleanup{release};
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] { return transfers == 1; }));
    downloads->clear(session);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] { return transfers == 2; }));
    release = true;
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    EXPECT_EQ(transfers, 2);
}
TEST_F(AudioDownloads, DeletedEntryCannotSaveLateAudio) {
    std::atomic_bool started{}, release{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        started = true;
        while (!release) std::this_thread::sleep_for(2ms);
        return EntryAudio{"audio", "audio/mpeg"};
    });
    ReleaseOnExit cleanup{release};
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] { return started.load(); }));
    {
        storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
        database.execute("DELETE FROM entries WHERE entry_id = 1");
    }
    release = true;
    ASSERT_TRUE(eventually([&] { return downloads->status(session, "Test").downloads.empty(); }));
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
}
TEST_F(AudioDownloads, OldVaultCompletionCannotSaveAfterSwitchingAwayAndBack) {
    std::atomic_bool started{}, cancelled{}, release{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto& cancel) -> std::optional<EntryAudio> {
        started = true;
        while (!cancel()) std::this_thread::sleep_for(2ms);
        cancelled = true;
        while (!release) std::this_thread::sleep_for(2ms);
        return EntryAudio{"old-vault-audio", "audio/mpeg"};
    });
    ReleaseOnExit cleanup{release};
    downloads->submit(session, 1, input());
    ASSERT_TRUE(eventually([&] { return started.load(); }));
    downloads->pause();
    vault->set(VaultDefinition{.name = "Other", .data = path});
    downloads->resume();
    downloads->pause();
    vault->set(VaultDefinition{.name = "Test", .data = path});
    downloads->resume();
    ASSERT_TRUE(eventually([&] { return cancelled.load(); }));
    release = true;
    downloads->request_stop();
    ASSERT_TRUE(downloads->join_until(std::chrono::steady_clock::now() + 2s));
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
}
TEST_F(AudioDownloads, CachedAudioReadRejectsChangedGenerationEvenAfterSwitchingBack) {
    const auto entry = sessions->lookup_entry_audio(session, 1);
    ASSERT_TRUE(entry);
    sessions->save_entry_audio(*entry, {"saved", "audio/mpeg"});
    auto downloads = make([](const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        throw std::runtime_error("Cached playback must not synthesize");
    });
    std::future<int> read;
    {
        const auto maintenance = sessions->reserve_maintenance();
        read = std::async(std::launch::async, [&] {
            try { downloads->audio(session, 1, "Test"); return 200; }
            catch (const AudioDownloadError& error) { return error.status; }
        });
        // The blob lookup is blocked by maintenance after capturing the generation.
        EXPECT_EQ(read.wait_for(100ms), std::future_status::timeout);
        downloads->pause(); downloads->resume();
        downloads->pause(); downloads->resume();
    }
    EXPECT_EQ(read.get(), 409);
    EXPECT_EQ(downloads->audio(session, 1, "Test")->audio, "saved");
}
}
}
