import type {
  AiProviderId,
  AiProviderResponse,
  LocalAiResponse,
  LocalAiSettings,
  StatusType,
  ValidatableField,
} from './types';

// --- AI provider selection (Gemini vs LocalAI) ------------------------------

interface AiProviderControllerDeps {
  aiProviderSelect: ValidatableField;
  fetchAiProviderJson: (path: string, init?: RequestInit) => Promise<AiProviderResponse>;
  notify: (message: string, type?: StatusType) => void;
  providerSettingsApi: string;
  updateButtons: () => void;
}

export function createAiProviderController(deps: AiProviderControllerDeps) {
  let isBusy = false;
  let provider: AiProviderId = 'gemini';

  function applySettings(settings?: { provider?: AiProviderId }) {
    provider = settings?.provider === 'localai' ? 'localai' : 'gemini';
    deps.aiProviderSelect.value = provider;
  }

  async function fetchStatus() {
    try {
      const data = await deps.fetchAiProviderJson(deps.providerSettingsApi);
      applySettings(data.settings);
    } catch (error) {
      console.error('AI provider status failed:', error);
    } finally {
      deps.updateButtons();
    }
  }

  async function setProvider(nextProvider: AiProviderId) {
    if (isBusy || nextProvider === provider) {
      return;
    }

    isBusy = true;
    deps.notify('Updating AI provider...', 'info');
    deps.updateButtons();

    try {
      const data = await deps.fetchAiProviderJson(deps.providerSettingsApi, {
        method: 'PATCH',
        body: JSON.stringify({ provider: nextProvider }),
      });
      applySettings(data.settings);
      deps.notify(data.message || 'AI provider updated.', 'success');
    } catch (error) {
      console.error('AI provider update failed:', error);
      // Revert the select back to the last-known-good provider on failure.
      deps.aiProviderSelect.value = provider;
      deps.notify(
        error instanceof Error ? error.message : 'Failed to update AI provider.',
        'error'
      );
    } finally {
      isBusy = false;
      deps.updateButtons();
    }
  }

  function handleProviderChange() {
    const value = deps.aiProviderSelect.value.trim();
    if (value === 'gemini' || value === 'localai') {
      void setProvider(value);
    }
  }

  return {
    fetchStatus,
    getProvider: () => provider,
    handleProviderChange,
    isBusy: () => isBusy,
  };
}

// --- LocalAI settings (base URL, models, optional key, context limit) ------

interface LocalAiControllerDeps {
  apiKeyInput: ValidatableField;
  baseUrlInput: ValidatableField;
  contextLimitInput: ValidatableField;
  fetchLocalAiJson: (path: string, init?: RequestInit) => Promise<LocalAiResponse>;
  localAiResetApi: string;
  localAiSettingsApi: string;
  notify: (message: string, type?: StatusType) => void;
  textModelInput: ValidatableField;
  transcriptionModelInput: ValidatableField;
  updateButtons: () => void;
}

function maskedApiKeyDisplay(settings?: LocalAiSettings): string {
  if (settings?.has_stored_api_key !== true) {
    return '';
  }
  const last4 = typeof settings.api_key_last4 === 'string' ? settings.api_key_last4 : '';
  return last4 ? `******${last4}` : '******';
}

export function createLocalAiController(deps: LocalAiControllerDeps) {
  let isBusy = false;
  // Tracks the masked placeholder currently shown in apiKeyInput so Save can tell "untouched
  // masked value" apart from "the user typed a real new key" without a separate readOnly state.
  let lastAppliedMaskedKey = '';

  function applySettings(settings?: LocalAiSettings) {
    deps.baseUrlInput.value = settings?.base_url ?? '';
    deps.textModelInput.value = settings?.text_model ?? '';
    deps.transcriptionModelInput.value = settings?.transcription_model ?? '';
    deps.contextLimitInput.value =
      typeof settings?.context_limit === 'number' ? String(settings.context_limit) : '';
    lastAppliedMaskedKey = maskedApiKeyDisplay(settings);
    deps.apiKeyInput.value = lastAppliedMaskedKey;
  }

  async function fetchStatus() {
    try {
      const data = await deps.fetchLocalAiJson(deps.localAiSettingsApi);
      applySettings(data.settings);
    } catch (error) {
      console.error('LocalAI settings status failed:', error);
    } finally {
      deps.updateButtons();
    }
  }

  async function saveSettings() {
    if (isBusy) {
      return;
    }

    const body: Record<string, string | number> = {
      base_url: deps.baseUrlInput.value.trim(),
      text_model: deps.textModelInput.value.trim(),
      transcription_model: deps.transcriptionModelInput.value.trim(),
    };

    const contextLimitRaw = deps.contextLimitInput.value.trim();
    if (contextLimitRaw) {
      const parsed = Number.parseInt(contextLimitRaw, 10);
      if (!Number.isNaN(parsed)) {
        body.context_limit = parsed;
      }
    }

    // Only send api_key when the field no longer shows the masked placeholder we applied from
    // the last fetch -- that's the only reliable signal the user typed a new key.
    const apiKeyValue = deps.apiKeyInput.value.trim();
    if (apiKeyValue && apiKeyValue !== lastAppliedMaskedKey) {
      body.api_key = apiKeyValue;
    }

    isBusy = true;
    deps.notify('Saving LocalAI settings...', 'info');
    deps.updateButtons();

    try {
      const data = await deps.fetchLocalAiJson(deps.localAiSettingsApi, {
        method: 'PATCH',
        body: JSON.stringify(body),
      });
      applySettings(data.settings);
      deps.notify(data.message || 'LocalAI settings stored.', 'success');
    } catch (error) {
      console.error('LocalAI settings save failed:', error);
      deps.notify(
        error instanceof Error ? error.message : 'Failed to store LocalAI settings.',
        'error'
      );
    } finally {
      isBusy = false;
      deps.updateButtons();
    }
  }

  async function resetSettings() {
    if (isBusy) {
      return;
    }

    isBusy = true;
    deps.notify('Resetting LocalAI settings...', 'info');
    deps.updateButtons();

    try {
      const data = await deps.fetchLocalAiJson(deps.localAiResetApi, { method: 'POST' });
      applySettings(data.settings);
      deps.notify(data.message || 'LocalAI settings reset.', 'success');
    } catch (error) {
      console.error('LocalAI settings reset failed:', error);
      deps.notify(
        error instanceof Error ? error.message : 'Failed to reset LocalAI settings.',
        'error'
      );
    } finally {
      isBusy = false;
      deps.updateButtons();
    }
  }

  return {
    fetchStatus,
    isBusy: () => isBusy,
    resetSettings,
    saveSettings,
  };
}
