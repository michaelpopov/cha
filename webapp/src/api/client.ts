import type { components } from './schema';
import { hasIdentity, isRecord } from './guards';

export type AudioDownloadBatchEntry = components['schemas']['AudioDownloadBatchEntry'];
export type AudioDownloadBatchRequest = components['schemas']['AudioDownloadBatchRequest'];
export type AudioDownloadBatchAcceptance = components['schemas']['AudioDownloadBatchAcceptance'];
export type AudioDownloadRequest = components['schemas']['AudioDownloadRequest'];
export type AudioDownloadAcceptance = components['schemas']['AudioDownloadAcceptance'];
export type AudioDownloadStatus = components['schemas']['AudioDownloadStatus'];

export type Bootstrap = components['schemas']['Bootstrap'];
export type CharacterDetail = components['schemas']['CharacterDetail'];
export type MarkdownFile = components['schemas']['MarkdownFile'];
export type CreateCharacterRequest = components['schemas']['CreateCharacterRequest'];
export type UpdateCharacterRequest = components['schemas']['UpdateCharacterRequest'];
export type UpdateCharacterDefinitionRequest =
  components['schemas']['UpdateCharacterDefinitionRequest'];
export type PersonaDetail = components['schemas']['PersonaDetail'];
export type CreatePersonaRequest = components['schemas']['CreatePersonaRequest'];
export type UpdatePersonaRequest = components['schemas']['UpdatePersonaRequest'];
export type ForumDetail = components['schemas']['ForumDetail'];
export type ForumSummary = components['schemas']['ForumSummary'];
export type CreateForumRequest = components['schemas']['CreateForumRequest'];
export type UpdateForumRequest = components['schemas']['UpdateForumRequest'];
export type UpdateForumMembersRequest = components['schemas']['UpdateForumMembersRequest'];
export type CharacterAppearance = components['schemas']['CharacterAppearance'];
export type SpeechVoice = components['schemas']['SpeechVoice'];
export type SessionListing = components['schemas']['SessionListing'];
export type CreateSessionResult = components['schemas']['CreateSessionResult'];
export type SessionLabelResult = components['schemas']['SessionLabelResult'];
export type OpenSessionResult = components['schemas']['OpenSessionResult'];
export type SessionSnapshot = components['schemas']['SessionSnapshot'];
export type CommandResult = components['schemas']['CommandResult'];
export type InputRequest = components['schemas']['InputRequest'];
export type CoverRequest = components['schemas']['CoverRequest'];
export type DeleteTurnRequest = components['schemas']['DeleteTurnRequest'];
export type OpenAiAuth = components['schemas']['OpenAiAuth'];
export type VaultDetail = components['schemas']['VaultDetail'];
export type CreateVaultRequest = components['schemas']['CreateVaultRequest'];
export type MergeVaultRequest = components['schemas']['MergeVaultRequest'];
export type VaultUpdate = Omit<components['schemas']['UpdateVaultRequest'], 'vault_name'>;
export type ProviderSummary = components['schemas']['ProviderSummary'];
export type ProviderDetail = components['schemas']['ProviderDetail'];
export type CreateProviderRequest = components['schemas']['CreateProviderRequest'];
export type ProviderUpdate = components['schemas']['ProviderUpdate'];
export type StyleDetail = components['schemas']['StyleDetail'];
export type CreateStyleRequest = components['schemas']['CreateStyleRequest'];
export type StyleUpdate = components['schemas']['StyleUpdate'];
export type VoiceDetail = components['schemas']['VoiceDetail'];
export type CreateVoiceRequest = components['schemas']['CreateVoiceRequest'];
export type VoiceUpdate = components['schemas']['VoiceUpdate'];
export type VoiceInputSettings = components['schemas']['VoiceInputSettings'];
export type VoiceInputRuntime = components['schemas']['VoiceInputRuntime'];
// Native wire omits the HTTP secret. `api_key?: never` keeps the union distinct.
export type NativeVoiceInputRuntime = Omit<VoiceInputRuntime, 'api_key'> & {
  readonly api_key?: never;
};
export type VoiceOutputSettings = components['schemas']['VoiceOutputSettings'];
export type VoiceOutputRuntime = components['schemas']['VoiceOutputRuntime'];
export type ApiKeyDetail = components['schemas']['ApiKeyDetail'];
export type CreateApiKeyRequest = components['schemas']['CreateApiKeyRequest'];
export type R2StorageDetail = components['schemas']['R2StorageDetail'];
export type SaveR2StorageRequest = components['schemas']['SaveR2StorageRequest'];
export type ErrorCode = components['schemas']['ErrorResponse']['error']['code'];
export type MediaResource = components['schemas']['MediaResource'];
export type BridgeInfo = components['schemas']['BridgeInfo'];
export type NativeRequest = components['schemas']['NativeRequest'];
export type NativeReply = components['schemas']['NativeReply'];
export type NativeSessionEvent = components['schemas']['NativeSessionEvent'];
export type NativeDeliveryAck = components['schemas']['NativeDeliveryAck'];

// Generated API unions are compile-time only. Keeping the runtime list checked
// against that union prevents a newer or malformed server string from being
// presented to the rest of the client as a code this browser actually knows.
const knownErrorCodes = {
  not_found: true,
  bad_request: true,
  body_too_large: true,
  prompt_too_large: true,
  forbidden_origin: true,
  internal_error: true,
  speech_busy: true,
  vault_changed: true,
  session_stopping: true,
  session_limit_reached: true,
  session_open_timeout: true,
  server_stopping: true,
  session_not_live: true,
  command_timeout: true,
  command_queue_full: true,
  vault_password_required: true,
  source_vault_password_required: true,
  invalid_argument: true,
  operation_cancelled: true,
  application_unavailable: true,
} satisfies Record<ErrorCode, true>;

