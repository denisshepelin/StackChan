#include <hal/gemini_turn_state.h>

#include <cstdlib>
#include <iostream>

namespace {

using gemini::TurnState;
using Phase = TurnState::Phase;

void expect(bool condition, const char* label)
{
    if (!condition) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}

void testPushToTalkTurns()
{
    TurnState turn;
    expect(turn.phase() == Phase::Ready, "Starts ready");
    expect(!turn.release(), "Idle release does not submit a turn");
    expect(turn.press(), "Press starts recording");
    const auto first = turn.turnId();
    expect(turn.phase() == Phase::Recording, "Recording while held");
    expect(!turn.press(), "Repeated press cannot restart capture");
    expect(!turn.startPlayback(first), "Playback cannot start while recording");
    expect(turn.release(), "Release submits recording");
    expect(turn.phase() == Phase::Thinking, "Thinking after release");
    expect(!turn.release(), "Repeated release cannot submit again");
    expect(!turn.press(), "Thinking ignores presses");
    expect(turn.turnId() == first, "Ignored press preserves response ownership");
    expect(turn.startPlayback(first), "Buffered response starts playback");
    expect(turn.phase() == Phase::Speaking, "Speaking after prebuffer");
    expect(turn.finishResponse(first), "Drained response returns to idle");
    expect(turn.phase() == Phase::Ready, "Idle after playback");
    expect(turn.press(), "Next press starts another turn");
    expect(turn.turnId() != first, "Each turn has distinct audio ownership");
}

void testPressInterruptsPlayback()
{
    TurnState turn;
    turn.press();
    const auto interrupted = turn.turnId();
    turn.release();
    turn.startPlayback(interrupted);
    expect(turn.press(), "Press interrupts speaking and records immediately");
    expect(turn.phase() == Phase::Recording, "Interrupted playback becomes recording");
    expect(!turn.finishResponse(interrupted), "Stale completion cannot end recording");
    expect(!turn.startPlayback(interrupted), "Stale audio cannot restart playback");
    turn.release();
    expect(!turn.finishResponse(interrupted), "Stale completion cannot end new thinking turn");
    expect(turn.startPlayback(turn.turnId()), "New response can play after interruption");
}

void testShakeResetsEveryPhase()
{
    for (const auto phase : {Phase::Ready, Phase::Recording, Phase::Thinking, Phase::Speaking, Phase::Error}) {
        TurnState turn;
        if (phase == Phase::Error) {
            turn.fail();
        } else if (phase != Phase::Ready) {
            turn.press();
            if (phase != Phase::Recording) {
                turn.release();
            }
            if (phase == Phase::Speaking) {
                turn.startPlayback(turn.turnId());
            }
        }
        const auto old_turn = turn.turnId();
        turn.resetConversation();
        expect(turn.phase() == Phase::Resetting, "Shake resets every phase");
        expect(turn.turnId() != old_turn, "Shake invalidates outstanding audio");
        expect(!turn.press(), "No capture during session teardown");
        expect(!turn.release(), "Release after shake cannot submit old audio");
        expect(!turn.finishResponse(old_turn), "Old response cannot finish reset");
        turn.resetConversation();
        turn.finishReset();
        expect(turn.phase() == Phase::Ready, "Repeated shakes can finish in ready state");
        expect(turn.press(), "New conversation accepts a new press");
        expect(!turn.startPlayback(old_turn), "Old conversation cannot play in new conversation");
    }
}

void testQuickTapAndEmptyResponse()
{
    TurnState turn;
    expect(turn.press() && turn.release(), "Quick tap still has paired turn boundaries");
    expect(turn.finishResponse(turn.turnId()), "No-audio response returns to ready");
    expect(turn.phase() == Phase::Ready, "No-audio response cannot stick in thinking");
}

void testConnectionFailure()
{
    TurnState turn;
    turn.press();
    const auto failed = turn.turnId();
    turn.fail();
    expect(turn.phase() == Phase::Error, "Failure cancels the current turn");
    expect(!turn.release(), "Release after failure does not submit");
    expect(!turn.finishResponse(failed), "Failed response cannot return to ready");
    expect(turn.press(), "Fresh press can retry after failure");
    turn.finishReset();
    expect(turn.phase() == Phase::Recording, "Unrelated reset completion cannot overwrite recording");
}

}  // namespace

int main()
{
    testPushToTalkTurns();
    testPressInterruptsPlayback();
    testShakeResetsEveryPhase();
    testQuickTapAndEmptyResponse();
    testConnectionFailure();
    std::cout << "Gemini push-to-talk state tests passed\n";
}
