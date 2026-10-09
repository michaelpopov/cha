#include "media/audio_download.h"
#include "app/current_vault.h"
#include "providers/api_key_store.h"
#include "storage/sqlite_storage.h"
#include "support/test_workspace.h"
#include "support/test_transcript.h"
#include "support/test_notifier.h"
#include "session/session_open.h"
#include "runtime/text_input.h"
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
        config->apply_voice_output_update({.fishaudio = WorkspaceVoiceProviderOutput{.url = "https://api.fish.audio/v1/tts", .model = "s2.1-pro",
            .api_key_id = api.id, .output_format = "mp3"}, .default_voice = "Reader"});
        sessions = std::make_unique<SessionRepository>(
            [this] { return config->snapshot(); }, path, config->workspace_path(), config->welcome_path(),
            TemporarySessionSeed{{"temporary", "welcome"}, "Welcome"});
        session = sessions->create("lobby", "Audio").identity;
        const auto prepared = sessions->prepare(session);
        SessionJournal journal(path, prepared.session_key);
        for (EntryId id = 1; id <= 5; ++id)
            journal.record_entry(make_character_entry(id, "guide", "Guide", std::to_string(id), EntryStatus::complete));
        vault = std::make_unique<CurrentVault>(VaultDefinition{.name = "Test", .data = path});
    }
    AudioDownloadBatchRequest input(std::initializer_list<EntryId> ids) {
        AudioDownloadBatchRequest request{"Test", {}};
        for (const auto id : ids) request.entries.push_back({id, {.reference_id = "voice"}});
        return request;
    }
    Json http_input() { return {{"vault_name", "Test"}, {"reference_id", "voice"}}; }
    std::unique_ptr<AudioDownloadManager> make(AudioDownloadManager::Transport transfer) {
        return std::make_unique<AudioDownloadManager>(
            *sessions, [this] { return vault->get().name; }, true,
            std::move(transfer));
    }
    void add_reply(std::string text = "Hello.", std::string character = "guide") {
        const auto prepared = sessions->prepare(session);
        SessionJournal journal(path, prepared.session_key);
        journal.record_entry(make_character_entry(6, std::move(character), "Guide",
            std::move(text), EntryStatus::complete));
    }
    test::TestWorkspace workspace;
    std::filesystem::path path;
    std::unique_ptr<WorkspaceConfigStore> config;
    std::unique_ptr<ApiKeyStore> keys;
    std::unique_ptr<SessionRepository> sessions;
    std::unique_ptr<CurrentVault> vault;
    FullSessionId session;
};

TEST_F(AudioDownloads, StreamsCleanedReplyAndCachesAudioWithoutChangingTranscript) {
    const std::string original = "**Hello.** ([source](https://example.com))\n\n# Next _paragraph_.";
    add_reply(original);
    std::atomic_int transfers{};
    std::atomic_bool release_audio{}, first_chunk{};
    auto downloads = make(
        [&](const auto&, const auto&, const auto& request, const auto& cancel,
            const AudioChunkCallback& emit) -> std::optional<EntryAudio> {
            EXPECT_EQ(request.body.at("text"), "Hello. [long pause] Next paragraph.");
            if (++transfers == 1) throw std::runtime_error("Retry FishAudio only");
            emit("audio/mpeg", "first");
            first_chunk = true;
            while (!release_audio && !cancel()) std::this_thread::sleep_for(2ms);
            if (cancel()) return std::nullopt;
            emit("audio/mpeg", "second");
            return EntryAudio{"firstsecond", "audio/mpeg"};
        });
    ReleaseOnExit audio_cleanup{release_audio};
    downloads->submit_batch(session, input({6}));
    ASSERT_TRUE(eventually([&] { return first_chunk.load(); }));
    const auto stream = downloads->stream(session, 6, "Test");
    ASSERT_TRUE(stream);
    EXPECT_EQ(stream->read(0)->body, "first");
    const auto first = downloads->audio_chunk(session, 6, "Test", 0);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->body, "first");
    EXPECT_EQ(first->mime_type, "audio/mpeg");
    EXPECT_FALSE(first->complete);
    const auto waiting = downloads->audio_chunk(session, 6, "Test", 5);
    ASSERT_TRUE(waiting);
    EXPECT_TRUE(waiting->body.empty());
    EXPECT_FALSE(waiting->complete);
    EXPECT_FALSE(sessions->cached_audio_entries(session).contains(6));
    release_audio = true;
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(6); }));
    EXPECT_EQ(downloads->audio(session, 6, "Test")->audio, "firstsecond");
    EXPECT_EQ(sessions->lookup_entry_audio(session, 6)->entry_text, original);
    EXPECT_EQ(downloads->submit_batch(session, input({6})).at(0).kind, AudioAcceptanceKind::cached);
    const auto tail = downloads->audio_chunk(session, 6, "Test", 5);
    ASSERT_TRUE(tail);
    EXPECT_EQ(tail->body, "second");
    ASSERT_TRUE(eventually([&] { return downloads->audio_chunk(session, 6, "Test", 5)->complete; }));
    EXPECT_EQ(transfers, 2);
}

