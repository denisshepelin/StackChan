/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "gemini_turn_state.h"

#include <ArduinoJson.hpp>
#include <audio/audio_codec.h>
#include <board.h>
#include <esp_ae_rate_cvt.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_websocket_client.h>
#include <freertos/queue.h>
#include <hal/board/config.h>
#include <mooncake_log.h>
#include <mbedtls/base64.h>
#include <gemini_config.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr uint32_t kInternalCaps      = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr uint32_t kPsramCaps         = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr size_t kCaptureFrames       = AUDIO_INPUT_SAMPLE_RATE * 32 / 1000;
constexpr uint32_t kInputSampleRate   = 16000;
constexpr size_t kInputFrameSamples   = kInputSampleRate * 32 / 1000;
constexpr size_t kPlaybackFrameSamples = AUDIO_OUTPUT_SAMPLE_RATE * 32 / 1000;
constexpr size_t kPlaybackPrebufferSamples = AUDIO_OUTPUT_SAMPLE_RATE * 350 / 1000;
constexpr size_t kUplinkQueueFrames   = 160;
constexpr size_t kMaxPlaybackChunks   = 64;
constexpr int32_t kPlaybackGainPercent = 60;
// Sized from measured high-water marks, with margin for the websocket error path that is not on them
constexpr uint32_t kCaptureTaskStack   = 8192;
constexpr uint32_t kSendTaskStack      = 10240;
constexpr uint32_t kPlaybackTaskStack  = 6144;
constexpr uint32_t kWebsocketTaskStack = 6144;
constexpr int kWebsocketTaskPriority   = 10;
constexpr int kWebsocketBufferSize     = 8192;
constexpr uint32_t kSetupTimeoutMs    = 20000;
constexpr uint32_t kSendTimeoutMs     = 3000;
constexpr uint32_t kMaxRecordingMs    = 30000;
constexpr uint32_t kResponseTimeoutMs = 30000;
// A send that exceeds this makes the websocket client treat the socket as dead and abort the session
constexpr uint32_t kAudioSendTimeoutMs = 1000;
const std::string_view kTag           = "Gemini-Live";
const char* kGeminiEndpoint =
    "wss://generativelanguage.googleapis.com/ws/"
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
constexpr char kSetupMessage[] =
    R"json({"setup":{"model":"models/gemini-3.8-live","systemInstruction":{"parts":[{"text":"You are a warm, patient learning companion for a curious five-year-old child. Speak only Russian, German, or English. Match the language the child is currently using; if it is unclear, use Russian. Explain things accurately in simple, age-appropriate language without talking down to the child. Start with a direct answer, usually in two or three short sentences, using concrete examples from everyday life, animals, toys, or nature when helpful. Avoid long lists, jargon, and unnecessary detail; explain unfamiliar words simply. Use words a five-year-old can understand and introduce only one new idea at a time. Do not assume the child can read or understand abstract concepts. Adapt to the child's understanding and go deeper in small steps when asked. Encourage curiosity without turning the conversation into a lesson or quiz. When it fits naturally, offer one short, intriguing related question the child might enjoy exploring, such as 'Want to find out why...?' Do not add a follow-up to every answer, and let the child choose the direction. Welcome repeated questions and gently correct misconceptions. Be honest when you do not know; do not invent facts. Handle sensitive topics calmly and factually at a child-appropriate level. Do not give instructions for dangerous activities; suggest a safe alternative and help from a trusted adult when needed. Do not request personal information or encourage keeping secrets from caregivers. Be clear that you are an AI companion, not a human, if asked."}]},"generationConfig":{"responseModalities":["AUDIO"]},"tools":[{"googleSearch":{}}],"realtimeInputConfig":{"automaticActivityDetection":{"disabled":true},"activityHandling":"START_OF_ACTIVITY_INTERRUPTS"},"inputAudioTranscription":{}}})json";
constexpr std::string_view kAudioMessagePrefix = R"json({"realtimeInput":{"audio":{"data":")json";
constexpr std::string_view kAudioMessageSuffix = R"json(","mimeType":"audio/pcm;rate=16000"}}})json";

enum class AudioPacketKind { Begin, Audio, End };

struct AudioFrame {
    AudioPacketKind kind;
    uint32_t turn_id;
    size_t sample_count;
    int16_t samples[kInputFrameSamples];
};

bool isJsonWhitespace(char value)
{
    return value == ' ' || value == '\n' || value == '\r' || value == '\t';
}

size_t findJsonObjectEnd(std::string_view json, size_t object_start)
{
    size_t depth    = 0;
    bool in_string  = false;
    bool is_escaped = false;
    for (size_t index = object_start; index < json.size(); ++index) {
        const char value = json[index];
        if (in_string) {
            if (is_escaped) {
                is_escaped = false;
            } else if (value == '\\') {
                is_escaped = true;
            } else if (value == '"') {
                in_string = false;
            }
            continue;
        }
        if (value == '"') {
            in_string = true;
        } else if (value == '{') {
            ++depth;
        } else if (value == '}' && --depth == 0) {
            return index;
        }
    }
    return std::string_view::npos;
}

