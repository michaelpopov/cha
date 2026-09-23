import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it } from 'vitest';

import {
  isCharacterDetail,
  isCommandResult,
  isNativeVoiceInputRuntime,
  isSessionSnapshot,
  isVoiceInputSettings,
  type SessionSnapshot,
} from './client';
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
  it('match frontend bootstrap, snapshot, append, and error expectations', () => {
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
    expect(isCharacterDetail(character)).toBe(true);

    const info = loadFixture('bridge-info.json') as { protocol_version: number };
    expect(info.protocol_version).toBe(nativeProtocolVersion);
    const provider = loadFixture('provider-detail.json') as { api_key: unknown };
    expect(provider.api_key).toBeNull();
    const runtime = loadFixture('voice-input-runtime.json') as Record<string, unknown>;
    expect(runtime).not.toHaveProperty('api_key');
    expect(isNativeVoiceInputRuntime(runtime)).toBe(true);
    expect(isNativeVoiceInputRuntime({ ...runtime, provider: 'grok' })).toBe(false);
    const { provider: _provider, ...runtimeWithoutProvider } = runtime;
    expect(isNativeVoiceInputRuntime(runtimeWithoutProvider)).toBe(false);
    expect(isVoiceInputSettings({
      ...runtime,
      api_key: 'api_key_1',
    })).toBe(true);
    expect(isVoiceInputSettings(runtime)).toBe(false);
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

describe('snapshot validation', () => {
  function snapshot(): SessionSnapshot {
    return loadFixture('snapshot.json') as SessionSnapshot;
  }

  it.each([
    ['missing forum identity', { forum: { default_persona_id: 'guest', default_persona_display_name: 'Guest' } }],
    ['empty session identity', { session_id: '' }],
    ['missing session label', { session_label: undefined }],
    ['malformed character', { characters: [{}] }],
    ['empty default character', { default_character_id: '' }],
    ['empty transcript entry', { transcript: [{}] }],
    ['empty generation state', { generation: {} }],
    ['unsafe covered entry', { covered_until: Number.MAX_SAFE_INTEGER + 1 }],
    ['non-text notice', { notice: {} }],
    ['unknown lifecycle', { lifecycle: 'closed' }],
    ['unknown shutdown reason', { shutdown_reason: 'unknown' }],
  ])('rejects %s', (_name, patch) => {
    expect(isSessionSnapshot({ ...snapshot(), ...patch })).toBe(false);
  });

  it.each([
    ['missing forum ID', { id: undefined }],
    ['missing forum name', { display_name: undefined }],
    ['empty persona ID', { default_persona_id: '' }],
    ['missing forum members', { members: undefined }],
    ['malformed forum member', { members: [{}] }],
  ])('rejects %s', (_name, patch) => {
    const value = snapshot();
    expect(isSessionSnapshot({ ...value, forum: { ...value.forum, ...patch } })).toBe(false);
  });

  it.each([
    ['unsafe entry ID', { id: Number.MAX_SAFE_INTEGER + 1 }],
    ['negative entry ID', { id: -1 }],
    ['fractional request ID', { request_id: 1.5 }],
    ['unknown entry kind', { kind: 'system' }],
    ['unknown entry status', { status: 'pending' }],
    ['missing participant', { participant_id: undefined }],
    ['missing recipient', { addressed_to: undefined }],
    ['non-text author', { display_name: {} }],
    ['non-text recipient', { addressed_to_name: {} }],
    ['non-text content', { text: {} }],
    ['missing timestamp', { created_at: undefined }],
    ['invalid timestamp', { created_at: 'yesterday' }],
    ['invalid audio flag', { has_cached_audio: 'yes' }],
  ])('rejects a transcript with %s', (_name, patch) => {
    const value = snapshot();
    expect(isSessionSnapshot({
      ...value, transcript: [{ ...value.transcript[0], ...patch }],
    })).toBe(false);
  });

  it.each([
    ['missing active flag', { active: undefined }],
    ['unsafe request ID', { request_id: Number.MAX_SAFE_INTEGER + 1 }],
    ['negative request ID', { request_id: -1 }],
    ['missing character ID', { character_id: undefined }],
    ['non-text character name', { character_display_name: {} }],
    ['unknown phase', { phase: 'complete' }],
    ['non-text reasoning', { reasoning_text: [] }],
  ])('rejects generation with %s', (_name, patch) => {
    const value = snapshot();
    expect(isSessionSnapshot({ ...value, generation: { ...value.generation, ...patch } })).toBe(false);
  });

  it('accepts unknown timestamps, empty notice attribution, and idle generation', () => {
    const value = snapshot();
    value.transcript[0].created_at = null;
    delete value.transcript[0].has_cached_audio;
    value.transcript.push({
      ...value.transcript[0], id: 2, kind: 'notice', participant_id: '', display_name: '',
      addressed_to: '', addressed_to_name: '', text: '',
    });
    expect(isSessionSnapshot(value)).toBe(true);
  });

  it.each(['reasoning', 'answering', 'stopping'] as const)('accepts active %s generation', (phase) => {
    const value = snapshot();
    value.generation = {
      active: true, request_id: 7, character_id: 'assistant',
      character_display_name: 'Assistant', phase, reasoning_text: 'Thinking…',
    };
    value.transcript[0].request_id = 7;
    value.covered_until = 1;
    value.notice = 'Stopping soon';
    value.lifecycle = 'stopping';
    value.shutdown_reason = 'session_closed';
    expect(isSessionSnapshot(value)).toBe(true);
  });
});