TEST_F(AudioDownloads, HumanEntriesCannotGenerateAudio) {
    const auto prepared = sessions->prepare(session);
    SessionJournal journal(path, prepared.session_key);
    journal.record_entry(test::human_entry(6, {"human", "You"}, {"guide", "Guide"}, "Question"));
    auto downloads = make(
        [](const auto&, const auto&, const auto&, const auto&,
            const AudioChunkCallback&) -> std::optional<EntryAudio> {
            ADD_FAILURE() << "Human entries must not call FishAudio";
            return std::nullopt;
        });
    EXPECT_THROW(downloads->submit_batch(session, input({6})), std::invalid_argument);
    const AudioDownloadBatchRequest batch{"Test", {
        {1, {.reference_id = "voice"}}, {6, {.reference_id = "voice"}},
    }};
    EXPECT_THROW(downloads->submit_batch(session, batch), std::invalid_argument);
    EXPECT_TRUE(downloads->status(session, "Test").downloads.empty());
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
}

TEST_F(AudioDownloads, ReadsBoundedChunksFromCachedAudioAndRejectsInvalidOffsets) {
    const auto entry = sessions->lookup_entry_audio(session, 1, false);
    ASSERT_TRUE(entry);
    const std::string bytes(70 * 1024, 'a');
    sessions->save_entry_audio(*entry, {bytes, "audio/mpeg"});
    auto downloads = make([](const auto&, const auto&, const auto&, const auto&,
        const AudioChunkCallback&) -> std::optional<EntryAudio> {
        ADD_FAILURE() << "Cached audio must not call FishAudio";
        return std::nullopt;
    });
    const auto first = downloads->audio_chunk(session, 1, "Test", 0);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->body.size(), 64u * 1024);
    EXPECT_FALSE(first->complete);
    const auto last = downloads->audio_chunk(session, 1, "Test", first->body.size());
    ASSERT_TRUE(last);
    EXPECT_EQ(first->body + last->body, bytes);
    EXPECT_TRUE(last->complete);
    EXPECT_TRUE(downloads->audio_chunk(session, 1, "Test", bytes.size())->body.empty());
    EXPECT_THROW(downloads->audio_chunk(session, 1, "Test", bytes.size() + 1), std::invalid_argument);
    EXPECT_FALSE(downloads->audio_chunk(session, 99, "Test", 0));
}

TEST_F(AudioDownloads, BatchAcceptanceQueuesThreeWorkersAndDeduplicatesExistingJobs) {
    std::atomic_int started{}, attempts{};
    std::atomic_bool release{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto& cancel, const AudioChunkCallback&) -> std::optional<EntryAudio> {
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
    // The cache write becomes visible before workers remove their active jobs.
    ASSERT_TRUE(eventually([&] {
        const auto cached = downloads->submit_batch(session, batch);
        return std::all_of(cached.begin(), cached.end(), [](const auto& item) {
            return item.kind == AudioAcceptanceKind::cached;
        });
    }));
}

TEST_F(AudioDownloads, InvalidBatchAdmitsNoNewJobs) {
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
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

TEST_F(AudioDownloads, NewBatchRunsBeforeBacklogAndPreservesBatchOrder) {
    const auto prepared = sessions->prepare(session);
    SessionJournal journal(path, prepared.session_key);
    for (EntryId id = 6; id <= 7; ++id)
        journal.record_entry(make_character_entry(id, "guide", "Guide", std::to_string(id), EntryStatus::complete));
    std::mutex mutex;
    std::vector<std::string> started;
    std::set<std::string> released;
    std::atomic_bool release_all{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        const auto text = request.body.at("text").template get<std::string>();
        {
            std::lock_guard lock(mutex);
            started.push_back(text);
        }
        while (!release_all && !cancel()) {
            {
                std::lock_guard lock(mutex);
                if (released.contains(text)) break;
            }
            std::this_thread::sleep_for(2ms);
        }
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{text, "audio/mpeg"}};
    });
    ReleaseOnExit cleanup{release_all};
    AudioDownloadBatchRequest old_batch{"Test", {}};
    for (EntryId id = 1; id <= 5; ++id) old_batch.entries.push_back({id, {.reference_id = "voice"}});
    downloads->submit_batch(session, old_batch);
    ASSERT_TRUE(eventually([&] { std::lock_guard lock(mutex); return started.size() == 3; }));
    const AudioDownloadBatchRequest new_batch{"Test", {
        {6, {.reference_id = "voice"}}, {7, {.reference_id = "voice"}}, {6, {.reference_id = "voice"}},
    }};
    EXPECT_EQ(downloads->submit_batch(session, new_batch).size(), 3);
    // Free only one worker so dispatch order is observable without thread races.
    std::string previous = "1";
    std::size_t expected_count = 3;
    for (const std::string next : {"6", "7", "4", "5"}) {
        {
            std::lock_guard lock(mutex);
            released.insert(previous);
        }
        ++expected_count;
        ASSERT_TRUE(eventually([&] { std::lock_guard lock(mutex); return started.size() == expected_count; }));
        {
            std::lock_guard lock(mutex);
            EXPECT_EQ(started.back(), next);
        }
        previous = next;
    }
}

