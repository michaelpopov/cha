// Async body run inside the instrumented production WKWebView.
const check = (value, message) => { if (!value) throw new Error(message); };
const same = (a, b) => Object.keys(b).every(key => a[key] === b[key]);
const native = request => window.webkit.messageHandlers.chaPersistenceTest.postMessage(request);
const snapshot = () => native({ action: 'snapshot' });
const until = async (label, predicate) => {
  const deadline = Date.now() + 8000;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise(resolve => setTimeout(resolve, 10));
  }
  throw new Error('Timed out: ' + label);
};
const inverse = value => Object.fromEntries(Object.entries(value).map(([key, on]) => [key, !on]));
if (mode === 'reload' && phase > 0) expected = inverse(expected);
if (mode === 'failure-reload' && phase > 0) expected = { ...expected, sidebar: !expected.sidebar };
check(same(window.__CHA_NATIVE_INITIAL_VIEW_STATE__, expected), 'Incorrect bootstrap preferences');
await until('ready Chat', () => document.querySelector('textarea[aria-label="Message"]:not(:disabled)'));
await until('initial native report', async () => same((await snapshot()).first, expected));
check(same((await snapshot()).menu, expected), 'Incorrect initial native checkmarks');
const checkDOM = value => {
  check(document.querySelector('.cha-app').dataset.sidebar === (value.sidebar ? 'open' : 'closed'), 'Incorrect sidebar');
  check(Boolean(document.querySelector('.cha-main.is-chat.is-notes')) === value.split, 'Incorrect split layout');
  check(Boolean(document.querySelector('.cha-chat-toolbar')) === value.chat_toolbar, 'Incorrect Chat toolbar');
  const russian = document.querySelector('.cha-chat-column .cha-transliteration-toggle');
  check(Boolean(russian) === value.russian, 'Incorrect Russian controls');
  if (russian) check(russian.getAttribute('aria-pressed') === 'false', 'Typing mode was persisted');
};
checkDOM(expected);
const items = { split: 'Split view', sidebar: 'Sidebar', pictures: 'Pictures', toolbar: 'Toolbar',
  chat_toolbar: 'Chat toolbar', notes_prompt: 'Notes prompt', russian: 'Russian' };
const toggle = async key => {
  await native({ action: 'menu', title: items[key] });
};
if (mode === 'hold') {
  await native({ action: 'hold' });
  window.__CHA_NATIVE_POST__(JSON.stringify({ ...expected, native_action: 'view_state', connection_id: window.__CHA_NATIVE_CONNECTION_ID__ }));
  check(same((await snapshot()).menu, expected), 'Another instance changed live preferences');
} else if (mode === 'invalid') {
  const post = value => window.__CHA_NATIVE_POST__(JSON.stringify(value));
  const base = { ...expected, native_action: 'view_state', connection_id: window.__CHA_NATIVE_CONNECTION_ID__ };
  post({ ...base, sidebar: 0 });
  post({ ...base, connection_id: 'stale', sidebar: !expected.sidebar });
  const incomplete = { ...base, sidebar: !expected.sidebar };
  delete incomplete.russian;
  post(incomplete);
  check(same((await snapshot()).menu, expected), 'Invalid report changed checkmarks');
} else if (mode === 'change' || (mode === 'reload' && phase === 0)) {
  const changed = { ...expected };
  // The last change is observed through the DOM, then the probe returns
  // immediately to normal native quit, without polling the report or file.
  for (const key of Object.keys(items)) {
    await toggle(key);
    changed[key] = !changed[key];
    if (key !== 'russian') {
      await until('native ' + key, async () => same((await snapshot()).menu, changed));
    } else {
      await new Promise(resolve => {
        const done = () => Boolean(document.querySelector('.cha-chat-column .cha-transliteration-toggle')) === changed.russian;
        if (done()) return resolve();
        const observer = new MutationObserver(() => { if (done()) { observer.disconnect(); resolve(); } });
        observer.observe(document.body, { childList: true, subtree: true });
      });
    }
  }
  checkDOM(changed);
  if (mode === 'reload') return 'reload';
} else if (mode === 'failure' || mode === 'recovery' || (mode === 'failure-reload' && phase === 0)) {
  await native({ action: 'writable', value: false });
  await toggle('sidebar');
  const changed = { ...expected, sidebar: !expected.sidebar };
  await until('live change despite save failure', async () => same((await snapshot()).menu, changed));
  checkDOM(changed);
  if (mode === 'failure-reload') return 'reload';
  if (mode === 'recovery') {
    await native({ action: 'writable', value: true });
    await toggle('pictures');
    changed.pictures = !changed.pictures;
    await until('full snapshot after recovery', async () => same((await snapshot()).menu, changed));
  }
}
return 'quit';