function isErrorCode(value: unknown): value is ErrorCode {
  return typeof value === 'string' && Object.hasOwn(knownErrorCodes, value);
}

type Fetcher = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;

export class ChaError extends Error {
  constructor(
    readonly status: number,
    readonly code: ErrorCode,
    message: string,
  ) {
    super(message);
    this.name = 'ChaError';
  }
}

export class ChaUnavailableError extends Error {
  constructor() {
    super('CHA’s application API is unavailable. Check that CHA is running and try again.');
    this.name = 'ChaUnavailableError';
  }
}

export class ChaProtocolError extends TypeError {
  constructor() {
    super('CHA returned an incompatible response. Restart CHA with matching browser files.');
    this.name = 'ChaProtocolError';
  }
}

// Only server-authored public API messages and fixed browser messages reach the
// screen. Arbitrary exception text can contain implementation details and is
// kept out of the customer interface.
export function publicErrorMessage(failure: unknown, fallback: string): string {
  return failure instanceof ChaError
      || failure instanceof ChaUnavailableError
      || failure instanceof ChaProtocolError
    ? failure.message
    : fallback;
}

export interface ChaClient {
  getBootstrap(): Promise<Bootstrap>;
  getCharacter(characterId: string): Promise<CharacterDetail>;
  createCharacter(request: CreateCharacterRequest): Promise<CharacterDetail>;
  updateCharacter(characterId: string, settings: UpdateCharacterRequest): Promise<CharacterDetail>;
  updateCharacterDefinition(
    characterId: string,
    update: UpdateCharacterDefinitionRequest,
  ): Promise<CharacterDetail>;
  deleteCharacter(characterId: string): Promise<void>;
  getCharacterFile(characterId: string, filename: string): Promise<MarkdownFile>;
  createCharacterFile(characterId: string, filename: string, content: string): Promise<MarkdownFile>;
  updateCharacterFile(characterId: string, filename: string, content: string): Promise<MarkdownFile>;
  deleteCharacterFile(characterId: string, filename: string): Promise<void>;
  getPersona(personaId: string): Promise<PersonaDetail>;
  createPersona(request: CreatePersonaRequest): Promise<PersonaDetail>;
  updatePersona(personaId: string, update: UpdatePersonaRequest): Promise<PersonaDetail>;
  deletePersona(personaId: string): Promise<void>;
  getForumFile(forumId: string, filename: string): Promise<MarkdownFile>;
  createForumFile(forumId: string, filename: string, content: string): Promise<MarkdownFile>;
  updateForumFile(forumId: string, filename: string, content: string): Promise<MarkdownFile>;
  deleteForumFile(forumId: string, filename: string): Promise<void>;
  getForum(forumId: string): Promise<ForumDetail>;
  createForum(request: CreateForumRequest): Promise<ForumDetail>;
  updateForum(forumId: string, update: UpdateForumRequest): Promise<ForumDetail>;
  deleteForum(forumId: string): Promise<void>;
  updateForumMembers(
    forumId: string,
    update: UpdateForumMembersRequest,
  ): Promise<ForumDetail>;
  listSessions(forumId: string): Promise<SessionListing[]>;
  createSession(forumId: string, label: string): Promise<CreateSessionResult>;
  renameSession(forumId: string, sessionId: string, label: string): Promise<SessionLabelResult>;
  deleteSession(forumId: string, sessionId: string): Promise<void>;
  clearSessionAudioCache(forumId: string, sessionId: string): Promise<void>;
  downloadSession(forumId: string, sessionId: string): Promise<string>;
  openSession(forumId: string, sessionId: string): Promise<OpenSessionResult>;
  getSessionSnapshot(forumId: string, sessionId: string): Promise<SessionSnapshot>;
  submitInput(forumId: string, sessionId: string, input: InputRequest): Promise<CommandResult>;
  coverConversation(
    forumId: string,
    sessionId: string,
    request: CoverRequest,
  ): Promise<CommandResult>;
  uncoverConversation(forumId: string, sessionId: string): Promise<CommandResult>;
  deleteTurn(
    forumId: string,
    sessionId: string,
    request: DeleteTurnRequest,
  ): Promise<CommandResult>;
  stopGeneration(forumId: string, sessionId: string): Promise<CommandResult>;
  setDefaultCharacter(
    forumId: string,
    sessionId: string,
    characterId: string,
  ): Promise<CommandResult>;
  getOpenAiAuth(): Promise<OpenAiAuth>;
  startOpenAiAuth(): Promise<OpenAiAuth>;
  pollOpenAiAuth(): Promise<OpenAiAuth>;
  disconnectOpenAiAuth(): Promise<OpenAiAuth>;
  listVaults(): Promise<VaultDetail[]>;
  createVault(request: CreateVaultRequest): Promise<VaultDetail>;
  listR2Vaults(): Promise<string[]>;
  downloadR2Vault(name: string): Promise<VaultDetail>;
  updateVault(vaultName: string, update: VaultUpdate): Promise<VaultDetail>;
  deleteVault(vaultName: string): Promise<void>;
  listProviders(): Promise<ProviderSummary[]>;
  createProvider(request: CreateProviderRequest): Promise<ProviderDetail>;
  getProvider(providerId: string): Promise<ProviderDetail>;
  testProvider(providerId: string, candidate: ProviderUpdate): Promise<void>;
  updateProvider(providerId: string, update: ProviderUpdate): Promise<ProviderDetail>;
  deleteProvider(providerId: string): Promise<void>;
  listStyles(): Promise<StyleDetail[]>;
  createStyle(request: CreateStyleRequest): Promise<StyleDetail>;
  updateStyle(styleId: string, update: StyleUpdate): Promise<StyleDetail>;
  deleteStyle(styleId: string): Promise<void>;
  listVoices(): Promise<VoiceDetail[]>;
  createVoice(request: CreateVoiceRequest): Promise<VoiceDetail>;
  updateVoice(voiceId: string, update: VoiceUpdate): Promise<VoiceDetail>;
  deleteVoice(voiceId: string): Promise<void>;
  getVoiceInputSettings(): Promise<VoiceInputSettings | null>;
  saveVoiceInputSettings(settings: VoiceInputSettings): Promise<VoiceInputSettings>;
  getVoiceInputRuntime(): Promise<VoiceInputRuntime | NativeVoiceInputRuntime | null>;
  getVoiceOutputSettings(): Promise<VoiceOutputSettings | null>;
  saveVoiceOutputSettings(settings: VoiceOutputSettings): Promise<VoiceOutputSettings>;
  getVoiceOutputRuntime(): Promise<VoiceOutputRuntime | null>;
  listApiKeys(): Promise<ApiKeyDetail[]>;
  createApiKey(request: CreateApiKeyRequest): Promise<ApiKeyDetail>;
  renameApiKey(apiKeyId: string, displayName: string): Promise<ApiKeyDetail>;
  replaceApiKeyValue(apiKeyId: string, value: string): Promise<ApiKeyDetail>;
  deleteApiKey(apiKeyId: string): Promise<void>;
  getR2Storage(): Promise<R2StorageDetail | null>;
  saveR2Storage(request: SaveR2StorageRequest): Promise<R2StorageDetail>;
  deleteR2Storage(): Promise<void>;
  startAudioDownloadBatch(forumId: string, sessionId: string, request: AudioDownloadBatchRequest): Promise<AudioDownloadBatchAcceptance>;
  startAudioDownload(forumId: string, sessionId: string, entryId: number, request: AudioDownloadRequest): Promise<AudioDownloadAcceptance>;
  getAudioDownloads(forumId: string, sessionId: string, vaultName: string): Promise<AudioDownloadStatus>;
  resolveAudioSource?(
    forumId: string,
    sessionId: string,
    entryId: number,
    vaultName: string,
  ): Promise<MediaResource>;
  previewSpeech?(
    text: string,
    referenceId: string | undefined,
    settings: { speed?: number } | undefined,
    signal?: AbortSignal,
  ): Promise<MediaResource>;
  releaseResource?(resourceId: string): Promise<void>;
  connectVoiceInput?(
    sdp: string,
    languages: string[],
    signal?: AbortSignal,
  ): Promise<string>;
  switchVault(vaultName: string, password?: string): Promise<void>;
  mergeVault(sourceVault: string, password?: string): Promise<void>;
}

