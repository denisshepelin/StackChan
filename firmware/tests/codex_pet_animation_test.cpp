#include <stackchan/avatar/skins/codex_pet/codex_pet_animation.h>

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool condition, const char* label)
{
    if (!condition) {
        std::cerr << label << '\n';
        std::exit(1);
    }
}

}  // namespace

int main()
{
    using stackchan::avatar::CodexPetTiming;
    const CodexPetTiming idle{{280, 110, 110, 140, 140, 320}, 6};
    expect(idle.valid(), "Standard Idle timing is valid");
    expect(idle.frameAtElapsed(0) == 0, "Idle starts on frame zero");
    expect(idle.frameAtElapsed(279) == 0, "First Idle frame has a long hold");
    expect(idle.frameAtElapsed(280) == 1, "Exact boundary advances one frame");
    expect(idle.frameAtElapsed(780) == 5, "Final Idle frame starts at 780 ms");
    expect(idle.frameAtElapsed(1099) == 5, "Final Idle frame lasts 320 ms");
    expect(idle.frameAtElapsed(1100) == 0, "Idle loops at 1100 ms");
    expect(idle.frameAtElapsed(1100 * 100 + 390) == 2, "Delayed updates skip to the right frame");

    const CodexPetTiming wave{{140, 140, 140, 280}, 4};
    expect(wave.frameAtElapsed(699) == 3 && wave.frameAtElapsed(700) == 0,
           "Wave skips unused atlas columns and preserves the last-frame hold");
    const CodexPetTiming waiting{{150, 150, 150, 150, 150, 260}, 6};
    expect(waiting.frameAtElapsed(749) == 4 && waiting.frameAtElapsed(750) == 5,
           "Waiting begins its final hold at 750 ms");
    expect(waiting.frameAtElapsed(1009) == 5 && waiting.frameAtElapsed(1010) == 0,
           "Waiting preserves the 260 ms final hold");
    const CodexPetTiming run{{120, 120, 120, 120, 120, 220}, 6};
    expect(run.frameAtElapsed(599) == 4 && run.frameAtElapsed(600) == 5,
           "Run thinking animation advances at 120 ms intervals");
    expect(run.frameAtElapsed(819) == 5 && run.frameAtElapsed(820) == 0,
           "Run preserves the 220 ms final hold");

    const uint32_t started_at = UINT32_MAX - 100;
    const uint32_t now = 179;
    expect(idle.frameAtElapsed(now - started_at) == 1, "Unsigned elapsed time handles tick wraparound");
    expect(!CodexPetTiming{}.valid(), "Empty animation is invalid");
    expect(!CodexPetTiming{{100}, 9}.valid(), "Too many frames is invalid");
    expect(!CodexPetTiming{{100, 0}, 2}.valid(), "Zero-duration frame is invalid");
    expect(!CodexPetTiming{{10001}, 1}.valid(), "Excessive frame duration is invalid");
    expect(CodexPetTiming{}.frameAtElapsed(123) == 0, "Invalid timing never divides by zero");
    std::cout << "Codex pet animation tests passed\n";
}
