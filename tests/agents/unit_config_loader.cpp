#include "agents/config.h"
#include "util/utf8_path.h"

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
          member_(root_ / "forum" / "members" / "member" / "character.toml") {
        std::filesystem::create_directories(definition_.parent_path());
        std::filesystem::create_directories(defaults_.parent_path());
        std::filesystem::create_directories(member_.parent_path());
        write(application_, "host = \"application\"\nport = 81\nmode = \"test\"\n");
    }
    ~ConfigFiles() { std::filesystem::remove_all(root_); }
    void write(const std::filesystem::path& path, std::string_view contents) const {
        std::ofstream file(path);
        file << contents;
    }
    CharacterConfigPaths paths(bool defaults = false, bool member = false, bool application = false) const {
        return {.application_provider = application ? std::optional{ProviderConfig{
                    .source = application_, .host = "application", .port = 81, .mode = Mode::test}} : std::nullopt,
            .definition = definition_,
            .forum_defaults = defaults ? std::optional{defaults_} : std::nullopt,
            .member_override = member ? std::optional{member_} : std::nullopt};
    }
    const std::filesystem::path& definition() const { return definition_; }
    const std::filesystem::path& defaults() const { return defaults_; }
    const std::filesystem::path& member() const { return member_; }
    const std::filesystem::path& application() const { return application_; }
private:
    std::filesystem::path root_, application_ = root_ / "workspace.toml", definition_, defaults_, member_;
};

constexpr std::string_view required_definition =
    "display_name = \"Example\"\nhost = \"definition.example\"\nport = 8080\n";

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

void expect_definition_values(const LoadedConfig& loaded) {
    const Config& config = loaded.config;
    EXPECT_EQ(config.host, "definition");
    EXPECT_EQ(config.port, 80);
    EXPECT_EQ(config.mode, Mode::test);
    EXPECT_EQ(config.model, "one");
    EXPECT_TRUE(config.stream);
    EXPECT_DOUBLE_EQ(config.temperature, 0.1);
    EXPECT_EQ(config.api_key, "one");
    EXPECT_EQ(config.api_key_env, "ONE");
    EXPECT_EQ(config.reasoning_effort, "low");
    EXPECT_EQ(config.reasoning_format, ReasoningFormat::none);
    EXPECT_FALSE(config.https);
    EXPECT_EQ(loaded.prompt_variables, (TemplateScope{{"value", "definition"}, {"base", "base"}}));
}

constexpr std::string_view complete_definition =
    "display_name = \"Example\"\nhost = \"definition\"\nport = 80\nmode = \"test\"\n"
    "model = \"one\"\nstream = true\ntemperature = 0.1\napi_key = \"one\"\n"
    "api_key_env = \"ONE\"\nreasoning_effort = \"low\"\nreasoning_format = \"none\"\n"
    "https = false\n[prompt]\nvalue = \"definition\"\nbase = \"base\"\n";

TEST(Config, LoadsDefinitionOnlyAndKeepsReservedNameDefault) {
    ConfigFiles files;
    files.write(files.definition(),
        std::string(required_definition) + "description = \"Useful character\"\n");
    const Config effective = load_config(files.paths()).config;
    EXPECT_EQ(effective.id, "definition");
    EXPECT_EQ(effective.display_name, "Example");
    EXPECT_EQ(effective.description, "Useful character");
    EXPECT_EQ(effective.name, Config{}.name);
    EXPECT_EQ(effective.host, "definition.example");
    EXPECT_EQ(effective.port, 8080);
}

