const restoreVoiceSettingsKey = 'cha.restoreVoiceSettings';

export function reloadForVoiceSettings(): void {
  window.sessionStorage.setItem(restoreVoiceSettingsKey, 'true');
  window.location.reload();
}

export function consumeVoiceSettingsRestore(): boolean {
  const restore = window.sessionStorage.getItem(restoreVoiceSettingsKey) === 'true';
  window.sessionStorage.removeItem(restoreVoiceSettingsKey);
  return restore;
}
