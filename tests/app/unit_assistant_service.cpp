#include "app/assistant_service.h"

#include "providers/maintenance.h"
#include "providers/openai_oauth.h"
#include "support/test_workspace.h"
#include "util/logging.h"
#include "workspace/builtins.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

std::string file_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

std::string reference_from_sources(bool chaweb_host) {
    const std::filesystem::path root{CHA_SOURCE_DIRECTORY};
    std::string text = chaweb_host
        ? "Host: cha-daemon (ChaWeb)\n\n"
        : "Host: desktop application\n\n";
    text += file_text(root / "resources" / "assistant-maintenance.md");
    text.push_back('\n');
    text += file_text(root / "docs" / "MaintainerGuide.md");
    text.push_back('\n');
    text += file_text(root / "packaging" / "linux" / "README.md");
    return text;
}

MaintenanceContext welcome_context(std::uint64_t epoch = 1) {
    return {
        .character_id = std::string(workspace_assistant_id),
        .forum_id = std::string(workspace_entrance_id),
        .session_id = std::string(welcome_id),
        .request_id = 7,
        .context_epoch = epoch,
    };
}

// One imported vault, its store, and the process log buffer for one test.
class ServiceHarness {
public:
    ServiceHarness() {
        shutdown_diagnostic_logging();
        log_directory_ = std::filesystem::temp_directory_path()
            / ("cha_assistant_service_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(log_directory_);
        initialize_diagnostic_logging(log_directory_ / "cha.log", "off");
        database_ = test::import_test_database(workspace_.root());
        store_ = WorkspaceConfigStore::open(database_);
        oauth_path_ = log_directory_ / "openai-auth.json";
        oauth_ = std::make_unique<OpenAiOAuth>(oauth_path_);
        bind();
    }

    ~ServiceHarness() {
        service_.reset();
        oauth_.reset();
        store_.reset();
        set_diagnostic_log_clock_for_test({});
        shutdown_diagnostic_logging();
        std::error_code ignored;
        std::filesystem::remove_all(log_directory_, ignored);
    }

    ServiceHarness(const ServiceHarness&) = delete;
    ServiceHarness& operator=(const ServiceHarness&) = delete;

    WorkspaceConfigStore& store() { return *store_; }
    std::timed_mutex& lifecycle() { return lifecycle_; }
    std::atomic_bool& stopping() { return stopping_; }
    std::atomic_bool& cancelled() { return cancelled_; }
    MaintenanceContext& context() { return context_; }

    // Admission runs first after the lifecycle lock, then again after the read.
    void fail_after_lock() { fail_after_ = admit_count_ + 1; }

    void deny_after_read(ErrorCode code) {
        fail_after_ = admit_count_ + 2;
        fail_code_ = code;
    }

    void use_oauth(std::string_view json) {
        {
            std::ofstream file(oauth_path_, std::ios::binary);
            file << json;
        }
        oauth_ = std::make_unique<OpenAiOAuth>(oauth_path_);
        bind();
    }

    Json run(std::string_view name, const Json& arguments) {
        return Json::parse(service_->execute(
            name, arguments.dump(), context_, cancelled_));
    }

private:
    void bind() {
        service_ = std::make_unique<AssistantService>(AssistantService::Links{
            .store = store_.get(),
            .oauth = oauth_.get(),
            .lifecycle = &lifecycle_,
            .stopping = &stopping_,
            .admit = [this](std::uint64_t epoch) -> std::optional<ErrorCode> {
                const int seen = ++admit_count_;
                if (fail_after_ != 0 && seen >= fail_after_) return fail_code_;
                if (epoch != admitted_epoch_) return ErrorCode::vault_changed;
                return std::nullopt;
            },
        });
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::filesystem::path log_directory_;
    std::filesystem::path oauth_path_;
    std::unique_ptr<WorkspaceConfigStore> store_;
    std::unique_ptr<OpenAiOAuth> oauth_;
    std::unique_ptr<AssistantService> service_;
    std::timed_mutex lifecycle_;
    std::atomic_bool stopping_{false};
    std::atomic_bool cancelled_{false};
    MaintenanceContext context_ = welcome_context();
    std::uint64_t admitted_epoch_{1};
    int admit_count_{};
    int fail_after_{};
    ErrorCode fail_code_{ErrorCode::vault_changed};
};

Json read_arguments(std::string path) {
    return {{"paths", Json::array({std::move(path)})}};
}

Json logs_arguments(
    std::optional<std::uint64_t> after,
    const char* level,
    const char* contains,
    int limit) {
    return {
        {"after", after ? Json(*after) : Json(nullptr)},
        {"minimum_level", level ? Json(level) : Json(nullptr)},
        {"contains", contains ? Json(contains) : Json(nullptr)},
        {"limit", limit},
    };
}

Json replace_change(std::string path, std::string content, std::string version) {
    return {
        {"action", "apply"},
        {"version", std::move(version)},
        {"changes", Json::array({Json{
            {"path", std::move(path)},
            {"operation", "replace"},
            {"content", std::move(content)},
        }})},
    };
}

const std::string kGuide = "characters/guide/character.toml";
const std::string kPrompt = "characters/guide/CHARACTER.md";

TEST(AssistantReference, ConcatenatesBothHostLinesAndTheThreeDocuments) {
    const std::filesystem::path root{CHA_SOURCE_DIRECTORY};
    const std::string desktop = maintenance_reference(false);
    const std::string chaweb = maintenance_reference(true);
    EXPECT_EQ(desktop, reference_from_sources(false));
    EXPECT_EQ(chaweb, reference_from_sources(true));
    EXPECT_EQ(desktop.compare(0, std::string("Host: desktop application\n\n").size(),
        "Host: desktop application\n\n"), 0);
    EXPECT_EQ(chaweb.compare(0, std::string("Host: cha-daemon (ChaWeb)\n\n").size(),
        "Host: cha-daemon (ChaWeb)\n\n"), 0);
    EXPECT_NE(chaweb.find(
        "Host: cha-daemon (ChaWeb)\n\n# Operating instructions for Assistant"),
        std::string::npos);
    EXPECT_EQ(desktop.find(
        "Host: cha-daemon (ChaWeb)\n\n# Operating instructions for Assistant"),
        std::string::npos);
    EXPECT_NE(desktop.find("# CHA workspace maintainer guide"), std::string::npos);
    EXPECT_NE(desktop.find("# CHA daemon on Linux"), std::string::npos);
    EXPECT_NE(desktop.find("## Four recipes"), std::string::npos);
    const std::string guide = file_text(root / "resources" / "application-guide.md");
    EXPECT_EQ(guide.compare(0, std::string("# CHA application guide").size(),
        "# CHA application guide"), 0);
    EXPECT_EQ(guide.find("# Operating instructions for Assistant"), std::string::npos);
    EXPECT_EQ(desktop.find("# CHA application guide"), std::string::npos);
}

TEST(AssistantService, RejectsOversizedArgumentsBeforeIdentity) {
    ServiceHarness harness;
    auto wrong = welcome_context();
    wrong.character_id = "guide";
    const Json rejected = Json::parse(
        AssistantService{AssistantService::Links{
            .store = &harness.store(),
            .lifecycle = &harness.lifecycle(),
            .stopping = &harness.stopping(),
            .admit = [](std::uint64_t) -> std::optional<ErrorCode> {
                return std::nullopt;
            },
        }}.execute(
            "vault_config_list",
            std::string(maintenance_argument_limit + 1, 'x'),
            wrong,
            harness.cancelled()));
    EXPECT_EQ(rejected["error"], "too_large");
    EXPECT_EQ(rejected["committed"], false);
    EXPECT_EQ(rejected["message"], "Tool arguments are too large.");
}

TEST(AssistantService, RejectsTheWrongSessionAndAStaleEpoch) {
    ServiceHarness harness;
    harness.context().session_id = "lobby-session";
    const Json mismatch = harness.run("vault_config_list", {{"prefix", nullptr}});
    EXPECT_EQ(mismatch["error"], "unavailable");
    EXPECT_EQ(mismatch["message"],
        "This maintenance tool is not available for this request.");
    EXPECT_EQ(mismatch["committed"], false);

    harness.context() = welcome_context(4);
    const Json stale = harness.run("vault_config_list", {{"prefix", nullptr}});
    EXPECT_EQ(stale["error"], "stale_context");
    EXPECT_EQ(stale["message"],
        "The vault context changed. Read the configuration again.");

    harness.context() = welcome_context();
    harness.fail_after_lock();
    const Json after_lock = harness.run("vault_config_list", {{"prefix", nullptr}});
    EXPECT_EQ(after_lock["error"], "stale_context");
    EXPECT_EQ(harness.store().config_revision(), 1u);
}

TEST(AssistantService, MapsACompletedReadDenialToItsAdmitCode) {
    ServiceHarness unavailable;
    unavailable.deny_after_read(ErrorCode::application_unavailable);
    const Json denied = unavailable.run("vault_config_list", {{"prefix", nullptr}});
    EXPECT_EQ(denied["error"], "unavailable");
    EXPECT_EQ(denied["message"], "The application is unavailable.");
    EXPECT_EQ(denied["committed"], false);

    ServiceHarness changed;
    changed.deny_after_read(ErrorCode::vault_changed);
    const Json stale = changed.run("vault_config_list", {{"prefix", nullptr}});
    EXPECT_EQ(stale["error"], "stale_context");
    EXPECT_EQ(stale["message"],
        "The vault context changed. Read the configuration again.");
    EXPECT_EQ(stale["committed"], false);
}

TEST(AssistantService, CancellationLeavesBeforeTheLifecycleLock) {
    ServiceHarness harness;
    harness.stopping() = true;
    const Json stopped = harness.run("assistant_logs", logs_arguments(std::nullopt, nullptr, nullptr, 1));
    EXPECT_EQ(stopped["error"], "cancelled");
    EXPECT_EQ(stopped["message"], "The maintenance tool was cancelled.");

    harness.stopping() = false;
    harness.lifecycle().lock();
    auto pending = std::async(std::launch::async, [&harness] {
        return harness.run("vault_config_list", {{"prefix", nullptr}});
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    harness.cancelled() = true;
    const auto status = pending.wait_for(std::chrono::seconds(2));
    harness.lifecycle().unlock();
    ASSERT_EQ(status, std::future_status::ready)
        << "The tool kept waiting after cancellation";
    const Json cancelled = pending.get();
    EXPECT_EQ(cancelled["error"], "cancelled");
    EXPECT_EQ(cancelled["committed"], false);
}

TEST(AssistantService, ListsReadsAndReportsKeyMetadata) {
    ServiceHarness harness;
    ASSERT_EQ(harness.store().config_revision(), 1u);
    harness.store().apply_api_key_create("api_key_1", "Secret", "private-value");
    const std::string version = std::to_string(harness.store().config_revision());
    const Json listed = harness.run("vault_config_list", {{"prefix", "characters/guide"}});
    EXPECT_EQ(listed["version"], version);
    ASSERT_TRUE(listed["entries"].is_array());
    bool saw_guide = false;
    for (const Json& entry : listed["entries"]) {
        EXPECT_TRUE(entry["path"].get<std::string>().starts_with("characters/guide"));
        if (entry["path"] == kGuide) saw_guide = true;
    }
    EXPECT_TRUE(saw_guide);

    const Json read = harness.run("vault_config_read", Json{
        {"paths", Json::array({kGuide, "missing.toml", "system/keys/api_key_1/config.toml"})},
    });
    EXPECT_EQ(read["version"], version);
    ASSERT_EQ(read["files"].size(), 3u);
    EXPECT_EQ(read["files"][0]["status"], "ok");
    EXPECT_EQ(read["files"][0]["content"], "display_name = \"Guide\"\nprovider = \"test\"\n");
    EXPECT_EQ(read["files"][1]["status"], "missing");
    EXPECT_FALSE(read["files"][1].contains("content"));
    EXPECT_EQ(read["files"][2]["status"], "key_metadata");
    EXPECT_FALSE(read["files"][2].contains("content"));
    EXPECT_EQ(read["files"][2]["key"]["display_name"], "Secret");
    EXPECT_EQ(read["files"][2]["key"]["credential_present"], true);
    EXPECT_EQ(read.dump().find("private-value"), std::string::npos);

    const Json unknown = harness.run("vault_config_explore", {{"prefix", nullptr}});
    EXPECT_EQ(unknown["error"], "unknown_tool");
    const Json malformed = Json::parse(
        AssistantService{AssistantService::Links{
            .store = &harness.store(),
            .lifecycle = &harness.lifecycle(),
            .stopping = &harness.stopping(),
            .admit = [](std::uint64_t) -> std::optional<ErrorCode> {
                return std::nullopt;
            },
        }}.execute("vault_config_list", "not-json", welcome_context(), harness.cancelled()));
    EXPECT_EQ(malformed["error"], "invalid_argument");
}

TEST(AssistantService, ReadMarksAFileTooLargeWhenItsEscapedTextDoesNotFit) {
    ServiceHarness harness;
    // Each quote doubles in JSON. Three raw files fit the store budget, but
    // only two fit the encoded tool result.
    const std::string quoted(60 * 1024, '"');
    std::vector<WorkspaceConfigChange> changes;
    std::vector<std::string> paths;
    for (const char* name : {"one", "two", "three"}) {
        paths.push_back("characters/guide/" + std::string(name) + ".md");
        changes.push_back({
            .path = paths.back(),
            .operation = WorkspaceConfigOperation::create,
            .content = quoted,
        });
    }
    const auto created = harness.store().apply_config(
        harness.store().config_revision(), changes);
    ASSERT_TRUE(created.committed) << created.error_message;

    const std::string encoded = AssistantService{AssistantService::Links{
        .store = &harness.store(),
        .lifecycle = &harness.lifecycle(),
        .stopping = &harness.stopping(),
        .admit = [](std::uint64_t) -> std::optional<ErrorCode> { return std::nullopt; },
    }}.execute(
        "vault_config_read", Json{{"paths", paths}}.dump(), welcome_context(),
        harness.cancelled());
    EXPECT_LE(encoded.size(), maintenance_call_result_limit);
    const Json read = Json::parse(encoded);
    ASSERT_EQ(read["files"].size(), 3u);
    EXPECT_EQ(read["files"][0]["status"], "ok");
    EXPECT_EQ(read["files"][0]["content"], quoted);
    EXPECT_EQ(read["files"][1]["status"], "ok");
    EXPECT_EQ(read["files"][2]["status"], "too_large");
    EXPECT_FALSE(read["files"][2].contains("content"));
}

TEST(AssistantService, ExercisesTheFourMaintenanceRecipes) {
    ServiceHarness harness;
    const auto revision = [&] { return std::to_string(harness.store().config_revision()); };
    log_info("guide diagnosis marker");
    const Json diagnosis = harness.run("vault_config_read", Json{
        {"paths", Json::array({kGuide, "system/providers/test/config.toml"})},
    });
    EXPECT_EQ(diagnosis["version"], "1");
    EXPECT_EQ(diagnosis["files"][0]["status"], "ok");
    EXPECT_NE(diagnosis["files"][0]["content"].get<std::string>().find("Guide"), std::string::npos);
    EXPECT_EQ(diagnosis["files"][1]["status"], "ok");
    const Json found = harness.run(
        "assistant_logs", logs_arguments(std::nullopt, nullptr, "guide diagnosis", 5));
    ASSERT_FALSE(found["entries"].empty());
    EXPECT_NE(found["entries"].back()["text"].get<std::string>().find("guide diagnosis marker"),
        std::string::npos);
    EXPECT_EQ(found["lost"], false);
    EXPECT_EQ(harness.store().config_revision(), 1u);

    const std::string original = diagnosis["files"][0]["content"].get<std::string>();
    std::string renamed = original;
    const auto name = renamed.find("Guide");
    ASSERT_NE(name, std::string::npos);
    renamed.replace(name, 5, "Guide renamed");
    const Json applied = harness.run(
        "vault_config_apply", replace_change(kGuide, renamed, revision()));
    EXPECT_EQ(applied["committed"], true);
    EXPECT_EQ(applied["error"], nullptr);
    EXPECT_EQ(applied["message"], "Configuration was saved.");
    EXPECT_EQ(applied["undo_available"], true);
    EXPECT_EQ(applied["version"], "2");
    ASSERT_EQ(applied["changed"].size(), 1u);
    EXPECT_EQ(applied["changed"][0]["old_bytes"], original.size());
    EXPECT_EQ(applied["changed"][0]["new_bytes"], renamed.size());
    EXPECT_EQ(harness.store().snapshot()->find_character("guide")->character.display_name,
        "Guide renamed");
    EXPECT_NE(harness.store().snapshot()->find_forum_member("lobby", "guide"), nullptr);

    const Json same = harness.run(
        "vault_config_apply", replace_change(kGuide, renamed, revision()));
    EXPECT_EQ(same["committed"], true);
    EXPECT_TRUE(same["changed"].empty());
    EXPECT_EQ(same["message"], "No configuration changes were necessary.");
    EXPECT_EQ(same["undo_available"], true);
    EXPECT_EQ(harness.store().config_revision(), 2u);

    const Json undone = harness.run("vault_config_apply", Json{
        {"action", "undo"},
        {"version", revision()},
        {"changes", nullptr},
    });
    EXPECT_EQ(undone["committed"], true);
    EXPECT_EQ(undone["message"], "Configuration was saved.");
    EXPECT_EQ(harness.store().snapshot()->find_character("guide")->character.display_name, "Guide");

    const auto prompt = harness.run("vault_config_read", read_arguments(kPrompt));
    const std::string prompt_text = prompt["files"][0]["content"].get<std::string>();
    std::string same_length = prompt_text;
    ASSERT_GE(same_length.size(), 4u);
    same_length.replace(0, 4, "XXXX");
    ASSERT_EQ(same_length.size(), prompt_text.size());
    std::string warned = original;
    warned += "legacy_flag = true\n";
    const Json warned_apply = harness.run("vault_config_apply", Json{
        {"action", "apply"},
        {"version", revision()},
        {"changes", Json::array({
            Json{{"path", kGuide}, {"operation", "replace"}, {"content", warned}},
            Json{{"path", kPrompt}, {"operation", "replace"}, {"content", same_length}},
        })},
    });
    EXPECT_EQ(warned_apply["committed"], true);
    EXPECT_EQ(warned_apply["changed"][1]["old_bytes"], prompt_text.size());
    EXPECT_EQ(warned_apply["changed"][1]["new_bytes"], same_length.size());
    bool saw_legacy = false;
    for (const Json& warning : warned_apply["warnings"]) {
        EXPECT_EQ(warning["path"], kGuide);
        const std::string message = warning["message"].get<std::string>();
        if (message.find("legacy_flag") != std::string::npos) {
            saw_legacy = true;
            EXPECT_NE(message.find("[path]"), std::string::npos);
            EXPECT_EQ(message.find(harness.store().workspace_path().string()), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_legacy);

    const Json broken = harness.run(
        "vault_config_apply",
        replace_change(kGuide, "display_name = \"Broken\n", revision()));
    EXPECT_EQ(broken["committed"], false);
    EXPECT_EQ(broken["error"], "validation_failure");
    EXPECT_EQ(harness.store().read_config(std::vector<std::string>{kPrompt}).files[0].content,
        same_length);

    const Json protected_assistant = harness.run(
        "vault_config_apply",
        replace_change("system/assistant/character.toml", original, revision()));
    EXPECT_EQ(protected_assistant["error"], "protected_path");
    EXPECT_EQ(protected_assistant["committed"], false);
    const Json protected_provider = harness.run(
        "vault_config_apply",
        replace_change("system/providers/test/config.toml", "mode = \"test\"\n", revision()));
    EXPECT_EQ(protected_provider["error"], "protected_path");
    EXPECT_NE(protected_provider["message"].get<std::string>().find("read-only"), std::string::npos);

    const Json stale = harness.run(
        "vault_config_apply", replace_change(kGuide, original, "1"));
    EXPECT_EQ(stale["error"], "stale_version");

    const auto before_create = harness.store().config_revision();
    const Json created = harness.run("vault_config_apply", Json{
        {"action", "apply"},
        {"version", revision()},
        {"changes", Json::array({Json{
            {"path", "system/providers/copy/config.toml"},
            {"operation", "create"},
            {"content", "host = \"test\"\nport = 2\nmode = \"test\"\nmodel = \"second\"\n"},
        }})},
    });
    EXPECT_EQ(created["committed"], true);
    EXPECT_EQ(created["undo_available"], false);
    EXPECT_TRUE(created["changed"][0]["old_bytes"].is_null());
    EXPECT_NE(harness.store().snapshot()->find_provider("copy"), nullptr);
    EXPECT_GT(harness.store().config_revision(), before_create);
    const Json cleared = harness.run("vault_config_apply", Json{
        {"action", "undo"},
        {"version", revision()},
        {"changes", nullptr},
    });
    EXPECT_EQ(cleared["committed"], false);
    EXPECT_EQ(cleared["error"], "unavailable_undo");
    EXPECT_EQ(harness.store().read_config(std::vector<std::string>{kPrompt}).files[0].content,
        same_length);

    harness.store().apply_api_key_create("api_key_9", "Search", "search-secret");
    const auto before_key = harness.store().config_revision();
    const Json destination = harness.run("vault_config_apply", Json{
        {"action", "apply"},
        {"version", revision()},
        {"changes", Json::array({Json{
            {"path", "system/web-search/config.toml"},
            {"operation", "create"},
            {"content", "provider = \"brave\"\napi_key = \"api_key_9\"\ntool_enabled = true\n"},
        }})},
    });
    EXPECT_EQ(destination["committed"], false);
    EXPECT_EQ(destination["error"], "credential_destination_protected");
    EXPECT_EQ(harness.store().config_revision(), before_key);

    std::atomic<std::chrono::steady_clock::time_point> now{
        std::chrono::steady_clock::now()};
    set_diagnostic_log_clock_for_test([&now] { return now.load(); });
    const Json verbose = harness.run("assistant_logging", {{"verbose", true}});
    EXPECT_EQ(verbose["level"], "debug");
    EXPECT_TRUE(verbose["expires_in_ms"].is_number());
    const auto latest = verbose["latest_number"].get<std::uint64_t>();
    log_debug("verbose reproduction marker");
    const Json reproduced = harness.run(
        "assistant_logs", logs_arguments(latest, "debug", "verbose reproduction", 5));
    ASSERT_EQ(reproduced["entries"].size(), 1u);
    EXPECT_EQ(reproduced["entries"][0]["level"], "debug");
    EXPECT_GT(reproduced["entries"][0]["number"].get<std::uint64_t>(), latest);
    now = now.load() + std::chrono::minutes(6);
    const auto before_expired = harness.run(
        "assistant_logs", logs_arguments(std::nullopt, "info", nullptr, 1));
    const auto number_before = before_expired["latest_number"].get<std::uint64_t>();
    log_debug("expired debug marker");
    const Json expired = harness.run(
        "assistant_logs", logs_arguments(std::nullopt, "debug", "expired debug", 5));
    EXPECT_EQ(expired["level"], "info");
    EXPECT_TRUE(expired["expires_in_ms"].is_null());
    EXPECT_TRUE(expired["entries"].empty());
    EXPECT_EQ(expired["latest_number"], number_before);
    const Json quiet = harness.run("assistant_logging", {{"verbose", false}});
    EXPECT_EQ(quiet["level"], "info");
    EXPECT_TRUE(quiet["expires_in_ms"].is_null());
}

TEST(AssistantService, RedactsSecretsAndKeepsOneMultilineLogRecord) {
    ServiceHarness harness;
    harness.store().apply_api_key_create("api_key_1", "Secret", "super-secret-key-value");
    harness.use_oauth(
        R"({"access_token":"oauth-access-token-value","refresh_token":"oauth-refresh-token-value","expires_at":1893456000,"account_id":"acct"})");
    const std::string workspace = harness.store().workspace_path().string();
    const std::string root = harness.store().private_root().string();
    log_info(
        "secret super-secret-key-value token oauth-access-token-value "
        "refresh oauth-refresh-token-value workspace " + workspace
        + " root " + root);
    log_info("fake record\r\nsecond line\n");
    const Json redacted = harness.run(
        "assistant_logs", logs_arguments(std::nullopt, nullptr, "super-secret-key-value", 5));
    EXPECT_TRUE(redacted["entries"].empty());
    const Json shown = harness.run(
        "assistant_logs", logs_arguments(std::nullopt, nullptr, "fake record", 5));
    ASSERT_EQ(shown["entries"].size(), 1u);
    const std::string text = shown["entries"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("second line"), std::string::npos);
    EXPECT_NE(text.find('\n'), std::string::npos);
    const Json secrets = harness.run(
        "assistant_logs", logs_arguments(std::nullopt, nullptr, "[REDACTED]", 5));
    ASSERT_FALSE(secrets["entries"].empty());
    const std::string hidden = secrets["entries"].back()["text"].get<std::string>();
    EXPECT_EQ(hidden.find("super-secret-key-value"), std::string::npos);
    EXPECT_EQ(hidden.find("oauth-access-token-value"), std::string::npos);
    EXPECT_EQ(hidden.find("oauth-refresh-token-value"), std::string::npos);
    EXPECT_EQ(hidden.find(workspace), std::string::npos);
    EXPECT_EQ(hidden.find(root), std::string::npos);
    EXPECT_NE(hidden.find("[path]"), std::string::npos);
    EXPECT_NE(hidden.find("[REDACTED]"), std::string::npos);
}

} // namespace
} // namespace cha
