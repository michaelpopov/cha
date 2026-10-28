import { prepareEpubImport } from '../notes/epubImport';
import { readFileBytes } from '../notes/pictures';

import {
  backgroundAudioStartRejected,
  type NoteItem,
  type FolderStoreSummary,
  type FolderStoreChoices,
  type FolderStoreStart,
  type FolderStoreOperation,
  type NoteContent,
  type NotePage,
  type TextFileRequest,
  type NotesClient,
  type NoteCompressionStartResult,
  type NoteCompressionStatus,
  type NoteAudioInfo,
  type NoteAudioAccepted,
  type NoteAudioBackgroundStatus,
  type NoteAudioStartResult,
  type NoteAudioClient,
  ChaError,
  ChaProtocolError,
  isApiKeyDetail,
  isCharacterDetail,
  isCharacterPicture,
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
  isAudioAcceptance,
  isAudioStatus,
  isMediaResource,
  isNativeVoiceInputRuntime,
  isVoiceInputSettings,
  isXaiVoicePieces,
  isXaiVoiceStartResult,
  isVoiceOutputRuntime,
  isVoiceOutputSettings,
  isJevSettings,
  isSessionNamingSettings,
  isNotesSettings,
  isActivityEventList,
  isWebSearchSettings,
  type JevSettings,
  type SessionNamingSettings,
  type NotesSettings,
  type WebSearchSettings,
  type ApiKeyDetail,
  type AudioDownloadBatchAcceptance,
  type ChaClient,
  type ForumSystemPrompt,
  type CharacterPicture,
  type CoverRequest,
  type CreateApiKeyRequest,
  type CreateProviderRequest,
  type CreateStyleRequest,
  type CreateVaultRequest,
  type CreateVoiceRequest,
  type DeleteTurnRequest,
  type InputRequest,
  type OpenSessionResult,
  type ProviderSummary,
  type ProviderUpdate,
  type R2StorageDetail,
  type SaveR2StorageRequest,
  type StyleDetail,
  type StyleUpdate,
  type VaultDetail,
  type VaultUploadCheck,
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
  state: 'running' | 'maintenance' | 'stopping' | 'unavailable';
  context_epoch: number;
  application_version: string;
  capabilities: { can_modify: boolean; can_transfer_r2: boolean };
  bootstrap: unknown;
} {
  return isRecord(value)
    && (value.state === 'running' || value.state === 'maintenance'
      || value.state === 'stopping' || value.state === 'unavailable')
    && Number.isSafeInteger(value.context_epoch)
    && (value.context_epoch as number) >= 1
    && typeof value.application_version === 'string'
    && isRecord(value.capabilities)
    && typeof value.capabilities.can_modify === 'boolean'
    && typeof value.capabilities.can_transfer_r2 === 'boolean'
    && 'bootstrap' in value;
}

