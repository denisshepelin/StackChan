#include "codex_pet_avatar.h"

#include <ArduinoJson.hpp>
#include <assets.h>
#include <assets/assets.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <src/misc/cache/instance/lv_image_cache.h>

#include <algorithm>
#include <string>

namespace stackchan::avatar {
namespace {

static_assert(sizeof(lv_image_header_t) == 12, "Codex pet converter requires LVGL v9 image headers");
constexpr std::string_view kPetTag = "Codex-Pet";
constexpr std::array<const char*, 6> kAnimationNames = {"idle", "wave", "failed", "review", "waiting", "run"};
constexpr std::array<const char*, 6> kAnimationFiles = {
    "gemini_pet_idle.bin", "gemini_pet_wave.bin", "gemini_pet_failed.bin", "gemini_pet_review.bin",
    "gemini_pet_waiting.bin", "gemini_pet_run.bin"};
constexpr std::array<size_t, 6> kAnimationFrameCounts = {6, 4, 8, 6, 6, 6};

}  // namespace

CodexPetAvatar::~CodexPetAvatar()
{
    if (_panel) {
        lv_obj_delete(_panel);
    }
    for (const auto& animation : _animations) {
        for (const auto& frame : animation.frames) {
            if (frame.data) {
                lv_image_cache_drop(&frame);
            }
        }
    }
}

void CodexPetAvatar::init(lv_obj_t* parent)
{
    _panel = lv_obj_create(parent);
    lv_obj_remove_style_all(_panel);
    lv_obj_set_size(_panel, 320, 240);
    lv_obj_center(_panel);
    lv_obj_set_style_bg_opa(_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(_panel, lv_color_hex(0x102A43), 0);
    lv_obj_remove_flag(_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_panel, LV_OBJ_FLAG_PRESS_LOCK);

    _image = lv_image_create(_panel);
    lv_obj_remove_flag(_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(_image);

    _message = lv_label_create(_panel);
    lv_obj_set_width(_message, 292);
    lv_obj_set_style_text_align(_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(_message, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(_message, lv_color_hex(0x102A43), 0);
    lv_obj_set_style_bg_opa(_message, LV_OPA_80, 0);
    lv_obj_align(_message, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_remove_flag(_message, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(_message, LV_OBJ_FLAG_HIDDEN);

    _loaded = loadPetAssets();
    if (!_loaded) {
        mclog::tagError(kPetTag, "Codex pet assets missing or invalid; reflash the assets partition");
        lv_label_set_text(_message, "Pet assets missing.\nReflash the assets partition.");
        lv_obj_remove_flag(_message, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    _animation_started_at = GetHAL().millis();
    renderPetFrame(_animation_started_at);
    lv_obj_center(_image);
}

bool CodexPetAvatar::loadPetAssets()
{
    void* manifest_data = nullptr;
    size_t manifest_size = 0;
    if (!Assets::GetInstance().GetAssetData("gemini_pet.json", manifest_data, manifest_size) ||
        manifest_size == 0 || manifest_size > 16384) {
        return false;
    }
    ArduinoJson::JsonDocument manifest;
    if (ArduinoJson::deserializeJson(manifest, static_cast<const char*>(manifest_data), manifest_size) ||
        manifest["schemaVersion"] != 1 || !manifest["frameWidth"].is<uint16_t>() ||
        !manifest["frameHeight"].is<uint16_t>() || !manifest["backgroundColor"].is<uint32_t>()) {
        return false;
    }
    const uint16_t width = manifest["frameWidth"];
    const uint16_t height = manifest["frameHeight"];
    const uint32_t background = manifest["backgroundColor"];
    if (width == 0 || width > 320 || height == 0 || height > 208 || background > 0xFFFFFF) {
        return false;
    }
    for (size_t index = 0; index < _animations.size(); ++index) {
        auto config = manifest["animations"][kAnimationNames[index]];
        const char* file = config["file"] | "";
        if (std::string_view(file) != kAnimationFiles[index]) {
            return false;
        }
        auto durations = config["durationsMs"].as<ArduinoJson::JsonArrayConst>();
        auto& animation = _animations[index];
        animation.timing.frame_count = durations.size();
        if (animation.timing.frame_count != kAnimationFrameCounts[index]) {
            return false;
        }
        for (size_t frame = 0; frame < durations.size(); ++frame) {
            if (!durations[frame].is<uint16_t>()) {
                return false;
            }
            animation.timing.durations_ms[frame] = durations[frame];
        }
        if (!animation.timing.valid()) {
            return false;
        }
        const auto strip = assets::get_image(file);
        const uint32_t frame_bytes = static_cast<uint32_t>(width) * height * 2;
        if (!strip.data || strip.header.magic != LV_IMAGE_HEADER_MAGIC ||
            strip.header.cf != LV_COLOR_FORMAT_RGB565 || strip.header.flags != 0 ||
            strip.header.w != width || strip.header.h != height * durations.size() ||
            strip.header.stride != width * 2 || strip.data_size != frame_bytes * durations.size()) {
            return false;
        }
        for (size_t frame = 0; frame < durations.size(); ++frame) {
            auto& descriptor = animation.frames[frame];
            descriptor = strip;
            descriptor.header.h = height;
            descriptor.data_size = frame_bytes;
            descriptor.data = strip.data + frame_bytes * frame;
        }
    }
    lv_obj_set_style_bg_color(_panel, lv_color_hex(background), 0);
    lv_obj_set_style_bg_color(_message, lv_color_hex(background), 0);
    lv_image_set_pivot(_image, width / 2, height / 2);
    lv_image_set_scale(_image, std::min(176u * LV_SCALE_NONE / width, 192u * LV_SCALE_NONE / height));
    mclog::tagInfo(kPetTag, "Codex pet loaded: {}x{} frames from flash", width, height);
    return true;
}

void CodexPetAvatar::setAnimation(CodexPetAnimation animation)
{
    if (!_loaded || animation == _animation) {
        return;
    }
    _animation = animation;
    _animation_started_at = GetHAL().millis();
    _frame_index = 8;
    renderPetFrame(_animation_started_at);
}

void CodexPetAvatar::setErrorMessage(std::string_view message)
{
    if (!_loaded) {
        return;
    }
    if (message.empty()) {
        lv_obj_add_flag(_message, LV_OBJ_FLAG_HIDDEN);
    } else {
        const std::string text(message);
        lv_label_set_text(_message, text.c_str());
        lv_obj_remove_flag(_message, LV_OBJ_FLAG_HIDDEN);
    }
}

void CodexPetAvatar::showConversationReset()
{
    _reset_started_at = GetHAL().millis();
    _reset_feedback = true;
}

void CodexPetAvatar::renderPetFrame(uint32_t now)
{
    if (!_loaded) {
        return;
    }
    const auto& animation = _animations[static_cast<size_t>(_animation)];
    const size_t frame = animation.timing.frameAtElapsed(now - _animation_started_at);
    if (frame != _frame_index) {
        _frame_index = frame;
        lv_image_set_src(_image, &animation.frames[frame]);
    }
}

void CodexPetAvatar::update()
{
    const uint32_t now = GetHAL().millis();
    renderPetFrame(now);
    if (_reset_feedback && _loaded) {
        constexpr std::array<int, 8> angles = {-80, 80, -60, 60, -40, 40, -20, 20};
        const size_t phase = (now - _reset_started_at) / 150;
        if (phase < angles.size()) {
            lv_image_set_rotation(_image, angles[phase]);
        } else {
            lv_image_set_rotation(_image, 0);
            _reset_feedback = false;
        }
    }
    Avatar::update();
}

}  // namespace stackchan::avatar