TEST(Config, OverlaysEveryRuntimeAndPromptFieldAcrossThreeLayers) {
    ConfigFiles files;
    files.write(files.definition(),
        "display_name = \"Example\"\nhost = \"definition\"\nport = 80\nmode = \"test\"\nmodel = \"one\"\nstream = false\ntemperature = 0.1\napi_key = \"one\"\napi_key_env = \"ONE\"\nreasoning_effort = \"low\"\nreasoning_format = \"none\"\nhttps = false\n[prompt]\nvalue = \"definition\"\nbase = \"base\"\n");
    files.write(files.defaults(),
        "host = \"defaults\"\nport = 443\nmode = \"test\"\nmodel = \"two\"\nstream = false\ntemperature = 0.2\napi_key = \"two\"\napi_key_env = \"TWO\"\nreasoning_effort = \"medium\"\nreasoning_format = \"reasoning\"\nhttps = false\n[prompt]\nvalue = \"defaults\"\ndefault = \"default\"\n");
    files.write(files.member(),
        "host = \"member\"\nport = 8443\nmode = \"net\"\nmodel = \"three\"\nstream = true\ntemperature = 0.3\napi_key = \"three\"\napi_key_env = \"THREE\"\nreasoning_effort = \"high\"\nreasoning_format = \"reasoning_content\"\nhttps = true\n[prompt]\nvalue = \"member\"\nmember = \"member\"\n");
    const LoadedConfig loaded = load_config(files.paths(true, true));
    const Config& effective = loaded.config;
    EXPECT_EQ(effective.id, "definition");
    EXPECT_EQ(effective.display_name, "Example");
    EXPECT_EQ(effective.name, Config{}.name);
    EXPECT_EQ(effective.host, "member");
    EXPECT_EQ(effective.port, 8443);
    EXPECT_EQ(effective.mode, Mode::net);
    EXPECT_EQ(effective.model, "three");
    EXPECT_TRUE(effective.stream);
    EXPECT_DOUBLE_EQ(effective.temperature, 0.3);
    EXPECT_EQ(effective.api_key, "three");
    EXPECT_EQ(effective.api_key_env, "THREE");
    EXPECT_EQ(effective.reasoning_effort, "high");
    EXPECT_EQ(effective.reasoning_format, ReasoningFormat::reasoning_content);
    EXPECT_TRUE(effective.https);
    EXPECT_EQ(loaded.prompt_variables.at("value"), "member");
    EXPECT_EQ(loaded.prompt_variables.at("base"), "base");
    EXPECT_EQ(loaded.prompt_variables.at("default"), "default");
    EXPECT_EQ(loaded.prompt_variables.at("member"), "member");
}

TEST(Config, ForumDefaultsOverlayDefinitionWhenMemberIsAbsent) {
    ConfigFiles files;
    files.write(files.definition(), complete_definition);
    files.write(files.defaults(), "host = \"defaults\"\nmode = \"net\"\nstream = true\nhttps = true\n[prompt]\nvalue = \"defaults\"\n");
    const LoadedConfig loaded = load_config(files.paths(true));
    EXPECT_EQ(loaded.config.host, "defaults");
    EXPECT_EQ(loaded.config.mode, Mode::net);
    EXPECT_TRUE(loaded.config.stream);
    EXPECT_TRUE(loaded.config.https);
    EXPECT_EQ(loaded.prompt_variables.at("value"), "defaults");
    EXPECT_EQ(loaded.config.model, "one");
    EXPECT_EQ(loaded.prompt_variables.at("base"), "base");
}

TEST(Config, EmptyAndCommentOnlyOptionalLayersAreNoopOverlays) {
    ConfigFiles files;
    files.write(files.definition(), complete_definition);
    files.write(files.defaults(), "");
    expect_definition_values(load_config(files.paths(true)));
    files.write(files.defaults(), "# defaults\n");
    expect_definition_values(load_config(files.paths(true)));
    files.write(files.member(), "");
    expect_definition_values(load_config(files.paths(false, true)));
    files.write(files.member(), "# member\n");
    expect_definition_values(load_config(files.paths(false, true)));
}

