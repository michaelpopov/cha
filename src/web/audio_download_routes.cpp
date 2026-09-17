#include "web/audio_download_routes.h"
#include "web/audio_download.h"
#include "web/http_response.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "web/web_settings.h"
#include "session/not_found_error.h"
#include "util/logging.h"
#include <httplib.h>

namespace cha::web {
namespace {
using Json = nlohmann::json;

const char* job_state_string(AudioJobState state) {
    switch (state) {
    case AudioJobState::queued: return "queued";
    case AudioJobState::running: return "running";
    case AudioJobState::failed: return "failed";
    }
    throw std::logic_error("Invalid audio job state.");
}

Json acceptance_json(const AudioAcceptance& acceptance) {
    Json value{{"entry_id", acceptance.entry_id}};
    switch (acceptance.kind) {
    case AudioAcceptanceKind::cached:
        value["cached"] = true;
        break;
    case AudioAcceptanceKind::queued:
        value["cached"] = false;
        value["state"] = "queued";
        break;
    case AudioAcceptanceKind::running:
        value["cached"] = false;
        value["state"] = "running";
        break;
    }
    return value;
}
}

void install_audio_download_routes(httplib::Server& server, AudioDownloadManager& downloads, const WebSettings& settings) {
    const std::string base = R"(/api/v1/forums/([^/]+)/sessions/([^/]+))";
    const auto handle = [](httplib::Response& response, const auto& action) {
        try {
            action();
        } catch (const AudioDownloadError& error) {
            const auto code = error.code == "vault_changed" ? ErrorCode::vault_changed
                : error.code == "not_found" ? ErrorCode::not_found : ErrorCode::speech_busy;
            set_error_response(response, error.status, {code, error.what()});
        } catch (const SessionNotFoundError&) {
            set_route_not_found(response);
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        } catch (const Json::exception&) {
            set_error_response(response, 400, {ErrorCode::bad_request, "Invalid audio request."});
        } catch (const std::invalid_argument& error) {
            set_error_response(response, 400, {ErrorCode::bad_request, error.what()});
        } catch (const std::out_of_range&) {
            set_error_response(response, 400, {ErrorCode::bad_request, "Invalid entry ID."});
        } catch (const std::exception& error) {
            log_warn(error.what());
            set_error_response(response, 500, {ErrorCode::internal_error, "Audio request failed."});
        }
    };
    server.Post(base + R"(/entries/([1-9][0-9]*)/audio-download)", [&downloads, settings, handle](const auto& request, auto& response) {
        if (!validate_json_mutation(request, response)) return;
        Json input;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [&](const Json& parsed) {
            if (!parsed.is_object()) throw std::invalid_argument("Invalid audio request.");
            for (const auto& [name, value] : parsed.items()) {
                if (name != "vault_name" && name != "reference_id" && name != "settings") {
                    throw std::invalid_argument("Invalid audio request.");
                }
            }
            input = parsed;
        })) return;
        handle(response, [&] {
            const auto entry_id = std::stoull(request.matches[3]);
            const AudioDownloadRequest decoded{
                input.at("vault_name").get<std::string>(), decode_fish_audio_synthesis(input)};
            auto value = downloads.submit({request.matches[1], request.matches[2]}, entry_id, decoded);
            set_json_response(response, value.kind == AudioAcceptanceKind::cached ? 200 : 202, acceptance_json(value));
        });
    });
    server.Post(base + "/audio-downloads", [&downloads, settings, handle](const auto& request, auto& response) {
        if (!validate_json_mutation(request, response)) return;
        AudioDownloadBatchRequest input;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [&](const Json& parsed) {
            if (!parsed.is_object() || parsed.size() != 2 || !parsed.contains("vault_name")
                || !parsed.at("vault_name").is_string() || parsed.at("vault_name").get<std::string>().empty()
                || !parsed.contains("entries") || !parsed.at("entries").is_array() || parsed.at("entries").empty()) {
                throw std::invalid_argument("Invalid audio batch request.");
            }
            std::set<EntryId> ids;
            for (const auto& entry : parsed.at("entries")) {
                if (!entry.is_object() || !entry.contains("entry_id") || !entry.at("entry_id").is_number_unsigned()
                    || entry.at("entry_id").get<EntryId>() == 0 || !ids.insert(entry.at("entry_id").get<EntryId>()).second
                    || !entry.contains("reference_id") || !entry.at("reference_id").is_string()
                    || entry.at("reference_id").get<std::string>().empty()) {
                    throw std::invalid_argument("Invalid audio batch entry.");
                }
                for (const auto& [name, value] : entry.items()) {
                    if (name != "entry_id" && name != "reference_id" && name != "settings") {
                        throw std::invalid_argument("Invalid audio batch entry.");
                    }
                }
            }
            input.vault_name = parsed.at("vault_name").get<std::string>();
            for (const auto& entry : parsed.at("entries")) {
                input.entries.push_back({entry.at("entry_id").get<EntryId>(), decode_fish_audio_synthesis(entry)});
            }
        })) return;
        handle(response, [&] {
            Json entries = Json::array();
            for (const auto& acceptance : downloads.submit_batch({request.matches[1], request.matches[2]}, input)) {
                entries.push_back(acceptance_json(acceptance));
            }
            set_json_response(response, 202, {{"entries", std::move(entries)}});
        });
    });
    server.Get(base + "/audio-downloads", [&downloads, handle](const auto& request, auto& response) {
        handle(response, [&] {
            const auto status = downloads.status({request.matches[1], request.matches[2]}, request.get_param_value("vault_name"));
            Json pending = Json::array();
            for (const auto& download : status.downloads) {
                Json value{{"entry_id", download.entry_id}, {"state", job_state_string(download.state)}};
                if (download.state == AudioJobState::failed) {
                    value["error"] = "Audio download failed. Try again.";
                }
                pending.push_back(std::move(value));
            }
            set_json_response(response, 200, {{"cached_entry_ids", status.cached_entry_ids}, {"downloads", std::move(pending)}});
        });
    });
    server.Get(base + R"(/entries/([1-9][0-9]*)/audio)", [&downloads, handle](const auto& request, auto& response) {
        handle(response, [&] {
            auto audio = downloads.audio({request.matches[1], request.matches[2]}, std::stoull(request.matches[3]), request.get_param_value("vault_name"));
            if (!audio) return set_route_not_found(response, "Cached audio not found.");
            response.set_content(std::move(audio->audio), audio->content_type);
            response.set_header("Cache-Control", "no-store");
        });
    });
}
}
