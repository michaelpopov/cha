#include "workspace/workspace_config_editor.h"
#include "workspace/workspace_internal.h"
#include "providers/voice_output_config.h"
#include "util/base64.h"
#include "util/path_name.h"
#include "util/picture.h"
#include "util/public_name.h"
#include "util/text.h"

#include <algorithm>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace cha {

std::string_view embedded_character_voice();
std::string_view embedded_new_character_template();

using namespace workspace_detail;

namespace {

toml::table api_key_table(
    std::string_view display_name,
    std::string_view value) {
    toml::table table;
    table.insert("display_name", std::string(display_name));
    table.insert("type", "models");
    table.insert("value", std::string(value));
    return table;
}

toml::table r2_storage_table(const R2StorageKey& key) {
    toml::table table;
    table.insert("display_name", key.display_name);
    table.insert("type", "R2");
    table.insert("url", key.url);
    table.insert("access_key_id", key.access_key_id);
    table.insert("secret_key", key.secret_key);
    return table;
}

toml::table key_collection_table(std::uint64_t next_id) {
    if (next_id > static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("API key ID space is exhausted");
    }
    toml::table table;
    table.insert("next_id", static_cast<std::int64_t>(next_id));
    return table;
}

} // namespace

void WorkspaceConfigEditor::write_file(
    const std::filesystem::path& path, std::string_view content) {
    files_[source_.name(path)] = content;
}

void WorkspaceConfigEditor::write_toml(
    const std::filesystem::path& path, const toml::table& table) {
    std::ostringstream output;
    output << table << '\n';
    write_file(path, output.str());
}

void WorkspaceConfigEditor::rewrite_toml(
    const std::filesystem::path& path, const std::function<void(toml::table&)>& edit) {
    toml::table table = read_toml(source_, path, "config file");
    edit(table);
    write_toml(path, table);
}

void WorkspaceConfigEditor::remove_directory(const std::filesystem::path& path) {
    const std::string prefix = source_.name(path) + "/";
    std::erase_if(files_, [&](const auto& file) { return file.first.starts_with(prefix); });
}

void WorkspaceConfigEditor::write_markdown_file(
    const std::filesystem::path& config_path,
    const std::map<std::string, std::string, std::less<>>& markdown_files,
    std::string_view filename,
    std::optional<std::string_view> content,
    bool create,
    std::string_view required_filename,
    std::string_view subject,
    bool allow_text) {
    try {
        require_path_component(filename, config_path);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid Markdown filename");
    }
    const auto name = path_from_utf8(filename);
    if (name.extension() != ".md"
        && !(allow_text && name.extension() == ".txt")) {
        throw std::invalid_argument("Invalid Markdown filename");
    }
    if (!create && !markdown_files.contains(filename)) {
        throw std::out_of_range("Unknown " + std::string(subject) + " file");
    }
    const auto path = config_path.parent_path() / name;
    const bool exists = source_.is_regular_file(path);
    if (create && source_.exists(path)) {
        throw std::invalid_argument("File already exists");
    }
    if (!create && !exists) throw std::out_of_range("Unknown " + std::string(subject) + " file");
    if (content) {
        write_file(path, *content);
    } else {
        if (filename == required_filename) {
            throw std::invalid_argument(std::string(required_filename) + " is required");
        }
        files_.erase(source_.name(path));
    }
}

void WorkspaceConfigEditor::write_provider(
    std::string_view provider_id,
    std::string_view display_name,
    const ModelBackendConfig& provider) {
    const auto path = workspace_.provider_config_paths_.find(std::string(provider_id));
    if (path == workspace_.provider_config_paths_.end()) {
        throw std::runtime_error(
            "Provider '" + std::string(provider_id)
            + "' has no writable configuration");
    }
    try {
        validate_public_name(display_name, "Provider name", path->second);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid provider name");
    }
    if (provider_config_error(provider)) {
        throw std::invalid_argument("Invalid provider settings");
    }
    toml::table table;
    table.insert("display_name", std::string(display_name));
    table.insert("host", provider.host);
    table.insert("port", provider.port);
    if (!provider.base_path.empty()) table.insert("base_path", provider.base_path);
    table.insert("mode", to_string(provider.mode));
    table.insert("model", provider.model);
    table.insert("stream", provider.stream);
    if (provider.temperature) table.insert("temperature", *provider.temperature);
    if (provider.max_tokens) table.insert("max_tokens", *provider.max_tokens);
    table.insert("timeout_s", provider.timeout_s);
    table.insert("idle_timeout_s", provider.idle_timeout_s);
    if (!provider.api_key_id.empty()) table.insert("api_key", provider.api_key_id);
    if (!provider.api_key_env.empty()) {
        table.insert("api_key_env", provider.api_key_env);
    }
    table.insert("reasoning_effort", provider.reasoning_effort);
    table.insert("reasoning_format", to_string(provider.reasoning_format));
    table.insert("https", provider.https);
    table.insert("api", to_string(provider.api));
    table.insert("auth", to_string(provider.auth));
    table.insert("web_search", to_string(provider.web_search));
    table.insert("cache_retention", to_string(provider.cache_retention));
    if (!provider.openrouter_targets.empty()) {
        toml::array targets;
        for (const std::string& target : provider.openrouter_targets) {
            targets.push_back(target);
        }
        table.insert("openrouter_targets", std::move(targets));
    }
    write_toml(path->second, table);
    // Validate the saved file directly; workspace loading can omit invalid providers.
    try {
        (void)load_provider(source_, path->second.parent_path(), {workspace_.root_});
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid provider settings");
    }
}

