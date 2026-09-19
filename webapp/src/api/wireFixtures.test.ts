import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

import { createChaClient, isSessionSnapshot } from './client';
import { isAppendEvent } from './events';
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
  });
});
