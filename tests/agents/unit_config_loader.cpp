#include "agents/character_config.h"
#include "util/path_name.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
namespace {

class ConfigFiles {
public:
    ConfigFiles()
        : root_(std::filesystem::temp_directory_path() / ("cha_config_"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))),
          definition_(root_ / "definition" / "character.toml"),
          defaults_(root_ / "forum" / "members" / "character_defaults.toml"),
          member_(root_ / "forum" / "members" / "member" / "character.toml"),
          providers_(root_ / "system" / "providers") {
        std::filesystem::create_directories(definition_.parent_path());
        std::filesystem::create_directories(defaults_.parent_path());
        std::filesystem::create_directories(member_.parent_path());
        write(application_, "[provider]\nprovider = \"application\"\n");
        write_provider("application", "host = \"application\"\nport = 81\nmode = \"test\"\n");
    }
    ~ConfigFiles() { std::filesystem::remove_all(root_); }
    void write(const std::filesystem::path& path, std::string_view contents) const {
        std::ofstream file(path);
        file << contents;
    }
    void write_provider(std::string_view name, std::string_view contents) const {
        const std::filesystem::path path = providers_ / std::string(name) / "config.toml";
        std::filesystem::create_directories(path.parent_path());
        write(path, contents);
    }
    // The workspace provider is supplied as an already-resolved layer, exactly
    // as WorkspaceDefinition passes it in.
    CharacterConfigPaths paths(bool defaults = false, bool member = false, bool application = true) const {
        return {.providers = {application ? std::optional{ProviderConfig{
                    .host = "application", .port = 81, .mode = Mode::test}} : std::nullopt,
                providers_},
            .definition = definition_,
            .forum_defaults = defaults ? std::optional{defaults_} : std::nullopt,
            .member_override = member ? std::optional{member_} : std::nullopt};
    }
    const std::filesystem::path& definition() const { return definition_; }
    const std::filesystem::path& defaults() const { return defaults_; }
    const std::filesystem::path& member() const { return member_; }
    const std::filesystem::path& application() const { return application_; }
    const std::filesystem::path& providers() const { return providers_; }
    std::filesystem::path provider(std::string_view name) const {
        return providers_ / std::string(name) / "config.toml";
    }
private:
    std::filesystem::path root_, application_ = root_ / "workspace.toml", definition_, defaults_, member_, providers_;
};

constexpr std::string_view required_definition = "display_name = \"Example\"\n";

void expect_error_containing(const std::function<void()>& operation,
    const std::filesystem::path& path, std::string_view field) {
    try {
        operation();
        FAIL() << "Expected config load to fail";
    } catch (const std::runtime_error& error) {
        const std::string message(error.what());
        EXPECT_NE(message.find(utf8_path(path)), std::string::npos);
        EXPECT_NE(message.find(field), std::string::npos);
    }
}

constexpr std::string_view complete_provider =
    "host = \"definition\"\nport = 80\nmode = \"test\"\nmodel = \"one\"\nstream = true\n"
    "temperature = 0.1\napi_key_env = \"ONE\"\nreasoning_effort = \"low\"\n"
    "reasoning_format = \"none\"\nhttps = false\nbase_path = \"/definition\"\n";

void expect_complete_provider_values(const ModelBackendConfig& config) {
    EXPECT_EQ(config.host, "definition");
    EXPECT_EQ(config.port, 80);
    EXPECT_EQ(config.base_path, "/definition");
    EXPECT_EQ(config.mode, Mode::test);
    EXPECT_EQ(config.model, "one");
    EXPECT_TRUE(config.stream);
    EXPECT_DOUBLE_EQ(config.temperature, 0.1);
    EXPECT_EQ(config.api_key_env, "ONE");
    EXPECT_EQ(config.reasoning_effort, "low");
    EXPECT_EQ(config.reasoning_format, ReasoningFormat::none);
    EXPECT_FALSE(config.https);
}

TEST(Config, BackendMaterializationRequiresHostAndPort) {
    EXPECT_THROW(
        (void)make_backend_config(ProviderConfig{.port = 80}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)make_backend_config(ProviderConfig{.host = "provider.example"}),
        std::invalid_argument);
}

