#include "bridge/bridge_router.h"

#include "session/not_found_error.h"
#include "util/path_name.h"
#include "web/json.h"
#include "web/live_session.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cha::bridge {
namespace {

using cha::web::ErrorCode;
using cha::web::LiveSessionHandle;

struct SessionIdentity {
    std::string forum_id;
    std::string session_id;
};

std::string require_identifier(
    const nlohmann::json& params,
    std::string_view key) {
    if (!params.contains(std::string(key)) || !params[std::string(key)].is_string()) {
        throw std::invalid_argument("The request was not valid.");
    }
    const std::string& value =
        params[std::string(key)].get_ref<const std::string&>();
    if (!cha::is_url_safe_identifier(value)) {
        throw std::invalid_argument("The request was not valid.");
    }
    return value;
}

void require_only_keys(
    const nlohmann::json& params,
    std::initializer_list<std::string_view> keys) {
    // Zero-param methods still require an empty object, not omitted keys.
    if (!params.is_object() || params.size() != keys.size()) {
        throw std::invalid_argument("The request was not valid.");
    }
    for (std::string_view key : keys) {
        if (!params.contains(std::string(key))) {
            throw std::invalid_argument("The request was not valid.");
        }
    }
}

SessionIdentity parse_session_identity(const nlohmann::json& params) {
    require_only_keys(params, {"forum_id", "session_id"});
    return {require_identifier(params, "forum_id"),
            require_identifier(params, "session_id")};
}

nlohmann::json encode_command_result(const cha::web::CommandSubmitResult& result) {
    if (const auto* error = std::get_if<ErrorCode>(&result)) {
        throw *error;
    }
    if (const auto* command = std::get_if<cha::web::CommandResult>(&result)) {
        return nlohmann::json(*command);
    }
    if (const auto* snapshot = std::get_if<cha::web::SessionSnapshot>(&result)) {
        return nlohmann::json(*snapshot);
    }
    if (const auto* subscribed =
            std::get_if<cha::web::SubscribeResult>(&result)) {
        return {
            {"connection_id", subscribed->connection_id},
            {"context_epoch", subscribed->context_epoch},
            {"subscription_id", subscribed->subscription_id},
        };
    }
    if (const auto* label = std::get_if<cha::web::SessionLabelResult>(&result)) {
        return nlohmann::json(*label);
    }
    throw ErrorCode::internal_error;
}

nlohmann::json encode_output_item(
    std::string_view connection_id,
    const LiveSessionHandle& session,
    std::string_view subscription_id,
    std::uint64_t context_epoch,
    const cha::app::SessionOutputItem& item) {
    const std::string event = item.kind == cha::app::SessionOutputItem::Kind::snapshot
        ? "session.snapshot"
        : "session.append";
    nlohmann::json payload;
    if (item.kind == cha::app::SessionOutputItem::Kind::snapshot) {
        payload = item.snapshot;
    } else {
        payload = cha::web::AppendEvent{item.target, item.text, item.seq};
        payload.erase("seq");
    }
    return session_event(
        connection_id,
        context_epoch,
        subscription_id,
        event,
        session->identity().forum_id,
        session->identity().session_id,
        item.seq,
        std::move(payload));
}

} // namespace

struct BridgeRouter::Impl : std::enable_shared_from_this<Impl> {
    struct Outstanding {
        Method method{Method::bridge_info};
        bool control{};
        std::chrono::steady_clock::time_point deadline{};
        std::shared_ptr<cha::web::CommandReply> reply;
        std::string forum_id;
        std::string session_id;
        std::string subscription_id;
        std::uint64_t context_epoch{};
    };

    struct ActiveSubscription {
        std::string subscription_id;
        std::string forum_id;
        std::string session_id;
        std::uint64_t context_epoch{};
        LiveSessionHandle session;
    };

    struct InFlight {
        std::uint64_t delivery_id{};
        bool has_session_event{};
        LiveSessionHandle session;
    };

