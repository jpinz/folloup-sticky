#include "localai_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <strings.h>
#include <utility>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "followup_task_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "recording_service.h"
#include "sdkconfig.h"

namespace localai_service {
namespace {

constexpr const char* kTag = "LocalAiService";
constexpr const char* kSettingsTag = "LocalAiSettings";
constexpr const char* kStorageNamespace = "localai";
// NVS key names are capped at 15 characters, so a few of these are abbreviated.
constexpr const char* kStorageBaseUrl = "base_url";
constexpr const char* kStorageTextModel = "text_model";
constexpr const char* kStorageTranscriptionModel = "tr_model";
constexpr const char* kStorageApiKey = "api_key";
constexpr const char* kStorageContextLimit = "ctx_limit";
constexpr const char* kPortalApiSettingsUri = "/api/settings/localai";
constexpr const char* kPortalApiSettingsResetUri = "/api/settings/localai/reset";
constexpr const char* kPortalApiRuntimeUri = "/api/runtime/localai";
constexpr size_t kMaxPortalPayloadLen = 512;
constexpr int kReadinessTimeoutMs = 8000;   // LAN request; expected to be fast
constexpr int kGenerateTimeoutMs = 60000;   // text generation can be slow on modest local hardware
constexpr int kTranscribeTimeoutMs = 60000;
constexpr uint32_t kWorkerTaskStackWords = 8192;
constexpr int kDefaultContextLimit = 8192;
constexpr int kMinContextLimit = 256;
constexpr int kMaxContextLimit = 1000000;
constexpr const char* kAudioMimeType = "audio/wav";
constexpr const char* kMultipartBoundary = "----FolloupLocalAiBoundary7MA4YWxkTrZu0gW";
constexpr size_t kHttpUploadChunkSamples = 2048;

struct ReadinessResult {
    bool success = false;
    int http_status = 0;
    int model_count = 0;
    std::string error_code;
    std::string error_message;
};

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::string error_code;
    std::string error_message;
};

struct ReadinessTaskContext {
    std::string base_url;
    std::string api_key;
    uint32_t generation = 0;
};

std::mutex s_mutex;
EventHandler s_event_handler = nullptr;
void* s_event_context = nullptr;
bool s_initialized = false;
bool s_network_connected = false;
bool s_access_point_mode = false;
bool s_request_in_flight = false;
bool s_readiness_checked = false;
bool s_reachable = false;
uint32_t s_readiness_generation = 0;
int s_last_http_status = 0;
int s_model_count = 0;
std::string s_stored_base_url;
std::string s_stored_text_model;
std::string s_stored_transcription_model;
std::string s_stored_api_key;
int s_stored_context_limit = 0;  // 0 means "not overridden"
std::string s_last_status_message;
std::string s_last_error_code;
std::string s_last_error_message;

std::string TrimCopy(std::string value)
{
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string TrimForLog(std::string value, size_t max_len = 96)
{
    value = TrimCopy(std::move(value));
    if (value.size() <= max_len) {
        return value;
    }
    if (max_len <= 3) {
        return value.substr(0, max_len);
    }
    return value.substr(0, max_len - 3) + "...";
}

std::string StripTrailingSlash(std::string value)
{
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

bool LooksLikeHttpUrl(const std::string& value)
{
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
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

esp_err_t WriteNvsStringOrErase(nvs_handle_t handle, const char* key, const std::string& value)
{
    if (value.empty()) {
        const esp_err_t err = nvs_erase_key(handle, key);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    return nvs_set_str(handle, key, value.c_str());
}

struct StoredSettings {
    std::string base_url;
    std::string text_model;
    std::string transcription_model;
    std::string api_key;
    int context_limit = 0;
};

StoredSettings LoadStoredSettings()
{
    StoredSettings settings = {};
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return settings;
    }
    if (err != ESP_OK) {
        ESP_LOGW(kSettingsTag, "Failed to open LocalAI NVS namespace: %s", esp_err_to_name(err));
        return settings;
    }

    settings.base_url = StripTrailingSlash(TrimCopy(ReadNvsString(handle, kStorageBaseUrl)));
    settings.text_model = TrimCopy(ReadNvsString(handle, kStorageTextModel));
    settings.transcription_model = TrimCopy(ReadNvsString(handle, kStorageTranscriptionModel));
    settings.api_key = TrimCopy(ReadNvsString(handle, kStorageApiKey));

    int32_t context_limit = 0;
    if (nvs_get_i32(handle, kStorageContextLimit, &context_limit) == ESP_OK) {
        settings.context_limit = static_cast<int>(context_limit);
    }
    nvs_close(handle);
    return settings;
}

bool SaveStoredSettingsLocked()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kSettingsTag, "Failed to open LocalAI NVS namespace for write: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = WriteNvsStringOrErase(handle, kStorageBaseUrl, s_stored_base_url);
    if (err == ESP_OK) {
        err = WriteNvsStringOrErase(handle, kStorageTextModel, s_stored_text_model);
    }
    if (err == ESP_OK) {
        err = WriteNvsStringOrErase(handle, kStorageTranscriptionModel, s_stored_transcription_model);
    }
    if (err == ESP_OK) {
        err = WriteNvsStringOrErase(handle, kStorageApiKey, s_stored_api_key);
    }
    if (err == ESP_OK) {
        if (s_stored_context_limit > 0) {
            err = nvs_set_i32(handle, kStorageContextLimit, s_stored_context_limit);
        } else {
            err = nvs_erase_key(handle, kStorageContextLimit);
            if (err == ESP_ERR_NVS_NOT_FOUND) {
                err = ESP_OK;
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(kSettingsTag, "Failed to save LocalAI settings: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool ClearStoredSettingsFromNvs()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kStorageNamespace, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(kSettingsTag, "Failed to open LocalAI NVS namespace for clear: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(kSettingsTag, "Failed to clear LocalAI settings: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

std::string GetSdkConfigBaseUrl()
{
#if defined(CONFIG_FOLLOWUP_LOCALAI_BASE_URL)
    return StripTrailingSlash(TrimCopy(CONFIG_FOLLOWUP_LOCALAI_BASE_URL));
#else
    return {};
#endif
}

std::string GetSdkConfigTextModel()
{
#if defined(CONFIG_FOLLOWUP_LOCALAI_TEXT_MODEL)
    return TrimCopy(CONFIG_FOLLOWUP_LOCALAI_TEXT_MODEL);
#else
    return {};
#endif
}

std::string GetSdkConfigTranscriptionModel()
{
#if defined(CONFIG_FOLLOWUP_LOCALAI_TRANSCRIPTION_MODEL)
    return TrimCopy(CONFIG_FOLLOWUP_LOCALAI_TRANSCRIPTION_MODEL);
#else
    return {};
#endif
}

std::string GetSdkConfigApiKey()
{
#if defined(CONFIG_FOLLOWUP_LOCALAI_API_KEY)
    return TrimCopy(CONFIG_FOLLOWUP_LOCALAI_API_KEY);
#else
    return {};
#endif
}

int GetSdkConfigContextLimit()
{
#if defined(CONFIG_FOLLOWUP_LOCALAI_CONTEXT_LIMIT)
    return CONFIG_FOLLOWUP_LOCALAI_CONTEXT_LIMIT;
#else
    return kDefaultContextLimit;
#endif
}

std::string GetEffectiveBaseUrlLocked()
{
    return !s_stored_base_url.empty() ? s_stored_base_url : GetSdkConfigBaseUrl();
}

std::string GetEffectiveTextModelLocked()
{
    return !s_stored_text_model.empty() ? s_stored_text_model : GetSdkConfigTextModel();
}

std::string GetEffectiveTranscriptionModelLocked()
{
    return !s_stored_transcription_model.empty() ? s_stored_transcription_model
                                                  : GetSdkConfigTranscriptionModel();
}

std::string GetEffectiveApiKeyLocked()
{
    return !s_stored_api_key.empty() ? s_stored_api_key : GetSdkConfigApiKey();
}

ApiKeySource GetApiKeySourceLocked()
{
    if (!s_stored_api_key.empty()) {
        return ApiKeySource::kNvs;
    }
    if (!GetSdkConfigApiKey().empty()) {
        return ApiKeySource::kSdkConfig;
    }
    return ApiKeySource::kNone;
}

std::string GetApiKeyLast4Locked()
{
    const std::string key = GetEffectiveApiKeyLocked();
    if (key.size() < 4) {
        return {};
    }
    return key.substr(key.size() - 4);
}

int GetEffectiveContextLimitLocked()
{
    if (s_stored_context_limit > 0) {
        return s_stored_context_limit;
    }
    const int sdkconfig_value = GetSdkConfigContextLimit();
    return sdkconfig_value > 0 ? sdkconfig_value : kDefaultContextLimit;
}

void SetLastErrorLocked(const char* error_code, const char* message)
{
    s_last_error_code = error_code != nullptr ? error_code : "";
    s_last_error_message = message != nullptr ? message : "";
}

void ClearLastErrorLocked()
{
    s_last_error_code.clear();
    s_last_error_message.clear();
}

bool IsConfiguredLocked()
{
    return !GetEffectiveBaseUrlLocked().empty() && !GetEffectiveTextModelLocked().empty();
}

Snapshot BuildSnapshotLocked()
{
    Snapshot snapshot = {};
    snapshot.settings.configured = IsConfiguredLocked();
    snapshot.settings.base_url = GetEffectiveBaseUrlLocked();
    snapshot.settings.text_model = GetEffectiveTextModelLocked();
    snapshot.settings.transcription_model = GetEffectiveTranscriptionModelLocked();
    snapshot.settings.has_stored_api_key = !s_stored_api_key.empty();
    snapshot.settings.has_sdkconfig_api_key = !GetSdkConfigApiKey().empty();
    snapshot.settings.api_key_source = GetApiKeySourceLocked();
    snapshot.settings.api_key_last4 = GetApiKeyLast4Locked();
    snapshot.settings.context_limit = GetEffectiveContextLimitLocked();

    snapshot.runtime.initialized = s_initialized;
    snapshot.runtime.ready = snapshot.settings.configured && s_reachable;
    snapshot.runtime.request_in_flight = s_request_in_flight;
    snapshot.runtime.readiness_checked = s_readiness_checked;
    snapshot.runtime.reachable = s_reachable;
    snapshot.runtime.last_http_status = s_last_http_status;
    snapshot.runtime.last_status_message = s_last_status_message;
    snapshot.runtime.last_error_code = s_last_error_code;
    snapshot.runtime.last_error_message = s_last_error_message;
    snapshot.runtime.model_count = s_model_count;
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

bool ShouldStartReadinessCheckLocked()
{
    return s_initialized &&
           s_network_connected &&
           !s_request_in_flight &&
           !s_reachable &&
           IsConfiguredLocked();
}

esp_err_t HttpEventHandler(esp_http_client_event_t* event)
{
    if (event == nullptr) {
        return ESP_FAIL;
    }
    auto* response = static_cast<HttpResponse*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && response != nullptr && event->data != nullptr &&
        event->data_len > 0) {
        response->body.append(static_cast<const char*>(event->data),
                              static_cast<size_t>(event->data_len));
    }
    return ESP_OK;
}

std::string JsonStringField(cJSON* root, const char* key)
{
    if (root == nullptr || key == nullptr) {
        return {};
    }

    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return {};
    }
    return item->valuestring;
}

// OpenAI-compatible error payloads nest the message under "error".
void PopulateHttpError(cJSON* root, const HttpResponse& response, std::string* error_code,
                       std::string* error_message)
{
    if (error_code == nullptr || error_message == nullptr) {
        return;
    }

    *error_code = "http_error";
    error_message->clear();
    if (root != nullptr) {
        cJSON* error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (cJSON_IsObject(error)) {
            const std::string type = JsonStringField(error, "type");
            const std::string message = JsonStringField(error, "message");
            if (!type.empty()) {
                *error_code = type;
            }
            if (!message.empty()) {
                *error_message = message;
            }
        } else if (cJSON_IsString(error)) {
            *error_message = error->valuestring != nullptr ? error->valuestring : "";
        }
    }
    if (error_message->empty()) {
        *error_message = response.body.empty() ? "LocalAI request failed" : response.body;
    }
    *error_message = TrimForLog(std::move(*error_message));
}

HttpResponse PerformGet(const std::string& url, const std::string& api_key, int timeout_ms)
{
    HttpResponse response = {};

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.crt_bundle_attach = esp_crt_bundle_attach;  // only used for https:// URLs
    config.timeout_ms = timeout_ms;
    config.event_handler = &HttpEventHandler;
    config.user_data = &response;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        response.error_code = "http_client_init_failed";
        response.error_message = "Failed to initialize LocalAI HTTP client";
        return response;
    }

    if (!api_key.empty()) {
        const std::string auth_header = "Bearer " + api_key;
        esp_http_client_set_header(client, "Authorization", auth_header.c_str());
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "folloup-sticky");

    const esp_err_t err = esp_http_client_perform(client);
    response.status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        response.error_code = "transport_error";
        response.error_message = esp_err_to_name(err);
    }
    return response;
}

HttpResponse PerformJsonPost(const std::string& url, const std::string& api_key,
                             const std::string& body, int timeout_ms)
{
    HttpResponse response = {};

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = timeout_ms;
    config.event_handler = &HttpEventHandler;
    config.user_data = &response;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        response.error_code = "http_client_init_failed";
        response.error_message = "Failed to initialize LocalAI HTTP client";
        return response;
    }

    if (!api_key.empty()) {
        const std::string auth_header = "Bearer " + api_key;
        esp_http_client_set_header(client, "Authorization", auth_header.c_str());
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "folloup-sticky");
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));

    const esp_err_t err = esp_http_client_perform(client);
    response.status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        response.error_code = "transport_error";
        response.error_message = esp_err_to_name(err);
    }
    return response;
}

ReadinessResult CheckReadiness(const std::string& base_url, const std::string& api_key)
{
    ReadinessResult result = {};
    const HttpResponse http = PerformGet(base_url + "/models", api_key, kReadinessTimeoutMs);
    result.http_status = http.status_code;

    if (!http.error_code.empty()) {
        result.error_code = http.error_code;
        result.error_message = TrimForLog(http.error_message);
        return result;
    }

    cJSON* root = cJSON_ParseWithLength(http.body.c_str(), http.body.size());
    if (http.status_code >= 200 && http.status_code < 300) {
        result.success = true;
        cJSON* data = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "data") : nullptr;
        if (cJSON_IsArray(data)) {
            result.model_count = cJSON_GetArraySize(data);
        }
    } else {
        PopulateHttpError(root, http, &result.error_code, &result.error_message);
    }
    if (root != nullptr) {
        cJSON_Delete(root);
    }
    return result;
}

void CompleteReadinessCheck(uint32_t generation, const ReadinessResult& result)
{
    bool stale_result = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (generation != s_readiness_generation) {
            stale_result = true;
        }
        if (!stale_result) {
            s_request_in_flight = false;
            s_readiness_checked = true;
            s_last_http_status = result.http_status;

            if (!result.success) {
                s_reachable = false;
                s_model_count = 0;
                s_last_status_message = "LocalAI is unreachable";
                SetLastErrorLocked(result.error_code.c_str(), result.error_message.c_str());
            } else {
                s_reachable = true;
                s_model_count = result.model_count;
                s_last_status_message = "Connected to LocalAI";
                ClearLastErrorLocked();
            }
        }
    }

