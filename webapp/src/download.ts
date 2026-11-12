interface WritableFile {
  write(contents: string): Promise<void>;
  close(): Promise<void>;
}

interface PickedFile {
  createWritable(): Promise<WritableFile>;
}

type SaveFilePicker = (options: {
  suggestedName: string;
  types: Array<{ description: string; accept: Record<string, string[]> }>;
}) => Promise<PickedFile>;

function saveFilePicker(): SaveFilePicker | undefined {
  return (window as Window & { showSaveFilePicker?: SaveFilePicker }).showSaveFilePicker;
}

type NativeSaveSession = (
  suggestedName: string,
  forumId: string,
  sessionId: string,
) => Promise<void>;

function nativeSaveSession(): NativeSaveSession | undefined {
  return (window as Window & { __CHA_NATIVE_SAVE_SESSION__?: NativeSaveSession })
    .__CHA_NATIVE_SAVE_SESSION__;
}

export function sessionMarkdownFilename(label: string): string {
  const safe = label
    .replace(/[<>:"/\\|?*#^\[\]\u0000-\u001f]/g, '-')
    .replace(/[ .]+$/g, '');
  return `${safe || 'session'}.md`;
}

let lastEpubUrl: string | undefined;

export function downloadEpub(title: string, bytes: Uint8Array<ArrayBuffer>): void {
  const url = URL.createObjectURL(new Blob([bytes], { type: 'application/epub+zip' }));
  // The host can open the Blob after the click returns.
  if (lastEpubUrl) URL.revokeObjectURL(lastEpubUrl);
  lastEpubUrl = url;
  const link = document.createElement('a');
  link.download = noteFolderFilename(title, 'epub');
  link.href = url;
  link.click();
}

export function noteFolderFilename(title: string, extension: 'epub' | 'sqlite'): string {
  return `${title.replace(/[<>:"/\\|?*\u0000-\u001f]/g, '-')}.${extension}`;
}

export async function saveMarkdownDownload(
  label: string,
  forumId: string,
  sessionId: string,
  load: () => Promise<string>,
): Promise<void> {
  const nativeSave = nativeSaveSession();
  if (nativeSave) {
    await nativeSave(sessionMarkdownFilename(label), forumId, sessionId);
    return;
  }
  const picker = saveFilePicker();
  if (picker) {
    let file: PickedFile;
    try {
      file = await picker.call(window, {
        suggestedName: sessionMarkdownFilename(label),
        types: [{ description: 'Markdown', accept: { 'text/markdown': ['.md'] } }],
      });
    } catch (failure: unknown) {
      if (failure instanceof DOMException && failure.name === 'AbortError') return;
      throw failure;
    }
    const contents = await load();
    const writable = await file.createWritable();
    await writable.write(contents);
    await writable.close();
    return;
  }

  const url = URL.createObjectURL(new Blob([await load()], { type: 'text/markdown' }));
  const link = document.createElement('a');
  link.download = sessionMarkdownFilename(label);
  link.href = url;
  link.click();
  URL.revokeObjectURL(url);
}
