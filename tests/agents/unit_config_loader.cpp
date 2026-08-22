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
          providers_(root_ / "system" / "providers"),
          styles_(root_ / "system" / "styles") {
        std::filesystem::create_directories(definition_.parent_path());
        std::filesystem::create_directories(defaults_.parent_path());
        std::filesystem::create_directories(member_.parent_path());
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
    void write_style(std::string_view name, std::string_view contents) const {
        const std::filesystem::path path = styles_ / std::string(name) / "config.toml";
        std::filesystem::create_directories(path.parent_path());
        write(path, contents);
    }
    CharacterConfigPaths paths(bool defaults = false, bool member = false) const {
        return {.providers_directory = providers_,
            .styles_directory = styles_,
            .definition = definition_,
            .forum_defaults = defaults ? std::optional{defaults_} : std::nullopt,
            .member_override = member ? std::optional{member_} : std::nullopt};
    }
    const std::filesystem::path& definition() const { return definition_; }
    const std::filesystem::path& defaults() const { return defaults_; }
    const std::filesystem::path& member() const { return member_; }
    const std::filesystem::path& providers() const { return providers_; }
    const std::filesystem::path& styles() const { return styles_; }
    std::filesystem::path provider(std::string_view name) const {
        return providers_ / std::string(name) / "config.toml";
    }
private:
    std::filesystem::path root_, definition_, defaults_, member_, providers_, styles_;
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
    "temperature = 0.1\nmax_tokens = 512\ntimeout_s = 90\nidle_timeout_s = 15\n"
    "api_key_env = \"ONE\"\nreasoning_effort = \"low\"\n"
    "reasoning_format = \"none\"\nhttps = false\nbase_path = \"/definition\"\n";

void expect_complete_provider_values(const ModelBackendConfig& config) {
    EXPECT_EQ(config.host, "definition");
    EXPECT_EQ(config.port, 80);
    EXPECT_EQ(config.base_path, "/definition");
    EXPECT_EQ(config.mode, Mode::test);
    EXPECT_EQ(config.model, "one");
    EXPECT_TRUE(config.stream);
    ASSERT_TRUE(config.temperature);
    EXPECT_DOUBLE_EQ(*config.temperature, 0.1);
    EXPECT_EQ(config.max_tokens, 512);
    EXPECT_EQ(config.timeout_s, 90);
    EXPECT_EQ(config.idle_timeout_s, 15);
    EXPECT_EQ(config.api_key_env, "ONE");
    EXPECT_EQ(config.reasoning_effort, "low");
    EXPECT_EQ(config.reasoning_format, ReasoningFormat::none);
    EXPECT_FALSE(config.https);
    EXPECT_EQ(config.cache_retention, CacheRetention::short_);
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
    const LoadedCharacterConfig loaded = load_character_config(files.paths());
    EXPECT_EQ(loaded.provider.id, "complete");
    expect_complete_provider_values(loaded.provider.config);
}

TEST(Config, ReadsAndValidatesCacheRetention) {
    ConfigFiles files;
    files.write_provider(
        "cached",
        "host = \"example\"\nport = 443\ncache_retention = \"long\"\n");
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"cached\"\n");
    EXPECT_EQ(
        load_character_config(files.paths()).provider.config.cache_retention,
        CacheRetention::long_);

    files.write_provider(
        "cached",
        "host = \"example\"\nport = 443\ncache_retention = \"off\"\n");
    EXPECT_EQ(
        load_character_config(files.paths()).provider.config.cache_retention,
        CacheRetention::off);

    files.write_provider(
        "cached",
        "host = \"example\"\nport = 443\ncache_retention = \"forever\"\n");
    expect_error_containing(
        [&] { (void)load_character_config(files.paths()); },
        files.providers() / "cached" / "config.toml",
        "cache_retention");
}

TEST(Config, IgnoresProviderSelectionsOutsideTheCharacterDefinition) {
    ConfigFiles files;
    files.write_provider("definition",
        "host = \"definition.example\"\nport = 443\nmode = \"test\"\nmodel = \"one\"\n"
        "temperature = 0.5\nreasoning_effort = \"low\"\nbase_path = \"/definition\"\n");
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"definition\"\n");
    // These names need not even resolve: provider keys in optional prompt
    // layers are inert legacy data.
    files.write(files.defaults(), "provider = \"missing-forum\"\n");
    files.write(files.member(), "provider = 42\n");

    const ModelBackendConfig config =
        load_character_config(files.paths(true, true)).provider.config;
    EXPECT_EQ(config.host, "definition.example");
    EXPECT_EQ(config.port, 443);
    EXPECT_EQ(config.mode, Mode::test);
    EXPECT_EQ(config.model, "one");
    EXPECT_EQ(config.temperature, 0.5);
    EXPECT_EQ(config.reasoning_effort, "low");
    EXPECT_EQ(config.base_path, "/definition");
}

