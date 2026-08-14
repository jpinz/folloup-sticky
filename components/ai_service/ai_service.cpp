#include "ai_service.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <utility>

#include "cJSON.h"
#include "esp_log.h"
#include "gemini_service.h"
#include "localai_service.h"
#include "nvs.h"
#include "recording_service.h"
#include "sdkconfig.h"

namespace ai_service {
namespace {

constexpr const char* kTag = "AiService";
constexpr const char* kStorageNamespace = "ai_provider";
constexpr const char* kStorageProviderKey = "provider";
constexpr const char* kProviderNameGemini = "gemini";
constexpr const char* kProviderNameLocalAi = "localai";
constexpr const char* kPortalApiSettingsUri = "/api/settings/ai";
constexpr const char* kPortalApiSettingsResetUri = "/api/settings/ai/reset";
constexpr const char* kPortalApiRuntimeUri = "/api/runtime/ai";
constexpr size_t kMaxPortalPayloadLen = 256;

std::mutex s_mutex;
EventHandler s_event_handler = nullptr;
void* s_event_context = nullptr;
bool s_initialized = false;
bool s_has_stored_provider = false;
Provider s_provider = Provider::kGemini;

std::string TrimCopy(std::string value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string ToLowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

Provider GetSdkConfigDefaultProvider()
{
#if defined(CONFIG_FOLLOWUP_AI_PROVIDER)
    Provider provider = Provider::kGemini;
    if (ParseProviderName(CONFIG_FOLLOWUP_AI_PROVIDER, &provider)) {
        return provider;
    }
#endif
    return Provider::kGemini;
}

std::string ReadNvsString(nvs_handle_t handle, const char* key)
{
    size_t size = 0;
    if (nvs_get_str(handle, key, nullptr, &size) != ESP_OK || size == 0) {
        return {};
    }

    std::string value(size, '\0');
    if (nvs_get_str(handle, key, value.data(), &size) != ESP_OK) {
        return {};
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

// Returns whether a stored provider preference exists, and if so, what it is.
bool LoadStoredProvider(Provider* out)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    const std::string stored = ReadNvsString(handle, kStorageProviderKey);
    nvs_close(handle);
    return !stored.empty() && out != nullptr && ParseProviderName(stored, out);
}

bool SaveStoredProvider(Provider provider)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to open AI provider NVS namespace for write: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(handle, kStorageProviderKey, ProviderName(provider));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to save AI provider preference: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ClearStoredProviderFromNvs()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to open AI provider NVS namespace for clear: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_erase_key(handle, kStorageProviderKey);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to clear AI provider preference: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool GeminiReadyLocked()
{
    return gemini_service::GetSnapshot().runtime.ready;
}

bool LocalAiReadyLocked()
{
    return localai_service::GetSnapshot().runtime.ready;
}

std::string ActiveStatusMessageLocked(Provider provider)
{
    if (provider == Provider::kGemini) {
        return gemini_service::GetSnapshot().runtime.last_status_message;
    }
    return localai_service::GetSnapshot().runtime.last_status_message;
}

Snapshot BuildSnapshotLocked()
{
    Snapshot snapshot = {};
    snapshot.settings.provider = s_provider;
    snapshot.settings.default_provider = GetSdkConfigDefaultProvider();
    snapshot.settings.has_stored_provider = s_has_stored_provider;

    snapshot.runtime.ready =
        s_provider == Provider::kGemini ? GeminiReadyLocked() : LocalAiReadyLocked();
    snapshot.runtime.status_message = ActiveStatusMessageLocked(s_provider);
    return snapshot;
}

void Notify()
{
    EventHandler handler = nullptr;
    void* context = nullptr;
    Event event = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        handler = s_event_handler;
        context = s_event_context;
        event.snapshot = BuildSnapshotLocked();
    }

    if (handler != nullptr) {
        handler(event, context);
    }
}

// Either underlying provider's own readiness/settings changing can affect the neutral snapshot
// (e.g. the currently-active provider just finished authenticating), so both forward here.
void OnGeminiEvent(const gemini_service::Event&, void*)
{
    Notify();
}

void OnLocalAiEvent(const localai_service::Event&, void*)
{
    Notify();
}

std::string ReadRequestBody(httpd_req_t* request)
{
    if (request == nullptr || request->content_len <= 0) {
        return {};
    }

    std::string body(static_cast<size_t>(request->content_len), '\0');
    size_t offset = 0;
    while (offset < body.size()) {
        const int received = httpd_req_recv(request, body.data() + offset, body.size() - offset);
        if (received <= 0) {
            return {};
        }
        offset += static_cast<size_t>(received);
    }
    return body;
}

std::string JsonString(cJSON* root)
{
    if (root == nullptr) {
        return "{}";
    }

    char* raw = cJSON_PrintUnformatted(root);
    if (raw == nullptr) {
        return "{}";
    }
    std::string json(raw);
    cJSON_free(raw);
    return json;
}

esp_err_t SendJsonResponse(httpd_req_t* request, int status_code, cJSON* root)
{
    if (request == nullptr) {
        if (root != nullptr) {
            cJSON_Delete(root);
        }
        return ESP_FAIL;
    }

    const std::string payload = JsonString(root);
    if (root != nullptr) {
        cJSON_Delete(root);
    }

    switch (status_code) {
        case 200:
            httpd_resp_set_status(request, HTTPD_200);
            break;
        case 400:
            httpd_resp_set_status(request, HTTPD_400);
            break;
        case 500:
        default:
            httpd_resp_set_status(request, HTTPD_500);
            break;
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_send(request, payload.c_str(), payload.size());
}

void AppendSnapshot(cJSON* root, const Snapshot& snapshot, const char* message)
{
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "message", message != nullptr ? message : "");

    cJSON* settings = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddStringToObject(settings, "provider", ProviderName(snapshot.settings.provider));
    cJSON_AddStringToObject(settings, "default_provider",
                            ProviderName(snapshot.settings.default_provider));
    cJSON_AddBoolToObject(settings, "has_stored_provider", snapshot.settings.has_stored_provider);

    cJSON* runtime = cJSON_AddObjectToObject(root, "runtime");
    cJSON_AddBoolToObject(runtime, "ready", snapshot.runtime.ready);
    cJSON_AddStringToObject(runtime, "provider", ProviderName(snapshot.settings.provider));
    cJSON_AddStringToObject(runtime, "status_message", snapshot.runtime.status_message.c_str());
}

bool ParsePatchBody(const std::string& body, SettingsPatch* patch, std::string* error)
{
    if (patch == nullptr) {
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(body.c_str(), body.size());
    if (root == nullptr) {
        if (error != nullptr) {
            *error = "Invalid JSON body";
        }
        return false;
    }

    cJSON* provider = cJSON_GetObjectItemCaseSensitive(root, "provider");
    if (cJSON_IsString(provider) && provider->valuestring != nullptr) {
        patch->has_provider = true;
        patch->provider = provider->valuestring;
    } else if (provider != nullptr && !cJSON_IsNull(provider)) {
        if (error != nullptr) {
            *error = "Invalid provider";
        }
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

esp_err_t RegisterPortalRoute(httpd_handle_t server, const httpd_uri_t* handler)
{
    if (server == nullptr || handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = httpd_register_uri_handler(server, handler);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register AI portal route %s [%d]: %s",
                 handler->uri != nullptr ? handler->uri : "<null>",
                 static_cast<int>(handler->method), esp_err_to_name(err));
    }
    return err;
}

esp_err_t HandlePortalSettingsGet(httpd_req_t* request)
{
    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "AI provider settings loaded");
    return SendJsonResponse(request, 200, root);
}

esp_err_t HandlePortalSettingsPatch(httpd_req_t* request)
{
    if (request == nullptr || request->content_len <= 0 ||
        request->content_len > static_cast<int>(kMaxPortalPayloadLen)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", "Invalid AI provider settings payload");
        return SendJsonResponse(request, 400, root);
    }

    const std::string body = ReadRequestBody(request);
    SettingsPatch patch = {};
    std::string parse_error;
    if (body.empty() || !ParsePatchBody(body, &patch, &parse_error)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", parse_error.empty()
                                                     ? "Invalid AI provider settings payload"
                                                     : parse_error.c_str());
        return SendJsonResponse(request, 400, root);
    }

    const Result result = ApplySettingsPatch(patch);
    if (!result.success) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", result.message.c_str());
        cJSON_AddStringToObject(root, "error_code", result.error_code.c_str());
        cJSON_AddStringToObject(root, "field", result.field.c_str());
        return SendJsonResponse(request, result.status_code, root);
    }

    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "AI provider updated");
    return SendJsonResponse(request, 200, root);
}

esp_err_t HandlePortalSettingsReset(httpd_req_t* request)
{
    const Result result = ResetSettings();
    if (!result.success) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", result.message.c_str());
        cJSON_AddStringToObject(root, "error_code", result.error_code.c_str());
        cJSON_AddStringToObject(root, "field", result.field.c_str());
        return SendJsonResponse(request, result.status_code, root);
    }

    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "AI provider reset");
    return SendJsonResponse(request, 200, root);
}

