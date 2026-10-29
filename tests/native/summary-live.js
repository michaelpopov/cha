// Executed by the existing native parity host. The runner supplies summaryLive.
async function nativeParity() {
  const capture = { source: summaryLive.source, answers: [], completed: [] };
  const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
  const visible = node => node && node.getClientRects().length;
  const until = async (label, condition, timeout = 30000) => {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      const value = condition();
      if (value) return value;
      const error = [...document.querySelectorAll('[role="alert"],.cha-message.is-error')].find(visible);
      if (error) throw new Error(label + ': ' + error.innerText);
      await wait(50);
    }
    throw new Error('Timed out: ' + label + ': ' + document.body.innerText.slice(-1800));
  };
  const check = (value, message) => { if (!value) throw new Error(message); };
  const button = (name, root = document) => [...root.querySelectorAll('button,[role="tab"]')]
    .find(node => visible(node) && (node.getAttribute('aria-label') ?? node.textContent).trim() === name);
  const click = async (name, root = document) => (await until(name, () => {
    const node = button(name, root); return node && !node.disabled && node;
  })).click();
  const value = async (node, text) => {
    const prototype = node.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype
      : node.tagName === 'SELECT' ? HTMLSelectElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(prototype, 'value').set.call(node, text);
    node.dispatchEvent(new Event(node.tagName === 'SELECT' ? 'change' : 'input', { bubbles: true }));
    await wait(100);
  };
  const composer = () => [...document.querySelectorAll('textarea[aria-label="Message"]')].find(visible);
  const editor = () => [...document.querySelectorAll('.ProseMirror')].find(visible)?.chaEditor;
  const row = name => [...document.querySelectorAll('[role="treeitem"]')]
    .find(node => visible(node) && node.getAttribute('aria-label') === name);
  const newSession = async character => {
    await click('Chat');
    const previous = location.hash;
    await click('Actions for forum The Lobby');
    await click('Create new session');
    await until('new empty conversation', () => location.hash !== previous && composer() && !composer().disabled
      && !document.querySelector('.cha-message.is-human'));
    const select = await until('Character selector', () => document.querySelector('select[aria-label="Character"]:not(:disabled)'));
    await value(select, character);
    await until('character selected', () => select.value === character);
    return location.hash;
  };
  const submit = async text => {
    const before = document.querySelectorAll('.cha-message.is-character').length;
    await value(await until('composer', () => composer() && !composer().disabled && composer()), text);
    await click('Send message');
    await until('completed reply', () => {
      const replies = [...document.querySelectorAll('.cha-message.is-character')];
      return replies.length > before && replies.at(-1).dataset.status === 'complete'
        && !document.querySelector('[aria-label="Stop generation"]') && replies.at(-1);
    }, 180000);
    return [...document.querySelectorAll('.cha-message.is-character')].at(-1).querySelector('.cha-message-text').innerText;
  };
  const openSummary = async () => {
    await click('Notes');
    for (const name of ['Forums', 'The Lobby']) {
      const folder = await until(name + ' folder', () => row(name));
      if (folder.getAttribute('aria-expanded') !== 'true') folder.click();
    }
    const target = await until('summary note', () => row(summaryLive.title), 180000);
    const previous = editor();
    const alreadyOpen = target.getAttribute('aria-current') === 'true';
    target.click();
    await until('summary editor', () => editor() && (alreadyOpen || editor() !== previous)
      && editor().getText().includes('Long summary'));
    return editor();
  };
  try {
    await until('startup', () => composer() && !composer().disabled);
    capture.sourceSession = await newSession('guide');
    for (const text of summaryLive.source) await submit(text);
    const recent = document.querySelector('.cha-recent-row.is-current .cha-recent-more');
    check(recent, 'Current recent session is missing');
    recent.click();
    await click('Rename…');
    await value(await until('Session name', () => document.querySelector('#cha-rename-session')), summaryLive.title);
    await click('Rename', document.querySelector('dialog'));
    await until('rename saved', () => button('Actions for ' + summaryLive.title));
    await click('Actions for ' + summaryLive.title);
    await click('Save summary');
    // Wait for the committed note before loading Events, which is a static snapshot.
    await openSummary();
    await click('Settings', document.querySelector('aside'));
    await click('Events');
    await until('summary success', () => document.body.innerText.includes('Summary saved for “' + summaryLive.title + '”'), 180000);
    capture.completed.push('Save summary success event');
    let current = await openSummary();
    check(!current.isEditable, 'Generated summary is not read-only');
    capture.beforeComment = current.getJSON();
    capture.completed.push('Generated note opened read-only');
    await value(await until('Notes Prompt', () => document.querySelector('textarea[aria-label="Prompt"]')), summaryLive.comment);
    const paragraph = [...current.view.dom.querySelectorAll('p')].at(-1);
    check(paragraph, 'No summary paragraph to comment on');
    paragraph.dispatchEvent(new MouseEvent('contextmenu', { bubbles: true, cancelable: true,
      clientX: paragraph.getBoundingClientRect().left + 5, clientY: paragraph.getBoundingClientRect().top + 5 }));
    await click('Add comment');
    await until('comment added', () => editor()?.getText().includes(summaryLive.code));
    capture.afterComment = editor().getJSON();
    // ANNOTATIONS appears when the real autosave commits the first comment.
    (await until('comment autosave', () => row('ANNOTATIONS'))).click();
    await until('annotation editor opened', () => editor() && editor() !== current);
    current = await openSummary();
    check(current.getText().includes(summaryLive.code), 'Comment did not survive reopening');
    capture.reopened = current.getJSON();
    capture.completed.push('Comment saved and reopened');
    if (summaryLive.primary && !summaryLive.natural) {
      const session = await newSession('live_reader');
      const prompt = 'Use summary_read on the Kestrel conversation listed in the supplied summary catalogue. What acknowledgement code is in its later comment?';
      const answer = { scenario: 'direct', session, prompt };
      capture.answers.push(answer);
      answer.answer = await submit(prompt);
    }
    const session = await newSession('live_reader');
    const prompt = summaryLive.natural
      ? 'What acknowledgement code was recorded for ' + summaryLive.tag + '?'
      : 'Call summary_search with query ' + summaryLive.tag
        + ' and before_note_id: null. Then call summary_read on the matching note and report its later comment.';
    const answer = { scenario: summaryLive.natural ? 'natural' : 'search', session, prompt };
    capture.answers.push(answer);
    answer.answer = await submit(prompt);
    capture.completed.push('Fresh consumers completed');
    return { ok: true, ...capture };
  } catch (error) {
    return { ok: false, ...capture, reason: String(error), screen: document.body.innerText.slice(-4000) };
  }
}
