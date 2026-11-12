import { afterEach, describe, expect, it, vi } from 'vitest';

import { downloadEpub, saveMarkdownDownload, sessionMarkdownFilename } from './download';

afterEach(() => {
  Reflect.deleteProperty(window, 'showSaveFilePicker');
  Reflect.deleteProperty(window, '__CHA_NATIVE_SAVE_SESSION__');
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('EPUB downloads', () => {
  it('saves the bytes under the folder filename and releases a URL only at the next export', async () => {
    const blobs: Blob[] = [];
    const revoked: string[] = [];
    vi.stubGlobal('URL', class extends URL {
      static createObjectURL = (blob: Blob) => `blob:${blobs.push(blob)}`;
      static revokeObjectURL = (url: string) => { revoked.push(url); };
    });
    const clicks: string[][] = [];
    vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(function (this: HTMLAnchorElement) {
      clicks.push([this.download, this.getAttribute('href')!]);
    });

    downloadEpub('History: L1?', new Uint8Array([1, 2]));
    expect(revoked).toEqual([]);
    downloadEpub('Next', new Uint8Array([3]));
    expect(clicks).toEqual([['History- L1-.epub', 'blob:1'], ['Next.epub', 'blob:2']]);
    expect(revoked).toEqual(['blob:1']);
    expect(blobs[0].type).toBe('application/epub+zip');
    expect(new Uint8Array(await blobs[0].arrayBuffer())).toEqual(new Uint8Array([1, 2]));
  });
});

describe('session Markdown downloads', () => {
  it('uses a filesystem-safe default name based on the session name', () => {
    expect(sessionMarkdownFilename('Architecture: review?')).toBe('Architecture- review-.md');
    expect(sessionMarkdownFilename('Notes #[draft]^')).toBe('Notes --draft--.md');
    expect(sessionMarkdownFilename('Trailing. ')).toBe('Trailing.md');
  });

  it('opens the native save picker before loading and writes the Markdown', async () => {
    const order: string[] = [];
    const write = vi.fn(async () => { order.push('write'); });
    const close = vi.fn(async () => { order.push('close'); });
    const picker = vi.fn(async () => {
      order.push('pick');
      return { createWritable: async () => ({ write, close }) };
    });
    Object.defineProperty(window, 'showSaveFilePicker', { configurable: true, value: picker });

    await saveMarkdownDownload('Planning', 'lobby', 'planning', async () => {
      order.push('load');
      return '# Planning\n';
    });

    expect(picker).toHaveBeenCalledWith(expect.objectContaining({ suggestedName: 'Planning.md' }));
    expect(write).toHaveBeenCalledWith('# Planning\n');
    expect(order).toEqual(['pick', 'load', 'write', 'close']);
  });

  it('uses the native save action when the host provides one', async () => {
    const save = vi.fn(async () => undefined);
    Object.defineProperty(window, '__CHA_NATIVE_SAVE_SESSION__', {
      configurable: true,
      value: save,
    });
    const load = vi.fn(async () => '# Planning\n');
    await saveMarkdownDownload('Planning', 'lobby', 'planning', load);
    expect(save).toHaveBeenCalledWith('Planning.md', 'lobby', 'planning');
    expect(load).not.toHaveBeenCalled();
  });

  it('does not load anything when the save dialog is cancelled', async () => {
    Object.defineProperty(window, 'showSaveFilePicker', {
      configurable: true,
      value: vi.fn(async () => { throw new DOMException('Cancelled', 'AbortError'); }),
    });
    const load = vi.fn(async () => '# Planning\n');
    await saveMarkdownDownload('Planning', 'lobby', 'planning', load);
    expect(load).not.toHaveBeenCalled();
  });
});
