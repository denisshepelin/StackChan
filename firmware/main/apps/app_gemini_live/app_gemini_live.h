/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <hal/hal.h>
#include <mooncake.h>

#include <atomic>
#include <mutex>
#include <string>

class AppGeminiLive : public mooncake::AppAbility {
public:
    AppGeminiLive();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void handleStatus(GeminiLiveStatus status, const std::string& message);

    std::atomic<bool> _conversation_reset_pending = false;
    int _imu_connection = -1;
    bool _status_pending             = false;
    GeminiLiveStatus _pending_status = GeminiLiveStatus::Connecting;
    std::string _pending_message;
    std::mutex _status_mutex;
};
