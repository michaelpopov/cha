#include "ui/web/session_registry.h"

#include "session/session_lease.h"
#include "session/session_controller.h"
#include "session/workspace.h"
#include "ui/web/sse_mailbox.h"

#include <condition_variable>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cha::web {

struct SessionRegistry::StartupResult {
    enum class Value { ready, busy, error, shutting_down };

    void complete(Value next) {
        {
            std::lock_guard lock(mutex);
            if (value) return;
            value = next;
        }
        changed.notify_all();
    }

    void wake() {
        {
            std::lock_guard lock(mutex);
            woken = true;
        }
        changed.notify_all();
    }

    [[nodiscard]] std::optional<Value> wait_for(
        std::chrono::milliseconds deadline) {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, deadline, [this] { return value.has_value() || woken; });
        return value;
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::optional<Value> value;
    bool woken{};
};

struct SessionRegistry::Entry {
    enum class State { starting, running, stopping };

    State state{State::starting};
    std::shared_ptr<WebSessionRuntime> runtime;
    std::thread owner;
    std::shared_ptr<StartupResult> startup;
    bool finished{};
};

SessionHandle::SessionHandle(std::shared_ptr<WebSessionRuntime> runtime)
    : runtime_(std::move(runtime)) {}

SessionHandle::operator bool() const noexcept { return static_cast<bool>(runtime_); }

WebSessionRuntime& SessionHandle::runtime() const { return *runtime_; }

SessionRegistry::SessionRegistry(
    WebSettings settings,
    RegistryControllerFactory factory,
    RegistryMetadataFactory metadata_factory)
    : settings_(std::move(settings)),
      factory_(std::move(factory)),
      metadata_factory_(std::move(metadata_factory)) {
    if (!factory_) throw std::invalid_argument("Session registry needs a controller factory");
    if (settings_.session_limit == 0) {
        throw std::invalid_argument("Web session limit must be positive");
    }
}

SessionRegistry SessionRegistry::from_workspace(
    WebSettings settings,
    std::shared_ptr<const Workspace> workspace) {
    if (!workspace) throw std::invalid_argument("Session registry needs a workspace");
    const auto controller_workspace = workspace;
    return SessionRegistry(
        std::move(settings),
        [controller_workspace](const SessionKey& key, WakeNotifier& notifier) {
            return adapt_session_controller(
                controller_workspace->open_session(key.forum, key.session_id, notifier));
        },
        [workspace = std::move(workspace)](const SessionKey& key) {
            const Forum forum = workspace->load_forum(key.forum);
            for (const SessionSummary& session : workspace->sessions(key.forum)) {
                if (session.id == key.session_id) {
                    return WebSessionMetadata{
                        .forum = {forum.name, forum.display_name},
                        .session_id = session.id,
                        .session_label = session.label,
                    };
                }
            }
            throw std::runtime_error("Session metadata is unavailable");
        });
}

SessionRegistry::~SessionRegistry() {
    // Production reaches normal destruction only after the process-shutdown
    // coordinator's bounded grace period has confirmed that every owner can
    // be joined.  If that grace expires, Section 19.1 requires immediate
    // process exit without running destructors, so a wedged owner must never
    // reach this unbounded final safety join.
    begin_shutdown();
    std::vector<std::thread*> owners;
    {
        std::lock_guard lock(mutex_);
        for (auto& [key, entry] : entries_) {
            (void)key;
            if (entry->owner.joinable()) owners.push_back(&entry->owner);
        }
    }
    for (std::thread* owner : owners) owner->join();
}

std::string SessionRegistry::path_for(const SessionKey& key) {
    return "/s/" + key.forum + "/" + key.session_id + "/";
}

