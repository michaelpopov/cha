import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

import { ChaProtocolError, isCommandResult, type SessionSnapshot } from './client';
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
  it('rejects a malformed snapshot before returning it to the application', async () => {
    const snapshot = loadFixture('snapshot.json') as SessionSnapshot;
    const bridge = createFakeNativeBridge({
      'session.snapshot': () => ({ ...snapshot, transcript: [{}] }),
    });
    const client = createNativeChaClient(bridge);
    await expect(client.getSessionSnapshot('entrance', 'welcome'))
      .rejects.toBeInstanceOf(ChaProtocolError);
  });

  it('reports maintenance before validating a partial bootstrap presentation', async () => {
    const bridge = createFakeNativeBridge({
      'bridge.info': () => loadFixture('bridge-info.json'),
      'app.bootstrap': () => ({
        state: 'maintenance',
        context_epoch: 4,
        application_version: 'development',
        capabilities: { can_modify: false, can_transfer_r2: false },
        bootstrap: { vault_name: 'Personal' },
      }),
    });
    const client = createNativeChaClient(bridge);

    await expect(client.getBootstrap()).rejects.toMatchObject({
      code: 'application_unavailable',
      message: 'CHA is applying workspace changes. Try again when maintenance finishes.',
    });
    expect(bridge.contextEpoch()).toBe(0);
  });

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
        application_version: 'development',
        capabilities: { can_modify: true, can_transfer_r2: false },
        bootstrap: bootstrapFixture,
      }),
      'session.submit': () => command.result,
      'session.open': () => ({ forum_id: 'lobby', session_id: 'planning' }),
    });
    const client = createNativeChaClient(bridge);
    await expect(client.getBootstrap()).resolves.toMatchObject({
      initial_session_id: 'welcome',
      capabilities: { can_modify: true, can_transfer_r2: false },
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

  it('runs active-vault maintenance through native methods', async () => {
    const bridge = createFakeNativeBridge({
      'vault.upload': () => ({ byte_count: 12, context_epoch: 4 }),
      'vault.download': () => ({ byte_count: 34, context_epoch: 5 }),
      'vault.import': () => ({ file_count: 2, context_epoch: 6 }),
      'vault.export': () => ({ file_count: 3, context_epoch: 7 }),
    });
    const client = createNativeChaClient(bridge);

    await expect(client.uploadVault()).resolves.toBe(12);
    await expect(client.downloadVault()).resolves.toBe(34);
    await expect(client.importVault()).resolves.toBe(2);
    await expect(client.exportVault()).resolves.toBe(3);
    expect(bridge.contextEpoch()).toBe(7);
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

  it('starts audio jobs and resolves opaque media resources', async () => {
    const resource = {
      resource_id: 'r1',
      url: '/media/r1',
      mime_type: 'audio/mpeg',
      byte_length: 4,
    };
    const bridge = createFakeNativeBridge({
      'audio.status': () => ({ cached_entry_ids: [2], downloads: [] }),
      'audio.start': () => ({ entry_id: 1, cached: false, state: 'queued' }),
      'audio.source': () => resource,
      'speech.start': () => resource,
      'speech.release': () => ({}),
      'audio.clearCache': () => ({}),
      'voiceInput.connect': () => ({ sdp: 'v=0 answer' }),
    });
    const client = createNativeChaClient(bridge);
    await expect(client.getAudioDownloads('lobby', 'planning', 'Personal'))
      .resolves.toEqual({ cached_entry_ids: [2], downloads: [] });
    await expect(client.startAudioDownload('lobby', 'planning', 1, {
      vault_name: 'Personal', reference_id: 'voice',
    })).resolves.toEqual({ entry_id: 1, cached: false, state: 'queued' });
    await expect(client.resolveAudioSource!('lobby', 'planning', 2, 'Personal'))
      .resolves.toEqual(resource);
    await expect(client.previewSpeech!('Hello', 'voice', undefined))
      .resolves.toEqual(resource);
    await expect(client.connectVoiceInput!('v=0 offer', ['en']))
      .resolves.toBe('v=0 answer');
    await client.clearSessionAudioCache('lobby', 'planning');
    await client.releaseResource!('r1');
  });
});
