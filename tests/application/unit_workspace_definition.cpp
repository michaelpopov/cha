#include "workspace/workspace_definition.h"

#include "agents/provider_client.h"
#include "workspace/builtins.h"
#include "support/test_workspace.h"
#include "util/environment.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct WorkspaceDefinitionTestAccess {
    static const std::vector<CharacterDefinition>& loaded(
        const WorkspaceDefinition& model, std::string_view forum_id) {
        return model.definitions_.at(std::string(forum_id));
    }

    static std::vector<CharacterDefinition> copy(
        const WorkspaceDefinition& model, std::string_view forum_id) {
        return model.copy_definitions_for(forum_id).definitions;
    }

    static void persist_persona(
        const WorkspaceDefinition& model,
        std::string_view forum_id,
        std::string_view persona_id) {
        model.persist_forum_default_persona(forum_id, persona_id);
    }
};

namespace {

WorkspaceDefinition load_model(const std::filesystem::path& root) {
    return WorkspaceDefinition::load(root, load_workspace_config(root));
}

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) previous_ = value;
    }

    ~ScopedEnvironmentVariable() {
        if (previous_) {
            (void)set_environment_variable(name_, *previous_);
        } else {
            (void)unset_environment_variable(name_);
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

std::vector<std::string> display_names(std::span<const CharacterMetadata> values) {
    std::vector<std::string> names;
    for (const CharacterMetadata& value : values) names.push_back(value.display_name);
    return names;
}

std::vector<std::string> display_names(std::span<const ForumInfo> values) {
    std::vector<std::string> names;
    for (const ForumInfo& value : values) names.push_back(value.display_name);
    return names;
}

void add_character(
    const test::TestWorkspace& fixture,
    std::string_view id,
    std::string_view display_name,
    std::string_view extra_config = {}) {
    const auto directory = fixture.root() / "characters" / std::string(id);
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "character.toml")
        << "display_name = \"" << display_name << "\"\n"
           "provider = \"test\"\n" << extra_config;
    std::ofstream(directory / "CHARACTER.md") << "Prompt\n";
}

void add_forum(
    const test::TestWorkspace& fixture,
    std::string_view id,
    std::string_view display_name,
    std::initializer_list<std::string_view> members) {
    const auto directory = fixture.root() / "forums" / std::string(id);
    for (std::string_view member : members) {
        std::filesystem::create_directories(directory / "members" / std::string(member));
    }
    std::ofstream(directory / "config.toml")
        << "display_name = \"" << display_name << "\"\n";
    std::ofstream(directory / "FORUM.md") << "Forum instructions\n";
}

// The workspace personas the model published, without the built-in Guest it
// always puts first.
PersonaRoster custom_personas(const WorkspaceDefinition& model) {
    PersonaRoster roster = *model.personas();
    EXPECT_FALSE(roster.empty());
    EXPECT_EQ(roster.front().id, guest_id);
    roster.erase(roster.begin());
    return roster;
}

// --- Published catalogs ----------------------------------------------------

TEST(WorkspaceDefinition, PublishesGuestFirstAndEveryCatalogInDisplayOrder) {
    test::TestWorkspace fixture;
    fixture.add_persona("author_key", "An Author");
    const WorkspaceDefinition model = load_model(fixture.root());

    ASSERT_EQ(model.personas()->size(), 3U);
    EXPECT_EQ(model.personas()->front().id, guest_id);
    EXPECT_EQ((*model.personas())[1].display_name, "An Author");
    EXPECT_EQ((*model.personas())[2].display_name, "Reader");
    EXPECT_EQ(display_names(model.characters()), (std::vector<std::string>{"Assistant", "Guide"}));
    EXPECT_EQ(display_names(model.forums()), (std::vector<std::string>{"Entrance", "The Lobby"}));
}

TEST(WorkspaceDefinition, ReportsForumMembershipAndDefaultsWithoutAPath) {
    test::TestWorkspace fixture;
    const WorkspaceDefinition model = load_model(fixture.root());

    const ForumInfo* lobby = model.find_forum("lobby");
    ASSERT_NE(lobby, nullptr);
    EXPECT_EQ(lobby->display_name, "The Lobby");
    EXPECT_EQ(lobby->member_ids, (std::vector<std::string>{"guide"}));
    EXPECT_EQ(lobby->default_character_id, "guide");
    EXPECT_EQ(lobby->default_persona_id, guest_id);

    const ForumInfo* entrance = model.find_forum(entrance_id);
    ASSERT_NE(entrance, nullptr);
    EXPECT_EQ(entrance->member_ids, (std::vector<std::string>{std::string(assistant_id)}));
    EXPECT_EQ(entrance->default_character_id, assistant_id);
    EXPECT_EQ(entrance->default_persona_id, guest_id);
}

TEST(WorkspaceDefinition, RejectsReferencedProvidersWithoutAConfiguredModel) {
    test::TestWorkspace fixture;
    fixture.write_provider("test", "host = \"test\"\nport = 1\nmode = \"test\"\n");

    try {
        (void)load_model(fixture.root());
        FAIL() << "expected missing model rejection";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("providers/test/config.toml"), std::string::npos);
        EXPECT_NE(message.find("provider 'test'"), std::string::npos);
        EXPECT_NE(message.find("model"), std::string::npos);
    }
}

TEST(WorkspaceDefinition, RejectsAnUnassignedCharactersProviderWithoutAConfiguredModel) {
    test::TestWorkspace fixture;
    fixture.add_character("unassigned", "Unassigned");
    std::ofstream(fixture.root() / "characters" / "unassigned" / "character.toml")
        << "display_name = \"Unassigned\"\nprovider = \"unassigned-provider\"\n";
    fixture.write_provider(
        "unassigned-provider", "host = \"test\"\nport = 1\nmode = \"test\"\n");

    try {
        (void)load_model(fixture.root());
        FAIL() << "expected missing model rejection";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("providers/unassigned-provider/config.toml"), std::string::npos);
        EXPECT_NE(message.find("provider 'unassigned-provider'"), std::string::npos);
    }
}