RegistryOpenResult SessionRegistry::open(
    SessionKey key,
    std::chrono::milliseconds deadline) {
    RetiredEntries retired;
    std::shared_ptr<StartupResult> startup;
    {
        std::unique_lock lock(mutex_);
        retired = sweep_locked();
        if (stopping_) {
            lock.unlock();
            reap(std::move(retired));
            return Error{ErrorCode::server_stopping, "Server is stopping."};
        }
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (found->second->state == Entry::State::running) {
                const std::string path = path_for(key);
                lock.unlock();
                reap(std::move(retired));
                return OpenSessionSuccess{path};
            }
            if (found->second->state == Entry::State::stopping) {
                lock.unlock();
                reap(std::move(retired));
                return Error{ErrorCode::session_stopping, "Session is stopping."};
            }
            startup = found->second->startup;
        } else {
            if (entries_.size() >= settings_.session_limit) {
                lock.unlock();
                reap(std::move(retired));
                return Error{ErrorCode::session_limit_reached, "Session limit reached."};
            }
            auto entry = std::make_unique<Entry>();
            startup = std::make_shared<StartupResult>();
            entry->startup = startup;
            auto [position, inserted] = entries_.emplace(key, std::move(entry));
            (void)inserted;
            try {
                position->second->owner = std::thread(
                    &SessionRegistry::owner_main, this, key, startup);
            } catch (const std::bad_alloc&) {
                startup->complete(StartupResult::Value::error);
                entries_.erase(position);
                lock.unlock();
                reap(std::move(retired));
                throw;
            } catch (...) {
                // No waiter can attach while this mutex acquisition is still
                // in progress, but resolve the result before removing the
                // entry so the insertion/start operation remains wholly
                // transactional if that ordering ever changes.
                startup->complete(StartupResult::Value::error);
                entries_.erase(position);
                lock.unlock();
                reap(std::move(retired));
                return Error{
                    ErrorCode::internal_error,
                    "Session could not be opened.",
                };
            }
        }
    }
    reap(std::move(retired));

    const auto outcome = startup->wait_for(deadline);
    if (!outcome) {
        std::lock_guard lock(mutex_);
        return stopping_
            ? RegistryOpenResult{Error{ErrorCode::server_stopping, "Server is stopping."}}
            : RegistryOpenResult{Error{ErrorCode::session_open_timeout, "Session is still opening."}};
    }
    switch (*outcome) {
    case StartupResult::Value::ready: return OpenSessionSuccess{path_for(key)};
    case StartupResult::Value::busy: return Error{ErrorCode::session_busy, "Session is busy."};
    case StartupResult::Value::error: return Error{ErrorCode::internal_error, "Session could not be opened."};
    case StartupResult::Value::shutting_down:
        return Error{ErrorCode::server_stopping, "Server is stopping."};
    }
    return Error{ErrorCode::internal_error, "Session could not be opened."};
}

std::optional<RegistryOpenResult> SessionRegistry::try_reattach(
    const SessionKey& key) {
    RetiredEntries retired;
    std::optional<RegistryOpenResult> result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        if (stopping_) {
            result = Error{ErrorCode::server_stopping, "Server is stopping."};
        } else {
            const auto found = entries_.find(key);
            if (found != entries_.end()
                && found->second->state == Entry::State::running) {
                result = OpenSessionSuccess{path_for(key)};
            }
        }
    }
    reap(std::move(retired));
    return result;
}

SessionHandle SessionRegistry::lookup(const SessionKey& key) {
    RetiredEntries retired;
    SessionHandle result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        const auto found = entries_.find(key);
        if (found != entries_.end() && found->second->state == Entry::State::running) {
            result = SessionHandle(found->second->runtime);
        }
    }
    reap(std::move(retired));
    return result;
}

RegistrySnapshot SessionRegistry::snapshot() {
    RetiredEntries retired;
    RegistrySnapshot result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        result.live_entry_count = entries_.size();
        for (const auto& [key, entry] : entries_) {
            if (entry->state == Entry::State::running) {
                result.running_sessions.push_back(key);
            }
        }
    }
    reap(std::move(retired));
    return result;
}

void SessionRegistry::begin_shutdown() {
    RetiredEntries retired;
    std::vector<std::shared_ptr<WebSessionRuntime>> runtimes;
    std::vector<std::shared_ptr<StartupResult>> startups;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        stopping_ = true;
        for (auto& [key, entry] : entries_) {
            (void)key;
            startups.push_back(entry->startup);
            if (entry->state == Entry::State::running && entry->runtime) {
                runtimes.push_back(entry->runtime);
            }
        }
    }
    reap(std::move(retired));
    for (const auto& startup : startups) startup->wake();
    for (const auto& runtime : runtimes) {
        runtime->request_shutdown(ShutdownReason::server_stopping);
    }
}

