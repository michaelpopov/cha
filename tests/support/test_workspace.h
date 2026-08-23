#pragma once

#include "characters/character.h"
#include "chat/persona.h"
#include "chat/session_identity.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cha::test {

// Small reusable workspace fixture for web tests. The default character uses the
// deterministic in-process test provider mode, so callers never need a live
// provider merely to construct a workspace.
class TestWorkspace {
public:
    TestWorkspace();
    ~TestWorkspace();
    TestWorkspace(const TestWorkspace&) = delete;
    TestWorkspace& operator=(const TestWorkspace&) = delete;

    const std::filesystem::path& root() const noexcept { return root_; }
    void write_workspace_config(std::string_view log_level = "off") const;
    void write_character_config(std::string_view contents) const;
    void write_character_defaults(std::string_view contents) const;
    void write_provider(std::string_view name, std::string_view contents) const;
    void write_style(std::string_view name, std::string_view contents) const;
    void add_persona(
        std::string_view id,
        std::string_view display_name,
        std::string_view prompt = "") const;
    void add_character(std::string_view id, std::string_view display_name) const;
    // Adds a forum whose only member is `member`, which must already exist.
    void add_forum(
        std::string_view id,
        std::string_view display_name,
        std::string_view member) const;

private:
    std::filesystem::path root_;
};

struct TestWorkspaceStyle {
    std::string id;
    CharacterAppearance appearance;
};

struct PublishedTestWorkspace {
    FullSessionId identity;
    std::string default_persona_id;
};

// Writes and publishes one complete, real Workspace for a controller test.
// CharacterDefinition is only convenient fixture input; SessionController
// never receives it and reads the resulting values through getws().
PublishedTestWorkspace publish_test_workspace(
    const std::vector<CharacterDefinition>& definitions,
    const PersonaRoster& personas,
    std::string_view default_character_id,
    const std::filesystem::path& database_path,
    FullSessionId identity = {},
    const std::vector<TestWorkspaceStyle>& styles = {},
    bool reuse_current = false);

} // namespace cha::test