    if (stale_result) {
        ESP_LOGI(kTag, "Ignoring stale LocalAI readiness result for generation %lu",
                 static_cast<unsigned long>(generation));
        return;
    }

    if (!result.success) {
        ESP_LOGW(kTag, "LocalAI readiness check failed: http=%d code=%s message=%s",
                 result.http_status,
                 result.error_code.empty() ? "http_error" : result.error_code.c_str(),
                 result.error_message.empty() ? "unknown" : result.error_message.c_str());
    } else {
        ESP_LOGI(kTag, "LocalAI readiness check succeeded: http=%d models=%d", result.http_status,
                 result.model_count);
    }

    Notify();
}

void ReadinessTask(void* arg)
{
    std::unique_ptr<ReadinessTaskContext> context(static_cast<ReadinessTaskContext*>(arg));
    if (context == nullptr) {
        CompleteReadinessCheck(0, ReadinessResult{
                                      .success = false,
                                      .http_status = 0,
                                      .model_count = 0,
                                      .error_code = "task_context_missing",
                                      .error_message = "LocalAI readiness task context missing",
                                  });
        vTaskDelete(nullptr);
        return;
    }

    const ReadinessResult result = CheckReadiness(context->base_url, context->api_key);
    CompleteReadinessCheck(context->generation, result);
    vTaskDelete(nullptr);
}

