#ifndef KAYLES_CLOCK_H
#define KAYLES_CLOCK_H

#include <chrono>

#include "kayles_types.h"

namespace kayles::clock {
    using namespace kayles::types;

    class Clock {
       public:
        virtual ~Clock() = default;
        virtual time_point_t now() const = 0;
    };

    class SystemClock : public Clock {
       public:
        time_point_t now() const override {
            return std::chrono::steady_clock::now();
        }
    };
}  // namespace kayles::clock

#endif