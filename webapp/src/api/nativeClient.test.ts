import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

import { ChaError, isCommandResult } from './client';
import { createFakeNativeBridge } from './nativeBridge';
import { createNativeChaClient } from './nativeClient';
import { bootstrapFixture } from '../test/fixtures';

const wireDirectory = join(
  dirname(fileURLToPath(import.meta.url)),
  '../../../tests/fixtures/wire',
);

function loadFixture(name: string): unknown {
  return JSON.parse(readFileSync(join(wireDirectory, name), 'utf8'));
}

describe('native CHA client', () => {
  it('bootstraps, submits, and checks real C++ command-result fixtures', async () => {
    const command = loadFixture('native-reply-command.json') as {
      result: unknown;
    };
    expect(isCommandResult(command.result)).toBe(true);

    const bridge = createFakeNativeBridge({
      'bridge.info': () => loadFixture('bridge-info.json'),
      'app.bootstrap': () => ({
        state: 'running',
        context_epoch: 3,
        bootstrap: bootstrapFixture,
      }),
      'session.submit': () => command.result,
      'session.open': () => ({ forum_id: 'lobby', session_id: 'planning' }),
    });
    const client = createNativeChaClient(bridge);
    await expect(client.getBootstrap()).resolves.toMatchObject({
      initial_session_id: 'welcome',
    });
    expect(bridge.contextEpoch()).toBe(3);
    await expect(client.submitInput('history', 'session-1', { text: 'Hello' }))
      .resolves.toEqual({ clear_input: true, notice: 'Saved' });
    await expect(client.openSession('lobby', 'planning'))
      .resolves.toEqual({ forum_id: 'lobby', session_id: 'planning' });
  });

  it('lists and switches vaults through native methods', async () => {
    const vault = {
      display_name: 'Personal',
      protected: false,
      data_path: '/tmp/personal.sqlite3',
      mirror_path: null,
      modify_path: null,
      active: true,
      can_delete: false,
    };
    const bridge = createFakeNativeBridge({
      'bridge.info': () => loadFixture('bridge-info.json'),
      'vault.list': () => [vault],
      'vault.switch': () => ({ state: 'running', context_epoch: 4 }),
    });
    const client = createNativeChaClient(bridge);
    await expect(client.listVaults()).resolves.toEqual([vault]);
    await expect(client.switchVault('Projects')).resolves.toBeUndefined();
    expect(bridge.contextEpoch()).toBe(4);
  });

  it('marks unmigrated methods unavailable instead of forwarding them', async () => {
    const client = createNativeChaClient(createFakeNativeBridge());
    await expect(client.listProviders()).rejects.toBeInstanceOf(ChaError);
    await expect(client.listProviders()).rejects.toMatchObject({
      code: 'invalid_argument',
      message: 'That operation is not available in native mode.',
    });
    await expect(client.getR2Storage()).rejects.toMatchObject({
      code: 'invalid_argument',
    });
  });
});