TEST_F(AudioDownloads, MulticastStartsAndCollectsAllModelsWhileAudioWorkersAreBlocked) {
    std::vector<std::string> characters;
    for (int index = 0; index < 6; ++index) {
        const auto id = config->create_character("Speaker " + std::to_string(index), "Test speaker");
        config->apply_character_settings(id, "test", std::nullopt);
        characters.push_back(id);
    }
    const auto persona = config->snapshot()->find_forum("lobby")->default_persona_id;
    config->apply_forum_members_and_persona("lobby", characters, persona);
    std::atomic_int audio_started{}, models_started{};
    std::atomic_bool release_audio{}, release_models{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto& cancel, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        ++audio_started;
        while (!release_audio && !cancel()) std::this_thread::sleep_for(2ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{"audio", "audio/mpeg"}};
    });
    ReleaseOnExit cleanup_audio{release_audio};
    AudioDownloadBatchRequest batch{"Test", {}};
    for (EntryId id = 1; id <= 5; ++id) batch.entries.push_back({id, {.reference_id = "voice"}});
    downloads->submit_batch(session, batch);
    ASSERT_TRUE(eventually([&] { return audio_started == 3; }));

    class Backend final : public ModelBackend {
    public:
        Backend(std::atomic_int& started, std::atomic_bool& release) : started_(started), release_(release) {}
        RequestPayload prepare(const GenerationRequest& input) override { return {.bytes = input.run.target.id}; }
        GenerationResult perform(RequestPayload payload, const GenerationDeltaSink& delta,
            const std::atomic_bool& cancelled) override {
            ++started_;
            while (!release_ && !cancelled) std::this_thread::sleep_for(2ms);
            if (cancelled) return {.outcome = GenerationOutcome::cancelled};
            delta({GenerationDeltaKind::answer, "Reply from " + payload.bytes});
            return {};
        }
    private:
        std::atomic_int& started_;
        std::atomic_bool& release_;
    };
    Providers providers([&](SharedCharacterDefinition) {
        return std::make_unique<Backend>(models_started, release_models);
    });
    ReleaseOnExit cleanup_models{release_models};
    auto opened = open_session(*sessions, session, providers, std::make_shared<test::NoopNotifier>(), *config);
    auto& controller = *opened.controller;
    const auto submitted = handle_text_input(controller, persona, "/mcast Question");
    ASSERT_TRUE(submitted.clear_input);
    // All six requests must enter perform(), not merely be queued behind three slots.
    ASSERT_TRUE(eventually([&] { return models_started == 6; }));
    release_models = true;
    ASSERT_TRUE(eventually([&] {
        (void)controller.receive_events(100);
        return !controller.is_generating();
    }));
    const auto entries = controller.view().transcript.entries;
    ASSERT_EQ(entries.size(), 17); // Five old entries plus six prompt/reply pairs.
    for (std::size_t index = 0; index < characters.size(); ++index)
        EXPECT_EQ(entries[6 + index * 2].text, "Reply from " + characters[index]);
    EXPECT_EQ(audio_started, 3);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    EXPECT_EQ(downloads->status(session, "Test").downloads.size(), 5);
}

TEST_F(AudioDownloads, ThreeWorkersQueueFourthAndDeduplicate) {
    std::mutex mutex;
    std::condition_variable cv;
    std::set<std::string> started;
    std::set<std::string> released;
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        const auto text = request.body.at("text").template get<std::string>();
        std::unique_lock lock(mutex);
        started.insert(text); cv.notify_all();
        while (!released.contains(text) && !cancel()) cv.wait_for(lock, 5ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{text, "audio/mpeg"}};
    });
    downloads->submit_batch(session, input({1, 2, 3, 4}));
    ASSERT_TRUE(eventually([&] { std::lock_guard lock(mutex); return started.size() == 3; }));
    const auto duplicate = downloads->submit_batch(session, {"Test", {{4, {}}}}).at(0); // Existing job wins over new inputs.
    EXPECT_EQ(duplicate.kind, AudioAcceptanceKind::queued);
    {
        std::lock_guard lock(mutex);
        EXPECT_FALSE(started.contains("4"));
        released.insert("1"); cv.notify_all();
    }
    ASSERT_TRUE(eventually([&] { std::lock_guard lock(mutex); return started.contains("4"); }));
    downloads->submit_batch(session, input({2}));
    {
        std::lock_guard lock(mutex);
        released = {"1", "2", "3", "4"}; cv.notify_all();
    }
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).size() == 4; }));
    EXPECT_EQ(downloads->submit_batch(session, input({1})).at(0).kind, AudioAcceptanceKind::cached);
}
TEST_F(AudioDownloads, PlaybackSharesTheDownloadBeforeItIsCached) {
    std::atomic_bool release{}, first_chunk{};
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto& cancel,
                             const AudioChunkCallback& emit) -> std::optional<EntryAudio> {
        ++attempts;
        emit("audio/mpeg", "first");
        first_chunk = true;
        while (!release && !cancel()) std::this_thread::sleep_for(2ms);
        if (cancel()) return std::nullopt;
        emit("audio/mpeg", "second");
        return EntryAudio{"firstsecond", "audio/mpeg"};
    });
    ReleaseOnExit cleanup{release};
    downloads->submit_batch(session, input({1}));
    ASSERT_TRUE(eventually([&] { return first_chunk.load(); }));
    auto stream = downloads->stream(session, 1, "Test");
    ASSERT_TRUE(stream);
    EXPECT_EQ(stream->read(0)->body, "first");
    EXPECT_FALSE(stream->read(0)->complete);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    downloads->submit_batch(session, input({1}));
    EXPECT_EQ(downloads->stream(session, 1, "Test"), stream);
    release = true;
    ASSERT_TRUE(eventually([&] { return stream->read(0)->complete; }));
    EXPECT_EQ(downloads->audio(session, 1, "Test")->audio, "firstsecond");
    EXPECT_EQ(stream->read(5)->body, "second");
    EXPECT_EQ(attempts, 1);
}

