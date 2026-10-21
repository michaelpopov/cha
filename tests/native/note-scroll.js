// Run twice with the same disposable vault through CHA_NATIVE_PARITY_SCRIPT.
// The second process checks the position saved by the first close handshake.
async function nativeParity() {
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const check = (value, message) => { if (!value) throw new Error(message); };
  let step = 'startup';
  const completed = [];
  const until = async (label, condition) => {
    const deadline = Date.now() + 5000;
    while (Date.now() < deadline) {
      const error = document.querySelector('[role="alert"]');
      check(!error, error?.textContent);
      const value = condition();
      if (value) return value;
      await wait(25);
    }
    throw new Error('timed out waiting for ' + label);
  };
  const button = (name) => Array.from(document.querySelectorAll('button, [role="tab"]'))
    .find((node) => node.getClientRects().length && (
      node.getAttribute('aria-label') ?? node.querySelector('.cha-primary-line')?.textContent ?? node.textContent
    ).trim() === name);
  const click = async (name) => (await until(name, () => {
    const found = button(name);
    return found && !found.disabled && found;
  })).click();
  const row = (name) => Array.from(document.querySelectorAll('[role="treeitem"]'))
    .find((node) => node.querySelector('.cha-notes-label')?.textContent === name);
  const host = () => document.querySelector('.cha-notes-document');
  const editor = () => document.querySelector('.ProseMirror')?.chaEditor;
  const open = async (name, text) => {
    const previous = editor();
    (await until(name, () => row(name))).click();
    await until('opened ' + name, () => editor() && editor() !== previous
      && editor().getText().includes(text));
    await wait(75);
  };
  const at = (top) => check(Math.abs(host().scrollTop - top) <= 1,
    `expected position ${top}, got ${host().scrollTop}`);
  const create = async (name) => {
    await click('Actions for Test');
    await click('New note');
    const dialog = await until('New note dialog', () => document.querySelector('dialog[aria-label="New note"]'));
    const input = dialog.querySelector('input');
    Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set.call(input, name);
    input.dispatchEvent(new Event('input', { bubbles: true }));
    await click('Create');
    await until('new editor', () => editor()?.isEditable && editor().isEmpty);
    await wait(75);
    at(0);
  };
  const fill = async (prefix) => {
    editor().commands.setContent(Array.from({ length: 120 }, (_, index) => (
      `<p>${prefix} paragraph ${index + 1}: reading position.</p>`
    )).join(''));
    await wait(75);
    check(host().scrollHeight - host().clientHeight > 1500, 'note is not long enough');
  };
  try {
    await until('Chat ready', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
    await click('Notes');
    await until('Notes tree', () => row('Test'));
    await until('Notes loaded', () => button('Actions for Test') && !button('Actions for Test').disabled);
    if (row('Scroll first')) {
      step = 'restart';
      await open('Scroll first', 'First paragraph');
      at(1234);
      check(!editor().isEditable, 'read-only state was lost');
      return { ok: true, completed: ['close position survives a new app process'] };
    }

    step = 'note switching';
    await create('Scroll first');
    await fill('First');
    host().scrollTop = 600;
    await create('Scroll second');
    await fill('Second');
    host().scrollTop = 300;
    await open('Scroll first', 'First paragraph');
    at(600);
    await open('Scroll second', 'Second paragraph');
    at(300);
    await open('Scroll first', 'First paragraph');
    at(600);
    completed.push('independent positions and new notes start at top');

    step = 'read-only and settings';
    await click('Actions for Scroll first');
    await click('Make read-only');
    await until('read-only', () => editor() && !editor().isEditable
      && button('Actions for Scroll first') && !button('Actions for Scroll first').disabled);
    host().scrollTop = 800;
    await click('Settings');
    await until('Settings', () => document.querySelector('section[aria-label="Settings"]'));
    await open('Scroll first', 'First paragraph');
    at(800);
    check(!editor().isEditable, 'read-only note became editable');
    completed.push('unchanged read-only note and settings navigation');

    step = 'chat';
    host().scrollTop = 900;
    await click('Chat');
    const forum = await until('forum', () => document.querySelector('nav[aria-label="Forums"] .cha-side-action'));
    forum.click();
    await until('left note for chat', () => !host());
    await click('Notes');
    await open('Scroll first', 'First paragraph');
    at(900);
    completed.push('chat navigation');

    step = 'close handshake';
    host().scrollTop = 1234;
    const post = window.__CHA_NATIVE_POST__;
    let ready = false;
    window.__CHA_NATIVE_POST__ = (message) => {
      const value = JSON.parse(message);
      if (value.native_action === 'close_reply' && value.attempt_id === 9001) {
        ready = value.result === 'ready';
        return;
      }
      post(message);
    };
    try {
      window.__CHA_NATIVE_PREPARE_CLOSE__(9001);
      await until('close ready', () => ready);
      window.__CHA_NATIVE_CLOSE_ABORTED__(9001);
    } finally {
      window.__CHA_NATIVE_POST__ = post;
    }
    completed.push('close saves the latest position before replying ready');
    return { ok: true, completed };
  } catch (error) {
    return { ok: false, step, completed, reason: String(error.message || error) };
  }
}
