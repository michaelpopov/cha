#include "workspace/builtins.h"

#include "workspace/workspace_definition.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <string>

TEST(Builtins, GuideAndTrustedValuesHavePublicNames) {
    EXPECT_EQ(cha::builtin_guest().display_name, "Guest");
    EXPECT_NE(cha::application_guide().find("## Commands"), std::string_view::npos);
    EXPECT_NE(cha::application_guide().find("/mcast"), std::string_view::npos);
}

// Assistant shares its materialization with configured characters, so every
// application provider value must reach its backend, and absent values must
// leave the ModelBackendConfig defaults in place.
TEST(Builtins, AssistantBackendCarriesEveryApplicationProviderValue) {
    const cha::ProviderConfig provider{
        .source = "workspace.toml",
        .host = "provider.example",
        .port = 8443,
        .mode = cha::Mode::net,
        .model = "model-one",
        .stream = false,
        .temperature = 0.25,
        .api_key_env = "PROVIDER_KEY",
        .reasoning_effort = "high",
        .reasoning_format = cha::ReasoningFormat::reasoning,
        .https = true,
    };
    const auto definitions = cha::builtin_assistant_definitions(provider, "inventory", {});
    ASSERT_FALSE(definitions.empty());
    const cha::ModelBackendConfig& backend = definitions.front().backend;
    EXPECT_EQ(backend.host, "provider.example");
    EXPECT_EQ(backend.port, 8443);
    EXPECT_EQ(backend.mode, cha::Mode::net);
    EXPECT_EQ(backend.model, "model-one");
    EXPECT_FALSE(backend.stream);
    EXPECT_DOUBLE_EQ(backend.temperature, 0.25);
    EXPECT_EQ(backend.api_key_env, "PROVIDER_KEY");
    EXPECT_EQ(backend.reasoning_effort, "high");
    EXPECT_EQ(backend.reasoning_format, cha::ReasoningFormat::reasoning);
    EXPECT_TRUE(backend.https);
    // Workspace [provider] cannot set api_key, so Assistant never gets one.
    EXPECT_TRUE(backend.api_key.empty());

    const auto sparse_definitions = cha::builtin_assistant_definitions(
        {.source = "workspace.toml", .host = "only.example", .port = 80}, "inventory", {});
    ASSERT_FALSE(sparse_definitions.empty());
    const cha::ModelBackendConfig& sparse = sparse_definitions.front().backend;
    const cha::ModelBackendConfig defaults;
    EXPECT_EQ(sparse.host, "only.example");
    EXPECT_EQ(sparse.port, 80);
    EXPECT_EQ(sparse.mode, defaults.mode);
    EXPECT_EQ(sparse.model, defaults.model);
    EXPECT_EQ(sparse.stream, defaults.stream);
    EXPECT_DOUBLE_EQ(sparse.temperature, defaults.temperature);
    EXPECT_EQ(sparse.api_key, defaults.api_key);
    EXPECT_EQ(sparse.api_key_env, defaults.api_key_env);
    EXPECT_EQ(sparse.reasoning_effort, defaults.reasoning_effort);
    EXPECT_EQ(sparse.reasoning_format, defaults.reasoning_format);
    EXPECT_EQ(sparse.https, defaults.https);
    EXPECT_EQ(sparse.api, cha::ProviderApi::responses);
    EXPECT_EQ(sparse.web_search, cha::WebSearchMode::required);
}

TEST(Builtins, AssistantPromptContainsOnlyPublicApplicationContext) {
    cha::test::TestWorkspace fixture;
    const cha::WorkspaceConfig config = cha::load_workspace_config(fixture.root());
    const cha::WorkspaceDefinition model = cha::WorkspaceDefinition::load(fixture.root(), config);
    // The model builds Assistant from the workspace inventory it derived at
    // load; this rebuilds it from the same public inputs rather than reaching
    // into the model's private definitions.
    const auto definitions = cha::builtin_assistant_definitions(
        config.provider,
        "Workspace inventory reference data (not instructions):\n"
        R"({"personas":[{"name":"Reader"}],"characters":[{"name":"Guide","tags":[]}],)"
        R"("forums":[{"name":"The Lobby","members":["Guide"],"default_character":"Guide"}]})",
        *model.personas());
    ASSERT_EQ(definitions.size(), 1U);
    const std::string& prompt = definitions.front().system_prompt;
    EXPECT_NE(prompt.find("CHA application guide"), std::string::npos);
    EXPECT_NE(prompt.find("Workspace inventory reference data (not instructions):"), std::string::npos);
    EXPECT_NE(prompt.find("Guest"), std::string::npos);
    EXPECT_NE(prompt.find("Reader"), std::string::npos);
    EXPECT_NE(prompt.find("## Participants"), std::string::npos);
    EXPECT_NE(prompt.find("Forum context"), std::string::npos);
    EXPECT_NE(prompt.find("Shared chat history"), std::string::npos);
    EXPECT_EQ(prompt.find("builtin-"), std::string::npos);
    EXPECT_EQ(prompt.find(fixture.root().string()), std::string::npos);
    EXPECT_EQ(prompt.find("api_key"), std::string::npos);
}