TEST(Config, ReadsEveryProviderSettingFromTheReferencedConfig) {
    ConfigFiles files;
    files.write_provider("complete", complete_provider);
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"complete\"\n");
    expect_complete_provider_values(load_character_config(files.paths()).backend);
}

TEST(Config, DerivesTheProvidersDirectoryFromTheWorkspaceRoot) {
    EXPECT_EQ(providers_directory("/workspace"),
        std::filesystem::path("/workspace") / "system" / "providers");
}

// A provider config is a whole backend. Selecting one must not leave stray
// values from the layer below in the effective configuration.
TEST(Config, AHigherLayerProviderReplacesTheLowerOneOutright) {
    ConfigFiles files;
    files.write_provider("definition",
        "host = \"definition.example\"\nport = 443\nmode = \"test\"\nmodel = \"one\"\n"
        "temperature = 0.5\nreasoning_effort = \"low\"\nbase_path = \"/definition\"\n");
    files.write_provider("forum",
        "host = \"forum.example\"\nport = 8443\nmode = \"net\"\nmodel = \"two\"\n");
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"definition\"\n");
    files.write(files.defaults(), "provider = \"forum\"\n");

    const ModelBackendConfig config =
        load_character_config(files.paths(true)).backend;
    EXPECT_EQ(config.host, "forum.example");
    EXPECT_EQ(config.port, 8443);
    EXPECT_EQ(config.mode, Mode::net);
    EXPECT_EQ(config.model, "two");
    EXPECT_DOUBLE_EQ(config.temperature, 1.0);
    EXPECT_TRUE(config.reasoning_effort.empty());
    EXPECT_TRUE(config.base_path.empty());
}

TEST(Config, EachLayerCanSelectTheProviderAndTheHighestOneWins) {
    ConfigFiles files;
    for (const std::string_view name : {"definition", "forum", "member"}) {
        files.write_provider(name,
            "host = \"" + std::string(name) + "\"\nport = 443\nmode = \"test\"\n");
    }
    files.write(files.definition(), required_definition);
    EXPECT_EQ(load_character_config(files.paths()).backend.host, "application");

    files.write(files.definition(),
        std::string(required_definition) + "provider = \"definition\"\n");
    EXPECT_EQ(load_character_config(files.paths()).backend.host, "definition");

    files.write(files.defaults(), "provider = \"forum\"\n");
    EXPECT_EQ(load_character_config(files.paths(true)).backend.host, "forum");

    files.write(files.member(), "provider = \"member\"\n");
    EXPECT_EQ(load_character_config(files.paths(true, true)).backend.host, "member");
}

TEST(Config, EmptyAndCommentOnlyOptionalLayersKeepTheProviderBelow) {
    ConfigFiles files;
    files.write_provider("complete", complete_provider);
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"complete\"\n");
    for (const std::string_view contents : {"", "# nothing here\n"}) {
        files.write(files.defaults(), contents);
        expect_complete_provider_values(load_character_config(files.paths(true)).backend);
        files.write(files.member(), contents);
        expect_complete_provider_values(
            load_character_config(files.paths(true, true)).backend);
    }
}

TEST(Config, MergesThePromptScopeAcrossLayersWhileTheProviderIsReplaced) {
    ConfigFiles files;
    files.write_provider("definition", "host = \"definition\"\nport = 80\nmode = \"test\"\n");
    files.write_provider("member", "host = \"member\"\nport = 8443\nmode = \"net\"\n");
    files.write(files.definition(),
        std::string(required_definition)
            + "provider = \"definition\"\n[prompt]\nvalue = \"definition\"\nbase = \"base\"\n");
    files.write(files.defaults(), "[prompt]\nvalue = \"defaults\"\ndefault = \"default\"\n");
    files.write(files.member(),
        "provider = \"member\"\n[prompt]\nvalue = \"member\"\nmember = \"member\"\n");

    const LoadedCharacterConfig loaded = load_character_config(files.paths(true, true));
    EXPECT_EQ(loaded.backend.host, "member");
    EXPECT_EQ(loaded.prompt_variables.at("value"), "member");
    EXPECT_EQ(loaded.prompt_variables.at("base"), "base");
    EXPECT_EQ(loaded.prompt_variables.at("default"), "default");
    EXPECT_EQ(loaded.prompt_variables.at("member"), "member");
}

