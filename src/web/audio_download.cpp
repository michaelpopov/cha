#include "web/audio_download.h"
#include "web/current_vault.h"
#include "web/http_response.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "web/web_settings.h"
#include "session/not_found_error.h"
#include "util/logging.h"
#include <httplib.h>

namespace cha::web {
using Json = nlohmann::json;
using namespace std::chrono_literals;

AudioDownloadManager::Key AudioDownloadManager::key(const FullSessionId& s, EntryId id) {
    return {s.forum_id, s.session_id, id};
}

AudioDownloadManager::AudioDownloadManager(const SessionRepository& sessions,
    CurrentVault& vault, bool enabled, Transport transport)
    : sessions_(sessions), vault_(vault), enabled_(enabled), transport_(std::move(transport)) {
    try {
        for (auto& thread : workers_) thread = std::thread([this] { worker(); });
    } catch (...) {
        request_stop();
        for (auto& thread : workers_) if (thread.joinable()) thread.join();
        throw;
    }
}
AudioDownloadManager::~AudioDownloadManager() {
    request_stop();
    for (auto& thread : workers_) if (thread.joinable()) thread.join();
}
void AudioDownloadManager::check(const FullSessionId& session, const std::string& vault) const {
    if (!is_valid_route_component(session.forum_id) || !is_valid_route_component(session.session_id))
        throw std::invalid_argument("Invalid session identity.");
    if (vault != vault_.get().name) throw AudioDownloadError(409, "vault_changed", "The active vault changed.");
    if (!enabled_) throw AudioDownloadError(404, "not_found", "Voice output is not available.");
    if (stopped_ || paused_ || clearing_.contains({session.forum_id, session.session_id}))
        throw AudioDownloadError(503, "speech_busy", "Audio downloads are temporarily unavailable.");
}
void AudioDownloadManager::check_generation(const FullSessionId& s, const std::string& vault, std::size_t generation) const {
    if (generation != generation_) throw AudioDownloadError(409, "vault_changed", "The active vault changed.");
    check(s, vault);
}

Json AudioDownloadManager::submit(const FullSessionId& s, EntryId id, const Json& input) {
    const auto vault = input.at("vault_name").get<std::string>();
    const auto identity = key(s, id);
    std::size_t generation;
    {
        std::lock_guard lock(mutex_);
        check(s, vault);
        generation = generation_;
        if (auto it = jobs_.find(identity); it != jobs_.end() && it->second->state != "failed")
            return {{"entry_id", id}, {"cached", false}, {"state", it->second->state}};
    }
    std::exception_ptr failure;
    std::optional<EntryAudioLookup> entry;
    auto job = std::make_shared<Job>();
    try {
        entry = sessions_.lookup_entry_audio(s, id, false);
        if (!entry) throw AudioDownloadError(404, "not_found", "Transcript entry not found.");
        if (!entry->has_cached_audio) {
            job = prepare_job(*entry, input);
        }
    } catch (...) { failure = std::current_exception(); }
    std::lock_guard lock(mutex_);
    check_generation(s, vault, generation);
    if (auto it = jobs_.find(identity); it != jobs_.end() && it->second->state != "failed")
        return {{"entry_id", id}, {"cached", false}, {"state", it->second->state}};
    if (failure) std::rethrow_exception(failure);
    if (entry->has_cached_audio) return {{"entry_id", id}, {"cached", true}};
    jobs_[identity] = job;
    queue_.push_back(job);
    // Retry waits share this condition variable with idle workers.
    changed_.notify_all();
    return {{"entry_id", id}, {"cached", false}, {"state", "queued"}};
}

std::shared_ptr<AudioDownloadManager::Job> AudioDownloadManager::prepare_job(
    const EntryAudioLookup& entry, const Json& input) {
    auto job = std::make_shared<Job>();
    const auto workspace = getws();
    if (!workspace || !workspace->voice_output())
        throw AudioDownloadError(404, "not_found", "Voice output is not configured.");
    job->entry = entry;
    job->output = *workspace->voice_output();
    const auto* key = workspace->find_api_key(job->output.api_key_id);
    if (!key) throw AudioDownloadError(404, "not_found", "Voice output is not configured.");
    job->key = key->value;
    Json synthesis = input;
    synthesis["text"] = entry_speech_text(entry);
    job->request = make_fish_audio_request(job->output, synthesis);
    return job;
}

Json AudioDownloadManager::submit_batch(const FullSessionId& s, const Json& input) {
    const auto vault = input.at("vault_name").get<std::string>();
    std::size_t generation;
    {
        std::lock_guard lock(mutex_);
        check(s, vault);
        generation = generation_;
    }
    struct Prepared {
        EntryId id;
        Json acceptance;
        std::shared_ptr<Job> job;
    };
    std::vector<Prepared> prepared;
    // Validate and prepare the entire batch before admitting new work.
    for (const auto& request : input.at("entries")) {
        const auto id = request.at("entry_id").get<EntryId>();
        {
            std::lock_guard lock(mutex_);
            check_generation(s, vault, generation);
            const auto it = jobs_.find(key(s, id));
            if (it != jobs_.end() && it->second->state != "failed") {
                prepared.push_back({id, {{"entry_id", id}, {"cached", false}, {"state", it->second->state}}, {}});
                continue;
            }
        }
        const auto entry = sessions_.lookup_entry_audio(s, id, false);
        if (!entry) throw AudioDownloadError(404, "not_found", "Transcript entry not found.");
        if (entry->has_cached_audio) prepared.push_back({id, {{"entry_id", id}, {"cached", true}}, {}});
        else prepared.push_back({id, {{"entry_id", id}, {"cached", false}, {"state", "queued"}}, prepare_job(*entry, request)});
    }
    Json accepted = Json::array();
    {
        std::lock_guard lock(mutex_);
        check_generation(s, vault, generation);
        for (auto& item : prepared) {
            const auto identity = key(s, item.id);
            const auto it = jobs_.find(identity);
            if (it != jobs_.end() && it->second->state != "failed") {
                accepted.push_back({{"entry_id", item.id}, {"cached", false}, {"state", it->second->state}});
            } else {
                if (item.job) {
                    jobs_[identity] = item.job;
                    queue_.push_back(item.job);
                }
                accepted.push_back(std::move(item.acceptance));
            }
        }
        changed_.notify_all();
    }
    return {{"entries", std::move(accepted)}};
}

Json AudioDownloadManager::status(const FullSessionId& s, const std::string& vault) {
    // Clients poll status on every session, so a runtime without downloads reports nothing.
    if (!enabled_) return {{"cached_entry_ids", Json::array()}, {"downloads", Json::array()}};
    std::size_t generation;
    Json downloads = Json::array();
    {
        std::lock_guard lock(mutex_);
        check(s, vault);
        generation = generation_;
        for (const auto& [k, job] : jobs_) if (job->entry.identity == s) {
            Json value{{"entry_id", job->entry.entry_id}, {"state", job->state}};
            if (job->state == "failed") value["error"] = "Audio download failed. Try again.";
            downloads.push_back(std::move(value));
        }
    }
    std::set<EntryId> cached;
    std::exception_ptr failure;
    try { cached = sessions_.cached_audio_entries(s); } catch (...) { failure = std::current_exception(); }
    {
        std::lock_guard lock(mutex_);
        check_generation(s, vault, generation);
    }
    if (failure) std::rethrow_exception(failure);
    Json pending = Json::array();
    for (auto& d : downloads) if (!cached.contains(d.at("entry_id").get<EntryId>())) pending.push_back(std::move(d));
    return {{"cached_entry_ids", cached}, {"downloads", std::move(pending)}};
}
std::optional<EntryAudio> AudioDownloadManager::audio(const FullSessionId& s, EntryId id, const std::string& vault) {
    std::size_t generation;
    {
        std::lock_guard lock(mutex_);
        check(s, vault);
        generation = generation_;
    }
    std::optional<EntryAudioLookup> entry;
    std::exception_ptr failure;
    try { entry = sessions_.lookup_entry_audio(s, id); } catch (...) { failure = std::current_exception(); }
    {
        std::lock_guard lock(mutex_);
        check_generation(s, vault, generation);
    }
    if (failure) std::rethrow_exception(failure);
    return entry ? std::move(entry->cached) : std::nullopt;
}
void AudioDownloadManager::clear(const FullSessionId& s) {
    {
        std::lock_guard lock(mutex_);
        if (paused_ || stopped_ || !clearing_.insert({s.forum_id, s.session_id}).second)
            throw AudioDownloadError(503, "speech_busy", "Audio downloads are temporarily unavailable.");
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->second->entry.identity == s) { it->second->cancelled = true; it = jobs_.erase(it); }
            else ++it;
        }
        std::erase_if(queue_, [](const auto& job) { return job->cancelled.load(); });
        changed_.notify_all();
    }
    std::exception_ptr failure;
    try { sessions_.clear_session_audio(s); } catch (...) { failure = std::current_exception(); }
    {
        std::lock_guard lock(mutex_);
        clearing_.erase({s.forum_id, s.session_id});
    }
    if (failure) std::rethrow_exception(failure);
}
void AudioDownloadManager::cancel_all() {
    for (auto& [k, job] : jobs_) job->cancelled = true;
    jobs_.clear();
    queue_.clear();
    ++generation_;
    changed_.notify_all();
}
void AudioDownloadManager::pause(bool cancel) {
    std::lock_guard lock(mutex_); paused_ = true; if (cancel) cancel_all();
}
void AudioDownloadManager::resume() { std::lock_guard lock(mutex_); paused_ = false; }
void AudioDownloadManager::request_stop() {
    std::lock_guard lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    cancel_all();
}
bool AudioDownloadManager::join_until(std::chrono::steady_clock::time_point deadline) {
    {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_until(lock, deadline, [this] { return exited_ == workers_.size(); })) return false;
    }
    for (auto& thread : workers_) if (thread.joinable()) thread.join();
    return true;
}
void AudioDownloadManager::worker() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
            if (stopped_) break;
            job = queue_.front(); queue_.pop_front();
            job->state = "running";
        }
        bool failed = false;
        try { run(job); } catch (const std::exception& e) { log_warn(e.what()); failed = true; }
        {
            std::lock_guard lock(mutex_);
            const auto it = jobs_.find(key(job->entry.identity, job->entry.entry_id));
            if (it != jobs_.end() && it->second == job) {
                if (failed && !job->cancelled) job->state = "failed";
                else jobs_.erase(it);
            }
        }
    }
    std::lock_guard lock(mutex_);
    ++exited_;
    changed_.notify_all();
}
void AudioDownloadManager::run(const std::shared_ptr<Job>& job) {
    const auto cancelled = [&] { return job->cancelled.load(); };
    for (int attempt = 0; attempt < 4 && !cancelled(); ++attempt) {
        std::optional<EntryAudio> result;
        std::optional<EntryAudioLookup> entry;
        try { entry = sessions_.lookup_entry_audio(job->entry.identity, job->entry.entry_id, false); }
        catch (const SessionNotFoundError&) { return; }
        catch (const ForumNotFoundError&) { return; }
        if (!entry || entry->session_key != job->entry.session_key || entry->entry_text != job->entry.entry_text
            || entry->database_path != job->entry.database_path || cancelled() || entry->has_cached_audio) return;
        try {
            result = transport_(job->output, job->key, job->request, cancelled);
            if (!result || cancelled()) return;
            if (!valid_entry_audio(*result)) throw std::runtime_error("FishAudio returned invalid audio.");
        } catch (const std::exception&) {
            if (cancelled()) return;
            if (attempt == 3) throw;
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, 50ms, cancelled);
            continue;
        }
        // Storage failures are terminal, outside the download retry loop.
        try { sessions_.save_entry_audio(job->entry, *result, cancelled); }
        catch (const SessionNotFoundError&) { return; }
        catch (const ForumNotFoundError&) { return; }
        return;
    }
}