void WorkspaceConfigEditor::create_provider(
    std::string_view provider_id,
    std::string_view display_name,
    std::string_view copy_from) {
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "providers" / std::string(provider_id);
    const std::filesystem::path path = directory / "config.toml";
    try {
        require_path_component(provider_id, directory.parent_path());
        validate_public_name(display_name, "Provider name", path);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid provider");
    }
    if (workspace_.find_provider(provider_id) != nullptr
        || source_.exists(directory)) {
        throw std::invalid_argument("Duplicate provider");
    }

    toml::table table;
    if (copy_from.empty()) {
        table.insert("display_name", std::string(display_name));
        table.insert("host", "api.openai.com");
        table.insert("port", 443);
        table.insert("mode", "net");
        table.insert("model", "gpt-5");
        table.insert("stream", true);
        table.insert("timeout_s", 600);
        table.insert("idle_timeout_s", 60);
        table.insert("reasoning_format", "auto");
        table.insert("https", true);
        table.insert("api", "responses");
        table.insert("auth", "none");
        table.insert("web_search", "off");
        table.insert("cache_retention", "short");
    } else {
        const auto source = workspace_.provider_config_paths_.find(std::string(copy_from));
        if (source == workspace_.provider_config_paths_.end()) {
            throw std::invalid_argument("Unknown provider to copy");
        }
        table = read_toml(source_, source->second, "provider config");
        table.insert_or_assign("display_name", std::string(display_name));
    }

    write_toml(path, table);
    (void)load_provider(source_, directory, {workspace_.root_});
}

void WorkspaceConfigEditor::delete_provider(std::string_view provider_id) {
    const auto path = workspace_.provider_config_paths_.find(std::string(provider_id));
    if (path == workspace_.provider_config_paths_.end()) {
        throw std::runtime_error(
            "Provider '" + std::string(provider_id)
            + "' has no writable configuration");
    }
    for (const WorkspaceCharacter& character : workspace_.characters_) {
        if (character.provider_id && *character.provider_id == provider_id) {
            throw std::invalid_argument("Provider is in use");
        }
    }
    if (workspace_.session_naming_.provider_id == provider_id
        || workspace_.notes_.compression_provider_id == provider_id) {
        throw std::invalid_argument("Provider is in use");
    }
    remove_directory(path->second.parent_path());
}

void WorkspaceConfigEditor::write_style(
    std::string_view style_id,
    std::string_view display_name,
    const CharacterAppearance& appearance) {
    const auto path = workspace_.style_config_paths_.find(std::string(style_id));
    if (path == workspace_.style_config_paths_.end()) {
        throw std::runtime_error(
            "Style '" + std::string(style_id)
            + "' has no writable configuration");
    }
    try {
        validate_public_name(display_name, "Style name", path->second);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid style name");
    }
    toml::table table;
    table.insert("display_name", std::string(display_name));
    table.insert("font", to_string(appearance.font));
    table.insert("style", to_string(appearance.style));
    table.insert("weight", to_string(appearance.weight));
    table.insert("size", to_string(appearance.size));
    table.insert("text_color", to_string(appearance.text_color));
    write_toml(path->second, table);
    try {
        (void)load_style(source_, path->second.parent_path(), {workspace_.root_});
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid style settings");
    }
}

void WorkspaceConfigEditor::create_style(
    std::string_view style_id,
    std::string_view display_name) {
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "styles" / std::string(style_id);
    const std::filesystem::path path = directory / "config.toml";
    try {
        require_path_component(style_id, directory.parent_path());
        validate_public_name(display_name, "Style name", path);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid style");
    }
    if (workspace_.find_style(style_id) != nullptr || source_.exists(directory)) {
        throw std::invalid_argument("Duplicate style");
    }

    toml::table table;
    table.insert("display_name", std::string(display_name));
    table.insert("font", "sans");
    table.insert("style", "normal");
    table.insert("weight", "normal");
    table.insert("size", "normal");
    table.insert("text_color", "normal");
    write_toml(path, table);
    (void)load_style(source_, directory, {workspace_.root_});
}

void WorkspaceConfigEditor::delete_style(std::string_view style_id) {
    const auto path = workspace_.style_config_paths_.find(std::string(style_id));
    if (path == workspace_.style_config_paths_.end()) {
        throw std::runtime_error(
            "Style '" + std::string(style_id)
            + "' has no writable configuration");
    }
    for (const WorkspaceCharacter& character : workspace_.characters_) {
        if (character.style_id && *character.style_id == style_id) {
            throw std::invalid_argument("Style is in use");
        }
    }
    for (const WorkspacePersona& persona : workspace_.personas_) {
        if (persona.style_id && *persona.style_id == style_id) {
            throw std::invalid_argument("Style is in use");
        }
    }
    remove_directory(path->second.parent_path());
}

void WorkspaceConfigEditor::write_voice(
    std::string_view voice_id,
    std::string_view display_name,
    std::string_view description,
    std::string_view elevenlabs_voice_id,
    const VoiceSettings& settings,
    std::string_view provider) {
    const auto path = workspace_.voice_config_paths_.find(std::string(voice_id));
    if (path == workspace_.voice_config_paths_.end()) {
        throw std::runtime_error(
            "Voice '" + std::string(voice_id)
            + "' has no writable configuration");
    }
    try {
        validate_public_name(display_name, "Voice name", path->second);
        if (!description.empty()) {
            validate_description(description, "Voice", path->second);
        }
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid voice details");
    }
    toml::table table;
    table.insert("display_name", std::string(display_name));
    if (!description.empty()) {
        table.insert("description", std::string(description));
    }
    validate_voice_output_provider(provider);
    table.insert("provider", std::string(provider));
    table.insert("elevenlabs_voice_id", std::string(elevenlabs_voice_id));
    if (settings.speed) table.insert("speed", *settings.speed);
    write_toml(path->second, table);
    try {
        (void)load_voice(source_, path->second.parent_path(), {workspace_.root_});
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid voice settings");
    }
}

void WorkspaceConfigEditor::create_voice(
    std::string_view voice_id,
    std::string_view display_name,
    std::string_view description,
    std::string_view elevenlabs_voice_id,
    std::string_view provider) {
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "voices" / std::string(voice_id);
    const std::filesystem::path path = directory / "config.toml";
    try {
        require_path_component(voice_id, directory.parent_path());
        validate_public_name(display_name, "Voice name", path);
        if (!description.empty()) validate_description(description, "Voice", path);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid voice");
    }
    if (elevenlabs_voice_id.empty() || workspace_.find_voice(voice_id) != nullptr
        || source_.exists(directory)) {
        throw std::invalid_argument("Invalid voice");
    }

    toml::table table;
    table.insert("display_name", std::string(display_name));
    if (!description.empty()) {
        table.insert("description", std::string(description));
    }
    validate_voice_output_provider(provider);
    table.insert("provider", std::string(provider));
    table.insert("elevenlabs_voice_id", std::string(elevenlabs_voice_id));
    write_toml(path, table);
    (void)load_voice(source_, directory, {workspace_.root_});
}

