#pragma once

#include <cstdint>

namespace gemini {

// Push-to-talk transitions; callers serialize access across audio and UI tasks.
class TurnState {
public:
    enum class Phase { Ready, Recording, Thinking, Speaking, Resetting, Error };

    Phase phase() const { return _phase; }
    uint32_t turnId() const { return _turn_id; }

    bool press()
    {
        if (_phase != Phase::Ready && _phase != Phase::Speaking && _phase != Phase::Error) {
            return false;
        }
        ++_turn_id;
        _phase = Phase::Recording;
        return true;
    }

    bool release()
    {
        if (_phase != Phase::Recording) {
            return false;
        }
        _phase = Phase::Thinking;
        return true;
    }

    bool startPlayback(uint32_t turn_id)
    {
        if (turn_id != _turn_id || _phase != Phase::Thinking) {
            return false;
        }
        _phase = Phase::Speaking;
        return true;
    }

    bool finishResponse(uint32_t turn_id)
    {
        if (turn_id != _turn_id || (_phase != Phase::Thinking && _phase != Phase::Speaking)) {
            return false;
        }
        _phase = Phase::Ready;
        return true;
    }

    void resetConversation()
    {
        ++_turn_id;
        _phase = Phase::Resetting;
    }

    void finishReset()
    {
        if (_phase == Phase::Resetting) {
            _phase = Phase::Ready;
        }
    }

    void fail()
    {
        ++_turn_id;
        _phase = Phase::Error;
    }

private:
    Phase _phase = Phase::Ready;
    uint32_t _turn_id = 0;
};

}  // namespace gemini
