#include "ui/web/session_registry.h"

#include "application/builtins.h"
#include "application/session_source.h"
#include "application/web_discovery.h"
#include "application/welcome_storage.h"
#include "session/not_found_error.h"
#include "session/session_lease.h"
#include "session/session_controller.h"
#include "session/workspace.h"
#include "ui/web/sse_mailbox.h"
#include "util/logging.h"

#include <condition_variable>
#include <exception>
#include <stdexcept>
#include <thread>
#include <utility>

namespace cha::web {
namespace {

std::string session_log(const SessionIdentity& key, std::string_view event) {
    return "web session forum_id=" + key.forum_id + " session_id=" + key.session_id
        + " event=" + std::string(event);
}

} // namespace

struct SessionRegistry::StartupResult {
    enum class Value { ready, busy, not_found, error, shutting_down };

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

struct SessionRegistry::RetiredEntry {
    SessionIdentity key;
    std::unique_ptr<Entry> entry;
};

SessionHandle::SessionHandle(std::shared_ptr<WebSessionRuntime> runtime)
    : runtime_(std::move(runtime)) {}

SessionHandle::operator bool() const noexcept { return static_cast<bool>(runtime_); }

WebSessionRuntime& SessionHandle::runtime() const { return *runtime_; }

SessionRegistry::SessionRegistry(
    WebSettings settings,
    RegistrySessionFactory factory)
    : settings_(std::move(settings)),
      factory_(std::move(factory)) {
    if (!factory_) throw std::invalid_argument("Session registry needs a controller factory");
    if (settings_.session_limit == 0) {
        throw std::invalid_argument("Web session limit must be positive");
    }
}

SessionRegistry SessionRegistry::from_workspace(
    WebSettings settings,
    std::shared_ptr<const Workspace> workspace,
    const WebDiscovery& discovery,
    WelcomeStorage& welcome_storage) {
    if (!workspace) throw std::invalid_argument("Session registry needs a workspace");
    const auto controller_workspace = workspace;
    const SharedPersonaRoster personas = discovery.effective_personas().roster();
    const std::string inventory = discovery.inventory().serialize();
    return SessionRegistry(
        std::move(settings),
        [controller_workspace, personas, inventory, &welcome_storage](
            const SessionIdentity& key, WakeNotifier& notifier) {
            if (key.forum_id == entrance_id) {
                if (key.session_id != welcome_id) throw SessionNotFoundError("Session not found");
                return RegistryOwnerInput{open_entrance_session(
                    *controller_workspace, personas, inventory,
                    welcome_storage.prepare(), notifier)};
            }
            return RegistryOwnerInput{
                controller_workspace->open_session(key, notifier, personas)};
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

RegistryOpenResult SessionRegistry::open(
    SessionIdentity key,
    std::chrono::milliseconds deadline) {
    log_info(session_log(key, "open_requested"));
    RetiredEntries retired;
    std::shared_ptr<StartupResult> startup;
    std::string deferred_event;
    {
        std::unique_lock lock(mutex_);
        retired = sweep_locked();
        if (stopping_) {
            lock.unlock();
            reap(std::move(retired));
            return RegistryOpenFailure::registry_stopping;
        }
        const auto found = entries_.find(key);
        if (found != entries_.end()) {
            if (found->second->state == Entry::State::running) {
                lock.unlock();
                log_info(session_log(key, "reattached"));
                reap(std::move(retired));
                return RegistryReady{};
            }
            if (found->second->state == Entry::State::stopping) {
                lock.unlock();
                log_warn(session_log(key, "open_rejected_stopping"));
                reap(std::move(retired));
                return RegistryOpenFailure::stopping;
            }
            startup = found->second->startup;
        } else {
            if (entries_.size() >= settings_.session_limit) {
                lock.unlock();
                log_warn(session_log(key, "open_rejected_limit"));
                reap(std::move(retired));
                return RegistryOpenFailure::limit_reached;
            }
            auto entry = std::make_unique<Entry>();
            deferred_event = "registry_starting";
            startup = std::make_shared<StartupResult>();
            entry->startup = startup;
            auto [position, inserted] = entries_.emplace(key, std::move(entry));
            (void)inserted;
            try {
                position->second->owner = std::thread(
                    &SessionRegistry::owner_main, this, key, startup);
            } catch (const std::bad_alloc&) {
                std::terminate();
            } catch (...) {
                // No waiter can attach while this mutex acquisition is still
                // in progress, but resolve the result before removing the
                // entry so the insertion/start operation remains wholly
                // transactional if that ordering ever changes.
                startup->complete(StartupResult::Value::error);
                entries_.erase(position);
                lock.unlock();
                reap(std::move(retired));
                return RegistryOpenFailure::internal_error;
            }
        }
    }
    if (!deferred_event.empty()) log_info(session_log(key, deferred_event));
    reap(std::move(retired));

    const auto outcome = startup->wait_for(deadline);
    if (!outcome) {
        log_warn(session_log(key, "open_deadline_expired"));
        std::lock_guard lock(mutex_);
        return stopping_
            ? RegistryOpenResult{RegistryOpenFailure::registry_stopping}
            : RegistryOpenResult{RegistryOpenFailure::open_timeout};
    }
    switch (*outcome) {
    case StartupResult::Value::ready: return RegistryReady{};
    case StartupResult::Value::busy: return RegistryOpenFailure::busy;
    case StartupResult::Value::not_found: return RegistryOpenFailure::not_found;
    case StartupResult::Value::error: return RegistryOpenFailure::internal_error;
    case StartupResult::Value::shutting_down:
        return RegistryOpenFailure::registry_stopping;
    }
    return RegistryOpenFailure::internal_error;
}

std::optional<RegistryOpenResult> SessionRegistry::try_reattach(
    const SessionIdentity& key) {
    RetiredEntries retired;
    std::optional<RegistryOpenResult> result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        if (stopping_) {
            result = RegistryOpenFailure::registry_stopping;
        } else {
            const auto found = entries_.find(key);
            if (found != entries_.end()
                && found->second->state == Entry::State::running) {
                result = RegistryReady{};
            }
        }
    }
    reap(std::move(retired));
    if (result) {
        if (std::holds_alternative<RegistryReady>(*result)) {
            log_info(session_log(key, "reattached"));
        } else {
            log_warn(session_log(key, "open_rejected_server_stopping"));
        }
    }
    return result;
}

SessionHandle SessionRegistry::lookup(const SessionIdentity& key) {
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

void SessionRegistry::begin_shutdown(
    const std::function<void()>& stop_accepting) {
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
    if (stop_accepting) stop_accepting();
    for (const auto& startup : startups) startup->wake();
    reap(std::move(retired));
    for (const auto& runtime : runtimes) {
        runtime->request_shutdown(ShutdownReason::server_stopping);
    }
}

bool SessionRegistry::join_shutdown(std::chrono::milliseconds grace) {
    const auto deadline = std::chrono::steady_clock::now() + grace;
    {
        std::unique_lock lock(mutex_);
        if (!lifecycle_changed_.wait_until(lock, deadline, [this] {
                for (const auto& [key, entry] : entries_) {
                    (void)key;
                    if (!entry->finished) return false;
                }
                return true;
            })) {
            return false;
        }
    }
    // finished is published only as the final teardown hook, after the
    // controller and every blocking runtime resource have been released.
    // From that hook to owner_main returning there is only non-blocking stack
    // unwinding, so reaping here cannot turn an owner operation into an
    // unbounded join outside the grace deadline.
    sweep();
    return true;
}

std::vector<SessionIdentity> SessionRegistry::unfinished_owners() {
    RetiredEntries retired;
    std::vector<SessionIdentity> result;
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
            retired.push_back({it->first, std::move(it->second)});
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    return retired;
}

void SessionRegistry::reap(RetiredEntries retired) {
    for (auto& retired_entry : retired) {
        if (retired_entry.entry->owner.joinable()) {
            retired_entry.entry->owner.join();
        }
        retired_entry.entry->runtime.reset();
        // Completion is recorded after join so a wedged owner can never look
        // like a successfully reaped session. Retaining the map key alongside
        // the retired entry keeps this registry transition session-scoped.
        log_info(session_log(retired_entry.key, "registry_sweep_joined"));
    }
}

void SessionRegistry::owner_main(SessionIdentity key, std::shared_ptr<StartupResult> startup) {
    // This shared pointer exists only for the construction handoff. At either
    // the commit point or a failed open, ownership moves into the entry before
    // this thread returns. The owner uses runtime_view thereafter and can
    // never destroy its own runtime by releasing the last owning reference.
    std::shared_ptr<WebSessionRuntime> runtime;
    WebSessionRuntime* runtime_view = nullptr;
    const auto finish_failed_start = [&](StartupResult::Value result) {
        {
            std::lock_guard lock(mutex_);
            startup->complete(result);
            const auto found = entries_.find(key);
            if (found != entries_.end()) {
                if (runtime) found->second->runtime = std::move(runtime);
                found->second->state = Entry::State::stopping;
                found->second->finished = true;
            }
        }
        lifecycle_changed_.notify_all();
    };
    try {
        auto notifier = std::make_shared<WakeNotifier>();
        RegistryOwnerInput owner_input = factory_(key, *notifier);
        SessionDescriptor descriptor;
        std::unique_ptr<WebSessionController> port;
        std::optional<OpenedSession> opened;
        if (auto* concrete = std::get_if<OpenedSession>(&owner_input)) {
            descriptor = concrete->descriptor;
            opened.emplace(std::move(*concrete));
        } else if (auto* backed = std::get_if<PortBackedSession>(&owner_input)) {
            descriptor = std::move(backed->descriptor);
            port = std::move(backed->controller);
        }
        auto mailbox = std::make_shared<SseMailbox>();
        runtime = std::make_shared<WebSessionRuntime>(settings_, std::move(descriptor), mailbox,
            WebRuntimeHooks{
                .mark_registry_stopping = [this, key] {
                    bool transitioned = false;
                    {
                        std::lock_guard lock(mutex_);
                        const auto found = entries_.find(key);
                        if (found != entries_.end()
                            && found->second->state != Entry::State::stopping) {
                            found->second->state = Entry::State::stopping;
                            transitioned = true;
                        }
                    }
                    if (transitioned) log_info(session_log(key, "registry_stopping"));
                },
                .mark_finished = [this, key] {
                    {
                        std::lock_guard lock(mutex_);
                        const auto found = entries_.find(key);
                        if (found != entries_.end()) found->second->finished = true;
                    }
                    lifecycle_changed_.notify_all();
                },
                // These two take their identity from the runtime rather than
                // capturing it, so the logged session can never drift from the
                // one the runtime is actually reporting about.
                .log_fatal = [](const SessionDescriptor& descriptor) {
                    log_error(session_log(descriptor.identity, "fatal_contained"));
                },
                .log_event = [](const SessionDescriptor& descriptor, std::string_view event) {
                    log_info(session_log(descriptor.identity, event));
                },
            }, WebRuntimeClock{}, notifier);
        runtime_view = runtime.get();
        bool stop_now = false;
        bool registry_running = false;
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
                registry_running = true;
                startup->complete(StartupResult::Value::ready);
            } else {
                startup->complete(StartupResult::Value::shutting_down);
            }
        }
        if (registry_running) log_info(session_log(key, "registry_running"));
        if (stop_now) {
            runtime_view->request_shutdown(ShutdownReason::server_stopping);
        }
        if (opened) runtime_view->run(std::move(*opened));
        else runtime_view->run_with_controller(std::move(port));
    } catch (const std::bad_alloc&) {
        std::terminate();
    } catch (const SessionBusyError&) {
        log_warn(session_log(key, "lease_busy"));
        finish_failed_start(StartupResult::Value::busy);
    } catch (const SessionNotFoundError&) {
        log_warn(session_log(key, "storage_not_found"));
        finish_failed_start(StartupResult::Value::not_found);
    } catch (const ForumNotFoundError&) {
        log_warn(session_log(key, "storage_not_found"));
        finish_failed_start(StartupResult::Value::not_found);
    } catch (...) {
        log_error(session_log(key, "startup_failed"));
        finish_failed_start(StartupResult::Value::error);
    }
}

} // namespace cha::web
