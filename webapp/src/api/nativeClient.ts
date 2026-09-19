import {
  ChaError,
  ChaProtocolError,
  isApiKeyDetail,
  isCharacterDetail,
  isCommandResult,
  isForumDetail,
  isMarkdownFile,
  isOpenAiAuth,
  isPersonaDetail,
  isProviderDetail,
  isProviderSummary,
  isR2StorageDetail,
  isSessionLabelResult,
  isSessionListingArray,
  isSessionSnapshot,
  isStyleDetail,
  isVoiceDetail,
  isVoiceInputSettings,
  isVoiceOutputRuntime,
  isVoiceOutputSettings,
  type ApiKeyDetail,
  type ChaClient,
  type CommandResult,
  type CoverRequest,
  type CreateApiKeyRequest,
  type CreateProviderRequest,
  type CreateSessionResult,
  type CreateStyleRequest,
  type CreateVaultRequest,
  type CreateVoiceRequest,
  type DeleteTurnRequest,
  type InputRequest,
  type OpenSessionResult,
  type ProviderDetail,
  type ProviderSummary,
  type ProviderUpdate,
  type R2StorageDetail,
  type SaveR2StorageRequest,
  type SessionSnapshot,
  type StyleDetail,
  type StyleUpdate,
  type VaultDetail,
  type VaultUpdate,
  type VoiceDetail,
  type NativeVoiceInputRuntime,
  type VoiceInputSettings,
  type VoiceOutputRuntime,
  type VoiceOutputSettings,
  type VoiceUpdate,
} from './client';
import { nativeProtocolVersion, type NativeBridge } from './nativeBridge';
import { isRecord } from './guards';
import { validateBootstrap } from '../state/bootstrap';

const unavailableMessage = 'That operation is not available in native mode.';

function nativeUnavailable(): Promise<never> {
  return Promise.reject(new ChaError(0, 'invalid_argument', unavailableMessage));
}

function isCreateSessionResult(value: unknown): value is CreateSessionResult {
  return isRecord(value) && typeof value.id === 'string' && typeof value.label === 'string';
}

function isOpenSessionResult(value: unknown): value is OpenSessionResult {
  return isRecord(value)
    && typeof value.forum_id === 'string'
    && typeof value.session_id === 'string';
}

function isBridgeInfo(value: unknown): value is {
  protocol_version: number;
  application_version: string;
  platform: string;
} {
  return isRecord(value)
    && value.protocol_version === nativeProtocolVersion
    && typeof value.application_version === 'string'
    && typeof value.platform === 'string';
}

function isBootstrapResult(value: unknown): value is {
  state: string;
  context_epoch: number;
  bootstrap: unknown;
} {
  return isRecord(value)
    && typeof value.state === 'string'
    && Number.isSafeInteger(value.context_epoch)
    && (value.context_epoch as number) >= 1
    && 'bootstrap' in value;
}

function isSessionExport(value: unknown): value is { markdown: string } {
  return isRecord(value) && typeof value.markdown === 'string';
}

function isNativeVoiceInputRuntime(value: unknown): value is NativeVoiceInputRuntime {
  return isRecord(value)
    && typeof value.url === 'string' && value.url.length > 0
    && typeof value.model === 'string' && value.model.length > 0
    && (value.delay === 'low' || value.delay === 'medium'
      || value.delay === 'high' || value.delay === 'xhigh')
    && typeof value.prompt === 'string'
    && value.api_key === undefined;
}

function isNullable<T>(
  value: unknown,
  check: (value: unknown) => value is T,
): value is T | null {
  return value === null || check(value);
}

function isVaultDetail(value: unknown): value is VaultDetail {
  return isRecord(value)
    && typeof value.display_name === 'string'
    && typeof value.protected === 'boolean'
    && typeof value.data_path === 'string'
    && (value.mirror_path === null || typeof value.mirror_path === 'string')
    && (value.modify_path === null || typeof value.modify_path === 'string')
    && typeof value.active === 'boolean'
    && typeof value.can_delete === 'boolean';
}

function isMaintenanceResult(value: unknown): value is {
  state: string;
  context_epoch: number;
} {
  return isRecord(value)
    && typeof value.state === 'string'
    && Number.isSafeInteger(value.context_epoch)
    && (value.context_epoch as number) >= 1;
}