TEST(Config, SeparatesDefinitionMetadataFromModelBackendConfiguration) {
    ConfigFiles files;
    files.write_provider("named", "host = \"named.example\"\nport = 8080\nmode = \"test\"\n");
    files.write(files.definition(),
        std::string(required_definition)
            + "description = \"Useful character\"\nprovider = \"named\"\n");
    const LoadedCharacterConfig loaded = load_character_config(files.paths());
    EXPECT_EQ(loaded.character.id, "definition");
    EXPECT_EQ(loaded.character.display_name, "Example");
    EXPECT_EQ(loaded.character.description, "Useful character");
    EXPECT_EQ(loaded.backend.host, "named.example");
    EXPECT_EQ(loaded.backend.port, 8080);
}

// Provider settings are centralized, so a file that still spells one out is
// stale configuration rather than a silent no-op.
TEST(Config, RejectsInlineProviderSettingsInEveryCharacterLayer) {
    ConfigFiles files;
    for (const std::string_view setting :
            {"host = \"example\"\n", "port = 80\n", "model = \"one\"\n",
             "api = \"responses\"\n", "web_search = \"off\"\n", "api_key = \"secret\"\n",
             "base_path = \"/api\"\n", "temperature = 0.5\n"}) {
        files.write(files.definition(), std::string(required_definition) + std::string(setting));
        expect_error_containing(
            [&] { (void)load_character_config(files.paths()); },
            files.definition(),
            "provider setting");
    }
    files.write(files.definition(), required_definition);
    files.write(files.defaults(), "model = \"one\"\n");
    expect_error_containing(
        [&] { (void)load_character_config(files.paths(true)); },
        files.defaults(),
        "provider setting");
    files.write(files.defaults(), "");
    files.write(files.member(), "host = \"example\"\n");
    expect_error_containing(
        [&] { (void)load_character_config(files.paths(true, true)); },
        files.member(),
        "provider setting");
}

TEST(Config, RejectsInlineProviderSettingsInTheWorkspaceTable) {
    ConfigFiles files;
    files.write(files.application(),
        "[provider]\nprovider = \"application\"\nhost = \"elsewhere\"\n");
    expect_error_containing(
        [&] { (void)load_provider_config(files.application()); },
        files.application(),
        "provider setting");
    files.write(files.application(), "[provider]\napi_key = \"secret\"\n");
    expect_error_containing(
        [&] { (void)load_provider_config(files.application()); },
        files.application(),
        "api_key");
    files.write(files.application(), "[provider]\nhtps = true\n");
    expect_error_containing(
        [&] { (void)load_provider_config(files.application()); },
        files.application(),
        "htps");
    files.write(files.application(), "[provider]\n");
    expect_error_containing(
        [&] { (void)load_provider_config(files.application()); },
        files.application(),
        "requires a 'provider' name");
}

TEST(Config, RejectsAProviderReferenceWithoutAConfigFile) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"missing\"\n");
    expect_error_containing(
        [&] { (void)load_character_config(files.paths()); },
        files.definition(),
        "missing");
}

TEST(Config, RejectsAProviderNameThatEscapesTheProvidersDirectory) {
    ConfigFiles files;
    for (const std::string_view name : {"../escape", "nested/name", ".."}) {
        files.write(files.definition(),
            std::string(required_definition) + "provider = \"" + std::string(name) + "\"\n");
        expect_error_containing(
            [&] { (void)load_character_config(files.paths()); },
            files.definition(),
            name);
    }
}

TEST(Config, RequiresAProviderSomewhereBelowTheCharacter) {
    ConfigFiles files;
    files.write(files.definition(), required_definition);
    expect_error_containing(
        [&] { (void)load_character_config(files.paths(false, false, false)); },
        files.definition(),
        "selects no provider");
}

