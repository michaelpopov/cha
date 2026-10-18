import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it, vi } from 'vitest';
import { strToU8, zipSync } from 'fflate';

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
  it('loads the forum system prompt using saved server settings', async () => {
    const response = { system_prompt: '<instructions>\nFull prompt.\n</instructions>' };
    const read = vi.fn(() => response);
    const client = createNativeChaClient(createFakeNativeBridge({ 'forum.systemPrompt': read }));
    expect(await client.getForumSystemPrompt('lobby', 'guide')).toEqual(response);
    expect(read).toHaveBeenCalledWith({ forum_id: 'lobby', character_id: 'guide' });
    const invalid = createNativeChaClient(createFakeNativeBridge({ 'forum.systemPrompt': () => ({ system_prompt: 42 }) }));
    await expect(invalid.getForumSystemPrompt('lobby', 'guide')).rejects.toBeInstanceOf(ChaProtocolError);
  });

  it('reads and replaces shared character instructions without a character id or filename', async () => {
    const file = { filename: 'character-voice.md', content: 'Shared voice.', writable: true };
    const read = vi.fn(() => file);
    const write = vi.fn(({ content }) => ({ ...file, content }));
    const client = createNativeChaClient(createFakeNativeBridge({
      'character.instructions.get': read,
      'character.instructions.update': write,
    }));
    expect(await client.getCharacterInstructions()).toEqual(file);
    expect(read).toHaveBeenCalledWith({});
    expect(await client.updateCharacterInstructions('New voice.')).toEqual({ ...file, content: 'New voice.' });
    expect(write).toHaveBeenCalledWith({ content: 'New voice.' });
  });

  it('renames files through the native bridge without copying their content', async () => {
    const rename = vi.fn(() => ({}));
    const client = createNativeChaClient(createFakeNativeBridge({ 'character.file.rename': rename }));
    await client.renameCharacterFile('guide', 'portrait.png', 'seneca.png');
    expect(rename).toHaveBeenCalledWith({ character_id: 'guide', filename: 'portrait.png', new_filename: 'seneca.png' });
  });

  it.each(['JPG', 'jpeg', 'png'])('uploads original %s bytes under the picture filename', async (extension) => {
    const write = vi.fn(() => ({}));
    const client = createNativeChaClient(createFakeNativeBridge({ 'character.picture.update': write }));
    const file = new File([new Uint8Array([0, 255, 128, 1])], `holiday.${extension}`);
    await client.updateCharacterPicture('guide', file);
    expect(write).toHaveBeenCalledWith({
      character_id: 'guide', filename: `holiday.${extension}`, content_base64: 'AP+AAQ==',
    });
    await client.updateCharacterPicture('guide', file, 'PICTURE.webp');
    expect(write).toHaveBeenLastCalledWith({
      character_id: 'guide', filename: `holiday.${extension}`, content_base64: 'AP+AAQ==',
      previous_filename: 'PICTURE.webp',
    });
  });

  it('does not upload a file to a vault switched while it was being read', async () => {
    const read = vi.spyOn(FileReader.prototype, 'readAsDataURL').mockImplementation(() => {});
    try {
      const write = vi.fn(() => ({}));
      const bridge = createFakeNativeBridge({ 'character.picture.update': write });
      const client = createNativeChaClient(bridge);
      const saving = client.updateCharacterPicture('guide', new File(['bytes'], 'portrait.png'));
      const reader = read.mock.instances[0] as FileReader;
      bridge.setContextEpoch(bridge.contextEpoch() + 1);
      Object.defineProperty(reader, 'result', { value: 'data:image/png;base64,AP8=' });
      reader.onload?.call(reader, new ProgressEvent('load') as ProgressEvent<FileReader>);
      await expect(saving).rejects.toMatchObject({ code: 'vault_changed' });
      expect(write).not.toHaveBeenCalled();
    } finally {
      read.mockRestore();
    }
  });

  it.each([
    ['PICTURE.png', 'image/png'],
    ['PICTURE.webp', 'image/webp'],
    ['PICTURE.jpg', 'image/jpeg'],
    ['PICTURE.jpeg', 'image/jpeg'],
    ['PICTURE.gif', 'image/gif'],
    ['my-portrait.PNG', 'image/png'],
    ['Seneca.jpg', 'image/jpeg'],
  ])('requests and accepts %s without decoding image data', async (filename, mime_type) => {
    let received: unknown;
    const picture = { filename, mime_type, content_base64: 'bad base64!' };
    const client = createNativeChaClient(createFakeNativeBridge({
      'character.picture.get': (params) => { received = params; return picture; },
    }));
    expect(await client.getCharacterPicture('guide')).toEqual(picture);
    expect(received).toEqual({ character_id: 'guide' });
    expect(await client.getCharacterPicture('guide', filename)).toEqual(picture);
    expect(received).toEqual({ character_id: 'guide', filename });
  });

  it.each([null, { filename: 'PICTURE.gif', mime_type: 'image/gif', content_base64: '' }])(
    'accepts no picture or empty picture data', async (response) => {
      const client = createNativeChaClient(createFakeNativeBridge({
        'character.picture.get': () => response,
      }));
      expect(await client.getCharacterPicture('assistant')).toEqual(response);
    },
  );

  it.each([
    undefined, {}, [],
    { filename: '../picture.png', mime_type: 'image/png', content_base64: '' },
    { filename: 'pictures\\portrait.png', mime_type: 'image/png', content_base64: '' },
    { filename: 'PICTURE.png', mime_type: 'image/jpeg', content_base64: '' },
    { filename: 'PICTURE.png', mime_type: 'image/png', content_base64: 123 },
    { filename: 'PICTURE.png', mime_type: 'image/png' },
    { filename: 'PICTURE.png', mime_type: 'image/png', content_base64: '', path: '/tmp' },
  ])('rejects malformed picture responses', async (response) => {
    const client = createNativeChaClient(createFakeNativeBridge({
      'character.picture.get': () => response,
    }));
    await expect(client.getCharacterPicture('guide')).rejects.toBeInstanceOf(ChaProtocolError);
  });

  it('propagates picture read errors', async () => {
    const error = new Error('not found');
    const client = createNativeChaClient(createFakeNativeBridge({
      'character.picture.get': () => { throw error; },
    }));
    await expect(client.getCharacterPicture('missing')).rejects.toBe(error);
  });

  it('uses a guarded deletion when discarding an unused session', async () => {
    let received: unknown;
    const client = createNativeChaClient(createFakeNativeBridge({
      'session.delete': (params) => { received = params; return {}; },
    }));
    await client.discardUnusedSession('lobby', 'unused');
    expect(received).toEqual({ forum_id: 'lobby', session_id: 'unused', only_if_unused: true });
  });

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
      'vault.upload.check': () => ({ etag: 'remote-etag', status: 'match', context_epoch: 3 }),
      'vault.download': () => ({ byte_count: 34, context_epoch: 5 }),
      'vault.import': () => ({ file_count: 2, context_epoch: 6 }),
      'vault.export': () => ({ file_count: 3, context_epoch: 7 }),
    });
    const client = createNativeChaClient(bridge);

    bridge.setContextEpoch(3);
    await expect(client.checkVaultUpload()).resolves.toEqual({
      etag: 'remote-etag', status: 'match', context_epoch: 3,
    });
    await expect(client.uploadVault(await client.checkVaultUpload())).resolves.toBe(12);
    await expect(client.downloadVault()).resolves.toBe(34);
    await expect(client.importVault()).resolves.toBe(2);
    await expect(client.exportVault()).resolves.toBe(3);
    expect(bridge.contextEpoch()).toBe(7);
  });

  it.each(['match', 'mismatch', 'missing'] as const)(
    'rejects a stale %s upload check before sending any upload request',
    async (status) => {
      const bridge = createFakeNativeBridge({
        'vault.upload.check': () => ({
          etag: status === 'missing' ? null : 'remote-etag',
          status,
          context_epoch: 3,
        }),
        'vault.upload': () => ({ byte_count: 12, context_epoch: 5 }),
      });
      const client = createNativeChaClient(bridge);
      bridge.setContextEpoch(3);
      const check = await client.checkVaultUpload();
      bridge.setContextEpoch(4);

      await expect(client.uploadVault(check)).rejects.toMatchObject({ code: 'vault_changed' });
      expect(bridge.posts).not.toContainEqual(expect.objectContaining({ method: 'vault.upload' }));
    },
  );

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
      reasoning_effort: 'none' as const,
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
        provider: 'openai',
        url: 'https://api.openai.com/v1/realtime',
        model: 'gpt-4o-transcribe',
        delay: 'low',
        prompt: '',
        send_phrase: 'over to you',
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
      provider: 'openai',
      url: 'https://api.openai.com/v1/realtime',
      model: 'gpt-4o-transcribe',
      delay: 'low',
      prompt: '',
      send_phrase: 'over to you',
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

  it('calls the xAI voice methods with session id and piece boundaries', async () => {
    const calls: Array<{ method: string; params: unknown }> = [];
    const bridge = createFakeNativeBridge({
      'voiceInput.xai.start': (params) => {
        calls.push({ method: 'voiceInput.xai.start', params });
        return { session_id: 'dictation-1', stop_budget_ms: 20000 };
      },
      'voiceInput.xai.audio': (params) => {
        calls.push({ method: 'voiceInput.xai.audio', params });
        return { session_id: 'dictation-1', pieces: ['Hello', ' world'] };
      },
      'voiceInput.xai.stop': (params) => {
        calls.push({ method: 'voiceInput.xai.stop', params });
        return { session_id: 'dictation-1', pieces: ['\n'] };
      },
      'voiceInput.xai.cancel': (params) => {
        calls.push({ method: 'voiceInput.xai.cancel', params });
        return {};
      },
    });
    const client = createNativeChaClient(bridge);
    await expect(client.startXaiVoiceInput('dictation-1', ['ru']))
      .resolves.toEqual({ session_id: 'dictation-1', stop_budget_ms: 20000 });
    await expect(client.sendXaiVoiceAudio('dictation-1', 'AAE='))
      .resolves.toEqual({ session_id: 'dictation-1', pieces: ['Hello', ' world'] });
    await expect(client.stopXaiVoiceInput('dictation-1', 1500))
      .resolves.toEqual({ session_id: 'dictation-1', pieces: ['\n'] });
    await client.cancelXaiVoiceInput('dictation-1');
    expect(calls).toEqual([
      { method: 'voiceInput.xai.start', params: { session_id: 'dictation-1', languages: ['ru'] } },
      { method: 'voiceInput.xai.audio', params: { session_id: 'dictation-1', pcm_base64: 'AAE=' } },
      { method: 'voiceInput.xai.stop', params: { session_id: 'dictation-1', remaining_ms: 1500 } },
      { method: 'voiceInput.xai.cancel', params: { session_id: 'dictation-1' } },
    ]);
  });
});

