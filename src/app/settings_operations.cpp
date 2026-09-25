#include "app/settings_operations.h"

#include "app/application.h"
#include "app/workspace_operations.h"
#include "providers/api_key_store.h"
#include "providers/provider_client.h"
#include "runtime/request_parser.h"
#include "runtime/live_session_manager.h"
#include "workspace/workspace_config_store.h"

#include <memory>
#include <stdexcept>
#include <utility>

namespace cha::app::settings {
namespace {

[[noreturn]] void fail(ErrorCode code, std::string message) {
    throw ApplicationError(code, std::move(message));
}

template<typename Fn>
auto with_settings_edit(Fn&& fn) {
    try {
        return fn();
    } catch (const ApplicationError&) {
        throw;
    } catch (const std::out_of_range&) {
        fail(ErrorCode::not_found, "That settings item was not found.");
    } catch (const WorkspaceRestartRequiredError& error) {
        fail(ErrorCode::application_unavailable, error.what());
    } catch (const std::invalid_argument& error) {
        fail(ErrorCode::invalid_argument, error.what());
    }
}

std::vector<std::string> provider_uses(
    const Workspace& workspace,
    std::string_view provider_id) {
    std::vector<std::string> result;
    for (const WorkspaceCharacter& character : workspace.characters()) {
        if (character.provider_id == provider_id) {
            result.push_back(character.character.display_name);
        }
    }
    if (workspace.web_search().enabled
        && workspace.web_search().query_provider_id == provider_id) {
        result.emplace_back("Search API");
    }
    return result;
}

std::vector<std::string> characters_using_style(
    const Workspace& workspace,
    std::string_view style_id) {
    std::vector<std::string> result;
    for (const WorkspaceCharacter& character : workspace.characters()) {
        if (character.style_id == style_id) {
            result.push_back(character.character.display_name);
        }
    }
    for (const WorkspacePersona& persona : workspace.personas()) {
        if (persona.style_id == style_id) result.push_back(persona.display_name);
    }
    return result;
}

std::vector<std::string> characters_using_voice(
    const Workspace& workspace,
    std::string_view voice_id) {
    std::vector<std::string> result;
    for (const WorkspaceCharacter& character : workspace.characters()) {
        if (character.voice_id == voice_id) {
            result.push_back(character.character.display_name);
        }
    }
    for (const WorkspacePersona& persona : workspace.personas()) {
        if (persona.voice_id == voice_id) result.push_back(persona.display_name);
    }
    const WorkspaceVoice* const voice = workspace.find_voice(voice_id);
    if (voice && workspace.voice_output()
        && workspace.voice_output()->default_voice == voice->label) {
        result.emplace_back("Voice output");
    }
    return result;
}

std::vector<std::string> providers_using_key(
    const Workspace& workspace,
    const ApiKeyInfo& key) {
    std::vector<std::string> result;
    for (const WorkspaceProvider& provider : workspace.providers()) {
        if (provider.config.api_key_id == key.id
            || provider.config.api_key_env == key.display_name) {
            result.push_back(provider.label);
        }
    }
    return result;
}

std::string_view status_name(OpenAiOAuthState state) {
    switch (state) {
    case OpenAiOAuthState::waiting:
        return "waiting";
    case OpenAiOAuthState::connected:
        return "connected";
    case OpenAiOAuthState::signed_out:
        return "signed_out";
    }
    return "signed_out";
}

const WorkspaceProvider& require_provider(
    const Workspace& workspace,
    std::string_view id,
    bool writable) {
    const WorkspaceProvider* provider = workspace.find_provider(id);
    if (provider == nullptr || (writable && !workspace.provider_is_writable(id))) {
        fail(ErrorCode::not_found, "That provider was not found.");
    }
    return *provider;
}

const WorkspaceStyle& require_style(
    const Workspace& workspace,
    std::string_view id,
    bool writable) {
    const WorkspaceStyle* style = workspace.find_style(id);
    if (style == nullptr || (writable && !workspace.style_is_writable(id))) {
        fail(ErrorCode::not_found, "That style was not found.");
    }
    return *style;
}

const WorkspaceVoice& require_voice(
    const Workspace& workspace,
    std::string_view id,
    bool writable) {
    const WorkspaceVoice* voice = workspace.find_voice(id);
    if (voice == nullptr || (writable && !workspace.voice_is_writable(id))) {
        fail(ErrorCode::not_found, "That voice was not found.");
    }
    return *voice;
}

VoiceInputSettings voice_input_settings(
    const WorkspaceVoiceInput& settings) {
    return {
        .provider = settings.provider,
        .url = settings.url,
        .model = settings.model,
        .api_key = settings.api_key_id,
        .delay = settings.delay,
        .prompt = settings.prompt,
        .send_phrase = settings.send_phrase,
    };
}

VoiceOutputSettings voice_output_settings(
    const WorkspaceVoiceOutput& settings) {
    return {
        .url = settings.url,
        .model = settings.model,
        .api_key = settings.api_key_id,
        .output_format = settings.output_format,
        .default_voice = settings.default_voice,
    };
}

} // namespace

ProviderSummary provider_summary(const WorkspaceProvider& provider) {
    return {
        .id = provider.id,
        .display_name = provider.label,
        .model = provider.config.model,
        .host = provider.config.host,
    };
}

ProviderDetail provider_detail(
    const WorkspaceProvider& provider,
    bool writable,
    std::vector<std::string> used_by,
    const ApiKeyStore& api_keys) {
    const ModelBackendConfig& config = provider.config;
    std::optional<std::string> api_key;
    if (!config.api_key_id.empty()) {
        api_key = config.api_key_id;
    } else if (!config.api_key_env.empty()) {
        if (const auto named = api_keys.find_by_name(config.api_key_env)) {
            api_key = named->id;
        }
    }
    return {
        .id = provider.id,
        .display_name = provider.label,
        .host = config.host,
        .port = config.port,
        .base_path = config.base_path,
        .mode = std::string(to_string(config.mode)),
        .model = config.model,
        .stream = config.stream,
        .temperature = config.temperature,
        .max_tokens = config.max_tokens,
        .timeout_s = config.timeout_s,
        .idle_timeout_s = config.idle_timeout_s,
        .api_key = std::move(api_key),
        .reasoning_effort = config.reasoning_effort,
        .reasoning_format = std::string(to_string(config.reasoning_format)),
        .https = config.https,
        .api = std::string(to_string(config.api)),
        .auth = std::string(to_string(config.auth)),
        .web_search = std::string(to_string(config.web_search)),
        .cache_retention = std::string(to_string(config.cache_retention)),
        .openrouter_targets = config.openrouter_targets,
        .writable = writable,
        .used_by = std::move(used_by),
    };
}

StyleDetail style_detail(
    const WorkspaceStyle& style,
    bool writable,
    std::vector<std::string> used_by) {
    return {
        .id = style.id,
        .display_name = style.label,
        .appearance = style.appearance,
        .writable = writable,
        .used_by = std::move(used_by),
    };
}

VoiceDetail voice_detail(
    const WorkspaceVoice& voice,
    bool writable,
    std::vector<std::string> used_by) {
    return {
        .id = voice.id,
        .display_name = voice.label,
        .description = voice.description,
        .elevenlabs_voice_id = voice.elevenlabs_voice_id,
        .speed = voice.settings.speed,
        .writable = writable,
        .used_by = std::move(used_by),
    };
}

ApiKeyDetail api_key_detail(
    const ApiKeyInfo& key,
    const Workspace& workspace) {
    std::vector<std::string> used_by = providers_using_key(workspace, key);
    if (workspace.voice_input()
        && key.id == workspace.voice_input()->api_key_id) {
        used_by.emplace_back("Voice input");
    }
    if (workspace.voice_output()
        && key.id == workspace.voice_output()->api_key_id) {
        used_by.emplace_back("Voice output");
    }
    if (workspace.jev() && key.id == workspace.jev()->api_key_id) {
        used_by.emplace_back("Recipient detection");
    }
    if ((workspace.web_search().enabled || workspace.web_search().tool_enabled
            || std::ranges::any_of(workspace.characters(), [](const auto& character) {
                return character.web_search_tool.value_or(false);
            }))
        && key.id == workspace.web_search().api_key_id) {
        used_by.emplace_back("Search API");
    }
    return {
        .id = key.id,
        .display_name = key.display_name,
        .has_value = key.has_value,
        .used_by = std::move(used_by),
    };
}

R2StorageDetail r2_storage_detail(const R2StorageInfo& key) {
    return {
        .id = key.id,
        .display_name = key.display_name,
        .url = key.url,
        .access_key_id = key.access_key_id,
        .has_secret_key = key.has_secret_key,
    };
}

OpenAiAuth openai_auth_from(const OpenAiOAuthSnapshot& snapshot) {
    OpenAiAuth result;
    result.status = std::string(status_name(snapshot.state));
    if (snapshot.state == OpenAiOAuthState::waiting) {
        result.user_code = snapshot.user_code;
        result.verification_url = snapshot.verification_url;
        result.attempt_expires_at = snapshot.attempt_expires_at;
        result.next_poll_delay_ms = snapshot.next_poll_delay_ms;
    }
    result.error = snapshot.error;
    return result;
}

std::vector<ProviderSummary> list_providers(
    const Workspace& workspace) {
    std::vector<ProviderSummary> result;
    for (const WorkspaceProvider& provider : workspace.providers()) {
        result.push_back(provider_summary(provider));
    }
    return result;
}

ProviderDetail get_provider(
    const Workspace& workspace,
    std::string_view id,
    const ApiKeyStore& api_keys) {
    const WorkspaceProvider& provider = require_provider(workspace, id, false);
    return provider_detail(
        provider,
        workspace.provider_is_writable(id),
        provider_uses(workspace, id),
        api_keys);
}

ProviderDetail create_provider(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const CreateProviderRequest& create) {
    return with_settings_edit([&] {
        std::string id;
        try {
            id = store.create_provider(create.display_name, create.copy_from);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid provider.");
        }
        const auto current = store.snapshot();
        const WorkspaceProvider* created = current->find_provider(id);
        if (created == nullptr) {
            fail(ErrorCode::internal_error, "The provider could not be created.");
        }
        return provider_detail(
            *created,
            current->provider_is_writable(id),
            provider_uses(*current, id),
            api_keys);
    });
}

ProviderDetail update_provider(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    const ApiKeyStore& api_keys,
    std::string_view id,
    const nlohmann::json& body) {
    const auto workspace = store.snapshot();
    const WorkspaceProvider& provider = require_provider(*workspace, id, true);
    ProviderUpdate update;
    try {
        update = parse_provider_update(
            body, provider.config.openrouter_targets);
    } catch (const std::invalid_argument&) {
        fail(ErrorCode::invalid_argument, "Invalid provider settings.");
    }
    return with_settings_edit([&] {
        try {
            const WorkspaceConfigEditResult edited = store.apply_provider_update(
                id, update.display_name, update.config);
            workspace::invalidate_affected_sessions(
                live_sessions, edited.affected_forum_ids);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid provider settings.");
        }
        const auto current = store.snapshot();
        const WorkspaceProvider* updated = current->find_provider(id);
        if (updated == nullptr) {
            fail(ErrorCode::internal_error, "The provider could not be updated.");
        }
        return provider_detail(
            *updated,
            current->provider_is_writable(id),
            provider_uses(*current, id),
            api_keys);
    });
}

void delete_provider(WorkspaceConfigStore& store, std::string_view id) {
    const auto workspace = store.snapshot();
    require_provider(*workspace, id, true);
    if (!provider_uses(*workspace, id).empty()) {
        fail(
            ErrorCode::invalid_argument,
            "This provider is still in use.");
    }
    with_settings_edit([&] {
        try {
            store.apply_provider_delete(id);
        } catch (const std::invalid_argument&) {
            fail(
                ErrorCode::invalid_argument,
                "This provider is still in use.");
        }
        return 0;
    });
}

void test_provider(
    const Workspace& workspace,
    std::string_view id,
    const nlohmann::json& body,
    OpenAiOAuth& openai_auth,
    ApiKeyStore& api_keys,
    const std::atomic_bool& cancellation) {
    const WorkspaceProvider& provider = require_provider(workspace, id, false);
    ProviderUpdate candidate;
    try {
        candidate = parse_provider_update(
            body, provider.config.openrouter_targets);
    } catch (const std::invalid_argument&) {
        fail(ErrorCode::invalid_argument, "The request was not valid.");
    }
    ModelBackendConfig config = std::move(candidate.config);
    config.mode = Mode::net;
    config.timeout_s = 10;
    config.idle_timeout_s = 10;
    config.web_search = WebSearchMode::off;
    const CharacterMetadata character{
        .id = "provider-test",
        .display_name = "Provider Test",
    };
    const auto definition = std::make_shared<const CharacterDefinition>(
        CharacterDefinition{
            .character = character,
            .provider = {std::string(id), std::move(config)},
        });
    const GenerationRequest request{
        .history = std::make_shared<const ModelHistory>(),
        .run = {
            .session = {"provider-test", "provider-test"},
            .request_id = 1,
            .target = character,
            .author = {"provider-test-user", "User"},
            .prompt_text = "Reply with OK.",
        },
    };
    try {
        ProviderClient client(definition, &openai_auth, &api_keys);
        const GenerationResult result = client.perform(
            client.prepare(request),
            [](GenerationDelta) {},
            cancellation);
        if (cancellation.load()) {
            fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
        }
        if (result.outcome != GenerationOutcome::completed) {
            const std::string reason = result.message.empty()
                ? "The provider returned an error." : result.message;
            fail(
                ErrorCode::invalid_argument,
                "Provider test failed: " + reason);
        }
    } catch (const ApplicationError&) {
        throw;
    } catch (const std::exception& error) {
        fail(
            ErrorCode::invalid_argument,
            "Provider test failed: " + std::string(error.what()));
    }
}

std::vector<StyleDetail> list_styles(
    const Workspace& workspace) {
    std::vector<StyleDetail> result;
    for (const WorkspaceStyle& style : workspace.styles()) {
        result.push_back(style_detail(
            style,
            workspace.style_is_writable(style.id),
            characters_using_style(workspace, style.id)));
    }
    return result;
}

StyleDetail create_style(
    WorkspaceConfigStore& store,
    std::string_view display_name) {
    return with_settings_edit([&] {
        std::string id;
        try {
            id = store.create_style(display_name);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid style.");
        }
        const auto current = store.snapshot();
        const WorkspaceStyle* created = current->find_style(id);
        if (created == nullptr) {
            fail(ErrorCode::internal_error, "The style could not be created.");
        }
        return style_detail(
            *created,
            current->style_is_writable(id),
            characters_using_style(*current, id));
    });
}

StyleDetail update_style(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const StyleUpdate& update) {
    require_style(*store.snapshot(), id, true);
    return with_settings_edit([&] {
        try {
            const WorkspaceConfigEditResult edited = store.apply_style_update(
                id, update.display_name, update.appearance);
            workspace::refresh_affected_sessions(
                live_sessions, edited.affected_forum_ids);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid style settings.");
        }
        const auto current = store.snapshot();
        const WorkspaceStyle* updated = current->find_style(id);
        if (updated == nullptr) {
            fail(ErrorCode::internal_error, "The style could not be updated.");
        }
        return style_detail(
            *updated,
            current->style_is_writable(id),
            characters_using_style(*current, id));
    });
}

void delete_style(WorkspaceConfigStore& store, std::string_view id) {
    const auto workspace = store.snapshot();
    require_style(*workspace, id, true);
    if (!characters_using_style(*workspace, id).empty()) {
        fail(
            ErrorCode::invalid_argument,
            "This style is still used by one or more characters.");
    }
    with_settings_edit([&] {
        try {
            store.apply_style_delete(id);
        } catch (const std::invalid_argument&) {
            fail(
                ErrorCode::invalid_argument,
                "This style is still used by one or more characters.");
        }
        return 0;
    });
}

std::vector<VoiceDetail> list_voices(
    const Workspace& workspace) {
    std::vector<VoiceDetail> result;
    for (const WorkspaceVoice& voice : workspace.voices()) {
        result.push_back(voice_detail(
            voice,
            workspace.voice_is_writable(voice.id),
            characters_using_voice(workspace, voice.id)));
    }
    return result;
}

VoiceDetail create_voice(
    WorkspaceConfigStore& store,
    const CreateVoiceRequest& create) {
    return with_settings_edit([&] {
        std::string id;
        try {
            id = store.create_voice(
                create.display_name,
                create.description,
                create.elevenlabs_voice_id);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid voice.");
        }
        const auto current = store.snapshot();
        const WorkspaceVoice* created = current->find_voice(id);
        if (created == nullptr) {
            fail(ErrorCode::internal_error, "The voice could not be created.");
        }
        return voice_detail(
            *created,
            current->voice_is_writable(id),
            characters_using_voice(*current, id));
    });
}

VoiceDetail update_voice(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const VoiceUpdate& update) {
    require_voice(*store.snapshot(), id, true);
    return with_settings_edit([&] {
        try {
            const WorkspaceConfigEditResult edited = store.apply_voice_update(
                id,
                update.display_name,
                update.description,
                update.elevenlabs_voice_id,
                VoiceSettings{.speed = update.settings.speed});
            workspace::refresh_affected_sessions(
                live_sessions, edited.affected_forum_ids);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid voice settings.");
        }
        const auto current = store.snapshot();
        const WorkspaceVoice* updated = current->find_voice(id);
        if (updated == nullptr) {
            fail(ErrorCode::internal_error, "The voice could not be updated.");
        }
        return voice_detail(
            *updated,
            current->voice_is_writable(id),
            characters_using_voice(*current, id));
    });
}

void delete_voice(WorkspaceConfigStore& store, std::string_view id) {
    const auto workspace = store.snapshot();
    require_voice(*workspace, id, true);
    if (!characters_using_voice(*workspace, id).empty()) {
        fail(ErrorCode::invalid_argument, "This voice is still in use.");
    }
    with_settings_edit([&] {
        try {
            store.apply_voice_delete(id);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "This voice is still in use.");
        }
        return 0;
    });
}