export function isAudioAcceptance(value: unknown): value is AudioDownloadAcceptance {
  return isRecord(value) && Number.isSafeInteger(value.entry_id) && (value.entry_id as number) > 0
    && typeof value.cached === 'boolean' && (value.cached
      ? value.state === undefined : value.state === 'queued' || value.state === 'running');
}
export function isAudioStatus(value: unknown): value is AudioDownloadStatus {
  return isRecord(value) && Array.isArray(value.cached_entry_ids)
    && value.cached_entry_ids.every((id) => Number.isSafeInteger(id) && id > 0)
    && Array.isArray(value.downloads) && value.downloads.every((job) => isRecord(job)
      && Number.isSafeInteger(job.entry_id) && (job.entry_id as number) > 0
      && ['queued', 'running', 'failed'].includes(job.state as string)
      && (job.error === undefined || typeof job.error === 'string'));
}
export function cachedAudioUrl(forumId: string, sessionId: string, entryId: number, vault: string): string {
  return `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}/entries/${entryId}/audio?vault_name=${encodeURIComponent(vault)}`;
}

function isOneOf(value: unknown, choices: readonly unknown[]): boolean {
  return choices.includes(value);
}

function isCharacterAppearance(value: unknown): value is CharacterAppearance {
  return isRecord(value)
    && isOneOf(value.font, ['sans', 'serif', 'mono'])
    && isOneOf(value.style, ['normal', 'italic'])
    && isOneOf(value.weight, ['light', 'normal', 'medium', 'semibold', 'bold'])
    && isOneOf(value.size, ['small', 'normal', 'large'])
    && isOneOf(value.text_color, ['normal', 'muted', 'accent']);
}

function isOptionalBoundedNumber(value: unknown, minimum: number, maximum: number): boolean {
  return value === undefined
    || (typeof value === 'number'
      && Number.isFinite(value)
      && value >= minimum
      && value <= maximum);
}

function isSpeechVoice(value: unknown): value is SpeechVoice {
  return isRecord(value)
    && hasIdentity(value)
    && typeof value.elevenlabs_voice_id === 'string'
    && value.elevenlabs_voice_id.length > 0
    && isRecord(value.settings)
    && isOptionalBoundedNumber(value.settings.speed, 0.7, 1.2);
}

function isCharacterSummary(value: unknown): boolean {
  return isRecord(value)
    && hasIdentity(value)
    && isCharacterAppearance(value.appearance)
    && (value.voice === undefined || isSpeechVoice(value.voice));
}

function isPersonaSummary(value: unknown): boolean {
  return isRecord(value)
    && hasIdentity(value)
    && isCharacterAppearance(value.appearance)
    && (value.voice === undefined || isSpeechVoice(value.voice));
}

export function isMarkdownFile(value: unknown): value is MarkdownFile {
  return isRecord(value) && typeof value.filename === 'string'
    && typeof value.content === 'string' && typeof value.writable === 'boolean';
}