    struct Connection {
        std::string id;
        bool invalid{};
        // Dev leftover: not wrapped at kMaxSafeInteger; replace the connection.
        std::uint64_t next_delivery_id{1};
        std::unordered_set<std::uint64_t> used_ids;
        std::unordered_map<std::uint64_t, Outstanding> outstanding;
        std::string latest_subscription_id;
        std::optional<ActiveSubscription> active;
        std::deque<nlohmann::json> pending_replies;
        // At most one taken SessionOutput item. Merge, snapshot fallback,
        // byte threshold, and dirty replacement stay in SessionOutput.
        std::optional<nlohmann::json> pending_session;
        std::optional<InFlight> in_flight;
        std::size_t ordinary_count{};
        std::size_t control_count{};
    };

    Impl(cha::app::Application& application, Options options)
        : application(application),
          options(std::move(options)) {
        if (!this->options.clock) {
            this->options.clock = [] {
                return std::chrono::steady_clock::now();
            };
        }
    }

    cha::app::Application& application;
    Options options;
    std::mutex mutex;
    std::condition_variable work;
    std::uint64_t next_connection{1};
    bool stopping{};
    std::unordered_map<std::string, std::shared_ptr<Connection>> connections;
    std::deque<std::function<void()>> ordinary_tasks;
    std::deque<std::function<void()>> control_tasks;

    std::chrono::steady_clock::time_point now() const {
        return options.clock();
    }

    std::shared_ptr<Connection> find_connection(
        std::string_view id,
        bool require_valid = true) {
        const auto found = connections.find(std::string(id));
        if (found == connections.end()) return {};
        if (require_valid && found->second->invalid) return {};
        return found->second;
    }

    void notify() {
        work.notify_all();
    }

    nlohmann::json error_reply(
        std::string_view connection_id,
        std::uint64_t id,
        std::uint64_t epoch,
        ErrorCode code,
        std::string_view message = {}) {
        return reply_error(
            connection_id,
            id,
            epoch,
            code,
            message.empty() ? public_error_message(code) : message);
    }

    void queue_message(const std::shared_ptr<Connection>& connection, nlohmann::json message) {
        connection->pending_replies.push_back(std::move(message));
        notify();
    }

    void finish_request(
        const std::shared_ptr<Connection>& connection,
        std::uint64_t id,
        nlohmann::json message) {
        const auto found = connection->outstanding.find(id);
        if (found == connection->outstanding.end()) return;
        if (found->second.control) --connection->control_count;
        else --connection->ordinary_count;
        connection->outstanding.erase(found);
        connection->used_ids.insert(id);
        queue_message(connection, std::move(message));
    }

    void fail_request(
        const std::shared_ptr<Connection>& connection,
        std::uint64_t id,
        std::uint64_t epoch,
        ErrorCode code,
        std::string_view message = {}) {
        finish_request(
            connection,
            id,
            error_reply(connection->id, id, epoch, code, message));
    }

    void invalidate(
        const std::shared_ptr<Connection>& connection,
        bool drop_pending = true) {
        connection->invalid = true;
        for (auto& [id, outstanding] : connection->outstanding) {
            if (outstanding.reply) outstanding.reply->abandon();
        }
        connection->outstanding.clear();
        connection->ordinary_count = 0;
        connection->control_count = 0;
        if (drop_pending) {
            connection->pending_replies.clear();
            connection->pending_session.reset();
            if (connection->in_flight && connection->in_flight->has_session_event
                && connection->in_flight->session) {
                connection->in_flight->session->acknowledge_output();
            }
            connection->in_flight.reset();
        }
        if (connection->active && connection->active->session) {
            (void)connection->active->session->enqueue(cha::web::UnsubscribeCommand{
                connection->id,
                connection->active->context_epoch,
                connection->active->subscription_id});
        }
        connection->active.reset();
        notify();
    }

