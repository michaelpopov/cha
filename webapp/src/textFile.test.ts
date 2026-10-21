import { describe, expect, it } from 'vitest';

import { readTextFile, textFileByteLimit, textFileTooLarge } from './textFile';

describe('text file attachment', () => {
  it('reads UTF-8 text and removes its byte-order mark', async () => {
    const file = new File(['\ufeff# План\n\nПривет 👋'], 'plan.md', { type: 'text/markdown' });
    expect(await readTextFile(file)).toBe('# План\n\nПривет 👋');
    expect(await readTextFile(new File(['x'.repeat(textFileByteLimit)], 'limit.txt')))
      .toHaveLength(textFileByteLimit);
  });

  it('rejects oversized, empty, binary, and invalid UTF-8 files', async () => {
    await expect(readTextFile(new File(['x'.repeat(textFileByteLimit + 1)], 'large.txt')))
      .rejects.toThrow(textFileTooLarge);
    await expect(readTextFile(new File([' \n\t'], 'empty.txt'))).rejects.toThrow('The file is empty.');
    await expect(readTextFile(new File(['a\0b'], 'binary.txt'))).rejects.toThrow('Choose a UTF-8 text file.');
    await expect(readTextFile(new File([new Uint8Array([0xff, 0xfe, 0x80])], 'invalid.txt')))
      .rejects.toThrow('Choose a UTF-8 text file.');
  });
});
