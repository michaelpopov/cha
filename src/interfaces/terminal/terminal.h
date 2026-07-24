#pragma once

namespace cha {

// Owns the process-wide ncurses lifecycle and switches modes for each UI phase.
class Terminal {
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    void configure_selector();
    void configure_chat();
    void resize();
    // Leaves ncurses mode; repeated calls are harmless.
    void restore();

private:
    bool active_{true};
};

} // namespace cha
