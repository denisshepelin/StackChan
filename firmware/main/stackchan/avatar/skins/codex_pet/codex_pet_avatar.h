#pragma once

#include "codex_pet_animation.h"
#include "../../avatar/avatar.h"

#include <lvgl.h>
#include <array>
#include <string_view>

namespace stackchan::avatar {

// A whole-character sprite avatar without independently animated eyes or mouth.
// All calls, including destruction, require the display lock.
class CodexPetAvatar final : public Avatar {
public:
    ~CodexPetAvatar() override;

    // Loads converter-generated assets from the flash partition, not the network.
    void init(lv_obj_t* parent);
    void update() override;
    lv_obj_t* touchPanel() const { return _panel; }
    void setAnimation(CodexPetAnimation animation);
    void setErrorMessage(std::string_view message);
    // Visual-only shake feedback; never commands a servo movement.
    void showConversationReset();

private:
    struct AnimationFrames {
        CodexPetTiming timing;
        std::array<lv_image_dsc_t, 8> frames{};
    };

    bool loadPetAssets();
    void renderPetFrame(uint32_t now);

    std::array<AnimationFrames, 6> _animations{};
    lv_obj_t* _panel = nullptr;
    lv_obj_t* _image = nullptr;
    lv_obj_t* _message = nullptr;
    CodexPetAnimation _animation = CodexPetAnimation::Idle;
    size_t _frame_index = 8;
    uint32_t _animation_started_at = 0;
    uint32_t _reset_started_at = 0;
    bool _reset_feedback = false;
    bool _loaded = false;
};

}  // namespace stackchan::avatar