export function isCharacterDetail(value: unknown): value is CharacterDetail {
  return isCharacterSummary(value)
    && isRecord(value)
    && typeof value.character_markdown === 'string'
    && typeof value.editable_markdown === 'string'
    && Array.isArray(value.markdown_files)
    && value.markdown_files.every((filename) => typeof filename === 'string')
    && (value.provider === null || typeof value.provider === 'string')
    && (value.style === null || typeof value.style === 'string')
    && (value.voice_id === null || typeof value.voice_id === 'string')
    && isOneOf(value.reasoning_effort, ['low', 'medium', 'high', 'xhigh', null])
    && isOneOf(value.web_search, ['off', 'auto', 'required', null])
    && Array.isArray(value.available_providers)
    && value.available_providers.every((option) => isRecord(option)
      && typeof option.id === 'string' && typeof option.label === 'string')
    && Array.isArray(value.available_styles)
    && value.available_styles.every((option) => isRecord(option)
      && typeof option.id === 'string'
      && typeof option.label === 'string'
      && isCharacterAppearance(option.appearance))
    && Array.isArray(value.available_voices)
    && value.available_voices.every((option) => isRecord(option)
      && typeof option.id === 'string' && typeof option.label === 'string')
    && typeof value.settings_writable === 'boolean'
    && typeof value.writable === 'boolean';
}

export function isPersonaDetail(value: unknown): value is PersonaDetail {
  return isPersonaSummary(value)
    && isRecord(value)
    && typeof value.persona_markdown === 'string'
    && (value.style === null || typeof value.style === 'string')
    && (value.voice_id === null || typeof value.voice_id === 'string')
    && Array.isArray(value.available_styles)
    && value.available_styles.every((option) => isRecord(option)
      && typeof option.id === 'string'
      && typeof option.label === 'string'
      && isCharacterAppearance(option.appearance))
    && Array.isArray(value.available_voices)
    && value.available_voices.every((option) => isRecord(option)
      && typeof option.id === 'string' && typeof option.label === 'string')
    && typeof value.writable === 'boolean';
}

export function isForumDetail(value: unknown): value is ForumDetail {
  return isRecord(value)
    && hasIdentity(value)
    && typeof value.default_character_id === 'string'
    && typeof value.default_persona_id === 'string'
    && typeof value.default_persona_display_name === 'string'
    && Array.isArray(value.members)
    && value.members.every(isCharacterSummary)
    && typeof value.forum_markdown === 'string'
    && Array.isArray(value.markdown_files)
    && value.markdown_files.every((filename) => typeof filename === 'string')
    && typeof value.writable === 'boolean';
}