void install_audio_download_routes(httplib::Server& server, AudioDownloadManager& downloads, const WebSettings& settings) {
    const std::string base = R"(/api/v1/forums/([^/]+)/sessions/([^/]+))";
    const auto handle = [](httplib::Response& response, const auto& action) {
        try { action(); }
        catch (const AudioDownloadError& e) {
            set_error_response(response, e.status, {e.code == "vault_changed" ? ErrorCode::vault_changed
                : e.code == "not_found" ? ErrorCode::not_found : ErrorCode::speech_busy, e.what()});
        } catch (const SessionNotFoundError&) { set_route_not_found(response);
        } catch (const ForumNotFoundError&) { set_route_not_found(response);
        } catch (const Json::exception&) { set_error_response(response, 400, {ErrorCode::bad_request, "Invalid audio request."});
        } catch (const std::invalid_argument& e) { set_error_response(response, 400, {ErrorCode::bad_request, e.what()});
        } catch (const std::out_of_range&) { set_error_response(response, 400, {ErrorCode::bad_request, "Invalid entry ID."});
        } catch (const std::exception& e) { log_warn(e.what()); set_error_response(response, 500, {ErrorCode::internal_error, "Audio request failed."}); }
    };
    server.Post(base + R"(/entries/([1-9][0-9]*)/audio-download)", [&downloads, settings, handle](const auto& request, auto& response) {
        if (!validate_json_mutation(request, response)) return;
        Json input;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [&](const Json& parsed) {
            if (!parsed.is_object()) throw std::invalid_argument("Invalid audio request.");
            for (const auto& [name, v] : parsed.items())
                if (name != "vault_name" && name != "reference_id" && name != "settings") throw std::invalid_argument("Invalid audio request.");
            input = parsed;
        })) return;
        handle(response, [&] {
            auto value = downloads.submit({request.matches[1], request.matches[2]}, std::stoull(request.matches[3]), input);
            set_json_response(response, value.at("cached").template get<bool>() ? 200 : 202, value);
        });
    });
    server.Post(base + "/audio-downloads", [&downloads, settings, handle](const auto& request, auto& response) {
        if (!validate_json_mutation(request, response)) return;
        Json input;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [&](const Json& parsed) {
            if (!parsed.is_object() || parsed.size() != 2 || !parsed.contains("vault_name")
                || !parsed.at("vault_name").is_string() || parsed.at("vault_name").get<std::string>().empty()
                || !parsed.contains("entries") || !parsed.at("entries").is_array() || parsed.at("entries").empty())
                throw std::invalid_argument("Invalid audio batch request.");
            std::set<EntryId> ids;
            for (const auto& entry : parsed.at("entries")) {
                if (!entry.is_object() || !entry.contains("entry_id") || !entry.at("entry_id").is_number_unsigned()
                    || entry.at("entry_id").get<EntryId>() == 0 || !ids.insert(entry.at("entry_id").get<EntryId>()).second
                    || !entry.contains("reference_id") || !entry.at("reference_id").is_string()
                    || entry.at("reference_id").get<std::string>().empty())
                    throw std::invalid_argument("Invalid audio batch entry.");
                for (const auto& [name, value] : entry.items())
                    if (name != "entry_id" && name != "reference_id" && name != "settings") throw std::invalid_argument("Invalid audio batch entry.");
            }
            input = parsed;
        })) return;
        handle(response, [&] { set_json_response(response, 202, downloads.submit_batch({request.matches[1], request.matches[2]}, input)); });
    });
    server.Get(base + "/audio-downloads", [&downloads, handle](const auto& request, auto& response) {
        handle(response, [&] { set_json_response(response, 200, downloads.status({request.matches[1], request.matches[2]}, request.get_param_value("vault_name"))); });
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
