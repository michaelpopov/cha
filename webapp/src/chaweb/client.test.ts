import { afterEach, describe, expect, it, vi } from 'vitest';

import { ChaProtocolError } from '../api/client';
import { bootstrapFixture, snapshotFixture, voiceOutputRuntimeFixture } from '../test/fixtures';
import { audioUrl, createChaWebClient } from './client';

afterEach(() => {
  vi.unstubAllGlobals();
  vi.useRealTimers();
});

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json; charset=utf-8' },
  });
}

function installFetch(handler: (url: string, init?: RequestInit) => Promise<Response> | Response) {
  const calls: Array<{ url: string; init?: RequestInit }> = [];
  vi.stubGlobal('fetch', (input: RequestInfo | URL, init?: RequestInit) => {
    const url = String(input);
    calls.push({ url, init });
    return handler(url, init);
  });
  return calls;
}

describe('ChaWeb HTTP client', () => {
  it('connects voice input and sends xAI audio through the same origin without provider keys', async () => {
    const calls = installFetch((url) => {
      if (url.endsWith('/voice-input')) return jsonResponse({
        provider: 'openai', url: 'https://provider.test/realtime', model: 'test',
        delay: 'low', prompt: '', send_phrase: 'over to you',
      });
      if (url.endsWith('/connect')) return jsonResponse({ sdp: 'answer' });
      if (url.endsWith('/start')) return jsonResponse({ session_id: 'dictation-1', stop_budget_ms: 20000 });
      if (url.endsWith('/cancel')) return jsonResponse({});
      return jsonResponse({ session_id: 'dictation-1', pieces: ['Hello'], preview: '' });
    });
    const client = createChaWebClient();
    const signal = new AbortController().signal;
    await expect(client.getVoiceInputRuntime()).resolves.toMatchObject({ provider: 'openai' });
    await expect(client.connectVoiceInput('offer', ['en'], signal)).resolves.toBe('answer');
    await client.startXaiVoiceInput('dictation-1', [], signal);
    await expect(client.sendXaiVoiceAudio('dictation-1', 'AAAA', signal)).resolves.toMatchObject({ pieces: ['Hello'] });
    await client.stopXaiVoiceInput('dictation-1', 1000, signal);
    await client.cancelXaiVoiceInput('dictation-1');
    expect(calls.map(({ url }) => url)).toEqual([
      '/api/cha/v1/voice-input', '/api/cha/v1/voice-input/connect',
      '/api/cha/v1/voice-input/xai/start', '/api/cha/v1/voice-input/xai/audio',
      '/api/cha/v1/voice-input/xai/stop', '/api/cha/v1/voice-input/xai/cancel',
    ]);
    expect(calls[1].init?.body).toBe(JSON.stringify({ sdp: 'offer', languages: ['en'] }));
    expect(calls[3].init?.body).toBe(JSON.stringify({ session_id: 'dictation-1', pcm_base64: 'AAAA' }));
    expect(calls.every(({ init }) => !new Headers(init?.headers).has('Authorization'))).toBe(true);
  });

  it('rejects a mismatched dictation and preserves explicit cancellation', async () => {
    installFetch(() => jsonResponse({ session_id: 'other', pieces: [] }));
    const controller = new AbortController();
    await expect(createChaWebClient().sendXaiVoiceAudio('one', 'AAAA', controller.signal))
      .rejects.toBeInstanceOf(ChaProtocolError);
    installFetch((_url, init) => new Promise((_resolve, reject) => {
      init?.signal?.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
    }));
    const pending = createChaWebClient().connectVoiceInput('offer', [], controller.signal);
    controller.abort();
    await expect(pending).rejects.toMatchObject({ name: 'AbortError' });
  });
  it('admits a session audio batch and validates each returned entry in order', async () => {
    const accepted = { entries: [{ entry_id: 7, cached: false, state: 'queued' }, { entry_id: 8, cached: true }] };
    const calls = installFetch(() => jsonResponse(accepted));
    await expect(createChaWebClient().startAudioBatch('a/b', 'c d', [7, 8], 'Personal')).resolves.toEqual(accepted);
    expect(calls[0]).toMatchObject({
      url: '/api/cha/v1/forums/a%2Fb/sessions/c%20d/audio',
      init: { method: 'POST', body: JSON.stringify({ vault_name: 'Personal', entry_ids: [7, 8] }) },
    });
    for (const entries of [[], [...accepted.entries].reverse(), [{ entry_id: 7, cached: false, state: 'failed' }, accepted.entries[1]]]) {
      installFetch(() => jsonResponse({ entries }));
      await expect(createChaWebClient().startAudioBatch('lobby', 'planning', [7, 8], 'Personal')).rejects.toBeInstanceOf(ChaProtocolError);
    }
  });

  it('clears audio only for the named session and vault without retrying a failure', async () => {
    const calls = installFetch(() => new Response(null, { status: 204 }));
    await expect(createChaWebClient().clearAudio('a/b', 'c d', 'Personal')).resolves.toBeUndefined();
    expect(calls).toHaveLength(1);
    expect(calls[0]).toMatchObject({
      url: '/api/cha/v1/forums/a%2Fb/sessions/c%20d/audio',
      init: { method: 'DELETE', body: JSON.stringify({ vault_name: 'Personal' }),
        headers: { 'Content-Type': 'application/json' } },
    });
    const failed = installFetch(() => jsonResponse({
      error: { code: 'vault_changed', message: 'The active vault changed.' },
    }, 409));
    await expect(createChaWebClient().clearAudio('lobby', 'planning', 'Personal'))
      .rejects.toMatchObject({ status: 409, code: 'vault_changed' });
    expect(failed).toHaveLength(1);
  });

  it('loads voice settings and audio jobs and requests speech by stored entry ID', async () => {
    const calls = installFetch((url, init) => {
      if (url.endsWith('/voice-output')) return jsonResponse(voiceOutputRuntimeFixture);
      if (init?.method === 'POST') return jsonResponse({ entry_id: 7, cached: false, state: 'queued' });
      return jsonResponse({ cached_entry_ids: [2], downloads: [{ entry_id: 7, state: 'running' }] });
    });
    const client = createChaWebClient();
    await expect(client.getVoiceOutputRuntime()).resolves.toEqual(voiceOutputRuntimeFixture);
    await expect(client.startAudio('a/b', 'c d', 7, 'Personal')).resolves.toMatchObject({ state: 'queued' });
    await expect(client.getAudioStatus('a/b', 'c d')).resolves.toMatchObject({ cached_entry_ids: [2] });
    expect(calls.map(({ url }) => url)).toEqual([
      '/api/cha/v1/voice-output', audioUrl('a/b', 'c d', 7), '/api/cha/v1/forums/a%2Fb/sessions/c%20d/audio',
    ]);
    expect(calls[1]?.init?.body).toBe(JSON.stringify({ vault_name: 'Personal' }));
  });

  it('accepts missing voice configuration and rejects mismatched audio acceptance and malformed jobs', async () => {
    installFetch(() => jsonResponse(null));
    await expect(createChaWebClient().getVoiceOutputRuntime()).resolves.toBeNull();
    installFetch(() => jsonResponse({ entry_id: 8, cached: true }));
    await expect(createChaWebClient().startAudio('lobby', 'planning', 7, 'Personal'))
      .rejects.toBeInstanceOf(ChaProtocolError);
    installFetch(() => jsonResponse({ cached_entry_ids: [], downloads: [{ entry_id: 7, state: 'unknown' }] }));
    await expect(createChaWebClient().getAudioStatus('lobby', 'planning')).rejects.toBeInstanceOf(ChaProtocolError);
  });

  it('deletes the specified session without a body and reports failures without retrying', async () => {
    const calls = installFetch(() => new Response(null, { status: 204 }));
    const client = createChaWebClient();
    await expect(client.deleteSession('a/b', 'c d')).resolves.toBeUndefined();
    expect(calls).toHaveLength(1);
    expect(calls[0]).toMatchObject({
      url: '/api/cha/v1/forums/a%2Fb/sessions/c%20d',
      init: { method: 'DELETE', redirect: 'error' },
    });
    expect(calls[0]?.init?.body).toBeUndefined();
    const failed = installFetch(() => jsonResponse({
      error: { code: 'session_stopping', message: 'The session is still stopping.' },
    }, 500));
    await expect(client.deleteSession('lobby', 'planning')).rejects.toMatchObject({
      status: 500, code: 'session_stopping', message: 'The session is still stopping.',
    });
    expect(failed).toHaveLength(1);
  });

  it('loads bootstrap from a relative URL and keeps extra fields', async () => {
    const calls = installFetch(() => jsonResponse({ ...bootstrapFixture, future: true }));
    const bootstrap = await createChaWebClient().getBootstrap();
    expect(bootstrap.vault_name).toBe('Personal');
    expect(bootstrap.entrance_forum_id).toBe('entrance');
    expect(calls).toHaveLength(1);
    expect(calls[0]?.url).toBe('/api/cha/v1/bootstrap');
    expect(calls[0]?.init?.method).toBe('GET');
    expect(calls[0]?.init?.redirect).toBe('error');
    expect(calls[0]?.init?.credentials).toBeUndefined();
    expect(calls[0]?.init?.headers).toBeUndefined();
    expect(calls[0]?.init?.body).toBeUndefined();
    expect(calls[0]?.init?.signal).toBeInstanceOf(AbortSignal);
  });

  it('lists sessions, creates one, reads a snapshot, and posts input and stop', async () => {
    const calls = installFetch((url, init) => {
      if (init?.method === 'POST' && url.endsWith('/sessions')) {
        return jsonResponse({ id: 'planning', label: 'Planning', future: true }, 201);
      }
      if (init?.method === 'POST') return new Response(null, { status: 204 });
      if (url.endsWith('/sessions')) {
        return jsonResponse([{
          id: 'planning', label: 'Planning', live: false, updated_at: 4, future: true,
        }]);
      }
      return jsonResponse({ ...snapshotFixture, future: true });
    });
    const client = createChaWebClient();

    const listed = await client.listSessions('a/b');
    expect(listed.map((session) => session.id)).toEqual(['planning']);
    const created = await client.createSession('lobby', 'hello');
    expect(created).toMatchObject({ id: 'planning', label: 'Planning' });
    const snapshot = await client.getSession('lobby', 'c d');
    expect(snapshot.session_id).toBe(snapshotFixture.session_id);
    await expect(client.submitInput('lobby', 'planning', 'next')).resolves.toBeUndefined();
    await expect(client.stopSession('lobby', 'planning')).resolves.toBeUndefined();

    expect(calls.map((call) => [call.init?.method, call.url, call.init?.body])).toEqual([
      ['GET', '/api/cha/v1/forums/a%2Fb/sessions', undefined],
      ['POST', '/api/cha/v1/forums/lobby/sessions', JSON.stringify({ text: 'hello' })],
      ['GET', '/api/cha/v1/forums/lobby/sessions/c%20d', undefined],
      ['POST', '/api/cha/v1/forums/lobby/sessions/planning/input', JSON.stringify({ text: 'next' })],
      ['POST', '/api/cha/v1/forums/lobby/sessions/planning/stop', JSON.stringify({})],
    ]);
    for (const call of calls.slice(1)) {
      if (call.init?.method !== 'POST') continue;
      expect(call.init.headers).toEqual({ 'Content-Type': 'application/json' });
      expect(call.init.redirect).toBe('error');
      expect(JSON.stringify(call.init.headers)).not.toContain('Authorization');
    }
  });

  it('exposes status and the safe JSON message for 404 and 422', async () => {
    installFetch((url) => jsonResponse(
      url.endsWith('/missing')
        ? { error: { code: 'not_found', message: 'Session not found.' }, extra: true }
        : { error: { code: 'invalid_argument', message: 'Unknown command' } },
      url.endsWith('/missing') ? 404 : 422,
    ));
    const client = createChaWebClient();
    await expect(client.getSession('lobby', 'missing')).rejects.toMatchObject({
      status: 404,
      code: 'not_found',
      message: 'Session not found.',
    });
    await expect(client.submitInput('lobby', 'planning', 'nope')).rejects.toMatchObject({
      status: 422,
      code: 'invalid_argument',
      message: 'Unknown command',
    });
  });

  it('uses a generic message for non-JSON failures and does not retry writes', async () => {
    let calls = 0;
    installFetch(() => {
      calls += 1;
      return new Response('<html><body>bad gateway</body></html>', {
        status: 502,
        headers: { 'Content-Type': 'text/html' },
      });
    });
    const error = await createChaWebClient().createSession('lobby', 'hello').then(
      () => { throw new Error('create should fail'); },
      (failure: unknown) => failure,
    );
    expect(error).toMatchObject({ status: 502, message: 'The request failed.' });
    expect(String(error)).not.toContain('bad gateway');
    expect(String(error)).not.toContain('<html>');
    expect(calls).toBe(1);
  });

  it('hides transport exceptions and does not retry them', async () => {
    let calls = 0;
    installFetch(() => {
      calls += 1;
      throw new TypeError('socket hang up');
    });
    await expect(clientSubmit()).rejects.toMatchObject({
      status: 0,
      message: 'The request failed.',
    });
    expect(calls).toBe(1);
  });

  it('rejects a success that is not JSON or not the expected record', async () => {
    installFetch(() => new Response('<html></html>', {
      status: 200,
      headers: { 'Content-Type': 'text/html' },
    }));
    await expect(createChaWebClient().getBootstrap()).rejects.toMatchObject({
      status: 200,
      message: 'The request failed.',
    });

    installFetch(() => jsonResponse({ nope: true }));
    await expect(createChaWebClient().listSessions('lobby')).rejects.toBeInstanceOf(ChaProtocolError);
  });

  it('times out once and clears the timer', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    let calls = 0;
    installFetch((_url, init) => {
      calls += 1;
      return new Promise((_resolve, reject) => {
        init?.signal?.addEventListener('abort', () => {
          reject(new DOMException('The operation was aborted.', 'AbortError'));
        });
      });
    });
    const pending = createChaWebClient().stopSession('lobby', 'planning');
    const rejected = expect(pending).rejects.toMatchObject({
      status: 0,
      message: 'The request timed out.',
    });
    await vi.advanceTimersByTimeAsync(60_000);
    await rejected;
    expect(calls).toBe(1);
    expect(vi.getTimerCount()).toBe(0);
  });

  it('clears the timeout when a request finishes', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    installFetch(() => jsonResponse(bootstrapFixture));
    await createChaWebClient().getBootstrap();
    expect(vi.getTimerCount()).toBe(0);
  });

  it('keeps the timeout active while reading a response body', async () => {
    vi.useFakeTimers();
    installFetch((_url, init) => new Response(new ReadableStream({
      start(controller) {
        init?.signal?.addEventListener('abort', () => {
          controller.error(new DOMException('Aborted', 'AbortError'));
        });
      },
    }), { status: 201, headers: { 'Content-Type': 'application/json' } }));
    const request = createChaWebClient().createSession('lobby', 'Only once');
    const rejected = expect(request).rejects.toMatchObject({ status: 0, message: 'The request timed out.' });
    await vi.advanceTimersByTimeAsync(60_000);
    await rejected;
    expect(vi.getTimerCount()).toBe(0);
  });

  it('treats a disconnected response body as a transport failure', async () => {
    installFetch(() => new Response(new ReadableStream({
      start(controller) { controller.error(new TypeError('Connection lost')); },
    }), { headers: { 'Content-Type': 'application/json' } }));
    await expect(createChaWebClient().getBootstrap()).rejects.toMatchObject({
      status: 0, message: 'The request failed.',
    });
  });
});

function clientSubmit() {
  return createChaWebClient().submitInput('lobby', 'planning', 'again');
}