void WorkspaceConfigEditor::delete_voice(std::string_view voice_id) {
    const auto path = workspace_.voice_config_paths_.find(std::string(voice_id));
    if (path == workspace_.voice_config_paths_.end()) {
        throw std::runtime_error(
            "Voice '" + std::string(voice_id)
            + "' has no writable configuration");
    }
    for (const WorkspaceCharacter& character : workspace_.characters_) {
        if (character.voice_id && *character.voice_id == voice_id) {
            throw std::invalid_argument("Voice is in use");
        }
    }
    for (const WorkspacePersona& persona : workspace_.personas_) {
        if (persona.voice_id && *persona.voice_id == voice_id) {
            throw std::invalid_argument("Voice is in use");
        }
    }
    const WorkspaceVoice* const voice = workspace_.find_voice(voice_id);
    if (workspace_.voice_output_ && voice
        && workspace_.voice_output_->default_voice == voice->label) {
        throw std::invalid_argument("Voice is in use");
    }
    remove_directory(path->second.parent_path());
}

void WorkspaceConfigEditor::write_jev(const std::optional<WorkspaceJev>& settings) {
    const auto directory = workspace_.root_ / "system" / "jev";
    if (!settings) {
        files_.erase(source_.name(directory / "config.toml"));
        return;
    }
    validate_jev_config(*settings);
    if (!workspace_.find_api_key(settings->api_key_id)) {
        throw std::invalid_argument("Select an existing API key for recipient detection.");
    }
    toml::table table;
    table.insert("url", settings->url);
    table.insert("model", settings->model);
    table.insert("api_key", settings->api_key_id);
    write_toml(directory / "config.toml", table);
}

void WorkspaceConfigEditor::write_session_naming(const WorkspaceSessionNaming& settings) {
    if (!workspace_.find_provider(settings.provider_id)) {
        throw std::invalid_argument("Select an existing provider for session naming.");
    }
    if (!valid_reasoning_effort(settings.reasoning_effort)) {
        throw std::invalid_argument("Select a valid effort for session naming.");
    }
    toml::table table;
    table.insert("naming_provider", settings.provider_id);
    table.insert("naming_reasoning_effort", settings.reasoning_effort);
    write_toml(workspace_.root_ / "system" / "session" / "config.toml", table);
}

void WorkspaceConfigEditor::write_notes(const WorkspaceNotes& settings) {
    if (!settings.compression_provider_id.empty()
        && !workspace_.find_provider(settings.compression_provider_id)) {
        throw std::invalid_argument("Select an existing provider for notes compression.");
    }
    if (!valid_reasoning_effort(settings.compression_reasoning_effort)) {
        throw std::invalid_argument("Select a valid effort for notes compression.");
    }
    toml::table table;
    table.insert("compression_provider", settings.compression_provider_id);
    table.insert("compression_reasoning_effort", settings.compression_reasoning_effort);
    table.insert("audio_url", settings.audio_url);
    table.insert("audio_api_key", settings.audio_api_key_id);
    write_toml(workspace_.root_ / "system" / "notes" / "config.toml", table);
}

void WorkspaceConfigEditor::write_web_search(const WorkspaceWebSearch& settings) {
    std::string provider = settings.provider;
    if (provider != "brave" && provider != "tavily") {
        if (settings.tool_enabled) {
            throw std::invalid_argument("Select a web search provider.");
        }
        log_warn("Ignoring unsupported web search provider; using Brave Search API");
        provider = "brave";
    }
    if (settings.tool_enabled) {
        if (!workspace_.find_api_key(settings.api_key_id)) {
            throw std::invalid_argument("Select an existing API key for web search.");
        }
    }
    toml::table table;
    table.insert("tool_enabled", settings.tool_enabled);
    table.insert("provider", provider);
    table.insert("api_key", settings.api_key_id);
    std::string reader = settings.read_provider;
    if (reader != "off" && reader != "firecrawl") {
        log_warn("Ignoring unsupported page reading provider; disabling page reading");
        reader = "off";
    }
    if (reader == "firecrawl" && !workspace_.find_api_key(settings.firecrawl_api_key_id)) {
        throw std::invalid_argument("Select an existing API key for page reading.");
    }
    table.insert("read_provider", reader);
    table.insert("firecrawl_api_key", settings.firecrawl_api_key_id);
    std::string web_reader_url(trim_view(settings.web_reader_url));
    if (!valid_web_reader_url(web_reader_url) || (settings.web_reader_enabled && web_reader_url.empty())) {
        if (settings.web_reader_enabled) {
            throw std::invalid_argument(
                "web_reader requires an absolute HTTP or HTTPS base URL without credentials, query, or fragment.");
        }
        log_warn("Ignoring unused invalid web_reader URL");
        web_reader_url.clear();
    }
    table.insert("web_reader_url", web_reader_url);
    table.insert("web_reader_enabled", settings.web_reader_enabled);
    table.insert("web_reader_api_key", settings.web_reader_api_key_id);
    write_toml(workspace_.root_ / "system" / "web-search" / "config.toml", table);
}

