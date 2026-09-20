// Exercise the document's policy through DOM operations and require violation
// events: a failed network request alone could just mean the host is offline.
async function nativePolicyChecks() {
  const violations = [];
  const record = (event) => violations.push(event);
  document.addEventListener('securitypolicyviolation', record);
  const inline = document.createElement('script');
  const remote = document.createElement('script');
  const button = document.createElement('button');
  const scriptUrl = 'https://cha-csp-test.invalid/disallowed.js';
  const fetchUrl = 'https://cha-csp-test.invalid/disallowed-fetch';
  window.__CHA_POLICY_INLINE__ = false;
  window.__CHA_POLICY_HANDLER__ = false;
  try {
    inline.textContent = 'window.__CHA_POLICY_INLINE__ = true;';
    document.head.append(inline);
    button.setAttribute('onclick', 'window.__CHA_POLICY_HANDLER__ = true;');
    document.body.append(button);
    button.click();
    remote.src = scriptUrl;
    document.head.append(remote);
    void fetch(fetchUrl).catch(() => {});
    const blocked = (directive, uri) => violations.some((event) =>
      event.effectiveDirective.startsWith(directive) && event.blockedURI === uri
      && event.disposition === 'enforce');
    const deadline = Date.now() + 3000;
    while (!(blocked('script-src', 'inline') && blocked('script-src', scriptUrl)
        && blocked('connect-src', fetchUrl))) {
      if (Date.now() >= deadline) throw new Error('missing enforced CSP violations: '
        + JSON.stringify(violations.map((event) => [event.effectiveDirective, event.blockedURI])));
      await new Promise((resolve) => setTimeout(resolve, 25));
    }
    if (window.__CHA_POLICY_INLINE__ || window.__CHA_POLICY_HANDLER__) {
      throw new Error('injected JavaScript executed');
    }
    if ((await fetch('/probe/audio', {cache: 'no-store'})).status !== 404) {
      throw new Error('prototype audio endpoint remains available');
    }
  } finally {
    document.removeEventListener('securitypolicyviolation', record);
    inline.remove();
    remote.remove();
    button.remove();
    delete window.__CHA_POLICY_INLINE__;
    delete window.__CHA_POLICY_HANDLER__;
  }
}

