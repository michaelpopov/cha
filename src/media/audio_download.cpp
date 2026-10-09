#include "media/audio_download.h"
#include "providers/voice_output_config.h"
#include "storage/not_found_error.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <algorithm>

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
    ActiveVaultName active_vault_name, bool enabled, Transport transport, WebSocketTransport websocket)
    : sessions_(sessions), active_vault_name_(std::move(active_vault_name)),
      enabled_(enabled), transport_(std::move(transport)), websocket_(std::move(websocket)) {
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

std::optional<EntryAudioLookup> AudioDownloadManager::lookup(
    const FullSessionId& session, EntryId id, const TranscriptEntry* live) const {
    auto entry = sessions_.lookup_entry_audio(session, id, false);
    if (entry) return entry;
    if (!live) return {};
    const auto prepared = sessions_.prepare(session);
    return EntryAudioLookup{.session_key = prepared.session_key, .entry_id = id,
        .database_path = prepared.database_path, .identity = session, .entry_text = live->text,
        .entry_kind = live->kind, .participant_id = live->participant_id};
}

void AudioDownloadManager::update_live(const FullSessionId& session, std::span<const TranscriptEntry> entries) {
    std::lock_guard lock(mutex_);
    for (auto& [identity, job] : jobs_) {
        if (!job->live || job->text_complete || job->entry.identity != session || job->cancelled) continue;
        const auto found = std::ranges::find(entries, job->entry.entry_id, &TranscriptEntry::id);
        if (found == entries.end() || found->request_id != job->request_id || found->created_at != job->created_at
            || found->kind != EntryKind::character || found->participant_id != job->entry.participant_id
            || found->status == EntryStatus::cancelled || found->status == EntryStatus::failed
            || !found->text.starts_with(job->entry.entry_text)) {
            job->cancelled = true;
            job->stream->fail();
        } else {
            job->entry.entry_text = found->text;
            job->text_complete = found->status == EntryStatus::complete;
        }
    }
    changed_.notify_all();
}

std::shared_ptr<AudioDownloadManager::Job> AudioDownloadManager::prepare_job(
    const EntryAudioLookup& entry, const VoiceSynthesis& synthesis, const TranscriptEntry* live) {
    if (entry.entry_kind != EntryKind::character) {
        throw std::invalid_argument("Voice output is only available for character replies.");
    }
    if (synthesis.decoding_failure) std::rethrow_exception(synthesis.decoding_failure);
    validate_voice_output_provider(synthesis.provider);
    auto job = std::make_shared<Job>();
    const auto workspace = sessions_.workspace();
    if (!workspace || !workspace->voice_output()) {
        throw AudioDownloadError(404, "not_found", "Voice output is not configured.");
    }
    job->entry = entry;
    const auto& output = *workspace->voice_output();
    auto resolved = synthesis;
    if (!resolved.reference_id) {
        const auto* character = workspace->find_character(entry.participant_id);
        const auto* voice = character && character->voice_id
            ? workspace->find_voice(*character->voice_id) : nullptr;
        if (voice) {
            resolved.reference_id = voice->elevenlabs_voice_id;
            resolved.provider = voice->provider;
            resolved.settings = voice->settings;
        } else if (const auto* fallback = workspace->find_voice_by_name(output.default_voice)) {
            resolved.reference_id = fallback->elevenlabs_voice_id;
            resolved.provider = fallback->provider;
            resolved.settings = fallback->settings;
        } else {
            throw AudioDownloadError(404, "not_found", "Voice output is not configured.");
        }
    }
    try { job->output = select_voice_output(output, resolved.provider); }
    catch (const std::invalid_argument& error) { throw AudioDownloadError(404, "not_found", error.what()); }
    const auto* credential = workspace->find_api_key(job->output.api_key_id);
    if (!credential) throw AudioDownloadError(404, "not_found", "Voice output is not configured.");
    job->key = credential->value;
    if (live) {
        if (job->output.connection != "websocket")
            throw std::invalid_argument("HTTP speech requires a completed reply.");
        job->live = true;
        job->text_complete = false;
        job->request_id = live->request_id;
        job->created_at = live->created_at;
        job->request = make_voice_output_request(job->output, std::nullopt, resolved);
    } else {
        job->request = make_voice_output_request(job->output, entry_speech_text(entry), resolved);
    }
    return job;
}

std::vector<AudioAcceptance> AudioDownloadManager::submit_batch(
    const FullSessionId& session, const AudioDownloadBatchRequest& input, std::span<const TranscriptEntry> live) {
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
        const auto found = std::ranges::find(live, id, &TranscriptEntry::id);
        const auto* streaming = found != live.end() && found->status == EntryStatus::streaming ? &*found : nullptr;
        const auto entry = lookup(session, id, streaming);
        if (!entry) throw AudioDownloadError(404, "not_found", "Transcript entry not found.");
        if (entry->has_cached_audio) {
            prepared.push_back({id, {id, AudioAcceptanceKind::cached}, {}});
        } else {
            auto job = prepare_job(*entry, request.synthesis, streaming);
            prepared.push_back({id, {id, AudioAcceptanceKind::queued}, std::move(job)});
        }
    }
    std::vector<AudioAcceptance> accepted;
    std::vector<std::shared_ptr<Job>> new_jobs;
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
                    new_jobs.push_back(item.job);
                }
                accepted.push_back(std::move(item.acceptance));
            }
        }
        // New replies should not wait behind an old conversation's cache backlog.
        // Keep the order within each batch and leave running downloads alone.
        queue_.insert(queue_.begin(), new_jobs.begin(), new_jobs.end());
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
std::shared_ptr<AudioStream> AudioDownloadManager::stream(
    const FullSessionId& session, EntryId id, const std::string& vault) {
    std::lock_guard lock(mutex_);
    check(session, vault);
    const auto found = jobs_.find(key(session, id));
    if (found == jobs_.end() || found->second->state == AudioJobState::failed) return {};
    return found->second->stream;
}

