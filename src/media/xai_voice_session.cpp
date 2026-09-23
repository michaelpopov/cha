#include "media/xai_voice_session.h"

#include "media/xai_transcript.h"
#include "util/logging.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

namespace cha::media {
namespace {

using clock = std::chrono::steady_clock;

// The worker sees new audio and stop requests only between socket waits.
constexpr std::chrono::milliseconds request_poll{20};

struct Terminal {
    ErrorCode code = ErrorCode::internal_error;
    std::string message;
};

struct Session {
    std::string connection_id;
    std::string session_id;
    std::chrono::milliseconds deadline{30000};
    std::string url;
    std::string authorization;

    std::mutex mu;
    std::atomic_bool cancel{false};
    std::atomic_bool timed_out{false};
    std::atomic<std::uint64_t> start_request_id{0};
    std::atomic<std::uint64_t> audio_request_id{0};
    std::atomic<std::uint64_t> stop_request_id{0};

    std::shared_ptr<app::OperationReply> start_reply;
    bool start_completed = false;
    bool ready = false;
    bool finished = false;
    // Set before the registry slot changes, so a request in between still
    // gets the failure.
    std::optional<Terminal> failure;

    struct AudioCall {
        std::vector<unsigned char> pcm;
        std::shared_ptr<app::OperationReply> reply;
        std::chrono::milliseconds deadline{0};
    };
    std::optional<AudioCall> audio;

    struct StopCall {
        std::shared_ptr<app::OperationReply> reply;
        clock::time_point deadline{};
    };
    std::optional<StopCall> stop;
};

using SessionKey = std::pair<std::string, std::string>;

SessionKey key_for(std::string_view connection_id, std::string_view session_id) {
    return {std::string(connection_id), std::string(session_id)};
}

nlohmann::json pieces_json(
    std::string_view session_id,
    std::vector<std::string> pieces) {
    nlohmann::json values = nlohmann::json::array();
    for (std::string& piece : pieces) values.push_back(std::move(piece));
    return {{"session_id", session_id}, {"pieces", std::move(values)}};
}

} // namespace

struct XaiVoiceSessions::Impl {
    struct Slot {
        enum class Kind { tombstone, live, terminal };
        Kind kind = Kind::tombstone;
        std::shared_ptr<Session> session;
        ErrorCode code = ErrorCode::internal_error;
        std::string message;
    };

    explicit Impl(app::BackgroundJobs& jobs) : jobs(jobs) {
        factory = [] { return make_xai_curl_socket(); };
    }

    void finish_slot(
        const std::shared_ptr<Session>& session,
        std::optional<Terminal> terminal) {
        std::lock_guard lock(mu);
        const SessionKey key = key_for(session->connection_id, session->session_id);
        const auto found = slots.find(key);
        if (found == slots.end() || found->second.session != session) return;
        if (terminal) {
            found->second.kind = Slot::Kind::terminal;
            found->second.session.reset();
            found->second.code = terminal->code;
            found->second.message = std::move(terminal->message);
        } else {
            slots.erase(found);
        }
        const auto active = active_by_connection.find(session->connection_id);
        if (active != active_by_connection.end()
            && active->second == session->session_id) {
            active_by_connection.erase(active);
        }
    }