TEST_F(AudioDownloads, PartialFailureDoesNotRetryOrCacheAndClearRevokesReaders) {
    std::atomic_bool release{};
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto& cancel,
                             const AudioChunkCallback& emit) -> std::optional<EntryAudio> {
        ++attempts;
        emit("audio/mpeg", "partial");
        while (!release && !cancel()) std::this_thread::sleep_for(2ms);
        throw std::runtime_error("Connection lost");
    });
    ReleaseOnExit cleanup{release};
    downloads->submit_batch(session, input({1}));
    auto stream = downloads->stream(session, 1, "Test");
    ASSERT_TRUE(stream);
    ASSERT_TRUE(eventually([&] { return !stream->empty(); }));
    release = true;
    ASSERT_TRUE(eventually([&] { return stream->read(0)->failed; }));
    ASSERT_TRUE(downloads->audio_chunk(session, 1, "Test", 0));
    EXPECT_TRUE(downloads->audio_chunk(session, 1, "Test", 0)->failed);
    EXPECT_EQ(attempts, 1);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());

    release = false;
    downloads->submit_batch(session, input({2}));
    auto second = downloads->stream(session, 2, "Test");
    ASSERT_TRUE(second);
    downloads->clear(session);
    EXPECT_TRUE(second->read(0)->failed);
    EXPECT_FALSE(downloads->stream(session, 2, "Test"));
}

TEST_F(AudioDownloads, FourAttemptsWithFiftyMillisecondWaits) {
    std::mutex mutex;
    std::vector<std::chrono::steady_clock::time_point> attempts;
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        std::lock_guard lock(mutex);
        attempts.push_back(std::chrono::steady_clock::now());
        throw std::runtime_error("upstream failed");
    });
    downloads->submit_batch(session, input({1}));
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
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        const auto text = request.body.at("text").template get<std::string>();
        if (text == "3" && ++third_attempts == 1) throw std::runtime_error("Retry this transfer");
        if (text == "4") { fourth_started = true; return EntryAudio{"fourth", "audio/mpeg"}; }
        if (text == "1") first_started = true;
        if (text == "2") second_started = true;
        while (!cancel() && !(text == "1" && release_first)) std::this_thread::sleep_for(1ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{"audio", "audio/mpeg"}};
    });
    downloads->submit_batch(session, input({1}));
    downloads->submit_batch(session, input({2}));
    ASSERT_TRUE(eventually([&] { return first_started && second_started; }));
    downloads->submit_batch(session, input({3}));
    ASSERT_TRUE(eventually([&] { return third_attempts == 1; }));
    // Let worker 3 enter its retry wait before worker 1 becomes idle.
    std::this_thread::sleep_for(10ms);
    release_first = true;
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    std::this_thread::sleep_for(5ms);
    downloads->submit_batch(session, input({4}));
    ASSERT_TRUE(eventually([&] { return fourth_started.load(); }));
    EXPECT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(4); }));
}

TEST_F(AudioDownloads, OpusAudioWithParametersIsSavedWithoutRetry) {
    config->apply_voice_output_update({.fishaudio = WorkspaceVoiceProviderOutput{.url = "https://api.fish.audio/v1/tts", .model = "s2.1-pro",
        .api_key_id = config->snapshot()->voice_output()->fishaudio->api_key_id, .output_format = "opus"}, .default_voice = "Reader"});
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        ++attempts;
        EXPECT_EQ(request.body.at("format"), "opus");
        return EntryAudio{"opus-audio", "audio/opus; codecs=opus"};
    });
    downloads->submit_batch(session, input({1}));
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    const auto saved = downloads->audio(session, 1, "Test");
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->content_type, "audio/opus; codecs=opus");
    EXPECT_EQ(attempts, 1);
}

TEST_F(AudioDownloads, DeletedKeyReportsNotConfiguredButCachedAudioStillWorks) {
    std::atomic_int transfers{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        ++transfers;
        return EntryAudio{"audio", "audio/mpeg"};
    });
    keys->remove(config->snapshot()->voice_output()->fishaudio->api_key_id);
    try {
        downloads->submit_batch(session, input({1}));
        FAIL() << "An uncached entry needs a configured API key";
    } catch (const AudioDownloadError& error) {
        EXPECT_EQ(error.status, 404);
        EXPECT_EQ(error.code, "not_found");
        EXPECT_STREQ(error.what(), "Voice output is not configured.");
    }
    const auto entry = sessions->lookup_entry_audio(session, 1, false);
    ASSERT_TRUE(entry);
    sessions->save_entry_audio(*entry, {"cached", "audio/mpeg"});
    EXPECT_EQ(downloads->submit_batch(session, input({1})).at(0).kind, AudioAcceptanceKind::cached);
    EXPECT_EQ(downloads->audio(session, 1, "Test")->audio, "cached");
    EXPECT_EQ(transfers, 0);
}

