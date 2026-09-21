#include "bridge/bridge_router.h"

#include "app/media_operations.h"
#include "app/vault_operations.h"
#include "bridge/operation_dispatch.h"
#include "bridge/request_params.h"
#include "storage/not_found_error.h"
#include "app/application_config.h"
#include "media/audio_download.h"
#include "providers/fish_audio.h"
#include "runtime/request_parser.h"
#include "runtime/live_session.h"
#include "util/logging.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cha::bridge {
namespace {

struct SessionIdentity {
    std::string forum_id;
    std::string session_id;
};

SessionIdentity parse_session_identity(const nlohmann::json& params) {
    require_only_keys(params, {"forum_id", "session_id"});
    return {require_identifier(params, "forum_id"),
            require_identifier(params, "session_id")};
}

std::string optional_string(const nlohmann::json& params, std::string_view key) {
    const std::string name(key);
    if (!params.contains(name) || params[name].is_null()) return {};
    if (!params[name].is_string()) {
        throw std::invalid_argument("The request was not valid.");
    }
    return params[name].get<std::string>();
}

std::optional<std::string> nullable_string(
    const nlohmann::json& params,
    std::string_view key) {
    const std::string name(key);
    if (!params.contains(name) || params[name].is_null()) return std::nullopt;
    if (!params[name].is_string()) {
        throw std::invalid_argument("The request was not valid.");
    }
    const std::string value = params[name].get<std::string>();
    if (value.empty()) return std::nullopt;
    return value;
}

std::uint64_t require_safe_id(const nlohmann::json& params, std::string_view key) {
    const std::string name(key);
    if (!params.contains(name)) {
        throw std::invalid_argument("The request was not valid.");
    }
    const auto value = as_safe_uint(params[name]);
    if (!value || *value == 0) {
        throw std::invalid_argument("The request was not valid.");
    }
    return *value;
}

FishAudioSynthesis parse_synthesis_fields(const nlohmann::json& params) {
    return decode_fish_audio_synthesis(params);
}

nlohmann::json encode_command_result(const CommandSubmitResult& result) {
    if (const auto* error = std::get_if<ErrorCode>(&result)) {
        throw *error;
    }
    if (const auto* command = std::get_if<CommandResult>(&result)) {
        return nlohmann::json(*command);
    }
    if (const auto* snapshot = std::get_if<SessionSnapshot>(&result)) {
        return nlohmann::json(*snapshot);
    }
    if (const auto* subscribed =
            std::get_if<SubscribeResult>(&result)) {
        return {
            {"connection_id", subscribed->connection_id},
            {"context_epoch", subscribed->context_epoch},
            {"subscription_id", subscribed->subscription_id},
        };
    }
    if (const auto* label = std::get_if<SessionLabelResult>(&result)) {
        return nlohmann::json(*label);
    }
    throw ErrorCode::internal_error;
}

nlohmann::json encode_output_item(
    std::string_view connection_id,
    const LiveSessionHandle& session,
    std::string_view subscription_id,
    std::uint64_t context_epoch,
    const app::SessionOutputItem& item) {
    const std::string event = item.kind == app::SessionOutputItem::Kind::snapshot
        ? "session.snapshot"
        : "session.append";
    nlohmann::json payload;
    if (item.kind == app::SessionOutputItem::Kind::snapshot) {
        payload = item.snapshot;
    } else {
        payload = AppendEvent{item.target, item.text, item.seq};
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
        bool cancelled{};
        std::chrono::steady_clock::time_point deadline{};
        std::shared_ptr<CommandReply> reply;
        std::shared_ptr<app::OperationReply> operation;
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

    enum class ReplyCapacity { ordinary, control, unadmitted };

    struct PendingReply {
        nlohmann::json message;
        ReplyCapacity capacity{ReplyCapacity::unadmitted};
        bool context_bound{};
    };

    struct InFlight {
        std::uint64_t delivery_id{};
        std::size_t ordinary_replies{};
        std::size_t control_replies{};
        std::size_t unadmitted_replies{};
        bool has_session_event{};
        LiveSessionHandle session;
    };

    struct Connection {
        std::string id;
        bool invalid{};
        std::uint64_t next_delivery_id{1};
        std::optional<std::uint64_t> highest_request_id;
        std::unordered_map<std::uint64_t, Outstanding> outstanding;
        std::string latest_subscription_id;
        std::uint64_t latest_subscription_epoch{};
        std::optional<ActiveSubscription> active;
        std::deque<PendingReply> pending_replies;
        std::optional<nlohmann::json> pending_context;
        std::optional<nlohmann::json> pending_invalidation;
        // At most one taken SessionOutput item. Merge, snapshot fallback,
        // byte threshold, and dirty replacement stay in SessionOutput.
        std::optional<nlohmann::json> pending_session;
        std::optional<InFlight> in_flight;
        std::size_t ordinary_count{};
        std::size_t control_count{};
        std::size_t unadmitted_count{};
    };

    Impl(app::Application& application, Options options)
        : application(application),
          options(std::move(options)) {
        if (!this->options.clock) {
            this->options.clock = [] {
                return std::chrono::steady_clock::now();
            };
        }
    }

    void listen_for_context_changes() {
        auto weak = std::weak_ptr<Impl>(shared_from_this());
        application.set_context_changed(
            [weak](std::uint64_t epoch, app::ApplicationState state) {
                if (auto impl = weak.lock()) {
                    impl->publish_context_changed(epoch, state);
                }
            });
    }

    void publish_context_changed(
        std::uint64_t epoch,
        app::ApplicationState state) {
        std::lock_guard lock(mutex);
        if (notified_epoch == epoch && notified_state == state) return;
        notified_epoch = epoch;
        notified_state = state;
        const auto name = app::application_state_name(state);
        for (auto& [id, connection] : connections) {
            (void)id;
            if (connection->invalid) continue;
            connection->pending_context =
                context_changed_event(connection->id, epoch, name);
        }
        notify();
    }

    app::Application& application;
    Options options;
    std::mutex mutex;
    std::mutex ready_tasks_mutex;
    std::condition_variable work;
    std::uint64_t next_connection{1};
    bool stopping{};
    std::unordered_map<std::string, std::shared_ptr<Connection>> connections;
    std::deque<std::function<void()>> ordinary_tasks;
    std::deque<std::function<void()>> control_tasks;
    std::deque<std::function<void()>> ready_tasks;
    std::uint64_t notified_epoch{};
    app::ApplicationState notified_state{app::ApplicationState::running};

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

    bool has_ready_tasks() {
        std::lock_guard lock(ready_tasks_mutex);
        return !ready_tasks.empty();
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

    void queue_reply(
        const std::shared_ptr<Connection>& connection,
        nlohmann::json message,
        ReplyCapacity capacity,
        bool context_bound = false) {
        connection->pending_replies.push_back(
            {std::move(message), capacity, context_bound});
        notify();
    }

    void finish_request(
        const std::shared_ptr<Connection>& connection,
        std::uint64_t id,
        nlohmann::json message) {
        const auto found = connection->outstanding.find(id);
        if (found == connection->outstanding.end()) return;
        const ReplyCapacity capacity = found->second.control
            ? ReplyCapacity::control
            : ReplyCapacity::ordinary;
        const auto method = found->second.method;
        const bool context_bound = requires_context_epoch(method)
            && !changes_context(method);
        connection->outstanding.erase(found);
        queue_reply(connection, std::move(message), capacity, context_bound);
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
        const bool first_invalidation = !connection->invalid;
        connection->invalid = true;
        for (auto& [id, outstanding] : connection->outstanding) {
            if (outstanding.reply) outstanding.reply->abandon();
            if (outstanding.operation) outstanding.operation->abandon();
            if (outstanding.method == Method::session_subscribe
                && outstanding.reply) {
                auto session = application.subscription_handle(
                    outstanding.forum_id, outstanding.session_id);
                if (session) {
                    (void)session->enqueue(UnsubscribeCommand{
                        connection->id,
                        outstanding.context_epoch,
                        outstanding.subscription_id});
                }
            }
            if (!drop_pending) {
                connection->pending_replies.push_back({
                    error_reply(
                        connection->id,
                        id,
                        outstanding.context_epoch,
                        ErrorCode::operation_cancelled),
                    outstanding.control
                        ? ReplyCapacity::control
                        : ReplyCapacity::ordinary});
            }
        }
        connection->outstanding.clear();
        connection->pending_context.reset();
        if (!drop_pending && first_invalidation) {
            connection->pending_invalidation =
                connection_invalidated_event(connection->id);
        }
        if (connection->pending_session && connection->active
            && connection->active->session) {
            connection->active->session->acknowledge_output();
        }
        connection->pending_session.reset();
        if (drop_pending) {
            connection->pending_replies.clear();
            connection->pending_invalidation.reset();
            if (connection->in_flight && connection->in_flight->has_session_event
                && connection->in_flight->session) {
                connection->in_flight->session->acknowledge_output();
            }
            connection->in_flight.reset();
            connection->ordinary_count = 0;
            connection->control_count = 0;
            connection->unadmitted_count = 0;
        }
        if (connection->active && connection->active->session) {
            (void)connection->active->session->enqueue(UnsubscribeCommand{
                connection->id,
                connection->active->context_epoch,
                connection->active->subscription_id});
        }
        connection->active.reset();
        connection->latest_subscription_id.clear();
        connection->latest_subscription_epoch = 0;
        application.release_connection_resources(connection->id);
        notify();
    }

    void admit_error(
        const std::shared_ptr<Connection>& connection,
        std::optional<std::uint64_t> id,
        std::uint64_t epoch,
        ErrorCode code,
        std::string_view message = {}) {
        if (!id) return;
        if (!connection->highest_request_id
            || *id > *connection->highest_request_id) {
            connection->highest_request_id = *id;
        }
        const std::size_t error_limit = std::max<std::size_t>(1, options.control_limit);
        if (connection->unadmitted_count >= error_limit) {
            invalidate(connection, false);
            return;
        }
        ++connection->unadmitted_count;
        queue_reply(
            connection,
            error_reply(connection->id, *id, epoch, code, message),
            ReplyCapacity::unadmitted);
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
            const auto* subscribed = outstanding.method == Method::session_subscribe
                ? std::get_if<SubscribeResult>(&*peeked)
                : nullptr;
            LiveSessionHandle subscribed_session;
            if (subscribed) {
                subscribed_session = subscribed->session;
            }
            LiveSessionHandle stale_subscription;
            {
                std::lock_guard lock(mutex);
                connection = find_connection(connection_id);
                if (!connection) {
                    stale_subscription = std::move(subscribed_session);
                } else {
                    if (outstanding.method == Method::session_subscribe) {
                        const bool current = subscribed
                            && subscribed->subscription_id
                                == connection->latest_subscription_id
                            && subscribed->context_epoch
                                == connection->latest_subscription_epoch;
                        if (subscribed_session && current) {
                            connection->active = ActiveSubscription{
                                subscribed->subscription_id,
                                outstanding.forum_id,
                                outstanding.session_id,
                                subscribed->context_epoch,
                                std::move(subscribed_session)};
                        } else {
                            stale_subscription = std::move(subscribed_session);
                        }
                    }
                    if (outstanding.method == Method::session_unsubscribe
                        && connection->active
                        && connection->active->subscription_id
                            == outstanding.subscription_id
                        && connection->active->context_epoch
                            == outstanding.context_epoch) {
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
                }
            }
            if (stale_subscription && subscribed) {
                (void)stale_subscription->enqueue(UnsubscribeCommand{
                    connection_id,
                    subscribed->context_epoch,
                    subscribed->subscription_id});
            }
        } catch (const ErrorCode code) {
            std::lock_guard lock(mutex);
            connection = find_connection(connection_id);
            if (!connection) return;
            fail_request(connection, id, outstanding.context_epoch, code);
        }
        pump();
    }

    void schedule_session_completion(
        std::string connection_id,
        std::uint64_t id) {
        auto weak = std::weak_ptr<Impl>(shared_from_this());
        {
            // This callback can run inline on the session runtime while a
            // bridge task holds `mutex` waiting for a runtime lookup.
            std::lock_guard lock(ready_tasks_mutex);
            ready_tasks.push_back(
                [weak, connection_id = std::move(connection_id), id] {
                    if (auto impl = weak.lock()) {
                        impl->complete_session(connection_id, id);
                    }
                });
        }
        notify();
    }

    void complete_background(
        std::string connection_id,
        std::uint64_t id) {
        std::shared_ptr<Connection> connection;
        Outstanding outstanding;
        {
            std::lock_guard lock(mutex);
            connection = find_connection(connection_id);
            if (!connection) return;
            const auto found = connection->outstanding.find(id);
            if (found == connection->outstanding.end() || !found->second.operation) {
                return;
            }
            outstanding = found->second;
        }
        const auto peeked = outstanding.operation->peek();
        if (!peeked) return;
        if (const auto context =
                check_epoch(outstanding.method, outstanding.context_epoch)) {
            std::lock_guard lock(mutex);
            connection = find_connection(connection_id);
            if (connection) {
                fail_request(
                    connection, id, outstanding.context_epoch, *context);
            }
            return;
        }
        if (const auto* failure =
                std::get_if<app::OperationReply::Failure>(&*peeked)) {
            std::lock_guard lock(mutex);
            connection = find_connection(connection_id);
            if (!connection) return;
            fail_request(
                connection,
                id,
                outstanding.context_epoch,
                failure->code,
                failure->message);
            return;
        }
        std::lock_guard lock(mutex);
        connection = find_connection(connection_id);
        if (!connection) return;
        finish_request(
            connection,
            id,
            reply_ok(
                connection->id,
                id,
                outstanding.context_epoch,
                std::get<nlohmann::json>(*peeked)));
    }

    void start_background(
        std::string connection_id,
        std::uint64_t id,
        std::shared_ptr<app::OperationReply> reply) {
        {
            std::lock_guard lock(mutex);
            auto connection = find_connection(connection_id);
            if (connection && connection->outstanding.contains(id)) {
                connection->outstanding.at(id).operation = reply;
            } else {
                reply->abandon();
                application.release_request_resources(connection_id, id);
                return;
            }
        }
        auto weak = std::weak_ptr<Impl>(shared_from_this());
        reply->set_ready_callback([weak, connection_id, id] {
            if (auto impl = weak.lock()) impl->complete_background(connection_id, id);
        });
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

    std::shared_ptr<CommandReply> start_session_command(
        const std::shared_ptr<Connection>& connection,
        std::uint64_t id,
        Outstanding outstanding,
        WebCommand command) {
        auto outcome = application.submit_async(
            outstanding.forum_id,
            outstanding.session_id,
            std::move(command),
            outstanding.context_epoch);
        if (const auto* error = std::get_if<ErrorCode>(&outcome)) {
            fail_request(connection, id, outstanding.context_epoch, *error);
            return {};
        }
        auto reply = std::get<std::shared_ptr<CommandReply>>(
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
            {
                std::lock_guard lock(mutex);
                auto connection = find_connection(connection_id);
                if (!connection) return;
                const auto found = connection->outstanding.find(id);
                if (found == connection->outstanding.end()) return;
                if (found->second.cancelled) {
                    fail_request(
                        connection, id, epoch, ErrorCode::operation_cancelled);
                    return;
                }
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
                    {"state", app::application_state_name(boot.state)},
                    {"context_epoch", boot.context_epoch},
                    {"application_version", kApplicationVersion},
                    {"capabilities",
                     {
                         {"can_modify", boot.capabilities.can_modify},
                         {"can_transfer_r2", boot.capabilities.can_transfer_r2},
                     }},
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
                    forum_id, params["label"].get<std::string>(), epoch);
                break;
            }
            case Method::session_open: {
                // Waits up to open_deadline on this worker. Control tasks use a
                // separate deque so they are not queued behind unstarted opens.
                const auto identity = parse_session_identity(params);
                auto opened = application.open_session(
                    identity.forum_id, identity.session_id, epoch);
                if (const auto* error = std::get_if<ErrorCode>(&opened)) {
                    fail(*error);
                    return;
                }
                result = std::get<OpenSessionSuccess>(opened);
                break;
            }
            case Method::session_close: {
                const auto identity = parse_session_identity(params);
                // Missing sessions are a successful no-op.
                application.close_session(
                    identity.forum_id, identity.session_id, epoch);
                result = nlohmann::json::object();
                break;
            }
            case Method::session_list: {
                require_only_keys(params, {"forum_id"});
                result = application.list_sessions(
                    require_identifier(params, "forum_id"), epoch);
                break;
            }
            case Method::session_rename: {
                require_only_keys(params, {"forum_id", "session_id", "label"});
                if (!params["label"].is_string()) {
                    throw std::invalid_argument("The request was not valid.");
                }
                result = application.rename_session(
                    require_identifier(params, "forum_id"),
                    require_identifier(params, "session_id"),
                    params["label"].get<std::string>(),
                    epoch);
                break;
            }
            case Method::session_delete: {
                const auto identity = parse_session_identity(params);
                if (const auto error = application.delete_session(
                        identity.forum_id, identity.session_id, epoch)) {
                    fail(*error);
                    return;
                }
                result = nlohmann::json::object();
                break;
            }
            case Method::session_export: {
                const auto identity = parse_session_identity(params);
                result = application.export_session(
                    identity.forum_id, identity.session_id, epoch);
                break;
            }
            case Method::vault_list: {
                require_only_keys(params, {});
                const auto snapshot = application.vault_snapshot();
                result = nlohmann::json::array();
                for (const auto& vault : snapshot.vaults) {
                    result.push_back(app::vault::vault_detail_json(
                        vault, snapshot.active.name, snapshot.vaults.size()));
                }
                break;
            }
            case Method::vault_create: {
                VaultCreate create;
                create.display_name = require_string(params, "display_name");
                create.copy_from = nullable_string(params, "copy_from");
                create.password = optional_string(params, "password");
                const auto created =
                    application.create_vault(std::move(create), epoch);
                const auto snapshot = application.vault_snapshot();
                result = app::vault::vault_detail_json(
                    created, snapshot.active.name, snapshot.vaults.size());
                break;
            }
            case Method::vault_update: {
                const std::string name = require_string(params, "vault_name");
                VaultUpdate update;
                update.display_name = require_string(params, "display_name");
                update.password = optional_string(params, "password");
                const auto updated =
                    application.update_vault(name, std::move(update), epoch);
                epoch = application.context_epoch();
                const auto snapshot = application.vault_snapshot();
                result = app::vault::vault_detail_json(
                    updated, snapshot.active.name, snapshot.vaults.size());
                break;
            }
            case Method::vault_delete: {
                application.delete_vault(
                    require_string(params, "vault_name"), epoch);
                result = nlohmann::json::object();
                break;
            }
            case Method::vault_switch: {
                const auto switched = application.switch_vault(
                    require_string(params, "vault_name"),
                    optional_string(params, "password"),
                    epoch);
                epoch = switched.context_epoch;
                result = {
                    {"state", app::application_state_name(switched.state)},
                    {"context_epoch", switched.context_epoch},
                };
                break;
            }
            case Method::vault_merge: {
                const auto merged = application.merge_vault(
                    require_string(params, "source_vault"),
                    optional_string(params, "password"),
                    epoch);
                epoch = merged.context_epoch;
                result = {
                    {"state", app::application_state_name(merged.state)},
                    {"context_epoch", merged.context_epoch},
                };
                break;
            }
            case Method::vault_r2_list: {
                require_only_keys(params, {});
                result = application.list_r2_vaults(epoch);
                break;
            }
            case Method::vault_r2_download: {
                const auto created = application.download_r2_vault(
                    require_string(params, "name"), epoch);
                const auto snapshot = application.vault_snapshot();
                result = app::vault::vault_detail_json(
                    created, snapshot.active.name, snapshot.vaults.size());
                break;
            }
            case Method::provider_test: {
                const std::string provider_id =
                    require_identifier(params, "provider_id");
                start_background(
                    connection_id,
                    id,
                    application.test_provider(
                        provider_id, without_key(params, "provider_id"), epoch));
                return;
            }
            case Method::voice_input_connect: {
                if (!params.is_object() || !params.contains("sdp")
                    || !params["sdp"].is_string()) {
                    throw std::invalid_argument("The request was not valid.");
                }
                for (const auto& [name, value] : params.items()) {
                    if (name != "sdp" && name != "languages") {
                        throw std::invalid_argument("The request was not valid.");
                    }
                }
                std::vector<std::string> languages;
                if (params.contains("languages")) {
                    if (!params["languages"].is_array()) {
                        throw std::invalid_argument("The request was not valid.");
                    }
                    for (const auto& language : params["languages"]) {
                        if (!language.is_string()) {
                            throw std::invalid_argument("The request was not valid.");
                        }
                        languages.push_back(language.get<std::string>());
                    }
                }
                start_background(
                    connection_id,
                    id,
                    application.connect_voice_input(
                        connection_id,
                        id,
                        params["sdp"].get<std::string>(),
                        std::move(languages),
                        epoch));
                return;
            }
            case Method::voice_input_cancel: {
                require_only_keys(params, {"request_id"});
                const auto target = require_safe_id(params, "request_id");
                {
                    std::lock_guard lock(mutex);
                    auto connection = find_connection(connection_id);
                    if (connection) {
                        const auto found = connection->outstanding.find(target);
                        if (found != connection->outstanding.end()) {
                            found->second.cancelled = true;
                        }
                    }
                }
                application.cancel_voice_input(connection_id, target, epoch);
                result = nlohmann::json::object();
                break;
            }
            case Method::speech_start: {
                if (!params.is_object() || !params.contains("text")
                    || !params["text"].is_string()) {
                    throw std::invalid_argument("The request was not valid.");
                }
                for (const auto& [name, value] : params.items()) {
                    if (name != "text" && name != "reference_id"
                        && name != "settings") {
                        throw std::invalid_argument("The request was not valid.");
                    }
                }
                start_background(
                    connection_id,
                    id,
                    application.start_speech(
                        connection_id,
                        id,
                        params["text"].get<std::string>(),
                        parse_synthesis_fields(params),
                        epoch));
                return;
            }
            case Method::speech_cancel: {
                require_only_keys(params, {"request_id"});
                const auto target = require_safe_id(params, "request_id");
                {
                    std::lock_guard lock(mutex);
                    auto connection = find_connection(connection_id);
                    if (connection) {
                        const auto found = connection->outstanding.find(target);
                        if (found != connection->outstanding.end()) {
                            found->second.cancelled = true;
                        }
                    }
                }
                application.cancel_speech(connection_id, target, epoch);
                result = nlohmann::json::object();
                break;
            }
            case Method::speech_release:
            case Method::audio_release:
                require_only_keys(params, {"resource_id"});
                application.release_resource(
                    connection_id, require_string(params, "resource_id"), epoch);
                result = nlohmann::json::object();
                break;
            case Method::audio_start: {
                if (!params.is_object()
                    || !params.contains("forum_id")
                    || !params.contains("session_id")
                    || !params.contains("entry_id")
                    || !params.contains("vault_name")) {
                    throw std::invalid_argument("The request was not valid.");
                }
                for (const auto& [name, value] : params.items()) {
                    if (name != "forum_id" && name != "session_id"
                        && name != "entry_id" && name != "vault_name"
                        && name != "reference_id" && name != "settings") {
                        throw std::invalid_argument("The request was not valid.");
                    }
                }
                AudioDownloadRequest request{
                    require_string(params, "vault_name"),
                    parse_synthesis_fields(params)};
                result = app::audio_acceptance_json(application.start_audio(
                    require_identifier(params, "forum_id"),
                    require_identifier(params, "session_id"),
                    require_safe_id(params, "entry_id"),
                    std::move(request),
                    epoch));
                break;
            }
            case Method::audio_start_batch: {
                require_only_keys(
                    params, {"forum_id", "session_id", "vault_name", "entries"});
                if (!params["entries"].is_array() || params["entries"].empty()) {
                    throw std::invalid_argument("The request was not valid.");
                }
                AudioDownloadBatchRequest request;
                request.vault_name = require_string(params, "vault_name");
                std::set<EntryId> ids;
                for (const auto& entry : params["entries"]) {
                    if (!entry.is_object() || !entry.contains("entry_id")
                        || !entry.contains("reference_id")) {
                        throw std::invalid_argument("The request was not valid.");
                    }
                    for (const auto& [name, value] : entry.items()) {
                        if (name != "entry_id" && name != "reference_id"
                            && name != "settings") {
                            throw std::invalid_argument("The request was not valid.");
                        }
                    }
                    const EntryId entry_id = require_safe_id(entry, "entry_id");
                    if (!ids.insert(entry_id).second) {
                        throw std::invalid_argument("The request was not valid.");
                    }
                    request.entries.push_back(
                        {entry_id, parse_synthesis_fields(entry)});
                }
                nlohmann::json entries = nlohmann::json::array();
                for (const auto& acceptance : application.start_audio_batch(
                         require_identifier(params, "forum_id"),
                         require_identifier(params, "session_id"),
                         std::move(request),
                         epoch)) {
                    entries.push_back(app::audio_acceptance_json(acceptance));
                }
                result = {{"entries", std::move(entries)}};
                break;
            }
            case Method::audio_status: {
                require_only_keys(
                    params, {"forum_id", "session_id", "vault_name"});
                result = app::audio_status_json(application.audio_status(
                    require_identifier(params, "forum_id"),
                    require_identifier(params, "session_id"),
                    require_string(params, "vault_name"),
                    epoch));
                break;
            }
            case Method::audio_source: {
                require_only_keys(
                    params,
                    {"forum_id", "session_id", "entry_id", "vault_name"});
                const auto resource = application.audio_source(
                    connection_id,
                    require_identifier(params, "forum_id"),
                    require_identifier(params, "session_id"),
                    require_safe_id(params, "entry_id"),
                    require_string(params, "vault_name"),
                    epoch);
                result = app::media_resource_json(
                    resource.resource_id,
                    resource.mime_type,
                    resource.byte_length);
                break;
            }
            case Method::audio_clear_cache: {
                const auto identity = parse_session_identity(params);
                application.clear_audio_cache(
                    identity.forum_id, identity.session_id, epoch);
                result = nlohmann::json::object();
                break;
            }
            case Method::openai_auth_start:
                require_only_keys(params, {});
                start_background(
                    connection_id, id, application.start_openai_auth(epoch));
                return;
            case Method::openai_auth_poll:
                require_only_keys(params, {});
                start_background(
                    connection_id, id, application.poll_openai_auth(epoch));
                return;
            default:
                if (auto workspace_result = dispatch_workspace_operation(
                        application, method, params, epoch)) {
                    result = std::move(*workspace_result);
                } else if (auto settings_result = dispatch_settings_operation(
                               application, method, params, epoch)) {
                    result = std::move(*settings_result);
                } else {
                    fail(ErrorCode::invalid_argument, "That method is not available.");
                    return;
                }
                break;
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
        } catch (const app::ApplicationError& error) {
            fail(error.code, error.what());
        } catch (const UnknownVaultError& error) {
            fail(ErrorCode::invalid_argument, error.what());
        } catch (const VaultPasswordError& error) {
            fail(
                method == Method::vault_merge
                    ? ErrorCode::source_vault_password_required
                    : ErrorCode::vault_password_required,
                error.what());
        } catch (const SessionNotFoundError&) {
            fail(ErrorCode::not_found);
        } catch (const ForumNotFoundError&) {
            fail(ErrorCode::not_found);
        } catch (const std::out_of_range&) {
            fail(ErrorCode::not_found);
        } catch (const std::invalid_argument& error) {
            fail(ErrorCode::invalid_argument, error.what());
        } catch (const WorkspaceRestartRequiredError& error) {
            fail(ErrorCode::application_unavailable, error.what());
        } catch (const std::runtime_error& error) {
            // The reply stays generic; the log keeps the reason.
            log_error(
                "Request " + std::string(method_name(method)) + " failed: "
                + error.what());
            fail(application.state() == app::ApplicationState::running
                ? ErrorCode::internal_error
                : ErrorCode::application_unavailable);
        } catch (...) {
            log_error("Request " + std::string(method_name(method)) + " failed");
            fail(ErrorCode::internal_error);
        }
    }

    void run_session(
        std::string connection_id,
        std::uint64_t id,
        Outstanding outstanding,
        nlohmann::json params) {
        if (const auto context =
                check_epoch(outstanding.method, outstanding.context_epoch)) {
            std::lock_guard lock(mutex);
            auto connection = find_connection(connection_id);
            if (connection) {
                fail_request(
                    connection, id, outstanding.context_epoch, *context);
            }
            return;
        }
        std::shared_ptr<CommandReply> reply;
        {
            std::lock_guard lock(mutex);
            auto connection = find_connection(connection_id);
            if (!connection) return;
            const auto found = connection->outstanding.find(id);
            if (found == connection->outstanding.end()) return;
            try {
                WebCommand command;
                switch (outstanding.method) {
                case Method::session_submit: {
                    require_only_keys(params, {"forum_id", "session_id", "input"});
                    outstanding.forum_id = require_identifier(params, "forum_id");
                    outstanding.session_id = require_identifier(params, "session_id");
                    auto input = parse_input_command(params["input"]);
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
                    command = StopCommand{};
                    break;
                }
                case Method::session_snapshot: {
                    const auto identity = parse_session_identity(params);
                    outstanding.forum_id = identity.forum_id;
                    outstanding.session_id = identity.session_id;
                    command = SnapshotCommand{};
                    break;
                }
                case Method::session_cover: {
                    require_only_keys(
                        params, {"forum_id", "session_id", "through_entry_id"});
                    outstanding.forum_id = require_identifier(params, "forum_id");
                    outstanding.session_id = require_identifier(params, "session_id");
                    command = parse_cover_command({
                        {"through_entry_id", params["through_entry_id"]},
                    });
                    break;
                }
                case Method::session_uncover: {
                    const auto identity = parse_session_identity(params);
                    outstanding.forum_id = identity.forum_id;
                    outstanding.session_id = identity.session_id;
                    command = UncoverCommand{};
                    break;
                }
                case Method::session_delete_turn: {
                    require_only_keys(
                        params, {"forum_id", "session_id", "response_entry_id"});
                    outstanding.forum_id = require_identifier(params, "forum_id");
                    outstanding.session_id = require_identifier(params, "session_id");
                    command = parse_delete_turn_command({
                        {"response_entry_id", params["response_entry_id"]},
                    });
                    break;
                }
                case Method::session_set_default_character: {
                    require_only_keys(
                        params, {"forum_id", "session_id", "character_id"});
                    outstanding.forum_id = require_identifier(params, "forum_id");
                    outstanding.session_id = require_identifier(params, "session_id");
                    auto parsed = parse_default_character_command({
                        {"character_id", params["character_id"]},
                    });
                    if (parsed.character_id.empty()) {
                        throw std::invalid_argument("The request was not valid.");
                    }
                    command = std::move(parsed);
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
                    command = SubscribeCommand{
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
                    command = UnsubscribeCommand{
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
            if (auto impl = weak.lock()) {
                impl->schedule_session_completion(connection_id, id);
            }
        });
    }

    bool is_session_method(Method method) const {
        return method == Method::session_submit
            || method == Method::session_stop
            || method == Method::session_snapshot
            || method == Method::session_cover
            || method == Method::session_uncover
            || method == Method::session_delete_turn
            || method == Method::session_set_default_character
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
        if (connection->highest_request_id
            && request.id <= *connection->highest_request_id) {
            admit_error(
                connection,
                request.id,
                request.context_epoch,
                ErrorCode::invalid_argument,
                "Request ids must increase.");
            return;
        }
        connection->highest_request_id = request.id;
        const bool control = is_control_method(request.method);
        const std::size_t& count =
            control ? connection->control_count : connection->ordinary_count;
        const std::size_t limit =
            control ? options.control_limit : options.ordinary_limit;
        if (count >= limit) {
            admit_error(
                connection,
                request.id,
                request.context_epoch,
                ErrorCode::command_queue_full,
                control ? std::string_view{}
                        : std::string_view{"Too many in-flight requests."});
            if (!connection->invalid) invalidate(connection, false);
            return;
        }
        Outstanding outstanding;
        outstanding.method = request.method;
        outstanding.control = control;
        outstanding.deadline = now()
            + options.command_deadline.value_or(
                application.settings().command_deadline);
        outstanding.context_epoch = request.context_epoch;
        if (request.method == Method::session_subscribe
            || request.method == Method::session_unsubscribe) {
            try {
                require_only_keys(
                    request.params,
                    {"forum_id", "session_id", "subscription_id"});
                outstanding.forum_id =
                    require_identifier(request.params, "forum_id");
                outstanding.session_id =
                    require_identifier(request.params, "session_id");
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
            if (request.method == Method::session_subscribe) {
                connection->latest_subscription_id = outstanding.subscription_id;
                connection->latest_subscription_epoch = request.context_epoch;
            } else if (connection->latest_subscription_id
                           == outstanding.subscription_id
                       && connection->latest_subscription_epoch
                           == request.context_epoch) {
                connection->latest_subscription_id.clear();
                connection->latest_subscription_epoch = 0;
            }
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

BridgeRouter::BridgeRouter(app::Application& application)
    : BridgeRouter(application, Options{}) {}

BridgeRouter::BridgeRouter(app::Application& application, Options options)
    : impl_(std::make_shared<Impl>(application, std::move(options))) {
    impl_->listen_for_context_changes();
}

BridgeRouter::~BridgeRouter() {
    shutdown();
    impl_->application.set_context_changed({});
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
    auto parsed = parse_request(json, impl_->options.request_bytes_limit);
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping) return;
    auto connection = impl_->find_connection(trusted_connection_id);
    if (!connection) return;
    if (const auto* failure = std::get_if<ParseFailure>(&parsed)) {
        impl_->admit_error(
            connection,
            failure->id,
            0,
            failure->code,
            failure->message);
        return;
    }
    auto request = std::get<ParsedRequest>(std::move(parsed));
    impl_->handle_parsed(connection, std::move(request));
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
        const auto& delivered = *connection->in_flight;
        if (delivered.has_session_event) {
            session = delivered.session;
        }
        connection->ordinary_count -= std::min(
            connection->ordinary_count, delivered.ordinary_replies);
        connection->control_count -= std::min(
            connection->control_count, delivered.control_replies);
        connection->unadmitted_count -= std::min(
            connection->unadmitted_count, delivered.unadmitted_replies);
        connection->in_flight.reset();
        impl_->notify();
    }
    if (session) session->acknowledge_output();
    impl_->pump();
}

void BridgeRouter::run_tasks() {
    run_control_tasks();
    run_ordinary_tasks();
}

void BridgeRouter::run_control_tasks() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard lock(impl_->ready_tasks_mutex);
            if (!impl_->ready_tasks.empty()) {
                task = std::move(impl_->ready_tasks.front());
                impl_->ready_tasks.pop_front();
            }
        }
        if (!task) {
            std::lock_guard lock(impl_->mutex);
            if (!impl_->control_tasks.empty()) {
                task = std::move(impl_->control_tasks.front());
                impl_->control_tasks.pop_front();
            } else {
                return;
            }
        }
        task();
    }
}

void BridgeRouter::run_ordinary_tasks() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard lock(impl_->mutex);
            if (impl_->ordinary_tasks.empty()) return;
            task = std::move(impl_->ordinary_tasks.front());
            impl_->ordinary_tasks.pop_front();
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
            if (found->second.operation) found->second.operation->abandon();
            const auto epoch = found->second.context_epoch;
            if (found->second.method == Method::speech_start
                || found->second.method == Method::voice_input_connect) {
                try {
                    impl_->application.cancel_speech(connection->id, request_id, epoch);
                } catch (const app::ApplicationError&) {
                    // Context invalidation already revokes these resources.
                }
            }
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
    if (connection->next_delivery_id > kMaxSafeInteger) {
        impl_->invalidate(connection);
        return std::nullopt;
    }
    std::vector<nlohmann::json> messages;
    Impl::InFlight in_flight;
    // Invalidate frontend requests before any results from the replaced vault.
    if (connection->pending_context) {
        messages.push_back(std::move(*connection->pending_context));
        connection->pending_context.reset();
    }
    while (!connection->pending_replies.empty()) {
        auto reply = std::move(connection->pending_replies.front());
        connection->pending_replies.pop_front();
        switch (reply.capacity) {
        case Impl::ReplyCapacity::ordinary:
            ++in_flight.ordinary_replies;
            break;
        case Impl::ReplyCapacity::control:
            ++in_flight.control_replies;
            break;
        case Impl::ReplyCapacity::unadmitted:
            ++in_flight.unadmitted_replies;
            break;
        }
        if (reply.context_bound) {
            const auto epoch = reply.message.at("context_epoch").get<std::uint64_t>();
            if (const auto error = impl_->application.check_context(epoch)) {
                reply.message = impl_->error_reply(
                    connection->id, reply.message.at("id").get<std::uint64_t>(),
                    epoch, *error);
            }
        }
        messages.push_back(std::move(reply.message));
    }
    if (connection->pending_invalidation) {
        messages.push_back(std::move(*connection->pending_invalidation));
        connection->pending_invalidation.reset();
    }
    if (connection->pending_session) {
        if (!impl_->application.check_context(
                connection->pending_session->at("context_epoch").get<std::uint64_t>())) {
            messages.push_back(std::move(*connection->pending_session));
            in_flight.has_session_event = true;
            if (connection->active) in_flight.session = connection->active->session;
        } else if (connection->active && connection->active->session) {
            connection->active->session->acknowledge_output();
        }
        connection->pending_session.reset();
    }
    if (messages.empty()) return std::nullopt;
    in_flight.delivery_id = connection->next_delivery_id++;
    connection->in_flight = std::move(in_flight);
    return delivery_batch(
        connection->id, connection->in_flight->delivery_id, std::move(messages));
}

bool BridgeRouter::wait_for_work(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->work.wait_for(lock, timeout, [&] {
        if (impl_->stopping) return true;
        if (impl_->has_ready_tasks()) return true;
        if (!impl_->control_tasks.empty()) {
            return true;
        }
        for (const auto& [id, connection] : impl_->connections) {
            if (!connection->in_flight
                && (!connection->pending_replies.empty()
                    || connection->pending_context
                    || connection->pending_invalidation
                    || connection->pending_session)) {
                return true;
            }
        }
        return false;
    });
}

bool BridgeRouter::wait_for_ordinary_work(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->work.wait_for(lock, timeout, [&] {
        return impl_->stopping || !impl_->ordinary_tasks.empty();
    });
}

void BridgeRouter::shutdown() {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
    impl_->ordinary_tasks.clear();
    impl_->control_tasks.clear();
    {
        std::lock_guard ready_lock(impl_->ready_tasks_mutex);
        impl_->ready_tasks.clear();
    }
    for (auto& [id, connection] : impl_->connections) {
        impl_->invalidate(connection);
    }
    impl_->notify();
}

} // namespace cha::bridge
