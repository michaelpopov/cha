import { useEffect, useRef, useState } from 'react';
import type { Editor } from '@tiptap/core';
import type { Node as ProseMirrorNode } from '@tiptap/pm/model';
import { publicErrorMessage, type NoteAudioClient } from '../api/client';
import { fetchLocalResource } from '../textToSpeech';
import { beginSpeechPlayback, claimSpeechPlayback } from '../speechPlayback';
import { isNoteComment } from './editor';

type Block = { position: number; size: number; text: string };
export function noteAudioBlocks(document: ProseMirrorNode): Block[] {
  const blocks: Block[] = [];
  document.descendants((node, position) => {
    if (['codeBlock', 'image', 'horizontalRule'].includes(node.type.name) || Boolean(isNoteComment(node))) return false;
    if (node.type.name === 'heading' || node.type.name === 'paragraph') {
      let text = '';
      node.descendants(inline => {
        if (inline.isText) text += inline.text;
        else if (inline.type.name === 'hardBreak') text += '\n';
      });
      if (text.trim()) blocks.push({ position, size: node.nodeSize, text });
      return false;
    }
    return true;
  });
  return blocks;
}

type PlaybackState = 'stopped' | 'playing' | 'paused';
export function useNoteAudio({ client, id, editor, enabled, menuOpen, onError }: {
  client: NoteAudioClient | undefined; id: number | null; editor: Editor | null;
  enabled: boolean; menuOpen: boolean; onError(message: string): void;
}) {
  const [count, setCount] = useState(0);
  const [state, setState] = useState<PlaybackState>('stopped');
  const actions = useRef<{ play(index: number): void; stop(): void } | null>(null);
  const menu = useRef(menuOpen); menu.current = menuOpen;
  const report = useRef(onError); report.current = onError;

  useEffect(() => {
    setCount(0); setState('stopped');
    if (!client || id === null || !editor || editor.isDestroyed || !enabled) return;
    let disposed = false;
    const reload = () => { void client.info(id).then(info => { if (!disposed) setCount(info.count); }, () => {}); };
    reload();
    const changed = (event: Event) => {
      stop();
      if ((event as CustomEvent<number>).detail === id) reload();
    };
    const dom = editor.view.dom;
    const host = dom.closest<HTMLElement>('.cha-notes-document');
    let audio: HTMLAudioElement | null = null;
    let objectUrl: string | null = null;
    let resource: string | null = null;
    let request: AbortController | null = null;
    let active: HTMLElement | null = null;
    let serial = 0;
    let currentState: PlaybackState = 'stopped';
    let endSpeech: (() => void) | undefined;
    let releaseFocus: (() => void) | undefined;

    function transition(next: PlaybackState) {
      currentState = next;
      if (!disposed) setState(next);
    }
    function releaseClip() {
      request?.abort(); request = null;
      if (audio) {
        audio.onended = null; audio.onerror = null; audio.onplaying = null; audio.onpause = null;
        audio.pause(); audio.removeAttribute('src'); audio.load(); audio = null;
      }
      endSpeech?.(); endSpeech = undefined;
      if (objectUrl) URL.revokeObjectURL(objectUrl); objectUrl = null;
      if (resource) void client!.release(resource).catch(() => {}); resource = null;
    }
    function stop() {
      ++serial; transition('stopped'); releaseClip(); releaseFocus?.(); releaseFocus = undefined;
      active?.classList.remove('cha-note-audio-current'); active = null;
    }
    function reveal(block: HTMLElement) {
      if (!host) return;
      const top = block.getBoundingClientRect().top, viewport = host.getBoundingClientRect();
      if (top < viewport.top || top >= viewport.top + host.clientHeight)
        host.scrollTop += top - viewport.top - 20;
    }
    async function startClip(clip: HTMLAudioElement, token: number) {
      try {
        await clip.play();
        if (serial === token && currentState === 'paused') clip.pause();
      } catch (error) {
        if (disposed || serial !== token || (error instanceof DOMException && error.name === 'AbortError')) return;
        stop(); report.current(publicErrorMessage(error, 'Note audio could not be played.'));
      }
    }
    async function load(index: number, token: number) {
      try {
        const blocks = noteAudioBlocks(editor!.state.doc);
        if (index >= blocks.length) { stop(); return; }
        releaseClip();
        const source = await client!.source(id!, index);
        if (disposed || serial !== token) { await client!.release(source.resource_id); return; }
        resource = source.resource_id;
        request = new AbortController();
        const blob = await fetchLocalResource(source.url, request.signal);
        if (disposed || serial !== token) return;
        objectUrl = URL.createObjectURL(blob);
        const clip = new Audio(objectUrl);
        audio = clip;
        let revealed = false;
        clip.onended = () => { if (serial === token) void load(index + 1, token); };
        clip.onerror = () => {
          if (serial !== token) return;
          stop(); report.current('Note audio could not be played.');
        };
        clip.onplaying = () => {
          if (serial !== token || currentState !== 'playing') { clip.pause(); return; }
          endSpeech ??= beginSpeechPlayback();
          if (revealed) return;
          revealed = true;
          active?.classList.remove('cha-note-audio-current');
          active = editor!.view.nodeDOM(noteAudioBlocks(editor!.state.doc)[index]?.position ?? blocks[index].position) as HTMLElement | null;
          active?.classList.add('cha-note-audio-current');
          if (active) reveal(active);
        };
        clip.onpause = () => { endSpeech?.(); endSpeech = undefined; };
        if (currentState === 'playing') await startClip(clip, token);
      } catch (error) {
        if (disposed || serial !== token) return;
        stop(); report.current(publicErrorMessage(error, 'Note audio could not be played.'));
      }
    }
    function play(index: number) {
      if (currentState !== 'stopped') return;
      releaseFocus = claimSpeechPlayback(stop);
      transition('playing');
      void load(index, serial);
    }
    function keydown(event: KeyboardEvent) {
      if (event.code !== 'Space' && event.key !== ' ') return;
      if (currentState === 'stopped' || menu.current
        || (event.target as Element).closest('input,textarea,select,button,dialog,[role="menu"],[contenteditable="true"]')) return;
      event.preventDefault();
      if (event.repeat) return;
      if (currentState === 'playing') { transition('paused'); audio?.pause(); }
      else { transition('playing'); if (audio) void startClip(audio, serial); }
    }
    actions.current = { play, stop };
    dom.addEventListener('keydown', keydown);
    window.addEventListener('cha-note-audio-changed', changed);
    return () => {
      disposed = true; stop(); actions.current = null;
      dom.removeEventListener('keydown', keydown);
      window.removeEventListener('cha-note-audio-changed', changed);
    };
  }, [client, id, editor, enabled]);

  function blockAt(position: number) {
    if (!editor || !enabled) return -1;
    return noteAudioBlocks(editor.state.doc).findIndex(block => position >= block.position && position < block.position + block.size);
  }
  return {
    state,
    stop() { actions.current?.stop(); },
    canPlay(position: number) { const index = blockAt(position); return state === 'stopped' && index >= 0 && index < count; },
    play(position: number) { const index = blockAt(position); if (index >= 0 && index < count) actions.current?.play(index); },
  };
}