    void admit_error(
        const std::shared_ptr<Connection>& connection,
        std::optional<std::uint64_t> id,
        std::uint64_t epoch,
        ErrorCode code,
        std::string_view message = {}) {
        if (!id) return;
        queue_message(
            connection,
            error_reply(connection->id, *id, epoch, code, message));
    }

    std::optional<ErrorCode> check_epoch(Method method, std::uint64_t epoch) {
        if (!requires_context_epoch(method)) return std::nullopt;
        return application.check_context(epoch);
    }

    void enqueue_task(std::function<void()> task, bool control) {
        if (control) control_tasks.push_back(std::move(task));
        else ordinary_tasks.push_back(std::move(task));
        notify();
    }

    void complete_session(
        std::string connection_id,
        std::uint64_t id) {
        std::shared_ptr<Connection> connection;
        Outstanding outstanding;
        {
            std::lock_guard lock(mutex);
            connection = find_connection(connection_id);
            if (!connection) return;
            const auto found = connection->outstanding.find(id);
            if (found == connection->outstanding.end() || !found->second.reply) {
                return;
            }
            outstanding = found->second;
        }
        const auto peeked = outstanding.reply->peek();
        if (!peeked) return;
        try {
            nlohmann::json result = encode_command_result(*peeked);
            std::lock_guard lock(mutex);
            connection = find_connection(connection_id);
            if (!connection) return;
            if (outstanding.method == Method::session_subscribe) {
                const auto* subscribed =
                    std::get_if<cha::web::SubscribeResult>(&*peeked);
                if (subscribed
                    && subscribed->subscription_id
                        == connection->latest_subscription_id) {
                    auto session = application.live_sessions().lookup(
                        {outstanding.forum_id, outstanding.session_id});
                    if (session) {
                        connection->active = ActiveSubscription{
                            subscribed->subscription_id,
                            outstanding.forum_id,
                            outstanding.session_id,
                            subscribed->context_epoch,
                            std::move(session)};
                    }
                }
            }
            if (outstanding.method == Method::session_unsubscribe
                && connection->active
                && connection->active->subscription_id
                    == outstanding.subscription_id) {
                connection->active.reset();
            }
            finish_request(
                connection,
                id,
                reply_ok(
                    connection->id,
                    id,
                    outstanding.context_epoch,
                    std::move(result)));
        } catch (const ErrorCode code) {
            std::lock_guard lock(mutex);
            connection = find_connection(connection_id);
            if (!connection) return;
            fail_request(connection, id, outstanding.context_epoch, code);
        }
        pump();
    }

    void pump() {
        // Take at most one item per connection. Further coalescing remains in
        // LiveSession's SessionOutput until this slot is free again.
        struct PendingTake {
            std::shared_ptr<Connection> connection;
            LiveSessionHandle session;
            std::string subscription_id;
            std::uint64_t context_epoch{};
        };
        std::vector<PendingTake> takes;
        {
            std::lock_guard lock(mutex);
            for (auto& [id, connection] : connections) {
                if (connection->invalid || connection->pending_session
                    || !connection->active) {
                    continue;
                }
                takes.push_back({
                    connection,
                    connection->active->session,
                    connection->active->subscription_id,
                    connection->active->context_epoch});
            }
        }
        for (auto& take : takes) {
            auto item = take.session->take_output();
            if (!item) continue;
            std::lock_guard lock(mutex);
            if (take.connection->invalid || !take.connection->active
                || take.connection->active->subscription_id != take.subscription_id
                || take.connection->pending_session) {
                take.session->acknowledge_output();
                continue;
            }
            take.connection->pending_session = encode_output_item(
                take.connection->id,
                take.session,
                take.subscription_id,
                take.context_epoch,
                *item);
            notify();
        }
    }

