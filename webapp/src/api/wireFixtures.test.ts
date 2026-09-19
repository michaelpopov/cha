import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

import { createChaClient, isCommandResult, isSessionSnapshot } from './client';
import { isAppendEvent } from './events';
import { isNativeSessionEvent } from './nativeEvents';
import { nativeProtocolVersion } from './nativeBridge';
import { validateBootstrap } from '../state/bootstrap';

const wireDirectory = join(
  dirname(fileURLToPath(import.meta.url)),
  '../../../tests/fixtures/wire',
);

function loadFixture(name: string): unknown {
  return JSON.parse(readFileSync(join(wireDirectory, name), 'utf8'));
}

describe('C++ wire fixtures', () => {
  it('match frontend bootstrap, snapshot, append, and error expectations', async () => {
    expect(validateBootstrap(loadFixture('bootstrap.json')).initial_session_id)
      .toBe('welcome');
    expect(isSessionSnapshot(loadFixture('snapshot.json'))).toBe(true);
    expect(isAppendEvent(loadFixture('append-entry.json'))).toBe(true);
    expect(isAppendEvent(loadFixture('append-reasoning.json'))).toBe(true);

    const command = loadFixture('command-result.json');
    expect(command).toEqual({ clear_input: true, notice: 'Saved' });

    const error = loadFixture('error.json') as {
      error: { code: string; message: string };
    };
    expect(error.error.code).toBe('command_timeout');
    expect(error.error.message).toBe('<script>alert(1)</script>');

    const character = loadFixture('character-detail.json');
    const client = createChaClient(async () => new Response(JSON.stringify(character), {
      headers: { 'Content-Type': 'application/json' },
    }));
    await expect(client.getCharacter('guide')).resolves.toMatchObject({ id: 'guide' });

    const info = loadFixture('bridge-info.json') as { protocol_version: number };
    expect(info.protocol_version).toBe(nativeProtocolVersion);
    const provider = loadFixture('provider-detail.json') as { api_key: unknown };
    expect(provider.api_key).toBeNull();
    const runtime = loadFixture('voice-input-runtime.json') as Record<string, unknown>;
    expect(runtime).not.toHaveProperty('api_key');
    const key = loadFixture('api-key-detail.json') as { has_value: boolean };
    expect(key.has_value).toBe(true);
    const auth = loadFixture('openai-auth-status.json') as { status: string };
    expect(auth.status).toBe('waiting');
    expect(isCommandResult(
      (loadFixture('native-reply-command.json') as { result: unknown }).result,
    )).toBe(true);
    expect(isNativeSessionEvent(loadFixture('native-event-append.json'))).toBe(true);
  });

  it('loads DTOs from the transport-neutral schema rather than HTTP paths', () => {
    const dto = readFileSync(
      join(dirname(fileURLToPath(import.meta.url)), '../../../resources/dto.yaml'),
      'utf8',
    );
    expect(dto).toContain('title: CHA DTOs');
    expect(dto).toContain('NativeRequest:');
    expect(dto).toContain('MediaResource:');
    expect(dto).toContain('Bootstrap:');
  });
});