export async function connectNativeBridge(bridge: NativeBridge): Promise<void> {
  const info = await bridge.invoke('bridge.info', {});
  if (!isBridgeInfo(info)) throw new ChaProtocolError();
}

export function createNativeChaClient(bridge: NativeBridge): ChaClient {
  const call = async <T>(
    method: string,
    params: unknown,
    validate: (value: unknown) => value is T,
  ): Promise<T> => {
    const result = await bridge.invoke(method, params);
    if (!validate(result)) throw new ChaProtocolError();
    return result;
  };

  return {
    getBootstrap: async () => {
      await connectNativeBridge(bridge);
      const result = await bridge.invoke('app.bootstrap', {});
      if (!isBootstrapResult(result)) throw new ChaProtocolError();
      bridge.setContextEpoch(result.context_epoch);
      return validateBootstrap(result.bootstrap);
    },
    createSession: (forumId, label) => call(
      'session.create',
      { forum_id: forumId, label },
      isCreateSessionResult,
    ),
    openSession: (forumId, sessionId) => call(
      'session.open',
      { forum_id: forumId, session_id: sessionId },
      isOpenSessionResult,
    ),
    getSessionSnapshot: (forumId, sessionId) => call(
      'session.snapshot',
      { forum_id: forumId, session_id: sessionId },
      isSessionSnapshot,
    ),
    submitInput: (forumId, sessionId, input: InputRequest) => call(
      'session.submit',
      { forum_id: forumId, session_id: sessionId, input },
      isCommandResult,
    ),
    stopGeneration: (forumId, sessionId) => call(
      'session.stop',
      { forum_id: forumId, session_id: sessionId },
      isCommandResult,
    ),
    getCharacter: (characterId) => call(
      'character.get',
      { character_id: characterId },
      isCharacterDetail,
    ),
    createCharacter: (request) => call(
      'character.create',
      request,
      isCharacterDetail,
    ),
    updateCharacter: (characterId, settings) => call(
      'character.update',
      { character_id: characterId, ...settings },
      isCharacterDetail,
    ),
    updateCharacterDefinition: (characterId, update) => call(
      'character.updateDefinition',
      { character_id: characterId, ...update },
      isCharacterDetail,
    ),
    deleteCharacter: async (characterId) => {
      await call('character.delete', { character_id: characterId }, isRecord);
    },
    getCharacterFile: (characterId, filename) => call(
      'character.file.get',
      { character_id: characterId, filename },
      isMarkdownFile,
    ),
    createCharacterFile: (characterId, filename, content) => call(
      'character.file.create',
      { character_id: characterId, filename, content },
      isMarkdownFile,
    ),
    updateCharacterFile: (characterId, filename, content) => call(
      'character.file.update',
      { character_id: characterId, filename, content },
      isMarkdownFile,
    ),
    deleteCharacterFile: async (characterId, filename) => {
      await call(
        'character.file.delete',
        { character_id: characterId, filename },
        isRecord,
      );
    },
    getPersona: (personaId) => call(
      'persona.get',
      { persona_id: personaId },
      isPersonaDetail,
    ),
    createPersona: (request) => call(
      'persona.create',
      request,
      isPersonaDetail,
    ),
    updatePersona: (personaId, update) => call(
      'persona.update',
      { persona_id: personaId, ...update },
      isPersonaDetail,
    ),
    deletePersona: async (personaId) => {
      await call('persona.delete', { persona_id: personaId }, isRecord);
    },
    getForumFile: (forumId, filename) => call(
      'forum.file.get',
      { forum_id: forumId, filename },
      isMarkdownFile,
    ),
    createForumFile: (forumId, filename, content) => call(
      'forum.file.create',
      { forum_id: forumId, filename, content },
      isMarkdownFile,
    ),
    updateForumFile: (forumId, filename, content) => call(
      'forum.file.update',
      { forum_id: forumId, filename, content },
      isMarkdownFile,
    ),
    deleteForumFile: async (forumId, filename) => {
      await call(
        'forum.file.delete',
        { forum_id: forumId, filename },
        isRecord,
      );
    },
    getForum: (forumId) => call(
      'forum.get',
      { forum_id: forumId },
      isForumDetail,
    ),
    createForum: (request) => call(
      'forum.create',
      request,
      isForumDetail,
    ),
    updateForum: (forumId, update) => call(
      'forum.update',
      { forum_id: forumId, ...update },
      isForumDetail,
    ),
    deleteForum: async (forumId) => {
      await call('forum.delete', { forum_id: forumId }, isRecord);
    },
    updateForumMembers: (forumId, update) => call(
      'forum.members.update',
      { forum_id: forumId, ...update },
      isForumDetail,
    ),
    listSessions: (forumId) => call(
      'session.list',
      { forum_id: forumId },
      isSessionListingArray,
    ),
    renameSession: (forumId, sessionId, label) => call(
      'session.rename',
      { forum_id: forumId, session_id: sessionId, label },
      isSessionLabelResult,
    ),
    deleteSession: async (forumId, sessionId) => {
      await call(
        'session.delete',
        { forum_id: forumId, session_id: sessionId },
        isRecord,
      );
    },
    clearSessionAudioCache: nativeUnavailable,
    downloadSession: async (forumId, sessionId) => {
      const exported = await call(
        'session.export',
        { forum_id: forumId, session_id: sessionId },
        isSessionExport,
      );
      return exported.markdown;
    },
    coverConversation: (forumId, sessionId, request: CoverRequest) => call(
      'session.cover',
      { forum_id: forumId, session_id: sessionId, ...request },
      isCommandResult,
    ),
    uncoverConversation: (forumId, sessionId) => call(
      'session.uncover',
      { forum_id: forumId, session_id: sessionId },
      isCommandResult,
    ),
    deleteTurn: (forumId, sessionId, request: DeleteTurnRequest) => call(
      'session.deleteTurn',
      { forum_id: forumId, session_id: sessionId, ...request },
      isCommandResult,
    ),
    setDefaultCharacter: (forumId, sessionId, characterId) => call(
      'session.setDefaultCharacter',
      { forum_id: forumId, session_id: sessionId, character_id: characterId },
      isCommandResult,
    ),
    getOpenAiAuth: () => call('openaiAuth.get', {}, isOpenAiAuth),
    startOpenAiAuth: () => call('openaiAuth.start', {}, isOpenAiAuth),
    pollOpenAiAuth: () => call('openaiAuth.poll', {}, isOpenAiAuth),
    disconnectOpenAiAuth: () => call('openaiAuth.disconnect', {}, isOpenAiAuth),
    listVaults: () => call(
      'vault.list',
      {},
      (value): value is VaultDetail[] => Array.isArray(value) && value.every(isVaultDetail),
    ),
    createVault: (request: CreateVaultRequest) => call(
      'vault.create',
      request,
      isVaultDetail,
    ),
    listR2Vaults: () => call(
      'vault.r2.list',
      {},
      (value): value is string[] => Array.isArray(value)
        && value.every((name) => typeof name === 'string'),
    ),
    downloadR2Vault: (name) => call(
      'vault.r2.download',
      { name },
      isVaultDetail,
    ),
    updateVault: (vaultName, update: VaultUpdate) => call(
      'vault.update',
      { vault_name: vaultName, ...update },
      isVaultDetail,
    ),
    deleteVault: async (vaultName) => {
      await call('vault.delete', { vault_name: vaultName }, isRecord);
    },
    listProviders: () => call(
      'provider.list',
      {},
      (value): value is ProviderSummary[] => Array.isArray(value)
        && value.every(isProviderSummary),
    ),
    createProvider: (request: CreateProviderRequest) => call(
      'provider.create',
      request,
      isProviderDetail,
    ),
    getProvider: (providerId) => call(
      'provider.get',
      { provider_id: providerId },
      isProviderDetail,
    ),
    testProvider: async (providerId, candidate: ProviderUpdate) => {
      await call(
        'provider.test',
        { provider_id: providerId, ...candidate },
        isRecord,
      );
    },
    updateProvider: (providerId, update: ProviderUpdate) => call(
      'provider.update',
      { provider_id: providerId, ...update },
      isProviderDetail,
    ),
    deleteProvider: async (providerId) => {
      await call('provider.delete', { provider_id: providerId }, isRecord);
    },
    listStyles: () => call(
      'style.list',
      {},
      (value): value is StyleDetail[] => Array.isArray(value)
        && value.every(isStyleDetail),
    ),
    createStyle: (request: CreateStyleRequest) => call(
      'style.create',
      request,
      isStyleDetail,
    ),
    updateStyle: (styleId, update: StyleUpdate) => call(
      'style.update',
      { style_id: styleId, ...update },
      isStyleDetail,
    ),
    deleteStyle: async (styleId) => {
      await call('style.delete', { style_id: styleId }, isRecord);
    },
    listVoices: () => call(
      'voice.list',
      {},
      (value): value is VoiceDetail[] => Array.isArray(value)
        && value.every(isVoiceDetail),
    ),
    createVoice: (request: CreateVoiceRequest) => call(
      'voice.create',
      request,
      isVoiceDetail,
    ),
    updateVoice: (voiceId, update: VoiceUpdate) => call(
      'voice.update',
      { voice_id: voiceId, ...update },
      isVoiceDetail,
    ),
    deleteVoice: async (voiceId) => {
      await call('voice.delete', { voice_id: voiceId }, isRecord);
    },
    getVoiceInputSettings: () => call(
      'voiceInput.get',
      {},
      (value): value is VoiceInputSettings | null => (
        isNullable(value, isVoiceInputSettings)
      ),
    ),
    saveVoiceInputSettings: (settings: VoiceInputSettings) => call(
      'voiceInput.save',
      settings,
      isVoiceInputSettings,
    ),
    getVoiceInputRuntime: () => call(
      'voiceInput.runtime',
      {},
      (value): value is NativeVoiceInputRuntime | null => (
        isNullable(value, isNativeVoiceInputRuntime)
      ),
    ),
    getVoiceOutputSettings: () => call(
      'voiceOutput.get',
      {},
      (value): value is VoiceOutputSettings | null => (
        isNullable(value, isVoiceOutputSettings)
      ),
    ),
    saveVoiceOutputSettings: (settings: VoiceOutputSettings) => call(
      'voiceOutput.save',
      settings,
      isVoiceOutputSettings,
    ),
    getVoiceOutputRuntime: () => call(
      'voiceOutput.runtime',
      {},
      (value): value is VoiceOutputRuntime | null => (
        isNullable(value, isVoiceOutputRuntime)
      ),
    ),
    listApiKeys: () => call(
      'apiKey.list',
      {},
      (value): value is ApiKeyDetail[] => Array.isArray(value)
        && value.every(isApiKeyDetail),
    ),
    createApiKey: (request: CreateApiKeyRequest) => call(
      'apiKey.create',
      request,
      isApiKeyDetail,
    ),
    renameApiKey: (apiKeyId, displayName) => call(
      'apiKey.rename',
      { api_key_id: apiKeyId, display_name: displayName },
      isApiKeyDetail,
    ),
    replaceApiKeyValue: (apiKeyId, value) => call(
      'apiKey.replaceValue',
      { api_key_id: apiKeyId, value },
      isApiKeyDetail,
    ),
    deleteApiKey: async (apiKeyId) => {
      await call('apiKey.delete', { api_key_id: apiKeyId }, isRecord);
    },
    getR2Storage: () => call(
      'r2Storage.get',
      {},
      (value): value is R2StorageDetail | null => (
        isNullable(value, isR2StorageDetail)
      ),
    ),
    saveR2Storage: (request: SaveR2StorageRequest) => call(
      'r2Storage.save',
      request,
      isR2StorageDetail,
    ),
    deleteR2Storage: async () => {
      await call('r2Storage.delete', {}, isRecord);
    },
    startAudioDownloadBatch: nativeUnavailable,
    startAudioDownload: nativeUnavailable,
    getAudioDownloads: nativeUnavailable,
    switchVault: async (vaultName, password) => {
      const result = await call(
        'vault.switch',
        { vault_name: vaultName, password: password || null },
        isMaintenanceResult,
      );
      bridge.setContextEpoch(result.context_epoch);
    },
    mergeVault: async (sourceVault, password) => {
      const result = await call(
        'vault.merge',
        { source_vault: sourceVault, password: password || null },
        isMaintenanceResult,
      );
      bridge.setContextEpoch(result.context_epoch);
    },
  };
}

export function isNativeCommandResult(value: unknown): value is CommandResult {
  return isCommandResult(value);
}

export function isNativeSessionSnapshot(value: unknown): value is SessionSnapshot {
  return isSessionSnapshot(value);
}