std::optional<JevSettings> get_jev_settings(const Workspace& workspace) {
    if (!workspace.jev()) return std::nullopt;
    const auto& settings = *workspace.jev();
    return JevSettings{settings.url, settings.model, settings.api_key_id};
}

JevSettings save_jev_settings(WorkspaceConfigStore& store, const JevSettings& update) {
    return with_settings_edit([&] {
        try {
            store.apply_jev_update(WorkspaceJev{update.url, update.model, update.api_key});
        } catch (const std::invalid_argument& error) {
            fail(ErrorCode::invalid_argument, error.what());
        }
        return *get_jev_settings(*store.snapshot());
    });
}

void disable_jev(WorkspaceConfigStore& store) {
    with_settings_edit([&] { store.apply_jev_update(std::nullopt); });
}

WebSearchSettings get_web_search_settings(const Workspace& workspace) {
    const auto& settings = workspace.web_search();
    return {settings.enabled, settings.provider, settings.api_key_id,
        settings.query_provider_id, settings.tool_enabled};
}

WebSearchSettings save_web_search_settings(
    WorkspaceConfigStore& store, const WebSearchSettings& update) {
    return with_settings_edit([&] {
        store.apply_web_search_update(
            WorkspaceWebSearch{update.enabled, update.provider, update.api_key,
                update.query_provider, update.tool_enabled});
        return get_web_search_settings(*store.snapshot());
    });
}