TEST(Config, EmptyAndCommentOnlyOptionalLayersKeepTheProviderBelow) {
    ConfigFiles files;
    files.write_provider("complete", complete_provider);
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"complete\"\n");
    for (const std::string_view contents : {"", "# nothing here\n"}) {
        files.write(files.defaults(), contents);
        expect_complete_provider_values(load_character_config(files.paths(true)).provider.config);
        files.write(files.member(), contents);
        expect_complete_provider_values(
            load_character_config(files.paths(true, true)).provider.config);
    }
}

TEST(Config, MergesPromptScopeWithoutOverridingTheProvider) {
    ConfigFiles files;
    files.write_provider("definition", "host = \"definition\"\nport = 80\nmode = \"test\"\n");
    files.write(files.definition(),
        std::string(required_definition)
            + "provider = \"definition\"\n[prompt]\nvalue = \"definition\"\nbase = \"base\"\n");
    files.write(files.defaults(), "[prompt]\nvalue = \"defaults\"\ndefault = \"default\"\n");
    files.write(files.member(),
        "provider = \"member\"\n[prompt]\nvalue = \"member\"\nmember = \"member\"\n");

    const LoadedCharacterConfig loaded = load_character_config(files.paths(true, true));
    EXPECT_EQ(loaded.provider.config.host, "definition");
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
    EXPECT_EQ(loaded.provider.id, "named");
    EXPECT_EQ(loaded.provider.config.host, "named.example");
    EXPECT_EQ(loaded.provider.config.port, 8080);
}

// Connection settings belong in named provider configs. Character definitions
// may select one by name, while prompt layers cannot inline connection fields.
TEST(Config, RejectsInlineProviderSettingsInCharacterAndPromptLayers) {
    ConfigFiles files;
    for (const std::string_view setting :
            {"host = \"example\"\n", "port = 80\n", "model = \"one\"\n",
             "api = \"responses\"\n", "web_search = \"off\"\n", "cache_retention = \"off\"\n", "api_key = \"secret\"\n",
             "base_path = \"/api\"\n", "temperature = 0.5\n", "max_tokens = 10\n",
             "timeout_s = 10\n", "idle_timeout_s = 10\n"}) {
        files.write(files.definition(), std::string(required_definition) + std::string(setting));
        expect_error_containing(
            [&] { (void)load_character_config(files.paths()); },
            files.definition(),
            "provider setting");
    }
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"application\"\n");
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

TEST(Config, RequiresAProviderInTheCharacterDefinition) {
    ConfigFiles files;
    files.write(files.definition(), required_definition);
    expect_error_containing(
        [&] { (void)load_character_config(files.paths()); },
        files.definition(),
        "requires a 'provider' name");
}

TEST(Config, RejectsDefinitionOnlyAndRemovedIdentityFieldsInEveryLayer) {
    ConfigFiles files;
    for (const std::string_view key : {"name", "id"}) {
        files.write(files.definition(),
            std::string(required_definition) + std::string(key) + " = \"Wrong\"\n");
        expect_error_containing([&] { (void)load_character_config(files.paths()); }, files.definition(), key);
    }
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"application\"\n");
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
    expect_provider_error("host = \"example\"\nport = 80\nmax_tokens = 0\n", "max_tokens");
    expect_provider_error("host = \"example\"\nport = 80\ntimeout_s = 0\n", "timeout_s");
    expect_provider_error("host = \"example\"\nport = 80\nidle_timeout_s = -1\n", "idle_timeout_s");
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

TEST(Config, IgnoresAForumProviderWithoutLoadingIt) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"application\"\n");
    files.write(files.defaults(), "provider = \"missing\"\n");
    EXPECT_EQ(load_character_config(files.paths(true)).provider.config.host, "application");
}

TEST(Config, DefaultsCharacterAppearanceToTheInterfaceSettings) {
    ConfigFiles files;
    files.write(files.definition(),
        "display_name = \"Example\"\nprovider = \"application\"\n");
    const CharacterAppearance appearance =
        load_character_metadata(files.definition()).appearance;
    EXPECT_EQ(appearance, CharacterAppearance{});
    EXPECT_EQ(appearance.font, CharacterFont::sans);
    EXPECT_EQ(appearance.size, CharacterScale::normal);
    EXPECT_EQ(load_character_config(files.paths()).character.appearance, CharacterAppearance{});
}

