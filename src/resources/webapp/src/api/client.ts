import type { components } from './schema';

export type Bootstrap = components['schemas']['Bootstrap'];
export type CharacterDetail = components['schemas']['CharacterDetail'];
export type SessionListing = components['schemas']['SessionListing'];
export type CreateSessionResult = components['schemas']['CreateSessionResult'];
export type OpenSessionResult = components['schemas']['OpenSessionResult'];
export type SessionSnapshot = components['schemas']['SessionSnapshot'];
export type CommandResult = components['schemas']['CommandResult'];
export type InputRequest = components['schemas']['InputRequest'];
export type ErrorCode = components['schemas']['ErrorResponse']['error']['code'];

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

export interface ChaClient {
  getBootstrap(): Promise<Bootstrap>;
  getCharacter(characterId: string): Promise<CharacterDetail>;
  listSessions(forumId: string): Promise<SessionListing[]>;
  createSession(forumId: string, label: string): Promise<CreateSessionResult>;
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
    if (typeof code === 'string' && typeof message === 'string') {
      return new ChaError(status, code as ErrorCode, message);
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
  const response = await fetcher(url, {
    ...init,
    headers: {
      Accept: 'application/json',
      ...init.headers,
    },
  });

  let payload: unknown;
  try {
    payload = await response.json();
  } catch {
    if (!response.ok) throw errorFrom(response.status, undefined);
    throw new Error(`CHA returned invalid JSON for ${url}.`);
  }

  if (!response.ok) throw errorFrom(response.status, payload);
  return payload as T;
}

function component(value: string): string {
  return encodeURIComponent(value);
}

function jsonMutation(body: unknown): RequestInit {
  return {
    method: 'POST',
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

    listSessions: (forumId) => requestJson<SessionListing[]>(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions`,
    ),

    createSession: (forumId, label) => requestJson<CreateSessionResult>(
      fetcher,
      `/api/v1/forums/${component(forumId)}/sessions`,
      jsonMutation({ label }),
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
        throw new TypeError('CHA returned an incompatible session snapshot.');
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
      sessionApiUrl(forumId, sessionId, 'actions/default-agent'),
      jsonMutation({ character_id: characterId }),
    ),
  };
}

export const chaClient = createChaClient();
