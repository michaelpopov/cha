import { afterEach, describe, expect, it, vi } from 'vitest';

import { bootstrapFixture, snapshotFixture } from '../test/fixtures';
import { createChaWebClient } from './client';

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
    await expect(createChaWebClient().listSessions('lobby')).rejects.toMatchObject({
      message: expect.stringContaining('incompatible'),
    });
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
