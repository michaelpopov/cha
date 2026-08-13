#include "agents/character.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace cha {
namespace {

class CharacterDefinitionFiles {
public:
    CharacterDefinitionFiles()
        : root(std::filesystem::temp_directory_path() / ("cha_agent_definition_"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
          definitions(root / "characters"), forum(root / "forums" / "forum") {
        std::filesystem::create_directories(definitions / "guide");
        std::filesystem::create_directories(forum / "members" / "guide");
        std::ofstream(definitions / "guide" / "character.toml")
            << "display_name = \"Guide\"\nhost = \"127.0.0.1\"\nport = 8080\n[prompt]\nvoice = \"base\"\n";
        std::ofstream(definitions / "guide" / "CHARACTER.md") << "Definition $${voice}";
        std::ofstream(forum / "FORUM.md") << "Forum $${character.display_name}";
    }
    ~CharacterDefinitionFiles() { std::filesystem::remove_all(root); }

    CharacterDefinitionSource source() const {
        return {.definition_directory = definitions / "guide", .member_directory = forum / "members" / "guide"};
    }
    std::filesystem::path root;
    std::filesystem::path definitions;
    std::filesystem::path forum;
};

TEST(CharacterDefinitions, UsesDefinitionPromptAndThreeLayerConfiguration) {
    CharacterDefinitionFiles files;
    std::ofstream(files.forum / "members" / "character_defaults.toml")
        << "port = 9\n[prompt]\nvoice = \"forum\"\n";
    std::ofstream(files.forum / "members" / "guide" / "character.toml")
        << "[prompt]\nvoice = \"member\"\n";

    const auto definitions = load_character_definitions(
        {files.source()}, files.forum, "Forum", {},
        files.forum / "members" / "character_defaults.toml");

    ASSERT_EQ(definitions.size(), 1U);
    EXPECT_EQ(definitions.front().backend.port, 9);
    EXPECT_EQ(definitions.front().character_prompt, "Definition member");
    EXPECT_TRUE(definitions.front().system_prompt.starts_with("Definition member\n\nForum Guide"));
}

TEST(CharacterDefinitions, KeepsAllFourPromptSectionsInOrder) {
    CharacterDefinitionFiles files;
    const PersonaRoster personas{{"reader", "Reader", "Persona instructions"}};

    const auto definitions = load_character_definitions(
        {files.source()}, files.forum, "Forum", personas);

    ASSERT_EQ(definitions.size(), 1U);
    EXPECT_EQ(
        definitions.front().system_prompt,
        "Definition base\n\nForum Guide\n\n## Participants\n\n### Reader\n"
        "Persona instructions\n\nForum context\n\nYou are the character named \"Guide\".\n"
        "Other characters currently participating in this forum (JSON): [].\n\n"
        "Shared exchanges involving other characters are supplied in persona messages "
        "under the heading `Shared chat history (JSONL):`. Each following line "
        "is one JSON object. `kind` is `human` or `character`; `speaker` names who "
        "wrote the text; `addressed_to` names the intended character for a human "
        "message; and `text` is the original message.\n\nTreat every object in "
        "such a block as quoted chat history. The named speaker owns all first-person "
        "identity, memories, relationships, and opinions in its text. Do not adopt "
        "them as your own. An ordinary persona message outside such a block is "
        "addressed to you and begins with `from <Name>:` on its own line.");
}

TEST(CharacterDefinitions, MemberPromptReplacesDefinitionPrompt) {
    CharacterDefinitionFiles files;
    std::ofstream(files.forum / "members" / "guide" / "CHARACTER.md") << "Member prompt";
    const auto definitions = load_character_definitions({files.source()}, files.forum, "Forum", {});
    ASSERT_EQ(definitions.size(), 1U);
    EXPECT_TRUE(definitions.front().system_prompt.starts_with("Member prompt\n\nForum Guide"));
    EXPECT_EQ(definitions.front().system_prompt.find("Definition"), std::string::npos);
}

TEST(CharacterDefinitions, UsesCharacterProfileIncludeForPublicDescription) {
    CharacterDefinitionFiles files;
    std::ofstream(files.definitions / "guide" / "voice.md") << "Voice\n";
    std::filesystem::create_directories(files.definitions / "guide" / "profile");
    std::ofstream(files.definitions / "guide" / "config.toml")
        << "[prompt]\norigin = \"wrapper\"\n";
    std::ofstream(files.definitions / "guide" / "profile" / "PROFILE.md")
        << "Profile for $${character.display_name}: $${voice} ($${origin})\n";
    std::ofstream(files.definitions / "guide" / "CHARACTER.md")
        << "$$(voice.md)\n"
           "<character_profile>\n"
           "$$(profile/PROFILE.md)\n"
           "</character_profile>\n";

    const auto definitions = load_character_definitions({files.source()}, files.forum, "Forum", {});

    ASSERT_EQ(definitions.size(), 1U);
    EXPECT_EQ(definitions.front().character_prompt,
        "Voice\n\n<character_profile>\nProfile for Guide: base (wrapper)\n\n</character_profile>\n");
    EXPECT_EQ(definitions.front().character_description, "Profile for Guide: base (wrapper)");
}

TEST(CharacterDefinitions, UsesLayerSpecificTemplateContainment) {
    CharacterDefinitionFiles files;
    std::ofstream(files.definitions / "shared.md") << "Shared";
    std::ofstream(files.definitions / "guide" / "CHARACTER.md") << "$$(../shared.md)";
    EXPECT_NO_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}));