std::string_view findJsonStringValue(std::string_view object, std::string_view key)
{
    size_t position = 0;
    while ((position = object.find(key, position)) != std::string_view::npos) {
        if (position == 0 || object[position - 1] != '"' || position + key.size() >= object.size() ||
            object[position + key.size()] != '"') {
            position += key.size();
            continue;
        }
        position += key.size() + 1;
        while (position < object.size() && isJsonWhitespace(object[position])) {
            ++position;
        }
        if (position >= object.size() || object[position++] != ':') {
            continue;
        }
        while (position < object.size() && isJsonWhitespace(object[position])) {
            ++position;
        }
        if (position >= object.size() || object[position++] != '"') {
            continue;
        }

        const size_t value_start = position;
        bool is_escaped          = false;
        for (; position < object.size(); ++position) {
            const char value = object[position];
            if (is_escaped) {
                return {};
            }
            if (value == '\\') {
                is_escaped = true;
            } else if (value == '"') {
                return object.substr(value_start, position - value_start);
            }
        }
        return {};
    }
    return {};
}

template <typename T, uint32_t Caps>
class CapabilityAllocator {
public:
    using value_type = T;
    using is_always_equal = std::true_type;

    CapabilityAllocator() noexcept = default;

    template <typename U>
    CapabilityAllocator(const CapabilityAllocator<U, Caps>&) noexcept
    {
    }

    [[nodiscard]] T* allocate(size_t count)
    {
        if (count == 0) {
            return nullptr;
        }
        if (count > std::numeric_limits<size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        auto* memory = static_cast<T*>(heap_caps_malloc(count * sizeof(T), Caps));
        if (!memory) {
            throw std::bad_alloc();
        }
        return memory;
    }

    void deallocate(T* memory, size_t) noexcept
    {
        heap_caps_free(memory);
    }

    template <typename U>
    struct rebind {
        using other = CapabilityAllocator<U, Caps>;
    };
};

template <typename T, typename U, uint32_t Caps>
constexpr bool operator==(const CapabilityAllocator<T, Caps>&, const CapabilityAllocator<U, Caps>&) noexcept
{
    return true;
}

template <typename T, typename U, uint32_t Caps>
constexpr bool operator!=(const CapabilityAllocator<T, Caps>&, const CapabilityAllocator<U, Caps>&) noexcept
{
    return false;
}

using InternalAudioBuffer = std::vector<int16_t, CapabilityAllocator<int16_t, kInternalCaps>>;
using PsramAudioBuffer    = std::vector<int16_t, CapabilityAllocator<int16_t, kPsramCaps>>;
using PsramString = std::basic_string<char, std::char_traits<char>, CapabilityAllocator<char, kPsramCaps>>;
using PsramPlaybackQueue =
    std::deque<PsramAudioBuffer, CapabilityAllocator<PsramAudioBuffer, kPsramCaps>>;

class PsramJsonAllocator final : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override
    {
        return heap_caps_malloc(size, kPsramCaps);
    }

    void deallocate(void* memory) override
    {
        heap_caps_free(memory);
    }

    void* reallocate(void* memory, size_t size) override
    {
        return heap_caps_realloc(memory, size, kPsramCaps);
    }
};

PsramJsonAllocator psram_json_allocator;

class GeminiLiveClient {
public:
    GeminiLiveClient() : _response_filter(&psram_json_allocator)
    {
        _response_filter["setupComplete"]                              = true;
        _response_filter["serverContent"]["inputTranscription"]["text"] = true;
        _response_filter["serverContent"]["interrupted"]              = true;
        _response_filter["serverContent"]["turnComplete"]             = true;
    }

    static void* operator new(size_t size)
    {
        auto* memory = heap_caps_malloc(size, kInternalCaps);
        if (!memory) {
            throw std::bad_alloc();
        }
        return memory;
    }

    static void operator delete(void* memory) noexcept
    {
        heap_caps_free(memory);
    }

    ~GeminiLiveClient()
    {
        stop();
    }

    bool start()
    {
        if (std::strlen(STACKCHAN_GEMINI_API_KEY) == 0) {
            emitStatus(GeminiLiveStatus::Error, "GOOGLE_API_KEY was not embedded");
            return false;
        }

        auto& board = Board::GetInstance();
        _codec      = board.GetAudioCodec();
        if (!_codec) {
            emitStatus(GeminiLiveStatus::Error, "Audio codec unavailable");
            return false;
        }
        if (!initializeResampler()) {
            emitStatus(GeminiLiveStatus::Error, "Microphone resampler unavailable");
            return false;
        }
        if (!initializeAudioBuffers()) {
            emitStatus(GeminiLiveStatus::Error, "Could not create audio buffers");
            return false;
        }

        _running = true;
        _capture_finished = false;
        // Network writes run on the send task, never on the UI or capture task.
        BaseType_t result =
            xTaskCreatePinnedToCore(captureTaskEntry, "gemini_capture", kCaptureTaskStack, this, 5, &_capture_task, 1);
        if (result != pdPASS) {
            _capture_finished = true;
            _running = false;
            emitStatus(GeminiLiveStatus::Error, "Could not start capture task");
            return false;
        }

        _send_finished = false;
        result = xTaskCreatePinnedToCore(sendTaskEntry, "gemini_send", kSendTaskStack, this, 9, &_send_task, 0);
        if (result != pdPASS) {
            _send_finished = true;
            _running = false;
            emitStatus(GeminiLiveStatus::Error, "Could not start send task");
            return false;
        }

        _playback_finished = false;
        result =
            xTaskCreatePinnedToCore(playbackTaskEntry, "gemini_playback", kPlaybackTaskStack, this, 8, &_playback_task, 1);
        if (result != pdPASS) {
            _playback_finished = true;
            _running = false;
            emitStatus(GeminiLiveStatus::Error, "Could not start playback task");
            return false;
        }

        emitStatus(GeminiLiveStatus::Ready, "Hold the face to talk");
        return true;
    }