TEST(WorkspaceDefinition, ValidatesReferencedCredentialEnvironmentVariablesWithoutExposingValues) {
    constexpr std::string_view variable = "CHA_BLOCK_ONE_PROVIDER_KEY";
    constexpr std::string_view secret = "never-log-this-value";
    ScopedEnvironmentVariable environment{std::string(variable)};
    test::TestWorkspace fixture;
    fixture.write_provider(
        "test",
        "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n"
        "api_key_env = \"CHA_BLOCK_ONE_PROVIDER_KEY\"\n");

    ASSERT_TRUE(unset_environment_variable(variable));
    EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);

    ASSERT_TRUE(set_environment_variable(variable, ""));
    EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);

    ASSERT_TRUE(set_environment_variable(variable, secret));
    const WorkspaceDefinition model = load_model(fixture.root());
    const CharacterDefinition& definition =
        WorkspaceDefinitionTestAccess::loaded(model, "lobby").front();
    EXPECT_TRUE(definition.provider.config.api_key.empty());
    ProviderClient client(definition);
    const ModelBackendInfo info = client.info();
    EXPECT_EQ(info.model, "fake");
    EXPECT_EQ(info.model.find(secret), std::string::npos);
    EXPECT_EQ(info.api.find(secret), std::string::npos);

    try {
        ASSERT_TRUE(unset_environment_variable(variable));
        (void)load_model(fixture.root());
        FAIL() << "expected missing credential rejection";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("providers/test/config.toml"), std::string::npos);
        EXPECT_NE(message.find("provider 'test'"), std::string::npos);
        EXPECT_NE(message.find(std::string(variable)), std::string::npos);
        EXPECT_EQ(message.find(std::string(secret)), std::string::npos);
    }
}

TEST(WorkspaceDefinition, LaterDefinitionsUseAnUpdatedProviderSnapshot) {
    test::TestWorkspace fixture;
    fixture.write_provider("test", "host = \"first\"\nport = 1\nmode = \"test\"\nmodel = \"one\"\n");
    const WorkspaceDefinition first = load_model(fixture.root());

    fixture.write_provider("test", "host = \"second\"\nport = 2\nmode = \"test\"\nmodel = \"two\"\n");
    const WorkspaceDefinition second = load_model(fixture.root());

    const CharacterDefinition& first_definition =
        WorkspaceDefinitionTestAccess::loaded(first, "lobby").front();
    const CharacterDefinition& second_definition =
        WorkspaceDefinitionTestAccess::loaded(second, "lobby").front();
    EXPECT_EQ(first_definition.provider.id, "test");
    EXPECT_EQ(first_definition.provider.config.model, "one");
    EXPECT_EQ(first_definition.provider.config.host, "first");
    EXPECT_EQ(second_definition.provider.id, "test");
    EXPECT_EQ(second_definition.provider.config.model, "two");
    EXPECT_EQ(second_definition.provider.config.host, "second");
}

void expect_prompt_contains_only(
    const std::vector<CharacterDefinition>& definitions,
    std::string_view included,
    std::string_view excluded) {
    ASSERT_FALSE(definitions.empty());
    const std::string& prompt = definitions.front().system_prompt;
    EXPECT_NE(prompt.find("## Participants"), std::string::npos);
    EXPECT_NE(prompt.find(included), std::string::npos);
    EXPECT_EQ(prompt.find(excluded), std::string::npos);
}

TEST(WorkspaceDefinition, EmbedsOnlyTheForumDefaultPersonaInCharacterPrompts) {
    test::TestWorkspace fixture;
    fixture.add_persona("reader", "Reader", "READER_PERSONA_BODY");
    fixture.add_persona("author", "Author", "AUTHOR_PERSONA_BODY");
    std::ofstream(fixture.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_persona = \"reader\"\n";
    add_forum(fixture, "circle", "The Circle", {"guide"});
    std::ofstream(fixture.root() / "forums" / "circle" / "config.toml")
        << "display_name = \"The Circle\"\ndefault_persona = \"author\"\n";

    const WorkspaceDefinition model = load_model(fixture.root());
    expect_prompt_contains_only(
        WorkspaceDefinitionTestAccess::loaded(model, "lobby"),
        "READER_PERSONA_BODY",
        "AUTHOR_PERSONA_BODY");
    expect_prompt_contains_only(
        WorkspaceDefinitionTestAccess::loaded(model, "circle"),
        "AUTHOR_PERSONA_BODY",
        "READER_PERSONA_BODY");
}

TEST(WorkspaceDefinition, ReloadsPromptsFromThePersistedForumDefaultPersona) {
    test::TestWorkspace fixture;
    fixture.add_persona("reader", "Reader", "READER_PERSONA_BODY");
    fixture.add_persona("author", "Author", "AUTHOR_PERSONA_BODY");
    std::ofstream(fixture.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_persona = \"reader\"\n";

    const WorkspaceDefinition model = load_model(fixture.root());
    expect_prompt_contains_only(
        WorkspaceDefinitionTestAccess::loaded(model, "lobby"),
        "READER_PERSONA_BODY",
        "AUTHOR_PERSONA_BODY");

    WorkspaceDefinitionTestAccess::persist_persona(model, "lobby", "author");
    expect_prompt_contains_only(
        WorkspaceDefinitionTestAccess::copy(model, "lobby"),
        "AUTHOR_PERSONA_BODY",
        "READER_PERSONA_BODY");
}

TEST(WorkspaceDefinition, ServesAssistantDetailFromTheEmbeddedApplicationGuide) {
    test::TestWorkspace fixture;
    const WorkspaceDefinition model = load_model(fixture.root());

    EXPECT_EQ(model.character_markdown(assistant_id), application_guide());
    EXPECT_EQ(model.character_markdown("guide"), "Character instructions\n");
    EXPECT_THROW((void)model.character_markdown("absent"), std::runtime_error);
}

TEST(WorkspaceDefinition, ExpandsThePublicProfileOfAnUnassignedCharacter) {
    test::TestWorkspace fixture;
    add_character(
        fixture, "orphan", "Orphan",
        "[prompt]\norigin = \"definition scope\"\n");
    std::ofstream(fixture.root() / "characters" / "voice.md")
        << "Voice for $${character.display_name} in '$${forum.display_name}'\n";
    std::ofstream(fixture.root() / "characters" / "orphan" / "PROFILE.md")
        << "Profile for $${character.display_name} from $${origin}\n";
    std::ofstream(fixture.root() / "characters" / "orphan" / "CHARACTER.md")
        << "$$(../voice.md)\n"
           "<character_profile>\n"
           "$$(PROFILE.md)\n"
           "</character_profile>\n";

    const WorkspaceDefinition model = load_model(fixture.root());

    EXPECT_EQ(
        model.character_markdown("orphan"),
        "Profile for Orphan from definition scope");
}

TEST(WorkspaceDefinition, ShowsAnUnexpandableUnassignedCharacterVerbatim) {
    // No forum supplies this character's variables any more, which must cost
    // the character its expansion and not the whole workspace its load.
    test::TestWorkspace fixture;
    add_character(fixture, "orphan", "Orphan");
    std::ofstream(fixture.root() / "characters" / "orphan" / "CHARACTER.md")
        << "Orphan speaks in a $${voice} voice\n";

    const WorkspaceDefinition model = load_model(fixture.root());

    EXPECT_EQ(
        model.character_markdown("orphan"),
        "Orphan speaks in a $${voice} voice\n");
}

TEST(WorkspaceDefinition, ValidatesCharacterProviderAndIgnoresForumProvider) {
    test::TestWorkspace fixture;
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"missing-character\"\n");
    try {
        (void)load_model(fixture.root());
        FAIL() << "expected a missing character provider to stop startup";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("missing-character"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("character.toml"), std::string::npos);
    }

    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"test\"\n");
    fixture.write_character_defaults("provider = \"missing-forum\"\n");
    EXPECT_NO_THROW((void)load_model(fixture.root()));
}

