#pragma once

#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "transcript/transcript.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace cha::test {

// Records what reached the backend and answers with one scripted delta.
class RecordingBackend final : public CompletionBackend {
public:
    RecordingBackend(
        std::string id,
        std::string name,
        std::string answer,
        CompletionResult result = {})
        : id_(std::move(id)),
          name_(std::move(name)),
          answer_(std::move(answer)),
          result_(std::move(result)) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        prepared.store(true, std::memory_order_release);
        received = input;
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool&) override {
        performed.store(true, std::memory_order_release);
        if (!answer_.empty()) {
            on_delta({CompletionDeltaKind::answer, answer_});
        }
        return result_;
    }

    AgentRuntimeInfo info() const override {
        return {{id_, name_}, "model", "test://completion", true};
    }

    std::atomic_bool prepared{};
    std::atomic_bool performed{};
    CompletionInput received;

private:
    std::string id_;
    std::string name_;
    std::string answer_;
    CompletionResult result_;
};

// Counts how many executions are inside perform() at once, so a test can prove
// full-width fan-out without sleeping.
struct BarrierState {
    std::atomic_int entered{};
    std::atomic_bool release{};
};

class BarrierBackend final : public CompletionBackend {
public:
    BarrierBackend(std::string id, std::string name, BarrierState& state)
        : id_(std::move(id)), name_(std::move(name)), state_(state) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        prepared.store(true, std::memory_order_release);
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool& cancellation) override {
        state_.entered.fetch_add(1, std::memory_order_acq_rel);
        while (!state_.release.load(std::memory_order_acquire)
               && !cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return cancellation.load(std::memory_order_acquire)
            ? CompletionResult{CompletionOutcome::cancelled, {}}
            : CompletionResult{};
    }

    AgentRuntimeInfo info() const override {
        return {{id_, name_}, "model", "test://completion", true};
    }

    std::atomic_bool prepared{};

private:
    std::string id_;
    std::string name_;
    BarrierState& state_;
};

class ThrowingBackend final : public CompletionBackend {
public:
    RequestPayload prepare(const CompletionInput&) override {
        throw std::runtime_error("preparation failed");
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool&) override {
        return {};
    }

    AgentRuntimeInfo info() const override {
        return {{"one-id", "One"}, "model", "test://completion", true};
    }
};

inline CompletionInput completion_request(
    const Transcript& transcript,
    RequestId id,
    std::string target,
    std::string name,
    std::string text = "Question") {
    return {
        .history = std::make_shared<const CompletionHistory>(
            transcript.completion_history()),
        .run = {
            .request_id = id,
            .target = {std::move(target), std::move(name)},
            .prompt_text = std::move(text),
        },
    };
}

inline bool wait_until_entered(const BarrierState& state, int expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (state.entered.load(std::memory_order_acquire) < expected
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return state.entered.load(std::memory_order_acquire) == expected;
}

} // namespace cha::test