std::vector<SessionKey> SessionRegistry::unfinished_owners() {
    RetiredEntries retired;
    std::vector<SessionKey> result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        result.reserve(entries_.size());
        for (const auto& [key, entry] : entries_) {
            if (!entry->finished && entry->owner.joinable()) result.push_back(key);
        }
    }
    reap(std::move(retired));
    return result;
}

void SessionRegistry::sweep() {
    RetiredEntries retired;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
    }
    reap(std::move(retired));
}

SessionRegistry::RetiredEntries SessionRegistry::sweep_locked() {
    RetiredEntries retired;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second->finished) {
            retired.push_back(std::move(it->second));
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    return retired;
}

void SessionRegistry::reap(RetiredEntries retired) {
    for (auto& entry : retired) {
        if (entry->owner.joinable()) entry->owner.join();
        entry->runtime.reset();
    }
}

void SessionRegistry::owner_main(SessionKey key, std::shared_ptr<StartupResult> startup) {
    // This shared pointer exists only for the construction handoff. At either
    // the commit point or a failed open, ownership moves into the entry before
    // this thread returns. The owner uses runtime_view thereafter and can
    // never destroy its own runtime by releasing the last owning reference.
    std::shared_ptr<WebSessionRuntime> runtime;
    WebSessionRuntime* runtime_view = nullptr;
    try {
        WebSessionMetadata metadata;
        if (metadata_factory_) {
            metadata = metadata_factory_(key);
        } else {
            metadata.forum.id = key.forum;
            metadata.session_id = key.session_id;
        }
        auto mailbox = std::make_shared<SseMailbox>();
        runtime = std::make_shared<WebSessionRuntime>(settings_, std::move(metadata), mailbox,
            WebRuntimeHooks{
                .mark_registry_stopping = [this, key] {
                    std::lock_guard lock(mutex_);
                    const auto found = entries_.find(key);
                    if (found != entries_.end()) found->second->state = Entry::State::stopping;
                },
                .mark_finished = [this, key] {
                    std::lock_guard lock(mutex_);
                    const auto found = entries_.find(key);
                    if (found != entries_.end()) found->second->finished = true;
                },
            });
        runtime_view = runtime.get();
        std::unique_ptr<WebSessionController> controller =
            factory_(key, runtime_view->notifier_for_owner());
        bool stop_now = false;
        {
            std::lock_guard lock(mutex_);
            const auto found = entries_.find(key);
            if (found == entries_.end()) {
                // This is unreachable while the registry's lifecycle
                // invariants hold, but never strand attached waiters if a
                // future change violates them.  The owner remains the sole
                // startup-result writer.
                startup->complete(
                    stopping_ ? StartupResult::Value::shutting_down
                              : StartupResult::Value::error);
                return;
            }
            stop_now = stopping_;
            // State, rather than presence of this internal owning reference,
            // controls publication. Even the shutting-down path transfers
            // ownership so destruction happens only after the owner is joined.
            found->second->runtime = std::move(runtime);
            if (!stop_now) {
                found->second->state = Entry::State::running;
                startup->complete(StartupResult::Value::ready);
            } else {
                startup->complete(StartupResult::Value::shutting_down);
            }
        }
        if (stop_now) {
            runtime_view->request_shutdown(ShutdownReason::server_stopping);
        }
        runtime_view->run_with_controller(std::move(controller));
    } catch (const std::bad_alloc&) {
        std::terminate();
    } catch (const SessionBusyError&) {
        std::lock_guard lock(mutex_);
        startup->complete(StartupResult::Value::busy);
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (runtime) found->second->runtime = std::move(runtime);
            found->second->state = Entry::State::stopping;
            found->second->finished = true;
        }
    } catch (...) {
        std::lock_guard lock(mutex_);
        startup->complete(StartupResult::Value::error);
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (runtime) found->second->runtime = std::move(runtime);
            found->second->state = Entry::State::stopping;
            found->second->finished = true;
        }
    }
}

} // namespace cha::web
