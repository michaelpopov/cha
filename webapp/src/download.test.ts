import { afterEach, describe, expect, it, vi } from 'vitest';

import { saveMarkdownDownload, sessionMarkdownFilename } from './download';

afterEach(() => {
  Reflect.deleteProperty(window, 'showSaveFilePicker');
  vi.restoreAllMocks();
});

describe('session Markdown downloads', () => {
  it('uses a filesystem-safe default name based on the session name', () => {
    expect(sessionMarkdownFilename('Architecture: review?')).toBe('Architecture- review-.md');
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

    await saveMarkdownDownload('Planning', async () => {
      order.push('load');
      return '# Planning\n';
    });

    expect(picker).toHaveBeenCalledWith(expect.objectContaining({ suggestedName: 'Planning.md' }));
    expect(write).toHaveBeenCalledWith('# Planning\n');
    expect(order).toEqual(['pick', 'load', 'write', 'close']);
  });

  it('does not load anything when the save dialog is cancelled', async () => {
    Object.defineProperty(window, 'showSaveFilePicker', {
      configurable: true,
      value: vi.fn(async () => { throw new DOMException('Cancelled', 'AbortError'); }),
    });
    const load = vi.fn(async () => '# Planning\n');
    await saveMarkdownDownload('Planning', load);
    expect(load).not.toHaveBeenCalled();
  });
});
