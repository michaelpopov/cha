#pragma once

#include "config.h"

#include <atomic>

namespace cha {

class Pipe;

class User {
public:
    User(const Config& config, std::atomic_bool& cancellation);
    void run(Pipe& pipe_in, Pipe& pipe_out);

private:
    const Config& _config;
    std::atomic_bool& _cancellation;
};

} // namespace cha