// Shared real-host bridge checks. Runs only against the harness's disposable
// vault. This probe owns delivery acknowledgements; reload before using the UI
// again because instrumented request IDs share the document's monotonic range.
async function nativeParity() {
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const deadline = Date.now() + 15000;
  while (!document.querySelector('textarea[aria-label="Message"]:not(:disabled)')) {
    if (Date.now() > deadline) return {ok: false, reason: 'composer not ready'};
    await wait(25);
  }
  await wait(100);
  const connection = window.__CHA_NATIVE_CONNECTION_ID__;
  const pending = new Map();
  let epoch = 0;
  let next = 9000000000000;
  const post = (value) => window.__CHA_NATIVE_POST__(JSON.stringify(value));
  window.__CHA_NATIVE_RECEIVE__ = (batch) => {
    try {
      if (batch.connection_id !== connection) return;
      for (const reply of batch.messages) {
        const complete = pending.get(reply.id);
        if (!complete || reply.connection_id !== connection) continue;
        pending.delete(reply.id);
        complete(reply);
      }
    } finally {
      post({connection_id: batch.connection_id, delivery_id: batch.delivery_id});
    }
  };
  const rpc = (method, params = {}) => new Promise((resolve, reject) => {
    const id = next++;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(method + ' timed out'));
    }, 10000);
    pending.set(id, (reply) => {
      clearTimeout(timer);
      if (reply.ok) resolve(reply.result);
      else reject(new Error(method + ': ' + reply.error.code));
    });
    post({connection_id: connection, id, context_epoch: epoch, method, params});
  });
  const check = (value, message) => { if (!value) throw new Error(message); };
  try {
    await nativePolicyChecks();
    const boot = await rpc('app.bootstrap');
    epoch = boot.context_epoch;
    const originalVault = boot.bootstrap.vault_name;
    const created = await rpc('session.create', {forum_id: 'lobby', label: 'Parity'});
    const identity = {forum_id: 'lobby', session_id: created.id};
    await rpc('session.rename', {...identity, label: 'Renamed parity'});
    await rpc('session.open', identity);
    await rpc('session.close', identity);
    await rpc('session.open', identity);
    const persisted = await rpc('session.snapshot', identity);
    check(persisted.session_label === 'Renamed parity', 'renamed session did not persist');
    const other = await rpc('session.create', {forum_id: 'lobby', label: 'Navigation'});
    for (let i = 0; i < 12; ++i) {
      const selected = {...identity, session_id: i % 2 ? created.id : other.id};
      await rpc('session.open', selected);
      check((await rpc('session.snapshot', selected)).session_id === selected.session_id,
        'rapid selection restored the wrong conversation');
    }
    const character = await rpc('character.create', {display_name: 'Parity guide', description: 'Test'});
    const persona = await rpc('persona.create', {display_name: 'Parity narrator'});
    const forum = await rpc('forum.create', {display_name: 'Parity room', persona_id: persona.id});
    for (const [kind, id] of [['character', character.id], ['forum', forum.id]]) {
      const file = {[kind + '_id']: id, filename: 'notes.md'};
      await rpc(kind + '.file.create', {...file, content: 'Uploaded — 日本語'});
      check((await rpc(kind + '.file.get', file)).content === 'Uploaded — 日本語', 'file upload changed content');
      await rpc(kind + '.file.update', {...file, content: 'Edited content'});
      check((await rpc(kind + '.file.get', file)).content === 'Edited content', 'file edit did not persist');
      await rpc(kind + '.file.delete', file);
      let rejected = false;
      try { await rpc(kind + '.file.get', file); } catch { rejected = true; }
      check(rejected, 'deleted file remained readable');
    }
    await rpc('character.delete', {character_id: character.id});
    await rpc('forum.delete', {forum_id: forum.id});
    await rpc('persona.delete', {persona_id: persona.id});
    await rpc('session.delete', {...identity, session_id: other.id});
    const copy = await rpc('vault.create', {display_name: 'Parity copy', copy_from: originalVault, password: null});
    const toMerge = await rpc('persona.create', {display_name: 'Merged persona'});
    const switched = await rpc('vault.switch', {vault_name: copy.display_name, password: null});
    check(switched.context_epoch > epoch && switched.state === 'running', 'switch did not renew context');
    epoch = switched.context_epoch;
    const merged = await rpc('vault.merge', {source_vault: originalVault, password: null});
    check(merged.context_epoch > epoch && merged.state === 'running', 'merge did not renew context');
    epoch = merged.context_epoch;
    check((await rpc('persona.get', {persona_id: toMerge.id})).display_name === 'Merged persona',
      'vault merge lost the source configuration');
    check((await rpc('session.list', {forum_id: 'lobby'})).some((session) => session.label === 'Renamed parity'),
      'vault merge lost the destination conversation');
    epoch = (await rpc('vault.switch', {vault_name: originalVault, password: null})).context_epoch;
    await rpc('vault.delete', {vault_name: copy.display_name});
    await rpc('persona.delete', {persona_id: toMerge.id});
    await rpc('session.delete', identity);
    check(!(await rpc('session.list', {forum_id: 'lobby'})).some((session) => session.id === created.id),
      'deleted conversation remained in storage');
    return {ok: true, csp: 'inline scripts, event handlers, remote scripts and fetch blocked',
      sessions: 'rename/reopen/delete', navigation: '12 selections',
      files: 'character and forum create/edit/delete', vaults: 'copy/switch/merge/delete'};
  } catch (error) {
    return {ok: false, reason: String(error.message || error)};
  }
}