    std::mutex mu;
    app::BackgroundJobs& jobs;
    SocketFactory factory;
    std::map<SessionKey, Slot> slots;
    std::map<std::string, std::string> active_by_connection;
};

namespace {

void run_worker(
    const std::function<void(const std::shared_ptr<Session>&, std::optional<Terminal>)>& finish,
    const std::shared_ptr<Session>& session,
    std::unique_ptr<XaiSocket> socket,
    std::atomic_bool& job_cancel) {
    XaiTranscriptNormalizer normalizer;
    std::vector<std::string> pieces;
    std::size_t pending_bytes = 0;
    bool audio_done_sent = false;
    bool got_done = false;
    bool failed = false;

    const auto stopping = [&] {
        return job_cancel.load() || session->cancel.load();
    };
    const auto fail = [&](ErrorCode code, std::string message) {
        if (failed) return;
        failed = true;
        log_warn(message);
        pieces.clear();
        pending_bytes = 0;
        std::shared_ptr<app::OperationReply> start_reply;
        std::shared_ptr<app::OperationReply> audio_reply;
        std::shared_ptr<app::OperationReply> stop_reply;
        bool start_completed = false;
        {
            std::lock_guard lock(session->mu);
            session->finished = true;
            session->failure = Terminal{code, message};
            start_completed = session->start_completed;
            start_reply = std::move(session->start_reply);
            if (session->audio) audio_reply = std::move(session->audio->reply);
            session->audio.reset();
            session->audio_request_id.store(0);
            if (session->stop) stop_reply = std::move(session->stop->reply);
            session->stop.reset();
            session->stop_request_id.store(0);
        }
        bool delivered = false;
        if (start_reply && !start_completed) {
            delivered = start_reply->fail(code, message) || delivered;
        }
        if (audio_reply) delivered = audio_reply->fail(code, message) || delivered;
        if (stop_reply) delivered = stop_reply->fail(code, message) || delivered;
        // Nobody asks for the result of a cancelled dictation, so keep none.
        const bool cancelled = session->cancel.load() && !session->timed_out.load();
        if (!start_completed || delivered || cancelled) finish(session, std::nullopt);
        else finish(session, Terminal{code, std::move(message)});
        socket->close();
    };
    const auto throw_stopped = [&] {
        if (!stopping()) return;
        throw XaiVoiceFailure(
            ErrorCode::operation_cancelled, std::string(xai_operation_cancelled));
    };
    const auto append_addition = [&](std::string addition) {
        if (addition.empty()) return;
        if (pending_bytes + addition.size() > xai_max_pending_text) {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_pending_text_limit));
        }
        pending_bytes += addition.size();
        pieces.push_back(std::move(addition));
    };
    const auto handle = [&](const XaiIncoming& message) {
        if (message.closed) {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_connection_closed));
        }
        if (message.binary) return XaiTranscriptUpdate{};
        if (message.payload.size() > xai_max_provider_message) {
            throw XaiTranscriptError(std::string(xai_malformed_transcript));
        }
        const nlohmann::json event = nlohmann::json::parse(
            message.payload, nullptr, false);
        if (event.is_discarded()) {
            throw XaiTranscriptError(std::string(xai_malformed_transcript));
        }
        XaiTranscriptUpdate update = normalizer.apply(event);
        append_addition(std::move(update.addition));
        if (update.done) {
            if (!audio_done_sent) {
                throw XaiVoiceFailure(
                    ErrorCode::internal_error, std::string(xai_completed_early));
            }
            got_done = true;
        }
        return update;
    };
    const auto drain = [&] {
        while (auto message = socket->recv(std::chrono::milliseconds::zero())) {
            (void)handle(*message);
        }
    };

    try {
        const auto budget = xai_deadline_budget(session->deadline);
        const auto startup_deadline = clock::now() + budget.startup;
        socket->connect(
            session->url, session->authorization, startup_deadline, stopping);
        bool created = false;
        while (!created) {
            throw_stopped();
            if (clock::now() >= startup_deadline) {
                throw XaiVoiceFailure(
                    ErrorCode::command_timeout, std::string(xai_timed_out));
            }
            auto message = socket->recv(std::chrono::milliseconds(100));
            if (!message) continue;
            created = handle(*message).created;
        }
        throw_stopped();
        std::shared_ptr<app::OperationReply> start_reply;
        {
            std::lock_guard lock(session->mu);
            session->ready = true;
            session->start_completed = true;
            start_reply = session->start_reply;
        }
        if (!start_reply->complete({
                {"session_id", session->session_id},
                {"stop_budget_ms", budget.stop_budget.count()},
            })) {
            session->cancel.store(true);
            throw XaiVoiceFailure(
                ErrorCode::operation_cancelled,
                std::string(xai_operation_cancelled));
        }

        while (!got_done) {
            throw_stopped();
            drain();
            if (got_done) break;
            std::vector<unsigned char> pcm;
            std::shared_ptr<app::OperationReply> audio_reply;
            std::chrono::milliseconds audio_deadline{0};
            std::optional<clock::time_point> stop_deadline;
            {
                std::lock_guard lock(session->mu);
                if (session->audio && !session->audio->pcm.empty()) {
                    pcm = std::move(session->audio->pcm);
                    audio_reply = session->audio->reply;
                    audio_deadline = session->audio->deadline;
                }
                if (session->stop) stop_deadline = session->stop->deadline;
            }
            if (!pcm.empty() && !audio_done_sent) {
                const auto send_deadline = clock::now()
                    + xai_deadline_budget(audio_deadline).audio_send;
                socket->send(
                    std::string_view(
                        reinterpret_cast<const char*>(pcm.data()), pcm.size()),
                    true,
                    send_deadline,
                    stopping);
                drain();
                auto delivered = pieces;
                pieces.clear();
                pending_bytes = 0;
                {
                    std::lock_guard lock(session->mu);
                    session->audio_request_id.store(0);
                    if (session->audio) audio_reply = std::move(session->audio->reply);
                    session->audio.reset();
                }
                if (audio_reply && !audio_reply->complete(
                        pieces_json(session->session_id, std::move(delivered)))) {
                    session->cancel.store(true);
                    throw XaiVoiceFailure(
                        ErrorCode::operation_cancelled,
                        std::string(xai_operation_cancelled));
                }
                continue;
            }
            if (stop_deadline && !audio_done_sent) {
                if (clock::now() >= *stop_deadline) {
                    throw XaiVoiceFailure(
                        ErrorCode::command_timeout, std::string(xai_timed_out));
                }
                socket->send(
                    "{\"type\":\"audio.done\"}",
                    false,
                    *stop_deadline,
                    stopping);
                audio_done_sent = true;
                drain();
                continue;
            }
            if (audio_done_sent) {
                if (!stop_deadline || clock::now() >= *stop_deadline) {
                    throw XaiVoiceFailure(
                        ErrorCode::command_timeout, std::string(xai_timed_out));
                }
                if (auto message = socket->recv(std::chrono::milliseconds(100))) {
                    (void)handle(*message);
                }
                continue;
            }
            if (auto message = socket->recv(request_poll)) {
                (void)handle(*message);
            }
        }

        std::shared_ptr<app::OperationReply> stop_reply;
        {
            std::lock_guard lock(session->mu);
            session->finished = true;
            session->stop_request_id.store(0);
            if (session->stop) stop_reply = std::move(session->stop->reply);
            session->stop.reset();
        }
        socket->close();
        finish(session, std::nullopt);
        if (stop_reply) {
            (void)stop_reply->complete(
                pieces_json(session->session_id, std::move(pieces)));
        }
    } catch (const XaiVoiceFailure& error) {
        // A bridge deadline cancels the worker. Report it as a timeout.
        if (error.code == ErrorCode::operation_cancelled && session->timed_out.load()) {
            fail(ErrorCode::command_timeout, std::string(xai_timed_out));
        } else {
            fail(error.code, error.what());
        }
    } catch (const XaiTranscriptError& error) {
        fail(ErrorCode::internal_error, error.what());
    } catch (const std::exception&) {
        fail(ErrorCode::internal_error, std::string(xai_connection_failed));
    }
}

} // namespace