TEST(WorkspaceDefinition, ResolvesACharacterStyleIntoPublishedMetadata) {
    test::TestWorkspace fixture;
    fixture.write_style("serif-bold", "font = \"serif\"\nweight = \"bold\"\n");
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"test\"\nstyle = \"serif-bold\"\n");

    const WorkspaceDefinition model = load_model(fixture.root());

    const std::span<const CharacterMetadata> characters = model.characters();
    const auto guide = std::ranges::find(characters, "guide", &CharacterMetadata::id);
    ASSERT_NE(guide, characters.end());
    EXPECT_EQ(guide->appearance, (CharacterAppearance{
        CharacterFont::serif, CharacterSlant::normal,
        CharacterWeight::bold, CharacterScale::normal}));
}

TEST(WorkspaceDefinition, ReadsCharacterProviderAndOptionalStyleFromDisk) {
    test::TestWorkspace fixture;
    const WorkspaceDefinition model = load_model(fixture.root());

    const std::optional<CharacterSettings> settings = model.character_settings("guide");
    ASSERT_TRUE(settings);
    EXPECT_EQ(settings->provider, std::optional<std::string>("test"));
    EXPECT_FALSE(settings->style);
    EXPECT_TRUE(model.character_config_path("guide"));
    EXPECT_FALSE(model.character_config_path(assistant_id));
    EXPECT_FALSE(model.character_settings(assistant_id));

    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"test\"\nstyle = \"serif-bold\"\n");
    fixture.write_style("serif-bold", "font = \"serif\"\nweight = \"bold\"\n");
    const std::optional<CharacterSettings> after_edit = model.character_settings("guide");
    ASSERT_TRUE(after_edit);
    EXPECT_EQ(after_edit->provider, std::optional<std::string>("test"));
    EXPECT_EQ(after_edit->style, std::optional<std::string>("serif-bold"));
}

