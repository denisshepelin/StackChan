/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

#include <ArduinoJson.hpp>
#include <audio/audio_codec.h>
#include <board.h>
#include <esp_ae_rate_cvt.h>
#include <esp_aec.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <esp_websocket_client.h>
#include <hal/board/config.h>
#include <mooncake_log.h>
#include <mbedtls/base64.h>
#include <gemini_config.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr size_t kCaptureFrames       = AUDIO_INPUT_SAMPLE_RATE / 25;
constexpr uint32_t kAecSampleRate     = 16000;
constexpr size_t kMaxPlaybackChunks   = 64;
constexpr uint32_t kTaskStopTimeoutMs = 2000;
constexpr uint32_t kSetupTimeoutMs    = 20000;
constexpr uint32_t kSendTimeoutMs     = 3000;
const std::string_view kTag           = "Gemini-Live";
const char* kGeminiEndpoint =
    "wss://generativelanguage.googleapis.com/ws/"
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=";
constexpr char kSetupMessage[] =
    R"json({"setup":{"model":"models/gemini-3.1-flash-live-preview","generationConfig":{"responseModalities":["AUDIO"]}}})json";
static_assert(sizeof(kSetupMessage) - 1 < 126, "Gemini setup must use a single-byte WebSocket payload length");

class GeminiLiveClient {
public:
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
        if (!_codec->input_reference() || _codec->input_channels() < 2 || !initializeAec()) {
            emitStatus(GeminiLiveStatus::Error, "Echo cancellation unavailable");
            return false;
        }

        _running          = true;
        BaseType_t result =
            xTaskCreatePinnedToCore(captureTaskEntry, "gemini_capture", 6144, this, 5, &_capture_task, 1);
        if (result != pdPASS) {
            _running = false;
            emitStatus(GeminiLiveStatus::Error, "Could not start capture task");
            return false;
        }

        result = xTaskCreatePinnedToCore(playbackTaskEntry, "gemini_playback", 4096, this, 6, &_playback_task, 1);
        if (result != pdPASS) {
            _running = false;
            vTaskDelete(_capture_task);
            _capture_task = nullptr;
            emitStatus(GeminiLiveStatus::Error, "Could not start playback task");
            return false;
        }

