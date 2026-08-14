#ifndef AI_SERVICE_H_
#define AI_SERVICE_H_

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"

namespace recording_service {
class RecordedClip;
}

namespace ai_service {

// The provider-neutral AI layer picks exactly one of these as the "active" backend. Both
// providers stay fully initialized underneath so switching is instant and readiness for the
// inactive provider is still tracked for the portal UI.
enum class Provider : uint8_t {
    kGemini = 0,
    kLocalAi,
};

struct SettingsSnapshot {
    Provider provider = Provider::kGemini;
    Provider default_provider = Provider::kGemini;
    bool has_stored_provider = false;
};

struct RuntimeSnapshot {
    bool ready = false;
    std::string status_message;
};

struct Snapshot {
    SettingsSnapshot settings = {};
    RuntimeSnapshot runtime = {};
};

struct Event {
    Snapshot snapshot = {};
};

struct SettingsPatch {
    bool has_provider = false;
    std::string provider;  // "gemini" | "localai"
};

struct Result {
    bool success = false;
    bool validation_error = false;
    int status_code = 500;
    std::string field;
    std::string error_code;
    std::string message;
};

// Provider-neutral mirrors of the per-provider result shapes so callers never need to depend on
// gemini_service/localai_service headers directly.
struct TextResult {
    bool success = false;
    int http_status = 0;
    std::string text = {};
    std::string error_code = {};
    std::string error_message = {};
};

struct TokenCountResult {
    bool success = false;
    int http_status = 0;
    int total_tokens = 0;
    std::string error_code = {};
    std::string error_message = {};
};

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

Provider GetActiveProvider();
const char* ProviderName(Provider provider);
bool ParseProviderName(const std::string& name, Provider* out);
bool IsReady();
bool IsConfigured();

// Dispatches to whichever provider is currently active.
TextResult GenerateText(const std::string& prompt);
TokenCountResult CountTokens(const std::string& prompt);
TranscriptionResult Transcribe(const recording_service::RecordedClip& clip);

// Pinned-provider overloads: dispatch to `provider` explicitly instead of re-reading whatever is
// currently active. Callers that make several of these calls over the lifetime of one logical
// request (e.g. summary_service's chunked map-reduce/rollup) must read GetActiveProvider() once,
// then pass that pinned value to every call for the duration of the request. This keeps the
// request on a single backend even if the active provider is switched concurrently mid-job.
TextResult GenerateText(Provider provider, const std::string& prompt);
TokenCountResult CountTokens(Provider provider, const std::string& prompt);

// Caps `default_budget` (assumed sized for Gemini's large context window) down to LocalAI's
// configurable context limit when LocalAI is the active provider; returns `default_budget`
// unchanged when Gemini is active.
size_t GetContextTokenBudget(size_t default_budget);
// Pinned-provider overload of the above; see the pinning note on GenerateText/CountTokens.
size_t GetContextTokenBudget(Provider provider, size_t default_budget);

void SetNetworkState(bool connected, bool access_point_mode);
// Registers this facade's own provider-selection routes plus forwards registration to both
// gemini_service and localai_service so app_shell only has to call this one function.
void RegisterPortalRoutes(httpd_handle_t server);

}  // namespace ai_service

#endif  // AI_SERVICE_H_