TEST_F(AudioDownloads, ResolvesCharacterVoiceAndSpeedWhenNoVoiceIsSupplied) {
    const auto voice_id = config->create_voice("Guide voice", "", "guide-ref");
    config->apply_voice_update(voice_id, "Guide voice", "", "guide-ref", {.speed = 1.15});
    const auto workspace_snapshot = config->snapshot();
    const auto* character = workspace_snapshot->find_character("guide");
    ASSERT_NE(character, nullptr);
    config->apply_character_settings("guide", character->provider_id.value_or(""), std::nullopt, voice_id);
    auto downloads = make([](const auto&, const auto&, const auto& request, const auto&,
        const AudioChunkCallback&) -> std::optional<EntryAudio> {
        EXPECT_EQ(request.body.at("reference_id"), "guide-ref");
        EXPECT_EQ(request.body.at("prosody").at("speed"), 1.15);
        return EntryAudio{"audio", "audio/mpeg"};
    });
    downloads->submit_batch(session, {"Test", {{1, {}}}});
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
}

TEST_F(AudioDownloads, ResolvesDefaultVoiceAndDeduplicatesPendingRequests) {
    std::atomic_int transfers{};
    std::atomic_bool release{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel,
        const AudioChunkCallback&) -> std::optional<EntryAudio> {
        ++transfers;
        EXPECT_EQ(request.body.at("reference_id"), "voice");
        while (!release && !cancel()) std::this_thread::sleep_for(2ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{"audio", "audio/mpeg"}};
    });
    ReleaseOnExit cleanup{release};
    EXPECT_EQ(downloads->submit_batch(session, {"Test", {{1, {}}}}).at(0).kind, AudioAcceptanceKind::queued);
    ASSERT_TRUE(eventually([&] { return transfers.load() == 1; }));
    EXPECT_EQ(downloads->submit_batch(session, {"Test", {{1, {}}}}).at(0).kind, AudioAcceptanceKind::running);
    EXPECT_EQ(downloads->status(session, "Test").downloads.size(), 1u);
    release = true;
    // The worker saves the audio before it removes the finished job.
    ASSERT_TRUE(eventually([&] {
        return downloads->submit_batch(session, {"Test", {{1, {}}}}).at(0).kind == AudioAcceptanceKind::cached;
    }));
    EXPECT_EQ(transfers, 1);
}
TEST_F(AudioDownloads, DisabledDownloadsReportEmptyStatusButRejectWork) {
    AudioDownloadManager downloads(
        *sessions, [this] { return vault->get().name; }, false);
    for (const std::string name : {"Test", "Other"}) {
        const auto status = downloads.status(session, name);
        EXPECT_TRUE(status.cached_entry_ids.empty());
        EXPECT_TRUE(status.downloads.empty());
    }
    EXPECT_THROW(downloads.submit_batch(session, input({1})), AudioDownloadError);
}
TEST_F(AudioDownloads, DeletedQueuedEntryIsDroppedBeforeTransfer) {
    std::mutex mutex;
    std::set<std::string> started;
    std::atomic_bool release{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        {
            std::lock_guard lock(mutex);
            started.insert(request.body.at("text").template get<std::string>());
        }
        while (!release && !cancel()) std::this_thread::sleep_for(2ms);
        return cancel() ? std::nullopt : std::optional<EntryAudio>{{"audio", "audio/mpeg"}};
    });
    downloads->submit_batch(session, input({1, 2, 3, 4}));
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
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        if (++attempts < 4) throw std::runtime_error("upstream failed");
        return EntryAudio{"audio", "audio/mpeg"};
    });
    downloads->submit_batch(session, input({1}));
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    EXPECT_EQ(attempts, 4);
}
TEST_F(AudioDownloads, CancellationStopsStalledTransferAndPreservesNewWork) {
    std::atomic_bool started{}, canceled{};
    auto downloads = make([&](const auto&, const auto&, const auto& request, const auto& cancel, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        if (request.body.at("text") == "2") return EntryAudio{"new audio", "audio/mpeg"};
        started = true;
        while (!cancel()) std::this_thread::sleep_for(2ms);
        canceled = true;
        return std::nullopt;
    });
    downloads->submit_batch(session, input({1}));
    ASSERT_TRUE(eventually([&] { return started.load(); }));
    downloads->pause(); downloads->resume();
    ASSERT_TRUE(eventually([&] { return canceled.load(); }));
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    downloads->submit_batch(session, input({2}));
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(2); }));
    EXPECT_FALSE(sessions->cached_audio_entries(session).contains(1));
    downloads->request_stop();
    EXPECT_TRUE(downloads->join_until(std::chrono::steady_clock::now() + 2s));
}
TEST_F(AudioDownloads, CancellationWakesRetryWait) {
    std::atomic_int attempts{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        ++attempts;
        throw std::runtime_error("upstream failed");
    });
    downloads->submit_batch(session, input({1}));
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
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        ++transfers; return EntryAudio{"audio", "audio/mpeg"};
    });
    downloads->submit_batch(session, input({1}));
    ASSERT_TRUE(eventually([&] {
        const auto status = downloads->status(session, "Test");
        return !status.downloads.empty() && status.downloads[0].state == AudioJobState::failed;
    }));
    EXPECT_EQ(transfers, 1);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    database.execute("DROP TRIGGER reject_audio");
    const auto retry = downloads->submit_batch(session, input({1})).at(0);
    EXPECT_EQ(retry.kind, AudioAcceptanceKind::queued);
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    EXPECT_EQ(transfers, 2);
}
TEST_F(AudioDownloads, ClearCancelsOldCompletionWithoutReplacingNewJob) {
    std::atomic_int transfers{};
    std::atomic_bool release{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        ++transfers;
        while (!release) std::this_thread::sleep_for(2ms);
        return EntryAudio{"audio", "audio/mpeg"}; // Simulate a result racing cancellation.
    });
    ReleaseOnExit cleanup{release};
    downloads->submit_batch(session, input({1}));
    ASSERT_TRUE(eventually([&] { return transfers == 1; }));
    downloads->clear(session);
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    downloads->submit_batch(session, input({1}));
    ASSERT_TRUE(eventually([&] { return transfers == 2; }));
    release = true;
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    EXPECT_EQ(transfers, 2);
}
TEST_F(AudioDownloads, DeletedEntryCannotSaveLateAudio) {
    std::atomic_bool started{}, release{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        started = true;
        while (!release) std::this_thread::sleep_for(2ms);
        return EntryAudio{"audio", "audio/mpeg"};
    });
    ReleaseOnExit cleanup{release};
    downloads->submit_batch(session, input({1}));
    ASSERT_TRUE(eventually([&] { return started.load(); }));
    const auto stream = downloads->stream(session, 1, "Test");
    ASSERT_TRUE(stream);
    {
        storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
        database.execute("DELETE FROM entries WHERE entry_id = 1");
    }
    release = true;
    ASSERT_TRUE(eventually([&] { return downloads->status(session, "Test").downloads.empty(); }));
    EXPECT_TRUE(sessions->cached_audio_entries(session).empty());
    EXPECT_TRUE(stream->read(0)->failed);
    EXPECT_FALSE(stream->read(0)->complete);
}
TEST_F(AudioDownloads, OldVaultCompletionCannotSaveAfterSwitchingAwayAndBack) {
    std::atomic_bool started{}, cancelled{}, release{};
    auto downloads = make([&](const auto&, const auto&, const auto&, const auto& cancel, const AudioChunkCallback&) -> std::optional<EntryAudio> {
        started = true;
        while (!cancel()) std::this_thread::sleep_for(2ms);
        cancelled = true;
        while (!release) std::this_thread::sleep_for(2ms);
        return EntryAudio{"old-vault-audio", "audio/mpeg"};
    });
    ReleaseOnExit cleanup{release};
    downloads->submit_batch(session, input({1}));
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
    auto downloads = make([](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&) -> std::optional<EntryAudio> {
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

TEST_F(AudioDownloads, UncachedAudioRejectsInvalidProviders) {
    auto downloads = make([](const auto&, const auto&, const auto&, const auto&, const AudioChunkCallback&)
        -> std::optional<EntryAudio> { throw std::runtime_error("Invalid input must not reach transport"); });
    for (const Json& provider : {Json(1), Json(nullptr), Json("eleven")}) {
        const auto synthesis = decode_voice_synthesis({{"provider", provider}});
        EXPECT_THROW(downloads->submit_batch(session, {"Test", {{1, synthesis}}}), std::invalid_argument);
    }
}

TEST_F(AudioDownloads, DefaultVoiceSettingsApplyWhenNoVoiceIsRequested) {
    const auto output = config->snapshot()->voice_output();
    ASSERT_TRUE(output);
    const auto* voice = config->snapshot()->find_voice_by_name(output->default_voice);
    ASSERT_NE(voice, nullptr);
    config->apply_voice_update(voice->id, voice->label, voice->description, voice->elevenlabs_voice_id,
        {.speed = 0.8}, voice->provider);
    auto downloads = make([](const auto&, const auto&, const auto& request, const auto&,
        const AudioChunkCallback&) -> std::optional<EntryAudio> {
        EXPECT_EQ(request.body.at("prosody").at("speed"), 0.8);
        return EntryAudio{"audio", "audio/mpeg"};
    });
    downloads->submit_batch(session, {"Test", {{1, {}}, {2, {}}}});
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).size() == 2; }));
}