export function isProviderSummary(value: unknown): value is ProviderSummary {
  return isRecord(value) && hasIdentity(value)
    && typeof value.model === 'string' && typeof value.host === 'string';
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

export function isProviderDetail(value: unknown): value is ProviderDetail {
  return isRecord(value) && hasIdentity(value)
    && typeof value.model === 'string' && typeof value.host === 'string'
    && isOneOf(value.mode, ['net', 'test'])
    && typeof value.port === 'number'
    && Number.isInteger(value.port)
    && typeof value.base_path === 'string'
    && typeof value.stream === 'boolean'
    && (value.temperature === null || typeof value.temperature === 'number')
    && (value.max_tokens === null || (typeof value.max_tokens === 'number'
      && Number.isInteger(value.max_tokens)))
    && typeof value.timeout_s === 'number'
    && typeof value.idle_timeout_s === 'number'
    && (value.api_key === null || typeof value.api_key === 'string')
    && typeof value.reasoning_effort === 'string'
    && isOneOf(value.reasoning_format, ['auto', 'none', 'reasoning_content', 'reasoning'])
    && typeof value.https === 'boolean'
    && isOneOf(value.api, ['chat_completions', 'responses'])
    && isOneOf(value.auth, ['none', 'openai_subscription'])
    && isOneOf(value.web_search, ['off', 'auto', 'required'])
    && isOneOf(value.cache_retention, ['off', 'short', 'long'])
    && Array.isArray(value.openrouter_targets)
    && value.openrouter_targets.every((target) => typeof target === 'string')
    && typeof value.writable === 'boolean'
    && Array.isArray(value.used_by)
    && value.used_by.every((name) => typeof name === 'string');
}

export function isStyleDetail(value: unknown): value is StyleDetail {
  return isRecord(value) && hasIdentity(value)
    && isOneOf(value.font, ['sans', 'serif', 'mono'])
    && isOneOf(value.style, ['normal', 'italic'])
    && isOneOf(value.weight, ['light', 'normal', 'medium', 'semibold', 'bold'])
    && isOneOf(value.size, ['small', 'normal', 'large'])
    && isOneOf(value.text_color, ['normal', 'muted', 'accent'])
    && typeof value.writable === 'boolean'
    && Array.isArray(value.used_by)
    && value.used_by.every((name) => typeof name === 'string');
}

function isNullableBoundedNumber(
  value: unknown,
  minimum: number,
  maximum: number,
): boolean {
  return value === null
    || (typeof value === 'number' && Number.isFinite(value)
      && value >= minimum && value <= maximum);
}

export function isVoiceDetail(value: unknown): value is VoiceDetail {
  return isRecord(value) && hasIdentity(value)
    && typeof value.description === 'string'
    && typeof value.elevenlabs_voice_id === 'string'
    && value.elevenlabs_voice_id.length > 0
    && isNullableBoundedNumber(value.speed, 0.7, 1.2)
    && typeof value.writable === 'boolean'
    && Array.isArray(value.used_by)
    && value.used_by.every((name) => typeof name === 'string');
}

export function isApiKeyDetail(value: unknown): value is ApiKeyDetail {
  return isRecord(value) && hasIdentity(value)
    && typeof value.has_value === 'boolean'
    && Array.isArray(value.used_by)
    && value.used_by.every((name) => typeof name === 'string');
}

export function isVoiceInputSettings(value: unknown): value is VoiceInputSettings {
  return isRecord(value)
    && typeof value.url === 'string' && value.url.length > 0
    && typeof value.model === 'string' && value.model.length > 0
    && typeof value.api_key === 'string' && value.api_key.length > 0
    && (value.delay === 'low' || value.delay === 'medium'
      || value.delay === 'high' || value.delay === 'xhigh')
    && typeof value.prompt === 'string';
}

export function isVoiceInputRuntime(value: unknown): value is VoiceInputRuntime {
  return isVoiceInputSettings(value);
}

export function isNativeVoiceInputRuntime(value: unknown): value is NativeVoiceInputRuntime {
  return isRecord(value)
    && typeof value.url === 'string' && value.url.length > 0
    && typeof value.model === 'string' && value.model.length > 0
    && (value.delay === 'low' || value.delay === 'medium'
      || value.delay === 'high' || value.delay === 'xhigh')
    && typeof value.prompt === 'string'
    && value.api_key === undefined;
}

export function isUsableVoiceInputRuntime(
  value: unknown,
): value is VoiceInputRuntime | NativeVoiceInputRuntime {
  return isVoiceInputRuntime(value) || isNativeVoiceInputRuntime(value);
}

export function isMediaResource(value: unknown): value is MediaResource {
  return isRecord(value)
    && typeof value.resource_id === 'string' && value.resource_id.length > 0
    && typeof value.url === 'string' && value.url.length > 0
    && typeof value.mime_type === 'string' && value.mime_type.length > 0
    && Number.isSafeInteger(value.byte_length)
    && (value.byte_length as number) >= 0;
}

export function isVoiceOutputSettings(value: unknown): value is VoiceOutputSettings {
  return isRecord(value)
    && typeof value.url === 'string' && value.url.length > 0
    && typeof value.model === 'string' && value.model.length > 0
    && typeof value.api_key === 'string' && value.api_key.length > 0
    && typeof value.output_format === 'string' && value.output_format.length > 0
    && typeof value.default_voice === 'string' && value.default_voice.length > 0;
}

export function isVoiceOutputRuntime(value: unknown): value is VoiceOutputRuntime {
  return isRecord(value)
    && typeof value.url === 'string' && value.url.length > 0
    && typeof value.model === 'string' && value.model.length > 0
    && typeof value.output_format === 'string' && value.output_format.length > 0
    && typeof value.default_voice_id === 'string'
    && value.default_voice_id.length > 0;
}

export function isR2StorageDetail(value: unknown): value is R2StorageDetail {
  return isRecord(value) && hasIdentity(value)
    && typeof value.url === 'string'
    && typeof value.access_key_id === 'string'
    && typeof value.has_secret_key === 'boolean';
}

export function isOpenAiAuth(value: unknown): value is OpenAiAuth {
  return isRecord(value)
    && (value.status === 'signed_out'
      || value.status === 'waiting'
      || value.status === 'connected')
    && (value.user_code === undefined || typeof value.user_code === 'string')
    && (value.verification_url === undefined
      || typeof value.verification_url === 'string')
    && (value.attempt_expires_at === undefined
      || (typeof value.attempt_expires_at === 'number'
        && Number.isSafeInteger(value.attempt_expires_at)))
    && (value.next_poll_delay_ms === undefined
      || (typeof value.next_poll_delay_ms === 'number'
        && Number.isSafeInteger(value.next_poll_delay_ms)))
    && (value.error === undefined || typeof value.error === 'string');
}

export function isSessionListingArray(value: unknown): value is SessionListing[] {
  return Array.isArray(value) && value.every((session) => isRecord(session)
    && typeof session.id === 'string'
    && typeof session.label === 'string'
    && typeof session.live === 'boolean'
    && typeof session.updated_at === 'number'
    && Number.isFinite(session.updated_at));
}

// A snapshot arrives two ways, over this request and over the event stream, and
// both are parsed JSON that the generated types only describe at compile time.
// The check lives here so neither route trusts a shape the other would reject.
export function isSessionLabelResult(value: unknown): value is SessionLabelResult {
  return isRecord(value) && typeof value.id === 'string' && typeof value.label === 'string';
}

export function isCommandResult(value: unknown): value is CommandResult {
  return isRecord(value)
    && typeof value.clear_input === 'boolean'
    && (value.notice === undefined || typeof value.notice === 'string');
}

export function isSessionSnapshot(value: unknown): value is SessionSnapshot {
  return isRecord(value)
    && isRecord(value.forum)
    && typeof value.forum.default_persona_id === 'string'
    && value.forum.default_persona_id.length > 0
    && typeof value.forum.default_persona_display_name === 'string'
    && value.forum.default_persona_display_name.length > 0
    && typeof value.session_id === 'string'
    && typeof value.session_label === 'string'
    && Array.isArray(value.characters)
    && value.characters.every(isCharacterSummary)
    && typeof value.default_character_id === 'string'
    && Array.isArray(value.transcript)
    && value.transcript.every((entry) => isRecord(entry)
      && (entry.has_cached_audio === undefined || typeof entry.has_cached_audio === 'boolean'))
    && (value.covered_until === undefined
      || (typeof value.covered_until === 'number'
        && Number.isSafeInteger(value.covered_until)
        && value.covered_until > 0))
    && isRecord(value.generation)
    && typeof value.lifecycle === 'string';
}

function errorFrom(status: number, payload: unknown): ChaError {
  if (isRecord(payload) && isRecord(payload.error)) {
    const { code, message } = payload.error;
    if (isErrorCode(code) && typeof message === 'string') {
      return new ChaError(status, code, message);
    }
  }
  return new ChaError(
    status,
    'internal_error',
    `CHA returned an invalid error response (${status}).`,
  );
}

async function requestJson<T>(
  fetcher: Fetcher,
  url: string,
  init: RequestInit = {},
): Promise<T> {
  let response: Response;
  try {
    response = await fetcher(url, {
      ...init,
      headers: {
        Accept: 'application/json',
        ...init.headers,
      },
    });
  } catch {
    throw new ChaUnavailableError();
  }

  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    if (!response.ok) throw errorFrom(response.status, undefined);
    throw new ChaProtocolError();
  }

  if (!response.ok) throw errorFrom(response.status, payload);
  return payload as T;
}