std::optional<VoiceInputSettings> get_voice_input_settings(
    const Workspace& workspace) {
    if (!workspace.voice_input()) return std::nullopt;
    return voice_input_settings(*workspace.voice_input());
}

VoiceInputSettings save_voice_input_settings(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const VoiceInputSettings& update) {
    if (!api_keys.find(update.api_key)) {
        fail(ErrorCode::invalid_argument, "Invalid voice input settings.");
    }
    WorkspaceVoiceInput settings{
        .provider = update.provider,
        .url = update.url,
        .model = update.model,
        .api_key_id = update.api_key,
        .delay = update.delay,
        .prompt = update.prompt,
        .send_phrase = update.send_phrase,
    };
    normalize_unused_voice_input_delay(settings.provider, settings.delay);
    return with_settings_edit([&] {
        try {
            store.apply_voice_input_update(settings);
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            if (message == openai_voice_input_url_message
                || message == xai_voice_input_url_message) {
                fail(ErrorCode::invalid_argument, message);
            }
            fail(ErrorCode::invalid_argument, "Invalid voice input settings.");
        }
        return voice_input_settings(settings);
    });
}

std::optional<VoiceInputRuntime> get_voice_input_runtime(
    const Workspace& workspace,
    const ApiKeyStore& api_keys,
    bool voice_enabled) {
    if (!voice_enabled || !workspace.voice_input()
        || !api_keys.find(workspace.voice_input()->api_key_id)) {
        return std::nullopt;
    }
    const WorkspaceVoiceInput& settings = *workspace.voice_input();
    return VoiceInputRuntime{
        .provider = settings.provider,
        .url = settings.url,
        .model = settings.model,
        .delay = settings.delay,
        .prompt = settings.prompt,
        .send_phrase = settings.send_phrase,
    };
}

