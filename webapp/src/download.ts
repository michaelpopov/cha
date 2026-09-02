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

export function sessionMarkdownFilename(label: string): string {
  const safe = label
    .replace(/[<>:"/\\|?*#^\[\]\u0000-\u001f]/g, '-')
    .replace(/[ .]+$/g, '');
  return `${safe || 'session'}.md`;
}

export async function saveMarkdownDownload(
  label: string,
  load: () => Promise<string>,
): Promise<void> {
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