TEST(Config, ReadsEveryCharacterAppearanceFieldAndCarriesItIntoTheEffectiveConfig) {
    ConfigFiles files;
    files.write_style("serif-italic-semibold-large-accent",
        "font = \"serif\"\n"
        "style = \"italic\"\n"
        "weight = \"semibold\"\n"
        "size = \"large\"\n"
        "text_color = \"accent\"\n");
    files.write(files.definition(),
        "display_name = \"Seneca\"\n"
        "provider = \"application\"\n"
        "style = \"serif-italic-semibold-large-accent\"\n");
    const CharacterAppearance expected{CharacterFont::serif, CharacterSlant::italic,
        CharacterWeight::semibold, CharacterScale::large, CharacterTextColor::accent};
    EXPECT_EQ(load_character_metadata(
        files.definition(), files.providers(), files.styles()).appearance, expected);
    EXPECT_EQ(load_character_config(files.paths()).character.appearance, expected);
}

TEST(Config, RejectsInvalidStyleConfigsAndLegacyAppearance) {
    ConfigFiles files;
    const std::string named = "display_name = \"Example\"\n";
    files.write_style("invalid-font", "font = \"comic\"\n");
    files.write(files.definition(), named + "style = \"invalid-font\"\n");
    EXPECT_THROW((void)load_character_config(files.paths()), std::runtime_error);
    files.write_style("invalid-field", "colour = \"red\"\n");
    files.write(files.definition(), named + "style = \"invalid-field\"\n");
    EXPECT_THROW((void)load_character_config(files.paths()), std::runtime_error);
    files.write_style("invalid-type", "font = 3\n");
    files.write(files.definition(), named + "style = \"invalid-type\"\n");
    EXPECT_THROW((void)load_character_config(files.paths()), std::runtime_error);
    files.write_style("invalid-text-color", "text_color = \"violet\"\n");
    files.write(files.definition(), named + "style = \"invalid-text-color\"\n");
    EXPECT_THROW((void)load_character_config(files.paths()), std::runtime_error);
    files.write(files.definition(), named + "[appearance]\nfont = \"serif\"\n");
    EXPECT_THROW((void)load_character_config(files.paths()), std::runtime_error);
    files.write(files.definition(), named);
    files.write(files.defaults(), "style = \"serif\"\n");
    EXPECT_THROW((void)load_character_config(files.paths(true)), std::runtime_error);
}

TEST(Config, RejectsAStyleReferenceWithoutAConfigFile) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "style = \"missing\"\n");
    expect_error_containing(
        [&] { (void)load_character_config(files.paths()); },
        files.definition(),
        "missing");
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
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"application\"\n");
    const ModelBackendConfig config = load_character_config(files.paths()).provider.config;
    EXPECT_TRUE(config.model.empty());
    EXPECT_TRUE(config.base_path.empty());
    EXPECT_TRUE(config.api_key.empty());
    EXPECT_FALSE(config.temperature);
    EXPECT_FALSE(config.max_tokens);
    EXPECT_EQ(config.timeout_s, 600);
    EXPECT_EQ(config.idle_timeout_s, 60);
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
    const ModelBackendConfig config = load_character_config(files.paths()).provider.config;
    EXPECT_FALSE(config.stream);
    EXPECT_FALSE(config.https);
}

TEST(Config, DerivesIdFromANonAsciiDefinitionDirectory) {
    const auto root = std::filesystem::temp_directory_path() / "cha_config_\xC3\xA9";
    const auto definition = root / "caf\xC3\xA9" / "character.toml";
    const auto providers = root / "system" / "providers";
    std::filesystem::create_directories(definition.parent_path());
    std::filesystem::create_directories(providers / "test");
    {
        std::ofstream file(definition);
        file << required_definition << "provider = \"test\"\n";
    }
    std::ofstream(providers / "test" / "config.toml")
        << "host = \"test\"\nport = 1\nmode = \"test\"\n";
    const LoadedCharacterConfig loaded = load_character_config({
        .providers_directory = providers,
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
    const ModelBackendConfig automatic = load_character_config(files.paths()).provider.config;
    EXPECT_EQ(automatic.api, ProviderApi::responses);
    EXPECT_EQ(automatic.web_search, WebSearchMode::automatic);

    files.write_provider("search",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"responses\"\nweb_search = \"required\"\n");
    const ModelBackendConfig required = load_character_config(files.paths()).provider.config;
    EXPECT_EQ(required.api, ProviderApi::responses);
    EXPECT_EQ(required.web_search, WebSearchMode::required);
}

TEST(Config, AProviderConfigCanDisableWebSearchOnEitherApi) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "provider = \"quiet\"\n");
    files.write_provider("quiet",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"responses\"\nweb_search = \"off\"\n");
    const ModelBackendConfig responses = load_character_config(files.paths()).provider.config;
    EXPECT_EQ(responses.api, ProviderApi::responses);
    EXPECT_EQ(responses.web_search, WebSearchMode::off);

    files.write_provider("quiet",
        "host = \"example\"\nport = 80\nmode = \"test\"\napi = \"chat_completions\"\nweb_search = \"off\"\n");
    const ModelBackendConfig chat = load_character_config(files.paths()).provider.config;
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