std::optional<std::string> voice_input_secret(
    const Workspace& workspace,
    const ApiKeyStore& api_keys,
    bool voice_enabled) {
    if (!voice_enabled || !workspace.voice_input()
        || !api_keys.find(workspace.voice_input()->api_key_id)) {
        return std::nullopt;
    }
    return api_keys.value(workspace.voice_input()->api_key_id);
}

std::optional<VoiceOutputSettings> get_voice_output_settings(
    const Workspace& workspace) {
    if (!workspace.voice_output()) return std::nullopt;
    return voice_output_settings(*workspace.voice_output());
}

VoiceOutputSettings save_voice_output_settings(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const VoiceOutputSettings& update) {
    const auto workspace = store.snapshot();
    if (!api_keys.find(update.api_key)
        || !workspace->find_voice_by_name(update.default_voice)) {
        fail(ErrorCode::invalid_argument, "Invalid voice output settings.");
    }
    WorkspaceVoiceOutput settings{
        .url = update.url,
        .model = update.model,
        .api_key_id = update.api_key,
        .output_format = update.output_format,
        .default_voice = update.default_voice,
    };
    return with_settings_edit([&] {
        try {
            store.apply_voice_output_update(settings);
        } catch (const std::invalid_argument& error) {
            fail(ErrorCode::invalid_argument, error.what());
        }
        return voice_output_settings(*store.snapshot()->voice_output());
    });
}

