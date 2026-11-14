import {
  ChaProtocolError,
  isAudioAcceptance,
  isAudioStatus,
  isVoiceOutputRuntime,
  isNativeVoiceInputRuntime,
  isXaiVoiceStartResult,
  isXaiVoicePieces,
  type NativeVoiceInputRuntime,
  type XaiVoiceStartResult,
  type XaiVoicePieces,
  type AudioDownloadAcceptance,
  type AudioDownloadBatchAcceptance,
  type AudioDownloadStatus,
  type VoiceOutputRuntime,
  isSessionLabelResult,
  isSessionListingArray,
  isSessionSnapshot,
  type Bootstrap,
  type CreateSessionResult,
  type SessionListing,
  type SessionSnapshot,
  type NoteItem,
  type NoteContent,
} from '../api/client';
import { isRecord } from '../api/guards';
import { isNoteItem, isNoteItemList } from '../api/noteGuards';
import { validateBootstrap } from '../state/bootstrap';

const API = '/api/cha/v1';
const TIMEOUT_MS = 60_000;
const FAILED = 'The request failed.';
const TIMED_OUT = 'The request timed out.';
const MAX_CONTEXT_EPOCH = '18446744073709551615';

export class ChaWebError extends Error {
  readonly status: number;
  readonly code: string | undefined;

  constructor(status: number, message: string, code?: string) {
    super(message);
    this.name = 'ChaWebError';
    this.status = status;
    this.code = code;
  }
}

export function chaWebMessage(failure: unknown, fallback: string): string {
  if (failure instanceof ChaWebError || failure instanceof ChaProtocolError) return failure.message;
  return fallback;
}

export type ChaWebNote = { item: NoteItem; document: NoteContent['document'] };
type VaultContext = { daemon_token: string; context_epoch: string };
export type ChaWebBootstrap = Bootstrap & Partial<VaultContext>;

function contextQuery(context: VaultContext): string {
  return new URLSearchParams({
    daemon_token: context.daemon_token,
    context_epoch: context.context_epoch,
  }).toString();
}

export interface ChaWebClient {
  listNoteItems(signal?: AbortSignal): Promise<NoteItem[]>;
  getNote(id: number, signal?: AbortSignal): Promise<ChaWebNote>;
  getVoiceInputRuntime(): Promise<NativeVoiceInputRuntime | null>;
  connectVoiceInput(sdp: string, languages: string[], signal: AbortSignal): Promise<string>;
  startXaiVoiceInput(sessionId: string, languages: string[], signal: AbortSignal): Promise<XaiVoiceStartResult>;
  sendXaiVoiceAudio(sessionId: string, pcmBase64: string, signal: AbortSignal): Promise<XaiVoicePieces>;
  stopXaiVoiceInput(sessionId: string, remainingMs: number, signal: AbortSignal): Promise<XaiVoicePieces>;
  cancelXaiVoiceInput(sessionId: string): Promise<void>;
  getVoiceOutputRuntime(): Promise<VoiceOutputRuntime | null>;
  startAudio(forumId: string, sessionId: string, entryId: number, vaultName: string): Promise<AudioDownloadAcceptance>;
  startAudioBatch(forumId: string, sessionId: string, entryIds: number[], vaultName: string): Promise<AudioDownloadBatchAcceptance>;
  getAudioStatus(forumId: string, sessionId: string): Promise<AudioDownloadStatus>;
  audioUrl(forumId: string, sessionId: string, entryId: number): string;
  getBootstrap(): Promise<ChaWebBootstrap>;
  listSessions(forumId: string): Promise<SessionListing[]>;
  createSession(forumId: string, text: string): Promise<CreateSessionResult>;
  getSession(forumId: string, sessionId: string): Promise<SessionSnapshot>;
  submitInput(forumId: string, sessionId: string, text: string): Promise<void>;
  stopSession(forumId: string, sessionId: string): Promise<void>;
  deleteSession(forumId: string, sessionId: string): Promise<void>;
}

function segment(id: string): string {
  return encodeURIComponent(id);
}

function sessionsPath(forumId: string): string {
  return `${API}/forums/${segment(forumId)}/sessions`;
}

function sessionPath(forumId: string, sessionId: string): string {
  return `${sessionsPath(forumId)}/${segment(sessionId)}`;
}

function audioPath(forumId: string, sessionId: string, entryId: number): string {
  return `${sessionPath(forumId, sessionId)}/entries/${entryId}/audio`;
}

function isJsonContentType(header: string | null): boolean {
  if (!header) return false;
  return header.split(';', 1)[0]?.trim().toLowerCase() === 'application/json';
}

