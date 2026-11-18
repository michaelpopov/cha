// Runs in the instrumented production host against the fake provider.
async function nativeParity(scenario) {
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
  const messageBox = () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)');
  const action = async (name, label) => {
    const actionsMenu = () => document.querySelector('.cha-session-menu[role="menu"]');
    await click('Actions for ' + name, row(name));
    await click(label, await until('actions menu for ' + name, actionsMenu));
    await until('actions menu closed', () => !actionsMenu());
  };
  const select = async name => (await until(name, () => row(name))).click();
  const mode = async (name, shown = name) => {
    await click('Note text mode');
    const menu = await until('text mode menu', () => document.querySelector('[role="menu"][aria-label="Note text mode"]'));
    await click(name, menu);
    await until(name + ' mode loaded', () => button('Note text mode')?.title === name + ' mode; showing ' + shown);
  };
  const summaryText = version => until('Summary version ' + version, () => {
    const current = editor();
    return current?.getText().includes('Overview result ' + version) && current;
  });
  const held = name => until(name, async () => (await fetch('/' + name, { cache: 'no-store' })).ok);
  const openEvents = async () => {
    await click('Settings');
    await click('Events');
  };
  const event = prefix => until(prefix, () => [...document.querySelectorAll('.cha-event-message')]
    .find(node => node.textContent.startsWith(prefix)));
  const finish = n => held('batch-finished-' + n);

  await until('startup', messageBox);
  if (scenario === 'reopen' || scenario === 'reopen-final') {
    await openEvents();
    await event('Summarization cancelled for “Book”');
    await click('Notes');
    await select('Book');
    await select('Summary');
    await summaryText(scenario === 'reopen' ? 2 : 3);
    return;
  }

  await click('Notes');
  if (scenario === 'retry') {
    await action('Single', 'Summarize');
    await held('retry-backoff');
    return;
  }
  if (scenario === 'between-steps' || scenario === 'final-commit') {
    let pendingReply = false;
    let overviewDelivered = false;
    const receive = window.__CHA_NATIVE_RECEIVE__;
    window.__CHA_NATIVE_RECEIVE__ = batch => {
      const messages = batch.messages.filter(message => {
        if (!message.ok || !message.result || pendingReply) return true;
        if (scenario === 'between-steps') {
          pendingReply = message.result.markdown === 'L1 result between steps';
        } else {
          pendingReply = overviewDelivered && Object.keys(message.result).length === 0;
          if (message.result.markdown === 'Overview result 3') overviewDelivered = true;
        }
        return !pendingReply;
      });
      receive({ ...batch, messages });
    };
    if (scenario === 'final-commit') await select('Book');
    await action(scenario === 'between-steps' ? 'Single' : 'Book', 'Summarize');
    // Keep delivery acknowledgements flowing so close negotiation still runs.
    await until(scenario + ' reply held', () => pendingReply);
    return;
  }

  await select('Single');
  await until('editable note', () => editor()?.isEditable);
  await action('Single', 'Summarize');
  await held('single-request-held');
  check(!document.querySelector('dialog[open]'), 'Summarize opened a dialog');
  check(editor()?.isEditable, 'Editor was frozen by the summary request');
  await select('Foreground');
  await until('unrelated note opened', () => editor()?.isEditable && editor().getText() === 'Foreground original');
  editor().commands.insertContent(' Foreground edit');
  await mode('L1', 'full text');
  window.__CHA_NATIVE_SET_LAYOUT__('split');
  await until('split layout', () => editor() && messageBox());
  window.__CHA_NATIVE_SET_LAYOUT__('single');
  await until('single layout', () => editor() && !messageBox());
  await openEvents();
  await event('Summarization started for “Single”');
  await click('Chat');
  const send = async text => {
    const message = await until('chat composer', messageBox);
    Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value').set.call(message, text);
    message.dispatchEvent(new Event('input', { bubbles: true }));
    await click('Send message');
  };
  await send('##000001');
  await until('note attached', () => document.body.innerText.includes('Added note “Single” to this chat.'));
  await send('Check the attached note.');
  const message = await until('chat composer', messageBox);
  message.focus();
  await until('chat reply', () => document.body.innerText.includes('Attachment received.'));
  await finish(1);
  check(document.activeElement === messageBox(), 'Summary completion moved focus out of Chat');

  await click('Notes');
  await select('Single');
  await until('L1 text', () => editor()?.getText() === 'L1 result');
  await mode('L2');
  await until('L2 text', () => editor()?.getText().includes('L2 result'));
  const [title] = editor().getJSON().content;
  check(title.type === 'heading' && title.content.map(child => child.text).join('') === 'Single', 'L2 title is missing');
  check(editor().getJSON().content.some(node => node.type === 'bulletList'), 'L2 list was lost');
  check(editor().getText().includes('Key\tValue\n\nColor\tBlue'), 'L2 table did not become plain text rows');

  await select('Book');
  await action('Book', 'Summarize');
  await finish(2);
  await select('Summary');
  const first = await summaryText(1);
  const headings = first.getJSON().content.filter(node => node.type === 'heading')
    .map(node => node.content.map(child => child.text).join(''));
  check(headings.join('|') === 'Book|A First chapter|B Second chapter', 'Unexpected Summary headings: ' + headings);
  for (const name of headings.slice(1)) {
    check(row(name)?.querySelector('[aria-label="Other versions saved"]'), 'Missing version dot for ' + name);
  }
  await action('Book', 'Summarize');
  await finish(3);
  await summaryText(2);

  await action('C Excluded', 'Exclude from summaries');
  await action('Book', 'Summarize');
  await held('summary-request-held');
  // The test host now calls NSApp.terminate, which negotiates with the editor.
}
