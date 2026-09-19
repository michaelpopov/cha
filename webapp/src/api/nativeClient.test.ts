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

  it('lists sessions and loads character details through native methods', async () => {
    const character = loadFixture('character-detail.json');
    const bridge = createFakeNativeBridge({
      'session.list': () => [
        { id: 'planning', label: 'Planning', live: false, updated_at: 1 },
      ],
      'session.export': () => ({ markdown: '# Planning\n' }),
      'character.get': () => character,
    });
    const client = createNativeChaClient(bridge);
    await expect(client.listSessions('lobby')).resolves.toEqual([
      { id: 'planning', label: 'Planning', live: false, updated_at: 1 },
    ]);
    await expect(client.downloadSession('lobby', 'planning')).resolves.toBe('# Planning\n');
    await expect(client.getCharacter('guide')).resolves.toMatchObject({ id: 'guide' });
  });

  it('lists providers, keys, and R2 metadata through native methods', async () => {
    const provider = {
      id: 'test',
      display_name: 'Test',
      host: 'test',
      port: 1,
      base_path: '',
      mode: 'test' as const,
      model: 'fake',
      stream: true,
      temperature: null,
      max_tokens: null,
      timeout_s: 600,
      idle_timeout_s: 60,
      api_key: null,
      reasoning_effort: '',
      reasoning_format: 'auto' as const,
      https: false,
      api: 'responses' as const,
      auth: 'none' as const,
      web_search: 'off' as const,
      cache_retention: 'short' as const,
      openrouter_targets: [],
      writable: true,
      used_by: ['Guide'],
    };
    const key = {
      id: 'api_key_1',
      display_name: 'Router',
      has_value: true,
      used_by: ['Test'],
    };
    const r2 = {
      id: 'api_key_2',
      display_name: 'Backups',
      url: 'https://account.example/bucket',
      access_key_id: 'access',
      has_secret_key: true,
    };
    const bridge = createFakeNativeBridge({
      'provider.list': () => [{
        id: 'test', display_name: 'Test', model: 'fake', host: 'test',
      }],
      'provider.get': () => provider,
      'apiKey.list': () => [key],
      'r2Storage.get': () => r2,
      'openaiAuth.get': () => ({ status: 'signed_out' }),
      'voiceInput.runtime': () => ({
        url: 'https://api.openai.com/v1/realtime',
        model: 'gpt-4o-transcribe',
        delay: 'low',
        prompt: '',
      }),
    });
    const client = createNativeChaClient(bridge);
    await expect(client.listProviders()).resolves.toEqual([
      { id: 'test', display_name: 'Test', model: 'fake', host: 'test' },
    ]);
    await expect(client.getProvider('test')).resolves.toMatchObject({ id: 'test' });
    await expect(client.listApiKeys()).resolves.toEqual([key]);
    await expect(client.getR2Storage()).resolves.toEqual(r2);
    await expect(client.getOpenAiAuth()).resolves.toEqual({ status: 'signed_out' });
    await expect(client.getVoiceInputRuntime()).resolves.toEqual({
      url: 'https://api.openai.com/v1/realtime',
      model: 'gpt-4o-transcribe',
      delay: 'low',
      prompt: '',
    });
  });

  it('marks unmigrated media methods unavailable instead of forwarding them', async () => {
    const client = createNativeChaClient(createFakeNativeBridge());
    await expect(client.getAudioDownloads('lobby', 'planning', 'Personal'))
      .rejects.toBeInstanceOf(ChaError);
    await expect(client.getAudioDownloads('lobby', 'planning', 'Personal'))
      .rejects.toMatchObject({
        code: 'invalid_argument',
        message: 'That operation is not available in native mode.',
      });
  });
});
