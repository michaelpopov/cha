import {
  ChaError,
  ChaProtocolError,
  isCommandResult,
  isSessionSnapshot,
  type ChaClient,
  type CommandResult,
  type CreateSessionResult,
  type InputRequest,
  type OpenSessionResult,
  type SessionSnapshot,
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
    getCharacter: nativeUnavailable,
    createCharacter: nativeUnavailable,
    updateCharacter: nativeUnavailable,
    updateCharacterDefinition: nativeUnavailable,
    deleteCharacter: nativeUnavailable,
    getCharacterFile: nativeUnavailable,
    createCharacterFile: nativeUnavailable,
    updateCharacterFile: nativeUnavailable,
    deleteCharacterFile: nativeUnavailable,
    getPersona: nativeUnavailable,
    createPersona: nativeUnavailable,
    updatePersona: nativeUnavailable,
    deletePersona: nativeUnavailable,
    getForumFile: nativeUnavailable,
    createForumFile: nativeUnavailable,
    updateForumFile: nativeUnavailable,
    deleteForumFile: nativeUnavailable,
    getForum: nativeUnavailable,
    createForum: nativeUnavailable,
    updateForum: nativeUnavailable,
    deleteForum: nativeUnavailable,
    updateForumMembers: nativeUnavailable,
    listSessions: nativeUnavailable,
    renameSession: nativeUnavailable,
    deleteSession: nativeUnavailable,
    clearSessionAudioCache: nativeUnavailable,
    downloadSession: nativeUnavailable,
    coverConversation: nativeUnavailable,
    uncoverConversation: nativeUnavailable,
    deleteTurn: nativeUnavailable,
    setDefaultCharacter: nativeUnavailable,
    getOpenAiAuth: nativeUnavailable,
    startOpenAiAuth: nativeUnavailable,
    pollOpenAiAuth: nativeUnavailable,
    disconnectOpenAiAuth: nativeUnavailable,
    listVaults: nativeUnavailable,
    createVault: nativeUnavailable,
    listR2Vaults: nativeUnavailable,
    downloadR2Vault: nativeUnavailable,
    updateVault: nativeUnavailable,
    deleteVault: nativeUnavailable,
    listProviders: nativeUnavailable,
    createProvider: nativeUnavailable,
    getProvider: nativeUnavailable,
    testProvider: nativeUnavailable,
    updateProvider: nativeUnavailable,
    deleteProvider: nativeUnavailable,
    listStyles: nativeUnavailable,
    createStyle: nativeUnavailable,
    updateStyle: nativeUnavailable,
    deleteStyle: nativeUnavailable,
    listVoices: nativeUnavailable,
    createVoice: nativeUnavailable,
    updateVoice: nativeUnavailable,
    deleteVoice: nativeUnavailable,
    getVoiceInputSettings: nativeUnavailable,
    saveVoiceInputSettings: nativeUnavailable,
    getVoiceInputRuntime: nativeUnavailable,
    getVoiceOutputSettings: nativeUnavailable,
    saveVoiceOutputSettings: nativeUnavailable,
    getVoiceOutputRuntime: nativeUnavailable,
    listApiKeys: nativeUnavailable,
    createApiKey: nativeUnavailable,
    renameApiKey: nativeUnavailable,
    replaceApiKeyValue: nativeUnavailable,
    deleteApiKey: nativeUnavailable,
    getR2Storage: nativeUnavailable,
    saveR2Storage: nativeUnavailable,
    deleteR2Storage: nativeUnavailable,
    startAudioDownloadBatch: nativeUnavailable,
    startAudioDownload: nativeUnavailable,
    getAudioDownloads: nativeUnavailable,
    switchVault: nativeUnavailable,
    mergeVault: nativeUnavailable,
  };
}

export function isNativeCommandResult(value: unknown): value is CommandResult {
  return isCommandResult(value);
}

export function isNativeSessionSnapshot(value: unknown): value is SessionSnapshot {
  return isSessionSnapshot(value);
}