XaiDeadlineBudget xai_deadline_budget(std::chrono::milliseconds deadline) {
    const auto scaled = deadline.count() > 0
        ? std::chrono::milliseconds(deadline.count() * 2 / 3)
        : std::chrono::milliseconds::zero();
    return {
        std::min(std::chrono::milliseconds(15000), scaled),
        std::min(std::chrono::milliseconds(20000), scaled),
        std::min(std::chrono::milliseconds(2000), scaled),
    };
}

std::chrono::milliseconds xai_stop_limit(
    std::chrono::milliseconds remaining,
    std::chrono::milliseconds deadline) {
    if (remaining.count() < 0 || deadline.count() < 0) {
        return std::chrono::milliseconds::zero();
    }
    return std::min(remaining, deadline);
}

XaiVoiceSessions::XaiVoiceSessions(app::BackgroundJobs& jobs)
    : impl_(std::make_unique<Impl>(jobs)) {}

XaiVoiceSessions::~XaiVoiceSessions() {
    cancel_all();
}

void XaiVoiceSessions::set_socket_factory(SocketFactory factory) {
    std::lock_guard lock(impl_->mu);
    if (factory) impl_->factory = std::move(factory);
    else impl_->factory = [] { return make_xai_curl_socket(); };
}

std::shared_ptr<app::OperationReply> XaiVoiceSessions::start(
    std::string connection_id,
    std::uint64_t request_id,
    std::string session_id,
    std::string url,
    std::string authorization,
    std::chrono::milliseconds deadline) {
    auto reply = std::make_shared<app::OperationReply>();
    auto session = std::make_shared<Session>();
    session->connection_id = connection_id;
    session->session_id = session_id;
    session->deadline = deadline;
    session->url = std::move(url);
    session->authorization = std::move(authorization);
    session->start_reply = reply;
    session->start_request_id.store(request_id);
    SocketFactory factory;
    {
        std::lock_guard lock(impl_->mu);
        const SessionKey key = key_for(connection_id, session_id);
        const auto existing = impl_->slots.find(key);
        if (existing != impl_->slots.end()
            && existing->second.kind == Impl::Slot::Kind::tombstone) {
            impl_->slots.erase(existing);
            reply->fail(
                ErrorCode::operation_cancelled,
                std::string(xai_operation_cancelled));
            return reply;
        }
        if (existing != impl_->slots.end()
            && existing->second.kind == Impl::Slot::Kind::live) {
            reply->fail(
                ErrorCode::invalid_argument,
                "xAI voice input is already active.");
            return reply;
        }
        const auto active = impl_->active_by_connection.find(connection_id);
        if (active != impl_->active_by_connection.end()) {
            reply->fail(
                ErrorCode::invalid_argument,
                "xAI voice input is already active.");
            return reply;
        }
        if (existing != impl_->slots.end()) impl_->slots.erase(existing);
        Impl::Slot slot;
        slot.kind = Impl::Slot::Kind::live;
        slot.session = session;
        impl_->slots.insert_or_assign(key, std::move(slot));
        impl_->active_by_connection[connection_id] = session_id;
        factory = impl_->factory;
    }
    const bool launched = impl_->jobs.launch(
        [registry = impl_.get(), session, factory = std::move(factory)](
            std::atomic_bool& cancel) {
            std::unique_ptr<XaiSocket> socket;
            try {
                socket = factory ? factory() : make_xai_curl_socket();
            } catch (const std::exception&) {
                socket.reset();
            }
            const auto finish = [registry](
                                    const std::shared_ptr<Session>& finished,
                                    std::optional<Terminal> terminal) {
                registry->finish_slot(finished, std::move(terminal));
            };
            if (!socket) {
                if (session->start_reply) {
                    session->start_reply->fail(
                        ErrorCode::internal_error,
                        std::string(xai_connection_failed));
                }
                finish(session, std::nullopt);
                return;
            }
            run_worker(finish, session, std::move(socket), cancel);
        });
    if (!launched) {
        impl_->finish_slot(session, std::nullopt);
        reply->fail(
            ErrorCode::operation_cancelled, std::string(xai_operation_cancelled));
    }
    return reply;
}