TEST(Config, RejectsDefinitionOnlyAndRemovedIdentityFieldsInEveryLayer) {
    ConfigFiles files;
    files.write(files.definition(), required_definition);
    for (const std::string_view key : {"name", "id"}) {
        files.write(files.definition(), "display_name = \"Example\"\nhost = \"example\"\nport = 80\n"
            + std::string(key) + " = \"Wrong\"\n");
        expect_error_containing([&] { (void)load_config(files.paths()); }, files.definition(), key);
    }
    files.write(files.definition(), required_definition);
    for (const auto& path : {files.defaults(), files.member()}) {
        files.write(path, "display_name = \"Wrong\"\n");
        expect_error_containing(
            [&] { (void)load_config(files.paths(path == files.defaults(), path == files.member())); },
            path,
            "display_name");
        files.write(path, "tags = [\"Wrong\"]\n");
        expect_error_containing(
            [&] { (void)load_config(files.paths(path == files.defaults(), path == files.member())); },
            path,
            "tags");
        files.write(path, "name = \"Wrong\"\n");
        expect_error_containing(
            [&] { (void)load_config(files.paths(path == files.defaults(), path == files.member())); },
            path,
            "name");
        files.write(path, "id = \"wrong\"\n");
        expect_error_containing(
            [&] { (void)load_config(files.paths(path == files.defaults(), path == files.member())); },
            path,
            "id");
    }
}

TEST(Config, AttributesEffectiveValidationToHighestPrecedenceSource) {
    ConfigFiles files;
    files.write(files.definition(), "display_name = \"Example\"\nhost = \"example\"\nport = 65536\n");
    expect_error_containing([&] { (void)load_config(files.paths(false, false, true)); }, files.definition(), "port");
    files.write(files.definition(), "display_name = \"Example\"\nhost = \"example\"\nport = 80\ntemperature = 2.1\n");
    expect_error_containing([&] { (void)load_config(files.paths(false, false, true)); }, files.definition(), "temperature");
    files.write(files.definition(), required_definition);
    files.write(files.defaults(), "port = 65536\n");
    expect_error_containing([&] { (void)load_config(files.paths(true)); }, files.defaults(), "port");
    files.write(files.defaults(), "temperature = 2.1\n");
    expect_error_containing([&] { (void)load_config(files.paths(true)); }, files.defaults(), "temperature");
    files.write(files.defaults(), "");
    files.write(files.member(), "port = 65536\n");
    expect_error_containing([&] { (void)load_config(files.paths(true, true)); }, files.member(), "port");
    files.write(files.member(), "temperature = 2.1\n");
    expect_error_containing([&] { (void)load_config(files.paths(true, true)); }, files.member(), "temperature");
}

TEST(Config, InheritsApplicationProviderAndTracksAllFourLayers) {
    ConfigFiles files;
    files.write(files.definition(), "display_name = \"Example\"\n");
    EXPECT_EQ(load_config(files.paths(false, false, true)).config.host, "application");
    EXPECT_EQ(load_config(files.paths(false, false, true)).config.port, 81);
    files.write(files.definition(), "display_name = \"Example\"\nhost = \"definition\"\n");
    files.write(files.defaults(), "host = \"defaults\"\n");
    files.write(files.member(), "host = \"member\"\n");
    EXPECT_EQ(load_config(files.paths(true, true, true)).config.host, "member");
}

TEST(Config, ValidatesCharacterDescriptionsAndRejectsThemOutsideDefinitions) {
    ConfigFiles files;
    files.write(files.definition(), "display_name = \"Example\"\ndescription = \"Useful character\"\n");
    EXPECT_EQ(load_character_definition_metadata(files.definition()).description, "Useful character");
    files.write(files.definition(), "display_name = \"Example\"\ndescription = \" bad\"\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\n");
    files.write(files.defaults(), "description = \"Not here\"\n");
    EXPECT_THROW((void)load_config(files.paths(true)), std::runtime_error);
}

TEST(Config, DefaultsCharacterAppearanceToTheInterfaceSettings) {
    ConfigFiles files;
    files.write(files.definition(), "display_name = \"Example\"\n");
    const CharacterAppearance appearance =
        load_character_definition_metadata(files.definition()).appearance;
    EXPECT_EQ(appearance, CharacterAppearance{});
    EXPECT_EQ(appearance.font, CharacterFont::sans);
    EXPECT_EQ(appearance.size, CharacterScale::normal);
    EXPECT_EQ(load_config(files.paths(false, false, true)).config.appearance, CharacterAppearance{});
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
    EXPECT_EQ(load_character_definition_metadata(files.definition()).appearance, expected);
    EXPECT_EQ(load_config(files.paths(false, false, true)).config.appearance, expected);
}