void WorkspaceConfigEditor::write_voice_input(const WorkspaceVoiceInput& settings) {
    const std::filesystem::path directory = workspace_.root_ / "system" / "voice-input";
    const std::filesystem::path path = directory / "config.toml";
    if (!known_voice_input_provider(settings.provider)
        || settings.url.empty() || settings.model.empty()
        || settings.api_key_id.empty()) {
        throw std::invalid_argument("Invalid voice input settings");
    }
    if (!valid_voice_input_url(settings.provider, settings.url)) {
        throw std::invalid_argument(std::string(voice_input_url_message(settings.provider)));
    }
    std::string delay = settings.delay;
    if (settings.provider == "xai") {
        normalize_unused_voice_input_delay(settings.provider, delay);
    } else if (!valid_voice_input_delay(delay)) {
        throw std::invalid_argument("Invalid voice input settings");
    }
    toml::table table;
    table.insert("provider", settings.provider);
    table.insert("url", settings.url);
    table.insert("model", settings.model);
    table.insert("api_key", settings.api_key_id);
    table.insert("delay", delay);
    table.insert("prompt", settings.prompt);
    table.insert("send_phrase", settings.send_phrase);
    write_toml(path, table);
}

void WorkspaceConfigEditor::write_voice_output(const WorkspaceVoiceOutput& settings) {
    if (settings.default_voice.empty()) throw std::invalid_argument("Invalid voice output settings");
    toml::table table;
    table.insert("default_voice", settings.default_voice);
    if (settings.fishaudio) {
        const auto& fish = *settings.fishaudio;
        if (fish.api_key_id.empty()) throw std::invalid_argument("Invalid FishAudio output settings");
        table.insert("url", parse_voice_output_endpoint(fish.url));
        table.insert("model", normalize_voice_output_model(fish.model));
        table.insert("connection", normalize_voice_output_connection(fish.connection));
        table.insert("api_key", fish.api_key_id);
        table.insert("output_format", normalize_voice_output_format(fish.output_format));
    }
    if (settings.elevenlabs) {
        const auto& eleven = *settings.elevenlabs;
        if (eleven.api_key_id.empty()) throw std::invalid_argument("Invalid ElevenLabs output settings");
        table.insert("elevenlabs", toml::table{
            {"url", parse_voice_output_endpoint(eleven.url, "elevenlabs")},
            {"model", normalize_voice_output_model(eleven.model)},
            {"connection", normalize_voice_output_connection(eleven.connection)},
            {"api_key", eleven.api_key_id},
            {"output_format", normalize_elevenlabs_output_format(eleven.output_format)},
        });
    }
    write_toml(workspace_.root_ / "system" / "voice-output" / "config.toml", table);
}

void WorkspaceConfigEditor::create_api_key(
    std::string_view id,
    std::string_view display_name,
    std::string_view value) {
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "keys" / std::string(id);
    const std::filesystem::path path = directory / "config.toml";
    try {
        require_path_component(id, directory.parent_path());
        (void)saved_key_suffix(id, path);
        validate_saved_key_text(display_name, 100, "display_name", path);
        validate_saved_key_text(value, 16 * 1024, "value", path, false);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid API key");
    }
    if (workspace_.find_api_key(id) != nullptr
        || (workspace_.r2_storage_ && workspace_.r2_storage_->id == id)
        || source_.exists(directory)) {
        throw std::invalid_argument("Duplicate API key");
    }

    write_toml(path, api_key_table(display_name, value));
    write_next_api_key_id(saved_key_suffix(id, path) + 1);
    (void)load_saved_key(source_, directory, {workspace_.root_});
}

void WorkspaceConfigEditor::write_api_key(
    std::string_view id,
    std::string_view display_name,
    std::string_view value) {
    if (workspace_.find_api_key(id) == nullptr) {
        throw std::out_of_range("Unknown API key");
    }
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "keys" / std::string(id);
    const std::filesystem::path path = directory / "config.toml";
    try {
        validate_saved_key_text(display_name, 100, "display_name", path);
        validate_saved_key_text(value, 16 * 1024, "value", path, false);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid API key");
    }
    write_toml(path, api_key_table(display_name, value));
    (void)load_saved_key(source_, directory, {workspace_.root_});
}

void WorkspaceConfigEditor::delete_api_key(std::string_view id) {
    if (workspace_.find_api_key(id) == nullptr) {
        throw std::out_of_range("Unknown API key");
    }
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "keys" / std::string(id);
    remove_directory(directory);
}

void WorkspaceConfigEditor::create_r2_storage(const R2StorageKey& key) {
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "keys" / key.id;
    const std::filesystem::path path = directory / "config.toml";
    try {
        require_path_component(key.id, directory.parent_path());
        (void)saved_key_suffix(key.id, path);
        validate_saved_key_text(key.display_name, 100, "display_name", path);
        validate_saved_key_text(key.url, 16 * 1024, "url", path);
        validate_saved_key_text(
            key.access_key_id, 16 * 1024, "access_key_id", path);
        validate_saved_key_text(
            key.secret_key, 16 * 1024, "secret_key", path, false);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid R2 key");
    }
    if (workspace_.r2_storage_ || workspace_.find_api_key(key.id) != nullptr
        || source_.exists(directory)) {
        throw std::invalid_argument("Duplicate R2 key");
    }

    write_toml(path, r2_storage_table(key));
    write_next_api_key_id(saved_key_suffix(key.id, path) + 1);
    (void)load_saved_key(source_, directory, {workspace_.root_});
}

void WorkspaceConfigEditor::write_r2_storage(const R2StorageKey& key) {
    if (!workspace_.r2_storage_ || workspace_.r2_storage_->id != key.id) {
        throw std::out_of_range("Unknown R2 key");
    }
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "keys" / key.id;
    const std::filesystem::path path = directory / "config.toml";
    try {
        validate_saved_key_text(key.display_name, 100, "display_name", path);
        validate_saved_key_text(key.url, 16 * 1024, "url", path);
        validate_saved_key_text(
            key.access_key_id, 16 * 1024, "access_key_id", path);
        validate_saved_key_text(
            key.secret_key, 16 * 1024, "secret_key", path, false);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid R2 key");
    }
    write_toml(path, r2_storage_table(key));
    (void)load_saved_key(source_, directory, {workspace_.root_});
}

void WorkspaceConfigEditor::delete_r2_storage() {
    if (!workspace_.r2_storage_) throw std::out_of_range("Unknown R2 key");
    const std::filesystem::path directory =
        workspace_.root_ / "system" / "keys" / workspace_.r2_storage_->id;
    remove_directory(directory);
}

