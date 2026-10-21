import { readFileBytes } from './notes/pictures';

export const textFileByteLimit = 256 * 1024;
export const textFileTooLarge = 'This file is too large to add to a chat. The limit is 256 KiB of text.';
export const textFileAccept = 'text/*,.txt,.md,.markdown,.mdown,.mkd,.csv,.tsv,.json,.yaml,.yml,.toml,.xml,.log,'
  + '.py,.js,.ts,.tsx,.jsx,.cpp,.h,.c,.rs,.go,.sh,.css,.html,.sql';

export async function readTextFile(file: File, tooLargeMessage = textFileTooLarge): Promise<string> {
  if (file.size > textFileByteLimit) throw new Error(tooLargeMessage);
  let bytes: Uint8Array;
  try {
    bytes = await readFileBytes(file).bytes;
  } catch {
    throw new Error('The file could not be read. Try again.');
  }
  let text: string;
  try {
    text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  } catch {
    throw new Error('Choose a UTF-8 text file.');
  }
  if (/[\x00-\x08\x0b\x0e-\x1f]/.test(text)) {
    throw new Error('Choose a UTF-8 text file.');
  }
  if (!text.trim()) throw new Error('The file is empty.');
  return text;
}