// A character.toml edited into an unreadable state must not be reported as one
// that simply sets nothing: that would invite a save to overwrite a file this
// process cannot parse, and it would claim as fact something never read.
TEST(WorkspaceDefinition, ReportsNoCharacterSettingsForAConfigItCannotRead) {
    test::TestWorkspace fixture;
    const WorkspaceDefinition model = load_model(fixture.root());
    ASSERT_TRUE(model.character_settings("guide"));

    fixture.write_character_config("display_name = \"Guide\"\nstyle = \n");
    EXPECT_FALSE(model.character_settings("guide"));

    // A key of the wrong type is a broken file too, not an absent setting.
    fixture.write_character_config("display_name = \"Guide\"\nprovider = 12\n");
    EXPECT_FALSE(model.character_settings("guide"));

    fixture.write_character_config("display_name = \"Guide\"\nprovider = \"test\"\n");
    const std::optional<CharacterSettings> repaired = model.character_settings("guide");
    ASSERT_TRUE(repaired);
    EXPECT_EQ(repaired->provider, std::optional<std::string>("test"));
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(WorkspaceDefinition, WritesCharacterProviderAndOptionalStyle) {
    test::TestWorkspace fixture;
    fixture.write_provider("other", "host = \"test\"\nport = 2\nmode = \"test\"\nmodel = \"fake\"\n");
    fixture.write_style("serif-italic", "font = \"serif\"\nstyle = \"italic\"\n");
    const WorkspaceDefinition model = load_model(fixture.root());

    EXPECT_EQ(
        model.write_character_settings("guide", "other", "serif-italic"),
        (CharacterSettingsChange{true, true}));
    EXPECT_EQ(
        model.character_settings("guide"),
        (CharacterSettings{"other", "serif-italic"}));

    EXPECT_EQ(
        model.write_character_settings("guide", "other", "serif-italic"),
        CharacterSettingsChange{});

    EXPECT_EQ(
        model.write_character_settings("guide", "other", std::nullopt),
        (CharacterSettingsChange{false, true}));
    EXPECT_EQ(
        model.character_settings("guide"),
        (CharacterSettings{"other", std::nullopt}));
}

TEST(WorkspaceDefinition, ComparesAStaleSaveWithTheDocumentItActuallyReplaces) {
    test::TestWorkspace fixture;
    fixture.write_style("serif-italic", "font = \"serif\"\nstyle = \"italic\"\n");
    const WorkspaceDefinition model = load_model(fixture.root());
    const std::optional<CharacterSettings> stale = model.character_settings("guide");
    ASSERT_TRUE(stale);

    EXPECT_EQ(
        model.write_character_settings("guide", *stale->provider, "serif-italic"),
        (CharacterSettingsChange{false, true}));

    // This is the full form body a second browser prepared before the style
    // save above. Its old style changes the document it now replaces.
    EXPECT_EQ(
        model.write_character_settings("guide", "test", stale->style),
        (CharacterSettingsChange{false, true}));
}

TEST(WorkspaceDefinition, RejectsAnUnusableSelectionWithoutTouchingTheFile) {
    test::TestWorkspace fixture;
    fixture.write_provider("broken", "this is not a usable provider\n");
    fixture.write_provider("model-less", "host = \"test\"\nport = 1\nmode = \"test\"\n");
    fixture.write_style("broken", "font = \"comic\"\n");
    fixture.write_character_config("display_name = \"Guide\"\nprovider = \"test\"\n");
    const WorkspaceDefinition model = load_model(fixture.root());
    const auto path = fixture.root() / "characters" / "guide" / "character.toml";
    const std::string before = file_bytes(path);

    EXPECT_THROW(
        model.write_character_settings("guide", "broken", std::nullopt),
        std::invalid_argument);
    EXPECT_THROW(
        model.write_character_settings("guide", "model-less", std::nullopt),
        std::invalid_argument);
    EXPECT_THROW(
        model.write_character_settings("guide", "test", "broken"),
        std::invalid_argument);
    EXPECT_THROW(
        model.write_character_settings(assistant_id, "test", std::nullopt),
        std::runtime_error);
    EXPECT_EQ(file_bytes(path), before);

    fixture.write_character_config("display_name = \"Guide\"\nstyle = \n");
    EXPECT_THROW(
        model.write_character_settings("guide", "test", std::nullopt),
        std::runtime_error);
    EXPECT_EQ(
        file_bytes(path),
        "display_name = \"Guide\"\nstyle = \n");
}

TEST(WorkspaceDefinition, ListsOnlyProvidersAndStylesThatResolveAndDerivesLabels) {
    test::TestWorkspace fixture;
    fixture.write_provider("sol-high", "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n");
    fixture.write_provider("mistral-large", "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n");
    fixture.write_provider("broken", "this is not a usable provider\n");
    fixture.write_style("mono-large", "font = \"mono\"\nsize = \"large\"\n");
    fixture.write_style("serif-italic", "font = \"serif\"\nstyle = \"italic\"\n");
    fixture.write_style("broken", "font = \"comic\"\n");
    std::filesystem::create_directories(
        fixture.root() / "system" / "providers" / "empty-dir");

    const WorkspaceDefinition model = load_model(fixture.root());
    const std::vector<AvailableProvider> providers = model.available_providers();
    std::vector<std::string> provider_ids;
    std::vector<std::string> provider_labels;
    for (const AvailableProvider& option : providers) {
        provider_ids.push_back(option.id);
        provider_labels.push_back(option.label);
    }
    EXPECT_EQ(provider_ids, (std::vector<std::string>{"mistral-large", "sol-high", "test"}));
    EXPECT_EQ(provider_labels, (std::vector<std::string>{"Mistral large", "Sol high", "Test"}));

    const std::vector<AvailableStyle> styles = model.available_styles();
    ASSERT_EQ(styles.size(), 2U);
    EXPECT_EQ(styles[0].id, "mono-large");
    EXPECT_EQ(styles[0].label, "Mono large");
    EXPECT_EQ(styles[0].appearance, (CharacterAppearance{
        CharacterFont::mono, CharacterSlant::normal,
        CharacterWeight::normal, CharacterScale::large}));
    EXPECT_EQ(styles[1].id, "serif-italic");
    EXPECT_EQ(styles[1].label, "Serif italic");
    EXPECT_EQ(styles[1].appearance, (CharacterAppearance{
        CharacterFont::serif, CharacterSlant::italic,
        CharacterWeight::normal, CharacterScale::normal}));
}

TEST(WorkspaceDefinition, ResolvesASessionStyleIntoACompleteAppearance) {
    test::TestWorkspace fixture;
    fixture.write_style("serif-bold", "font = \"serif\"\nweight = \"bold\"\n");
    const WorkspaceDefinition model = load_model(fixture.root());

    const CharacterAppearance appearance = model.resolve_session_style("serif-bold");
    EXPECT_EQ(appearance, (CharacterAppearance{
        CharacterFont::serif, CharacterSlant::normal,
        CharacterWeight::bold, CharacterScale::normal}));
}

TEST(WorkspaceDefinition, SessionStyleFailuresListTheAvailableStyles) {
    test::TestWorkspace fixture;
    fixture.write_style("serif-bold", "font = \"serif\"\nweight = \"bold\"\n");
    fixture.write_style("broken", "font = \"comic\"\n");
    const WorkspaceDefinition model = load_model(fixture.root());

    // A name nothing is installed under reads as one plain sentence: no
    // filesystem path, and no config file described as referencing itself.
    try {
        (void)model.resolve_session_style("missing");
        FAIL() << "Expected an unknown style to be rejected";
    } catch (const std::invalid_argument& error) {
        EXPECT_EQ(
            std::string(error.what()),
            "Style 'missing' is not usable: no style config is installed"
            " under this name. Available styles: broken, serif-bold");
    }

    EXPECT_THROW(
        (void)model.resolve_session_style("../serif-bold"),
        std::invalid_argument);

    try {
        (void)model.resolve_session_style("broken");
        FAIL() << "Expected a malformed style to be rejected";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(
            std::string(error.what()).find("broken"), std::string::npos);
    }
}

TEST(WorkspaceDefinition, FailsStartupForMissingCharacterStyleReference) {
    test::TestWorkspace fixture;
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"test\"\nstyle = \"missing-style\"\n");
    try {
        (void)load_model(fixture.root());
        FAIL() << "expected a missing character style to stop startup";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("missing-style"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("character.toml"), std::string::npos);
    }
}

