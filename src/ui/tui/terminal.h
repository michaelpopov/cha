#pragma once

namespace cha {

// Owns the process-wide terminal lifecycle, which only one component may. It sets the screen up on
// construction, configures chat input and cursor modes, redraws after a resize,
// and restores the terminal on exit or destruction. Tui borrows it rather than
// configuring the screen itself. StartupSelector remains only for Block 7 cleanup.
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
