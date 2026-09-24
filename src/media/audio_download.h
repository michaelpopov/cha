#pragma once

#include "storage/session_repository.h"
#include "media/audio_stream.h"
#include "providers/fish_audio.h"
#include "workspace/workspace.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace cha {
enum class AudioJobState { queued, running, failed };
enum class AudioAcceptanceKind { cached, queued, running };

struct AudioAcceptance {
    EntryId entry_id;
    AudioAcceptanceKind kind;
};
struct AudioDownloadJob {
    EntryId entry_id;
    AudioJobState state;
};
struct AudioDownloadStatus {
    std::set<EntryId> cached_entry_ids;
    std::vector<AudioDownloadJob> downloads;
};
struct AudioDownloadRequest {
    std::string vault_name;
    FishAudioSynthesis synthesis;
};
struct AudioDownloadBatchEntry {
    EntryId entry_id;
    FishAudioSynthesis synthesis;
};
struct AudioDownloadBatchRequest {
    std::string vault_name;
    std::vector<AudioDownloadBatchEntry> entries;
};

class AudioDownloadError : public std::runtime_error {
public:
    AudioDownloadError(int status, std::string code, std::string message)
        : std::runtime_error(std::move(message)), status(status), code(std::move(code)) {}
    int status;
    std::string code;
};

// Only in-memory state is protected by mutex_. Repository and transport work
// always happen outside it. Jobs keep their identity after removal from jobs_.
class AudioDownloadManager {
public:
    using Transport = std::function<std::optional<EntryAudio>(const WorkspaceVoiceOutput&,
        const std::string&, const FishAudioRequest&, const std::function<bool()>&,
        const AudioChunkCallback&)>;
    // Reads the active vault's name on each check, so a vault switch is seen
    // without holding a reference to the composition root.
    using ActiveVaultName = std::function<std::string()>;
    AudioDownloadManager(const SessionRepository& sessions,
        ActiveVaultName active_vault_name, bool enabled,
        Transport transport = download_fish_audio);
    ~AudioDownloadManager();
    AudioAcceptance submit(const FullSessionId& session, EntryId id, const AudioDownloadRequest& input);
    std::vector<AudioAcceptance> submit_batch(const FullSessionId& session, const AudioDownloadBatchRequest& input);
    AudioDownloadStatus status(const FullSessionId& session, const std::string& vault);
    std::optional<EntryAudio> audio(const FullSessionId& session, EntryId id, const std::string& vault);
    std::shared_ptr<AudioStream> stream(const FullSessionId& session, EntryId id, const std::string& vault);
    void clear(const FullSessionId& session);
    void pause(bool cancel = true);
    void resume();
    void request_stop();
    bool join_until(std::chrono::steady_clock::time_point deadline);

private:
    using Key = std::tuple<std::string, std::string, EntryId>;
    struct Job {
        EntryAudioLookup entry;
        WorkspaceVoiceOutput output;
        std::string key;
        FishAudioRequest request;
        AudioJobState state{AudioJobState::queued};
        std::atomic_bool cancelled{false};
        std::shared_ptr<AudioStream> stream = std::make_shared<AudioStream>();
    };
    static Key key(const FullSessionId& session, EntryId id);
    void check(const FullSessionId& session, const std::string& vault) const;
    void check_generation(const FullSessionId& session, const std::string& vault, std::size_t generation) const;
    void worker();
    void run(const std::shared_ptr<Job>& job);
    void cancel_all();
    std::shared_ptr<Job> prepare_job(const EntryAudioLookup& entry, const FishAudioSynthesis& synthesis);
    const SessionRepository& sessions_;
    ActiveVaultName active_vault_name_;
    bool enabled_;
    Transport transport_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<Key, std::shared_ptr<Job>> jobs_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::set<std::pair<std::string, std::string>> clearing_;
    std::array<std::thread, 3> workers_;
    std::size_t generation_{};
    std::size_t exited_{};
    bool paused_{};
    bool stopped_{};
};

}