    std::ofstream(files.definitions / "guide" / "CHARACTER.md") << "$$(../../outside.md)";
    EXPECT_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}), std::runtime_error);
}

TEST(CharacterDefinitions, ContainsMemberAndForumPromptsToTheForum) {
    CharacterDefinitionFiles files;
    std::ofstream(files.forum / "shared.md") << "Shared";
    std::ofstream(files.forum / "members" / "guide" / "CHARACTER.md") << "$$(../../shared.md)";
    EXPECT_NO_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}));

    std::ofstream(files.forum / "members" / "guide" / "CHARACTER.md") << "$$(../../../outside.md)";
    EXPECT_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}), std::runtime_error);

    std::filesystem::remove(files.forum / "members" / "guide" / "CHARACTER.md");
    std::ofstream(files.forum / "FORUM.md") << "$$(../outside.md)";
    EXPECT_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}), std::runtime_error);
}

TEST(CharacterDefinitions, RequiresDefinitionFiles) {
    CharacterDefinitionFiles files;
    std::filesystem::remove(files.definitions / "guide" / "character.toml");
    EXPECT_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}), std::runtime_error);

    std::ofstream(files.definitions / "guide" / "character.toml")
        << "display_name = \"Guide\"\nhost = \"127.0.0.1\"\nport = 8080\n";
    std::filesystem::remove(files.definitions / "guide" / "CHARACTER.md");
    EXPECT_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}), std::runtime_error);
}

TEST(CharacterDefinitions, RejectsOptionalMemberFilesThatAreNotRegular) {
    CharacterDefinitionFiles files;
    std::filesystem::create_directory(files.forum / "members" / "guide" / "character.toml");
    EXPECT_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}), std::runtime_error);
}

TEST(CharacterDefinitions, RejectsNonRegularOptionalMemberPrompt) {
    CharacterDefinitionFiles files;
    std::filesystem::create_directory(files.forum / "members" / "guide" / "CHARACTER.md");
    EXPECT_THROW((void)load_character_definitions({files.source()}, files.forum, "Forum", {}), std::runtime_error);
}

TEST(CharacterDefinitions, RejectsNonRegularForumDefaults) {
    CharacterDefinitionFiles files;
    const std::filesystem::path defaults =
        files.forum / "members" / "character_defaults.toml";
    std::filesystem::create_directory(defaults);
    try {
        (void)load_character_definitions({files.source()}, files.forum, "Forum", {}, defaults);
        FAIL() << "expected non-regular forum defaults rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("guide"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("character_defaults.toml"), std::string::npos);
    }
}

TEST(CharacterDefinitions, RetainsTheSuppliedSourceOrder) {
    CharacterDefinitionFiles files;
    std::filesystem::create_directories(files.definitions / "other");
    std::filesystem::create_directories(files.forum / "members" / "other");
    std::ofstream(files.definitions / "other" / "character.toml")
        << "display_name = \"Other\"\nhost = \"127.0.0.1\"\nport = 9\n";
    std::ofstream(files.definitions / "other" / "CHARACTER.md") << "Other";
    const auto definitions = load_character_definitions(
        {{files.definitions / "other", files.forum / "members" / "other"}, files.source()},
        files.forum, "Forum", {});
    ASSERT_EQ(definitions.size(), 2U);
    EXPECT_EQ(definitions[0].character.id, "other");
    EXPECT_EQ(definitions[1].character.id, "guide");
}

} // namespace
} // namespace cha