describe('notes native client', () => {
  const folder = { id: 1, parent_id: null, kind: 'folder', name: 'Plan' };
  const note = { id: 4, parent_id: 1, kind: 'note', name: 'Draft' };
  const document = { type: 'doc', content: [{ type: 'paragraph' }] };

  function clientFor(handlers: Record<string, (params: unknown) => unknown>) {
    return createNativeChaClient(createFakeNativeBridge(handlers));
  }

  it('reads a page using the notes bridge and validates its reply', async () => {
    const page = { url: 'https://example.org/final', title: 'Title', markdown: '**Text**' };
    const read = vi.fn(() => page);
    expect(await clientFor({ 'notes.readPage': read }).readNotePage('https://example.org')).toEqual(page);
    expect(read).toHaveBeenCalledWith({ url: 'https://example.org' });
    for (const invalid of [{ ...page, markdown: 42 }, { ...page, extra: true }, {}]) {
      await expect(clientFor({ 'notes.readPage': () => invalid }).readNotePage('https://example.org'))
        .rejects.toThrow();
    }
  });

  function epubFile() {
    const files = {
      'META-INF/container.xml': '<container><rootfiles><rootfile full-path="book.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
      'book.opf': '<package><metadata><title>Book</title></metadata><manifest><item id="a" href="text.xhtml" media-type="application/xhtml+xml"/></manifest><spine><itemref idref="a"/></spine></package>',
      'text.xhtml': '<html xmlns="http://www.w3.org/1999/xhtml"><body><h1>First</h1></body></html>',
    };
    return new File([zipSync(Object.fromEntries(Object.entries(files).map(([path, text]) => [path, strToU8(text)])))], 'book.epub');
  }

  it('prepares the entire EPUB before sending one book import request', async () => {
    const write = vi.fn(() => ({ id: 12 }));
    const client = clientFor({ 'notes.importBook': write });
    expect(await client.importEpub(epubFile(), [])).toBe(12);
    expect(write).toHaveBeenCalledTimes(1);
    expect(write).toHaveBeenCalledWith({ name: 'Book', notes: [{ name: '001. First', document: {
      type: 'doc', content: [{ type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: 'First' }] }],
    } }] });
  });

  it('discards an EPUB prepared across a vault context change without sending it', async () => {
    const read = vi.spyOn(FileReader.prototype, 'readAsArrayBuffer').mockImplementation(() => {});
    try {
      const write = vi.fn(() => ({ id: 12 }));
      const bridge = createFakeNativeBridge({ 'notes.importBook': write });
      const client = createNativeChaClient(bridge);
      const result = client.importEpub(epubFile(), []);
      const reader = read.mock.instances[0] as FileReader;
      bridge.setContextEpoch(bridge.contextEpoch() + 1);
      // Finish the original read using the real FileReader implementation.
      read.mockRestore();
      const bytes = await new Promise<ArrayBuffer>((resolve) => {
        const other = new FileReader();
        other.onload = () => resolve(other.result as ArrayBuffer);
        other.readAsArrayBuffer(epubFile());
      });
      Object.defineProperty(reader, 'result', { value: bytes });
      reader.onload?.call(reader, new ProgressEvent('load') as ProgressEvent<FileReader>);
      await expect(result).rejects.toMatchObject({ code: 'vault_changed' });
      expect(write).not.toHaveBeenCalled();
    } finally {
      read.mockRestore();
    }
  });

  it('does not send a malformed EPUB and rejects an invalid import response', async () => {
    const write = vi.fn(() => ({ id: 0 }));
    const client = clientFor({ 'notes.importBook': write });
    await expect(client.importEpub(new File(['bad zip'], 'bad.epub'), [])).rejects.toMatchObject({ code: 'invalid_argument' });
    expect(write).not.toHaveBeenCalled();
    await expect(client.importEpub(epubFile(), [])).rejects.toBeInstanceOf(ChaProtocolError);
  });

  it('sends the ordinary note methods and accepts guarded replies', async () => {
    const calls: unknown[] = [];
    const client = clientFor({
      'notes.list': (params) => {
        calls.push(['list', params]);
        return { items: [folder, note] };
      },
      'notes.create': (params) => {
        calls.push(['create', params]);
        return note;
      },
      'notes.get': (params) => {
        calls.push(['get', params]);
        return { id: 4, document };
      },
      'notes.save': (params) => {
        calls.push(['save', params]);
        return {};
      },
      'notes.rename': (params) => {
        calls.push(['rename', params]);
        return { ...note, name: 'Final' };
      },
      'notes.delete': (params) => {
        calls.push(['delete', params]);
        return {};
      },
    });

    expect(await client.listNoteItems()).toEqual([folder, note]);
    expect(await client.createNoteItem(1, 'note', 'Draft')).toEqual(note);
    expect(await client.getNote(4)).toEqual({ id: 4, document });
    await client.saveNote(4, document);
    await client.saveNote(4, { type: 'doc', extra: true });
    expect(await client.renameNoteItem(4, 'Final')).toEqual({ ...note, name: 'Final' });
    await client.deleteNoteItem(4);
    expect(calls).toEqual([
      ['list', {}],
      ['create', { parent_id: 1, kind: 'note', name: 'Draft' }],
      ['get', { id: 4 }],
      ['save', { id: 4, document }],
      ['save', { id: 4, document: { type: 'doc', extra: true } }],
      ['rename', { id: 4, name: 'Final' }],
      ['delete', { id: 4 }],
    ]);
  });

  it.each([
    ['notes.list', { items: [{ ...folder, extra: true }] }],
    ['notes.list', { items: [{ ...folder, id: 0 }] }],
    ['notes.list', { items: [{ ...folder, id: 1.5 }] }],
    ['notes.list', { items: [{ ...folder, parent_id: 0 }] }],
    ['notes.list', { items: [{ ...folder, kind: 'file' }] }],
    ['notes.list', { item: [] }],
    ['notes.get', { id: 4, document: [] }],
    ['notes.get', { id: 4, document, extra: true }],
    ['notes.get', { id: 4 }],
    ['notes.save', { ok: true }],
    ['notes.delete', []],
    ['notes.create', { ...note, name: 1 }],
  ])('rejects a malformed %s reply', async (method, result) => {
    const client = clientFor({ [method]: () => result });
    const action = method === 'notes.list' ? client.listNoteItems()
      : method === 'notes.get' ? client.getNote(4)
      : method === 'notes.save' ? client.saveNote(4, document)
      : method === 'notes.delete' ? client.deleteNoteItem(4)
      : client.createNoteItem(null, 'folder', 'Plan');
    await expect(action).rejects.toBeInstanceOf(ChaProtocolError);
  });
});