void MaybeBeginReadinessCheck()
{
    bool should_start = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        should_start = ShouldStartReadinessCheckLocked();
    }
    if (should_start) {
        (void)BeginReadinessCheck();
    }
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
    cJSON_AddBoolToObject(settings, "configured", snapshot.settings.configured);
    cJSON_AddStringToObject(settings, "base_url", snapshot.settings.base_url.c_str());
    cJSON_AddStringToObject(settings, "text_model", snapshot.settings.text_model.c_str());
    cJSON_AddStringToObject(settings, "transcription_model",
                            snapshot.settings.transcription_model.c_str());
    cJSON_AddBoolToObject(settings, "has_stored_api_key", snapshot.settings.has_stored_api_key);
    cJSON_AddBoolToObject(settings, "has_sdkconfig_api_key",
                          snapshot.settings.has_sdkconfig_api_key);
    cJSON_AddStringToObject(settings, "api_key_source",
                            ApiKeySourceName(snapshot.settings.api_key_source));
    cJSON_AddStringToObject(settings, "api_key_last4", snapshot.settings.api_key_last4.c_str());
    cJSON_AddNumberToObject(settings, "context_limit", snapshot.settings.context_limit);

    cJSON* runtime = cJSON_AddObjectToObject(root, "runtime");
    cJSON_AddBoolToObject(runtime, "initialized", snapshot.runtime.initialized);
    cJSON_AddBoolToObject(runtime, "ready", snapshot.runtime.ready);
    cJSON_AddBoolToObject(runtime, "request_in_flight", snapshot.runtime.request_in_flight);
    cJSON_AddBoolToObject(runtime, "readiness_checked", snapshot.runtime.readiness_checked);
    cJSON_AddBoolToObject(runtime, "reachable", snapshot.runtime.reachable);
    cJSON_AddNumberToObject(runtime, "last_http_status", snapshot.runtime.last_http_status);
    cJSON_AddStringToObject(runtime, "last_status_message",
                            snapshot.runtime.last_status_message.c_str());
    cJSON_AddStringToObject(runtime, "last_error_code", snapshot.runtime.last_error_code.c_str());
    cJSON_AddStringToObject(runtime, "last_error_message",
                            snapshot.runtime.last_error_message.c_str());
    cJSON_AddNumberToObject(runtime, "model_count", snapshot.runtime.model_count);
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

    const auto read_string_field = [&](const char* key, bool* has_flag, std::string* out) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (item == nullptr || cJSON_IsNull(item)) {
            return true;
        }
        if (!cJSON_IsString(item) || item->valuestring == nullptr) {
            return false;
        }
        *has_flag = true;
        *out = item->valuestring;
        return true;
    };

    bool ok = read_string_field("base_url", &patch->has_base_url, &patch->base_url) &&
              read_string_field("text_model", &patch->has_text_model, &patch->text_model) &&
              read_string_field("transcription_model", &patch->has_transcription_model,
                                &patch->transcription_model) &&
              read_string_field("api_key", &patch->has_api_key, &patch->api_key);

    if (ok) {
        cJSON* context_limit = cJSON_GetObjectItemCaseSensitive(root, "context_limit");
        if (context_limit != nullptr && !cJSON_IsNull(context_limit)) {
            if (!cJSON_IsNumber(context_limit)) {
                ok = false;
            } else {
                patch->has_context_limit = true;
                patch->context_limit = context_limit->valueint;
            }
        }
    }

    if (!ok && error != nullptr) {
        *error = "Invalid LocalAI settings payload";
    }

    cJSON_Delete(root);
    return ok;
}

