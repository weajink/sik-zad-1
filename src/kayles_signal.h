#ifndef KAYLES_SIGNAL_H
#define KAYLES_SIGNAL_H

#include <signal.h>

namespace kayles::sig {
    inline volatile sig_atomic_t shutdown_requested = 0;

    extern "C" inline void handler(int) {
        shutdown_requested = 1;
    }

    inline void install() {
        struct sigaction sa {};
        sa.sa_handler = &handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }
}  // namespace kayles::sig

#endif