        emitStatus(GeminiLiveStatus::Ready, "Tap the face to talk");
        return true;
    }

    bool startTurn()
    {
        if (_streaming || _start_pending || _client) {
            return false;
        }

        _start_pending = true;
        _setup_started_at = GetHAL().millis();
        _reset_audio_pipeline = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
        emitStatus(GeminiLiveStatus::Connecting, "Connecting to Gemini...");
        if (!connectSession()) {
            _start_pending = false;
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
            return false;
        }
        return true;
    }

    void stopTurn()
    {
        if (_start_pending.exchange(false)) {
            closeSession();
            return;
        }

        if (!_streaming.exchange(false)) {
            return;
        }

        {
            sendText("{\"realtimeInput\":{\"audioStreamEnd\":true}}");
        }
        closeSession();
    }

    void stop()
    {
        if (_stopping.exchange(true)) {
            return;
        }

        _streaming = false;
        _running   = false;

        const uint32_t started_at = GetHAL().millis();
        while ((_capture_task || _playback_task) && GetHAL().millis() - started_at < kTaskStopTimeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (_capture_task) {
            vTaskDelete(_capture_task);
            _capture_task = nullptr;
        }
        if (_playback_task) {
            vTaskDelete(_playback_task);
            _playback_task = nullptr;
        }

        destroyClient();
        _connected = false;
        destroyAec();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }

private:
    bool initializeAec()
    {
        esp_ae_rate_cvt_cfg_t resampler_config = {
            .src_rate        = AUDIO_INPUT_SAMPLE_RATE,
            .dest_rate       = kAecSampleRate,
            .channel         = static_cast<uint8_t>(_codec->input_channels()),
            .bits_per_sample = ESP_AE_BIT16,
            .complexity      = 2,
            .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
        };
        if (esp_ae_rate_cvt_open(&resampler_config, &_input_resampler) != ESP_AE_ERR_OK || !_input_resampler) {
            return false;
        }

        aec_config_t aec_config = {
            .mic_num       = 1,
            .ref_num       = 1,
            .out_num       = 1,
            .filter_length = 4,
            .sample_rate   = kAecSampleRate,
            .caps          = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            .mode          = AEC_MODE_FD_LOW_COST,
            .nlp_level     = AEC_NLP_LEVEL_AGGR,
        };
        _aec = aec_create_from_config(&aec_config);
        if (!_aec) {
            destroyAec();
            return false;
        }

        _aec_chunk_frames = static_cast<size_t>(aec_get_chunksize(_aec));
        if (_aec_chunk_frames == 0 ||
            esp_ae_rate_cvt_get_max_out_sample_num(_input_resampler, kCaptureFrames, &_resampled_capacity) !=
                ESP_AE_ERR_OK ||
            _resampled_capacity == 0) {
            destroyAec();
            return false;
        }
        const size_t bytes = _aec_chunk_frames * sizeof(int16_t);
        _aec_mic           = static_cast<int16_t*>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT));
        _aec_reference     = static_cast<int16_t*>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT));
        _aec_output        = static_cast<int16_t*>(heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT));
        if (!_aec_mic || !_aec_reference || !_aec_output) {
            destroyAec();
            return false;
        }

        mclog::tagInfo(kTag, "AEC initialized: mode=FD_LOW_COST, rate={}, chunk={}", kAecSampleRate,
                       _aec_chunk_frames);
        return true;
    }

    void destroyAec()
    {
        if (_input_resampler) {
            esp_ae_rate_cvt_close(_input_resampler);
            _input_resampler = nullptr;
        }
        if (_aec) {
            aec_destroy(_aec);
            _aec = nullptr;
        }
        if (_aec_mic) {
            heap_caps_free(_aec_mic);
            _aec_mic = nullptr;
        }
        if (_aec_reference) {
            heap_caps_free(_aec_reference);
            _aec_reference = nullptr;
        }
        if (_aec_output) {
            heap_caps_free(_aec_output);
            _aec_output = nullptr;
        }
        _aec_input.clear();
        _aec_chunk_frames = 0;
        _resampled_capacity = 0;
    }

    bool connectSession()
    {
        _url = std::string(kGeminiEndpoint) + STACKCHAN_GEMINI_API_KEY;
        esp_websocket_client_config_t config{};
        config.uri                    = _url.c_str();
        config.crt_bundle_attach      = esp_crt_bundle_attach;
        config.disable_auto_reconnect = true;
        config.network_timeout_ms     = 10000;
        config.ping_interval_sec      = 20;
        config.buffer_size            = 4096;
        config.task_stack             = 6144;
        config.task_prio              = 4;
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

    bool sendText(std::string_view message)
    {
        std::lock_guard<std::mutex> lock(_protocol_mutex);
        if (!_client || !esp_websocket_client_is_connected(_client)) {
            return false;
        }
        return esp_websocket_client_send_text(_client, message.data(), message.size(), pdMS_TO_TICKS(kSendTimeoutMs)) ==
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
        if (_stopping || !_running) {
            return;
        }

        _resetting     = true;
        _streaming     = false;
        _speaking      = false;
        _connected     = false;
        _setup_send_pending = false;
        _turn_complete = false;
        clearPlayback();
        destroyClient();

        _resetting = false;
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        emitStatus(GeminiLiveStatus::Ready, "Tap the face to talk");
    }

    static void captureTaskEntry(void* context)
    {
        static_cast<GeminiLiveClient*>(context)->captureTask();
    }

    static void playbackTaskEntry(void* context)
    {
        static_cast<GeminiLiveClient*>(context)->playbackTask();
    }

    void captureTask()
    {
        const size_t input_channels = std::max(_codec->input_channels(), 1);
        std::vector<int16_t> input(kCaptureFrames * input_channels);
        std::vector<int16_t> resampled(_resampled_capacity * input_channels);

        while (_running) {
            if (_transport_failed.exchange(false)) {
                _start_pending = false;
                _streaming     = false;
                _speaking      = false;
                clearPlayback();
                destroyClient();
                esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
                emitStatus(GeminiLiveStatus::Error, "Gemini connection failed");
                continue;
            }

            if (_start_pending && GetHAL().millis() - _setup_started_at > kSetupTimeoutMs) {
                _start_pending = false;
                closeSession();
                emitStatus(GeminiLiveStatus::Error, "Gemini setup timed out");
                continue;
            }

            if (_setup_send_pending && GetHAL().millis() - _connected_at >= 100) {
                _setup_send_pending = false;
                if (sendText(kSetupMessage)) {
                    mclog::tagInfo(kTag, "Gemini setup sent");
                } else {
                    _transport_failed = true;
                }
                continue;
            }

            if (_streaming && _connected) {
                if (_reset_audio_pipeline.exchange(false)) {
                    esp_ae_rate_cvt_reset(_input_resampler);
                    _aec_input.clear();
                }
                if (!_codec->input_enabled()) {
                    _codec->EnableInput(true);
                }

                if (_codec->InputData(input)) {
                    uint32_t resampled_frames = _resampled_capacity;
                    if (esp_ae_rate_cvt_process(_input_resampler, input.data(), kCaptureFrames, resampled.data(),
                                                &resampled_frames) == ESP_AE_ERR_OK) {
                        _aec_input.insert(_aec_input.end(), resampled.begin(),
                                          resampled.begin() + resampled_frames * input_channels);
                    }

                    const size_t aec_input_samples = _aec_chunk_frames * input_channels;
                    while (_streaming && _connected && _aec_input.size() >= aec_input_samples) {
                        for (size_t frame = 0; frame < _aec_chunk_frames; ++frame) {
                            _aec_mic[frame]       = _aec_input[frame * input_channels];
                            _aec_reference[frame] = _aec_input[frame * input_channels + 1];
                        }
                        aec_process(_aec, _aec_mic, _aec_reference, _aec_output);
                        sendAudio(_speaking ? _aec_output : _aec_mic, _aec_chunk_frames, kAecSampleRate);
                        _aec_input.erase(_aec_input.begin(), _aec_input.begin() + aec_input_samples);
                    }
                }
                continue;
            }

            if (_codec->input_enabled()) {
                _codec->EnableInput(false);
            }

            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (_codec->input_enabled()) {
            _codec->EnableInput(false);
        }
        _capture_task = nullptr;
        vTaskDelete(nullptr);
    }

    void playbackTask()
    {
        while (_running) {
            std::vector<uint8_t> bytes;
            {
                std::lock_guard<std::mutex> lock(_playback_mutex);
                if (!_playback.empty()) {
                    bytes = std::move(_playback.front());
                    _playback.pop_front();
                }
            }

            if (!bytes.empty()) {
                if (!_codec->output_enabled()) {
                    _codec->EnableOutput(true);
                }
                bytes.resize(bytes.size() - (bytes.size() % sizeof(int16_t)));
                std::vector<int16_t> samples(bytes.size() / sizeof(int16_t));
                std::memcpy(samples.data(), bytes.data(), bytes.size());
                _codec->OutputData(samples);
                continue;
            }

            if (_turn_complete.exchange(false)) {
                _speaking = false;
                if (_codec->output_enabled()) {
                    _codec->EnableOutput(false);
                }
                if (_streaming) {
                    emitStatus(GeminiLiveStatus::Listening, "Full duplex active - tap the face to end");
                }
                continue;
            }

            if (_codec->output_enabled() && !_speaking) {
                _codec->EnableOutput(false);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (_codec->output_enabled()) {
            _codec->EnableOutput(false);
        }
        _playback_task = nullptr;
        vTaskDelete(nullptr);
    }

    void handleMessage(const char* data, size_t length)
    {
        ArduinoJson::JsonDocument document;
        if (ArduinoJson::deserializeJson(document, data, length)) {
            emitStatus(GeminiLiveStatus::Error, "Invalid Gemini response");
            return;
        }

        if (!document["setupComplete"].isNull()) {
            mclog::tagInfo(kTag, "Gemini setup complete");
            if (_start_pending.exchange(false)) {
                _streaming     = true;
                _turn_complete = false;
                emitStatus(GeminiLiveStatus::Listening, "Full duplex active - tap the face to end");
            }
            return;
        }

        auto server_content = document["serverContent"];
        if (server_content.isNull()) {
            return;
        }

        auto parts = server_content["modelTurn"]["parts"];
        if (parts.is<ArduinoJson::JsonArray>()) {
            for (auto part : parts.as<ArduinoJson::JsonArray>()) {
                const char* mime_type = part["inlineData"]["mimeType"] | "";
                const char* encoded   = part["inlineData"]["data"] | "";
                if (std::strncmp(mime_type, "audio/pcm", 9) == 0 && encoded[0] != '\0') {
                    queueAudio(encoded);
                    if (!_speaking.exchange(true)) {
                        emitStatus(GeminiLiveStatus::Speaking, "");
                    }
                }
            }
        }

        const bool interrupted = server_content["interrupted"] | false;
        if (interrupted) {
            clearPlayback();
            _speaking      = false;
            _turn_complete = false;
            if (_streaming) {
                emitStatus(GeminiLiveStatus::Listening, "Full duplex active - tap the face to end");
            }
        }

        if (!interrupted && (server_content["turnComplete"] | false)) {
            _turn_complete = true;
        }
    }

    void sendAudio(const int16_t* samples, size_t sample_count, uint32_t sample_rate)
    {
        const auto* bytes             = reinterpret_cast<const unsigned char*>(samples);
        const size_t byte_count       = sample_count * sizeof(int16_t);
        const size_t encoded_capacity = 4 * ((byte_count + 2) / 3) + 1;
        std::string encoded(encoded_capacity, '\0');
        size_t encoded_size = 0;
        if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()), encoded_capacity, &encoded_size,
                                  bytes, byte_count) != 0) {
            return;
        }
        encoded.resize(encoded_size);
        sendText(fmt::format("{{\"realtimeInput\":{{\"audio\":{{\"data\":\"{}\","
                             "\"mimeType\":\"audio/pcm;rate={}\"}}}}}}",
                             encoded, sample_rate));
    }

    void queueAudio(const char* encoded)
    {
        if (!_streaming) {
            return;
        }

        const size_t encoded_size = std::strlen(encoded);
        size_t decoded_capacity   = 0;
        mbedtls_base64_decode(nullptr, 0, &decoded_capacity, reinterpret_cast<const unsigned char*>(encoded),
                              encoded_size);
        if (decoded_capacity == 0) {
            return;
        }

        std::vector<uint8_t> decoded(decoded_capacity);
        size_t decoded_size = 0;
        if (mbedtls_base64_decode(decoded.data(), decoded.size(), &decoded_size,
                                  reinterpret_cast<const unsigned char*>(encoded), encoded_size) != 0) {
            return;
        }
        decoded.resize(decoded_size);

        std::lock_guard<std::mutex> lock(_playback_mutex);
        if (_playback.size() >= kMaxPlaybackChunks) {
            _playback.pop_front();
        }
        _playback.push_back(std::move(decoded));
    }

    void clearPlayback()
    {
        std::lock_guard<std::mutex> lock(_playback_mutex);
        _playback.clear();
    }

    void emitStatus(GeminiLiveStatus status, std::string message)
    {
        GetHAL().onGeminiLiveStatus.emit(status, message);
    }

    AudioCodec* _codec = nullptr;
    aec_handle_t* _aec = nullptr;
    esp_ae_rate_cvt_handle_t _input_resampler = nullptr;
    int16_t* _aec_mic = nullptr;
    int16_t* _aec_reference = nullptr;
    int16_t* _aec_output = nullptr;
    size_t _aec_chunk_frames = 0;
    uint32_t _resampled_capacity = 0;
    std::vector<int16_t> _aec_input;
    esp_websocket_client_handle_t _client = nullptr;
    std::string _url;
    std::string _incoming_message;
    TaskHandle_t _capture_task       = nullptr;
    TaskHandle_t _playback_task      = nullptr;
    std::atomic<bool> _running       = false;
    std::atomic<bool> _stopping      = false;
    std::atomic<bool> _resetting     = false;
    std::atomic<bool> _connected     = false;
    std::atomic<bool> _setup_send_pending = false;
    std::atomic<bool> _start_pending = false;
    std::atomic<bool> _streaming     = false;
    std::atomic<bool> _speaking      = false;
    std::atomic<bool> _turn_complete = false;
    std::atomic<bool> _transport_failed = false;
    std::atomic<bool> _reset_audio_pipeline = false;
    std::atomic<uint32_t> _setup_started_at = 0;
    std::atomic<uint32_t> _connected_at = 0;
    std::mutex _protocol_mutex;
    std::mutex _playback_mutex;
    std::deque<std::vector<uint8_t>> _playback;
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

void Hal::stopGeminiLiveService()
{
    gemini_live_client.reset();
}