esp_err_t HandlePortalRuntimeGet(httpd_req_t* request)
{
    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "AI runtime loaded");
    return SendJsonResponse(request, 200, root);
}

}  // namespace

esp_err_t Init()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_initialized) {
            return ESP_OK;
        }

        Provider stored = Provider::kGemini;
        s_has_stored_provider = LoadStoredProvider(&stored);
        s_provider = s_has_stored_provider ? stored : GetSdkConfigDefaultProvider();
        s_initialized = true;
    }

    // Both providers stay fully initialized regardless of which one is active, so readiness for
    // the inactive provider is still tracked (and switching providers is instant).
    gemini_service::SetEventHandler(OnGeminiEvent, nullptr);
    localai_service::SetEventHandler(OnLocalAiEvent, nullptr);
    (void)gemini_service::Init();
    (void)localai_service::Init();

    ESP_LOGI(kTag, "AI provider service initialized: provider=%s stored=%d",
             ProviderName(GetActiveProvider()), s_has_stored_provider ? 1 : 0);
    Notify();
    return ESP_OK;
}

void SetEventHandler(EventHandler handler, void* context)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_event_handler = handler;
    s_event_context = context;
}

Snapshot GetSnapshot()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return BuildSnapshotLocked();
}

Result ApplySettingsPatch(const SettingsPatch& patch)
{
    if (!patch.has_provider) {
        return {
            .success = false,
            .validation_error = true,
            .status_code = 400,
            .field = "provider",
            .error_code = "missing_provider",
            .message = "AI provider is required",
        };
    }

    Provider provider = Provider::kGemini;
    if (!ParseProviderName(patch.provider, &provider)) {
        return {
            .success = false,
            .validation_error = true,
            .status_code = 400,
            .field = "provider",
            .error_code = "invalid_provider",
            .message = "AI provider must be \"gemini\" or \"localai\"",
        };
    }

    bool save_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!SaveStoredProvider(provider)) {
            save_failed = true;
        } else {
            s_provider = provider;
            s_has_stored_provider = true;
        }
    }

    if (save_failed) {
        return {
            .success = false,
            .validation_error = false,
            .status_code = 500,
            .field = "provider",
            .error_code = "nvs_write_failed",
            .message = "Failed to store AI provider preference",
        };
    }

    Notify();
    return {
        .success = true,
        .validation_error = false,
        .status_code = 200,
        .field = {},
        .error_code = {},
        .message = "AI provider updated",
    };
}