    bool startTurn()
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (!_running || _resetting || _reset_pending || !_turn.press()) {
            return false;
        }
        clearPlaybackLocked();
        _accept_response = false;
        _capture_pending = true;
        _turn_started_at = GetHAL().millis();
        resetPerformanceCounters();
        emitStatus(GeminiLiveStatus::Listening, "Release to send");
        return true;
    }

    void stopTurn()
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (_turn.release()) {
            emitStatus(GeminiLiveStatus::Thinking, "Thinking...");
        }
    }

    void resetConversation()
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (!_running) {
            return;
        }
        _turn.resetConversation();
        _capture_pending = false;
        _accept_response = false;
        clearPlaybackLocked();
        _reset_pending = true;
        emitStatus(GeminiLiveStatus::Connecting, "New conversation...");
    }

    void stop()
    {
        if (_stopping.exchange(true)) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(_state_mutex);
            _running = false;
            _accept_response = false;
        }

        // Workers own mutexes and codec buffers; let bounded I/O finish before freeing them.
        while (!_capture_finished || !_send_finished || !_playback_finished) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        destroyClient();
        _connected = false;
        if (_input_resampler) {
            esp_ae_rate_cvt_close(_input_resampler);
            _input_resampler = nullptr;
        }
        destroyAudioBuffers();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }

