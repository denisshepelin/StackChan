#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace stackchan::avatar {

// Codex pet moves exported for Gemini; array order is stable.
enum class CodexPetAnimation : uint8_t { Idle, Wave, Failed, Review, Waiting, Run };

// Per-frame timing in milliseconds, including Codex's longer final-frame holds.
struct CodexPetTiming {
    std::array<uint16_t, 8> durations_ms{};
    size_t frame_count = 0;

    bool valid() const
    {
        if (frame_count == 0 || frame_count > durations_ms.size()) {
            return false;
        }
        for (size_t frame = 0; frame < frame_count; ++frame) {
            if (durations_ms[frame] == 0 || durations_ms[frame] > 10000) {
                return false;
            }
        }
        return true;
    }

    // Elapsed time is unsigned now - start, so animation survives clock wraparound.
    size_t frameAtElapsed(uint32_t elapsed_ms) const
    {
        if (!valid()) {
            return 0;
        }
        uint32_t cycle_ms = 0;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            cycle_ms += durations_ms[frame];
        }
        uint32_t phase_ms = elapsed_ms % cycle_ms;
        for (size_t frame = 0; frame < frame_count; ++frame) {
            if (phase_ms < durations_ms[frame]) {
                return frame;
            }
            phase_ms -= durations_ms[frame];
        }
        return 0;
    }
};

}  // namespace stackchan::avatar
