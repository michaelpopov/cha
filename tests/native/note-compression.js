// Real host smoke test with the delayed local provider in test-note-compression.py.
async function nativeParity() {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const until = async (label, condition) => {
    const deadline = Date.now() + 30000;
    while (Date.now() < deadline) {
      const value = condition();
      if (value) return value;
      await wait(25);
    }
    throw new Error('Timed out: ' + label + ': ' + document.body.innerText.slice(-1200));
  };
  const check = (condition, message) => { if (!condition) throw new Error(message); };
  const button = (name, root = document) => [...root.querySelectorAll('button,[role="tab"]')]
    .find(node => node.getClientRects().length && (node.getAttribute('aria-label') ?? node.textContent).trim() === name);
  const click = async (name, root = document) => (await until(name, () => {
    const node = button(name, root); return node && !node.disabled && node;
  })).click();
  const row = name => [...document.querySelectorAll('[role="treeitem"]')].find(node => node.getAttribute('aria-label') === name);
  const editor = () => document.querySelector('.ProseMirror')?.chaEditor;
  const action = async (name, label) => {
    await click('Actions for ' + name);
    await click(label, await until('menu', () => document.querySelector('[role="menu"]')));
    await until('menu closed', () => !document.querySelector('[role="menu"]'));
  };
  const completed = [];
  try {
    await until('startup', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    await click('Notes');
    (await until('Single', () => row('Single'))).click();
    await until('editor', () => editor()?.isEditable);
    await action('Single', 'Compress');
    await until('editor released', () => editor()?.isEditable);
    check(!document.querySelector('dialog'), 'Compression opened a modal');
    editor().commands.insertContent(' edited');
    await click('Chat');
    (await until('recent chat', () => document.querySelector('[aria-label="Recent sessions"] .cha-recent-open'))).click();
    await until('Chat during compression', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    await click('Notes');
    (await until('Single again', () => row('Single'))).click();
    await action('Single', 'Cancel compression');
    await action('Single', 'Compress');
    await until('replacement saved', () => row('Single')?.querySelector('.cha-notes-compressed'));
    completed.push('delayed acceptance, editing, navigation, Cancel and restart');

    await action('Batch', 'Compress');
    (await until('Batch', () => row('Batch'))).click();
    (await until('Nested folder', () => row('Nested folder'))).click();
    check(!row('B nested')?.querySelector('.cha-notes-compressed'), 'Waiting folder note finished before the active request');
    check(!row('A slow')?.querySelector('.cha-notes-compressed'), 'Delayed folder note finished too early');
    await click('Actions for Batch');
    let folderMenu = await until('folder menu', () => document.querySelector('[role="menu"]'));
    check(button('Compress', folderMenu)?.disabled, 'Folder Compress enabled while a descendant job was running');
    folderMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    await until('folder menu closed', () => !document.querySelector('[role="menu"]'));
    await action('A slow', 'Cancel compression');
    await until('nested completion after cancellation and retry', () => row('B nested')?.querySelector('.cha-notes-compressed'));
    await click('Actions for Batch');
    folderMenu = await until('folder menu after cancellation', () => document.querySelector('[role="menu"]'));
    await until('folder Compress enabled after last job retired', () => {
      const compress = button('Compress', folderMenu); return compress && !compress.disabled;
    });
    folderMenu.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true, cancelable: true }));
    await until('folder menu closed', () => !document.querySelector('[role="menu"]'));
    completed.push('folder Compress disabled for background jobs and enabled after cancellation');
    check(!document.querySelector('dialog'), 'Folder compression opened a modal');
    completed.push('recursive folder acceptance, serialized requests, bounded retry, per-note Cancel');
    await click('Settings', document.querySelector('aside'));
    await click('Events');
    await until('compression events', () => document.body.innerText.includes('Compressed version ready for “B nested”'));
    check(document.body.innerText.includes('Compression cancelled for “A slow”'), 'Cancellation event missing');
    completed.push('Events outcomes');
    return { ok: true, completed };
  } catch (error) { return { ok: false, completed, reason: String(error) }; }
}
