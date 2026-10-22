import type { Editor } from '@tiptap/core';
import { trackSelectionBookmark, withIsolatedHistory } from './editor';

export async function noteClipboardCommand(
  editor: Editor,
  command: 'Cut' | 'Copy' | 'Copy All' | 'Paste',
  available: () => boolean,
): Promise<void> {
  if (command === 'Copy All') {
    const { selection, storedMarks } = editor.state;
    try {
      editor.commands.selectAll();
      // Copy captures its content before waiting for clipboard access. Restore
      // the selection now so the user can keep reading or typing while it waits.
      return noteClipboardCommand(editor, 'Copy', available);
    } finally {
      if (!editor.isDestroyed) {
        editor.view.dispatch(editor.state.tr.setSelection(selection).setStoredMarks(storedMarks));
      }
    }
  }

  // Native clipboard commands keep the editor's rich-text copy/paste handlers.
  // Some hosts allow only the asynchronous Clipboard API for Paste.
  if (withIsolatedHistory(editor, () => document.execCommand?.(command.toLowerCase()) ?? false)) return;

  const selection = trackSelectionBookmark(editor);
  const documentBefore = editor.state.doc;
  try {
    if (command === 'Paste') {
      const data = new DataTransfer();
      const items = await navigator.clipboard.read();
      for (const item of items) {
        for (const type of item.types) {
          if (type === 'text/html' || type === 'text/plain') {
            data.setData(type, await (await item.getType(type)).text());
          } else if (['image/png', 'image/jpeg', 'image/gif', 'image/webp'].includes(type)) {
            data.items.add(new File([await item.getType(type)], 'clipboard', { type }));
          }
        }
      }
      if (!available()) return;
      editor.view.dispatch(editor.state.tr.setSelection(selection.current().resolve(editor.state.doc)));
      editor.view.dom.dispatchEvent(new ClipboardEvent('paste', {
        clipboardData: data, bubbles: true, cancelable: true,
      }));
    } else {
      const copied = editor.view.serializeForClipboard(editor.state.selection.content());
      await navigator.clipboard.write([new ClipboardItem({
        'text/html': new Blob([copied.dom.innerHTML], { type: 'text/html' }),
        'text/plain': new Blob([copied.text], { type: 'text/plain' }),
      })]);
      // Do not cut text that changed while the clipboard write was pending.
      if (command === 'Cut' && available() && editor.state.doc.eq(documentBefore)) {
        withIsolatedHistory(editor, () => {
          editor.view.dispatch(editor.state.tr
            .setSelection(selection.current().resolve(editor.state.doc)).deleteSelection());
          return true;
        });
      }
    }
  } finally {
    selection.stop();
  }
}
