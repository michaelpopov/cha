import type { components } from './schema';

export type Bootstrap = components['schemas']['Bootstrap'];
export type CharacterDetail = components['schemas']['CharacterDetail'];
export type UpdateCharacterRequest = components['schemas']['UpdateCharacterRequest'];
export type PersonaDetail = components['schemas']['PersonaDetail'];
export type ForumDetail = components['schemas']['ForumDetail'];
export type ForumSummary = components['schemas']['ForumSummary'];
export type CharacterAppearance = components['schemas']['CharacterAppearance'];
export type SessionListing = components['schemas']['SessionListing'];
export type CreateSessionResult = components['schemas']['CreateSessionResult'];
export type SessionLabelResult = components['schemas']['SessionLabelResult'];
export type OpenSessionResult = components['schemas']['OpenSessionResult'];
export type SessionSnapshot = components['schemas']['SessionSnapshot'];
export type CommandResult = components['schemas']['CommandResult'];
export type InputRequest = components['schemas']['InputRequest'];
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
  session_busy: true,
  session_stopping: true,
  session_limit_reached: true,
  session_open_timeout: true,
  server_stopping: true,
  session_not_live: true,
  browser_stream_in_use: true,
  command_timeout: true,
  command_queue_full: true,
  session_delete_conflict: true,
  workspace_reload_failed: true,
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
  updateCharacter(characterId: string, settings: UpdateCharacterRequest): Promise<CharacterDetail>;
  getPersona(personaId: string): Promise<PersonaDetail>;
  getForum(forumId: string): Promise<ForumDetail>;
  listSessions(forumId: string): Promise<SessionListing[]>;
  createSession(forumId: string, label: string): Promise<CreateSessionResult>;
  renameSession(forumId: string, sessionId: string, label: string): Promise<SessionLabelResult>;
  deleteSession(forumId: string, sessionId: string): Promise<void>;
  downloadSession(forumId: string, sessionId: string): Promise<string>;
  openSession(forumId: string, sessionId: string): Promise<OpenSessionResult>;
  getSessionSnapshot(forumId: string, sessionId: string): Promise<SessionSnapshot>;
  submitInput(forumId: string, sessionId: string, input: InputRequest): Promise<CommandResult>;
  stopGeneration(forumId: string, sessionId: string): Promise<CommandResult>;
  setDefaultCharacter(
    forumId: string,
    sessionId: string,
    characterId: string,
  ): Promise<CommandResult>;
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
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
    && typeof value.default_character_id === 'string'
    && Array.isArray(value.transcript)
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

    getCharacter: (characterId) => requestJson<CharacterDetail>(
      fetcher,
      `/api/v1/characters/${component(characterId)}`,
    ),

    updateCharacter: (characterId, settings) => requestJson<CharacterDetail>(
      fetcher,
      `/api/v1/characters/${component(characterId)}`,
      jsonMutation(settings, 'PATCH'),
    ),

    getPersona: (personaId) => requestJson<PersonaDetail>(
      fetcher,
      `/api/v1/personas/${component(personaId)}`,
    ),

    getForum: (forumId) => requestJson<ForumDetail>(
      fetcher,
      `/api/v1/forums/${component(forumId)}`,
    ),

    listSessions: (forumId) => requestJson<SessionListing[]>(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions`,
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
  };
}

export const chaClient = createChaClient();