esp_err_t RegisterPortalRoute(httpd_handle_t server, const httpd_uri_t* handler)
{
    if (server == nullptr || handler == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_err_t err = httpd_register_uri_handler(server, handler);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "Failed to register LocalAI portal route %s [%d]: %s",
                 handler->uri != nullptr ? handler->uri : "<null>",
                 static_cast<int>(handler->method), esp_err_to_name(err));
    }
    return err;
}

esp_err_t HandlePortalSettingsGet(httpd_req_t* request)
{
    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "LocalAI settings loaded");
    return SendJsonResponse(request, 200, root);
}

esp_err_t HandlePortalSettingsPatch(httpd_req_t* request)
{
    if (request == nullptr || request->content_len <= 0 ||
        request->content_len > static_cast<int>(kMaxPortalPayloadLen)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", "Invalid LocalAI settings payload");
        return SendJsonResponse(request, 400, root);
    }

    const std::string body = ReadRequestBody(request);
    SettingsPatch patch = {};
    std::string parse_error;
    if (body.empty() || !ParsePatchBody(body, &patch, &parse_error)) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "success", false);
        cJSON_AddStringToObject(root, "message", parse_error.empty()
                                                     ? "Invalid LocalAI settings payload"
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
    AppendSnapshot(root, GetSnapshot(), "LocalAI settings stored");
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
    AppendSnapshot(root, GetSnapshot(), "LocalAI settings reset");
    return SendJsonResponse(request, 200, root);
}