Result ResetSettings()
{
    bool clear_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!ClearStoredProviderFromNvs()) {
            clear_failed = true;
        } else {
            s_has_stored_provider = false;
            s_provider = GetSdkConfigDefaultProvider();
        }
    }

    if (clear_failed) {
        return {
            .success = false,
            .validation_error = false,
            .status_code = 500,
            .field = "provider",
            .error_code = "nvs_clear_failed",
            .message = "Failed to reset AI provider preference",
        };
    }

    Notify();
    return {
        .success = true,
        .validation_error = false,
        .status_code = 200,
        .field = {},
        .error_code = {},
        .message = "AI provider reset",
    };
}

Provider GetActiveProvider()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_provider;
}

const char* ProviderName(Provider provider)
{
    switch (provider) {
        case Provider::kLocalAi:
            return kProviderNameLocalAi;
        case Provider::kGemini:
        default:
            return kProviderNameGemini;
    }
}

bool ParseProviderName(const std::string& name, Provider* out)
{
    const std::string normalized = ToLowerCopy(TrimCopy(name));
    if (normalized == kProviderNameGemini) {
        if (out != nullptr) {
            *out = Provider::kGemini;
        }
        return true;
    }
    if (normalized == kProviderNameLocalAi) {
        if (out != nullptr) {
            *out = Provider::kLocalAi;
        }
        return true;
    }
    return false;
}

bool IsReady()
{
    return GetSnapshot().runtime.ready;
}

bool IsConfigured()
{
    const Provider provider = GetActiveProvider();
    return provider == Provider::kGemini ? gemini_service::HasApiKey()
                                         : localai_service::IsConfigured();
}

TextResult GenerateText(Provider provider, const std::string& prompt)
{
    TextResult result = {};
    if (provider == Provider::kGemini) {
        const gemini_service::TextResult provider_result = gemini_service::GenerateText(prompt);
        result.success = provider_result.success;
        result.http_status = provider_result.http_status;
        result.text = provider_result.text;
        result.error_code = provider_result.error_code;
        result.error_message = provider_result.error_message;
    } else {
        const localai_service::TextResult provider_result = localai_service::GenerateText(prompt);
        result.success = provider_result.success;
        result.http_status = provider_result.http_status;
        result.text = provider_result.text;
        result.error_code = provider_result.error_code;
        result.error_message = provider_result.error_message;
    }
    return result;
}