    std::shared_ptr<cha::web::CommandReply> start_session_command(
        const std::shared_ptr<Connection>& connection,
        std::uint64_t id,
        Outstanding outstanding,
        cha::web::WebCommand command) {
        auto outcome = application.submit_async(
            outstanding.forum_id, outstanding.session_id, std::move(command));
        if (const auto* error = std::get_if<ErrorCode>(&outcome)) {
            fail_request(connection, id, outstanding.context_epoch, *error);
            return {};
        }
        auto reply = std::get<std::shared_ptr<cha::web::CommandReply>>(
            std::move(outcome));
        outstanding.reply = reply;
        connection->outstanding.insert_or_assign(id, outstanding);
        return reply;
    }

    void run_ordinary(
        std::string connection_id,
        std::uint64_t id,
        std::uint64_t epoch,
        Method method,
        nlohmann::json params) {
        auto fail = [&](ErrorCode code, std::string_view message = {}) {
            std::lock_guard lock(mutex);
            auto connection = find_connection(connection_id);
            if (connection) fail_request(connection, id, epoch, code, message);
        };
        try {
            if (const auto context = check_epoch(method, epoch)) {
                fail(*context);
                return;
            }
            nlohmann::json result = nlohmann::json::object();
            switch (method) {
            case Method::bridge_info:
                require_only_keys(params, {});
                result = bridge_info_result(options.platform);
                break;
            case Method::app_bootstrap: {
                require_only_keys(params, {});
                const auto boot = application.bootstrap();
                result = {
                    {"state",
                     boot.state == cha::app::ApplicationState::ready
                         ? "ready"
                         : "unavailable"},
                    {"context_epoch", boot.context_epoch},
                    {"bootstrap", boot.presentation},
                };
                epoch = boot.context_epoch;
                break;
            }
            case Method::session_create: {
                require_only_keys(params, {"forum_id", "label"});
                const std::string forum_id = require_identifier(params, "forum_id");
                if (!params["label"].is_string()) {
                    throw std::invalid_argument("The request was not valid.");
                }
                result = application.create_session(
                    forum_id, params["label"].get<std::string>());
                break;
            }
            case Method::session_open: {
                // Waits up to open_deadline on this worker. Control tasks use a
                // separate deque so they are not queued behind unstarted opens.
                const auto identity = parse_session_identity(params);
                auto opened = application.open_session(
                    identity.forum_id, identity.session_id);
                if (const auto* error = std::get_if<ErrorCode>(&opened)) {
                    fail(*error);
                    return;
                }
                result = std::get<cha::web::OpenSessionSuccess>(opened);
                break;
            }
            case Method::session_close: {
                const auto identity = parse_session_identity(params);
                // Missing sessions are a successful no-op.
                application.close_session(identity.forum_id, identity.session_id);
                result = nlohmann::json::object();
                break;
            }
            default:
                fail(ErrorCode::invalid_argument, "That method is not available.");
                return;
            }
            std::lock_guard lock(mutex);
            auto connection = find_connection(connection_id);
            if (!connection) return;
            finish_request(
                connection,
                id,
                reply_ok(connection->id, id, epoch, std::move(result)));
        } catch (const ErrorCode code) {
            fail(code);
        } catch (const cha::SessionNotFoundError&) {
            fail(ErrorCode::not_found);
        } catch (const cha::ForumNotFoundError&) {
            fail(ErrorCode::not_found);
        } catch (const std::invalid_argument&) {
            fail(ErrorCode::invalid_argument);
        } catch (const std::runtime_error&) {
            fail(application.running()
                ? ErrorCode::internal_error
                : ErrorCode::application_unavailable);
        } catch (...) {
            fail(ErrorCode::internal_error);
        }
    }