esp_err_t HandlePortalRuntimeGet(httpd_req_t* request)
{
    cJSON* root = cJSON_CreateObject();
    AppendSnapshot(root, GetSnapshot(), "LocalAI runtime loaded");
    return SendJsonResponse(request, 200, root);
}

// {"model": ..., "messages": [{"role": "user", "content": prompt}], "temperature": 0}
std::string BuildChatRequestBody(const std::string& model, const std::string& prompt)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model.c_str());
    cJSON* messages = cJSON_AddArrayToObject(root, "messages");
    cJSON* message = cJSON_CreateObject();
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(message, "content", prompt.c_str());
    cJSON_AddItemToArray(messages, message);
    cJSON_AddNumberToObject(root, "temperature", 0);

    char* raw = cJSON_PrintUnformatted(root);
    std::string json = raw != nullptr ? raw : "";
    if (raw != nullptr) {
        cJSON_free(raw);
    }
    cJSON_Delete(root);
    return json;
}

// choices[0].message.content, the OpenAI chat-completions response shape.
std::string ExtractChatCompletionText(cJSON* root)
{
    if (root == nullptr) {
        return {};
    }
    cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        return {};
    }
    cJSON* first = cJSON_GetArrayItem(choices, 0);
    if (!cJSON_IsObject(first)) {
        return {};
    }
    cJSON* message = cJSON_GetObjectItemCaseSensitive(first, "message");
    if (!cJSON_IsObject(message)) {
        return {};
    }
    return JsonStringField(message, "content");
}

void AppendLe16(uint16_t value, std::array<uint8_t, 44>* out, size_t* offset)
{
    if (out == nullptr || offset == nullptr || *offset + 2U > out->size()) {
        return;
    }
    (*out)[(*offset)++] = static_cast<uint8_t>(value & 0xFF);
    (*out)[(*offset)++] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void AppendLe32(uint32_t value, std::array<uint8_t, 44>* out, size_t* offset)
{
    if (out == nullptr || offset == nullptr || *offset + 4U > out->size()) {
        return;
    }
    (*out)[(*offset)++] = static_cast<uint8_t>(value & 0xFF);
    (*out)[(*offset)++] = static_cast<uint8_t>((value >> 8) & 0xFF);
    (*out)[(*offset)++] = static_cast<uint8_t>((value >> 16) & 0xFF);
    (*out)[(*offset)++] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

std::array<uint8_t, 44> BuildWavHeaderPcm16Mono(size_t sample_count, uint32_t sample_rate_hz)
{
    constexpr uint16_t kChannels = 1;
    constexpr uint16_t kBitsPerSample = 16;
    constexpr uint16_t kBlockAlign = kChannels * (kBitsPerSample / 8U);
    const uint32_t data_bytes = static_cast<uint32_t>(sample_count * sizeof(int16_t));
    const uint32_t byte_rate = sample_rate_hz * kBlockAlign;

    std::array<uint8_t, 44> header = {};
    size_t offset = 0;
    header[offset++] = 'R';
    header[offset++] = 'I';
    header[offset++] = 'F';
    header[offset++] = 'F';
    AppendLe32(36U + data_bytes, &header, &offset);
    header[offset++] = 'W';
    header[offset++] = 'A';
    header[offset++] = 'V';
    header[offset++] = 'E';
    header[offset++] = 'f';
    header[offset++] = 'm';
    header[offset++] = 't';
    header[offset++] = ' ';
    AppendLe32(16U, &header, &offset);
    AppendLe16(1U, &header, &offset);
    AppendLe16(kChannels, &header, &offset);
    AppendLe32(sample_rate_hz, &header, &offset);
    AppendLe32(byte_rate, &header, &offset);
    AppendLe16(kBlockAlign, &header, &offset);
    AppendLe16(kBitsPerSample, &header, &offset);
    header[offset++] = 'd';
    header[offset++] = 'a';
    header[offset++] = 't';
    header[offset++] = 'a';
    AppendLe32(data_bytes, &header, &offset);
    return header;
}

bool ReadHttpResponseBody(esp_http_client_handle_t client, HttpResponse* response)
{
    if (client == nullptr || response == nullptr) {
        return false;
    }
    std::array<char, 512> buffer = {};
    while (true) {
        const int read = esp_http_client_read(client, buffer.data(), buffer.size());
        if (read < 0) {
            response->error_code = "transport_error";
            response->error_message = "Failed reading HTTP response body";
            return false;
        }
        if (read == 0) {
            break;
        }
        response->body.append(buffer.data(), static_cast<size_t>(read));
    }
    return true;
}

std::string BuildMultipartFieldPart(const std::string& name, const std::string& value)
{
    return "--" + std::string(kMultipartBoundary) + "\r\n" + "Content-Disposition: form-data; name=\"" +
           name + "\"\r\n\r\n" + value + "\r\n";
}

std::string BuildMultipartFileHeader()
{
    return "--" + std::string(kMultipartBoundary) + "\r\n" +
           "Content-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n" +
           "Content-Type: " + kAudioMimeType + "\r\n\r\n";
}

std::string BuildMultipartClosing()
{
    return "\r\n--" + std::string(kMultipartBoundary) + "--\r\n";
}

// Streams a multipart/form-data body (model field + WAV file field) directly to the socket so
// the full clip never needs to be buffered into a single contiguous allocation.
HttpResponse PerformMultipartTranscription(const std::string& url, const std::string& api_key,
                                           const std::string& model,
                                           const recording_service::RecordedClip& clip)
{
    HttpResponse response = {};

    const std::string model_part = model.empty() ? "" : BuildMultipartFieldPart("model", model);
    const std::string file_header = BuildMultipartFileHeader();
    const std::array<uint8_t, 44> wav_header =
        BuildWavHeaderPcm16Mono(clip.sample_count(), clip.sample_rate_hz());
    const std::string closing = BuildMultipartClosing();
    const size_t total_bytes = model_part.size() + file_header.size() + wav_header.size() +
                                clip.pcm16_byte_count() + closing.size();

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = kTranscribeTimeoutMs;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        response.error_code = "http_client_init_failed";
        response.error_message = "Failed to initialize LocalAI transcription client";
        return response;
    }

    if (!api_key.empty()) {
        const std::string auth_header = "Bearer " + api_key;
        esp_http_client_set_header(client, "Authorization", auth_header.c_str());
    }
    const std::string content_type =
        std::string("multipart/form-data; boundary=") + kMultipartBoundary;
    esp_http_client_set_header(client, "Content-Type", content_type.c_str());
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, static_cast<int>(total_bytes));
    if (err != ESP_OK) {
        response.error_code = "transport_error";
        response.error_message = esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return response;
    }

    bool write_failed = false;
    const auto write_bytes = [&](const void* data, size_t size) {
        if (write_failed || data == nullptr || size == 0) {
            return;
        }
        const int written =
            esp_http_client_write(client, static_cast<const char*>(data), static_cast<int>(size));
        if (written != static_cast<int>(size)) {
            write_failed = true;
        }
    };

    if (!model_part.empty()) {
        write_bytes(model_part.data(), model_part.size());
    }
    write_bytes(file_header.data(), file_header.size());
    write_bytes(wav_header.data(), wav_header.size());
    clip.ForEachChunk([&](const int16_t* chunk_data, size_t chunk_size) {
        if (write_failed || chunk_data == nullptr || chunk_size == 0) {
            return;
        }
        write_bytes(chunk_data, chunk_size * sizeof(int16_t));
    });
    write_bytes(closing.data(), closing.size());

    if (write_failed) {
        response.error_code = "transport_error";
        response.error_message = "Failed streaming LocalAI audio upload";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return response;
    }

    const int response_length = esp_http_client_fetch_headers(client);
    response.status_code = esp_http_client_get_status_code(client);
    if (response.status_code <= 0 && response_length < 0) {
        response.error_code = "transport_error";
        response.error_message = "Failed fetching LocalAI transcription response headers";
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return response;
    }
    ReadHttpResponseBody(client, &response);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return response;
}

}  // namespace