TextResult GenerateText(const std::string& prompt)
{
    return GenerateText(GetActiveProvider(), prompt);
}

TokenCountResult CountTokens(Provider provider, const std::string& prompt)
{
    TokenCountResult result = {};
    if (provider == Provider::kGemini) {
        const gemini_service::TokenCountResult provider_result = gemini_service::CountTokens(prompt);
        result.success = provider_result.success;
        result.http_status = provider_result.http_status;
        result.total_tokens = provider_result.total_tokens;
        result.error_code = provider_result.error_code;
        result.error_message = provider_result.error_message;
    } else {
        const localai_service::TokenCountResult provider_result =
            localai_service::CountTokens(prompt);
        result.success = provider_result.success;
        result.http_status = provider_result.http_status;
        result.total_tokens = provider_result.total_tokens;
        result.error_code = provider_result.error_code;
        result.error_message = provider_result.error_message;
    }
    return result;
}

TokenCountResult CountTokens(const std::string& prompt)
{
    return CountTokens(GetActiveProvider(), prompt);
}

TranscriptionResult Transcribe(const recording_service::RecordedClip& clip)
{
    TranscriptionResult result = {};
    if (GetActiveProvider() == Provider::kGemini) {
        const gemini_service::TranscriptionResult provider_result = gemini_service::Transcribe(clip);
        result.success = provider_result.success;
        result.http_status = provider_result.http_status;
        result.transcript = provider_result.transcript;
        result.error_code = provider_result.error_code;
        result.error_message = provider_result.error_message;
        result.clip_duration_ms = provider_result.clip_duration_ms;
        result.wav_bytes = provider_result.wav_bytes;
        result.upload_elapsed_ms = provider_result.upload_elapsed_ms;
        result.total_elapsed_ms = provider_result.total_elapsed_ms;
    } else {
        const localai_service::TranscriptionResult provider_result =
            localai_service::Transcribe(clip);
        result.success = provider_result.success;
        result.http_status = provider_result.http_status;
        result.transcript = provider_result.transcript;
        result.error_code = provider_result.error_code;
        result.error_message = provider_result.error_message;
        result.clip_duration_ms = provider_result.clip_duration_ms;
        result.wav_bytes = provider_result.wav_bytes;
        result.upload_elapsed_ms = provider_result.upload_elapsed_ms;
        result.total_elapsed_ms = provider_result.total_elapsed_ms;
    }
    return result;
}

size_t GetContextTokenBudget(Provider provider, size_t default_budget)
{
    if (provider != Provider::kLocalAi) {
        return default_budget;
    }
    const int context_limit = localai_service::GetEffectiveContextLimit();
    if (context_limit <= 0) {
        return default_budget;
    }
    return std::min(default_budget, static_cast<size_t>(context_limit));
}

size_t GetContextTokenBudget(size_t default_budget)
{
    return GetContextTokenBudget(GetActiveProvider(), default_budget);
}

void SetNetworkState(bool connected, bool access_point_mode)
{
    gemini_service::SetNetworkState(connected, access_point_mode);
    localai_service::SetNetworkState(connected, access_point_mode);
}

void RegisterPortalRoutes(httpd_handle_t server)
{
    if (server == nullptr) {
        return;
    }

    httpd_uri_t settings_get = {
        .uri = kPortalApiSettingsUri,
        .method = HTTP_GET,
        .handler = HandlePortalSettingsGet,
        .user_ctx = nullptr,
    };
    httpd_uri_t settings_patch = {
        .uri = kPortalApiSettingsUri,
        .method = HTTP_PATCH,
        .handler = HandlePortalSettingsPatch,
        .user_ctx = nullptr,
    };
    httpd_uri_t settings_reset = {
        .uri = kPortalApiSettingsResetUri,
        .method = HTTP_POST,
        .handler = HandlePortalSettingsReset,
        .user_ctx = nullptr,
    };
    httpd_uri_t runtime_get = {
        .uri = kPortalApiRuntimeUri,
        .method = HTTP_GET,
        .handler = HandlePortalRuntimeGet,
        .user_ctx = nullptr,
    };

    if (RegisterPortalRoute(server, &settings_get) != ESP_OK ||
        RegisterPortalRoute(server, &settings_patch) != ESP_OK ||
        RegisterPortalRoute(server, &settings_reset) != ESP_OK ||
        RegisterPortalRoute(server, &runtime_get) != ESP_OK) {
        ESP_LOGW(kTag, "AI provider portal routes are incomplete");
    }

    gemini_service::RegisterPortalRoutes(server);
    localai_service::RegisterPortalRoutes(server);
}

}  // namespace ai_service