private:
    bool initializeAudioBuffers()
    {
        _active_audio_frames = static_cast<AudioFrame*>(
            heap_caps_aligned_calloc(16, 2, sizeof(AudioFrame), kInternalCaps));
        _uplink_queue_storage = static_cast<uint8_t*>(
            heap_caps_malloc(kUplinkQueueFrames * sizeof(AudioFrame), kPsramCaps));
        if (!_active_audio_frames || !_uplink_queue_storage) {
            destroyAudioBuffers();
            return false;
        }

        _uplink_queue = xQueueCreateStatic(kUplinkQueueFrames, sizeof(AudioFrame), _uplink_queue_storage,
                                           &_uplink_queue_control);
        if (!_uplink_queue) {
            destroyAudioBuffers();
            return false;
        }

        mclog::tagInfo(kTag, "Audio buffers: active={}B internal, uplink={}B psram",
                       2 * sizeof(AudioFrame), kUplinkQueueFrames * sizeof(AudioFrame));
        return true;
    }

    void destroyAudioBuffers()
    {
        if (_uplink_queue) {
            vQueueDelete(_uplink_queue);
            _uplink_queue = nullptr;
        }
        if (_uplink_queue_storage) {
            heap_caps_free(_uplink_queue_storage);
            _uplink_queue_storage = nullptr;
        }
        if (_active_audio_frames) {
            heap_caps_free(_active_audio_frames);
            _active_audio_frames = nullptr;
        }
    }

    bool initializeResampler()
    {
        esp_ae_rate_cvt_cfg_t config = {
            .src_rate = AUDIO_INPUT_SAMPLE_RATE,
            .dest_rate = kInputSampleRate,
            .channel = 1,
            .bits_per_sample = ESP_AE_BIT16,
            .complexity = 2,
            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        return esp_ae_rate_cvt_open(&config, &_input_resampler) == ESP_AE_ERR_OK && _input_resampler &&
               esp_ae_rate_cvt_get_max_out_sample_num(_input_resampler, kCaptureFrames, &_resampled_capacity) ==
                   ESP_AE_ERR_OK && _resampled_capacity > 0;
    }

    bool connectSession()
    {
        _url.clear();
        _url.append(kGeminiEndpoint);
        _url.append(STACKCHAN_GEMINI_API_KEY);
        esp_websocket_client_config_t config{};
        config.uri                    = _url.c_str();
        config.crt_bundle_attach      = esp_crt_bundle_attach;
        config.disable_auto_reconnect = true;
        config.network_timeout_ms     = 10000;
        config.ping_interval_sec      = 20;
        config.buffer_size            = kWebsocketBufferSize;
        config.task_stack             = kWebsocketTaskStack;
        config.task_core_id_set       = true;
        config.task_core_id           = 0;
        config.task_prio              = kWebsocketTaskPriority;
        config.user_context           = this;

        _client = esp_websocket_client_init(&config);
        if (!_client) {
            emitStatus(GeminiLiveStatus::Error, "Could not create WebSocket");
            return false;
        }

        if (esp_websocket_register_events(_client, WEBSOCKET_EVENT_ANY, websocketEvent, this) != ESP_OK ||
            esp_websocket_client_start(_client) != ESP_OK) {
            emitStatus(GeminiLiveStatus::Error, "Could not connect to Gemini");
            destroyClient();
            return false;
        }
        return true;
    }

    static void websocketEvent(void* context, esp_event_base_t, int32_t event_id, void* event_data)
    {
        static_cast<GeminiLiveClient*>(context)->handleWebsocketEvent(
            static_cast<esp_websocket_event_id_t>(event_id), static_cast<esp_websocket_event_data_t*>(event_data));
    }

    void handleWebsocketEvent(esp_websocket_event_id_t event_id, esp_websocket_event_data_t* data)
    {
        if (event_id == WEBSOCKET_EVENT_CONNECTED) {
            _connected = true;
            _setup_send_pending = true;
            _connected_at = GetHAL().millis();
            mclog::tagInfo(kTag, "Gemini connected");
            return;
        }

        if (event_id == WEBSOCKET_EVENT_DATA && data) {
            if (data->op_code != 0x01 && data->op_code != 0x02 && data->op_code != 0x00) {
                return;
            }
            const size_t payload_size = static_cast<size_t>(std::max(data->payload_len, 0));
            const size_t offset       = static_cast<size_t>(std::max(data->payload_offset, 0));
            const size_t chunk_size   = static_cast<size_t>(std::max(data->data_len, 0));
            if (offset == 0) {
                _incoming_message.assign(payload_size, '\0');
            }
            if (offset + chunk_size > _incoming_message.size()) {
                _incoming_message.clear();
                _transport_failed = true;
                return;
            }
            std::memcpy(_incoming_message.data() + offset, data->data_ptr, chunk_size);
            if (offset + chunk_size == payload_size) {
                handleMessage(_incoming_message.data(), _incoming_message.size());
                _incoming_message.clear();
            }
            return;
        }

        if (event_id == WEBSOCKET_EVENT_ERROR && data) {
            if (!_stopping && !_resetting) {
                mclog::tagError(kTag, "WebSocket error: type={}, tls={}, socket={}",
                                static_cast<int>(data->error_handle.error_type),
                                static_cast<int>(data->error_handle.esp_tls_last_esp_err),
                                data->error_handle.esp_transport_sock_errno);
                _transport_failed = true;
            }
            return;
        }

        if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED) {
            _connected = false;
            if (!_stopping && !_resetting) {
                _transport_failed = true;
            }
        }
    }

    bool sendText(std::string_view message, uint32_t timeout_ms = kSendTimeoutMs)
    {
        std::lock_guard<std::mutex> lock(_protocol_mutex);
        if (!_client || !esp_websocket_client_is_connected(_client)) {
            return false;
        }
        return esp_websocket_client_send_text(_client, message.data(), message.size(), pdMS_TO_TICKS(timeout_ms)) ==
               static_cast<int>(message.size());
    }

    void destroyClient()
    {
        std::lock_guard<std::mutex> lock(_protocol_mutex);
        if (!_client) {
            return;
        }
        esp_websocket_unregister_events(_client, WEBSOCKET_EVENT_ANY, websocketEvent);
        esp_websocket_client_stop(_client);
        esp_websocket_client_destroy(_client);
        _client = nullptr;
        _incoming_message.clear();
    }

    void closeSession()
    {
        _resetting = true;
        destroyClient();
        _connected = false;
        _session_ready = false;
        _setup_send_pending = false;
        _server_turn_active = false;
        _transport_failed = false;
        _capture_failed = false;
        _uplink_overflow = false;
        xQueueReset(_uplink_queue);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        _resetting = false;
    }

    bool isCurrentTurn(uint32_t turn_id)
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        return _running && !_reset_pending && _turn.turnId() == turn_id;
    }

    void failSession(const char* message)
    {
        {
            std::lock_guard<std::mutex> lock(_state_mutex);
            _turn.resetConversation();
            _capture_pending = false;
            _accept_response = false;
            clearPlaybackLocked();
        }
        closeSession();
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (!_reset_pending) {
            _turn.fail();
            emitStatus(GeminiLiveStatus::Error, message);
        }
    }

    bool ensureSession(uint32_t turn_id)
    {
        if (_session_ready) {
            return true;
        }
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (!_client && !connectSession()) {
            return false;
        }
        const uint32_t started_at = GetHAL().millis();
        while (isCurrentTurn(turn_id) && !_transport_failed && !_capture_failed && !_uplink_overflow &&
               GetHAL().millis() - started_at < kSetupTimeoutMs) {
            if (_setup_send_pending && GetHAL().millis() - _connected_at >= 100) {
                _setup_send_pending = false;
                if (!sendText(kSetupMessage)) {
                    return false;
                }
            }
            if (_session_ready) {
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return false;
    }

    static void captureTaskEntry(void* context)
    {
        auto* client = static_cast<GeminiLiveClient*>(context);
        client->captureTask();
        client->_capture_finished = true;
        vTaskDelete(nullptr);
    }

    static void playbackTaskEntry(void* context)
    {
        auto* client = static_cast<GeminiLiveClient*>(context);
        client->playbackTask();
        client->_playback_finished = true;
        vTaskDelete(nullptr);
    }

    static void sendTaskEntry(void* context)
    {
        auto* client = static_cast<GeminiLiveClient*>(context);
        client->sendTask();
        client->_send_finished = true;
        vTaskDelete(nullptr);
    }

    void captureTask()
    {
        const size_t channels = std::max(_codec->input_channels(), 1);
        InternalAudioBuffer input(kCaptureFrames * channels);
        InternalAudioBuffer mono(kCaptureFrames);
        InternalAudioBuffer resampled(_resampled_capacity);
        auto& packet = _active_audio_frames[0];
        bool capturing = false;
        uint32_t capture_turn = 0;

        while (_running) {
            bool begin = false;
            bool recording = false;
            bool current = false;
            {
                std::lock_guard<std::mutex> lock(_state_mutex);
                if (_capture_pending) {
                    _capture_pending = false;
                    capture_turn = _turn.turnId();
                    begin = true;
                }
                current = capture_turn == _turn.turnId();
                if (_turn.phase() == gemini::TurnState::Phase::Recording &&
                    GetHAL().millis() - _turn_started_at >= kMaxRecordingMs) {
                    _turn.release();
                    emitStatus(GeminiLiveStatus::Thinking, "Thinking...");
                }
                recording = current && _turn.phase() == gemini::TurnState::Phase::Recording;
            }

            if (begin) {
                esp_ae_rate_cvt_reset(_input_resampler);
                packet = {};
                packet.kind = AudioPacketKind::Begin;
                packet.turn_id = capture_turn;
                enqueueAudio(packet);
                capturing = true;
            }

            if (capturing && recording) {
                bool captured = false;
                {
                    std::lock_guard<std::mutex> codec_lock(_codec_mutex);
                    _codec->EnableOutput(false);
                    _codec->EnableInput(true);
                    captured = _codec->InputData(input.data(), input.size());
                }
                if (!captured) {
                    _capture_failed = true;
                    continue;
                }
                for (size_t frame = 0; frame < kCaptureFrames; ++frame) {
                    mono[frame] = input[frame * channels];
                }
                uint32_t count = _resampled_capacity;
                if (esp_ae_rate_cvt_process(_input_resampler, mono.data(), kCaptureFrames,
                                            resampled.data(), &count) != ESP_AE_ERR_OK) {
                    _capture_failed = true;
                    continue;
                }
                for (size_t offset = 0; offset < count; offset += kInputFrameSamples) {
                    packet.kind = AudioPacketKind::Audio;
                    packet.sample_count = std::min(kInputFrameSamples, static_cast<size_t>(count) - offset);
                    std::memcpy(packet.samples, resampled.data() + offset,
                                packet.sample_count * sizeof(int16_t));
                    enqueueAudio(packet);
                }
                continue;
            }

            if (capturing) {
                {
                    std::lock_guard<std::mutex> codec_lock(_codec_mutex);
                    _codec->EnableInput(false);
                }
                if (current) {
                    packet.kind = AudioPacketKind::End;
                    packet.sample_count = 0;
                    enqueueAudio(packet);
                }
                capturing = false;
            }
            // A millisecond delay shorter than the FreeRTOS tick would busy-loop.
            vTaskDelay(1);
        }
        {
            std::lock_guard<std::mutex> codec_lock(_codec_mutex);
            _codec->EnableInput(false);
        }
    }

    void enqueueAudio(const AudioFrame& packet)
    {
        if (!isCurrentTurn(packet.turn_id)) {
            return;
        }
        if (xQueueSend(_uplink_queue, &packet, 0) == pdTRUE) {
            recordMaximum(_uplink_max_depth, uxQueueMessagesWaiting(_uplink_queue));
        } else {
            // Never submit a silently truncated utterance after network congestion.
            _uplink_overflow = true;
        }
    }

    bool sendPacket(const AudioFrame& packet)
    {
        if (packet.kind == AudioPacketKind::Begin) {
            if (!ensureSession(packet.turn_id) ||
                !sendText("{\"realtimeInput\":{\"activityStart\":{}}}")) {
                return false;
            }
            // Drain the previous response's interruption/completion before accepting a new response.
            const uint32_t started_at = GetHAL().millis();
            while (_server_turn_active && isCurrentTurn(packet.turn_id) && !_transport_failed) {
                if (GetHAL().millis() - started_at >= kSendTimeoutMs) {
                    return false;
                }
                vTaskDelay(1);
            }
            return true;
        }
        if (packet.kind == AudioPacketKind::End) {
            {
                std::lock_guard<std::mutex> lock(_state_mutex);
                if (_turn.turnId() != packet.turn_id || _reset_pending) {
                    return true;
                }
                _accept_response = true;
                _server_turn_active = true;
                _response_started_at = GetHAL().millis();
            }
            return sendText("{\"realtimeInput\":{\"activityEnd\":{}}}");
        }
        const int64_t started_at = esp_timer_get_time();
        const bool sent = sendAudio(packet.samples, packet.sample_count);
        recordDuration(_send_time_us, _send_max_us,
                       static_cast<uint32_t>(esp_timer_get_time() - started_at));
        ++_send_frames;
        return sent;
    }

    void sendTask()
    {
        while (_running) {
            if (_reset_pending.exchange(false)) {
                closeSession();
                std::lock_guard<std::mutex> lock(_state_mutex);
                if (!_reset_pending) {
                    _turn.finishReset();
                    emitStatus(GeminiLiveStatus::Ready, "New conversation. Hold the face to talk");
                }
                continue;
            }
            if (_capture_failed.exchange(false)) {
                failSession("Microphone capture failed. Hold to retry");
                continue;
            }
            if (_uplink_overflow.exchange(false)) {
                failSession("Network too slow. Hold to retry");
                continue;
            }
            if (_transport_failed.exchange(false)) {
                failSession("Connection lost. Hold to start a new conversation");
                continue;
            }
            bool timed_out = false;
            {
                std::lock_guard<std::mutex> lock(_state_mutex);
                timed_out = _accept_response && !_turn_complete &&
                            GetHAL().millis() - _response_started_at >= kResponseTimeoutMs;
            }
            if (timed_out) {
                failSession("Response timed out. Hold to start a new conversation");
                continue;
            }
            auto& packet = _active_audio_frames[1];
            if (xQueueReceive(_uplink_queue, &packet, pdMS_TO_TICKS(10)) != pdTRUE ||
                !isCurrentTurn(packet.turn_id)) {
                continue;
            }
            if (!sendPacket(packet) && isCurrentTurn(packet.turn_id)) {
                failSession("Could not send turn. Hold to start a new conversation");
            }
        }
    }

    void playbackTask()
    {
        while (_running) {
            PsramAudioBuffer samples;
            uint32_t playback_turn = 0;
            {
                std::lock_guard<std::mutex> lock(_state_mutex);
                playback_turn = _turn.turnId();
                const bool enough_audio = _playback_queued_samples >= kPlaybackPrebufferSamples;
                if (_accept_response && !_playback.empty() &&
                    (_turn.phase() == gemini::TurnState::Phase::Speaking || enough_audio || _turn_complete)) {
                    if (_turn.startPlayback(playback_turn)) {
                        emitStatus(GeminiLiveStatus::Speaking, "");
                    }
                    samples = std::move(_playback.front());
                    _playback.pop_front();
                    _playback_queued_samples -= samples.size();
                }
            }
            if (!samples.empty()) {
                for (size_t offset = 0; offset < samples.size() && _running; offset += kPlaybackFrameSamples) {
                    std::lock_guard<std::mutex> codec_lock(_codec_mutex);
                    {
                        std::lock_guard<std::mutex> lock(_state_mutex);
                        if (_turn.turnId() != playback_turn ||
                            _turn.phase() != gemini::TurnState::Phase::Speaking) {
                            _codec->EnableOutput(false);
                            break;
                        }
                    }
                    _codec->EnableInput(false);
                    _codec->EnableOutput(true);
                    _codec->OutputData(samples.data() + offset,
                                       std::min(kPlaybackFrameSamples, samples.size() - offset));
                }
                continue;
            }
            {
                std::lock_guard<std::mutex> codec_lock(_codec_mutex);
                std::lock_guard<std::mutex> lock(_state_mutex);
                if (_accept_response && _turn_complete && _playback.empty()) {
                    _codec->EnableOutput(false);
                    _accept_response = false;
                    _turn_complete = false;
                    if (_turn.finishResponse(playback_turn)) {
                        logPerformanceCounters();
                        emitStatus(GeminiLiveStatus::Ready, "Hold the face to talk");
                    }
                } else if (_turn.phase() != gemini::TurnState::Phase::Speaking) {
                    _codec->EnableOutput(false);
                }
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
        }
        {
            std::lock_guard<std::mutex> codec_lock(_codec_mutex);
            _codec->EnableOutput(false);
        }
    }

    void handleMessage(const char* data, size_t length)
    {
        ArduinoJson::JsonDocument document(&psram_json_allocator);
        if (ArduinoJson::deserializeJson(document, data, length,
                                         ArduinoJson::DeserializationOption::Filter(_response_filter))) {
            _transport_failed = true;
            return;
        }

        if (!document["setupComplete"].isNull()) {
            mclog::tagInfo(kTag, "Gemini setup complete");
            _session_ready = true;
            return;
        }

        auto server_content = document["serverContent"];
        if (server_content.isNull()) {
            return;
        }

        const char* input_transcription = server_content["inputTranscription"]["text"] | "";
        if (input_transcription[0] != '\0') {
            mclog::tagInfo(kTag, "Input: {}", input_transcription);
        }

        std::lock_guard<std::mutex> lock(_state_mutex);
        if (!_running || _reset_pending || _resetting) {
            return;
        }
        const bool interrupted = server_content["interrupted"] | false;
        const bool complete = server_content["turnComplete"] | false;
        // An interrupted event is followed by turnComplete; only the latter ends the old response.
        if (complete) {
            _server_turn_active = false;
        }
        if (!_accept_response) {
            return;
        }
        _response_started_at = GetHAL().millis();
        if (interrupted) {
            clearPlaybackLocked();
        } else {
            queueInlineAudio(std::string_view(data, length));
        }
        if (complete) {
            _turn_complete = true;
        }
        if (_playback_task) {
            xTaskNotifyGive(_playback_task);
        }
    }

    bool sendAudio(const int16_t* samples, size_t sample_count)
    {
        const auto* bytes             = reinterpret_cast<const unsigned char*>(samples);
        const size_t byte_count       = sample_count * sizeof(int16_t);
        const size_t encoded_capacity = 4 * ((byte_count + 2) / 3) + 1;
        const size_t encoded_offset   = kAudioMessagePrefix.size();
        _audio_message.assign(kAudioMessagePrefix);
        _audio_message.resize(encoded_offset + encoded_capacity + kAudioMessageSuffix.size());

        size_t encoded_size = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(_audio_message.data() + encoded_offset),
                                  encoded_capacity, &encoded_size, bytes, byte_count) != 0) {
            return false;
        }
        std::memcpy(_audio_message.data() + encoded_offset + encoded_size, kAudioMessageSuffix.data(),
                    kAudioMessageSuffix.size());
        _audio_message.resize(encoded_offset + encoded_size + kAudioMessageSuffix.size());
        return sendText(_audio_message, kAudioSendTimeoutMs);
    }

    void queueInlineAudio(std::string_view message)
    {
        constexpr std::string_view inline_data_key = "\"inlineData\"";
        size_t position = 0;
        while ((position = message.find(inline_data_key, position)) != std::string_view::npos) {
            position += inline_data_key.size();
            while (position < message.size() && isJsonWhitespace(message[position])) {
                ++position;
            }
            if (position >= message.size() || message[position++] != ':') {
                continue;
            }
            while (position < message.size() && isJsonWhitespace(message[position])) {
                ++position;
            }
            if (position >= message.size() || message[position] != '{') {
                continue;
            }

            const size_t object_end = findJsonObjectEnd(message, position);
            if (object_end == std::string_view::npos) {
                return;
            }
            const std::string_view object = message.substr(position, object_end - position + 1);
            const std::string_view mime_type = findJsonStringValue(object, "mimeType");
            const std::string_view encoded   = findJsonStringValue(object, "data");
            if (mime_type.starts_with("audio/pcm") && !encoded.empty()) {
                warnOnRateMismatch(mime_type);
                queueAudio(encoded);
            }
            position = object_end + 1;
        }
    }

    // Playback feeds the codec unresampled, so a rate other than the codec's would play at the wrong speed
    void warnOnRateMismatch(std::string_view mime_type)
    {
        const size_t rate_position = mime_type.find("rate=");
        if (rate_position == std::string_view::npos) {
            return;
        }

        long rate = 0;
        for (size_t position = rate_position + 5; position < mime_type.size(); ++position) {
            const char digit = mime_type[position];
            if (digit < '0' || digit > '9') {
                break;
            }
            rate = rate * 10 + digit - '0';
        }
        if (rate > 0 && rate != AUDIO_OUTPUT_SAMPLE_RATE && !_rate_mismatch_logged.exchange(true)) {
            mclog::tagError(kTag, "Gemini audio rate {} does not match codec rate {}; playback speed will be wrong",
                            rate, AUDIO_OUTPUT_SAMPLE_RATE);
        }
    }

    bool queueAudio(std::string_view encoded)
    {
        if (!_accept_response || encoded.empty()) {
            return false;
        }

        const size_t decoded_capacity = ((encoded.size() + 3) / 4) * 3;
        PsramAudioBuffer decoded((decoded_capacity + sizeof(int16_t) - 1) / sizeof(int16_t));
        size_t decoded_size = 0;
        if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(decoded.data()),
                                  decoded.size() * sizeof(int16_t), &decoded_size,
                                  reinterpret_cast<const unsigned char*>(encoded.data()), encoded.size()) != 0 ||
            decoded_size == 0 || decoded_size % sizeof(int16_t) != 0) {
            return false;
        }
        decoded.resize(decoded_size / sizeof(int16_t));
        std::transform(decoded.begin(), decoded.end(), decoded.begin(), [](int16_t sample) {
            return static_cast<int16_t>(static_cast<int32_t>(sample) * kPlaybackGainPercent / 100);
        });
        if (_playback.size() >= kMaxPlaybackChunks) {
            _transport_failed = true;
            return false;
        }
        _playback_queued_samples += decoded.size();
        _playback.push_back(std::move(decoded));
        ++_playback_chunks;
        _playback_bytes += decoded_size;
        recordMaximum(_playback_max_depth, static_cast<uint32_t>(_playback.size()));
        if (_playback_task) {
            xTaskNotifyGive(_playback_task);
        }
        return true;
    }

    // Called with _state_mutex held; in-flight playback checks the turn ID every 32 ms.
    void clearPlaybackLocked()
    {
        _playback.clear();
        _playback_queued_samples = 0;
        _turn_complete = false;
    }

    void emitStatus(GeminiLiveStatus status, std::string message)
    {
        GetHAL().onGeminiLiveStatus.emit(status, message);
    }

    static void recordDuration(std::atomic<uint32_t>& total, std::atomic<uint32_t>& maximum, uint32_t duration)
    {
        total += duration;
        recordMaximum(maximum, duration);
    }

    static void recordMaximum(std::atomic<uint32_t>& maximum, uint32_t value)
    {
        uint32_t current = maximum;
        while (current < value && !maximum.compare_exchange_weak(current, value)) {
        }
    }

    void resetPerformanceCounters()
    {
        _send_frames        = 0;
        _send_time_us       = 0;
        _send_max_us        = 0;
        _playback_chunks    = 0;
        _playback_bytes     = 0;
        _playback_max_depth = 0;
        _uplink_max_depth   = 0;
    }

    void logPerformanceCounters()
    {
        const uint32_t send_frames = _send_frames;
        mclog::tagInfo(kTag, "Stack unused: capture={}B send={}B playback={}B; free internal={}B",
                       _capture_task ? uxTaskGetStackHighWaterMark(_capture_task) : 0,
                       _send_task ? uxTaskGetStackHighWaterMark(_send_task) : 0,
                       _playback_task ? uxTaskGetStackHighWaterMark(_playback_task) : 0,
                       heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        mclog::tagInfo(kTag,
                       "Perf: send avg={}us max={}us; uplink max_queue={}; "
                       "playback chunks={} bytes={} max_queue={}",
                       send_frames ? static_cast<uint32_t>(_send_time_us) / send_frames : 0,
                       static_cast<uint32_t>(_send_max_us), static_cast<uint32_t>(_uplink_max_depth),
                       static_cast<uint32_t>(_playback_chunks),
                       static_cast<uint32_t>(_playback_bytes), static_cast<uint32_t>(_playback_max_depth));
    }

    AudioCodec* _codec = nullptr;
    esp_ae_rate_cvt_handle_t _input_resampler = nullptr;
    uint32_t _resampled_capacity = 0;
    // Capture and send each own one packet in internal SRAM.
    AudioFrame* _active_audio_frames = nullptr;
    uint8_t* _uplink_queue_storage = nullptr;
    StaticQueue_t _uplink_queue_control{};
    QueueHandle_t _uplink_queue = nullptr;
    esp_websocket_client_handle_t _client = nullptr;
    PsramString _url;
    PsramString _incoming_message;
    PsramString _audio_message;
    ArduinoJson::JsonDocument _response_filter;
    TaskHandle_t _capture_task       = nullptr;
    TaskHandle_t _send_task          = nullptr;
    TaskHandle_t _playback_task      = nullptr;
    std::atomic<bool> _capture_finished = true;
    std::atomic<bool> _send_finished = true;
    std::atomic<bool> _playback_finished = true;
    std::atomic<bool> _running       = false;
    std::atomic<bool> _stopping      = false;
    std::atomic<bool> _resetting     = false;
    std::atomic<bool> _connected     = false;
    std::atomic<bool> _setup_send_pending = false;
    std::atomic<bool> _session_ready = false;
    std::atomic<bool> _server_turn_active = false;
    std::atomic<bool> _transport_failed = false;
    std::atomic<bool> _capture_failed = false;
    std::atomic<bool> _uplink_overflow = false;
    std::atomic<bool> _reset_pending = false;
    std::atomic<bool> _rate_mismatch_logged = false;
    std::atomic<uint32_t> _connected_at = 0;
    // Protected by _state_mutex, including response and playback ownership.
    gemini::TurnState _turn;
    bool _capture_pending = false;
    bool _accept_response = false;
    bool _turn_complete = false;
    uint32_t _turn_started_at = 0;
    uint32_t _response_started_at = 0;
    std::atomic<uint32_t> _send_frames = 0;
    std::atomic<uint32_t> _send_time_us = 0;
    std::atomic<uint32_t> _send_max_us = 0;
    std::atomic<uint32_t> _playback_chunks = 0;
    std::atomic<uint32_t> _playback_bytes = 0;
    std::atomic<uint32_t> _playback_max_depth = 0;
    std::atomic<uint32_t> _uplink_max_depth = 0;
    std::mutex _protocol_mutex;
    std::mutex _state_mutex;
    std::mutex _codec_mutex;
    PsramPlaybackQueue _playback;
    size_t _playback_queued_samples = 0;
};

std::unique_ptr<GeminiLiveClient> gemini_live_client;

}  // namespace

bool Hal::startGeminiLiveService(std::function<void(std::string_view)> onLog)
{
    startNetwork(onLog);
    if (onLog) {
        onLog("Starting Gemini Live...");
    }

    gemini_live_client = std::make_unique<GeminiLiveClient>();
    return gemini_live_client->start();
}

bool Hal::startGeminiLiveTurn()
{
    return gemini_live_client && gemini_live_client->startTurn();
}

void Hal::stopGeminiLiveTurn()
{
    if (gemini_live_client) {
        gemini_live_client->stopTurn();
    }
}

void Hal::resetGeminiLiveConversation()
{
    if (gemini_live_client) {
        gemini_live_client->resetConversation();
    }
}

void Hal::stopGeminiLiveService()
{
    gemini_live_client.reset();
}
