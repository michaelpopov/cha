import { getSchema, type Editor } from '@tiptap/core';
import StarterKit from '@tiptap/starter-kit';
import { act, cleanup, fireEvent, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import type { NoteAudioClient } from '../api/client';
import { claimSpeechPlayback } from '../speechPlayback';
import { fetchLocalResource } from '../textToSpeech';
import { createNotesEditor, notesExtensions } from './editor';
import { noteAudioBlocks, useNoteAudio } from './useNoteAudio';

vi.mock('../textToSpeech', () => ({ fetchLocalResource: vi.fn() }));
const paragraph = (text: string) => ({ type: 'paragraph', content: [{ type: 'text', text }] });
class AudioClip {
  static clips: AudioClip[] = [];
  onended: (() => void) | null = null;
  onerror: (() => void) | null = null;
  onplaying: (() => void) | null = null;
  onpause: (() => void) | null = null;
  currentTime = 0;
  play = vi.fn(async () => { this.onplaying?.(); });
  pause = vi.fn(() => { this.onpause?.(); });
  removeAttribute = vi.fn();
  load = vi.fn();
  constructor() { AudioClip.clips.push(this); }
}
let editor: Editor;
let host: HTMLDivElement;
function setup() {
  host = document.createElement('div'); host.className = 'cha-notes-document'; document.body.appendChild(host);
  editor = createNotesEditor({ element: host.appendChild(document.createElement('div')),
    content: { type: 'doc', content: [paragraph('First'), paragraph('Second'), paragraph('Third')] },
    readPicture: async () => new Uint8Array() });
  editor.setEditable(false);
  const client: NoteAudioClient = {
    cancel: vi.fn(),
    startBackground: vi.fn(), backgroundStatus: vi.fn(),
    info: vi.fn().mockResolvedValue({ count: 3 }), start: vi.fn(), status: vi.fn(),
    source: vi.fn(async (_id, ordinal) => ({ resource_id: `r${ordinal}`, url: `/media/r${ordinal}`, mime_type: 'audio/mp4', byte_length: 10 })),
    release: vi.fn().mockResolvedValue(undefined),
  };
  const error = vi.fn();
  const hook = renderHook(({ enabled, menuOpen }) => useNoteAudio({ client, id: 4, editor, enabled, menuOpen, onError: error }),
    { initialProps: { enabled: true, menuOpen: false } });
  return { ...hook, client, error };
}
async function flush() { await act(async () => {}); }
function space(target = editor.view.dom, repeat = false) {
  return fireEvent.keyDown(target, { code: 'Space', key: ' ', repeat });
}
beforeEach(() => {
  AudioClip.clips = [];
  vi.stubGlobal('Audio', AudioClip);
  vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:clip');
  vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
  vi.mocked(fetchLocalResource).mockResolvedValue(new Blob(['audio'], { type: 'audio/mp4' }));
});
afterEach(() => {
  cleanup(); editor?.destroy(); host?.remove(); vi.restoreAllMocks(); vi.unstubAllGlobals(); vi.clearAllMocks();
});

describe('note narration blocks', () => {
  it('uses reading order, preserves inline text and hard breaks, and excludes comments and nontext blocks', () => {
    const document = getSchema(notesExtensions().map(extension => extension.name === 'starterKit' ? StarterKit.configure({ strike: false, trailingNode: false }) : extension)).nodeFromJSON({ type: 'doc', content: [
      { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: 'Title' }] },
      { type: 'paragraph', content: [{ type: 'text', text: '  Link', marks: [{ type: 'link', attrs: { href: 'https://example.com' } }] },
        { type: 'hardBreak' }, { type: 'text', text: 'text  ', marks: [{ type: 'bold' }] }] },
      { type: 'bulletList', content: [{ type: 'listItem', content: [paragraph('Repeat'),
        { type: 'orderedList', content: [{ type: 'listItem', content: [paragraph('Nested')] }] }] }] },
      { type: 'blockquote', content: [paragraph('Repeat')] },
      { type: 'paragraph', content: [{ type: 'text', text: 'Comment', marks: [{ type: 'highlight', attrs: { color: '#b3d9ff' } }] }] },
      { type: 'paragraph', content: [{ type: 'image', attrs: { src: 'note-picture:test' } }] },
      { type: 'codeBlock', content: [{ type: 'text', text: 'Skipped code' }] }, { type: 'horizontalRule' }, paragraph(' \n\t '),
    ] });
    const blocks = noteAudioBlocks(document);
    expect(blocks.map(block => block.text)).toEqual(['Title', '  Link\ntext  ', 'Repeat', 'Nested', 'Repeat']);
    for (const block of blocks) expect(document.nodeAt(block.position)?.nodeSize).toBe(block.size);
  });
});

