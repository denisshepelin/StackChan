#pragma once

#include <cstdint>

// Deterministic clock for host-side servo animation tests; no hardware access.
struct MotionTestHal {
    uint32_t now_ms = 0;
    uint32_t millis() const { return now_ms; }
};

inline MotionTestHal& GetHAL()
{
    static MotionTestHal hal;
    return hal;
}