TEST(WorkspaceDefinition, FailsToLoadWhenAnyConfiguredForumHasABrokenPrompt) {
    test::TestWorkspace fixture;
    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "$$(missing.md)";

    try {
        (void)load_model(fixture.root());
        FAIL() << "expected a missing prompt include to fail model loading";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("lobby"), std::string::npos) << message;
        EXPECT_NE(message.find("guide"), std::string::npos) << message;
        EXPECT_NE(message.find("cannot read included file"), std::string::npos) << message;
    }
}

TEST(WorkspaceDefinition, RejectsCharacterAndForumIdsReservedForBuiltins) {
    {
        test::TestWorkspace fixture;
        const auto definition = fixture.root() / "characters" / std::string(guest_id);
        std::filesystem::create_directories(definition);
        std::filesystem::create_directories(
            fixture.root() / "forums" / "lobby" / "members" / std::string(guest_id));
        std::ofstream(definition / "character.toml") << "display_name = \"Impostor\"\n";
        std::ofstream(definition / "CHARACTER.md") << "Prompt\n";
        EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        const auto forum = fixture.root() / "forums" / std::string(entrance_id);
        std::filesystem::create_directories(forum / "members" / "guide");
        std::ofstream(forum / "FORUM.md") << "Forum";
        std::ofstream(forum / "config.toml") << "display_name = \"Other\"\n";
        EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
    }
}

TEST(WorkspaceDefinition, RejectsMalformedForumsButAcceptsAWorkspaceWithNoCustomForum) {
    {
        test::TestWorkspace fixture;
        std::filesystem::create_directories(fixture.root() / "forums" / "broken" / "members");
        std::ofstream(fixture.root() / "forums" / "broken" / "config.toml")
            << "display_name = \"Broken\"\n";
        std::filesystem::create_directories(
            fixture.root() / "forums" / "bad-config" / "members" / "guide");
        std::ofstream(fixture.root() / "forums" / "bad-config" / "config.toml")
            << "display_name = [not valid TOML";
        EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        std::filesystem::remove_all(fixture.root() / "forums" / "lobby");
        const WorkspaceDefinition model = load_model(fixture.root());
        EXPECT_EQ(display_names(model.forums()), (std::vector<std::string>{"Entrance"}));
        EXPECT_TRUE(model.session_directories().empty());
    }
}

TEST(WorkspaceDefinition, RejectsDuplicateAndReservedForumPublicNames) {
    test::TestWorkspace fixture;
    const auto forum = fixture.root() / "forums" / "other";
    std::filesystem::create_directories(forum / "members" / "guide");
    std::ofstream(forum / "FORUM.md") << "Forum";
    std::ofstream(forum / "config.toml") << "display_name = \"the lobby\"\n";
    EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
    std::ofstream(forum / "config.toml") << "display_name = \"Entrance\"\n";
    EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
}

TEST(WorkspaceDefinition, KeepsPublicResultsFixedWhenWorkspaceFilesChangeAfterLoading) {
    test::TestWorkspace fixture;
    const WorkspaceDefinition model = load_model(fixture.root());
    const std::vector<std::string> characters = display_names(model.characters());
    const std::vector<std::string> forums = display_names(model.forums());
    const std::size_t persona_count = model.personas()->size();

    fixture.write_character_config(
        "display_name = \"Renamed\"\nprovider = \"test\"\n");
    std::ofstream(fixture.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"Renamed Lobby\"\n";
    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "Edited instructions\n";
    fixture.add_persona("writer", "Writer");

    EXPECT_EQ(display_names(model.characters()), characters);
    EXPECT_EQ(display_names(model.forums()), forums);
    EXPECT_EQ(model.personas()->size(), persona_count);
    EXPECT_EQ(model.find_character("guide")->display_name, "Guide");
    EXPECT_EQ(model.find_forum("lobby")->display_name, "The Lobby");
    EXPECT_EQ(model.character_markdown("guide"), "Character instructions\n");

    const WorkspaceDefinition reloaded = load_model(fixture.root());
    EXPECT_EQ(reloaded.find_character("guide")->display_name, "Renamed");
    EXPECT_EQ(reloaded.find_forum("lobby")->display_name, "Renamed Lobby");
    EXPECT_EQ(reloaded.character_markdown("guide"), "Edited instructions\n");
    EXPECT_EQ(reloaded.personas()->size(), persona_count + 1);
}

// --- Workspace layout and validation ---------------------------------------

// A hand-built workspace, so each test can break exactly one part of the
// directory graph the model validates at load.
class WorkspaceDefinitionLayoutTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_workspace_definition_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "characters" / "guide");
        std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "guide");
        std::filesystem::create_directories(root_ / "personas" / "operator");
        write_provider("test", "host = \"127.0.0.1\"\nport = 8080\nmode = \"test\"\nmodel = \"fake\"\n");
        std::filesystem::create_directories(root_ / "system" / "assistant");
        std::ofstream(root_ / "system" / "assistant" / "character.toml")
            << "display_name = \"Assistant\"\nprovider = \"test\"\n";
        std::ofstream(root_ / "workspace.toml")
            << "[logging]\n"
               "file = \"logs/cha.log\"\n"
               "level = \"off\"\n";
        std::ofstream(root_ / "forums" / "lobby" / "config.toml")
            << "display_name = \"The Lobby\"\n";
        std::ofstream(root_ / "forums" / "lobby" / "FORUM.md") << "Forum instructions";
        std::ofstream(root_ / "forums" / "lobby" / "members" / "character_defaults.toml")
            << "# Provider selection is per character.\n";
        std::ofstream(root_ / "characters" / "guide" / "character.toml")
            << "display_name = \"Guide\"\nprovider = \"test\"\n";
        std::ofstream(root_ / "characters" / "guide" / "CHARACTER.md")
            << "Character instructions";
        std::ofstream(root_ / "personas" / "operator" / "persona.toml")
            << "display_name = \"Reader\"\n";
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    WorkspaceDefinition load() const { return load_model(root_); }

    void write_provider(std::string_view name, std::string_view contents) const {
        const std::filesystem::path directory = providers_directory(root_) / std::string(name);
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "config.toml") << contents;
    }

    std::filesystem::path root_;
};

