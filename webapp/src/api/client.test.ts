import { describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  ChaProtocolError,
  ChaUnavailableError,
  createChaClient,
  sessionEventsUrl,
  type ProviderUpdate,
} from './client';
import {
  characterDetailFixture,
  forumDetailFixture,
  personaDetailFixture,
  snapshotFixture,
} from '../test/fixtures';

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
    ) => Promise<Response>>(async (input, init) => {
      const url = String(input);
      if (url.endsWith('/download')) {
        return new Response('# Session\n', { headers: { 'Content-Type': 'text/markdown' } });
      }
      if (url.endsWith('/api/v1/session')) return jsonResponse(snapshotFixture);
      if (url.includes('/characters')) return jsonResponse(characterDetailFixture);
      if (url.includes('/personas')) return jsonResponse(personaDetailFixture);
      if (url.endsWith('/sessions') && init?.method === undefined) return jsonResponse([]);
      if (url.includes('/forums/') || url === '/api/v1/forums') {
        return jsonResponse(forumDetailFixture);
      }
      return jsonResponse({});
    });
    const client = createChaClient(fetcher);

    await client.getBootstrap();
    await client.getCharacter('a b');
    await client.getPersona('read er');
    await client.updatePersona('read er', { display_name: 'Reader' });
    await client.listSessions('f/one');
    await client.createSession('forum', 'Review');
    await client.renameSession('forum', 'session', 'Renamed');
    await client.deleteSession('forum', 'session');
    expect(await client.downloadSession('f/one', 's two')).toBe('# Session\n');
    await client.openSession('forum', 'session');
    await client.getSessionSnapshot('forum', 'session');
    await client.submitInput('forum', 'session', { text: 'Hello' });
    await client.stopGeneration('forum', 'session');
    await client.setDefaultCharacter('forum', 'session', 'guide');
    await client.updateCharacter('a b', {
      provider: 'terra',
      style: null,
      reasoning_effort: null,
      web_search: null,
    });
    await client.updateCharacterDefinition('a b', { display_name: 'Guide' });
    await client.getOpenAiAuth();
    await client.startOpenAiAuth();
    await client.pollOpenAiAuth();
    await client.disconnectOpenAiAuth();
    await client.switchVault('Projects');
    await client.createPersona({ display_name: 'Project manager' });
    await client.createCharacter({
      display_name: 'Mentor',
      description: 'A thoughtful guide.',
    });
    await client.createForum({ display_name: 'Brain Trust', persona_id: 'reader' });
    await client.getForum('f one');
    await client.updateForum('f one', { display_name: 'Brain Trust' });
    await client.updateForumMembers('f one', { character_ids: ['guide', 'critic'] });
    await client.deletePersona('read er');
    await client.deleteCharacter('a b');
    await client.deleteForum('f one');

    expect(fetcher.mock.calls.map(([url]) => url)).toEqual([
      '/api/v1/bootstrap',
      '/api/v1/characters/a%20b',
      '/api/v1/personas/read%20er',
      '/api/v1/personas/read%20er',
      '/api/v1/forums/f%2Fone/sessions',
      '/api/v1/forums/forum/sessions',
      '/api/v1/forums/forum/sessions/session',
      '/api/v1/forums/forum/sessions/session',
      '/api/v1/forums/f%2Fone/sessions/s%20two/download',
      '/api/v1/forums/forum/sessions/session/open',
      '/s/forum/session/api/v1/session',
      '/s/forum/session/api/v1/input',
      '/s/forum/session/api/v1/actions/stop',
      '/s/forum/session/api/v1/actions/default-character',
      '/api/v1/characters/a%20b',
      '/api/v1/characters/a%20b/definition',
      '/api/v1/openai/auth',
      '/api/v1/openai/auth/login',
      '/api/v1/openai/auth/poll',
      '/api/v1/openai/auth/disconnect',
      '/api/v1/vault/switch',
      '/api/v1/personas',
      '/api/v1/characters',
      '/api/v1/forums',
      '/api/v1/forums/f%20one',
      '/api/v1/forums/f%20one',
      '/api/v1/forums/f%20one/members',
      '/api/v1/personas/read%20er',
      '/api/v1/characters/a%20b',
      '/api/v1/forums/f%20one',
    ]);

    expect(fetcher.mock.calls[0][1]?.method).toBeUndefined();
    expect(new Headers(fetcher.mock.calls[0][1]?.headers).get('Accept')).toBe('application/json');
    expect(fetcher.mock.calls[3][1]?.method).toBe('PATCH');
    expect(fetcher.mock.calls[3][1]?.body).toBe('{"display_name":"Reader"}');
    expect(fetcher.mock.calls[5][1]?.method).toBe('POST');
    expect(new Headers(fetcher.mock.calls[5][1]?.headers).get('Content-Type'))
      .toBe('application/json');
    expect(fetcher.mock.calls[5][1]?.body).toBe('{"label":"Review"}');
    expect(fetcher.mock.calls[6][1]?.method).toBe('PATCH');
    expect(fetcher.mock.calls[6][1]?.body).toBe('{"label":"Renamed"}');
    expect(fetcher.mock.calls[7][1]?.method).toBe('DELETE');
    expect(fetcher.mock.calls[7][1]?.body).toBe('{}');
    expect(new Headers(fetcher.mock.calls[8][1]?.headers).get('Accept')).toBe('text/markdown');
    expect(fetcher.mock.calls[9][1]?.body).toBe('{}');
    expect(fetcher.mock.calls[11][1]?.body).toBe('{"text":"Hello"}');
    for (const call of fetcher.mock.calls.slice(-3)) {
      expect(call[1]?.method).toBe('DELETE');
      expect(call[1]?.body).toBe('{}');
    }
    expect(fetcher.mock.calls[12][1]?.body).toBe('{}');
    expect(fetcher.mock.calls[13][1]?.body).toBe('{"character_id":"guide"}');
    expect(fetcher.mock.calls[14][1]?.method).toBe('PATCH');
    expect(fetcher.mock.calls[14][1]?.body).toBe(
      '{"provider":"terra","style":null,"reasoning_effort":null,"web_search":null}',
    );
    expect(fetcher.mock.calls[15][1]?.method).toBe('PATCH');
    expect(fetcher.mock.calls[15][1]?.body).toBe('{"display_name":"Guide"}');
    expect(fetcher.mock.calls[16][1]?.method).toBeUndefined();
    expect(new Headers(fetcher.mock.calls[16][1]?.headers).get('Accept')).toBe('application/json');
    expect(fetcher.mock.calls[17][1]?.method).toBe('POST');
    expect(new Headers(fetcher.mock.calls[17][1]?.headers).get('Content-Type'))
      .toBe('application/json');
    expect(fetcher.mock.calls[18][1]?.method).toBe('POST');
    expect(fetcher.mock.calls[18][1]?.body).toBe('{}');
    expect(fetcher.mock.calls[19][1]?.method).toBe('POST');
    expect(fetcher.mock.calls[19][1]?.body).toBe('{}');
    expect(fetcher.mock.calls[20][1]?.method).toBe('POST');
    expect(fetcher.mock.calls[20][1]?.body).toBe('{"vault_name":"Projects"}');
    expect(fetcher.mock.calls[21][1]?.method).toBe('POST');
    expect(fetcher.mock.calls[21][1]?.body).toBe('{"display_name":"Project manager"}');
    expect(fetcher.mock.calls[22][1]?.method).toBe('POST');
    expect(fetcher.mock.calls[22][1]?.body).toBe(
      '{"display_name":"Mentor","description":"A thoughtful guide."}',
    );
    expect(fetcher.mock.calls[23][1]?.method).toBe('POST');
    expect(fetcher.mock.calls[23][1]?.body).toBe(
      '{"display_name":"Brain Trust","persona_id":"reader"}',
    );
    expect(fetcher.mock.calls[24][1]?.method).toBeUndefined();
    expect(fetcher.mock.calls[25][1]?.method).toBe('PATCH');
    expect(fetcher.mock.calls[25][1]?.body).toBe('{"display_name":"Brain Trust"}');
    expect(fetcher.mock.calls[26][1]?.method).toBe('PUT');
    expect(fetcher.mock.calls[26][1]?.body).toBe(
      '{"character_ids":["guide","critic"]}',
    );
    expect(sessionEventsUrl('f one', 's/two')).toBe('/s/f%20one/s%2Ftwo/api/v1/events');
  });

  it('accepts an empty 204 from switchVault and reports switch errors', async () => {
    const ok = createChaClient(async () => new Response(null, { status: 204 }));
    await expect(ok.switchVault('Projects')).resolves.toBeUndefined();

    const failed = createChaClient(async () => jsonResponse({
      error: { code: 'bad_request', message: 'Unknown vault.' },
    }, 400));
    await expect(failed.switchVault('missing')).rejects.toEqual(expect.objectContaining({
      name: 'ChaError',
      status: 400,
      code: 'bad_request',
      message: 'Unknown vault.',
    }));
  });

  it('posts candidate settings when testing a provider', async () => {
    const fetcher = vi.fn<(
      input: RequestInfo | URL,
      init?: RequestInit,
    ) => Promise<Response>>(async () => new Response(null, { status: 204 }));
    const client = createChaClient(fetcher);
    const candidate: ProviderUpdate = {
      display_name: 'OpenAI',
      host: 'api.openai.com',
      port: 443,
      base_path: '',
      mode: 'net',
      model: 'gpt-5',
      stream: true,
      temperature: null,
      max_tokens: null,
      timeout_s: 600,
      idle_timeout_s: 60,
      api_key: 'api_key_1',
      api_key_env: null,
      reasoning_effort: '',
      reasoning_format: 'auto',
      https: true,
      api: 'responses',
      auth: 'none',
      web_search: 'off',
      cache_retention: 'short',
    };

    await expect(client.testProvider('Open AI', candidate)).resolves.toBeUndefined();
    expect(fetcher).toHaveBeenCalledOnce();
    expect(fetcher.mock.calls[0][0]).toBe('/api/v1/providers/Open%20AI/test');
    expect(fetcher.mock.calls[0][1]?.method).toBe('POST');
    expect(fetcher.mock.calls[0][1]?.body).toBe(JSON.stringify(candidate));
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

  it('does not claim an unknown server error code is part of the browser contract', async () => {
    const client = createChaClient(async () => jsonResponse({
      error: { code: 'future_private_error', message: 'read /private/provider-key' },
    }, 500));

    await expect(client.getBootstrap()).rejects.toEqual(expect.objectContaining({
      name: 'ChaError',
      status: 500,
      code: 'internal_error',
      message: 'CHA returned an invalid error response (500).',
    }));
  });

  it('rejects a session snapshot whose shape the contract does not describe', async () => {
    const client = createChaClient(async () => jsonResponse({ session_id: 'one' }));
    await expect(client.getSessionSnapshot('forum', 'one')).rejects.toThrow(TypeError);
  });

  it('rejects malformed detail and listing responses at the API boundary', async () => {
    const client = createChaClient(async () => jsonResponse({ id: 'incomplete' }));

    await expect(client.getCharacter('guide')).rejects.toBeInstanceOf(ChaProtocolError);
    await expect(client.getPersona('reader')).rejects.toBeInstanceOf(ChaProtocolError);
    await expect(client.getForum('lobby')).rejects.toBeInstanceOf(ChaProtocolError);
    await expect(client.listSessions('lobby')).rejects.toBeInstanceOf(ChaProtocolError);
  });

  it('reports OpenAI auth errors through the existing envelope', async () => {
    const client = createChaClient(async () => jsonResponse({
      error: { code: 'bad_request', message: 'Expected a JSON request body.' },
    }, 400));

    await expect(client.startOpenAiAuth()).rejects.toEqual(expect.objectContaining({
      name: 'ChaError',
      status: 400,
      code: 'bad_request',
      message: 'Expected a JSON request body.',
    }));
  });

  it('turns a transport failure into a fixed message without leaking exception details', async () => {
    const client = createChaClient(async () => {
      throw new Error('open /private/customer/.env containing a-secret-key');
    });

    const request = client.getBootstrap();
    await expect(request).rejects.toBeInstanceOf(ChaUnavailableError);
    await expect(request).rejects.not.toThrow(/private|secret/i);
  });
});
