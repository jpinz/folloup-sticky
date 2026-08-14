#ifndef LOCALAI_SERVICE_H_
#define LOCALAI_SERVICE_H_

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"

namespace recording_service {
class RecordedClip;
}

namespace localai_service {

enum class ApiKeySource : uint8_t {
    kNone = 0,
    kSdkConfig,
    kNvs,
};

struct SettingsSnapshot {
    bool configured = false;
    std::string base_url;
    std::string text_model;
    std::string transcription_model;
    bool has_stored_api_key = false;
    bool has_sdkconfig_api_key = false;
    ApiKeySource api_key_source = ApiKeySource::kNone;
    std::string api_key_last4;
    int context_limit = 0;
};

struct RuntimeSnapshot {
    bool initialized = false;
    bool ready = false;
    bool request_in_flight = false;
    bool readiness_checked = false;
    bool reachable = false;
    int last_http_status = 0;
    std::string last_status_message;
    std::string last_error_code;
    std::string last_error_message;
    int model_count = 0;
};

struct Snapshot {
    SettingsSnapshot settings = {};
    RuntimeSnapshot runtime = {};
};

struct Event {
    Snapshot snapshot = {};
};

struct SettingsPatch {
    bool has_base_url = false;
    std::string base_url;
    bool has_text_model = false;
    std::string text_model;
    bool has_transcription_model = false;
    std::string transcription_model;
    bool has_api_key = false;
    std::string api_key;
    bool has_context_limit = false;
    int context_limit = 0;
};

struct Result {
    bool success = false;
    bool validation_error = false;
    int status_code = 500;
    std::string field;
    std::string error_code;
    std::string message;
};

// Result of a synchronous OpenAI-compatible chat-completions call.
struct TextResult {
    bool success = false;
    int http_status = 0;
    std::string text = {};
    std::string error_code = {};
    std::string error_message = {};
};

// LocalAI backends don't expose a universal standalone token-count endpoint, so this always
// reports failure; callers fall back to a character-based estimate.
struct TokenCountResult {
    bool success = false;
    int http_status = 0;
    int total_tokens = 0;
    std::string error_code = {};
    std::string error_message = {};
};

// Result of a synchronous audio transcription (streamed multipart upload) call.
struct TranscriptionResult {
    bool success = false;
    int http_status = 0;
    std::string transcript = {};
    std::string error_code = {};
    std::string error_message = {};
    uint32_t clip_duration_ms = 0;
    size_t wav_bytes = 0;
    uint64_t upload_elapsed_ms = 0;
    uint64_t total_elapsed_ms = 0;
};

using EventHandler = void (*)(const Event& event, void* context);

esp_err_t Init();
void SetEventHandler(EventHandler handler, void* context);
Snapshot GetSnapshot();

Result ApplySettingsPatch(const SettingsPatch& patch);
Result ResetSettings();

bool IsConfigured();
std::string GetEffectiveBaseUrl();
std::string GetEffectiveTextModel();
std::string GetEffectiveTranscriptionModel();
std::string GetEffectiveApiKey();
int GetEffectiveContextLimit();

// Synchronous LocalAI calls (block on HTTP; run them from a worker task, never a UI/input
// task). They use the effective base URL/models/key and return the parsed result or an error.
TextResult GenerateText(const std::string& prompt);
TokenCountResult CountTokens(const std::string& prompt);
TranscriptionResult Transcribe(const recording_service::RecordedClip& clip);

bool BeginReadinessCheck();
void SetNetworkState(bool connected, bool access_point_mode);
void RegisterPortalRoutes(httpd_handle_t server);

const char* ApiKeySourceName(ApiKeySource source);

}  // namespace localai_service

#endif  // LOCALAI_SERVICE_H_