describe('note playlist', () => {
  it('starts at the clicked middle block, plays in order, and stops at the end', async () => {
    const { result, client } = setup(); await flush();
    act(() => result.current.play(noteAudioBlocks(editor.state.doc)[1].position)); await flush();
    expect(client.source).toHaveBeenLastCalledWith(4, 1);
    expect(result.current.state).toBe('playing');
    expect(editor.view.dom.children[1]).toHaveClass('cha-note-audio-current');
    act(() => AudioClip.clips[0].onended?.()); await flush();
    expect(client.source).toHaveBeenLastCalledWith(4, 2);
    act(() => AudioClip.clips[1].onended?.()); await flush();
    expect(result.current.state).toBe('stopped');
    expect(editor.view.dom.querySelector('.cha-note-audio-current')).toBeNull();
    expect(client.source).toHaveBeenCalledTimes(2);
    expect(client.release).toHaveBeenCalledWith('r1');
    expect(client.release).toHaveBeenCalledWith('r2');
  });

  it('pauses at the same position, ignores repeat and click, and reveals only once per clip', async () => {
    const { result } = setup(); await flush();
    const block = editor.view.dom.children[0] as HTMLElement;
    vi.spyOn(host, 'getBoundingClientRect').mockReturnValue({ top: 0 } as DOMRect);
    Object.defineProperty(host, 'clientHeight', { value: 300 });
    vi.spyOn(block, 'getBoundingClientRect').mockReturnValue({ top: 500 } as DOMRect);
    act(() => result.current.play(0)); await flush();
    expect(host.scrollTop).toBe(480);
    const audio = AudioClip.clips[0]; audio.currentTime = 12;
    act(() => { expect(space()).toBe(false); });
    expect(result.current.state).toBe('paused');
    expect(block).toHaveClass('cha-note-audio-current');
    host.scrollTop = 0;
    act(() => { expect(space(editor.view.dom, true)).toBe(false); fireEvent.click(block); });
    expect(result.current.state).toBe('paused');
    act(() => space()); await flush();
    expect(result.current.state).toBe('playing'); expect(audio.currentTime).toBe(12);
    expect(host.scrollTop).toBe(0);
  });

  it('pauses between clips, retains speech ownership, and cannot resume after chat takeover', async () => {
    const { result, client } = setup(); await flush();
    act(() => result.current.play(0)); await flush();
    let resolve!: (value: Awaited<ReturnType<NoteAudioClient['source']>>) => void;
    vi.mocked(client.source).mockImplementationOnce(() => new Promise(done => { resolve = done; }));
    act(() => AudioClip.clips[0].onended?.());
    act(() => space());
    expect(result.current.state).toBe('paused');
    await act(async () => resolve({ resource_id: 'next', url: '/media/next', mime_type: 'audio/mp4', byte_length: 10 }));
    expect(AudioClip.clips[1].play).not.toHaveBeenCalled();
    let release!: () => void;
    act(() => { release = claimSpeechPlayback(vi.fn()); });
    expect(result.current.state).toBe('stopped');
    act(() => { expect(space()).toBe(true); });
    expect(AudioClip.clips[1].play).not.toHaveBeenCalled();
    release();
  });

  it('releases late sources after stop without touching a new playlist', async () => {
    const { result, client } = setup(); await flush();
    let resolve!: (value: Awaited<ReturnType<NoteAudioClient['source']>>) => void;
    vi.mocked(client.source).mockImplementationOnce(() => new Promise(done => { resolve = done; }));
    act(() => result.current.play(0));
    act(() => result.current.stop());
    act(() => result.current.play(noteAudioBlocks(editor.state.doc)[1].position)); await flush();
    await act(async () => resolve({ resource_id: 'stale', url: '/media/stale', mime_type: 'audio/mp4', byte_length: 10 }));
    expect(client.release).toHaveBeenCalledWith('stale');
    expect(result.current.state).toBe('playing');
    expect(AudioClip.clips).toHaveLength(1);
  });

  it('ignores unrelated completion, but stops matching playback synchronously and refreshes without autoplay', async () => {
    const { result, client } = setup(); await flush();
    act(() => result.current.play(0)); await flush();
    act(() => window.dispatchEvent(new CustomEvent('cha-note-audio-changed', { detail: { id: 5, affectedNoteOnly: true } })));
    expect(result.current.state).toBe('playing');
    expect(client.info).toHaveBeenCalledTimes(1);
    act(() => window.dispatchEvent(new CustomEvent('cha-note-audio-changed', { detail: { id: 4, affectedNoteOnly: true } })));
    expect(result.current.state).toBe('stopped');
    expect(AudioClip.clips[0].pause).toHaveBeenCalled();
    await flush();
    expect(client.info).toHaveBeenCalledTimes(2);
    expect(AudioClip.clips).toHaveLength(1);
  });

  it('discards a source pending across completion and releases its resource', async () => {
    const { result, client } = setup(); await flush();
    let resolve!: (value: Awaited<ReturnType<NoteAudioClient['source']>>) => void;
    vi.mocked(client.source).mockReturnValueOnce(new Promise(done => { resolve = done; }));
    act(() => result.current.play(0));
    act(() => window.dispatchEvent(new CustomEvent('cha-note-audio-changed', { detail: { id: 4, affectedNoteOnly: true } })));
    await act(async () => resolve({ resource_id: 'late', url: '/media/late', mime_type: 'audio/mp4', byte_length: 10 }));
    expect(client.release).toHaveBeenCalledWith('late');
    expect(AudioClip.clips).toHaveLength(0);
    expect(result.current.state).toBe('stopped');
  });

  it('stops on dialog opening for another note and on transition to editable mode', async () => {
    const { result, rerender } = setup(); await flush();
    act(() => result.current.play(0)); await flush();
    act(() => window.dispatchEvent(new CustomEvent('cha-note-audio-changed', { detail: 9 })));
    expect(result.current.state).toBe('stopped');
    act(() => result.current.play(0)); await flush();
    rerender({ enabled: false, menuOpen: false });
    expect(result.current.state).toBe('stopped');
    expect(editor.view.dom.querySelector('.cha-note-audio-current')).toBeNull();
  });

  it('leaves Space in menus and inputs unchanged and stops on a clip error', async () => {
    const { result, rerender, error } = setup(); await flush();
    act(() => result.current.play(0)); await flush();
    const input = editor.view.dom.appendChild(document.createElement('input'));
    act(() => { expect(space(input)).toBe(true); });
    expect(result.current.state).toBe('playing');
    rerender({ enabled: true, menuOpen: true });
    act(() => { expect(space()).toBe(true); });
    expect(result.current.state).toBe('playing');
    act(() => AudioClip.clips[0].onerror?.());
    expect(result.current.state).toBe('stopped');
    expect(error).toHaveBeenCalledWith('Note audio could not be played.');
  });
});