std::optional<VoiceOutputRuntime> get_voice_output_runtime(
    const Workspace& workspace,
    const ApiKeyStore& api_keys,
    bool voice_enabled) {
    const WorkspaceVoiceOutput* const output =
        workspace.voice_output() ? &*workspace.voice_output() : nullptr;
    const WorkspaceVoice* const default_voice = output
        ? workspace.find_voice_by_name(output->default_voice) : nullptr;
    if (!voice_enabled || !output || !default_voice
        || !api_keys.find(output->api_key_id)) {
        return std::nullopt;
    }
    return VoiceOutputRuntime{
        .url = output->url,
        .model = output->model,
        .output_format = output->output_format,
        .default_voice_id = default_voice->elevenlabs_voice_id,
    };
}

std::vector<ApiKeyDetail> list_api_keys(
    const Workspace& workspace,
    const ApiKeyStore& api_keys) {
    std::vector<ApiKeyDetail> result;
    for (const ApiKeyInfo& key : api_keys.list()) {
        result.push_back(api_key_detail(key, workspace));
    }
    return result;
}

ApiKeyDetail create_api_key(
    const Workspace& workspace,
    ApiKeyStore& api_keys,
    const CreateApiKeyRequest& create) {
    return with_settings_edit([&] {
        try {
            return api_key_detail(
                api_keys.create(create.display_name, create.value),
                workspace);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid API key.");
        }
    });
}