TEST(Config, RejectsUnknownAppearanceFieldsValuesAndPlacements) {
    ConfigFiles files;
    const std::string named = "display_name = \"Example\"\n";
    files.write(files.definition(), named + "[appearance]\nfont = \"comic\"\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), named + "[appearance]\ncolour = \"red\"\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), named + "[appearance]\nfont = 3\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), named + "appearance = \"serif\"\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    // Appearance belongs to the character, not to one forum's use of it.
    files.write(files.definition(), named);
    files.write(files.defaults(), "[appearance]\nfont = \"serif\"\n");
    EXPECT_THROW((void)load_config(files.paths(true)), std::runtime_error);
}

TEST(Config, LoadsAndValidatesDefinitionMetadataTags) {
    ConfigFiles files;
    files.write(files.definition(), "display_name = \"Example\"\n");
    EXPECT_TRUE(load_character_definition_metadata(files.definition()).tags.empty());
    files.write(files.definition(), "display_name = \"Example\"\ntags = []\n");
    EXPECT_TRUE(load_character_definition_metadata(files.definition()).tags.empty());
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\" Stoic \", \"Philosopher\"]\n");
    const auto metadata = load_character_definition_metadata(files.definition());
    EXPECT_EQ(metadata.id, "definition");
    EXPECT_EQ(metadata.display_name, "Example");
    EXPECT_EQ(metadata.tags, (std::vector<std::string>{"Stoic", "Philosopher"}));
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\"Stoic\", \"stoic\"]\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\"\"]\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\ntags = [1]\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\"line\\nbreak\"]\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
    files.write(files.definition(), "display_name = \"Example\"\ntags = [\"tab\\tcharacter\"]\n");
    EXPECT_THROW((void)load_character_definition_metadata(files.definition()), std::runtime_error);
}

TEST(Config, RequiresDefinitionDisplayName) {
    ConfigFiles files;
    files.write(files.definition(), "host = \"example\"\nport = 80\n");
    expect_error_containing([&] { (void)load_config(files.paths()); }, files.definition(), "display_name");
    expect_error_containing([&] { (void)load_character_definition_metadata(files.definition()); }, files.definition(), "display_name");
}

TEST(Config, PreservesExistingDefaultsAndValidation) {
    ConfigFiles files;
    files.write(files.definition(), required_definition);
    const Config config = load_config(files.paths()).config;
    EXPECT_TRUE(config.model.empty());
    EXPECT_DOUBLE_EQ(config.temperature, 1.0);
    EXPECT_EQ(config.reasoning_format, ReasoningFormat::automatic);

    files.write(files.definition(), "display_name = \"Example\"\nhost = \"example\"\nport = 80\nmode = \"invalid\"\n");
    expect_error_containing([&] { (void)load_config(files.paths()); }, files.definition(), "mode");
    files.write(files.definition(), "display_name = \"Example\"\nhost = \"example\"\nport = 80\nreasoning_format = \"invalid\"\n");
    expect_error_containing([&] { (void)load_config(files.paths()); }, files.definition(), "reasoning_format");
}

TEST(Config, DerivesIdFromANonAsciiDefinitionDirectory) {
    const auto root = std::filesystem::temp_directory_path() / "cha_config_\xC3\xA9";
    const auto definition = root / "caf\xC3\xA9" / "character.toml";
    std::filesystem::create_directories(definition.parent_path());
    {
        std::ofstream file(definition);
        file << required_definition;
    }
    const LoadedConfig loaded = load_config({.definition = definition});
    EXPECT_EQ(loaded.config.id, "caf\xC3\xA9");
    EXPECT_EQ(load_character_definition_metadata(definition).id, "caf\xC3\xA9");
    std::filesystem::remove_all(root);
}

} // namespace
} // namespace cha
