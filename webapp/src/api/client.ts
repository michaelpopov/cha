import type { components } from './schema';
import { hasIdentity, isRecord } from './guards';

export type Bootstrap = components['schemas']['Bootstrap'];
export type CharacterDetail = components['schemas']['CharacterDetail'];
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
export type VaultUpdate = Omit<components['schemas']['UpdateVaultRequest'], 'vault_name'>;
export type ProviderSummary = components['schemas']['ProviderSummary'];
export type ProviderDetail = components['schemas']['ProviderDetail'];
export type CreateProviderRequest = components['schemas']['CreateProviderRequest'];
export type ProviderUpdate = components['schemas']['ProviderUpdate'];
export type StyleDetail = components['schemas']['StyleDetail'];
export type CreateStyleRequest = components['schemas']['CreateStyleRequest'];
export type StyleUpdate = components['schemas']['StyleUpdate'];
export type ApiKeyDetail = components['schemas']['ApiKeyDetail'];
export type CreateApiKeyRequest = components['schemas']['CreateApiKeyRequest'];
export type ErrorCode = components['schemas']['ErrorResponse']['error']['code'];

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
  session_stopping: true,
  session_limit_reached: true,
  session_open_timeout: true,
  server_stopping: true,
  session_not_live: true,
  command_timeout: true,
  command_queue_full: true,
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
  getPersona(personaId: string): Promise<PersonaDetail>;
  createPersona(request: CreatePersonaRequest): Promise<PersonaDetail>;
  updatePersona(personaId: string, update: UpdatePersonaRequest): Promise<PersonaDetail>;
  deletePersona(personaId: string): Promise<void>;
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
  listApiKeys(): Promise<ApiKeyDetail[]>;
  createApiKey(request: CreateApiKeyRequest): Promise<ApiKeyDetail>;
  renameApiKey(apiKeyId: string, displayName: string): Promise<ApiKeyDetail>;
  replaceApiKeyValue(apiKeyId: string, value: string): Promise<ApiKeyDetail>;
  deleteApiKey(apiKeyId: string): Promise<void>;
  switchVault(vaultName: string): Promise<void>;
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
    && isOptionalBoundedNumber(value.settings.stability, 0, 1)
    && isOptionalBoundedNumber(value.settings.similarity_boost, 0, 1)
    && isOptionalBoundedNumber(value.settings.style, 0, 1)
    && (value.settings.use_speaker_boost === undefined
      || typeof value.settings.use_speaker_boost === 'boolean')
    && isOptionalBoundedNumber(value.settings.speed, 0.7, 1.2);
}

function isCharacterSummary(value: unknown): boolean {
  return isRecord(value)
    && hasIdentity(value)
    && isCharacterAppearance(value.appearance)
    && (value.voice === undefined || isSpeechVoice(value.voice));
}

function isCharacterDetail(value: unknown): value is CharacterDetail {
  return isCharacterSummary(value)
    && isRecord(value)
    && typeof value.character_markdown === 'string'
    && typeof value.editable_markdown === 'string'
    && (value.provider === null || typeof value.provider === 'string')
    && (value.style === null || typeof value.style === 'string')
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
    && typeof value.writable === 'boolean';
}

function isPersonaDetail(value: unknown): value is PersonaDetail {
  return isRecord(value)
    && hasIdentity(value)
    && typeof value.persona_markdown === 'string'
    && typeof value.writable === 'boolean';
}

function isForumDetail(value: unknown): value is ForumDetail {
  return isRecord(value)
    && hasIdentity(value)
    && typeof value.default_character_id === 'string'
    && typeof value.default_persona_id === 'string'
    && typeof value.default_persona_display_name === 'string'
    && Array.isArray(value.members)
    && value.members.every(isCharacterSummary)
    && typeof value.forum_markdown === 'string'
    && typeof value.writable === 'boolean';
}

function isProviderSummary(value: unknown): value is ProviderSummary {
  return isRecord(value) && hasIdentity(value)
    && typeof value.model === 'string' && typeof value.host === 'string';
}

function isVaultDetail(value: unknown): value is VaultDetail {
  return isRecord(value)
    && typeof value.display_name === 'string'
    && typeof value.data_path === 'string'
    && (value.mirror_path === null || typeof value.mirror_path === 'string')
    && (value.modify_path === null || typeof value.modify_path === 'string')
    && typeof value.active === 'boolean'
    && typeof value.can_delete === 'boolean';
}

function isProviderDetail(value: unknown): value is ProviderDetail {
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

function isStyleDetail(value: unknown): value is StyleDetail {
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

function isApiKeyDetail(value: unknown): value is ApiKeyDetail {
  return isRecord(value) && hasIdentity(value)
    && typeof value.has_value === 'boolean'
    && Array.isArray(value.used_by)
    && value.used_by.every((name) => typeof name === 'string');
}

function isSessionListingArray(value: unknown): value is SessionListing[] {
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

    switchVault: (vaultName) => requestEmpty(
      fetcher,
      '/api/v1/vault/switch',
      jsonMutation({ vault_name: vaultName }),
    ),
  };
}

export const chaClient = createChaClient();
