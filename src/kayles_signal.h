#ifndef KAYLES_SIGNAL_H
#define KAYLES_SIGNAL_H

#include <signal.h>

#include <cerrno>
#include <cstring>
#include <iostream>

namespace kayles::sig {
    inline volatile sig_atomic_t shutdown_requested = 0;

    extern "C" inline void handler(int) {
        shutdown_requested = 1;
    }

    inline bool install(std::string_view prog) {
        struct sigaction sa {};
        sa.sa_handler = &handler;
        if (sigemptyset(&sa.sa_mask) < 0) {
            std::cerr << prog << ": sigemptyset: " << strerror(errno) << "\n";
            return false;
        }
        sa.sa_flags = 0;
        if (sigaction(SIGINT, &sa, nullptr) < 0) {
            std::cerr << prog << ": sigaction(SIGINT): " << strerror(errno) << "\n";
            return false;
        }
        if (sigaction(SIGTERM, &sa, nullptr) < 0) {
            std::cerr << prog << ": sigaction(SIGTERM): " << strerror(errno) << "\n";
            return false;
        }
        return true;
    }
}  // namespace kayles::sig

#endif