TEST_F(WorkspaceDefinitionLayoutTest, ResolvesLoggingSettingsRelativeToTheWorkspace) {
    const WorkspaceConfig config = load_workspace_config(root_);
    EXPECT_EQ(config.log_file, root_ / "logs" / "cha.log");
    EXPECT_EQ(config.log_level, "off");
    EXPECT_NO_THROW((void)WorkspaceDefinition::load(root_, config));
}

TEST_F(WorkspaceDefinitionLayoutTest, LeavesAnAbsoluteLogPathUntouched) {
    const std::filesystem::path absolute_log = root_ / "elsewhere" / "cha.log";
    std::ofstream(root_ / "workspace.toml")
        << "[logging]\nfile = \"" << absolute_log.string()
        << "\"\nlevel = \"off\"\n";

    EXPECT_EQ(load_workspace_config(root_).log_file, absolute_log);
}

TEST_F(WorkspaceDefinitionLayoutTest, DerivesProviderAndStyleDirectoriesFromTheRoot) {
    write_provider("named", "host = \"provider.example\"\nport = 444\nmode = \"test\"\n");
    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nprovider = 42\nhost = \"ignored\"\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    const WorkspaceConfig config = load_workspace_config(root_);
    EXPECT_EQ(config.providers_directory, providers_directory(root_));
    EXPECT_EQ(config.styles_directory, styles_directory(root_));
    EXPECT_NO_THROW((void)WorkspaceDefinition::load(root_, config));
}

