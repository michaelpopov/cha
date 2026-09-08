#include "web/settings_routes.h"

#include "providers/api_key_store.h"
#include "providers/openai_oauth.h"
#include "providers/provider_client.h"
#include "web/http_response.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha::web {
namespace {

using Json = nlohmann::json;

std::shared_ptr<const Workspace> published_workspace() {
    auto workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    return workspace;
}

GenerationResult test_provider(
    std::string_view provider_id,
    ModelBackendConfig config,
    OpenAiOAuth& openai_auth,
    ApiKeyStore& api_keys) {
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
            .provider = {std::string(provider_id), std::move(config)},
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
    ProviderClient client(definition, &openai_auth, &api_keys);
    const std::atomic_bool cancellation{false};
    return client.perform(
        client.prepare(request),
        [](GenerationDelta) {},
        cancellation);
}

std::string_view mode_name(Mode value) {
    return value == Mode::net ? "net" : "test";
}

std::string_view api_name(ProviderApi value) {
    return value == ProviderApi::chat_completions
        ? "chat_completions" : "responses";
}

std::string_view auth_name(ProviderAuth value) {
    return value == ProviderAuth::openai_subscription
        ? "openai_subscription" : "none";
}

std::string_view reasoning_format_name(ReasoningFormat value) {
    switch (value) {
    case ReasoningFormat::automatic: return "auto";
    case ReasoningFormat::none: return "none";
    case ReasoningFormat::reasoning_content: return "reasoning_content";
    case ReasoningFormat::reasoning: return "reasoning";
    }
    throw std::logic_error("Invalid reasoning format");
}

std::string_view cache_retention_name(CacheRetention value) {
    switch (value) {
    case CacheRetention::off: return "off";
    case CacheRetention::short_: return "short";
    case CacheRetention::long_: return "long";
    }
    throw std::logic_error("Invalid cache retention");
}

std::vector<std::string> characters_using_provider(
    const Workspace& workspace,
    std::string_view provider_id) {
    std::vector<std::string> result;
    for (const WorkspaceCharacter& character : workspace.characters()) {
        if (character.provider_id == provider_id) {
            result.push_back(character.character.display_name);
        }
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
    return result;
}

Json provider_json(
    const WorkspaceProvider& provider,
    bool writable,
    std::vector<std::string> used_by,
    const ApiKeyStore& api_keys) {
    const ModelBackendConfig& config = provider.config;
    Json api_key = config.api_key_id.empty()
        ? Json(nullptr) : Json(config.api_key_id);
    if (api_key.is_null() && !config.api_key_env.empty()) {
        if (const auto named = api_keys.find_by_name(config.api_key_env)) {
            api_key = named->id;
        }
    }
    return {
        {"id", provider.id},
        {"display_name", provider.label},
        {"host", config.host},
        {"port", config.port},
        {"base_path", config.base_path},
        {"mode", mode_name(config.mode)},
        {"model", config.model},
        {"stream", config.stream},
        {"temperature", config.temperature
            ? Json(*config.temperature) : Json(nullptr)},
        {"max_tokens", config.max_tokens
            ? Json(*config.max_tokens) : Json(nullptr)},
        {"timeout_s", config.timeout_s},
        {"idle_timeout_s", config.idle_timeout_s},
        {"api_key", std::move(api_key)},
        {"reasoning_effort", config.reasoning_effort},
        {"reasoning_format", reasoning_format_name(config.reasoning_format)},
        {"https", config.https},
        {"api", api_name(config.api)},
        {"auth", auth_name(config.auth)},
        {"web_search", to_string(config.web_search)},
        {"cache_retention", cache_retention_name(config.cache_retention)},
        {"openrouter_targets", config.openrouter_targets},
        {"writable", writable},
        {"used_by", std::move(used_by)},
    };
}

Json provider_summary_json(const WorkspaceProvider& provider) {
    return {
        {"id", provider.id},
        {"display_name", provider.label},
        {"model", provider.config.model},
        {"host", provider.config.host},
    };
}

Json style_json(
    const WorkspaceStyle& style,
    bool writable,
    std::vector<std::string> used_by) {
    return {
        {"id", style.id},
        {"display_name", style.label},
        {"font", to_string(style.appearance.font)},
        {"style", to_string(style.appearance.style)},
        {"weight", to_string(style.appearance.weight)},
        {"size", to_string(style.appearance.size)},
        {"text_color", to_string(style.appearance.text_color)},
        {"writable", writable},
        {"used_by", std::move(used_by)},
    };
}

template<typename Value>
Value required(const Json& json, std::string_view name) {
    const auto found = json.find(std::string(name));
    if (found == json.end()) throw std::invalid_argument("Missing field");
    try {
        return found->get<Value>();
    } catch (const Json::exception&) {
        throw std::invalid_argument("Invalid field");
    }
}

std::optional<std::string> nullable_string(
    const Json& json,
    std::string_view name) {
    const auto found = json.find(std::string(name));
    if (found == json.end()) throw std::invalid_argument("Missing field");
    if (found->is_null()) return std::nullopt;
    if (!found->is_string()) throw std::invalid_argument("Invalid field");
    return found->get<std::string>();
}

std::optional<double> nullable_double(
    const Json& json,
    std::string_view name) {
    const auto found = json.find(std::string(name));
    if (found == json.end()) throw std::invalid_argument("Missing field");
    if (found->is_null()) return std::nullopt;
    if (!found->is_number()) throw std::invalid_argument("Invalid field");
    const double result = found->get<double>();
    if (!std::isfinite(result)) throw std::invalid_argument("Invalid field");
    return result;
}

std::optional<int> nullable_int(const Json& json, std::string_view name) {
    const auto found = json.find(std::string(name));
    if (found == json.end()) throw std::invalid_argument("Missing field");
    if (found->is_null()) return std::nullopt;
    if (!found->is_number_integer()) throw std::invalid_argument("Invalid field");
    return found->get<int>();
}

template<typename Enum>
Enum choice(
    std::string_view value,
    std::initializer_list<std::pair<std::string_view, Enum>> choices) {
    for (const auto& [name, result] : choices) {
        if (name == value) return result;
    }
    throw std::invalid_argument("Invalid choice");
}

struct ProviderUpdate {
    std::string display_name;
    ModelBackendConfig config;
};

ProviderUpdate parse_provider_update(
    const Json& json,
    const std::vector<std::string>& existing_openrouter_targets) {
    static constexpr std::size_t legacy_field_count = 19;
    const bool includes_openrouter_targets =
        json.is_object() && json.contains("openrouter_targets");
    if (!json.is_object()
        || json.size() != legacy_field_count + includes_openrouter_targets) {
        throw std::invalid_argument("Invalid provider");
    }
    ProviderUpdate result;
    result.display_name = required<std::string>(json, "display_name");
    result.config.host = required<std::string>(json, "host");
    result.config.port = required<int>(json, "port");
    result.config.base_path = required<std::string>(json, "base_path");
    result.config.mode = choice<Mode>(
        required<std::string>(json, "mode"),
        {{"net", Mode::net}, {"test", Mode::test}});
    result.config.model = required<std::string>(json, "model");
    result.config.stream = required<bool>(json, "stream");
    result.config.temperature = nullable_double(json, "temperature");
    result.config.max_tokens = nullable_int(json, "max_tokens");
    result.config.timeout_s = required<int>(json, "timeout_s");
    result.config.idle_timeout_s = required<int>(json, "idle_timeout_s");
    result.config.api_key_id = nullable_string(json, "api_key").value_or("");
    result.config.reasoning_effort = required<std::string>(json, "reasoning_effort");
    result.config.reasoning_format = choice<ReasoningFormat>(
        required<std::string>(json, "reasoning_format"),
        {{"auto", ReasoningFormat::automatic},
         {"none", ReasoningFormat::none},
         {"reasoning_content", ReasoningFormat::reasoning_content},
         {"reasoning", ReasoningFormat::reasoning}});
    result.config.https = required<bool>(json, "https");
    result.config.api = choice<ProviderApi>(
        required<std::string>(json, "api"),
        {{"chat_completions", ProviderApi::chat_completions},
         {"responses", ProviderApi::responses}});
    result.config.auth = choice<ProviderAuth>(
        required<std::string>(json, "auth"),
        {{"none", ProviderAuth::none},
         {"openai_subscription", ProviderAuth::openai_subscription}});
    result.config.web_search = choice<WebSearchMode>(
        required<std::string>(json, "web_search"),
        {{"off", WebSearchMode::off},
         {"auto", WebSearchMode::automatic},
         {"required", WebSearchMode::required}});
    result.config.cache_retention = choice<CacheRetention>(
        required<std::string>(json, "cache_retention"),
        {{"off", CacheRetention::off},
         {"short", CacheRetention::short_},
         {"long", CacheRetention::long_}});
    if (includes_openrouter_targets) {
        result.config.openrouter_targets =
            required<std::vector<std::string>>(json, "openrouter_targets");
    } else if (is_openrouter_host(result.config.host)) {
        result.config.openrouter_targets = existing_openrouter_targets;
    }
    if (!valid_openrouter_targets(result.config)) {
        throw std::invalid_argument("Invalid OpenRouter inference targets");
    }
    return result;
}

struct StyleUpdate {
    std::string display_name;
    CharacterAppearance appearance;
};

StyleUpdate parse_style_update(const Json& json) {
    if (!json.is_object() || json.size() != 6) {
        throw std::invalid_argument("Invalid style");
    }
    return {
        .display_name = required<std::string>(json, "display_name"),
        .appearance = {
            .font = choice<CharacterFont>(
                required<std::string>(json, "font"),
                {{"sans", CharacterFont::sans},
                 {"serif", CharacterFont::serif},
                 {"mono", CharacterFont::mono}}),
            .style = choice<CharacterSlant>(
                required<std::string>(json, "style"),
                {{"normal", CharacterSlant::normal},
                 {"italic", CharacterSlant::italic}}),
            .weight = choice<CharacterWeight>(
                required<std::string>(json, "weight"),
                {{"light", CharacterWeight::light},
                 {"normal", CharacterWeight::normal},
                 {"medium", CharacterWeight::medium},
                 {"semibold", CharacterWeight::semibold},
                 {"bold", CharacterWeight::bold}}),
            .size = choice<CharacterScale>(
                required<std::string>(json, "size"),
                {{"small", CharacterScale::small},
                 {"normal", CharacterScale::normal},
                 {"large", CharacterScale::large}}),
            .text_color = choice<CharacterTextColor>(
                required<std::string>(json, "text_color"),
                {{"normal", CharacterTextColor::normal},
                 {"muted", CharacterTextColor::muted},
                 {"accent", CharacterTextColor::accent}}),
        },
    };
}

std::string parse_create_name(const Json& json) {
    if (!json.is_object() || json.size() != 1) {
        throw std::invalid_argument("Invalid settings item");
    }
    return required<std::string>(json, "display_name");
}

struct ProviderCreate {
    std::string display_name;
    std::string copy_from;
};

ProviderCreate parse_provider_create(const Json& json) {
    if (!json.is_object() || json.size() != 2) {
        throw std::invalid_argument("Invalid provider");
    }
    return {
        .display_name = required<std::string>(json, "display_name"),
        .copy_from = nullable_string(json, "copy_from").value_or(""),
    };
}

bool provider_is_used(const Workspace& workspace, std::string_view provider_id) {
    for (const WorkspaceCharacter& character : workspace.characters()) {
        if (character.provider_id == provider_id) return true;
    }
    return false;
}

bool style_is_used(const Workspace& workspace, std::string_view style_id) {
    for (const WorkspaceCharacter& character : workspace.characters()) {
        if (character.style_id == style_id) return true;
    }
    return false;
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

Json key_json(const ApiKeyInfo& key, const Workspace& workspace) {
    return {
        {"id", key.id},
        {"display_name", key.display_name},
        {"has_value", key.has_value},
        {"used_by", providers_using_key(workspace, key)},
    };
}

void internal_error(httplib::Response& response, const std::exception& error) {
    set_error_response(
        response, 500, {ErrorCode::internal_error, error.what()});
}

} // namespace

SettingsRoutes::SettingsRoutes(
    LiveSessionManager& live_sessions,
    WebSettings settings,
    WorkspaceConfigStore& config,
    ApiKeyStore& api_keys,
    OpenAiOAuth& openai_auth)
    : live_sessions_(&live_sessions),
      settings_(std::move(settings)),
      config_(&config),
      api_keys_(&api_keys),
      openai_auth_(&openai_auth) {}

void SettingsRoutes::install(httplib::Server& server) const {
    LiveSessionManager* const live_sessions = live_sessions_;
    WorkspaceConfigStore* const config = config_;
    ApiKeyStore* const api_keys = api_keys_;
    OpenAiOAuth* const openai_auth = openai_auth_;
    const WebSettings settings = settings_;

    server.Get("/api/v1/providers", [](const httplib::Request&, httplib::Response& response) {
        const auto workspace = published_workspace();
        Json result = Json::array();
        for (const WorkspaceProvider& provider : workspace->providers()) {
            result.push_back(provider_summary_json(provider));
        }
        set_json_response(response, 200, result);
    });

    server.Post("/api/v1/providers", [api_keys, config, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        ProviderCreate create;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) { create = parse_provider_create(json); })) {
            return;
        }

        const auto workspace = published_workspace();
        std::string id;
        for (std::size_t suffix = 1;; ++suffix) {
            const std::string candidate = "provider_" + std::to_string(suffix);
            const std::filesystem::path directory =
                workspace->root() / "system" / "providers" / candidate;
            if (workspace->find_provider(candidate) == nullptr
                && !std::filesystem::exists(directory)) {
                id = candidate;
                break;
            }
        }
        try {
            config->apply_provider_create(
                id, create.display_name, create.copy_from);
        } catch (const std::invalid_argument&) {
            return set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid provider."});
        } catch (const WorkspaceRestartRequiredError& error) {
            return internal_error(response, error);
        }
        const auto current = published_workspace();
        const WorkspaceProvider* created = current->find_provider(id);
        if (created == nullptr) {
            return set_error_response(response, 500,
                {ErrorCode::internal_error, "The provider could not be created."});
        }
        set_json_response(response, 201, provider_json(
            *created,
            current->provider_is_writable(id),
            characters_using_provider(*current, id),
            *api_keys));
    });

    server.Get(R"(/api/v1/providers/([^/]+))", [api_keys](const httplib::Request& request, httplib::Response& response) {
        const auto workspace = published_workspace();
        const std::string id = request.matches[1];
        const WorkspaceProvider* provider = workspace->find_provider(id);
        if (!is_valid_route_component(id) || provider == nullptr) {
            return set_route_not_found(response, "That provider was not found.");
        }
        set_json_response(response, 200, provider_json(
            *provider,
            workspace->provider_is_writable(id),
            characters_using_provider(*workspace, id),
            *api_keys));
    });

    server.Post(R"(/api/v1/providers/([^/]+)/test)", [api_keys, openai_auth, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        const auto workspace = published_workspace();
        const WorkspaceProvider* provider = workspace->find_provider(id);
        if (!is_valid_route_component(id) || provider == nullptr) {
            return set_route_not_found(response, "That provider was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        ProviderUpdate candidate;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    candidate = parse_provider_update(
                        json, provider->config.openrouter_targets);
                })) {
            return;
        }
        try {
            const GenerationResult result = test_provider(
                id, std::move(candidate.config), *openai_auth, *api_keys);
            if (result.outcome != GenerationOutcome::completed) {
                const std::string reason = result.message.empty()
                    ? "The provider returned an error." : result.message;
                return set_error_response(response, 400, {
                    ErrorCode::bad_request,
                    "Provider test failed: " + reason,
                });
            }
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const std::exception& error) {
            set_error_response(response, 400, {
                ErrorCode::bad_request,
                "Provider test failed: " + std::string(error.what()),
            });
        }
    });

    server.Patch(R"(/api/v1/providers/([^/]+))", [api_keys, live_sessions, config, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        const auto workspace = published_workspace();
        const WorkspaceProvider* provider = workspace->find_provider(id);
        if (!is_valid_route_component(id) || provider == nullptr
            || !workspace->provider_is_writable(id)) {
            return set_route_not_found(response, "That provider was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        ProviderUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    update = parse_provider_update(
                        json, provider->config.openrouter_targets);
                })) {
            return;
        }
        try {
            const WorkspaceConfigEditResult edited = config->apply_provider_update(
                id, update.display_name, update.config);
            request_reload(*live_sessions, edited.affected_forum_ids);
        } catch (const std::invalid_argument&) {
            return set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid provider settings."});
        } catch (const WorkspaceRestartRequiredError& error) {
            return internal_error(response, error);
        }
        const auto current = published_workspace();
        const WorkspaceProvider* updated = current->find_provider(id);
        if (updated == nullptr) {
            return set_error_response(response, 500,
                {ErrorCode::internal_error, "The provider could not be updated."});
        }
        set_json_response(response, 200, provider_json(
            *updated,
            current->provider_is_writable(id),
            characters_using_provider(*current, id),
            *api_keys));
    });

    server.Delete(R"(/api/v1/providers/([^/]+))", [config](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        const auto workspace = published_workspace();
        if (!is_valid_route_component(id) || workspace->find_provider(id) == nullptr
            || !workspace->provider_is_writable(id)) {
            return set_route_not_found(response, "That provider was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        if (provider_is_used(*workspace, id)) {
            return set_error_response(response, 409,
                {ErrorCode::bad_request,
                 "This provider is still used by one or more characters."});
        }
        try {
            config->apply_provider_delete(id);
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const std::invalid_argument&) {
            set_error_response(response, 409,
                {ErrorCode::bad_request,
                 "This provider is still used by one or more characters."});
        } catch (const WorkspaceRestartRequiredError& error) {
            internal_error(response, error);
        }
    });

    server.Get("/api/v1/styles", [](const httplib::Request&, httplib::Response& response) {
        const auto workspace = published_workspace();
        Json result = Json::array();
        for (const WorkspaceStyle& style : workspace->styles()) {
            result.push_back(style_json(
                style,
                workspace->style_is_writable(style.id),
                characters_using_style(*workspace, style.id)));
        }
        set_json_response(response, 200, result);
    });

    server.Post("/api/v1/styles", [config, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        std::string display_name;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) { display_name = parse_create_name(json); })) {
            return;
        }

        const auto workspace = published_workspace();
        std::string id;
        for (std::size_t suffix = 1;; ++suffix) {
            const std::string candidate = "style_" + std::to_string(suffix);
            const std::filesystem::path directory =
                workspace->root() / "system" / "styles" / candidate;
            if (workspace->find_style(candidate) == nullptr
                && !std::filesystem::exists(directory)) {
                id = candidate;
                break;
            }
        }
        try {
            config->apply_style_create(id, display_name);
        } catch (const std::invalid_argument&) {
            return set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid style."});
        } catch (const WorkspaceRestartRequiredError& error) {
            return internal_error(response, error);
        }
        const auto current = published_workspace();
        const WorkspaceStyle* created = current->find_style(id);
        if (created == nullptr) {
            return set_error_response(response, 500,
                {ErrorCode::internal_error, "The style could not be created."});
        }
        set_json_response(response, 201, style_json(
            *created,
            current->style_is_writable(id),
            characters_using_style(*current, id)));
    });

    server.Patch(R"(/api/v1/styles/([^/]+))", [live_sessions, config, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        const auto workspace = published_workspace();
        if (!is_valid_route_component(id) || workspace->find_style(id) == nullptr
            || !workspace->style_is_writable(id)) {
            return set_route_not_found(response, "That style was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        StyleUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) { update = parse_style_update(json); })) {
            return;
        }
        try {
            const WorkspaceConfigEditResult edited = config->apply_style_update(
                id, update.display_name, update.appearance);
            request_reload(*live_sessions, edited.affected_forum_ids);
        } catch (const std::invalid_argument&) {
            return set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid style settings."});
        } catch (const WorkspaceRestartRequiredError& error) {
            return internal_error(response, error);
        }
        const auto current = published_workspace();
        const WorkspaceStyle* updated = current->find_style(id);
        if (updated == nullptr) {
            return set_error_response(response, 500,
                {ErrorCode::internal_error, "The style could not be updated."});
        }
        set_json_response(response, 200, style_json(
            *updated,
            current->style_is_writable(id),
            characters_using_style(*current, id)));
    });

    server.Delete(R"(/api/v1/styles/([^/]+))", [config](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        const auto workspace = published_workspace();
        if (!is_valid_route_component(id) || workspace->find_style(id) == nullptr
            || !workspace->style_is_writable(id)) {
            return set_route_not_found(response, "That style was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        if (style_is_used(*workspace, id)) {
            return set_error_response(response, 409,
                {ErrorCode::bad_request,
                 "This style is still used by one or more characters."});
        }
        try {
            config->apply_style_delete(id);
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const std::invalid_argument&) {
            set_error_response(response, 409,
                {ErrorCode::bad_request,
                 "This style is still used by one or more characters."});
        } catch (const WorkspaceRestartRequiredError& error) {
            internal_error(response, error);
        }
    });

    server.Get("/api/v1/api-keys", [api_keys](const httplib::Request&, httplib::Response& response) {
        const auto workspace = published_workspace();
        Json result = Json::array();
        for (const ApiKeyInfo& key : api_keys->list()) {
            result.push_back(key_json(key, *workspace));
        }
        set_json_response(response, 200, result);
    });

    server.Post("/api/v1/api-keys", [api_keys, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        std::string display_name;
        std::string value;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    if (!json.is_object() || json.size() != 2) {
                        throw std::invalid_argument("Invalid API key");
                    }
                    display_name = required<std::string>(json, "display_name");
                    value = required<std::string>(json, "value");
                })) return;
        try {
            const ApiKeyInfo created = api_keys->create(display_name, value);
            set_json_response(response, 201, key_json(created, *published_workspace()));
        } catch (const std::invalid_argument&) {
            set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid API key."});
        } catch (const std::exception& error) {
            internal_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/api-keys/([^/]+))", [api_keys, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        const std::string id = request.matches[1];
        std::string display_name;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    if (!json.is_object() || json.size() != 1) {
                        throw std::invalid_argument("Invalid API key");
                    }
                    display_name = required<std::string>(json, "display_name");
                })) return;
        try {
            const ApiKeyInfo updated = api_keys->rename(id, display_name);
            set_json_response(response, 200, key_json(updated, *published_workspace()));
        } catch (const std::out_of_range&) {
            set_route_not_found(response, "That API key was not found.");
        } catch (const std::invalid_argument&) {
            set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid API key."});
        } catch (const std::exception& error) {
            internal_error(response, error);
        }
    });

    server.Put(R"(/api/v1/api-keys/([^/]+)/value)", [api_keys, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        const std::string id = request.matches[1];
        std::string value;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    if (!json.is_object() || json.size() != 1) {
                        throw std::invalid_argument("Invalid API key");
                    }
                    value = required<std::string>(json, "value");
                })) return;
        try {
            const ApiKeyInfo updated = api_keys->replace(id, value);
            set_json_response(response, 200, key_json(updated, *published_workspace()));
        } catch (const std::out_of_range&) {
            set_route_not_found(response, "That API key was not found.");
        } catch (const std::invalid_argument&) {
            set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid API key."});
        } catch (const std::exception& error) {
            internal_error(response, error);
        }
    });

    server.Delete(R"(/api/v1/api-keys/([^/]+))", [api_keys](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        const std::string id = request.matches[1];
        try {
            api_keys->remove(id);
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const std::out_of_range&) {
            set_route_not_found(response, "That API key was not found.");
        } catch (const std::exception& error) {
            internal_error(response, error);
        }
    });
}

} // namespace cha::web