ApiKeyDetail rename_api_key(
    const Workspace& workspace,
    ApiKeyStore& api_keys,
    std::string_view id,
    std::string_view display_name) {
    return with_settings_edit([&] {
        try {
            return api_key_detail(
                api_keys.rename(id, display_name), workspace);
        } catch (const std::out_of_range&) {
            fail(ErrorCode::not_found, "That API key was not found.");
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid API key.");
        }
    });
}

ApiKeyDetail replace_api_key_value(
    const Workspace& workspace,
    ApiKeyStore& api_keys,
    std::string_view id,
    std::string_view value) {
    return with_settings_edit([&] {
        try {
            return api_key_detail(
                api_keys.replace(id, value), workspace);
        } catch (const std::out_of_range&) {
            fail(ErrorCode::not_found, "That API key was not found.");
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid API key.");
        }
    });
}

void delete_api_key(ApiKeyStore& api_keys, std::string_view id) {
    with_settings_edit([&] {
        try {
            api_keys.remove(id);
        } catch (const std::out_of_range&) {
            fail(ErrorCode::not_found, "That API key was not found.");
        }
        return 0;
    });
}

std::optional<R2StorageDetail> get_r2_storage(
    const ApiKeyStore& api_keys) {
    const std::optional<R2StorageInfo> key = api_keys.r2_info();
    if (!key) return std::nullopt;
    return r2_storage_detail(*key);
}