void WorkspaceConfigEditor::write_next_api_key_id(std::uint64_t next_id) {
    const std::filesystem::path path =
        workspace_.root_ / "system" / "keys" / "config.toml";
    write_toml(path, key_collection_table(next_id));
}

void WorkspaceConfigEditor::write_character_definition(
    std::string_view character_id,
    std::string_view display_name,
    std::optional<std::string_view> markdown) {
    const auto config = workspace_.character_config_paths_.find(std::string(character_id));
    if (config == workspace_.character_config_paths_.end()) {
        throw std::runtime_error(
            "Character '" + std::string(character_id)
            + "' has no writable configuration");
    }
    try {
        validate_public_name(display_name, "Character name", config->second, true);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid character name");
    }
    if (is_reserved_participant(display_name)) {
        throw std::invalid_argument("Reserved character name");
    }
    for (const WorkspaceCharacter& character : workspace_.characters_) {
        if (character.character.id != character_id
            && ascii_iequals(character.character.display_name, display_name)) {
            throw std::invalid_argument("Duplicate character name");
        }
    }
    for (const WorkspacePersona& persona : workspace_.personas_) {
        if (ascii_iequals(persona.display_name, display_name)) {
            throw std::invalid_argument("Character name conflicts with a persona");
        }
    }
    rewrite_toml(config->second, [&](toml::table& table) {
        table.insert_or_assign("display_name", std::string(display_name));
    });
    if (markdown) {
        const WorkspaceCharacter* character = workspace_.find_character(character_id);
        const std::filesystem::path filename = character != nullptr
                && character->prompt_template == embedded_new_character_template()
            ? "PROFILE.md" : "CHARACTER.md";
        write_file(config->second.parent_path() / filename, *markdown);
    }
}

void WorkspaceConfigEditor::write_character_file(
    std::string_view character_id,
    std::string_view filename,
    std::optional<std::string_view> content,
    bool create) {
    const auto* picture = picture_format(filename);
    if (picture && !content && !create) {
        try {
            require_path_component(filename, workspace_.root());
        } catch (const std::runtime_error&) {
            throw std::invalid_argument("Invalid picture filename.");
        }
        const auto directory = workspace_.character_directory(character_id);
        if (!directory || !source_.is_regular_file(*directory / filename)) {
            throw std::out_of_range("Unknown character picture");
        }
        files_.erase(source_.name(*directory / filename));
        return;
    }
    const auto config = workspace_.character_config_paths_.find(std::string(character_id));
    const auto* character = workspace_.find_character(character_id);
    if (config == workspace_.character_config_paths_.end() || !character) {
        throw std::out_of_range("Unknown writable character");
    }
    write_markdown_file(config->second, character->markdown_files,
        filename, content, create, "CHARACTER.md", "character", true);
}

void WorkspaceConfigEditor::rename_character_file(
    std::string_view character_id, std::string_view filename, std::string_view new_filename) {
    const auto directory = workspace_.character_directory(character_id);
    if (!directory) throw std::out_of_range("That character was not found.");
    try {
        require_path_component(filename, *directory);
        require_path_component(new_filename, *directory);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Enter a valid filename without folders.");
    }
    for (const auto reserved : {"CHARACTER.md", "character.toml", "PROFILE.md"}) {
        if (path_component_names_equal(filename, reserved)
            || path_component_names_equal(new_filename, reserved)) {
            throw std::invalid_argument("This filename is reserved.");
        }
    }
    const auto old_path = *directory / path_from_utf8(filename);
    const auto new_path = *directory / path_from_utf8(new_filename);
    const auto* picture = picture_format(filename);
    if (picture) {
        const auto* replacement = picture_format(new_filename);
        if (!replacement || replacement->mime_type != picture->mime_type) {
            throw std::invalid_argument("Keep the picture's file format when renaming it.");
        }
    } else if ((old_path.extension() != ".md" && old_path.extension() != ".txt")
        || (new_path.extension() != ".md" && new_path.extension() != ".txt")) {
        throw std::invalid_argument("Use a .md or .txt filename for a text file.");
    }
    const auto old_name = source_.name(old_path);
    const auto found = files_.find(old_name);
    if (found == files_.end()) throw std::out_of_range("That character file was not found.");
    if (filename == new_filename) return;
    for (const auto& path : source_.entries(*directory)) {
        const auto existing = utf8_path(path.filename());
        if (existing != filename && path_component_names_equal(existing, new_filename)) {
            throw std::invalid_argument("A file with this name already exists.");
        }
    }
    auto file = files_.extract(found);
    file.key() = source_.name(new_path);
    files_.insert(std::move(file));
    if (picture) return;

    // Keep prompt includes pointing at the renamed file. Ordinary prose and
    // escaped macros are left alone; the file stays in the same directory.
    for (auto& [name, content] : files_) {
        if (!name.ends_with(".md") && !name.ends_with(".txt")) continue;
        const auto parent = (workspace_.root() / path_from_utf8(name)).parent_path();
        for (std::size_t pos = 0; (pos = content.find("$$", pos)) != std::string::npos;) {
            if (content.compare(pos, 3, "$$$") == 0) { pos += 3; continue; }
            if (content.compare(pos, 3, "$$(") != 0) { pos += 2; continue; }
            const auto end = content.find_first_of(")\n", pos + 3);
            if (end == std::string::npos) break;
            if (content[end] != ')') { pos = end + 1; continue; }
            const auto include = path_from_utf8(trim_view(std::string_view(content).substr(pos + 3, end - pos - 3)));
            if ((parent / include).lexically_normal() == old_path.lexically_normal()) {
                const auto replacement = generic_utf8_path(include.parent_path() / path_from_utf8(new_filename));
                content.replace(pos + 3, end - pos - 3, replacement);
                pos += replacement.size() + 4;
            } else pos = end + 1;
        }
    }
}