    void run_session(
        std::string connection_id,
        std::uint64_t id,
        Outstanding outstanding,
        nlohmann::json params) {
        std::shared_ptr<cha::web::CommandReply> reply;
        {
            std::lock_guard lock(mutex);
            auto connection = find_connection(connection_id);
            if (!connection) return;
            const auto found = connection->outstanding.find(id);
            if (found == connection->outstanding.end()) return;
            try {
                if (const auto context =
                        check_epoch(outstanding.method, outstanding.context_epoch)) {
                    fail_request(
                        connection, id, outstanding.context_epoch, *context);
                    return;
                }
                cha::web::WebCommand command;
                switch (outstanding.method) {
                case Method::session_submit: {
                    require_only_keys(params, {"forum_id", "session_id", "input"});
                    outstanding.forum_id = require_identifier(params, "forum_id");
                    outstanding.session_id = require_identifier(params, "session_id");
                    auto input = cha::web::parse_input_command(params["input"]);
                    if (input.text.size() > application.settings().prompt_limit) {
                        fail_request(
                            connection,
                            id,
                            outstanding.context_epoch,
                            ErrorCode::prompt_too_large);
                        return;
                    }
                    command = std::move(input);
                    break;
                }
                case Method::session_stop: {
                    const auto identity = parse_session_identity(params);
                    outstanding.forum_id = identity.forum_id;
                    outstanding.session_id = identity.session_id;
                    command = cha::web::StopCommand{};
                    break;
                }
                case Method::session_snapshot: {
                    const auto identity = parse_session_identity(params);
                    outstanding.forum_id = identity.forum_id;
                    outstanding.session_id = identity.session_id;
                    command = cha::web::SnapshotCommand{};
                    break;
                }
                case Method::session_subscribe: {
                    require_only_keys(
                        params, {"forum_id", "session_id", "subscription_id"});
                    outstanding.forum_id = require_identifier(params, "forum_id");
                    outstanding.session_id = require_identifier(params, "session_id");
                    outstanding.subscription_id =
                        require_identifier(params, "subscription_id");
                    if (outstanding.subscription_id
                        != connection->latest_subscription_id) {
                        fail_request(
                            connection,
                            id,
                            outstanding.context_epoch,
                            ErrorCode::operation_cancelled);
                        return;
                    }
                    command = cha::web::SubscribeCommand{
                        connection->id,
                        outstanding.context_epoch,
                        outstanding.subscription_id};
                    break;
                }
                case Method::session_unsubscribe: {
                    require_only_keys(
                        params, {"forum_id", "session_id", "subscription_id"});
                    outstanding.forum_id = require_identifier(params, "forum_id");
                    outstanding.session_id = require_identifier(params, "session_id");
                    outstanding.subscription_id =
                        require_identifier(params, "subscription_id");
                    command = cha::web::UnsubscribeCommand{
                        connection->id,
                        outstanding.context_epoch,
                        outstanding.subscription_id};
                    break;
                }
                default:
                    fail_request(
                        connection,
                        id,
                        outstanding.context_epoch,
                        ErrorCode::invalid_argument,
                        "That method is not available.");
                    return;
                }
                reply = start_session_command(
                    connection, id, std::move(outstanding), std::move(command));
            } catch (const std::invalid_argument&) {
                fail_request(
                    connection,
                    id,
                    outstanding.context_epoch,
                    ErrorCode::invalid_argument);
                return;
            }
        }
        if (!reply) return;
        auto weak = std::weak_ptr<Impl>(shared_from_this());
        reply->set_ready_callback([weak, connection_id, id] {
            if (auto impl = weak.lock()) impl->complete_session(connection_id, id);
        });
    }

    bool is_session_method(Method method) const {
        return method == Method::session_submit
            || method == Method::session_stop
            || method == Method::session_snapshot
            || method == Method::session_subscribe
            || method == Method::session_unsubscribe;
    }