TEST_F(AudioDownloads, SelectsTheCharacterProviderBeforeItsCredentialAndKeepsFishAudioVoices) {
    const auto eleven_key = keys->create("ElevenLabs", "eleven-secret");
    auto output = *config->snapshot()->voice_output();
    output.elevenlabs = WorkspaceVoiceProviderOutput{.url = "https://api.elevenlabs.io/v1/text-to-speech",
        .model = "eleven_multilingual_v2", .api_key_id = eleven_key.id, .output_format = "mp3_44100_128"};
    config->apply_voice_output_update(output);
    const auto voice = config->create_voice("Eleven Guide", "", "eleven-ref", "elevenlabs");
    config->apply_voice_update(voice, "Eleven Guide", "", "eleven-ref", {.speed = 0.85}, "elevenlabs");
    const auto snapshot = config->snapshot();
    config->apply_character_settings("guide", snapshot->find_character("guide")->provider_id.value_or(""),
        std::nullopt, voice);
    auto downloads = make([](const auto& selected, const auto& key, const auto& request, const auto&,
        const AudioChunkCallback&) -> std::optional<EntryAudio> {
        EXPECT_EQ(request.provider, "elevenlabs");
        EXPECT_EQ(key, "eleven-secret");
        EXPECT_EQ(selected.model, "eleven_multilingual_v2");
        EXPECT_NE(request.url.find("/eleven-ref/stream?"), std::string::npos);
        EXPECT_EQ(request.body.at("voice_settings").at("speed"), 0.85);
        EXPECT_FALSE(request.body.contains("reference_id"));
        return EntryAudio{"eleven-audio", "audio/mpeg"};
    });
    downloads->submit_batch(session, {"Test", {{1, {}}}});
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(1); }));
    EXPECT_EQ(downloads->audio(session, 1, "Test")->audio, "eleven-audio");
    // An explicit FishAudio voice still uses the original key and request format.
    auto fish = make([](const auto&, const auto& key, const auto& request, const auto&,
        const AudioChunkCallback&) -> std::optional<EntryAudio> {
        EXPECT_EQ(request.provider, "fishaudio");
        EXPECT_EQ(key, "secret");
        EXPECT_EQ(request.body.at("reference_id"), "voice");
        return EntryAudio{"fish-audio", "audio/mpeg"};
    });
    fish->submit_batch(session, input({2}));
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(2); }));
}