esp_err_t Init()
{
    Snapshot snapshot = {};
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_initialized) {
            return ESP_OK;
        }

        const StoredSettings stored = LoadStoredSettings();
        s_stored_base_url = stored.base_url;
        s_stored_text_model = stored.text_model;
        s_stored_transcription_model = stored.transcription_model;
        s_stored_api_key = stored.api_key;
        s_stored_context_limit = stored.context_limit;
        s_last_status_message =
            IsConfiguredLocked() ? "LocalAI settings available" : "LocalAI is not configured";
        ClearLastErrorLocked();
        s_initialized = true;
        snapshot = BuildSnapshotLocked();
    }

    ESP_LOGI(kTag, "LocalAI service initialized: configured=%d base_url=%s text_model=%s",
             snapshot.settings.configured ? 1 : 0,
             snapshot.settings.base_url.empty() ? "<none>" : snapshot.settings.base_url.c_str(),
             snapshot.settings.text_model.empty() ? "<none>" : snapshot.settings.text_model.c_str());
    Notify();
    MaybeBeginReadinessCheck();
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
    bool should_start_check = false;
    bool save_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized) {
            const StoredSettings stored = LoadStoredSettings();
            s_stored_base_url = stored.base_url;
            s_stored_text_model = stored.text_model;
            s_stored_transcription_model = stored.transcription_model;
            s_stored_api_key = stored.api_key;
            s_stored_context_limit = stored.context_limit;
            s_initialized = true;
        }

        if (patch.has_base_url) {
            const std::string trimmed = StripTrailingSlash(TrimCopy(patch.base_url));
            if (trimmed.empty() || !LooksLikeHttpUrl(trimmed)) {
                return {
                    .success = false,
                    .validation_error = true,
                    .status_code = 400,
                    .field = "base_url",
                    .error_code = "invalid_base_url",
                    .message = "LocalAI base URL must start with http:// or https://",
                };
            }
        }
        if (patch.has_text_model && TrimCopy(patch.text_model).empty()) {
            return {
                .success = false,
                .validation_error = true,
                .status_code = 400,
                .field = "text_model",
                .error_code = "invalid_text_model",
                .message = "LocalAI text model is required",
            };
        }
        if (patch.has_transcription_model && TrimCopy(patch.transcription_model).empty()) {
            return {
                .success = false,
                .validation_error = true,
                .status_code = 400,
                .field = "transcription_model",
                .error_code = "invalid_transcription_model",
                .message = "LocalAI transcription model is required",
            };
        }
        if (patch.has_api_key && TrimCopy(patch.api_key).empty()) {
            return {
                .success = false,
                .validation_error = true,
                .status_code = 400,
                .field = "api_key",
                .error_code = "invalid_api_key",
                .message = "LocalAI API key cannot be blank",
            };
        }
        if (patch.has_context_limit &&
            (patch.context_limit < kMinContextLimit || patch.context_limit > kMaxContextLimit)) {
            return {
                .success = false,
                .validation_error = true,
                .status_code = 400,
                .field = "context_limit",
                .error_code = "invalid_context_limit",
                .message = "LocalAI context limit is out of range",
            };
        }

        if (patch.has_base_url) {
            s_stored_base_url = StripTrailingSlash(TrimCopy(patch.base_url));
        }
        if (patch.has_text_model) {
            s_stored_text_model = TrimCopy(patch.text_model);
        }
        if (patch.has_transcription_model) {
            s_stored_transcription_model = TrimCopy(patch.transcription_model);
        }
        if (patch.has_api_key) {
            s_stored_api_key = TrimCopy(patch.api_key);
        }
        if (patch.has_context_limit) {
            s_stored_context_limit = patch.context_limit;
        }

        if (!SaveStoredSettingsLocked()) {
            SetLastErrorLocked("nvs_write_failed", "Failed to store LocalAI settings");
            save_failed = true;
        } else {
            ++s_readiness_generation;
            s_request_in_flight = false;
            s_readiness_checked = false;
            s_reachable = false;
            s_last_http_status = 0;
            s_model_count = 0;
            s_last_status_message = "LocalAI settings stored";
            ClearLastErrorLocked();
            should_start_check = ShouldStartReadinessCheckLocked();
        }
    }

    if (save_failed) {
        Notify();
        return {
            .success = false,
            .validation_error = false,
            .status_code = 500,
            .field = {},
            .error_code = "nvs_write_failed",
            .message = "Failed to store LocalAI settings",
        };
    }

    Notify();
    if (should_start_check) {
        (void)BeginReadinessCheck();
    }
    return {
        .success = true,
        .validation_error = false,
        .status_code = 200,
        .field = {},
        .error_code = {},
        .message = "LocalAI settings stored",
    };
}