TEST(Config, ResolvesTheWorkspaceProviderReferenceAndRejectsAMissingOne) {
    ConfigFiles files;
    files.write_provider("workspace", "host = \"workspace.example\"\nport = 443\nmode = \"test\"\n");
    files.write(files.application(), "[provider]\nprovider = \"workspace\"\n");
    const ProviderConfig provider = load_provider_config(files.application());
    EXPECT_EQ(provider.host, "workspace.example");
    EXPECT_EQ(provider.port, 443);

    files.write(files.application(), "[provider]\nprovider = \"missing\"\n");
    expect_error_containing(
        [&] { (void)load_provider_config(files.application()); },
        files.application(),
        "missing");
}

TEST(Config, RejectsDefinitionOnlyAndRemovedIdentityFieldsInEveryLayer) {
    ConfigFiles files;
    for (const std::string_view key : {"name", "id"}) {
        files.write(files.definition(),
            std::string(required_definition) + std::string(key) + " = \"Wrong\"\n");
        expect_error_containing([&] { (void)load_character_config(files.paths()); }, files.definition(), key);
    }
    files.write(files.definition(), required_definition);
    for (const auto& path : {files.defaults(), files.member()}) {
        files.write(path, "display_name = \"Wrong\"\n");
        expect_error_containing(
            [&] { (void)load_character_config(files.paths(path == files.defaults(), path == files.member())); },
            path,
            "display_name");
        files.write(path, "tags = [\"Wrong\"]\n");
        expect_error_containing(
            [&] { (void)load_character_config(files.paths(path == files.defaults(), path == files.member())); },
            path,
            "tags");
        files.write(path, "name = \"Wrong\"\n");
        expect_error_containing(
            [&] { (void)load_character_config(files.paths(path == files.defaults(), path == files.member())); },
            path,
            "name");
        files.write(path, "id = \"wrong\"\n");
        expect_error_containing(
            [&] { (void)load_character_config(files.paths(path == files.defaults(), path == files.member())); },
            path,
            "id");
    }
}

// Every provider value is validated where it is written, so the message names
// the provider config rather than the character that referenced it.
TEST(Config, AttributesProviderValidationToTheProviderConfig) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"broken\"\n");
    const auto expect_provider_error = [&](std::string_view contents, std::string_view field) {
        files.write_provider("broken", contents);
        expect_error_containing(
            [&] { (void)load_character_config(files.paths()); },
            files.provider("broken"),
            field);
    };
    expect_provider_error("port = 80\n", "host");
    expect_provider_error("host = \"example\"\n", "port");
    expect_provider_error("host = \"example\"\nport = 65536\n", "port");
    expect_provider_error("host = \"example\"\nport = 80\ntemperature = 2.1\n", "temperature");
    expect_provider_error("host = \"example\"\nport = 80\nbase_path = \"api\"\n", "base_path");
    expect_provider_error("host = \"example\"\nport = 80\nmode = \"invalid\"\n", "mode");
    expect_provider_error("host = \"example\"\nport = 80\nreasoning_format = \"invalid\"\n", "reasoning_format");
    expect_provider_error("host = \"example\"\nport = 80\napi = \"completion\"\n", "api");
    expect_provider_error("host = \"example\"\nport = 80\nweb_search = \"maybe\"\n", "web_search");
    expect_provider_error("host = \"example\"\nport = 80\ndisplay_name = \"No\"\n", "display_name");
    expect_provider_error("host = \"example\"\nport = 80\napi_key = \"secret\"\n", "api_key");
}

TEST(Config, ValidatesCharacterDescriptionsAndRejectsThemOutsideDefinitions) {
    ConfigFiles files;
    files.write(files.definition(), "display_name = \"Example\"\ndescription = \"Useful character\"\n");
    EXPECT_EQ(load_character_metadata(files.definition()).description, "Useful character");
    files.write(files.definition(), "display_name = \"Example\"\ndescription = \" bad\"\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\n");
    files.write(files.defaults(), "description = \"Not here\"\n");
    EXPECT_THROW((void)load_character_config(files.paths(true)), std::runtime_error);
}

