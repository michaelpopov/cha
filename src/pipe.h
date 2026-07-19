#pragma once

#include <mutex>
#include <queue>
#include <string>
#include <string_view>

namespace cha {

enum class PipeEventKind {
    data,
    eom,
    closed,
};

struct PipeEvent {
    PipeEventKind kind;
    std::string data;

    bool operator==(const PipeEvent&) const = default;
};

class Pipe {
public:
    Pipe();
    ~Pipe();

    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;

    void put(std::string_view str);
    [[nodiscard]] PipeEvent get();
    [[nodiscard]] bool try_get(PipeEvent& event);
    void eom();
    void close();
    [[nodiscard]] int notification_fd() const;

private:
    void notify() const;
    void wait_for_notification() const;
    [[nodiscard]] bool try_consume_notification() const;

    bool closed_{};
    mutable std::mutex mutex_;
    std::queue<PipeEvent> messages_;
    int notification_fd_{-1};
};

} // namespace cha
