#include "media/audio_download.h"
#include "storage/not_found_error.h"
#include "util/logging.h"
#include "util/path_name.h"

namespace cha {
using namespace std::chrono_literals;
namespace {
AudioAcceptance active_acceptance(EntryId id, AudioJobState state) {
    switch (state) {
    case AudioJobState::queued: return {id, AudioAcceptanceKind::queued};
    case AudioJobState::running: return {id, AudioAcceptanceKind::running};
    case AudioJobState::failed: break;
    }
    throw std::logic_error("Failed jobs cannot be accepted.");
}
}

AudioDownloadManager::Key AudioDownloadManager::key(const FullSessionId& session, EntryId id) {
    return {session.forum_id, session.session_id, id};
}

AudioDownloadManager::AudioDownloadManager(const SessionRepository& sessions,
    ActiveVaultName active_vault_name, bool enabled, Transport transport)
    : sessions_(sessions), active_vault_name_(std::move(active_vault_name)),
      enabled_(enabled), transport_(std::move(transport)) {
    try {
        for (auto& thread : workers_) {
            thread = std::thread([this] { worker(); });
        }
    } catch (...) {
        request_stop();
        for (auto& thread : workers_) {
            if (thread.joinable()) thread.join();
        }
        throw;
    }
}
AudioDownloadManager::~AudioDownloadManager() {
    request_stop();
    for (auto& thread : workers_) {
        if (thread.joinable()) thread.join();
    }
}
void AudioDownloadManager::check(const FullSessionId& session, const std::string& vault) const {
    if (!is_url_safe_identifier(session.forum_id)
        || !is_url_safe_identifier(session.session_id)) {
        throw std::invalid_argument("Invalid session identity.");
    }
    if (vault != active_vault_name_()) throw AudioDownloadError(409, "vault_changed", "The active vault changed.");
    if (!enabled_) throw AudioDownloadError(404, "not_found", "Voice output is not available.");
    if (stopped_ || paused_ || clearing_.contains({session.forum_id, session.session_id})) {
        throw AudioDownloadError(503, "speech_busy", "Audio downloads are temporarily unavailable.");
    }
}
void AudioDownloadManager::check_generation(const FullSessionId& session, const std::string& vault, std::size_t generation) const {
    if (generation != generation_) throw AudioDownloadError(409, "vault_changed", "The active vault changed.");
    check(session, vault);
}

AudioAcceptance AudioDownloadManager::submit(const FullSessionId& session, EntryId id, const AudioDownloadRequest& input) {
    const auto& vault = input.vault_name;
    const auto identity = key(session, id);
    std::size_t generation;
    {
        std::lock_guard lock(mutex_);
        check(session, vault);
        generation = generation_;
        if (auto it = jobs_.find(identity); it != jobs_.end() && it->second->state != AudioJobState::failed) {
            return active_acceptance(id, it->second->state);
        }
    }
    std::exception_ptr failure;
    std::optional<EntryAudioLookup> entry;
    auto job = std::make_shared<Job>();
    try {
        entry = sessions_.lookup_entry_audio(session, id, false);
        if (!entry) throw AudioDownloadError(404, "not_found", "Transcript entry not found.");
        if (!entry->has_cached_audio) {
            job = prepare_job(*entry, input.synthesis);
        }
    } catch (...) {
        failure = std::current_exception();
    }
    std::lock_guard lock(mutex_);
    check_generation(session, vault, generation);
    if (auto it = jobs_.find(identity); it != jobs_.end() && it->second->state != AudioJobState::failed) {
        return active_acceptance(id, it->second->state);
    }
    if (failure) std::rethrow_exception(failure);
    if (entry->has_cached_audio) return {id, AudioAcceptanceKind::cached};
    jobs_[identity] = job;
    queue_.push_back(job);
    // Retry waits share this condition variable with idle workers.
    changed_.notify_all();
    return {id, AudioAcceptanceKind::queued};
}

std::shared_ptr<AudioDownloadManager::Job> AudioDownloadManager::prepare_job(
    const EntryAudioLookup& entry, const FishAudioSynthesis& synthesis) {
    auto job = std::make_shared<Job>();
    const auto workspace = sessions_.workspace();
    if (!workspace || !workspace->voice_output()) {
        throw AudioDownloadError(404, "not_found", "Voice output is not configured.");
    }
    job->entry = entry;
    job->output = *workspace->voice_output();
    const auto* credential = workspace->find_api_key(job->output.api_key_id);
    if (!credential) {
        throw AudioDownloadError(404, "not_found", "Voice output is not configured.");
    }
    job->key = credential->value;
    job->request = make_fish_audio_request(job->output, entry_speech_text(entry), synthesis);
    return job;
}

std::vector<AudioAcceptance> AudioDownloadManager::submit_batch(
    const FullSessionId& session, const AudioDownloadBatchRequest& input) {
    const auto& vault = input.vault_name;
    std::size_t generation;
    {
        std::lock_guard lock(mutex_);
        check(session, vault);
        generation = generation_;
    }
    struct Prepared {
        EntryId id;
        AudioAcceptance acceptance;
        std::shared_ptr<Job> job;
    };
    std::vector<Prepared> prepared;
    // Validate and prepare the entire batch before admitting new work.
    for (const auto& request : input.entries) {
        const auto id = request.entry_id;
        {
            std::lock_guard lock(mutex_);
            check_generation(session, vault, generation);
            const auto it = jobs_.find(key(session, id));
            if (it != jobs_.end() && it->second->state != AudioJobState::failed) {
                prepared.push_back({id, active_acceptance(id, it->second->state), {}});
                continue;
            }
        }
        const auto entry = sessions_.lookup_entry_audio(session, id, false);
        if (!entry) throw AudioDownloadError(404, "not_found", "Transcript entry not found.");
        if (entry->has_cached_audio) {
            prepared.push_back({id, {id, AudioAcceptanceKind::cached}, {}});
        } else {
            prepared.push_back({id, {id, AudioAcceptanceKind::queued}, prepare_job(*entry, request.synthesis)});
        }
    }
    std::vector<AudioAcceptance> accepted;
    {
        std::lock_guard lock(mutex_);
        check_generation(session, vault, generation);
        for (auto& item : prepared) {
            const auto identity = key(session, item.id);
            const auto it = jobs_.find(identity);
            if (it != jobs_.end() && it->second->state != AudioJobState::failed) {
                accepted.push_back(active_acceptance(item.id, it->second->state));
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
    return accepted;
}

AudioDownloadStatus AudioDownloadManager::status(const FullSessionId& session, const std::string& vault) {
    // Clients poll status on every session, so a runtime without downloads reports nothing.
    if (!enabled_) return {};
    std::size_t generation;
    std::vector<AudioDownloadJob> downloads;
    {
        std::lock_guard lock(mutex_);
        check(session, vault);
        generation = generation_;
        for (const auto& [identity, job] : jobs_) {
            if (job->entry.identity == session) {
                downloads.push_back({job->entry.entry_id, job->state});
            }
        }
    }
    std::set<EntryId> cached;
    std::exception_ptr failure;
    try {
        cached = sessions_.cached_audio_entries(session);
    } catch (...) {
        failure = std::current_exception();
    }
    {
        std::lock_guard lock(mutex_);
        check_generation(session, vault, generation);
    }
    if (failure) std::rethrow_exception(failure);
    std::erase_if(downloads, [&](const auto& download) { return cached.contains(download.entry_id); });
    return {std::move(cached), std::move(downloads)};
}
std::optional<EntryAudio> AudioDownloadManager::audio(const FullSessionId& session, EntryId id, const std::string& vault) {
    std::size_t generation;
    {
        std::lock_guard lock(mutex_);
        check(session, vault);
        generation = generation_;
    }
    std::optional<EntryAudioLookup> entry;
    std::exception_ptr failure;
    try {
        entry = sessions_.lookup_entry_audio(session, id);
    } catch (...) {
        failure = std::current_exception();
    }
    {
        std::lock_guard lock(mutex_);
        check_generation(session, vault, generation);
    }
    if (failure) std::rethrow_exception(failure);
    return entry ? std::move(entry->cached) : std::nullopt;
}
void AudioDownloadManager::clear(const FullSessionId& session) {
    {
        std::lock_guard lock(mutex_);
        if (paused_ || stopped_ || !clearing_.insert({session.forum_id, session.session_id}).second) {
            throw AudioDownloadError(503, "speech_busy", "Audio downloads are temporarily unavailable.");
        }
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->second->entry.identity == session) {
                it->second->cancelled = true;
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
        std::erase_if(queue_, [](const auto& job) { return job->cancelled.load(); });
        changed_.notify_all();
    }
    std::exception_ptr failure;
    try {
        sessions_.clear_session_audio(session);
    } catch (...) {
        failure = std::current_exception();
    }
    {
        std::lock_guard lock(mutex_);
        clearing_.erase({session.forum_id, session.session_id});
    }
    if (failure) std::rethrow_exception(failure);
}
void AudioDownloadManager::cancel_all() {
    for (auto& [identity, job] : jobs_) {
        job->cancelled = true;
    }
    jobs_.clear();
    queue_.clear();
    ++generation_;
    changed_.notify_all();
}
void AudioDownloadManager::pause(bool cancel) {
    std::lock_guard lock(mutex_);
    paused_ = true;
    if (cancel) cancel_all();
}
void AudioDownloadManager::resume() {
    std::lock_guard lock(mutex_);
    paused_ = false;
}
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
    for (auto& thread : workers_) {
        if (thread.joinable()) thread.join();
    }
    return true;
}
void AudioDownloadManager::worker() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
            if (stopped_) break;
            job = queue_.front();
            queue_.pop_front();
            job->state = AudioJobState::running;
        }
        bool failed = false;
        try {
            run(job);
        } catch (const std::exception& error) {
            log_warn(error.what());
            failed = true;
        }
        {
            std::lock_guard lock(mutex_);
            const auto it = jobs_.find(key(job->entry.identity, job->entry.entry_id));
            if (it != jobs_.end() && it->second == job) {
                if (failed && !job->cancelled) {
                    job->state = AudioJobState::failed;
                } else {
                    jobs_.erase(it);
                }
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
        try {
            entry = sessions_.lookup_entry_audio(job->entry.identity, job->entry.entry_id, false);
        } catch (const SessionNotFoundError&) {
            return;
        } catch (const ForumNotFoundError&) {
            return;
        }
        if (!entry || entry->session_key != job->entry.session_key || entry->entry_text != job->entry.entry_text
            || entry->database_path != job->entry.database_path || cancelled() || entry->has_cached_audio) {
            return;
        }
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
        try {
            sessions_.save_entry_audio(job->entry, *result, cancelled);
        } catch (const SessionNotFoundError&) {
            return;
        } catch (const ForumNotFoundError&) {
            return;
        }
        return;
    }
}

}