std::optional<AudioChunk> AudioDownloadManager::audio_chunk(
    const FullSessionId& session, EntryId id, const std::string& vault, std::uint64_t offset) {
    std::shared_ptr<AudioStream> stream;
    {
        std::lock_guard lock(mutex_);
        check(session, vault);
        if (const auto found = jobs_.find(key(session, id)); found != jobs_.end()) {
            stream = found->second->stream;
        }
    }
    if (stream) {
        auto chunk = stream->read(offset);
        if (!chunk) throw std::invalid_argument("Invalid audio offset.");
        return chunk;
    }
    const auto cached = audio(session, id, vault);
    if (!cached) return std::nullopt;
    if (offset > cached->audio.size()) throw std::invalid_argument("Invalid audio offset.");
    const auto start = static_cast<std::size_t>(offset);
    const auto count = std::min<std::size_t>(64 * 1024, cached->audio.size() - start);
    return AudioChunk{cached->content_type, cached->audio.substr(start, count),
        start + count == cached->audio.size()};
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
                it->second->stream->fail();
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
        job->stream->fail();
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
    if (job->live) { run_live(job); return; }
    struct Finish {
        AudioStream& stream;
        bool saved{};
        ~Finish() { if (saved) stream.finish(); else stream.fail(); }
    } finish{*job->stream};
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
            result = transport_(job->output, job->key, job->request, cancelled,
                [&](std::string_view type, std::string_view bytes) {
                    if (!cancelled()) job->stream->append(type, bytes);
                });
            if (!result || cancelled()) return;
            if (!valid_entry_audio(*result)) throw std::runtime_error("Voice provider returned invalid audio.");
        } catch (const std::exception&) {
            if (cancelled()) return;
            // Restarting after publishing audio would repeat words already played.
            if (attempt == 3 || !job->stream->empty()) throw;
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, 50ms, cancelled);
            continue;
        }
        // Storage failures are terminal, outside the download retry loop.
        try {
            sessions_.save_entry_audio(job->entry, *result, cancelled);
            const auto saved = sessions_.lookup_entry_audio(job->entry.identity, job->entry.entry_id, false);
            // A stale/deleted entry can make the conditional cache write a no-op.
            if (!saved || !saved->has_cached_audio || saved->entry_text != job->entry.entry_text
                || saved->session_key != job->entry.session_key) return;
        } catch (const SessionNotFoundError&) {
            return;
        } catch (const ForumNotFoundError&) {
            return;
        }
        if (cancelled()) return;
        // Also support transports that return a complete clip without callbacks.
        if (job->stream->empty()) job->stream->append(result->content_type, result->audio);
        finish.saved = true;
        return;
    }
}

void AudioDownloadManager::run_live(const std::shared_ptr<Job>& job) {
    struct Finish {
        AudioStream& stream;
        bool saved{};
        ~Finish() { if (saved) stream.finish(); else stream.fail(); }
    } finish{*job->stream};
    const auto cancelled = [&] { return job->cancelled.load(); };
    std::string sent_text;
    auto result = websocket_(job->output, job->key, job->request, cancelled,
        [&](std::string_view type, std::string_view bytes) {
            if (!cancelled()) job->stream->append(type, bytes);
        }, [&] {
            std::lock_guard lock(mutex_);
            const auto spoken = speech_text_prefix(entry_speech_text(job->entry), job->request.provider, job->text_complete);
            if (!spoken.starts_with(sent_text)) throw std::runtime_error("Speech text changed during synthesis.");
            auto text = spoken.substr(sent_text.size());
            sent_text = spoken;
            return VoiceTextChunk{std::move(text), job->text_complete};
        });
    if (!result || cancelled()) return;
    EntryAudioLookup expected;
    {
        std::lock_guard lock(mutex_);
        if (!job->text_complete) throw std::runtime_error("Speech completed before the answer finished.");
        expected = job->entry;
    }
    if (!valid_entry_audio(*result)) throw std::runtime_error("Voice provider returned invalid audio.");
    sessions_.save_entry_audio(expected, *result, cancelled);
    const auto saved = sessions_.lookup_entry_audio(expected.identity, expected.entry_id, false);
    if (!saved || !saved->has_cached_audio || saved->entry_text != expected.entry_text
        || saved->session_key != expected.session_key || saved->database_path != expected.database_path || cancelled()) return;
    if (job->stream->empty()) job->stream->append(result->content_type, result->audio);
    finish.saved = true;
}

}
