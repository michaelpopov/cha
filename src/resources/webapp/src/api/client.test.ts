import { describe, expect, it, vi } from 'vitest';

import { ChaError, createChaClient, sessionEventsUrl } from './client';
import { snapshotFixture } from '../test/fixtures';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

describe('CHA API client', () => {
  it('constructs every operation with its documented URL, headers, and body', async () => {
    const fetcher = vi.fn<(
      input: RequestInfo | URL,
      init?: RequestInit,
    ) => Promise<Response>>(async (input) => (
      String(input).endsWith('/api/v1/session')
        ? jsonResponse(snapshotFixture)
        : jsonResponse({})
    ));
    const client = createChaClient(fetcher);

    await client.getBootstrap();
    await client.getCharacter('a b');
    await client.listSessions('f/one');
    await client.createSession('forum', 'Review');
    await client.openSession('forum', 'session');
    await client.getSessionSnapshot('forum', 'session');
    await client.submitInput('forum', 'session', { persona: 'reader', text: 'Hello' });
    await client.stopGeneration('forum', 'session');
    await client.setDefaultCharacter('forum', 'session', 'guide');

    expect(fetcher.mock.calls.map(([url]) => url)).toEqual([
      '/api/v1/bootstrap',
      '/api/v1/characters/a%20b',
      '/api/v1/forums/f%2Fone/sessions',
      '/api/v1/forums/forum/sessions',
      '/api/v1/forums/forum/sessions/session/open',
      '/s/forum/session/api/v1/session',
      '/s/forum/session/api/v1/input',
      '/s/forum/session/api/v1/actions/stop',
      '/s/forum/session/api/v1/actions/default-agent',
    ]);

    expect(fetcher.mock.calls[0][1]?.method).toBeUndefined();
    expect(new Headers(fetcher.mock.calls[0][1]?.headers).get('Accept')).toBe('application/json');
    expect(fetcher.mock.calls[3][1]?.method).toBe('POST');
    expect(new Headers(fetcher.mock.calls[3][1]?.headers).get('Content-Type'))
      .toBe('application/json');
    expect(fetcher.mock.calls[3][1]?.body).toBe('{"label":"Review"}');
    expect(fetcher.mock.calls[4][1]?.body).toBe('{}');
    expect(fetcher.mock.calls[6][1]?.body).toBe('{"persona":"reader","text":"Hello"}');
    expect(fetcher.mock.calls[7][1]?.body).toBe('{}');
    expect(fetcher.mock.calls[8][1]?.body).toBe('{"character_id":"guide"}');
    expect(sessionEventsUrl('f one', 's/two')).toBe('/s/f%20one/s%2Ftwo/api/v1/events');
  });

  it('turns the error envelope into one ChaError shape', async () => {
    const fetcher = vi.fn<(
      input: RequestInfo | URL,
      init?: RequestInit,
    ) => Promise<Response>>(async () => jsonResponse({
      error: { code: 'session_not_live', message: 'Session is not live.' },
    }, 409));
    const client = createChaClient(fetcher);

    await expect(client.getSessionSnapshot('forum', 'session')).rejects.toEqual(
      expect.objectContaining({
        name: 'ChaError',
        status: 409,
        code: 'session_not_live',
        message: 'Session is not live.',
      }),
    );
    await expect(client.getSessionSnapshot('forum', 'session')).rejects.toBeInstanceOf(ChaError);
  });

  it('rejects a session snapshot whose shape the contract does not describe', async () => {
    const client = createChaClient(async () => jsonResponse({ session_id: 'one' }));
    await expect(client.getSessionSnapshot('forum', 'one')).rejects.toThrow(TypeError);
  });
});
