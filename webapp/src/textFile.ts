import { readFileBytes } from './notes/pictures';

export const textFileByteLimit = 256 * 1024;
export const textFileTooLarge = 'This file is too large to add to a chat. The limit is 256 KiB of text.';
export const textFileAccept = 'text/*,.txt,.md,.markdown,.mdown,.mkd,.csv,.tsv,.json,.yaml,.yml,.toml,.xml,.log,'
  + '.py,.js,.ts,.tsx,.jsx,.cpp,.h,.c,.rs,.go,.sh,.css,.html,.sql';

export function validateTextFile(text: string, tooLargeMessage: string, allowEmpty = false): void {
  if (new TextEncoder().encode(text).length > textFileByteLimit) throw new Error(tooLargeMessage);
  if (/[\x00-\x08\x0b\x0e-\x1f\ud800-\udfff]/u.test(text)) {
    throw new Error('Choose a UTF-8 text file.');
  }
  if (!allowEmpty && !text.trim()) throw new Error('The file is empty.');
}

export async function readTextFile(
  file: File,
  tooLargeMessage = textFileTooLarge,
  { allowEmpty = false, preserveBOM = false } = {},
): Promise<string> {
  if (file.size > textFileByteLimit) throw new Error(tooLargeMessage);
  let bytes: Uint8Array;
  try {
    bytes = await readFileBytes(file).bytes;
  } catch {
    throw new Error('The file could not be read. Try again.');
  }
  let text: string;
  try {
    text = new TextDecoder('utf-8', { fatal: true, ignoreBOM: preserveBOM }).decode(bytes);
  } catch {
    throw new Error('Choose a UTF-8 text file.');
  }
  validateTextFile(text, tooLargeMessage, allowEmpty);
  return text;
}