TEST(Config, ValidatesMetadataProviderReferencesWhenAProvidersDirectoryIsGiven) {
    ConfigFiles files;
    files.write_provider("named", "host = \"named.example\"\nport = 8080\nmode = \"test\"\n");
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"named\"\n");
    EXPECT_EQ(load_character_metadata(files.definition(), files.providers()).display_name, "Example");
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"missing\"\n");
    expect_error_containing(
        [&] { (void)load_character_metadata(files.definition(), files.providers()); },
        files.definition(),
        "missing");
}

TEST(Config, ValidatesForumProviderDefaultsWithoutLoadingAnyMember) {
    ConfigFiles files;
    files.write_provider("forum", "host = \"forum.example\"\nport = 443\nmode = \"test\"\n");
    files.write(files.defaults(), "provider = \"forum\"\n");
    EXPECT_NO_THROW(validate_forum_provider_defaults(files.defaults(), files.providers()));
    files.write(files.defaults(), "provider = \"missing\"\n");
    expect_error_containing(
        [&] { validate_forum_provider_defaults(files.defaults(), files.providers()); },
        files.defaults(),
        "missing");
}

TEST(Config, DefaultsCharacterAppearanceToTheInterfaceSettings) {
    ConfigFiles files;
    files.write(files.definition(), "display_name = \"Example\"\n");
    const CharacterAppearance appearance =
        load_character_metadata(files.definition()).appearance;
    EXPECT_EQ(appearance, CharacterAppearance{});
    EXPECT_EQ(appearance.font, CharacterFont::sans);
    EXPECT_EQ(appearance.size, CharacterScale::normal);
    EXPECT_EQ(load_character_config(files.paths()).character.appearance, CharacterAppearance{});
}

TEST(Config, ReadsEveryCharacterAppearanceFieldAndCarriesItIntoTheEffectiveConfig) {
    ConfigFiles files;
    files.write(files.definition(),
        "display_name = \"Seneca\"\n"
        "[appearance]\n"
        "font = \"serif\"\n"
        "style = \"italic\"\n"
        "weight = \"bold\"\n"
        "size = \"large\"\n");
    const CharacterAppearance expected{CharacterFont::serif, CharacterSlant::italic,
        CharacterWeight::bold, CharacterScale::large};
    EXPECT_EQ(load_character_metadata(files.definition()).appearance, expected);
    EXPECT_EQ(load_character_config(files.paths()).character.appearance, expected);
}

TEST(Config, RejectsUnknownAppearanceFieldsValuesAndPlacements) {
    ConfigFiles files;
    const std::string named = "display_name = \"Example\"\n";
    files.write(files.definition(), named + "[appearance]\nfont = \"comic\"\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), named + "[appearance]\ncolour = \"red\"\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), named + "[appearance]\nfont = 3\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), named + "appearance = \"serif\"\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    // Appearance belongs to the character, not to one forum's use of it.
    files.write(files.definition(), named);
    files.write(files.defaults(), "[appearance]\nfont = \"serif\"\n");
    EXPECT_THROW((void)load_character_config(files.paths(true)), std::runtime_error);
}

TEST(Config, LoadsAndValidatesDefinitionMetadataTags) {
    ConfigFiles files;
    files.write(files.definition(), "display_name = \"Example\"\n");
    EXPECT_TRUE(load_character_metadata(files.definition()).tags.empty());
    files.write(files.definition(), "display_name = \"Example\"\ntags = []\n");
    EXPECT_TRUE(load_character_metadata(files.definition()).tags.empty());
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\" Stoic \", \"Philosopher\"]\n");
    const auto metadata = load_character_metadata(files.definition());
    EXPECT_EQ(metadata.id, "definition");
    EXPECT_EQ(metadata.display_name, "Example");
    EXPECT_EQ(metadata.tags, (std::vector<std::string>{"Stoic", "Philosopher"}));
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\"Stoic\", \"stoic\"]\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\"\"]\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\ntags = [1]\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\"line\\nbreak\"]\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\"tab\\tcharacter\"]\n");
    EXPECT_THROW((void)load_character_metadata(files.definition()), std::runtime_error);
}

TEST(Config, RequiresDefinitionDisplayName) {
    ConfigFiles files;
    files.write(files.definition(), "provider = \"application\"\n");
    expect_error_containing([&] { (void)load_character_config(files.paths()); }, files.definition(), "display_name");
    expect_error_containing([&] { (void)load_character_metadata(files.definition()); }, files.definition(), "display_name");
}