TEST_F(WorkspaceDefinitionLayoutTest, IgnoresWorkspaceProviderConfiguration) {
    std::ofstream(root_ / "workspace.toml")
        << "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_NO_THROW((void)load());

    for (const std::string_view table :
            {"display_name = \"No\"\nprovider = \"test\"\n", "provider = \"test\"\nhtps = true\n",
             "provider = \"test\"\napi_key = \"secret\"\n", "host = \"test\"\nport = 1\n",
             "provider = \"absent\"\n", ""}) {
        std::ofstream(root_ / "workspace.toml")
            << "[provider]\n" << table
            << "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
        EXPECT_NO_THROW((void)load());
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, RequiresWorkspaceConfiguration) {
    std::filesystem::remove(root_ / "workspace.toml");

    try {
        (void)load();
        FAIL() << "expected missing workspace config to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("workspace.toml"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, RequiresValidLoggingConfiguration) {
    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nprovider = \"test\"\n";
    EXPECT_THROW((void)load_workspace_config(root_), std::runtime_error);

    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nprovider = \"test\"\n"
           "[logging]\nfile = \"\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)load_workspace_config(root_), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, RequiresWorkspaceDefinitions) {
    std::filesystem::remove_all(root_ / "characters");
    try {
        (void)load();
        FAIL() << "expected missing definitions directory to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(root_.string()), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("characters/"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, RequiresAForumsDirectory) {
    std::filesystem::remove_all(root_ / "forums");
    try {
        (void)load();
        FAIL() << "expected missing forums directory to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("forums/"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, RequiresMembersDirectory) {
    std::filesystem::remove_all(root_ / "forums" / "lobby" / "members");
    try {
        (void)load();
        FAIL() << "expected missing members directory to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("members/"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsDanglingMember) {
    std::filesystem::rename(
        root_ / "forums" / "lobby" / "members" / "guide",
        root_ / "forums" / "lobby" / "members" / "unknown");
    try {
        (void)load();
        FAIL() << "expected dangling member to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("unknown"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsInvalidMemberIdBeforeReferenceValidation) {
    std::filesystem::rename(
        root_ / "forums" / "lobby" / "members" / "guide",
        root_ / "forums" / "lobby" / "members" / "guide.invalid");
    try {
        (void)load();
        FAIL() << "expected invalid member ID to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("guide.invalid"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Character ID"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, ValidatesRequiredDefinitionFiles) {
    std::filesystem::remove(root_ / "characters" / "guide" / "character.toml");
    EXPECT_THROW((void)load(), std::runtime_error);

    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"Guide\"\n";
    std::filesystem::remove(root_ / "characters" / "guide" / "CHARACTER.md");
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, LoadsCharactersFromNestedGroupingDirectories) {
    const std::filesystem::path group = root_ / "characters" / "1" / "AAA";
    const std::filesystem::path guide = group / "guide";
    const std::filesystem::path begemot = group / "begemot";
    std::filesystem::create_directories(group);
    std::filesystem::rename(root_ / "characters" / "guide", guide);
    std::filesystem::create_directories(begemot);
    std::ofstream(begemot / "character.toml") << "display_name = \"Begemot\"\n";
    std::ofstream(begemot / "CHARACTER.md") << "Character instructions";

    const WorkspaceDefinition model = load();
    EXPECT_NE(model.find_character("guide"), nullptr);
    EXPECT_NE(model.find_character("begemot"), nullptr);
    EXPECT_EQ(model.character_config_path("guide"), guide / "character.toml");
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsMalformedDefinition) {
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = [\"Guide\"]\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsInvalidDefinitionId) {
    std::filesystem::rename(
        root_ / "characters" / "guide", root_ / "characters" / "guide.invalid");
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsReservedDefinitionDisplayName) {
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"system\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsDuplicateCharacterDisplayNames) {
    const std::filesystem::path duplicate = root_ / "characters" / "other";
    std::filesystem::create_directories(duplicate);
    std::ofstream(duplicate / "character.toml") << "display_name = \"gUiDe\"\n";
    std::ofstream(duplicate / "CHARACTER.md") << "Other instructions";
    try {
        (void)load();
        FAIL() << "expected duplicate character display name rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("gUiDe"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("public name"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, AcceptsEmptyMemberDirectoriesAndOrphanDefinitions) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "members" / "guide" / "character.toml");
    std::filesystem::create_directories(root_ / "characters" / "orphan");
    std::ofstream(root_ / "characters" / "orphan" / "character.toml")
        << "display_name = \"Orphan\"\n";
    std::ofstream(root_ / "characters" / "orphan" / "CHARACTER.md") << "Orphan";
    EXPECT_NO_THROW((void)load());
}

TEST_F(WorkspaceDefinitionLayoutTest, PublishesForumMarkdownVerbatim) {
    std::ofstream(root_ / "forums" / "lobby" / "FORUM.md")
        << "# House rules\n\nTake turns and stay in character.\n";
    EXPECT_EQ(
        load().forum_markdown("lobby"),
        "# House rules\n\nTake turns and stay in character.\n");

    // The template source is published, not an expansion: a description belongs
    // to the forum rather than to any one member, so there is no character to
    // expand against and an include stays unexpanded.
    std::ofstream(root_ / "forums" / "lobby" / "INCLUDED.md") << "Included body";
    std::ofstream(root_ / "forums" / "lobby" / "FORUM.md")
        << "Ask $${character.display_name}.\n$$(INCLUDED.md)\n";
    EXPECT_EQ(
        load().forum_markdown("lobby"),
        "Ask $${character.display_name}.\n$$(INCLUDED.md)\n");

    // The built-in Entrance keeps no forum directory, so it has none to publish.
    EXPECT_EQ(load().forum_markdown(std::string(entrance_id)), "");
    EXPECT_THROW((void)load().forum_markdown("missing"), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, ReadsAnOptionalShortForumDescription) {
    EXPECT_FALSE(load().find_forum("lobby")->description);
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndescription = \"Where visitors arrive\"\n";
    const WorkspaceDefinition model = load();
    ASSERT_TRUE(model.find_forum("lobby")->description);
    EXPECT_EQ(*model.find_forum("lobby")->description, "Where visitors arrive");

    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndescription = \" bad\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, EnumeratesForumsInNameOrderAndIgnoresUnsafeDirectories) {
    for (const std::string_view name : {"alpha", "zulu", "bad space", "bad#fragment"}) {
        const std::filesystem::path forum = root_ / "forums" / std::string(name);
        std::filesystem::create_directories(forum / "members" / "guide");
        std::ofstream(forum / "config.toml")
            << "display_name = \"Forum " << name << "\"\n";
        std::ofstream(forum / "FORUM.md") << "Forum";
    }
    const WorkspaceDefinition model = load();

    EXPECT_EQ(
        display_names(model.forums()),
        (std::vector<std::string>{"Entrance", "Forum alpha", "Forum zulu", "The Lobby"}));
    EXPECT_EQ(model.session_directories().size(), 3U);
    for (const std::string_view id : {"alpha", "lobby", "zulu"}) {
        EXPECT_NE(model.find_forum(id), nullptr) << id;
    }
    EXPECT_EQ(model.find_forum("bad space"), nullptr);
    EXPECT_EQ(model.find_forum("bad#fragment"), nullptr);
}

TEST_F(WorkspaceDefinitionLayoutTest, ResolvesDefaultCharacterWithoutReorderingMembers) {
    std::filesystem::create_directories(root_ / "characters" / "alpha");
    std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "alpha");
    std::ofstream(root_ / "characters" / "alpha" / "character.toml")
        << "display_name = \"Alpha\"\nprovider = \"test\"\n";
    std::ofstream(root_ / "characters" / "alpha" / "CHARACTER.md") << "Alpha";
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_character = \"guide\"\n";

    const WorkspaceDefinition model = load();
    const ForumInfo* const forum = model.find_forum("lobby");
    ASSERT_NE(forum, nullptr);
    EXPECT_EQ(forum->member_ids, (std::vector<std::string>{"alpha", "guide"}));
    EXPECT_EQ(forum->default_character_id, "guide");
}

TEST_F(WorkspaceDefinitionLayoutTest, DefaultsToFirstLexicographicMember) {
    std::filesystem::create_directories(root_ / "characters" / "alpha");
    std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "alpha");
    std::ofstream(root_ / "characters" / "alpha" / "character.toml")
        << "display_name = \"Alpha\"\nprovider = \"test\"\n";
    std::ofstream(root_ / "characters" / "alpha" / "CHARACTER.md") << "Alpha";

    const WorkspaceDefinition model = load();
    const ForumInfo* const forum = model.find_forum("lobby");
    ASSERT_NE(forum, nullptr);
    EXPECT_EQ(forum->default_character_id, "alpha");
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsUnknownAndMalformedDefaultCharacter) {
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_character = \"missing\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);

    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_character = [\"guide\"]\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsAnUnknownForumConfigField) {
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefualt_persona = \"operator\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, ResolvesAndValidatesForumDefaultPersona) {
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_persona = \"operator\"\n";
    EXPECT_EQ(load().find_forum("lobby")->default_persona_id, "operator");

    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_persona = \"missing\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);

    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_persona = [\"operator\"]\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, AcceptsLegacyDefaultAgentButRejectsBothKeys) {
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_agent = \"guide\"\n";
    EXPECT_EQ(load().find_forum("lobby")->default_character_id, "guide");

    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_character = \"guide\"\n"
           "default_agent = \"guide\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceDefinitionLayoutTest, RequiresPersonaDirectoryButAllowsItEmpty) {
    std::filesystem::remove_all(root_ / "personas");
    EXPECT_THROW((void)load(), std::runtime_error);

    std::filesystem::create_directories(root_ / "personas");
    EXPECT_TRUE(custom_personas(load()).empty());
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsPersonaCharacterIdCollision) {
    std::filesystem::rename(root_ / "personas" / "operator", root_ / "personas" / "guide");
    try {
        (void)load();
        FAIL() << "expected persona/character ID collision rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("guide"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Persona"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("character"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, RejectsPersonaCharacterDisplayNameCollision) {
    std::ofstream(root_ / "personas" / "operator" / "persona.toml")
        << "display_name = \"gUiDe\"\n";
    try {
        (void)load();
        FAIL() << "expected persona/character display-name collision rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("gUiDe"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Guide"), std::string::npos);
    }
}

TEST_F(WorkspaceDefinitionLayoutTest, AcceptsDefinitionDefaultsWithAMemberOverride) {
    write_provider("forum", "host = \"127.0.0.1\"\nport = 8080\nmode = \"test\"\n");
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"Guide\"\nprovider = \"test\"\n";
    std::ofstream(root_ / "forums" / "lobby" / "members" / "character_defaults.toml")
        << "provider = \"forum\"\n";
    std::ofstream(root_ / "forums" / "lobby" / "members" / "guide" / "character.toml")
        << "[prompt]\nvoice = \"member\"\n";
    std::ofstream(root_ / "characters" / "guide" / "CHARACTER.md") << "$${voice}";

    EXPECT_NO_THROW((void)load());
}

TEST_F(WorkspaceDefinitionLayoutTest, LoadsWithoutSharedCharacterDefaults) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "members" / "character_defaults.toml");
    EXPECT_NO_THROW((void)load());
}

// --- Persona loading --------------------------------------------------------

class WorkspaceDefinitionPersonaTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_model_personas_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "forums");
        std::filesystem::create_directories(root_ / "characters" / "guide");
        std::ofstream(root_ / "characters" / "guide" / "character.toml")
            << "display_name = \"Guide\"\nprovider = \"test\"\n";
        std::ofstream(root_ / "characters" / "guide" / "CHARACTER.md") << "Guide";
        const std::filesystem::path provider = providers_directory(root_) / "test";
        std::filesystem::create_directories(provider);
        std::ofstream(provider / "config.toml")
            << "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n";
        std::filesystem::create_directories(root_ / "system" / "assistant");
        std::ofstream(root_ / "system" / "assistant" / "character.toml")
            << "display_name = \"Assistant\"\nprovider = \"test\"\n";
        std::ofstream(root_ / "workspace.toml")
            << "[logging]\n"
               "file = \"cha.log\"\n"
               "level = \"off\"\n";
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    void add_persona(
        std::string_view id,
        std::string_view display_name,
        std::string_view prompt = "") const {
        const std::filesystem::path directory = root_ / "personas" / std::string(id);
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "persona.toml")
            << "display_name = \"" << display_name << "\"\n";
        if (!prompt.empty()) std::ofstream(directory / "PERSONA.md") << prompt;
    }

    PersonaRoster personas() const { return custom_personas(load_model(root_)); }

    std::filesystem::path root_;
};

TEST_F(WorkspaceDefinitionPersonaTest, PublishesPersonasInDisplayNameOrderAndReadsOptionalPrompt) {
    add_persona("zebra", "Zebra", "Verbatim\ntext");
    add_persona("alpha", "Alpha");

    const PersonaRoster roster = personas();

    ASSERT_EQ(roster.size(), 2U);
    EXPECT_EQ(roster[0].id, "alpha");
    EXPECT_EQ(roster[0].display_name, "Alpha");
    EXPECT_TRUE(roster[0].prompt.empty());
    EXPECT_EQ(roster[1].id, "zebra");
    EXPECT_EQ(roster[1].display_name, "Zebra");
    EXPECT_EQ(roster[1].prompt, "Verbatim\ntext");
}

TEST_F(WorkspaceDefinitionPersonaTest, LoadsOptionalDescriptionAndRejectsMalformedDescription) {
    add_persona("ada", "Ada");
    const auto config = root_ / "personas" / "ada" / "persona.toml";
    std::ofstream(config) << "display_name = \"Ada\"\ndescription = \"A careful reader\"\n";
    const PersonaRoster roster = personas();
    ASSERT_TRUE(roster[0].description);
    EXPECT_EQ(*roster[0].description, "A careful reader");

    std::ofstream(config) << "display_name = \"Ada\"\ndescription = \" bad\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceDefinitionPersonaTest, RequiresAPersonaTomlForEveryPersonaDirectory) {
    const std::filesystem::path directory = root_ / "personas" / "missing";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "PERSONA.md") << "Prompt";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceDefinitionPersonaTest, LoadsPersonasFromNestedGroupingDirectories) {
    add_persona("ada", "Ada");
    const std::filesystem::path group = root_ / "personas" / "1" / "AAA";
    std::filesystem::create_directories(group);
    std::filesystem::rename(root_ / "personas" / "ada", group / "ada");
    const std::filesystem::path beatrice = group / "beatrice";
    std::filesystem::create_directories(beatrice);
    std::ofstream(beatrice / "persona.toml") << "display_name = \"Beatrice\"\n";

    const PersonaRoster roster = personas();
    ASSERT_EQ(roster.size(), 2U);
    EXPECT_EQ(roster[0].id, "ada");
    EXPECT_EQ(roster[1].id, "beatrice");
}

TEST_F(WorkspaceDefinitionPersonaTest, RequiresPersonaPromptToBeARegularFile) {
    add_persona("ada", "Ada");
    std::filesystem::create_directory(root_ / "personas" / "ada" / "PERSONA.md");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceDefinitionPersonaTest, RejectsUnknownOrMissingOrWrongTypedConfigFields) {
    add_persona("ada", "Ada");
    const auto config = root_ / "personas" / "ada" / "persona.toml";

    std::ofstream(config) << "display_name = \"Ada\"\nextra = true\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "other = \"Ada\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = 42\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceDefinitionPersonaTest, RejectsInvalidAndReservedIds) {
    add_persona("bad-id", "Ada");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::filesystem::remove_all(root_ / "personas");
    add_persona("9ada", "Ada");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::filesystem::remove_all(root_ / "personas");
    add_persona("System", "Ada");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceDefinitionPersonaTest, RejectsInvalidReservedAndDuplicateDisplayNames) {
    add_persona("ada", " Ada");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    const auto config = root_ / "personas" / "ada" / "persona.toml";
    std::ofstream(config) << "display_name = \"@Ada\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"System\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ada\\u0001\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ada\"\n";
    add_persona("other", "aDA");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceDefinitionPersonaTest, RejectsUnicodeControlsLineBreaksAndBoundaryWhitespace) {
    add_persona("ada", "Ada");
    const std::filesystem::path config = root_ / "personas" / "ada" / "persona.toml";

    std::ofstream(config) << "display_name = \"Ada\\u0085Lovelace\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ada\\u2028Lovelace\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"\\u00a0Ada\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ada\\u3000\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ren\\u00e9e \\u674e\"\n";
    const PersonaRoster roster = personas();
    ASSERT_EQ(roster.size(), 1U);
    EXPECT_EQ(roster.front().display_name, "Renée 李");
}

} // namespace
} // namespace cha