R2StorageDetail save_r2_storage(
    ApiKeyStore& api_keys,
    const SaveR2StorageRequest& request) {
    return with_settings_edit([&] {
        try {
            const std::optional<std::string_view> secret = request.secret_key
                ? std::optional<std::string_view>(*request.secret_key)
                : std::nullopt;
            return r2_storage_detail(api_keys.save_r2(
                request.display_name,
                request.url,
                request.access_key_id,
                secret));
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument, "Invalid R2 credentials.");
        }
    });
}

void delete_r2_storage(ApiKeyStore& api_keys) {
    with_settings_edit([&] {
        try {
            api_keys.remove_r2();
        } catch (const std::out_of_range&) {
            fail(
                ErrorCode::not_found,
                "R2 storage credentials are not configured.");
        }
        return 0;
    });
}

OpenAiAuth openai_auth_status(const OpenAiOAuth& owner) {
    return openai_auth_from(owner.status());
}

OpenAiAuth start_openai_auth(
    OpenAiOAuth& owner,
    const std::atomic_bool& cancelled) {
    return openai_auth_from(owner.start([&] { return cancelled.load(); }));
}

OpenAiAuth poll_openai_auth(
    OpenAiOAuth& owner,
    const std::atomic_bool& cancelled) {
    return openai_auth_from(owner.poll([&] { return cancelled.load(); }));
}

OpenAiAuth disconnect_openai_auth(OpenAiOAuth& owner) {
    return openai_auth_from(owner.disconnect());
}

} // namespace cha::app::settings