async function requestValidated<T>(
  fetcher: Fetcher,
  url: string,
  validate: (value: unknown) => value is T,
  init: RequestInit = {},
): Promise<T> {
  const payload = await requestJson<unknown>(fetcher, url, init);
  if (!validate(payload)) throw new ChaProtocolError();
  return payload;
}

async function requestEmpty(
  fetcher: Fetcher,
  url: string,
  init: RequestInit,
): Promise<void> {
  let response: Response;
  try {
    response = await fetcher(url, {
      ...init,
      headers: { Accept: 'application/json', ...init.headers },
    });
  } catch {
    throw new ChaUnavailableError();
  }
  if (response.ok) return;
  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    payload = undefined;
  }
  throw errorFrom(response.status, payload);
}

async function requestText(fetcher: Fetcher, url: string): Promise<string> {
  let response: Response;
  try {
    response = await fetcher(url, { headers: { Accept: 'text/markdown' } });
  } catch {
    throw new ChaUnavailableError();
  }
  if (response.ok) return response.text();
  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    payload = undefined;
  }
  throw errorFrom(response.status, payload);
}

function component(value: string): string {
  return encodeURIComponent(value);
}

function jsonMutation(body: unknown, method = 'POST'): RequestInit {
  return {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  };
}

function sessionApiUrl(forumId: string, sessionId: string, suffix: string): string {
  return `/s/${component(forumId)}/${component(sessionId)}/api/v1/${suffix}`;
}

export function sessionEventsUrl(forumId: string, sessionId: string): string {
  return sessionApiUrl(forumId, sessionId, 'events');
}