TEST_F(AudioDownloads, LiveSpeechPublishesAudioBeforePersistenceAndCachesTheFlushedClip) {
    auto output = *config->snapshot()->voice_output();
    output.fishaudio->connection = "websocket";
    config->apply_voice_output_update(output);
    std::atomic_bool started{false}, first{false}, drained{false};
    std::string spoken;
    AudioDownloadManager downloads(*sessions, [this] { return vault->get().name; }, true,
        download_voice_output, [&](const auto&, const auto&, const auto& request, const auto& cancel,
            const AudioChunkCallback& audio, const VoiceTextInput& input) -> std::optional<EntryAudio> {
            EXPECT_EQ(request.body.at("text"), "");
            started = true;
            while (!cancel()) {
                auto text = input();
                spoken += text.text;
                if (!text.text.empty() && !first) { audio("audio/mpeg", "first-"); first = true; }
                if (text.complete) {
                    audio("audio/mpeg", "last");
                    drained = true;
                    return EntryAudio{"first-last", "audio/mpeg"};
                }
                std::this_thread::sleep_for(2ms);
            }
            return {};
        });
    auto reply = make_character_entry(6, "guide", "Guide", "", EntryStatus::streaming);
    downloads.submit_batch(session, input({6}), std::span(&reply, 1));
    ASSERT_TRUE(eventually([&] { return started.load(); }));
    EXPECT_FALSE(first);
    reply.text = "First sentence. **Bold";
    downloads.update_live(session, std::span(&reply, 1));
    ASSERT_TRUE(eventually([&] { return first.load(); }));
    EXPECT_FALSE(drained);
    EXPECT_FALSE(sessions->lookup_entry_audio(session, 6));
    auto stream = downloads.stream(session, 6, "Test");
    ASSERT_TRUE(stream);
    EXPECT_EQ(stream->read(0)->body, "first-");
    EXPECT_FALSE(stream->read(0)->complete);
    reply.text += " across\nchunks.** Final.";
    downloads.update_live(session, std::span(&reply, 1));
    reply.status = EntryStatus::complete;
    SessionJournal journal(path, sessions->prepare(session).session_key);
    journal.record_entry(reply);
    downloads.update_live(session, std::span(&reply, 1));
    ASSERT_TRUE(eventually([&] { return sessions->cached_audio_entries(session).contains(6); }));
    downloads.request_stop();
    ASSERT_TRUE(downloads.join_until(std::chrono::steady_clock::now() + 2s));
    EXPECT_EQ(spoken, "First sentence. Bold across\nchunks. Final.");
    EXPECT_EQ(sessions->lookup_entry_audio(session, 6)->cached->audio, "first-last");
    EXPECT_TRUE(stream->read(0)->complete);
    EXPECT_FALSE(stream->read(0)->failed);
}

TEST_F(AudioDownloads, LiveSpeechRejectsRewrittenCleanedTextEvenWhenItGrows) {
    auto output = *config->snapshot()->voice_output();
    output.fishaudio->connection = "websocket";
    config->apply_voice_output_update(output);
    auto reply = make_character_entry(6, "guide", "Guide", "Intro. *emphasis * ", EntryStatus::streaming);
    const auto first_text = speech_text_prefix(reply.text, "fishaudio", false);
    const auto final_text = reply.text + "tail* More words than before.";
    const auto final_speech = speech_text_prefix(final_text, "fishaudio", true);
    ASSERT_GE(final_speech.size(), first_text.size());
    ASSERT_FALSE(final_speech.starts_with(first_text));
    std::atomic_bool first{false}, release{false};
    std::atomic_int calls{0};
    AudioDownloadManager downloads(*sessions, [this] { return vault->get().name; }, true,
        download_voice_output, [&](const auto&, const auto&, const auto&, const auto& cancel,
            const AudioChunkCallback& audio, const VoiceTextInput& input) -> std::optional<EntryAudio> {
            ++calls;
            EXPECT_EQ(input().text, first_text);
            audio("audio/mpeg", "partial");
            first = true;
            while (!release && !cancel()) std::this_thread::sleep_for(2ms);
            if (cancel()) return {};
            try { (void)input(); }
            catch (const std::runtime_error& error) {
                EXPECT_STREQ(error.what(), "Speech text changed during synthesis.");
                throw;
            }
            ADD_FAILURE() << "A rewritten speech prefix must stop synthesis";
            return EntryAudio{"partial", "audio/mpeg"};
        });
    ReleaseOnExit cleanup{release};
    downloads.submit_batch(session, input({6}), std::span(&reply, 1));
    ASSERT_TRUE(eventually([&] { return first.load(); }));
    const auto stream = downloads.stream(session, 6, "Test");
    ASSERT_TRUE(stream);
    reply.text = final_text;
    reply.status = EntryStatus::complete;
    SessionJournal journal(path, sessions->prepare(session).session_key);
    journal.record_entry(reply);
    downloads.update_live(session, std::span(&reply, 1));
    release = true;
    ASSERT_TRUE(eventually([&] {
        const auto state = downloads.status(session, "Test");
        return !state.downloads.empty() && state.downloads[0].state == AudioJobState::failed;
    }));
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(stream->read(0)->failed);
    EXPECT_FALSE(stream->read(0)->complete);
    EXPECT_FALSE(sessions->cached_audio_entries(session).contains(6));
}

