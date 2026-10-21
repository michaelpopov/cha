import { Blob as NodeBlob } from 'node:buffer';
import type { Editor } from '@tiptap/core';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { noteClipboardCommand } from './clipboard';
import { createNotesEditor } from './editor';

let editor: Editor;
const originalCommand = Object.getOwnPropertyDescriptor(document, 'execCommand');

beforeEach(() => {
  editor = createNotesEditor({ content: '<p>A <strong>bold</strong> word</p>',
    readPicture: async () => new Uint8Array() });
  document.body.append(editor.view.dom);
  editor.commands.setTextSelection({ from: 3, to: 7 });
  editor.view.focus();
  Object.defineProperty(document, 'execCommand', { configurable: true, value: () => false });
  vi.stubGlobal('Blob', NodeBlob);
  vi.stubGlobal('ClipboardItem', class {
    constructor(public data: Record<string, Blob>) {}
    async getType(type: string) { return this.data[type]; }
  });
});

afterEach(() => {
  editor.destroy();
  document.body.replaceChildren();
  vi.unstubAllGlobals();
  if (originalCommand) Object.defineProperty(document, 'execCommand', originalCommand);
  else Reflect.deleteProperty(document, 'execCommand');
});

it('copies rich text and cuts only after the clipboard write succeeds', async () => {
  let finish!: () => void;
  const write = vi.fn((_items: ClipboardItem[]) => new Promise<void>((resolve) => { finish = resolve; }));
  vi.stubGlobal('navigator', { clipboard: { write } });
  const cut = noteClipboardCommand(editor, 'Cut', () => true);
  const copied = write.mock.calls[0][0][0];
  expect(await (await copied.getType('text/plain')).text()).toBe('bold');
  expect(await (await copied.getType('text/html')).text()).toContain('<strong>bold</strong>');
  expect(editor.getText()).toBe('A bold word');
  finish();
  await cut;
  expect(editor.getText()).toBe('A  word');
  editor.commands.undo();
  expect(editor.getText()).toBe('A bold word');
  expect(editor.view.dom.querySelector('strong')?.textContent).toBe('bold');
});

it('keeps the note intact when clipboard access fails or the note changes while cutting', async () => {
  vi.stubGlobal('navigator', { clipboard: { write: async () => { throw new Error('Denied'); } } });
  await expect(noteClipboardCommand(editor, 'Cut', () => true)).rejects.toThrow('Denied');
  expect(editor.getText()).toBe('A bold word');
  let finish!: () => void;
  vi.stubGlobal('navigator', { clipboard: { write: () => new Promise<void>((resolve) => { finish = resolve; }) } });
  const cut = noteClipboardCommand(editor, 'Cut', () => true);
  editor.view.dispatch(editor.state.tr.insertText('New ', 1));
  finish();
  await cut;
  expect(editor.getText()).toBe('New A bold word');
});

it('pastes rich clipboard content through the normal editor paste handler', async () => {
  vi.stubGlobal('DataTransfer', class {
    data = new Map<string, string>();
    files: File[] = [];
    items = { add: (file: File) => this.files.push(file) };
    get types() { return [...this.data.keys()]; }
    setData(type: string, value: string) { this.data.set(type, value); }
    getData(type: string) { return this.data.get(type) ?? ''; }
  });
  vi.stubGlobal('ClipboardEvent', class extends Event {
    clipboardData: DataTransfer | null;
    constructor(type: string, options: ClipboardEventInit) {
      super(type, options);
      this.clipboardData = options.clipboardData ?? null;
    }
  });
  vi.stubGlobal('navigator', { clipboard: { read: async () => [{
    types: ['text/html', 'text/plain'],
    getType: async (type: string) => new NodeBlob([type === 'text/html'
      ? '<p><em>pasted</em></p>' : 'pasted'], { type }),
  }] } });
  await noteClipboardCommand(editor, 'Paste', () => true);
  expect(editor.getText()).toBe('A pasted word');
  expect(editor.view.dom.querySelector('em')?.textContent).toBe('pasted');
  editor.commands.undo();
  expect(editor.getText()).toBe('A bold word');
});
