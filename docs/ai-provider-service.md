# AI Provider Service (Provider-Neutral Layer + LocalAI)

This document describes the provider-neutral AI layer added on top of the
existing Gemini integration, and the new LocalAI (self-hosted,
OpenAI-compatible) integration it can select instead of Gemini.

Three components are involved:

- `components/gemini_service/` — unchanged. Still owns Gemini API key
  precedence, Gemini authentication, and Gemini's own backend routes. See
  [`docs/gemini-service.md`](gemini-service.md) for its contract; Gemini
  remains fully, independently supported.
- `components/localai_service/` — new. Owns LocalAI settings precedence
  (base URL, text model, transcription model, optional API key, context
  limit), LocalAI readiness, and LocalAI's own backend routes. Structurally a
  sibling of `gemini_service`, not a variant of it.
- `components/ai_service/` — new. Owns provider selection (Gemini vs
  LocalAI) and is the only AI entry point the rest of the app calls. It
  dispatches text-generation/token-count/transcription calls to whichever
  provider is active and exposes a single provider-neutral settings/runtime
  snapshot and portal routes.

## Why A Neutral Layer

Before this change, `transcription_service`, `summary_service`,
`recording_session_service`, `main/status_bar_runtime.cpp`, and
`main/summarize_page_runtime.cpp` all called `gemini_service` directly. That
made Gemini a hard dependency baked into product code that has nothing to do
with Gemini specifically (it only needs "transcribe this clip" / "generate
this text" / "is an AI provider ready").

`ai_service` now sits between those callers and the two provider
implementations. Both providers stay fully initialized at all times — only
the *dispatch* is switched, so switching providers through the portal is
instant and doesn't require a reboot.

## Ownership

Current runtime split:

- `gemini_service` — Gemini settings, auth, and routes (unchanged; see
  `docs/gemini-service.md`)
- `localai_service` — LocalAI settings, readiness, and routes (this document)
- `ai_service` — active-provider selection, dispatch, and its own
  `/api/settings/ai` routes (this document)
- `wifi_service` — owns the backend HTTP server and hosts all three
  components' routes through the existing portal route registrar
- `main/app_shell.cpp`
  - initializes `ai_service` (which in turn initializes both
    `gemini_service` and `localai_service`), forwards Wi-Fi network state
    into `ai_service::SetNetworkState(...)`, registers portal routes through
    `ai_service::RegisterPortalRoutes(...)`, and reacts to the neutral
    `ai_service::Event`
- `main/status_bar_runtime.cpp` — reflects `ai_service::IsReady()` into
  `epaper_ui::StatusBarState::show_gemini_icon` (the field/asset name is
  unchanged; it now means "the active AI provider is ready", not
  specifically Gemini)
- `transcription_service` / `summary_service` / `recording_session_service`
  / `main/summarize_page_runtime.cpp` — call `ai_service::GenerateText`,
  `ai_service::CountTokens`, `ai_service::Transcribe`,
  `ai_service::IsReady()`, and `ai_service::IsConfigured()` instead of
  calling `gemini_service` directly

`app_shell` remains an orchestrator here: `ai_service` owns provider
selection/dispatch/route behavior, and `app_shell` only wires events,
startup order, and product-facing reactions (identical to how it already
treated `gemini_service`).

## LocalAI Settings

`localai_service` stores, in NVS namespace `localai`:

- `base_url` — the OpenAI-compatible API root, e.g.
  `http://192.168.1.20:8080/v1`. Endpoints are built by string-appending a
  suffix to this base: `{base}/models`, `{base}/chat/completions`,
  `{base}/audio/transcriptions`. Both `http://` and `https://` are
  supported; TLS validation uses the ESP-IDF CRT bundle for `https://` URLs.
- `text_model` — the model name sent as `"model"` in chat-completions
  requests.
- `transcription_model` — the model name sent as the `"model"` multipart
  field in transcription requests.
- `api_key` — optional. When present, sent as an HTTP bearer-token
  authorization header. Many self-hosted LocalAI deployments don't require
  one.
- `context_limit` — an integer token budget the summarizer uses to cap
  prompt sizing (see "Context Limit" below).

Each has a matching `sdkconfig` fallback (built-in default, for bench
testing), mirroring Gemini's precedence:

1. NVS-stored value saved through the backend API
2. built-in `sdkconfig` default
3. unset (component reports `configured = false` until `base_url` and
   `text_model` are both non-empty)

Build-time defaults (`main/Kconfig.projbuild`, under `Folloup Settings`):

- `CONFIG_FOLLOWUP_LOCALAI_BASE_URL` (default `""`)
- `CONFIG_FOLLOWUP_LOCALAI_TEXT_MODEL` (default `"gpt-4"`)
- `CONFIG_FOLLOWUP_LOCALAI_TRANSCRIPTION_MODEL` (default `"whisper-1"`)
- `CONFIG_FOLLOWUP_LOCALAI_API_KEY` (default `""`)
- `CONFIG_FOLLOWUP_LOCALAI_CONTEXT_LIMIT` (default `8192`)

Resetting LocalAI settings (`POST /api/settings/localai/reset`) clears all
five stored NVS overrides at once and reverts to the sdkconfig defaults
above (unlike Gemini's reset, which only clears the API key — LocalAI has
several interdependent fields, so a partial reset would leave settings in an
inconsistent state).

## Provider Selection

`ai_service` stores the active provider in NVS namespace `ai_provider`, key
`provider`, as the literal string `"gemini"` or `"localai"`. Precedence:

1. NVS-stored provider saved through the backend API
2. built-in `CONFIG_FOLLOWUP_AI_PROVIDER` sdkconfig default (`"gemini"`)

`POST /api/settings/ai/reset` clears the NVS override and reverts to the
sdkconfig default provider.

## Readiness

- **Gemini**: unchanged — see `docs/gemini-service.md` (`GET
  v1beta/<model>` with `x-goog-api-key`).
- **LocalAI**: `GET {base}/models`, sent with a bearer-token authorization
  header when a key is configured. Any `2xx` response marks LocalAI reachable;
  `model_count` is read from a top-level `"data"` array when present
  (OpenAI's `/models` response shape). This check runs automatically once
  Wi-Fi is connected (not access-point mode) and LocalAI is configured, same
  trigger conditions as Gemini's auth flow.

`ai_service::IsReady()` reflects whichever provider is currently active, so
readiness for the *inactive* provider doesn't affect the status bar or the
summarize/transcribe gating — but it is still tracked and returned by the
portal so switching providers doesn't require re-checking readiness first.

## OpenAI-Compatible Text Generation

`localai_service::GenerateText` posts to `{base}/chat/completions`:

```json
{
  "model": "<text_model>",
  "messages": [{"role": "user", "content": "<prompt>"}],
  "temperature": 0
}
```

The response is parsed as `choices[0].message.content`, the standard OpenAI
chat-completions shape. `temperature: 0` keeps summaries deterministic,
matching Gemini's `generateContent` behavior.

## Token Counting

LocalAI backends don't expose a universal, standalone token-count endpoint
the way Gemini's `countTokens` does. `localai_service::CountTokens` always
returns `success = false`; `summary_service` already falls back to a
character-based estimate (`chars / 4`) whenever a provider's token count
fails, so this degrades gracefully rather than requiring a wasted
completion call just to count tokens.

## Streamed Multipart WAV Transcription

`localai_service::Transcribe` posts a `multipart/form-data` body to
`{base}/audio/transcriptions`:

- a `model` field (the configured transcription model)
- a `file` field: `recording.wav`, `Content-Type: audio/wav`

The WAV bytes (44-byte header + PCM16 mono samples) are streamed directly to
the socket via `esp_http_client_write` in the same chunked fashion Gemini
uses for its resumable upload — the full clip is never buffered into one
contiguous allocation. The response is parsed as `{"text": "..."}`, the
Whisper/OpenAI transcription response shape.

## Context Limit

Gemini's context window is large enough that `summary_service`'s existing
token budgets (120000 input / 60000 chunk / 120000 rollup) were hardcoded.
Self-hosted LocalAI models are frequently much smaller (a few thousand
tokens), so `ai_service::GetContextTokenBudget(default_budget)` caps those
constants down to the configured `context_limit` whenever LocalAI is the
active provider:

- Gemini active: returns `default_budget` unchanged (no behavior change).
- LocalAI active: returns `min(default_budget, context_limit)`.

`summary_service` calls this once per constant
(`EffectiveInputTokenBudget()`, `EffectiveChunkTokenBudget()`,
`EffectiveRollupTokenBudget()`) instead of using the raw constants directly,
so the same chunk/rollup algorithm now stays inside whatever context window
the operator configured for their self-hosted model.

## Backend Endpoints

LocalAI (masked key only — the raw key is never returned):

- `GET /api/settings/localai`
- `PATCH /api/settings/localai`
- `POST /api/settings/localai/reset`
- `GET /api/runtime/localai`

Provider selection:

- `GET /api/settings/ai`
- `PATCH /api/settings/ai`
- `POST /api/settings/ai/reset`
- `GET /api/runtime/ai`

All four LocalAI routes and all four `ai` routes are registered by
`ai_service::RegisterPortalRoutes(server)`, which also forwards registration
to `gemini_service::RegisterPortalRoutes(server)` — `app_shell` only calls
`ai_service::RegisterPortalRoutes(...)` once.

### `PATCH /api/settings/localai` request shape

```json
{
  "base_url": "http://192.168.1.20:8080/v1",
  "text_model": "gpt-4",
  "transcription_model": "whisper-1",
  "api_key": "sk-...",
  "context_limit": 8192
}
```

All fields are optional; only the fields present are updated. `base_url`
must start with `http://` or `https://`. `api_key` is masked in every
response — only `has_stored_api_key`, `has_sdkconfig_api_key`,
`api_key_source`, and `api_key_last4` are ever returned.

### `PATCH /api/settings/ai` request shape

```json
{
  "provider": "localai"
}
```

`provider` must be `"gemini"` or `"localai"`.

## Deployment: Running A LocalAI-Compatible Server

Any server that implements the OpenAI `GET /models`, `POST
/chat/completions`, and `POST /audio/transcriptions` endpoints works,
including:

- [mudler/LocalAI](https://github.com/mudler/LocalAI)
- `llama.cpp`'s built-in OpenAI-compatible server
- Any other OpenAI-compatible gateway on the same LAN as the device

Point `base_url` at the server's API root including its version prefix
(commonly `/v1`), e.g. `http://192.168.1.20:8080/v1`. The device only needs
Wi-Fi station connectivity to the server's LAN; no internet access is
required for LocalAI the way it is for Gemini. `https://` is supported for
servers fronted by a reverse proxy with a certificate the ESP-IDF CRT bundle
trusts.

## Deferred Features

Not yet ported:

- on-device settings page UI for AI provider/LocalAI (portal-only for now,
  same as Gemini)
- streaming (token-by-token) text generation or transcription responses —
  both providers return a single complete response
- automatic model-list validation (confirming `text_model` /
  `transcription_model` actually exist in `GET /models`'s `data` array)