TEST(Config, PreservesModelBackendDefaultsForOmittedProviderFields) {
    ConfigFiles files;
    files.write(files.definition(), required_definition);
    const ModelBackendConfig config = load_character_config(files.paths()).backend;
    EXPECT_TRUE(config.model.empty());
    EXPECT_TRUE(config.base_path.empty());
    EXPECT_TRUE(config.api_key.empty());
    EXPECT_DOUBLE_EQ(config.temperature, 1.0);
    EXPECT_TRUE(config.stream);
    EXPECT_EQ(config.reasoning_format, ReasoningFormat::automatic);
    EXPECT_EQ(config.api, ProviderApi::responses);
    EXPECT_EQ(config.web_search, WebSearchMode::required);
}

// A present false is a value, not an absent one, so it must survive into the
// backend rather than being skipped as if the provider had said nothing.
TEST(Config, ReadsExplicitFalseBooleansFromAProviderConfig) {
    ConfigFiles files;
    files.write_provider("flags",
        "host = \"example\"\nport = 80\nmode = \"test\"\nstream = false\nhttps = false\n");
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"flags\"\n");
    const ModelBackendConfig config = load_character_config(files.paths()).backend;
    EXPECT_FALSE(config.stream);
    EXPECT_FALSE(config.https);
}

TEST(Config, DerivesIdFromANonAsciiDefinitionDirectory) {
    const auto root = std::filesystem::temp_directory_path() / "cha_config_\xC3\xA9";
    const auto definition = root / "caf\xC3\xA9" / "character.toml";
    std::filesystem::create_directories(definition.parent_path());
    {
        std::ofstream file(definition);
        file << required_definition;
    }
    const LoadedCharacterConfig loaded = load_character_config({
        .providers = {ProviderConfig{.host = "application", .port = 81, .mode = Mode::test}},
        .definition = definition});
    EXPECT_EQ(loaded.character.id, "caf\xC3\xA9");
    EXPECT_EQ(load_character_metadata(definition).id, "caf\xC3\xA9");
    std::filesystem::remove_all(root);
}

TEST(Config, ReadsResponsesWithAutomaticAndRequiredWebSearch) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"search\"\n");
    files.write_provider("search",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"responses\"\nweb_search = \"auto\"\n");
    const ModelBackendConfig automatic = load_character_config(files.paths()).backend;
    EXPECT_EQ(automatic.api, ProviderApi::responses);
    EXPECT_EQ(automatic.web_search, WebSearchMode::automatic);

    files.write_provider("search",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"responses\"\nweb_search = \"required\"\n");
    const ModelBackendConfig required = load_character_config(files.paths()).backend;
    EXPECT_EQ(required.api, ProviderApi::responses);
    EXPECT_EQ(required.web_search, WebSearchMode::required);
}

TEST(Config, AProviderConfigCanDisableWebSearchOnEitherApi) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"quiet\"\n");
    files.write_provider("quiet",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"responses\"\nweb_search = \"off\"\n");
    const ModelBackendConfig responses = load_character_config(files.paths()).backend;
    EXPECT_EQ(responses.api, ProviderApi::responses);
    EXPECT_EQ(responses.web_search, WebSearchMode::off);

    files.write_provider("quiet",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"chat_completions\"\nweb_search = \"off\"\n");
    const ModelBackendConfig chat = load_character_config(files.paths()).backend;
    EXPECT_EQ(chat.api, ProviderApi::chat_completions);
    EXPECT_EQ(chat.web_search, WebSearchMode::off);
}

TEST(Config, ExplicitChatCompletionsMustAlsoDisableMandatorySearch) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"chat\"\n");
    files.write_provider("chat",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"chat_completions\"\n");
    expect_error_containing(
        [&] { (void)load_character_config(files.paths()); },
        files.provider("chat"),
        "web_search");
    files.write_provider("chat",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"chat_completions\"\nweb_search = \"auto\"\n");
    expect_error_containing(
        [&] { (void)load_character_config(files.paths()); },
        files.provider("chat"),
        "web_search");
}

} // namespace
} // namespace cha