std::shared_ptr<app::OperationReply> XaiVoiceSessions::audio(
    std::string connection_id,
    std::uint64_t request_id,
    std::string session_id,
    std::vector<unsigned char> pcm,
    std::chrono::milliseconds deadline) {
    auto reply = std::make_shared<app::OperationReply>();
    std::shared_ptr<Session> session;
    std::optional<Terminal> terminal;
    {
        std::lock_guard lock(impl_->mu);
        const auto found = impl_->slots.find(key_for(connection_id, session_id));
        if (found == impl_->slots.end()) {
            reply->fail(
                ErrorCode::invalid_argument, "The request was not valid.");
            return reply;
        }
        if (found->second.kind == Impl::Slot::Kind::tombstone) {
            reply->fail(
                ErrorCode::operation_cancelled,
                std::string(xai_operation_cancelled));
            return reply;
        }
        if (found->second.kind == Impl::Slot::Kind::terminal) {
            terminal = Terminal{found->second.code, found->second.message};
            impl_->slots.erase(found);
        } else {
            session = found->second.session;
        }
    }
    if (terminal) {
        reply->fail(terminal->code, terminal->message);
        return reply;
    }
    {
        std::lock_guard lock(session->mu);
        if (session->failure) {
            reply->fail(session->failure->code, session->failure->message);
            return reply;
        }
        if (!session->ready || session->finished || session->audio || session->stop
            || session->cancel.load()) {
            reply->fail(
                ErrorCode::invalid_argument, "The request was not valid.");
            return reply;
        }
        session->audio = Session::AudioCall{
            std::move(pcm), reply, deadline};
        session->audio_request_id.store(request_id);
    }
    return reply;
}