    void handle_parsed(
        const std::shared_ptr<Connection>& connection,
        ParsedRequest request) {
        if (request.connection_id != connection->id) {
            admit_error(
                connection,
                request.id,
                request.context_epoch,
                ErrorCode::invalid_argument);
            return;
        }
        if (connection->outstanding.contains(request.id)) {
            admit_error(
                connection,
                request.id,
                request.context_epoch,
                ErrorCode::invalid_argument,
                "Duplicate outstanding request id.");
            return;
        }
        if (connection->used_ids.contains(request.id)) {
            admit_error(
                connection,
                request.id,
                request.context_epoch,
                ErrorCode::invalid_argument,
                "Request ids cannot be reused.");
            return;
        }
        const bool control = is_control_method(request.method);
        const std::size_t& count =
            control ? connection->control_count : connection->ordinary_count;
        const std::size_t limit =
            control ? options.control_limit : options.ordinary_limit;
        if (count >= limit) {
            if (!control) {
                queue_message(
                    connection,
                    error_reply(
                        connection->id,
                        request.id,
                        request.context_epoch,
                        ErrorCode::invalid_argument,
                        "Too many in-flight requests."));
                invalidate(connection, false);
            } else {
                admit_error(
                    connection,
                    request.id,
                    request.context_epoch,
                    ErrorCode::command_queue_full);
            }
            return;
        }
        if (const auto context =
                check_epoch(request.method, request.context_epoch)) {
            admit_error(
                connection, request.id, request.context_epoch, *context);
            return;
        }
        Outstanding outstanding;
        outstanding.method = request.method;
        outstanding.control = control;
        outstanding.deadline = now()
            + options.command_deadline.value_or(
                application.settings().command_deadline);
        outstanding.context_epoch = request.context_epoch;
        if (request.method == Method::session_subscribe) {
            try {
                outstanding.subscription_id =
                    require_identifier(request.params, "subscription_id");
            } catch (const std::invalid_argument&) {
                admit_error(
                    connection,
                    request.id,
                    request.context_epoch,
                    ErrorCode::invalid_argument);
                return;
            }
            connection->latest_subscription_id = outstanding.subscription_id;
        }
        connection->outstanding.emplace(request.id, outstanding);
        if (control) ++connection->control_count;
        else ++connection->ordinary_count;
        auto weak = std::weak_ptr<Impl>(shared_from_this());
        const std::string connection_id = connection->id;
        const auto id = request.id;
        const auto epoch = request.context_epoch;
        const auto method = request.method;
        auto params = std::move(request.params);
        enqueue_task(
            [weak, connection_id, id, epoch, method, params = std::move(params), outstanding]() mutable {
                auto impl = weak.lock();
                if (!impl) return;
                if (impl->is_session_method(method)) {
                    impl->run_session(connection_id, id, std::move(outstanding), params);
                } else {
                    impl->run_ordinary(connection_id, id, epoch, method, params);
                }
            },
            control);
    }
};

BridgeRouter::BridgeRouter(cha::app::Application& application)
    : BridgeRouter(application, Options{}) {}

BridgeRouter::BridgeRouter(cha::app::Application& application, Options options)
    : impl_(std::make_shared<Impl>(application, std::move(options))) {}

BridgeRouter::~BridgeRouter() {
    shutdown();
}

std::string BridgeRouter::open_connection() {
    std::lock_guard lock(impl_->mutex);
    std::string id = "view-" + std::to_string(impl_->next_connection++);
    auto connection = std::make_shared<Impl::Connection>();
    connection->id = id;
    impl_->connections.emplace(id, connection);
    return id;
}

void BridgeRouter::close_connection(std::string_view connection_id) {
    std::lock_guard lock(impl_->mutex);
    auto connection = impl_->find_connection(connection_id);
    if (!connection) return;
    impl_->invalidate(connection);
    impl_->connections.erase(std::string(connection_id));
}

void BridgeRouter::handle_request(
    std::string_view trusted_connection_id,
    std::string_view json) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) return;
    auto connection = impl_->find_connection(trusted_connection_id);
    if (!connection) return;
    auto parsed = parse_request(json, impl_->options.request_bytes_limit);
    if (const auto* failure = std::get_if<ParseFailure>(&parsed)) {
        impl_->admit_error(
            connection,
            failure->id,
            0,
            failure->code,
            failure->message);
        return;
    }
    impl_->handle_parsed(connection, std::get<ParsedRequest>(std::move(parsed)));
}