export function createChaClient(
  fetcher: Fetcher = globalThis.fetch.bind(globalThis),
): ChaClient {
  return {
    getBootstrap: () => requestJson<Bootstrap>(fetcher, '/api/v1/bootstrap'),

    getCharacter: (characterId) => requestValidated(
      fetcher,
      `/api/v1/characters/${component(characterId)}`,
      isCharacterDetail,
    ),

    createCharacter: (request) => requestValidated(
      fetcher,
      '/api/v1/characters',
      isCharacterDetail,
      jsonMutation(request),
    ),

    updateCharacter: (characterId, settings) => requestValidated(
      fetcher,
      `/api/v1/characters/${component(characterId)}`,
      isCharacterDetail,
      jsonMutation(settings, 'PATCH'),
    ),

    updateCharacterDefinition: (characterId, update) => requestValidated(
      fetcher,
      `/api/v1/characters/${component(characterId)}/definition`,
      isCharacterDetail,
      jsonMutation(update, 'PATCH'),
    ),

    deleteCharacter: (characterId) => requestEmpty(
      fetcher,
      `/api/v1/characters/${component(characterId)}`,
      jsonMutation({}, 'DELETE'),
    ),

    getCharacterFile: (characterId, filename) => requestValidated(
      fetcher,
      `/api/v1/characters/${component(characterId)}/files/${component(filename)}`,
      isMarkdownFile,
    ),

    createCharacterFile: (characterId, filename, content) => requestValidated(
      fetcher,
      `/api/v1/characters/${component(characterId)}/files`,
      isMarkdownFile,
      jsonMutation({ filename, content }),
    ),

    updateCharacterFile: (characterId, filename, content) => requestValidated(
      fetcher,
      `/api/v1/characters/${component(characterId)}/files/${component(filename)}`,
      isMarkdownFile,
      jsonMutation({ content }, 'PUT'),
    ),

    deleteCharacterFile: (characterId, filename) => requestEmpty(
      fetcher,
      `/api/v1/characters/${component(characterId)}/files/${component(filename)}`,
      jsonMutation({}, 'DELETE'),
    ),

    getPersona: (personaId) => requestValidated(
      fetcher,
      `/api/v1/personas/${component(personaId)}`,
      isPersonaDetail,
    ),

    createPersona: (request) => requestValidated(
      fetcher,
      '/api/v1/personas',
      isPersonaDetail,
      jsonMutation(request),
    ),

    updatePersona: (personaId, update) => requestValidated(
      fetcher,
      `/api/v1/personas/${component(personaId)}`,
      isPersonaDetail,
      jsonMutation(update, 'PATCH'),
    ),

    deletePersona: (personaId) => requestEmpty(
      fetcher,
      `/api/v1/personas/${component(personaId)}`,
      jsonMutation({}, 'DELETE'),
    ),

    getForumFile: (forumId, filename) => requestValidated(
      fetcher,
      `/api/v1/forums/${component(forumId)}/files/${component(filename)}`,
      isMarkdownFile,
    ),

    createForumFile: (forumId, filename, content) => requestValidated(
      fetcher,
      `/api/v1/forums/${component(forumId)}/files`,
      isMarkdownFile,
      jsonMutation({ filename, content }),
    ),

    updateForumFile: (forumId, filename, content) => requestValidated(
      fetcher,
      `/api/v1/forums/${component(forumId)}/files/${component(filename)}`,
      isMarkdownFile,
      jsonMutation({ content }, 'PUT'),
    ),

    deleteForumFile: (forumId, filename) => requestEmpty(
      fetcher,
      `/api/v1/forums/${component(forumId)}/files/${component(filename)}`,
      jsonMutation({}, 'DELETE'),
    ),

    getForum: (forumId) => requestValidated(
      fetcher,
      `/api/v1/forums/${component(forumId)}`,
      isForumDetail,
    ),

    createForum: (request) => requestValidated(
      fetcher,
      '/api/v1/forums',
      isForumDetail,
      jsonMutation(request),
    ),

    updateForum: (forumId, update) => requestValidated(
      fetcher,
      `/api/v1/forums/${component(forumId)}`,
      isForumDetail,
      jsonMutation(update, 'PATCH'),
    ),

    deleteForum: (forumId) => requestEmpty(
      fetcher,
      `/api/v1/forums/${component(forumId)}`,
      jsonMutation({}, 'DELETE'),
    ),

    updateForumMembers: (forumId, update) => requestValidated(
      fetcher,
      `/api/v1/forums/${component(forumId)}/members`,
      isForumDetail,
      jsonMutation(update, 'PUT'),
    ),

    listSessions: (forumId) => requestValidated(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions`,
      isSessionListingArray,
    ),

    createSession: (forumId, label) => requestJson<CreateSessionResult>(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions`,
      jsonMutation({ label }),
    ),

    renameSession: (forumId, sessionId, label) => requestJson<SessionLabelResult>(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}`,
      jsonMutation({ label }, 'PATCH'),
    ),

    deleteSession: (forumId, sessionId) => requestEmpty(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}`,
      jsonMutation({}, 'DELETE'),
    ),

    clearSessionAudioCache: (forumId, sessionId) => requestEmpty(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}/audio-cache`,
      jsonMutation({}, 'DELETE'),
    ),

    downloadSession: (forumId, sessionId) => requestText(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}/download`,
    ),

    openSession: (forumId, sessionId) => requestJson<OpenSessionResult>(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}/open`,
      jsonMutation({}),
    ),

    getSessionSnapshot: async (forumId, sessionId) => {
      const snapshot = await requestJson<unknown>(
        fetcher,
        sessionApiUrl(forumId, sessionId, 'session'),
      );
      if (!isSessionSnapshot(snapshot)) {
        throw new ChaProtocolError();
      }
      return snapshot;
    },

    submitInput: (forumId, sessionId, input) => requestJson<CommandResult>(
      fetcher,
      sessionApiUrl(forumId, sessionId, 'input'),
      jsonMutation(input),
    ),

    coverConversation: (forumId, sessionId, input) => requestJson<CommandResult>(
      fetcher,
      sessionApiUrl(forumId, sessionId, 'actions/cover'),
      jsonMutation(input),
    ),

    uncoverConversation: (forumId, sessionId) => requestJson<CommandResult>(
      fetcher,
      sessionApiUrl(forumId, sessionId, 'actions/uncover'),
      jsonMutation({}),
    ),

    deleteTurn: (forumId, sessionId, input) => requestJson<CommandResult>(
      fetcher,
      sessionApiUrl(forumId, sessionId, 'actions/delete-turn'),
      jsonMutation(input),
    ),

    stopGeneration: (forumId, sessionId) => requestJson<CommandResult>(
      fetcher,
      sessionApiUrl(forumId, sessionId, 'actions/stop'),
      jsonMutation({}),
    ),

    setDefaultCharacter: (forumId, sessionId, characterId) => requestJson<CommandResult>(
      fetcher,
      sessionApiUrl(forumId, sessionId, 'actions/default-character'),
      jsonMutation({ character_id: characterId }),
    ),

    getOpenAiAuth: () => requestJson<OpenAiAuth>(fetcher, '/api/v1/openai/auth'),

    startOpenAiAuth: () => requestJson<OpenAiAuth>(
      fetcher,
      '/api/v1/openai/auth/login',
      jsonMutation({}),
    ),

    pollOpenAiAuth: () => requestJson<OpenAiAuth>(
      fetcher,
      '/api/v1/openai/auth/poll',
      jsonMutation({}),
    ),

    disconnectOpenAiAuth: () => requestJson<OpenAiAuth>(
      fetcher,
      '/api/v1/openai/auth/disconnect',
      jsonMutation({}),
    ),

    listVaults: () => requestValidated(
      fetcher,
      '/api/v1/vaults',
      (value): value is VaultDetail[] => Array.isArray(value)
        && value.every(isVaultDetail),
    ),

    createVault: (request) => requestValidated(
      fetcher,
      '/api/v1/vaults',
      isVaultDetail,
      jsonMutation(request),
    ),

    listR2Vaults: () => requestValidated(
      fetcher,
      '/api/v1/r2-vaults',
      (value): value is string[] => Array.isArray(value)
        && value.every((name) => typeof name === 'string'),
    ),

    downloadR2Vault: (name) => requestValidated(
      fetcher,
      '/api/v1/r2-vaults',
      isVaultDetail,
      jsonMutation({ name }),
    ),

    updateVault: (vaultName, update) => requestValidated(
      fetcher,
      '/api/v1/vaults',
      isVaultDetail,
      jsonMutation({ vault_name: vaultName, ...update }, 'PATCH'),
    ),

    deleteVault: (vaultName) => requestEmpty(
      fetcher,
      '/api/v1/vaults',
      jsonMutation({ vault_name: vaultName }, 'DELETE'),
    ),

    listProviders: () => requestValidated(
      fetcher,
      '/api/v1/providers',
      (value): value is ProviderSummary[] => Array.isArray(value)
        && value.every(isProviderSummary),
    ),

    createProvider: (request) => requestValidated(
      fetcher,
      '/api/v1/providers',
      isProviderDetail,
      jsonMutation(request),
    ),

    getProvider: (providerId) => requestValidated(
      fetcher,
      `/api/v1/providers/${component(providerId)}`,
      isProviderDetail,
    ),

    testProvider: (providerId, candidate) => requestEmpty(
      fetcher,
      `/api/v1/providers/${component(providerId)}/test`,
      jsonMutation(candidate),
    ),

    updateProvider: (providerId, update) => requestValidated(
      fetcher,
      `/api/v1/providers/${component(providerId)}`,
      isProviderDetail,
      jsonMutation(update, 'PATCH'),
    ),

    deleteProvider: (providerId) => requestEmpty(
      fetcher,
      `/api/v1/providers/${component(providerId)}`,
      jsonMutation({}, 'DELETE'),
    ),

    listStyles: () => requestValidated(
      fetcher,
      '/api/v1/styles',
      (value): value is StyleDetail[] => Array.isArray(value)
        && value.every(isStyleDetail),
    ),

    createStyle: (request) => requestValidated(
      fetcher,
      '/api/v1/styles',
      isStyleDetail,
      jsonMutation(request),
    ),

    updateStyle: (styleId, update) => requestValidated(
      fetcher,
      `/api/v1/styles/${component(styleId)}`,
      isStyleDetail,
      jsonMutation(update, 'PATCH'),
    ),

    deleteStyle: (styleId) => requestEmpty(
      fetcher,
      `/api/v1/styles/${component(styleId)}`,
      jsonMutation({}, 'DELETE'),
    ),

    listVoices: () => requestValidated(
      fetcher,
      '/api/v1/voices',
      (value): value is VoiceDetail[] => Array.isArray(value)
        && value.every(isVoiceDetail),
    ),

    createVoice: (request) => requestValidated(
      fetcher,
      '/api/v1/voices',
      isVoiceDetail,
      jsonMutation(request),
    ),

    updateVoice: (voiceId, update) => requestValidated(
      fetcher,
      `/api/v1/voices/${component(voiceId)}`,
      isVoiceDetail,
      jsonMutation(update, 'PATCH'),
    ),

    deleteVoice: (voiceId) => requestEmpty(
      fetcher,
      `/api/v1/voices/${component(voiceId)}`,
      jsonMutation({}, 'DELETE'),
    ),

    getVoiceInputSettings: () => requestValidated(
      fetcher,
      '/api/v1/voice-input',
      (value): value is VoiceInputSettings | null => (
        value === null || isVoiceInputSettings(value)
      ),
    ),

    saveVoiceInputSettings: (settings) => requestValidated(
      fetcher,
      '/api/v1/voice-input',
      isVoiceInputSettings,
      jsonMutation(settings, 'PUT'),
    ),

    getVoiceInputRuntime: () => requestValidated(
      fetcher,
      '/api/v1/voice-input/runtime',
      (value): value is VoiceInputRuntime | null => (
        value === null || isVoiceInputRuntime(value)
      ),
    ),

    getVoiceOutputSettings: () => requestValidated(
      fetcher,
      '/api/v1/voice-output',
      (value): value is VoiceOutputSettings | null => (
        value === null || isVoiceOutputSettings(value)
      ),
    ),

    saveVoiceOutputSettings: (settings) => requestValidated(
      fetcher,
      '/api/v1/voice-output',
      isVoiceOutputSettings,
      jsonMutation(settings, 'PUT'),
    ),

    getVoiceOutputRuntime: () => requestValidated(
      fetcher,
      '/api/v1/voice-output/runtime',
      (value): value is VoiceOutputRuntime | null => (
        value === null || isVoiceOutputRuntime(value)
      ),
    ),

    listApiKeys: () => requestValidated(
      fetcher,
      '/api/v1/api-keys',
      (value): value is ApiKeyDetail[] => Array.isArray(value)
        && value.every(isApiKeyDetail),
    ),

    createApiKey: (request) => requestValidated(
      fetcher,
      '/api/v1/api-keys',
      isApiKeyDetail,
      jsonMutation(request),
    ),

    renameApiKey: (apiKeyId, displayName) => requestValidated(
      fetcher,
      `/api/v1/api-keys/${component(apiKeyId)}`,
      isApiKeyDetail,
      jsonMutation({ display_name: displayName }, 'PATCH'),
    ),

    replaceApiKeyValue: (apiKeyId, value) => requestValidated(
      fetcher,
      `/api/v1/api-keys/${component(apiKeyId)}/value`,
      isApiKeyDetail,
      jsonMutation({ value }, 'PUT'),
    ),

    deleteApiKey: (apiKeyId) => requestEmpty(
      fetcher,
      `/api/v1/api-keys/${component(apiKeyId)}`,
      jsonMutation({}, 'DELETE'),
    ),

    getR2Storage: () => requestValidated(
      fetcher,
      '/api/v1/r2-storage',
      (value): value is R2StorageDetail | null => (
        value === null || isR2StorageDetail(value)
      ),
    ),

    saveR2Storage: (request) => requestValidated(
      fetcher,
      '/api/v1/r2-storage',
      isR2StorageDetail,
      jsonMutation(request, 'PUT'),
    ),

    deleteR2Storage: () => requestEmpty(
      fetcher,
      '/api/v1/r2-storage',
      jsonMutation({}, 'DELETE'),
    ),

    startAudioDownloadBatch: (forumId, sessionId, request) => requestValidated(
      fetcher, `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}/audio-downloads`,
      (value): value is AudioDownloadBatchAcceptance => isRecord(value) && Array.isArray(value.entries) && value.entries.every(isAudioAcceptance),
      jsonMutation(request)),
    startAudioDownload: (forumId, sessionId, entryId, request) => requestValidated(
      fetcher, `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}/entries/${entryId}/audio-download`,
      isAudioAcceptance, jsonMutation(request)),
    getAudioDownloads: (forumId, sessionId, vaultName) => requestValidated(
      fetcher, `/api/v1/forums/${component(forumId)}/sessions/${component(sessionId)}/audio-downloads?vault_name=${encodeURIComponent(vaultName)}`,
      isAudioStatus),
    switchVault: (vaultName, password) => requestEmpty(
      fetcher,
      '/api/v1/vault/switch',
      jsonMutation({ vault_name: vaultName, password: password || null }),
    ),

    mergeVault: (sourceVault, password) => requestEmpty(
      fetcher,
      '/api/v1/vault/merge',
      jsonMutation({ source_vault: sourceVault, password: password || null }),
    ),
  };
}

export const chaClient = createChaClient();