std::shared_ptr<app::OperationReply> XaiVoiceSessions::stop(
    std::string connection_id,
    std::uint64_t request_id,
    std::string session_id,
    std::chrono::milliseconds remaining,
    std::chrono::milliseconds deadline) {
    auto reply = std::make_shared<app::OperationReply>();
    std::shared_ptr<Session> session;
    std::optional<Terminal> terminal;
    {
        std::lock_guard lock(impl_->mu);
        const auto found = impl_->slots.find(key_for(connection_id, session_id));
        if (found == impl_->slots.end()
            || found->second.kind == Impl::Slot::Kind::tombstone) {
            reply->complete(pieces_json(session_id, {}));
            return reply;
        }
        if (found->second.kind == Impl::Slot::Kind::terminal) {
            terminal = Terminal{found->second.code, found->second.message};
            impl_->slots.erase(found);
        } else {
            session = found->second.session;
        }
    }
    if (terminal) {
        reply->fail(terminal->code, terminal->message);
        return reply;
    }
    {
        std::lock_guard lock(session->mu);
        if (session->failure) {
            reply->fail(session->failure->code, session->failure->message);
            return reply;
        }
        if (session->stop || session->finished) {
            reply->complete(pieces_json(session_id, {}));
            return reply;
        }
        if (!session->ready || session->cancel.load()) {
            reply->fail(
                ErrorCode::invalid_argument, "The request was not valid.");
            return reply;
        }
        Session::StopCall stop;
        stop.reply = reply;
        stop.deadline = clock::now() + xai_stop_limit(remaining, deadline);
        session->stop = std::move(stop);
        session->stop_request_id.store(request_id);
    }
    return reply;
}

void XaiVoiceSessions::cancel(
    std::string_view connection_id,
    std::string_view session_id) {
    std::lock_guard lock(impl_->mu);
    const SessionKey key = key_for(connection_id, session_id);
    const auto found = impl_->slots.find(key);
    if (found == impl_->slots.end()) {
        impl_->slots.insert_or_assign(key, Impl::Slot{});
        return;
    }
    if (found->second.kind == Impl::Slot::Kind::tombstone) return;
    if (found->second.kind == Impl::Slot::Kind::terminal) {
        impl_->slots.erase(found);
        return;
    }
    if (found->second.session) found->second.session->cancel.store(true);
}

void XaiVoiceSessions::cancel_live(
    std::string_view connection_id,
    std::string_view session_id) {
    std::lock_guard lock(impl_->mu);
    const auto found = impl_->slots.find(key_for(connection_id, session_id));
    if (found == impl_->slots.end()
        || found->second.kind != Impl::Slot::Kind::live) {
        return;
    }
    if (found->second.session) found->second.session->cancel.store(true);
}

void XaiVoiceSessions::cancel_connection(std::string_view connection_id) {
    std::vector<std::shared_ptr<Session>> live;
    {
        std::lock_guard lock(impl_->mu);
        for (auto found = impl_->slots.begin(); found != impl_->slots.end();) {
            if (found->first.first != connection_id) {
                ++found;
                continue;
            }
            if (found->second.session) live.push_back(found->second.session);
            found = impl_->slots.erase(found);
        }
        impl_->active_by_connection.erase(std::string(connection_id));
    }
    for (const auto& session : live) session->cancel.store(true);
}

void XaiVoiceSessions::cancel_all() {
    std::vector<std::shared_ptr<Session>> live;
    {
        std::lock_guard lock(impl_->mu);
        for (auto& [key, slot] : impl_->slots) {
            if (slot.session) live.push_back(slot.session);
        }
        impl_->slots.clear();
        impl_->active_by_connection.clear();
    }
    for (const auto& session : live) session->cancel.store(true);
}

void XaiVoiceSessions::expire(
    std::string_view connection_id,
    std::uint64_t request_id) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(impl_->mu);
        const auto active = impl_->active_by_connection.find(std::string(connection_id));
        if (active == impl_->active_by_connection.end()) return;
        const auto found = impl_->slots.find(key_for(connection_id, active->second));
        if (found == impl_->slots.end()) return;
        session = found->second.session;
    }
    if (!session) return;
    if (session->start_request_id.load() == request_id
        || session->audio_request_id.load() == request_id
        || session->stop_request_id.load() == request_id) {
        session->timed_out.store(true);
        session->cancel.store(true);
    }
}

} // namespace cha::media
