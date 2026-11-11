// Real host smoke test with the local provider in test-note-summaries.py.
async function nativeParity() {
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const until = async (label, condition) => {
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline) {
      const value = await condition();
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
  const editor = () => document.querySelector('.cha-notes-document .ProseMirror')?.chaEditor;
  const action = async (name, label) => {
    const actionsMenu = () => document.querySelector('.cha-session-menu[role="menu"]');
    await click('Actions for ' + name, row(name));
    await click(label, await until('actions menu for ' + name, actionsMenu));
    await until('actions menu closed', () => !actionsMenu());
  };
  const completed = [];
  const dialog = () => document.querySelector('dialog[aria-label="Note summaries"][open]');
  const progress = text => until(text, () => dialog()?.innerText.includes(text) && dialog());
  const finish = () => until('summary dialog closed', () => !dialog());
  const select = async name => (await until(name, () => row(name))).click();
  const mode = async name => {
    await click('Note text mode');
    const menu = await until('text mode menu', () => document.querySelector('[role="menu"][aria-label="Note text mode"]'));
    await click(name, menu);
    await until(name + ' mode loaded', () => button('Note text mode')?.title === name + ' mode; showing ' + name);
  };
  const summaryText = version => until('Summary version ' + version, () => {
    const current = editor();
    return current?.getText().includes('Overview result ' + version) && current;
  });

  try {
    await until('startup', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    await click('Notes');
    await select('Single');
    await until('editable note', () => editor()?.isEditable);
    await action('Single', 'Summarize');
    const single = await progress('Summarizing “Single”…');
    check(document.activeElement === button('Cancel', single), 'Cancel did not receive focus');
    check(editor()?.isEditable === false, 'Expected a read-only editor during summary generation');
    await finish();
    await mode('L1');
    await until('L1 text', () => editor()?.getText() === 'L1 result');
    await mode('L2');
    await until('L2 text', () => editor()?.getText().includes('L2 result'));
    check(editor().getJSON().content.some(node => node.type === 'bulletList'), 'L2 list was lost');
    check(editor().getText().includes('Key\tValue\n\nColor\tBlue'), 'L2 table did not become plain text rows');
    completed.push('single-note dialog and L1/L2 editor modes');

    await select('Book');
    await action('Book', 'Summarize');
    await progress('Note 1 of 2');
    await progress('Note 2 of 2');
    await finish();
    await select('Summary');
    const first = await summaryText(1);
    const headings = first.getJSON().content.filter(node => node.type === 'heading')
      .map(node => node.content.map(child => child.text).join(''));
    check(headings.join('|') === 'A First chapter|B Second chapter', 'Unexpected Summary headings: ' + headings);
    for (const name of headings) {
      check(row(name)?.querySelector('[aria-label="Other versions saved"]'), 'Missing version dot for ' + name);
    }
    completed.push('folder progress, Summary headings, and version dots');

    await action('Book', 'Summarize');
    await progress('Note 1 of 2');
    await finish();
    const second = JSON.stringify((await summaryText(2)).getJSON());
    completed.push('folder Summary refreshed');

    await action('Book', 'Summarize');
    const delayed = await progress('Note 1 of 2');
    await until('provider holding the section request', async () =>
      (await fetch('/summary-request-held', { cache: 'no-store' })).ok);
    await click('Cancel', delayed);
    await finish();
    await select('A First chapter');
    await until('chapter opened', () => row('A First chapter')?.getAttribute('aria-current') === 'true'
      && editor()?.getText().includes('L2 result'));
    await select('Summary');
    check(JSON.stringify((await summaryText(2)).getJSON()) === second, 'Cancellation changed the Summary note');
    completed.push('folder cancellation preserved the Summary');

    window.__CHA_NATIVE_SET_LAYOUT__('split');
    const message = await until('chat composer', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    const send = async text => {
      Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set.call(message, text);
      message.dispatchEvent(new Event('input', { bubbles: true }));
      await click('Send message');
    };
    await send('##000001');
    await until('note attached', () => document.body.innerText.includes('Added note “Single” to this chat.'));
    await send('Check the attached note.');
    await until('chat reply', () => document.body.innerText.includes('Attachment received.'));
    completed.push('summarized note attached to chat');
    return { ok: true, completed };
  } catch (error) { return { ok: false, completed, reason: String(error) }; }
}