function isSessionExport(value: unknown): value is { markdown: string } {
  return isRecord(value) && typeof value.markdown === 'string';
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

function isNoteId(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 1;
}

function isNoteParent(value: unknown): value is number | null {
  return value === null || isNoteId(value);
}

function hasExactKeys(value: Record<string, unknown>, keys: readonly string[]): boolean {
  const actual = Object.keys(value);
  return actual.length === keys.length && keys.every((key) => Object.hasOwn(value, key));
}

function isFolderStoreSummary(value: unknown): value is FolderStoreSummary {
  return isRecord(value) && hasExactKeys(value, ['service', 'id', 'state'])
    && (value.service === 'openai' || value.service === 'google')
    && isNoteId(value.id) && (value.state === 'ready' || value.state === 'incomplete');
}

function isFolderStoreChoices(value: unknown): value is FolderStoreChoices {
  return isRecord(value) && hasExactKeys(value, ['choices']) && Array.isArray(value.choices)
    && value.choices.every(choice => isRecord(choice)
      && hasExactKeys(choice, ['service', 'api_key_id', 'api_key_name'])
      && (choice.service === 'openai' || choice.service === 'google')
      && typeof choice.api_key_id === 'string' && /^(?!\.{1,2}$)[A-Za-z0-9._~-]+$/.test(choice.api_key_id)
      && typeof choice.api_key_name === 'string' && choice.api_key_name.length > 0);
}

function isFolderStoreStart(value: unknown): value is FolderStoreStart {
  return isRecord(value) && hasExactKeys(value, ['operation_id']) && isNoteId(value.operation_id);
}

function isFolderStoreOperation(value: unknown): value is FolderStoreOperation {
  if (!isRecord(value)) return false;
  if (value.present === false) return hasExactKeys(value, ['present']);
  return value.present === true && hasExactKeys(value, ['present', 'operation_id', 'kind', 'phase',
    'completed', 'total', 'error', 'result', 'terminal', 'known_cleanup_remains', 'folder_deleted'])
    && isNoteId(value.operation_id)
    && ['upload', 'remove', 'folder_delete'].includes(value.kind as string)
    && ['preparing', 'uploading', 'indexing', 'cleaning', 'removing', 'completed', 'failed', 'cancelled', 'warning'].includes(value.phase as string)
    && Number.isSafeInteger(value.completed) && (value.completed as number) >= 0
    && Number.isSafeInteger(value.total) && (value.total as number) >= 0
    && typeof value.error === 'string'
    && ['', 'preflight', 'error', 'success', 'cancelled', 'warning', 'folder_deleted'].includes(value.result as string)
    && typeof value.terminal === 'boolean' && typeof value.known_cleanup_remains === 'boolean'
    && typeof value.folder_deleted === 'boolean';
}

function isNoteItem(value: unknown): value is NoteItem {
  return isRecord(value)
    && hasExactKeys(value, ['id', 'parent_id', 'kind', 'name', 'has_compressed', 'read_only',
      ...(Object.hasOwn(value, 'store') ? ['store'] : [])])
    && (!Object.hasOwn(value, 'store') || (value.kind === 'folder' && isFolderStoreSummary(value.store)))
    && isNoteId(value.id)
    && isNoteParent(value.parent_id)
    && (value.kind === 'folder' || value.kind === 'note' || value.kind === 'annotation')
    && typeof value.name === 'string'
    && typeof value.has_compressed === 'boolean'
    && typeof value.read_only === 'boolean';
}

function isNoteItemList(value: unknown): value is { items: NoteItem[] } {
  return isRecord(value)
    && hasExactKeys(value, ['items'])
    && Array.isArray(value.items)
    && value.items.every(isNoteItem);
}

function isNoteContent(value: unknown): value is NoteContent {
  return isRecord(value)
    && hasExactKeys(value, ['id', 'document',
      ...('compressed_text' in value ? ['compressed_text'] : []),
      ...('scroll_top' in value ? ['scroll_top'] : []),
    ])
    && isNoteId(value.id)
    && isRecord(value.document)
    && (!('compressed_text' in value) || typeof value.compressed_text === 'string')
    && (!('scroll_top' in value) || (typeof value.scroll_top === 'number'
      && Number.isSafeInteger(value.scroll_top) && value.scroll_top >= 0));
}

function isEmptyResult(value: unknown): value is Record<string, never> {
  return isRecord(value) && Object.keys(value).length === 0;
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

function isVaultUploadCheck(value: unknown): value is VaultUploadCheck {
  return isRecord(value)
    && Number.isSafeInteger(value.context_epoch)
    && (value.context_epoch as number) >= 1
    && (value.etag === null
      || (typeof value.etag === 'string' && value.etag.length > 0))
    && (value.status === 'match' || value.status === 'mismatch'
      || value.status === 'missing')
    && (value.status !== 'missing' || value.etag === null)
    && (value.status !== 'match' || value.etag !== null);
}

export async function connectNativeBridge(bridge: NativeBridge) {
  const info = await bridge.invoke('bridge.info', {});
  if (!isBridgeInfo(info)) throw new ChaProtocolError();
  return info;
}

function isAudioAccepted(value: unknown): value is NoteAudioAccepted {
  return isRecord(value) && Object.keys(value).length === 1 && isNoteId(value.attempt_id);
}
function isAudioBackgroundStatus(value: unknown): value is NoteAudioBackgroundStatus {
  return isRecord(value) && (value.pending === false ? Object.keys(value).length === 1
    : value.pending === true && Object.keys(value).length === 2 && isNoteId(value.attempt_id));
}

export function createNativeChaClient(bridge: NativeBridge): ChaClient & NotesClient {
  let platform = '';
  let connectionEnded = false;
  bridge.on('app.connectionInvalidated', () => { connectionEnded = true; });
  bridge.on<unknown>('noteAudio.changed', value => {
    if (!isRecord(value) || value.event !== 'noteAudio.changed' || typeof value.connection_id !== 'string'
      || Object.keys(value).length !== 4 || !isNoteId(value.id) || !isNoteId(value.context_epoch)
      || value.context_epoch !== bridge.contextEpoch()) return;
    window.dispatchEvent(new CustomEvent('cha-note-audio-changed', { detail: { id: value.id, affectedNoteOnly: true } }));
  });
  function contextEpoch() {
    if (connectionEnded) throw new ChaError('application_unavailable', 'The connection to CHA ended.');
    return bridge.contextEpoch();
  }
  function requireContext(epoch: number) {
    if (contextEpoch() !== epoch) throw new ChaError('vault_changed', 'The vault changed. Start again in the current vault.');
  }
  bridge.on<unknown>('notes.compressionChanged', value => {
    if (connectionEnded || !isRecord(value) || value.event !== 'notes.compressionChanged'
      || typeof value.connection_id !== 'string' || Object.keys(value).length !== 4
      || !isNoteId(value.id) || !isNoteId(value.context_epoch) || value.context_epoch !== contextEpoch()) return;
    window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: value.id, epoch: value.context_epoch } }));
  });
  async function compressionStatus(id: number, epoch: number): Promise<NoteCompressionStatus> {
    requireContext(epoch);
    const result = await bridge.invoke('notes.compressionStatus', { id });
    requireContext(epoch);
    if (!isRecord(result) || !hasExactKeys(result, ['pending']) || typeof result.pending !== 'boolean') throw new ChaProtocolError();
    return result as NoteCompressionStatus;
  }
  const call = async <T>(
    method: string,
    params: unknown,
    validate: (value: unknown) => value is T,
  ): Promise<T> => {
    const result = await bridge.invoke(method, params);
    if (!validate(result)) throw new ChaProtocolError();
    return result;
  };

  const storeCall = async <T>(method: string, params: unknown, epoch: number,
    validate: (value: unknown) => value is T): Promise<T> => {
    requireContext(epoch);
    const result = await call(method, params, validate);
    requireContext(epoch);
    return result;
  };

  const transferVault = async (
    method: string,
    field: 'byte_count' | 'file_count',
    params: unknown = {},
  ) => {
    const result = await bridge.invoke(method, params);
    if (!isRecord(result)
        || !Number.isSafeInteger(result[field]) || (result[field] as number) < 0
        || !Number.isSafeInteger(result.context_epoch)
        || (result.context_epoch as number) < 1) {
      throw new ChaProtocolError();
    }
    bridge.setContextEpoch(result.context_epoch as number);
    return result[field] as number;
  };

  const backgroundStatus = (id: number) => call('noteAudio.backgroundStatus', { id }, isAudioBackgroundStatus);
  function startBackground(id: number, voice: string, blocks: string[]): Promise<NoteAudioAccepted>;
  function startBackground(id: number, voice: string, blocks: string[], onlyIfMissing: true): Promise<NoteAudioStartResult>;
  async function startBackground(id: number, voice: string, blocks: string[], onlyIfMissing = false): Promise<NoteAudioStartResult> {
    const epoch = bridge.contextEpoch();
    try {
      return await call('noteAudio.startBackground', { id, voice, blocks, ...(onlyIfMissing ? { only_if_missing: true } : {}) },
        (value): value is NoteAudioStartResult => isAudioAccepted(value) || (onlyIfMissing && isRecord(value)
          && Object.keys(value).length === 1 && value.skipped === true));
    } catch (failure) {
      if (backgroundAudioStartRejected(failure)) throw failure;
      const contextEnded = failure instanceof ChaError && ['vault_changed', 'application_unavailable', 'server_stopping'].includes(failure.code);
      if (!contextEnded && !connectionEnded && bridge.contextEpoch() === epoch) {
        try {
          const status = await backgroundStatus(id);
          if (!connectionEnded && bridge.contextEpoch() === epoch && status.pending) return { attempt_id: status.attempt_id };
        } catch { /* Absence or a failed read cannot prove rejection. */ }
      }
      throw failure;
    }
  }
  const noteAudio: NoteAudioClient = {
    contextEpoch() {
      if (connectionEnded) throw new ChaError('application_unavailable', 'The connection to CHA ended. Close this dialog and start again.');
      return bridge.contextEpoch();
    },
    backgroundStatus,
    startBackground,
    info: (id: number) => call('noteAudio.info', { id }, (value): value is NoteAudioInfo =>
      isRecord(value) && Number.isSafeInteger(value.count) && (value.count as number) >= 0),
    source: (id: number, ordinal: number) => call('noteAudio.source', { id, ordinal }, isMediaResource),
    release: async (resourceId: string) => { await call('speech.release', { resource_id: resourceId }, isRecord); },
  };
  return {
    get noteAudio() { return platform === 'macos' || platform === 'windows' ? noteAudio : undefined; },
    getBootstrap: async () => {
      platform = (await connectNativeBridge(bridge)).platform;
      const result = await bridge.invoke('app.bootstrap', {});
      if (!isBootstrapResult(result)) throw new ChaProtocolError();
      if (result.state !== 'running') {
        const message = result.state === 'maintenance'
          ? 'CHA is applying workspace changes. Try again when maintenance finishes.'
          : result.state === 'stopping'
            ? 'CHA is shutting down. Restart CHA to continue.'
            : 'CHA could not open the workspace. Restart CHA and check its logs.';
        throw new ChaError('application_unavailable', message);
      }
      bridge.setContextEpoch(result.context_epoch);
      return { ...validateBootstrap(result.bootstrap), capabilities: result.capabilities };
    },
    createSession: (forumId, label) => call(
      'session.create',
      { forum_id: forumId, label },
      isSessionLabelResult,
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
    attachTextFile: (forumId, sessionId, file) => call(
      'session.attachTextFile',
      { forum_id: forumId, session_id: sessionId, ...file },
      isCommandResult,
    ),
    stopGeneration: (forumId, sessionId) => call(
      'session.stop',
      { forum_id: forumId, session_id: sessionId },
      isCommandResult,
    ),
    getCharacterPicture: (characterId, filename) => call(
      'character.picture.get',
      { character_id: characterId, ...(filename === undefined ? {} : { filename }) },
      (value): value is CharacterPicture | null => value === null || isCharacterPicture(value),
    ),
    updateCharacterPicture: async (characterId, file, previousFilename) => {
      const extension = file.name.slice(file.name.lastIndexOf('.')).toLowerCase();
      if (!['.png', '.webp', '.jpg', '.jpeg', '.gif'].includes(extension)) {
        throw new ChaError('invalid_argument', 'Use a .png, .jpg, .jpeg, .webp, or .gif filename.');
      }
      // Base64 and JSON must fit within the native bridge's 16 MiB request limit.
      if (file.size > 8 * 1024 * 1024) {
        throw new ChaError('body_too_large', 'Choose a picture no larger than 8 MB.');
      }
      const epoch = bridge.contextEpoch();
      const data = await new Promise<string>((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => resolve(String(reader.result));
        reader.onerror = () => reject(new ChaError('invalid_argument', 'The picture file could not be read.'));
        reader.readAsDataURL(file);
      });
      if (epoch !== bridge.contextEpoch()) {
        throw new ChaError('vault_changed', 'The active vault changed. Choose the picture again.');
      }
      await call('character.picture.update', {
        character_id: characterId,
        filename: file.name,
        content_base64: data.slice(data.indexOf(',') + 1),
        ...(previousFilename === undefined ? {} : { previous_filename: previousFilename }),
      }, isRecord);
    },
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
    getCharacterInstructions: () => call('character.instructions.get', {}, isMarkdownFile),
    updateCharacterInstructions: (content) => call(
      'character.instructions.update', { content }, isMarkdownFile,
    ),
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
    renameCharacterFile: async (characterId, filename, newFilename) => {
      await call('character.file.rename', {
        character_id: characterId, filename, new_filename: newFilename,
      }, isRecord);
    },
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
    getForumSystemPrompt: (forumId, characterId) => call(
      'forum.systemPrompt',
      { forum_id: forumId, character_id: characterId },
      (value): value is ForumSystemPrompt => isRecord(value) && typeof value.system_prompt === 'string',
    ),
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
    discardUnusedSession: async (forumId, sessionId) => {
      await call(
        'session.delete',
        { forum_id: forumId, session_id: sessionId, only_if_unused: true },
        isRecord,
      );
    },
    clearSessionAudioCache: async (forumId, sessionId) => {
      await call(
        'audio.clearCache',
        { forum_id: forumId, session_id: sessionId },
        isRecord,
      );
    },
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
    getJevSettings: () => call('jev.get', {},
      (value): value is JevSettings | null => isNullable(value, isJevSettings)),
    saveJevSettings: (settings: JevSettings) => call('jev.save', settings, isJevSettings),
    disableJev: () => call('jev.disable', {}, isRecord).then(() => undefined),
    getSessionNamingSettings: () => call('sessionNaming.get', {}, isSessionNamingSettings),
    saveSessionNamingSettings: (settings: SessionNamingSettings) => call(
      'sessionNaming.save', settings, isSessionNamingSettings),
    getNotesSettings: () => call('notesSettings.get', {}, isNotesSettings),
    listEvents: async () => (await call('events.list', {}, isActivityEventList)).items,
    saveNotesSettings: (settings: NotesSettings) => call(
      'notesSettings.save', settings, isNotesSettings),
    getWebSearchSettings: () => call('webSearch.get', {}, isWebSearchSettings),
    saveWebSearchSettings: (settings: WebSearchSettings) => call(
      'webSearch.save', settings, isWebSearchSettings),
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
    startAudioDownloadBatch: (forumId, sessionId, request) => call(
      'audio.startBatch',
      {
        forum_id: forumId,
        session_id: sessionId,
        vault_name: request.vault_name,
        entries: request.entries,
      },
      (value): value is AudioDownloadBatchAcceptance => isRecord(value)
        && Array.isArray(value.entries)
        && value.entries.every(isAudioAcceptance),
    ),
    startAudioDownload: (forumId, sessionId, entryId, request) => call(
      'audio.start',
      {
        forum_id: forumId,
        session_id: sessionId,
        entry_id: entryId,
        vault_name: request.vault_name,
        reference_id: request.reference_id,
        provider: request.provider,
        settings: request.settings,
      },
      isAudioAcceptance,
    ),
    getAudioDownloads: (forumId, sessionId, vaultName) => call(
      'audio.status',
      { forum_id: forumId, session_id: sessionId, vault_name: vaultName },
      isAudioStatus,
    ),
    resolveAudioSource: (forumId, sessionId, entryId, vaultName) => call(
      'audio.source',
      {
        forum_id: forumId,
        session_id: sessionId,
        entry_id: entryId,
        vault_name: vaultName,
      },
      isMediaResource,
    ),
    previewSpeech: (text, referenceId, settings, signal, provider) => bridge.invoke(
      'speech.start',
      { text, reference_id: referenceId, settings: settings ?? {}, provider },
      { signal, cancelMethod: 'speech.cancel' },
    ).then((value) => {
      if (!isMediaResource(value)) throw new ChaProtocolError();
      return value;
    }),
    releaseResource: async (resourceId) => {
      await call('speech.release', { resource_id: resourceId }, isRecord);
    },
    connectVoiceInput: (sdp, languages, signal) => bridge.invoke(
      'voiceInput.connect',
      { sdp, languages },
      { signal, cancelMethod: 'voiceInput.cancel' },
    ).then((value) => {
      if (!isRecord(value) || typeof value.sdp !== 'string' || !value.sdp) {
        throw new ChaProtocolError();
      }
      return value.sdp;
    }),
    startXaiVoiceInput: (sessionId, languages, signal) => bridge.invoke(
      'voiceInput.xai.start',
      { session_id: sessionId, languages },
      { signal },
    ).then((value) => {
      if (!isXaiVoiceStartResult(value)) throw new ChaProtocolError();
      return value;
    }),
    sendXaiVoiceAudio: (sessionId, pcmBase64, signal) => bridge.invoke(
      'voiceInput.xai.audio',
      { session_id: sessionId, pcm_base64: pcmBase64 },
      { signal },
    ).then((value) => {
      if (!isXaiVoicePieces(value)) throw new ChaProtocolError();
      return value;
    }),
    stopXaiVoiceInput: (sessionId, remainingMs, signal) => bridge.invoke(
      'voiceInput.xai.stop',
      { session_id: sessionId, remaining_ms: remainingMs },
      { signal },
    ).then((value) => {
      if (!isXaiVoicePieces(value)) throw new ChaProtocolError();
      return value;
    }),
    cancelXaiVoiceInput: async (sessionId) => {
      await call('voiceInput.xai.cancel', { session_id: sessionId }, isRecord);
    },
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
    checkVaultUpload: () => call(
      'vault.upload.check', {}, isVaultUploadCheck,
    ),
    uploadVault: async (check) => {
      if (check.context_epoch !== bridge.contextEpoch()) {
        throw new ChaError('vault_changed', 'The active vault changed. Check the upload again.');
      }
      return transferVault('vault.upload', 'byte_count', { etag: check.etag });
    },
    downloadVault: () => transferVault('vault.download', 'byte_count'),
    importVault: () => transferVault('vault.import', 'file_count'),
    exportVault: () => transferVault('vault.export', 'file_count'),
    listNoteItems: async () => (await call('notes.list', {}, isNoteItemList)).items,
    storeChoices: epoch => storeCall('notes.storeChoices', {}, epoch, isFolderStoreChoices),
    storeUpload: (folderId, service, apiKeyId, epoch) => storeCall('notes.storeUpload',
      { folder_id: folderId, service, api_key_id: apiKeyId }, epoch, isFolderStoreStart),
    storeRemove: (folderId, storeId, epoch) => storeCall('notes.storeRemove',
      { folder_id: folderId, store_id: storeId }, epoch, isFolderStoreStart),
    storeOperation: (folderId, epoch) => storeCall('notes.storeOperation', { folder_id: folderId }, epoch, isFolderStoreOperation),
    storeCancel: async (operationId, epoch) => {
      await storeCall('notes.storeCancel', { operation_id: operationId }, epoch, isEmptyResult);
    },
    storeClose: async (operationId, epoch) => {
      await storeCall('notes.storeClose', { operation_id: operationId }, epoch, isEmptyResult);
    },
    folderDelete: (request, epoch) => storeCall('notes.folderDelete', request, epoch, isFolderStoreStart),
    importEpub: async (file, items, parentId) => {
      const epoch = bridge.contextEpoch();
      const request = prepareEpubImport(await readFileBytes(file).bytes, file.name, items, parentId);
      if (epoch !== bridge.contextEpoch()) {
        throw new ChaError('vault_changed', 'The active vault changed. Import the EPUB again.');
      }
      const result = await call('notes.importBook', request, (value): value is { id: number } => (
        isRecord(value) && Object.keys(value).length === 1
        && Number.isSafeInteger(value.id) && (value.id as number) > 0
      ));
      return result.id;
    },
    createNoteItem: (parentId, kind, name) => call(
      'notes.create',
      { parent_id: parentId, kind, name },
      isNoteItem,
    ),
    getNote: (id) => call('notes.get', { id }, isNoteContent),
    readNoteTextFile: (path) => call('notes.readTextFile', { path }, (value): value is TextFileRequest | null => (
      value === null || (isRecord(value) && Object.keys(value).length === 2
        && typeof value.name === 'string' && typeof value.text === 'string')
    )),
    readNotePicture: async (url) => {
      const result = await call('notes.readPicture', { url }, (value): value is { base64: string } => (
        isRecord(value) && Object.keys(value).length === 1 && typeof value.base64 === 'string'
      ));
      return Uint8Array.from(atob(result.base64), (character) => character.charCodeAt(0));
    },
    readNotePage: (url) => call('notes.readPage', { url }, (value): value is NotePage => (
      isRecord(value)
      && Object.keys(value).length === 3
      && typeof value.url === 'string'
      && typeof value.title === 'string'
      && typeof value.markdown === 'string'
    )),
    saveNote: async (id, document) => {
      return call('notes.save', { id, document }, (value): value is { annotation_created: boolean } => (
        isRecord(value) && Object.keys(value).length === 1 && typeof value.annotation_created === 'boolean'
      ));
    },
    saveNoteScrollPosition: async (id, scrollTop) => {
      await call('notes.saveScrollPosition', { id, scroll_top: scrollTop }, isEmptyResult);
    },
    contextEpoch,
    compressionStatus,
    compressNote: async (id, epoch, skipIneligible = false) => {
      requireContext(epoch);
      try {
        const result = await bridge.invoke('notes.compressStart', { id, ...(skipIneligible ? { skip_ineligible: true } : {}) });
        requireContext(epoch);
        if (isRecord(result) && ((hasExactKeys(result, ['accepted']) && result.accepted === true)
          || (skipIneligible && hasExactKeys(result, ['skipped']) && result.skipped === true))) return result as NoteCompressionStartResult;
        throw new ChaProtocolError();
      } catch (failure) {
        if (backgroundAudioStartRejected(failure)) throw failure;
        requireContext(epoch);
        if (failure instanceof ChaError && ['vault_changed', 'application_unavailable', 'server_stopping'].includes(failure.code)) throw failure;
        try {
          if ((await compressionStatus(id, epoch)).pending) return { accepted: true };
        } catch { /* A false or unavailable status cannot resolve a lost reply. */ }
        requireContext(epoch);
        throw new ChaError('internal_error', 'Could not confirm whether compression started. Check Settings → Events before starting again.');
      }
    },
    cancelCompression: async (id, epoch) => {
      requireContext(epoch);
      try {
        const result = await bridge.invoke('notes.compressCancel', { id });
        requireContext(epoch);
        if (!isEmptyResult(result)) throw new ChaProtocolError();
      } catch (failure) {
        requireContext(epoch);
        if (failure instanceof ChaError && ['vault_changed', 'application_unavailable', 'server_stopping'].includes(failure.code)) throw failure;
        try { if (!(await compressionStatus(id, epoch)).pending) return; }
        catch { /* Show an error unless local retirement is confirmed. */ }
        requireContext(epoch);
        throw new ChaError('internal_error', 'Could not confirm cancellation. Reopen the note menu and try again.');
      }
    },
    setNoteReadOnly: (id, readOnly) => call('notes.setReadOnly', { id, read_only: readOnly }, isNoteItem),
    renameNoteItem: (id, name) => call('notes.rename', { id, name }, isNoteItem),
    deleteNoteItem: async (id) => {
      await call('notes.delete', { id }, isEmptyResult);
    },
  };
}
