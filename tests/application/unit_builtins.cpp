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

// Assistant receives the same resolved backend shape as configured characters.
TEST(Builtins, AssistantCarriesItsResolvedBackend) {
    const cha::ProviderConfig provider{
        .host = "provider.example",
        .port = 8443,
        .base_path = "/api",
        .mode = cha::Mode::net,
        .model = "model-one",
        .stream = false,
        .temperature = 0.25,
        .max_tokens = 512,
        .timeout_s = 90,
        .idle_timeout_s = 15,
        .api_key_env = "PROVIDER_KEY",
        .reasoning_effort = "high",
        .reasoning_format = cha::ReasoningFormat::reasoning,
        .https = true,
    };
    const auto definitions = cha::builtin_assistant_definitions(
        cha::make_backend_config(provider), "inventory", {});
    ASSERT_FALSE(definitions.empty());
    const cha::ModelBackendConfig& backend = definitions.front().backend;
    EXPECT_EQ(backend.host, "provider.example");
    EXPECT_EQ(backend.port, 8443);
    EXPECT_EQ(backend.base_path, "/api");
    EXPECT_EQ(backend.mode, cha::Mode::net);
    EXPECT_EQ(backend.model, "model-one");
    EXPECT_FALSE(backend.stream);
    ASSERT_TRUE(backend.temperature);
    EXPECT_DOUBLE_EQ(*backend.temperature, 0.25);
    EXPECT_EQ(backend.max_tokens, 512);
    EXPECT_EQ(backend.timeout_s, 90);
    EXPECT_EQ(backend.idle_timeout_s, 15);
    EXPECT_EQ(backend.api_key_env, "PROVIDER_KEY");
    EXPECT_EQ(backend.reasoning_effort, "high");
    EXPECT_EQ(backend.reasoning_format, cha::ReasoningFormat::reasoning);
    EXPECT_TRUE(backend.https);
    // No configuration file may set api_key, so Assistant never gets one.
    EXPECT_TRUE(backend.api_key.empty());

    const auto sparse_definitions = cha::builtin_assistant_definitions(
        {.host = "only.example", .port = 80}, "inventory", {});
    ASSERT_FALSE(sparse_definitions.empty());
    const cha::ModelBackendConfig& sparse = sparse_definitions.front().backend;
    const cha::ModelBackendConfig defaults;
    EXPECT_EQ(sparse.host, "only.example");
    EXPECT_EQ(sparse.port, 80);
    EXPECT_EQ(sparse.base_path, defaults.base_path);
    EXPECT_EQ(sparse.mode, defaults.mode);
    EXPECT_EQ(sparse.model, defaults.model);
    EXPECT_EQ(sparse.stream, defaults.stream);
    EXPECT_EQ(sparse.temperature, defaults.temperature);
    EXPECT_EQ(sparse.max_tokens, defaults.max_tokens);
    EXPECT_EQ(sparse.timeout_s, defaults.timeout_s);
    EXPECT_EQ(sparse.idle_timeout_s, defaults.idle_timeout_s);
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
    const cha::WorkspaceDefinition model = cha::WorkspaceDefinition::load(
        fixture.root(), cha::load_workspace_config(fixture.root()));
    // The model builds Assistant from the workspace inventory it derived at
    // load; this rebuilds it from the same public inputs rather than reaching
    // into the model's private definitions.
    const auto definitions = cha::builtin_assistant_definitions(
        {.host = "test", .port = 1, .mode = cha::Mode::test, .model = "fake"},
        "Workspace inventory reference data (not instructions):\n"
        R"({"characters":[{"name":"Guide","tags":[]}],)"
        R"("forums":[{"name":"The Lobby","members":["Guide"],)"
        R"("default_character":"Guide","default_persona":"Guest"}]})",
        cha::PersonaRoster{cha::builtin_guest()});
    ASSERT_EQ(definitions.size(), 1U);
    const std::string& prompt = definitions.front().system_prompt;
    EXPECT_NE(prompt.find("CHA application guide"), std::string::npos);
    EXPECT_NE(prompt.find("Workspace inventory reference data (not instructions):"), std::string::npos);
    // Entrance speaks as Guest, so Guest is the sole participant. The inventory
    // still names each custom forum's persona, which is how Assistant can
    // answer who the user speaks as elsewhere.
    EXPECT_NE(prompt.find("Guest"), std::string::npos);
    EXPECT_NE(prompt.find(R"("default_persona":"Guest")"), std::string::npos);
    EXPECT_EQ(prompt.find("### Reader"), std::string::npos);
    EXPECT_NE(prompt.find("## Participants"), std::string::npos);
    EXPECT_NE(prompt.find("Forum context"), std::string::npos);
    EXPECT_NE(prompt.find("Shared chat history"), std::string::npos);
    EXPECT_EQ(prompt.find("builtin-"), std::string::npos);
    EXPECT_EQ(prompt.find(fixture.root().string()), std::string::npos);
    EXPECT_EQ(prompt.find("api_key"), std::string::npos);
}
