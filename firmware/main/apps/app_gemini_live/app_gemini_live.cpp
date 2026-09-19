/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_gemini_live.h"

#include <apps/common/common.h>
#include <assets/assets.h>
#include <mooncake_log.h>
#include <smooth_lvgl.hpp>
#include <stackchan/stackchan.h>
#include <stackchan/avatar/skins/codex_pet/codex_pet_avatar.h>

#include <memory>

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace stackchan;

namespace {

const std::string_view kTag = "GEMINI.LIVE";

}  // namespace

AppGeminiLive::AppGeminiLive()
{
    setAppInfo().name           = "GEMINI.LIVE";
    static auto icon            = assets::get_image("icon_ai_agent.bin");
    setAppInfo().icon           = (void*)&icon;
    static uint32_t theme_color = 0x4285F4;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppGeminiLive::onCreate()
{
    mclog::tagInfo(kTag, "on create");
}

void AppGeminiLive::onOpen()
{
    mclog::tagInfo(kTag, "on open");

    GetHAL().onGeminiLiveStatus.connect(
        [this](GeminiLiveStatus status, const std::string& message) { handleStatus(status, message); });

    std::unique_ptr<view::LoadingPage> loading_page;
    {
        LvglLockGuard lock;
        loading_page = std::make_unique<view::LoadingPage>(0x4285F4, 0x102A43);
    }

    GetHAL().startGeminiLiveService([&](std::string_view message) {
        LvglLockGuard lock;
        loading_page->setMessage(message);
    });

    LvglLockGuard lock;
    loading_page.reset();

    auto avatar = std::make_unique<avatar::CodexPetAvatar>();
    avatar->init(lv_screen_active());
    auto* face_panel = avatar->touchPanel();
    lv_obj_add_event_cb(face_panel, [](lv_event_t* event) {
        const auto code = lv_event_get_code(event);
        if (code == LV_EVENT_PRESSED) {
            auto& motion = GetStackChan().motion();
            const bool was_paused = motion.isMotionPaused();
            motion.setMotionPaused(true);
            if (!GetHAL().startGeminiLiveTurn()) {
                motion.setMotionPaused(was_paused);
            }
        } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            GetHAL().stopGeminiLiveTurn();
        }
    }, LV_EVENT_ALL, nullptr);
    GetStackChan().attachAvatar(std::move(avatar));

    auto& stackchan = GetStackChan();
    _imu_connection = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent event) {
        if (event == ImuMotionEvent::Shake) {
            _conversation_reset_pending = true;
        }
    });
    stackchan.addModifier(std::make_unique<IdleMotionModifier>());

    view::create_home_indicator([this]() { close(); }, 0xAECBFA, 0x102A43);
    view::create_status_bar(0xAECBFA, 0x102A43);
}

void AppGeminiLive::onRunning()
{
    LvglLockGuard lock;
    auto& stackchan = GetStackChan();
    auto& face = static_cast<avatar::CodexPetAvatar&>(stackchan.avatar());

    if (_conversation_reset_pending.exchange(false)) {
        GetHAL().resetGeminiLiveConversation();
        face.showConversationReset();
    }

    GeminiLiveStatus status;
    std::string message;
    bool status_pending = false;
    {
        std::lock_guard<std::mutex> status_lock(_status_mutex);
        status_pending = _status_pending;
        if (status_pending) {
            status          = _pending_status;
            message         = std::move(_pending_message);
            _status_pending = false;
        }
    }

    if (status_pending) {
        stackchan.motion().setMotionPaused(status == GeminiLiveStatus::Listening ||
                                          status == GeminiLiveStatus::Thinking ||
                                          status == GeminiLiveStatus::Speaking);
        face.setErrorMessage(status == GeminiLiveStatus::Error ? message : "");

        switch (status) {
            case GeminiLiveStatus::Listening:
                GetHAL().showRgbColor(0, 50, 0);
                face.setAnimation(avatar::CodexPetAnimation::Wave);
                break;
            case GeminiLiveStatus::Thinking:
                GetHAL().showRgbColor(25, 15, 0);
                face.setAnimation(avatar::CodexPetAnimation::Run);
                break;
            case GeminiLiveStatus::Speaking:
                GetHAL().showRgbColor(0, 0, 35);
                face.setAnimation(avatar::CodexPetAnimation::Review);
                break;
            case GeminiLiveStatus::Error:
                GetHAL().showRgbColor(0, 0, 0);
                face.setAnimation(avatar::CodexPetAnimation::Failed);
                break;
            case GeminiLiveStatus::Ready:
                GetHAL().showRgbColor(0, 0, 0);
                face.setAnimation(avatar::CodexPetAnimation::Waiting);
                break;
            default:
                GetHAL().showRgbColor(0, 0, 0);
                face.setAnimation(avatar::CodexPetAnimation::Idle);
                break;
        }
    }

    stackchan.update();
    view::update_home_indicator();
    view::update_status_bar();
}

void AppGeminiLive::onClose()
{
    mclog::tagInfo(kTag, "on close");

    GetHAL().onImuMotionEvent.disconnect(_imu_connection);
    GetHAL().stopGeminiLiveService();
    GetHAL().onGeminiLiveStatus.clear();
    GetHAL().showRgbColor(0, 0, 0);

    {
        LvglLockGuard lock;
        GetStackChan().clearModifiers();
        GetStackChan().motion().setMotionPaused(false);
        GetStackChan().resetAvatar();
        view::destroy_home_indicator();
        view::destroy_status_bar();
    }

    GetHAL().requestWarmReboot(0);
}

void AppGeminiLive::handleStatus(GeminiLiveStatus status, const std::string& message)
{
    std::lock_guard<std::mutex> lock(_status_mutex);
    _pending_status  = status;
    _pending_message = message;
    _status_pending  = true;
}