void BridgeRouter::handle_ack(
    std::string_view trusted_connection_id,
    std::string_view json) {
    LiveSessionHandle session;
    {
        std::lock_guard lock(impl_->mutex);
        auto connection = impl_->find_connection(trusted_connection_id, false);
        if (!connection || !connection->in_flight) return;
        auto parsed = parse_ack(json, impl_->options.request_bytes_limit);
        const auto* ack = std::get_if<DeliveryAck>(&parsed);
        if (!ack || ack->connection_id != connection->id
            || ack->delivery_id != connection->in_flight->delivery_id) {
            return;
        }
        if (connection->in_flight->has_session_event) {
            session = connection->in_flight->session;
        }
        connection->in_flight.reset();
        impl_->notify();
    }
    if (session) session->acknowledge_output();
    impl_->pump();
}

void BridgeRouter::run_tasks() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->control_tasks.empty()) {
                task = std::move(impl_->control_tasks.front());
                impl_->control_tasks.pop_front();
            } else if (!impl_->ordinary_tasks.empty()) {
                task = std::move(impl_->ordinary_tasks.front());
                impl_->ordinary_tasks.pop_front();
            } else {
                return;
            }
        }
        task();
    }
}

void BridgeRouter::expire_timeouts() {
    std::vector<std::pair<std::shared_ptr<Impl::Connection>, std::uint64_t>> expired;
    {
        std::lock_guard lock(impl_->mutex);
        const auto now = impl_->now();
        for (auto& [id, connection] : impl_->connections) {
            if (connection->invalid) continue;
            for (const auto& [request_id, outstanding] : connection->outstanding) {
                if (outstanding.deadline <= now) {
                    expired.emplace_back(connection, request_id);
                }
            }
        }
        for (auto& [connection, request_id] : expired) {
            const auto found = connection->outstanding.find(request_id);
            if (found == connection->outstanding.end()) continue;
            if (found->second.reply) found->second.reply->abandon();
            const auto epoch = found->second.context_epoch;
            impl_->fail_request(
                connection, request_id, epoch, ErrorCode::command_timeout);
        }
    }
}

void BridgeRouter::pump_output() {
    impl_->pump();
}

std::optional<nlohmann::json> BridgeRouter::take_delivery(
    std::string_view connection_id) {
    expire_timeouts();
    pump_output();
    std::lock_guard lock(impl_->mutex);
    auto connection = impl_->find_connection(connection_id, false);
    if (!connection || connection->in_flight) return std::nullopt;
    std::vector<nlohmann::json> messages;
    while (!connection->pending_replies.empty()) {
        messages.push_back(std::move(connection->pending_replies.front()));
        connection->pending_replies.pop_front();
    }
    Impl::InFlight in_flight;
    // Dev leftover: replace the connection before JS-safe integer exhaustion.
    in_flight.delivery_id = connection->next_delivery_id++;
    if (connection->pending_session) {
        messages.push_back(*std::move(connection->pending_session));
        connection->pending_session.reset();
        in_flight.has_session_event = true;
        if (connection->active) in_flight.session = connection->active->session;
    }
    if (messages.empty()) return std::nullopt;
    connection->in_flight = std::move(in_flight);
    return delivery_batch(
        connection->id, connection->in_flight->delivery_id, std::move(messages));
}

bool BridgeRouter::wait_for_work(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->work.wait_for(lock, timeout, [&] {
        if (impl_->stopping) return true;
        if (!impl_->ordinary_tasks.empty() || !impl_->control_tasks.empty()) {
            return true;
        }
        for (const auto& [id, connection] : impl_->connections) {
            if (!connection->pending_replies.empty()
                || connection->pending_session) {
                return true;
            }
        }
        return false;
    });
}

void BridgeRouter::shutdown() {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
    impl_->ordinary_tasks.clear();
    impl_->control_tasks.clear();
    for (auto& [id, connection] : impl_->connections) {
        impl_->invalidate(connection);
    }
    impl_->notify();
}

} // namespace cha::bridge