TEST_F(AudioDownloads, LiveSpeechCancellationAndVaultChangesRevokeAudioWithoutCaching) {
    auto output = *config->snapshot()->voice_output();
    output.fishaudio->connection = "websocket";
    config->apply_voice_output_update(output);
    for (const auto action : {"cancel", "fail", "remove", "edit", "vault", "clear"}) {
        SCOPED_TRACE(action);
        std::atomic_bool first{false}, stopped{false};
        AudioDownloadManager downloads(*sessions, [this] { return vault->get().name; }, true,
            download_voice_output, [&](const auto&, const auto&, const auto&, const auto& cancel,
                const AudioChunkCallback& audio, const VoiceTextInput&) -> std::optional<EntryAudio> {
                audio("audio/mpeg", "partial"); first = true;
                while (!cancel()) std::this_thread::sleep_for(2ms);
                stopped = true;
                return EntryAudio{"late", "audio/mpeg"};
            });
        auto reply = make_character_entry(6, "guide", "Guide", "Hello. More", EntryStatus::streaming, 10);
        downloads.submit_batch(session, input({6}), std::span(&reply, 1));
        ASSERT_TRUE(eventually([&] { return first.load(); }));
        auto stream = downloads.stream(session, 6, "Test");
        ASSERT_TRUE(stream);
        if (std::string_view(action) == "vault") {
            downloads.pause();
            vault->set(VaultDefinition{.name = "Other", .data = path});
            vault->set(VaultDefinition{.name = "Test", .data = path});
            downloads.resume();
        } else if (std::string_view(action) == "clear") downloads.clear(session);
        else if (std::string_view(action) == "remove") downloads.update_live(session, {});
        else {
            if (std::string_view(action) == "edit") reply.text = "Changed";
            else reply.status = std::string_view(action) == "cancel" ? EntryStatus::cancelled : EntryStatus::failed;
            downloads.update_live(session, std::span(&reply, 1));
        }
        EXPECT_TRUE(eventually([&] { return stopped.load(); }));
        downloads.request_stop();
        ASSERT_TRUE(downloads.join_until(std::chrono::steady_clock::now() + 2s));
        EXPECT_TRUE(stream->read(0)->failed);
        EXPECT_TRUE(stream->read(0)->body.empty());
        EXPECT_FALSE(sessions->cached_audio_entries(session).contains(6));
    }
}

TEST_F(AudioDownloads, LiveProviderFailureDoesNotRestartPublishedAudio) {
    auto output = *config->snapshot()->voice_output();
    output.fishaudio->connection = "websocket";
    config->apply_voice_output_update(output);
    std::atomic_int calls{0};
    AudioDownloadManager downloads(*sessions, [this] { return vault->get().name; }, true,
        download_voice_output, [&](const auto&, const auto&, const auto&, const auto&,
            const AudioChunkCallback& audio, const VoiceTextInput&) -> std::optional<EntryAudio> {
            ++calls;
            audio("audio/mpeg", "partial");
            throw std::runtime_error("Disconnected");
        });
    auto reply = make_character_entry(6, "guide", "Guide", "Hello.", EntryStatus::streaming, 10);
    downloads.submit_batch(session, input({6}), std::span(&reply, 1));
    ASSERT_TRUE(eventually([&] {
        const auto state = downloads.status(session, "Test");
        return !state.downloads.empty() && state.downloads[0].state == AudioJobState::failed;
    }));
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(downloads.audio_chunk(session, 6, "Test", 0)->failed);
    EXPECT_FALSE(sessions->cached_audio_entries(session).contains(6));
}

TEST_F(AudioDownloads, LiveHttpRequestsWaitForCompletedReplies) {
    auto reply = make_character_entry(6, "guide", "Guide", "Hello.", EntryStatus::streaming, 10);
    auto downloads = make([](const auto&, const auto&, const auto&, const auto&, const auto&) -> std::optional<EntryAudio> {
        ADD_FAILURE() << "HTTP must not start for an unfinished reply";
        return {};
    });
    EXPECT_THROW(downloads->submit_batch(session, input({6}), std::span(&reply, 1)), std::invalid_argument);
}

}
}
