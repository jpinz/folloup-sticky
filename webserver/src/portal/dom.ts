import type {
  BottomSheetField,
  CardField,
  NetworkListField,
  NetworkStatusField,
  ValidatableField,
} from './types';

function getRequiredElement<T>(id: string): T {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`Required portal element "#${id}" was not found.`);
  }

  return element as T;
}

export interface PortalDom {
  followupLogoEl: HTMLDivElement;
  // WiFi
  wifiStatusCard: NetworkStatusField;
  wifiSettingsSheet: BottomSheetField;
  wifiSettingsCloseBtn: HTMLElement;
  wifiSettingsNotification: HTMLDivElement;
  networkList: NetworkListField;
  passwordInput: ValidatableField;
  scanBtn: HTMLButtonElement;
  connectBtn: HTMLButtonElement;
  // AI provider selection
  aiProviderCard: CardField;
  aiProviderSelect: ValidatableField;
  // Gemini API key
  geminiCard: CardField;
  geminiApiKeyInput: ValidatableField & { readOnly: boolean };
  geminiClearBtn: HTMLButtonElement;
  geminiSaveBtn: HTMLButtonElement;
  // LocalAI settings
  localAiCard: CardField;
  localAiBaseUrlInput: ValidatableField;
  localAiTextModelInput: ValidatableField;
  localAiTranscriptionModelInput: ValidatableField;
  localAiApiKeyInput: ValidatableField;
  localAiContextLimitInput: ValidatableField;
  localAiResetBtn: HTMLButtonElement;
  localAiSaveBtn: HTMLButtonElement;
  // Time / timezone
  timezoneLocationCard: CardField;
  timezoneSelect: ValidatableField;
  manualTimeInput: ValidatableField;
  manualDateInput: ValidatableField;
  timezoneLocationClearBtn: HTMLButtonElement;
  timezoneLocationSaveBtn: HTMLButtonElement;
}

export function createPortalDom(): PortalDom {
  return {
    followupLogoEl: getRequiredElement<HTMLDivElement>('followupLogo'),

    wifiStatusCard: getRequiredElement<NetworkStatusField>('wifiStatusCard'),
    wifiSettingsSheet: getRequiredElement<BottomSheetField>('wifiSettingsSheet'),
    wifiSettingsCloseBtn: getRequiredElement<HTMLElement>('wifiSettingsCloseBtn'),
    wifiSettingsNotification: getRequiredElement<HTMLDivElement>('wifiSettingsNotification'),
    networkList: getRequiredElement<NetworkListField>('networkList'),
    passwordInput: getRequiredElement<ValidatableField>('password'),
    scanBtn: getRequiredElement<HTMLButtonElement>('scanBtn'),
    connectBtn: getRequiredElement<HTMLButtonElement>('connectBtn'),

    aiProviderCard: getRequiredElement<CardField>('aiProviderCard'),
    aiProviderSelect: getRequiredElement<ValidatableField>('aiProviderSelect'),

    geminiCard: getRequiredElement<CardField>('geminiCard'),
    geminiApiKeyInput: getRequiredElement<ValidatableField & { readOnly: boolean }>(
      'geminiApiKeyInput'
    ),
    geminiClearBtn: getRequiredElement<HTMLButtonElement>('geminiClearBtn'),
    geminiSaveBtn: getRequiredElement<HTMLButtonElement>('geminiSaveBtn'),

    localAiCard: getRequiredElement<CardField>('localAiCard'),
    localAiBaseUrlInput: getRequiredElement<ValidatableField>('localAiBaseUrlInput'),
    localAiTextModelInput: getRequiredElement<ValidatableField>('localAiTextModelInput'),
    localAiTranscriptionModelInput: getRequiredElement<ValidatableField>(
      'localAiTranscriptionModelInput'
    ),
    localAiApiKeyInput: getRequiredElement<ValidatableField>('localAiApiKeyInput'),
    localAiContextLimitInput: getRequiredElement<ValidatableField>('localAiContextLimitInput'),
    localAiResetBtn: getRequiredElement<HTMLButtonElement>('localAiResetBtn'),
    localAiSaveBtn: getRequiredElement<HTMLButtonElement>('localAiSaveBtn'),

    timezoneLocationCard: getRequiredElement<CardField>('timezoneLocationCard'),
    timezoneSelect: getRequiredElement<ValidatableField>('timezoneSelect'),
    manualTimeInput: getRequiredElement<ValidatableField>('manualTimeInput'),
    manualDateInput: getRequiredElement<ValidatableField>('manualDateInput'),
    timezoneLocationClearBtn: getRequiredElement<HTMLButtonElement>('timezoneLocationClearBtn'),
    timezoneLocationSaveBtn: getRequiredElement<HTMLButtonElement>('timezoneLocationSaveBtn'),
  };
}