void WorkspaceConfigEditor::delete_character(std::string_view character_id) {
    const auto path = workspace_.character_config_paths_.find(std::string(character_id));
    if (path == workspace_.character_config_paths_.end()) {
        throw std::runtime_error(
            "Character '" + std::string(character_id)
            + "' has no writable configuration");
    }
    for (const WorkspaceForum& forum : workspace_.forums_) {
        const bool used = std::ranges::any_of(
            forum.members,
            [&](const WorkspaceForumMember& member) {
                return member.character_id == character_id;
            });
        if (used) throw std::invalid_argument("Character is in use");
    }
    remove_directory(path->second.parent_path());
}

void WorkspaceConfigEditor::write_persona(
    std::string_view persona_id,
    std::string_view display_name,
    std::string_view markdown,
    std::optional<std::string_view> style_id,
    std::optional<std::string_view> voice_id) {
    const auto directory = workspace_.persona_directories_.find(std::string(persona_id));
    if (directory == workspace_.persona_directories_.end()) {
        throw std::runtime_error(
            "Persona '" + std::string(persona_id)
            + "' has no writable configuration");
    }
    const std::filesystem::path config_path = directory->second / "persona.toml";
    try {
        validate_public_name(display_name, "Persona name", config_path, true);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid persona name");
    }
    if (is_reserved_participant(display_name)) {
        throw std::invalid_argument("Reserved persona name");
    }
    for (const WorkspacePersona& persona : workspace_.personas_) {
        if (persona.id != persona_id
            && ascii_iequals(persona.display_name, display_name)) {
            throw std::invalid_argument("Duplicate persona name");
        }
    }
    for (const WorkspaceCharacter& character : workspace_.characters_) {
        if (ascii_iequals(character.character.display_name, display_name)) {
            throw std::invalid_argument("Persona name conflicts with a character");
        }
    }
    if (style_id && workspace_.find_style(*style_id) == nullptr) {
        throw std::invalid_argument(
            "Style '" + std::string(*style_id) + "' does not exist");
    }
    if (voice_id) {
        log_warn("Ignoring obsolete persona voice setting: " + utf8_path(config_path));
    }
    rewrite_toml(config_path, [&](toml::table& table) {
        table.insert_or_assign("display_name", std::string(display_name));
        if (style_id) table.insert_or_assign("style", std::string(*style_id));
        else table.erase("style");
        table.erase("voice");
    });
    write_file(directory->second / "PERSONA.md", markdown);
}

void WorkspaceConfigEditor::delete_persona(std::string_view persona_id) {
    const auto directory = workspace_.persona_directories_.find(std::string(persona_id));
    if (directory == workspace_.persona_directories_.end()) {
        throw std::runtime_error(
            "Persona '" + std::string(persona_id)
            + "' has no writable configuration");
    }
    for (const WorkspaceForum& forum : workspace_.forums_) {
        if (forum.default_persona_id == persona_id) {
            throw std::invalid_argument("Persona is in use");
        }
    }
    remove_directory(directory->second);
}

void WorkspaceConfigEditor::create_persona(
    std::string_view persona_id,
    std::string_view display_name) {
    if (!is_persona_id(persona_id) || is_reserved_participant(persona_id)
        || workspace_.find_persona(persona_id) != nullptr) {
        throw std::invalid_argument("Invalid persona ID");
    }
    const std::filesystem::path directory =
        workspace_.root_ / "personas" / std::string(persona_id);
    const std::filesystem::path config_path = directory / "persona.toml";
    try {
        validate_public_name(display_name, "Persona name", config_path, true);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid persona name");
    }
    if (is_reserved_participant(display_name)) {
        throw std::invalid_argument("Reserved persona name");
    }
    for (const WorkspacePersona& persona : workspace_.personas_) {
        if (ascii_iequals(persona.display_name, display_name)) {
            throw std::invalid_argument("Duplicate persona name");
        }
    }
    for (const WorkspaceCharacter& character : workspace_.characters_) {
        if (ascii_iequals(character.character.display_name, display_name)) {
            throw std::invalid_argument("Persona name conflicts with a character");
        }
    }

    toml::table config;
    config.insert("display_name", std::string(display_name));
    write_toml(config_path, config);
    write_file(directory / "PERSONA.md", "");
}

void WorkspaceConfigEditor::create_character(
    std::string_view character_id,
    std::string_view display_name,
    std::string_view description,
    std::optional<std::string_view> provider_id,
    std::string_view profile) {
    try {
        validate_workspace_character_id(character_id);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid character ID");
    }
    if (workspace_.find_character(character_id) != nullptr
        || workspace_.find_persona(character_id) != nullptr) {
        throw std::invalid_argument("Duplicate character ID");
    }
    const std::filesystem::path directory =
        workspace_.root_ / "characters" / std::string(character_id);
    const std::filesystem::path config_path = directory / "character.toml";
    try {
        validate_public_name(display_name, "Character name", config_path, true);
        validate_description(description, "Character", config_path);
    } catch (const std::runtime_error& error) {
        throw std::invalid_argument(error.what());
    }
    if (is_reserved_participant(display_name)) {
        throw std::invalid_argument("Reserved character name");
    }
    for (const WorkspaceCharacter& character : workspace_.characters_) {
        if (ascii_iequals(character.character.display_name, display_name)) {
            throw std::invalid_argument("Duplicate character name");
        }
    }
    for (const WorkspacePersona& persona : workspace_.personas_) {
        if (ascii_iequals(persona.display_name, display_name)) {
            throw std::invalid_argument("Character name conflicts with a persona");
        }
    }

    if (provider_id && workspace_.find_provider(*provider_id) == nullptr) {
        throw std::invalid_argument("Unknown provider '" + std::string(*provider_id) + "'");
    }
    toml::table config;
    if (provider_id) config.insert("provider", std::string(*provider_id));
    config.insert("display_name", std::string(display_name));
    config.insert("description", std::string(description));
    write_toml(config_path, config);
    const std::filesystem::path shared_voice =
        workspace_.root_ / "characters" / "character-voice.md";
    if (!source_.exists(shared_voice)) {
        write_file(shared_voice, embedded_character_voice());
    }
    write_file(
        directory / "CHARACTER.md", embedded_new_character_template());
    write_file(directory / "PROFILE.md", profile);
}