Result ResetSettings()
{
    bool should_start_check = false;
    bool clear_failed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized) {
            s_initialized = true;
        }

        if (!ClearStoredSettingsFromNvs()) {
            SetLastErrorLocked("nvs_clear_failed", "Failed to clear LocalAI settings");
            clear_failed = true;
        } else {
            s_stored_base_url.clear();
            s_stored_text_model.clear();
            s_stored_transcription_model.clear();
            s_stored_api_key.clear();
            s_stored_context_limit = 0;
            ++s_readiness_generation;
            s_request_in_flight = false;
            s_readiness_checked = false;
            s_reachable = false;
            s_last_http_status = 0;
            s_model_count = 0;
            s_last_status_message = "LocalAI settings reset";
            ClearLastErrorLocked();
            should_start_check = ShouldStartReadinessCheckLocked();
        }
    }

    Notify();
    if (clear_failed) {
        return {
            .success = false,
            .validation_error = false,
            .status_code = 500,
            .field = {},
            .error_code = "nvs_clear_failed",
            .message = "Failed to clear LocalAI settings",
        };
    }

    if (should_start_check) {
        (void)BeginReadinessCheck();
    }
    return {
        .success = true,
        .validation_error = false,
        .status_code = 200,
        .field = {},
        .error_code = {},
        .message = "LocalAI settings reset",
    };
}

bool IsConfigured()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return IsConfiguredLocked();
}

std::string GetEffectiveBaseUrl()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return GetEffectiveBaseUrlLocked();
}

std::string GetEffectiveTextModel()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return GetEffectiveTextModelLocked();
}

std::string GetEffectiveTranscriptionModel()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return GetEffectiveTranscriptionModelLocked();
}

std::string GetEffectiveApiKey()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return GetEffectiveApiKeyLocked();
}

int GetEffectiveContextLimit()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return GetEffectiveContextLimitLocked();
}

TextResult GenerateText(const std::string& prompt)
{
    TextResult result = {};
    const std::string base_url = GetEffectiveBaseUrl();
    const std::string model = GetEffectiveTextModel();
    const std::string api_key = GetEffectiveApiKey();
    if (base_url.empty() || model.empty()) {
        result.error_code = "not_configured";
        result.error_message = "LocalAI is not configured";
        return result;
    }
    if (prompt.empty()) {
        result.error_code = "empty_prompt";
        result.error_message = "Prompt was empty";
        return result;
    }

    const HttpResponse http = PerformJsonPost(base_url + "/chat/completions", api_key,
                                              BuildChatRequestBody(model, prompt),
                                              kGenerateTimeoutMs);
    result.http_status = http.status_code;
    if (!http.error_code.empty()) {
        result.error_code = http.error_code;
        result.error_message = TrimForLog(http.error_message);
    } else {
        cJSON* root = cJSON_ParseWithLength(http.body.c_str(), http.body.size());
        if (http.status_code >= 200 && http.status_code < 300) {
            result.text = ExtractChatCompletionText(root);
            result.success = !result.text.empty();
            if (!result.success) {
                result.error_code = "empty_response";
                result.error_message = "LocalAI returned no text";
            }
        } else {
            PopulateHttpError(root, http, &result.error_code, &result.error_message);
        }
        if (root != nullptr) {
            cJSON_Delete(root);
        }
    }

    if (result.success) {
        ESP_LOGI(kTag, "LocalAI chat completion succeeded: http=%d chars=%u", result.http_status,
                 static_cast<unsigned>(result.text.size()));
    } else {
        ESP_LOGW(kTag, "LocalAI chat completion failed: http=%d code=%s message=%s",
                 result.http_status, result.error_code.empty() ? "<none>" : result.error_code.c_str(),
                 result.error_message.empty() ? "<none>" : result.error_message.c_str());
    }
    return result;
}

