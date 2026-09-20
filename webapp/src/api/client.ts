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
export type NativeVoiceInputRuntime = components['schemas']['VoiceInputRuntime'];
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

export class ChaError extends Error {
  constructor(
    readonly code: ErrorCode,
    message: string,
  ) {
    super(message);
    this.name = 'ChaError';
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
  getVoiceInputRuntime(): Promise<NativeVoiceInputRuntime | null>;
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
  resolveAudioSource(
    forumId: string,
    sessionId: string,
    entryId: number,
    vaultName: string,
  ): Promise<MediaResource>;
  previewSpeech(
    text: string,
    referenceId: string | undefined,
    settings: { speed?: number } | undefined,
    signal?: AbortSignal,
  ): Promise<MediaResource>;
  releaseResource(resourceId: string): Promise<void>;
  connectVoiceInput(
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

function isRosterSummary(value: unknown): boolean {
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
  return isRosterSummary(value)
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
  return isRosterSummary(value)
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

function isForumSummary(value: unknown): boolean {
  return isRecord(value)
    && hasIdentity(value)
    && typeof value.default_character_id === 'string'
    && value.default_character_id.length > 0
    && typeof value.default_persona_id === 'string'
    && value.default_persona_id.length > 0
    && typeof value.default_persona_display_name === 'string'
    && Array.isArray(value.members)
    && value.members.every(isRosterSummary);
}

export function isForumDetail(value: unknown): value is ForumDetail {
  return isForumSummary(value)
    && isRecord(value)
    && typeof value.forum_markdown === 'string'
    && Array.isArray(value.markdown_files)
    && value.markdown_files.every((filename) => typeof filename === 'string')
    && typeof value.writable === 'boolean';
}

export function isProviderSummary(value: unknown): value is ProviderSummary {
  return isRecord(value) && hasIdentity(value)
    && typeof value.model === 'string' && typeof value.host === 'string';
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
    && isCharacterAppearance(value)
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

export function isNativeVoiceInputRuntime(value: unknown): value is NativeVoiceInputRuntime {
  return isRecord(value)
    && typeof value.url === 'string' && value.url.length > 0
    && typeof value.model === 'string' && value.model.length > 0
    && (value.delay === 'low' || value.delay === 'medium'
      || value.delay === 'high' || value.delay === 'xhigh')
    && typeof value.prompt === 'string'
    && !('api_key' in value);
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

export function isSessionLabelResult(value: unknown): value is SessionLabelResult {
  return isRecord(value) && typeof value.id === 'string' && typeof value.label === 'string';
}

export function isCommandResult(value: unknown): value is CommandResult {
  return isRecord(value)
    && typeof value.clear_input === 'boolean'
    && (value.notice === undefined || typeof value.notice === 'string');
}

function isUnsignedInteger(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 0;
}

function isTranscriptEntry(value: unknown): boolean {
  return isRecord(value)
    && isUnsignedInteger(value.id)
    && isOneOf(value.kind, ['human', 'character', 'notice', 'error'])
    && typeof value.participant_id === 'string'
    && typeof value.display_name === 'string'
    && typeof value.addressed_to === 'string'
    && typeof value.addressed_to_name === 'string'
    && typeof value.text === 'string'
    && isOneOf(value.status, ['complete', 'streaming', 'cancelled', 'failed'])
    && (value.request_id === undefined || isUnsignedInteger(value.request_id))
    && (value.created_at === null || Number.isSafeInteger(value.created_at))
    && (value.has_cached_audio === undefined || typeof value.has_cached_audio === 'boolean');
}

function isGenerationState(value: unknown): boolean {
  return isRecord(value)
    && typeof value.active === 'boolean'
    && (value.request_id === undefined || isUnsignedInteger(value.request_id))
    && typeof value.character_id === 'string'
    && typeof value.character_display_name === 'string'
    && isOneOf(value.phase, ['waiting', 'reasoning', 'answering', 'stopping'])
    && typeof value.reasoning_text === 'string';
}

// Requests and events share this check before publishing parsed JSON to React.
export function isSessionSnapshot(value: unknown): value is SessionSnapshot {
  return isRecord(value)
    && isRecord(value.forum)
    && isForumSummary(value.forum)
    && typeof value.forum.default_persona_display_name === 'string'
    && value.forum.default_persona_display_name.length > 0
    && typeof value.session_id === 'string' && value.session_id.length > 0
    && typeof value.session_label === 'string'
    && Array.isArray(value.characters)
    && value.characters.every(isRosterSummary)
    && typeof value.default_character_id === 'string' && value.default_character_id.length > 0
    && Array.isArray(value.transcript)
    && value.transcript.every(isTranscriptEntry)
    && (value.covered_until === undefined
      || (isUnsignedInteger(value.covered_until) && value.covered_until > 0))
    && isGenerationState(value.generation)
    && (value.notice === undefined || typeof value.notice === 'string')
    && isOneOf(value.lifecycle, ['starting', 'running', 'stopping'])
    && (value.shutdown_reason === undefined || isOneOf(value.shutdown_reason, [
      'session_closed', 'reloading', 'session_failed', 'session_deleted', 'server_stopping',
    ]));
}