void WorkspaceConfigEditor::create_forum(
    std::string_view forum_id,
    std::string_view display_name,
    std::string_view persona_id) {
    const std::filesystem::path directory =
        workspace_.root_ / "forums" / std::string(forum_id);
    const std::filesystem::path config_path = directory / "config.toml";
    try {
        require_url_safe_identifier(forum_id, workspace_.root_ / "forums");
        validate_public_name(display_name, "Forum name", config_path);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid forum");
    }
    if (is_reserved_id(forum_id) || workspace_.find_forum(forum_id) != nullptr
        || ascii_iequals(display_name, "Entrance")
        || workspace_.find_persona(persona_id) == nullptr) {
        throw std::invalid_argument("Invalid forum");
    }
    for (const WorkspaceForum& forum : workspace_.forums_) {
        if (ascii_iequals(forum.display_name, display_name)) {
            throw std::invalid_argument("Duplicate forum name");
        }
    }

    toml::table config;
    config.insert("display_name", std::string(display_name));
    config.insert("default_persona", std::string(persona_id));
    write_toml(config_path, config);
    write_file(directory / "FORUM.md", "");
    const std::filesystem::path member =
        directory / "members" / std::string(workspace_assistant_id);

    write_file(member / "character.toml", "# Forum member\n");
}

void WorkspaceConfigEditor::delete_forum(std::string_view forum_id) {
    const auto path = workspace_.forum_config_paths_.find(std::string(forum_id));
    if (path == workspace_.forum_config_paths_.end()) {
        throw std::runtime_error(
            "Forum '" + std::string(forum_id)
            + "' has no writable configuration");
    }
    remove_directory(path->second.parent_path());
}

void WorkspaceConfigEditor::write_character_picture(
    std::string_view character_id,
    std::string_view filename,
    std::string_view content_base64, std::string_view previous_filename) {
    const auto directory = workspace_.character_directory(character_id);
    if (!directory) throw std::out_of_range("That character was not found.");
    try {
        require_path_component(filename, *directory);
        if (!previous_filename.empty()) require_path_component(previous_filename, *directory);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid picture filename.");
    }
    const auto* format = picture_format(filename);
    if (!format) {
        throw std::invalid_argument("Unsupported picture format.");
    }
    try {
        (void)decode_base64(content_base64);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid picture data.");
    }
    if (!previous_filename.empty()) {
        const auto* previous = picture_format(previous_filename);
        if (!previous) {
            throw std::invalid_argument("Unsupported picture filename.");
        }
        if (!source_.is_regular_file(*directory / previous_filename)) {
            throw std::out_of_range("That picture was not found.");
        }
        if (previous_filename != filename && source_.exists(*directory / filename)) {
            throw std::invalid_argument("A picture with this filename already exists.");
        }
        files_.erase(source_.name(*directory / previous_filename));
    }
    write_file(*directory / filename, content_base64);
}

void WorkspaceConfigEditor::write_character_settings(
    std::string_view character_id,
    std::string_view provider_id,
    std::optional<std::string_view> style_id,
    std::optional<std::string_view> voice_id,
    std::optional<std::string_view> reasoning_effort,
    std::optional<WebSearchMode> web_search,
    std::optional<bool> web_search_tool) {
    const auto configured = workspace_.character_config_paths_.find(std::string(character_id));
    if (configured == workspace_.character_config_paths_.end()
        && character_id != workspace_assistant_id) {
        throw std::runtime_error(
            "Character '" + std::string(character_id)
            + "' has no writable configuration");
    }
    const std::filesystem::path config = character_id == workspace_assistant_id
        ? workspace_.root_ / "system" / "assistant" / "character.toml"
        : configured->second;
    const WorkspaceProvider* const provider = workspace_.find_provider(provider_id);
    if (provider == nullptr) {
        throw std::invalid_argument(
            "Provider '" + std::string(provider_id) + "' does not exist");
    }
    if (style_id && workspace_.find_style(*style_id) == nullptr) {
        throw std::invalid_argument(
            "Style '" + std::string(*style_id) + "' does not exist");
    }
    if (voice_id && workspace_.find_voice(*voice_id) == nullptr) {
        throw std::invalid_argument(
            "Voice '" + std::string(*voice_id) + "' does not exist");
    }
    if (provider->config.api != ProviderApi::google
        && reasoning_effort && !valid_reasoning_effort(*reasoning_effort)) {
        throw std::invalid_argument(
            "Reasoning effort '" + std::string(*reasoning_effort)
            + "' is not supported");
    }
    if (web_search && *web_search != WebSearchMode::off
        && !provider_supports_web_search(provider->config)) {
        log_warn("Ignoring unsupported provider web search for character '"
            + std::string(character_id) + "' with provider '"
            + std::string(provider_id) + "'");
        web_search.reset();
    }
    rewrite_toml(config, [&](toml::table& table) {
        table.insert_or_assign("provider", std::string(provider_id));
        if (style_id) table.insert_or_assign("style", std::string(*style_id));
        else table.erase("style");
        if (voice_id) table.insert_or_assign("voice", std::string(*voice_id));
        else table.erase("voice");
        if (reasoning_effort) {
            table.insert_or_assign(
                "reasoning_effort", std::string(*reasoning_effort));
        } else {
            table.erase("reasoning_effort");
        }
        if (web_search) {
            table.insert_or_assign("web_search", std::string(to_string(*web_search)));
        } else {
            table.erase("web_search");
        }
        if (web_search_tool) table.insert_or_assign("web_search_tool", *web_search_tool);
        else table.erase("web_search_tool");
    });
}

void WorkspaceConfigEditor::write_forum_default_character(
    std::string_view forum_id,
    std::string_view character_id) {
    const auto config = workspace_.forum_config_paths_.find(std::string(forum_id));
    const WorkspaceForum* forum = workspace_.find_forum(forum_id);
    if (config == workspace_.forum_config_paths_.end() || forum == nullptr) {
        throw std::runtime_error(
            "Forum '" + std::string(forum_id)
            + "' has no writable configuration");
    }
    if (!std::ranges::any_of(
            forum->members,
            [character_id](const WorkspaceForumMember& member) {
                return member.character_id == character_id;
            })) {
        throw std::invalid_argument(
            "Character '" + std::string(character_id)
            + "' is not a member of forum '" + std::string(forum_id) + "'");
    }
    rewrite_toml(config->second, [&](toml::table& table) {
        table.erase("default_agent");
        table.insert_or_assign("default_character", std::string(character_id));
    });
}