TokenCountResult CountTokens(const std::string& prompt)
{
    // No universal OpenAI-compatible standalone token-count endpoint exists across LocalAI
    // backends; callers fall back to a character-based estimate on failure.
    TokenCountResult result = {};
    (void)prompt;
    result.error_code = "unsupported";
    result.error_message = "LocalAI does not support standalone token counting";
    return result;
}

TranscriptionResult Transcribe(const recording_service::RecordedClip& clip)
{
    TranscriptionResult result = {};
    result.clip_duration_ms = clip.duration_ms();
    result.wav_bytes = clip.wav_byte_count();

    const std::string base_url = GetEffectiveBaseUrl();
    const std::string model = GetEffectiveTranscriptionModel();
    const std::string api_key = GetEffectiveApiKey();
    if (base_url.empty() || model.empty()) {
        result.error_code = "not_configured";
        result.error_message = "LocalAI is not configured";
        return result;
    }
    if (clip.empty()) {
        result.error_code = "empty_audio";
        result.error_message = "No recorded audio available";
        return result;
    }

    const int64_t task_started_us = esp_timer_get_time();
    const HttpResponse http =
        PerformMultipartTranscription(base_url + "/audio/transcriptions", api_key, model, clip);
    result.http_status = http.status_code;
    result.total_elapsed_ms =
        static_cast<uint64_t>((esp_timer_get_time() - task_started_us) / 1000ULL);
    result.upload_elapsed_ms = result.total_elapsed_ms;

    if (!http.error_code.empty()) {
        result.error_code = http.error_code;
        result.error_message = TrimForLog(http.error_message);
        return result;
    }

    cJSON* root = cJSON_ParseWithLength(http.body.c_str(), http.body.size());
    if (http.status_code >= 200 && http.status_code < 300) {
        // OpenAI/whisper-style transcription responses are {"text": "..."}.
        result.transcript = TrimForLog(JsonStringField(root, "text"), 1U << 20);
        result.success = !result.transcript.empty();
        if (!result.success) {
            result.error_code = "empty_transcript";
            result.error_message = "LocalAI returned no transcript text";
        }
    } else {
        PopulateHttpError(root, http, &result.error_code, &result.error_message);
    }
    if (root != nullptr) {
        cJSON_Delete(root);
    }
    return result;
}

bool BeginReadinessCheck()
{
    std::string base_url;
    std::string api_key;
    uint32_t generation = 0;
    bool not_configured = false;

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (!s_initialized) {
            const StoredSettings stored = LoadStoredSettings();
            s_stored_base_url = stored.base_url;
            s_stored_text_model = stored.text_model;
            s_stored_transcription_model = stored.transcription_model;
            s_stored_api_key = stored.api_key;
            s_stored_context_limit = stored.context_limit;
            s_initialized = true;
        }
        if (s_request_in_flight) {
            return false;
        }

        base_url = GetEffectiveBaseUrlLocked();
        if (base_url.empty()) {
            s_request_in_flight = false;
            s_readiness_checked = false;
            s_reachable = false;
            s_last_http_status = 0;
            s_last_status_message = "Readiness check skipped";
            SetLastErrorLocked("not_configured", "LocalAI is not configured");
            not_configured = true;
        } else if (!s_network_connected) {
            return false;
        }

        if (!not_configured) {
            s_request_in_flight = true;
            s_readiness_checked = false;
            s_reachable = false;
            s_last_http_status = 0;
            s_last_status_message = "Checking LocalAI readiness";
            ClearLastErrorLocked();
            api_key = GetEffectiveApiKeyLocked();
            generation = ++s_readiness_generation;
        }
    }

    if (not_configured) {
        Notify();
        return false;
    }

    Notify();

    std::unique_ptr<ReadinessTaskContext> context(new (std::nothrow) ReadinessTaskContext{
        .base_url = std::move(base_url),
        .api_key = std::move(api_key),
        .generation = generation,
    });
    bool task_alloc_failed = false;
    bool task_start_failed = false;
    if (context == nullptr) {
        task_alloc_failed = true;
    } else {
        TaskHandle_t task_handle = nullptr;
        const BaseType_t created = xTaskCreatePinnedToCore(
            ReadinessTask, "localai_ready", kWorkerTaskStackWords, context.get(),
            followup_task_config::kPriorityAiProvider, &task_handle, followup_task_config::kSystemCore);
        if (created != pdPASS || task_handle == nullptr) {
            task_start_failed = true;
        } else {
            context.release();
        }
    }

    if (task_alloc_failed || task_start_failed) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_request_in_flight = false;
            s_last_status_message = "Failed to start LocalAI readiness check";
            SetLastErrorLocked(task_alloc_failed ? "task_alloc_failed" : "task_start_failed",
                               task_alloc_failed ? "Failed to allocate LocalAI task context"
                                                  : "Failed to start LocalAI readiness task");
        }
        Notify();
        return false;
    }

    ESP_LOGI(kTag, "Starting LocalAI readiness check (base_url=%s)", GetEffectiveBaseUrl().c_str());
    return true;
}

void SetNetworkState(bool connected, bool access_point_mode)
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_network_connected = connected;
        s_access_point_mode = access_point_mode;
    }
    MaybeBeginReadinessCheck();
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
        ESP_LOGW(kTag, "LocalAI portal routes are incomplete");
    }
}

const char* ApiKeySourceName(ApiKeySource source)
{
    switch (source) {
        case ApiKeySource::kSdkConfig:
            return "sdkconfig";
        case ApiKeySource::kNvs:
            return "nvs";
        case ApiKeySource::kNone:
        default:
            return "none";
    }
}

}  // namespace localai_service