function errorFromPayload(status: number, payload: unknown): ChaWebError {
  if (!isRecord(payload) || !isRecord(payload.error)) return new ChaWebError(status, FAILED);
  const raw = payload.error.message;
  const code = typeof payload.error.code === 'string' ? payload.error.code : undefined;
  if (typeof raw === 'string' && raw.trim() !== '') return new ChaWebError(status, raw, code);
  return new ChaWebError(status, FAILED, code);
}

async function readJson(response: Response): Promise<unknown> {
  if (!isJsonContentType(response.headers.get('content-type'))) {
    throw new ChaWebError(response.status, FAILED);
  }
  try {
    return await response.json();
  } catch (error) {
    if (error instanceof SyntaxError) throw new ChaWebError(response.status, FAILED);
    throw error;
  }
}

async function fetchResponse<T>(
  method: 'GET' | 'POST' | 'DELETE',
  path: string,
  expected: number,
  consume: (response: Response) => Promise<T>,
  body?: unknown,
  signal?: AbortSignal,
): Promise<T> {
  const controller = new AbortController();
  const abort = () => controller.abort();
  signal?.addEventListener('abort', abort, { once: true });
  const timer = setTimeout(() => controller.abort(), TIMEOUT_MS);
  try {
    signal?.throwIfAborted();
    const response = await fetch(path, {
      method,
      redirect: 'error',
      signal: controller.signal,
      headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
    if (response.status !== expected) {
      if (response.ok) throw new ChaProtocolError();
      throw errorFromPayload(response.status, await readJson(response));
    }
    return await consume(response);
  } catch (error) {
    if (signal?.aborted) throw new DOMException('The operation was aborted.', 'AbortError');
    if (controller.signal.aborted || (error instanceof DOMException && error.name === 'AbortError')) {
      throw new ChaWebError(0, TIMED_OUT);
    }
    if (error instanceof ChaWebError || error instanceof ChaProtocolError) throw error;
    throw new ChaWebError(0, FAILED);
  } finally {
    clearTimeout(timer);
    signal?.removeEventListener('abort', abort);
  }
}

async function readGuarded<T>(
  response: Response,
  guard: (value: unknown) => value is T,
): Promise<T> {
  const payload = await readJson(response);
  if (!guard(payload)) throw new ChaProtocolError();
  return payload;
}

export function createChaWebClient(onContextLost = () => {}): ChaWebClient {
  let context: VaultContext | undefined;
  let contextError: ChaWebError | undefined;

  function loseContext(error: ChaWebError): never {
    if (!contextError) {
      contextError = error;
      onContextLost();
    }
    throw contextError;
  }

  function withContext(path: string): string {
    if (contextError) throw contextError;
    if (!context) throw new ChaProtocolError();
    return `${path}?${contextQuery(context)}`;
  }

  async function exchange<T>(
    method: 'GET' | 'POST' | 'DELETE',
    path: string,
    expected: number,
    consume: (response: Response) => Promise<T>,
    body?: unknown,
    signal?: AbortSignal,
  ): Promise<T> {
    if (contextError) throw contextError;
    try {
      const result = await fetchResponse(method,
        path === `${API}/bootstrap` ? path : withContext(path), expected, consume, body, signal);
      if (contextError) throw contextError;
      return result;
    } catch (error) {
      if (contextError) throw contextError;
      if (context && error instanceof ChaWebError
        && (error.code === 'vault_changed' || error.code === 'application_unavailable')) {
        loseContext(error);
      }
      throw error;
    }
  }

  return {
    audioUrl: (forumId, sessionId, entryId) => withContext(audioPath(forumId, sessionId, entryId)),
    async listNoteItems(signal) {
      return (await exchange('GET', `${API}/notes`, 200,
        (response) => readGuarded(response, isNoteItemList), undefined, signal)).items;
    },

    getNote(id, signal) {
      if (!Number.isSafeInteger(id) || id < 1) return Promise.reject(new ChaProtocolError());
      return exchange('GET', `${API}/notes/${id}`, 200,
        (response) => readGuarded(response, (value): value is ChaWebNote => (
          isRecord(value) && Object.keys(value).length === 2 && isNoteItem(value.item)
          && value.item.id === id && value.item.kind !== 'folder' && isRecord(value.document)
        )), undefined, signal);
    },
    async getVoiceInputRuntime() {
      return exchange('GET', `${API}/voice-input`, 200,
        (response) => readGuarded(response,
          (value): value is NativeVoiceInputRuntime | null => value === null || isNativeVoiceInputRuntime(value)));
    },

    async connectVoiceInput(sdp, languages, signal) {
      const result = await exchange('POST', `${API}/voice-input/connect`, 200,
        (response) => readGuarded(response,
          (value): value is { sdp: string } => isRecord(value) && typeof value.sdp === 'string' && value.sdp.length > 0),
        { sdp, languages }, signal);
      return result.sdp;
    },

    async startXaiVoiceInput(sessionId, languages, signal) {
      return exchange('POST', `${API}/voice-input/xai/start`, 200,
        (response) => readGuarded(response,
          (value): value is XaiVoiceStartResult => isXaiVoiceStartResult(value) && value.session_id === sessionId),
        { session_id: sessionId, languages }, signal);
    },

    async sendXaiVoiceAudio(sessionId, pcmBase64, signal) {
      return exchange('POST', `${API}/voice-input/xai/audio`, 200,
        (response) => readGuarded(response,
          (value): value is XaiVoicePieces => isXaiVoicePieces(value) && value.session_id === sessionId),
        { session_id: sessionId, pcm_base64: pcmBase64 }, signal);
    },

    async stopXaiVoiceInput(sessionId, remainingMs, signal) {
      return exchange('POST', `${API}/voice-input/xai/stop`, 200,
        (response) => readGuarded(response,
          (value): value is XaiVoicePieces => isXaiVoicePieces(value) && value.session_id === sessionId),
        { session_id: sessionId, remaining_ms: remainingMs }, signal);
    },

    async cancelXaiVoiceInput(sessionId) {
      await exchange('POST', `${API}/voice-input/xai/cancel`, 200,
        async () => undefined, { session_id: sessionId });
    },
    async getVoiceOutputRuntime() {
      return exchange('GET', `${API}/voice-output`, 200,
        (response) => readGuarded(response,
          (value): value is VoiceOutputRuntime | null => value === null || isVoiceOutputRuntime(value)));
    },

    async startAudio(forumId, sessionId, entryId, vaultName) {
      return exchange('POST', audioPath(forumId, sessionId, entryId), 200,
        (response) => readGuarded(response,
          (value): value is AudioDownloadAcceptance => isAudioAcceptance(value) && value.entry_id === entryId),
        { vault_name: vaultName });
    },

    async getAudioStatus(forumId, sessionId) {
      return exchange('GET', `${sessionPath(forumId, sessionId)}/audio`, 200,
        (response) => readGuarded(response, isAudioStatus));
    },

    async startAudioBatch(forumId, sessionId, entryIds, vaultName) {
      return exchange('POST', `${sessionPath(forumId, sessionId)}/audio`, 200,
        (response) => readGuarded(response,
          (value): value is AudioDownloadBatchAcceptance => isRecord(value)
            && Array.isArray(value.entries) && value.entries.length === entryIds.length
            && value.entries.every((item, index) => isAudioAcceptance(item) && item.entry_id === entryIds[index])),
        { vault_name: vaultName, entry_ids: entryIds });
    },

    async getBootstrap() {
      const loaded = await exchange('GET', `${API}/bootstrap`, 200, async (response) => {
        const payload = await readJson(response);
        if (!isRecord(payload) || typeof payload.daemon_token !== 'string'
          || !/^[0-9a-f]{32}$/.test(payload.daemon_token)
          || typeof payload.context_epoch !== 'string' || !/^[1-9]\d{0,19}$/.test(payload.context_epoch)
          || (payload.context_epoch.length === MAX_CONTEXT_EPOCH.length
            && payload.context_epoch > MAX_CONTEXT_EPOCH)) {
          throw new ChaProtocolError();
        }
        try {
          return {
            ...validateBootstrap(payload),
            daemon_token: payload.daemon_token,
            context_epoch: payload.context_epoch,
          };
        } catch {
          throw new ChaProtocolError();
        }
      });
      if (contextError) throw contextError;
      if (context && (loaded.daemon_token !== context.daemon_token
        || loaded.context_epoch !== context.context_epoch)) {
        loseContext(new ChaWebError(409, 'The vault context has changed.', 'vault_changed'));
      }
      context = { daemon_token: loaded.daemon_token, context_epoch: loaded.context_epoch };
      return loaded;
    },

    async listSessions(forumId) {
      return exchange('GET', sessionsPath(forumId), 200,
        (response) => readGuarded(response, isSessionListingArray));
    },

    async createSession(forumId, text) {
      return exchange('POST', sessionsPath(forumId), 201,
        (response) => readGuarded(response, isSessionLabelResult), { text });
    },

    async getSession(forumId, sessionId) {
      return exchange('GET', sessionPath(forumId, sessionId), 200,
        (response) => readGuarded(response, isSessionSnapshot));
    },

    async submitInput(forumId, sessionId, text) {
      await exchange('POST', `${sessionPath(forumId, sessionId)}/input`, 204,
        async () => undefined, { text });
    },

    async stopSession(forumId, sessionId) {
      await exchange('POST', `${sessionPath(forumId, sessionId)}/stop`, 204,
        async () => undefined, {});
    },

    async deleteSession(forumId, sessionId) {
      await exchange('DELETE', sessionPath(forumId, sessionId), 204,
        async () => undefined);
    },
  };
}