void WorkspaceConfigEditor::write_forum(
    std::string_view forum_id,
    std::string_view display_name,
    std::string_view markdown) {
    const auto config = workspace_.forum_config_paths_.find(std::string(forum_id));
    if (config == workspace_.forum_config_paths_.end() || workspace_.find_forum(forum_id) == nullptr) {
        throw std::runtime_error(
            "Forum '" + std::string(forum_id)
            + "' has no writable configuration");
    }
    try {
        validate_public_name(display_name, "Forum name", config->second);
    } catch (const std::runtime_error&) {
        throw std::invalid_argument("Invalid forum name");
    }
    if (ascii_iequals(display_name, "Entrance")) {
        throw std::invalid_argument("Reserved forum name");
    }
    for (const WorkspaceForum& forum : workspace_.forums_) {
        if (forum.id != forum_id
            && ascii_iequals(forum.display_name, display_name)) {
            throw std::invalid_argument("Duplicate forum name");
        }
    }
    rewrite_toml(config->second, [&](toml::table& table) {
        table.insert_or_assign("display_name", std::string(display_name));
    });
    write_file(config->second.parent_path() / "FORUM.md", markdown);
}

void WorkspaceConfigEditor::write_forum_file(
    std::string_view forum_id,
    std::string_view filename,
    std::optional<std::string_view> content,
    bool create) {
    const auto config = workspace_.forum_config_paths_.find(std::string(forum_id));
    const auto* forum = workspace_.find_forum(forum_id);
    if (config == workspace_.forum_config_paths_.end() || !forum) {
        throw std::out_of_range("Unknown writable forum");
    }
    write_markdown_file(config->second, forum->markdown_files,
        filename, content, create, "FORUM.md", "forum", false);
}

void WorkspaceConfigEditor::add_prepared_character_to_forum(
    std::string_view forum_id, std::string_view character_id) {
    const auto config = workspace_.forum_config_paths_.find(std::string(forum_id));
    const auto* forum = workspace_.find_forum(forum_id);
    if (config == workspace_.forum_config_paths_.end() || !forum
        || forum_id == workspace_entrance_id) {
        throw std::invalid_argument("Forum '" + std::string(forum_id)
            + "' has no writable configuration");
    }
    const auto table = read_toml(source_, config->second, "forum config");
    if (!table.contains("default_character") && !table.contains("default_agent")
        && character_id < forum->default_character_id) {
        // Preserve all source bytes; an implicit default is the smallest member ID.
        std::string content = *source_.read(config->second);
        const std::size_t offset = content.starts_with("\xef\xbb\xbf") ? 3 : 0;
        content.insert(offset, "default_character = \"" + forum->default_character_id + "\"\n");
        write_file(config->second, content);
    }
    write_file(config->second.parent_path() / "members" / std::string(character_id)
        / "character.toml", "# Forum member\n");
}

void WorkspaceConfigEditor::write_forum_members(
    std::string_view forum_id,
    std::span<const std::string> character_ids) {
    const auto config = workspace_.forum_config_paths_.find(std::string(forum_id));
    const WorkspaceForum* forum = workspace_.find_forum(forum_id);
    if (config == workspace_.forum_config_paths_.end() || forum == nullptr) {
        throw std::runtime_error(
            "Forum '" + std::string(forum_id)
            + "' has no writable configuration");
    }
    if (character_ids.empty()) {
        throw std::invalid_argument("Forum requires a member");
    }

    std::vector<std::string> selected(character_ids.begin(), character_ids.end());
    std::ranges::sort(selected);
    if (std::ranges::adjacent_find(selected) != selected.end()) {
        throw std::invalid_argument("Duplicate forum member");
    }
    for (const std::string& character_id : selected) {
        const WorkspaceCharacter* character = workspace_.find_character(character_id);
        if (character == nullptr
            || (is_reserved_id(character_id)
                && character_id != workspace_assistant_id)
            || !character->provider_id) {
            throw std::invalid_argument("Invalid forum member");
        }
    }

    const std::filesystem::path members = config->second.parent_path() / "members";
    for (const WorkspaceForumMember& member : forum->members) {
        if (std::ranges::binary_search(selected, member.character_id)) continue;
        const std::filesystem::path directory =
            members / path_from_utf8(member.character_id);
        remove_directory(directory);
    }
    for (const std::string& character_id : selected) {
        if (workspace_.find_forum_member(forum_id, character_id) != nullptr) continue;
        const std::filesystem::path directory =
            members / path_from_utf8(character_id);

        write_file(directory / "character.toml", "# Forum member\n");
    }

    const std::string& default_character = std::ranges::binary_search(
        selected, forum->default_character_id)
        ? forum->default_character_id
        : selected.front();
    rewrite_toml(config->second, [&](toml::table& table) {
        table.erase("default_agent");
        table.insert_or_assign("default_character", default_character);
    });
}

void WorkspaceConfigEditor::write_forum_default_persona(
    std::string_view forum_id,
    std::string_view persona_id) {
    const auto config = workspace_.forum_config_paths_.find(std::string(forum_id));
    if (config == workspace_.forum_config_paths_.end() || workspace_.find_forum(forum_id) == nullptr) {
        throw std::runtime_error(
            "Forum '" + std::string(forum_id)
            + "' has no writable configuration");
    }
    if (workspace_.find_persona(persona_id) == nullptr) {
        throw std::invalid_argument(
            "Persona '" + std::string(persona_id) + "' does not exist");
    }
    rewrite_toml(config->second, [&](toml::table& table) {
        table.insert_or_assign("default_persona", std::string(persona_id));
    });
}

} // namespace cha
