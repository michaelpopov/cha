import { useEffect, useRef, useState } from 'react';

import {
  ChaProtocolError,
  type Bootstrap,
  type CreateSessionResult,
  type SessionListing,
  type SessionSnapshot,
} from '../api/client';
import { chaWebMessage, type ChaWebClient } from './client';
import {
  applyAcknowledgement,
  classifyReadFailure,
  classifyWriteFailure,
  commandControl,
  editDraft,
  newDraftKey,
  readRetryDelayMs,
  reconnectingNotice,
  sessionDraftKey,
  stopRequestedNotice,
  unknownSendNotice,
  type CommandInput,
  type DraftMap,
} from './outcome';
import {
  defaultForumId,
  parseChawebHash,
  sameChawebPlace,
  sessionHash,
  sessionUnavailable,
  visibleForums,
} from './route';

const pollDelayMs = 1_000;

type ConversationRef =
  | { kind: 'draft'; forumId: string }
  | { kind: 'session'; forumId: string; sessionId: string };

interface Status {
  awaitingKey: string | null;
  reconnecting: boolean;
  reconnectingKey: string | null;
  blockedKey: string | null;
  notices: Record<string, string>;
  pendingText: Record<string, string | undefined>;
  creating: Record<string, boolean | undefined>;
  inputs: Record<string, boolean | undefined>;
  stops: Record<string, boolean | undefined>;
  deletes: Record<string, boolean | undefined>;
  stopping: Record<string, true>;
  holds: Record<string, { inspected: boolean }>;
}

interface ReadJob {
  forumId: string;
  sessionId: string;
  gen: number;
  ack: number;
}

const emptyStatus: Status = {
  awaitingKey: null,
  reconnecting: false,
  reconnectingKey: null,
  blockedKey: null,
  notices: {},
  pendingText: {},
  creating: {},
  inputs: {},
  stops: {},
  deletes: {},
  stopping: {},
  holds: {},
};

function locationUrl(hash = ''): string {
  return `${window.location.pathname}${window.location.search}${hash}`;
}

function conversationKey(conversation: ConversationRef): string {
  return conversation.kind === 'draft'
    ? newDraftKey(conversation.forumId)
    : sessionDraftKey(conversation.forumId, conversation.sessionId);
}

function forumIsValid(bootstrap: Bootstrap | null, forumId: string): boolean {
  if (!bootstrap || !forumId) return false;
  return visibleForums(bootstrap).some((forum) => forum.id === forumId);
}

function pageVisible(): boolean {
  return document.visibilityState !== 'hidden';
}

function buildCommand(
  status: Status,
  conversation: ConversationRef | null,
  screen: 'list' | 'conversation',
  drafts: DraftMap,
  snapshot: SessionSnapshot | null,
  bootstrap: Bootstrap | null,
): CommandInput {
  const onConversation = screen === 'conversation' && conversation !== null;
  const kind = onConversation && conversation.kind === 'session' ? 'session' : 'draft';
  const key = conversation ? conversationKey(conversation) : '';
  const forumId = conversation?.forumId ?? '';
  const snapshotReady = kind === 'session'
    && conversation?.kind === 'session'
    && snapshot !== null
    && snapshot.forum.id === conversation.forumId
    && snapshot.session_id === conversation.sessionId;
  const hold = key ? status.holds[key] : undefined;
  return {
    kind,
    forumValid: forumIsValid(bootstrap, forumId),
    text: drafts[key]?.text ?? '',
    createPending: kind === 'draft' && Boolean(status.creating[forumId]),
    inputPending: Boolean(status.inputs[key]),
    stopPending: Boolean(status.stops[key]),
    stopping: Boolean(status.stopping[key]),
    generationActive: Boolean(snapshotReady && snapshot?.generation.active),
    snapshotReady,
    stateUnknown: kind === 'session' && (
      !snapshotReady || status.reconnecting || status.blockedKey === key
      || status.awaitingKey === key
      || status.pendingText[key] !== undefined
    ),
    sendBlocked: Boolean(hold),
  };
}

export function useChaweb(client: ChaWebClient) {
  const [bootstrap, setBootstrap] = useState<Bootstrap | null>(null);
  const [loading, setLoading] = useState(true);
  const [startupError, setStartupError] = useState<string | null>(null);
  const [startupCanRetry, setStartupCanRetry] = useState(false);
  const [retry, setRetry] = useState(0);
  const [routeReady, setRouteReady] = useState(false);
  const [screen, setScreen] = useState<'list' | 'conversation'>('list');
  const [forumId, setForumId] = useState('');
  const [conversation, setConversation] = useState<ConversationRef | null>(null);
  const [audioKey, setAudioKey] = useState('');
  const [drafts, setDrafts] = useState<DraftMap>({});
  const [sessions, setSessions] = useState<SessionListing[]>([]);
  const [listError, setListError] = useState<string | null>(null);
  const [listCanRetry, setListCanRetry] = useState(false);
  const [listKick, setListKick] = useState(0);
  const [snapshot, setSnapshot] = useState<SessionSnapshot | null>(null);
  const [status, setStatus] = useState<Status>(emptyStatus);

  const clientRef = useRef(client);
  const mounted = useRef(true);
  const bootstrapRef = useRef<Bootstrap | null>(null);
  const forumRef = useRef('');
  const screenRef = useRef<'list' | 'conversation'>('list');
  const conversationRef = useRef<ConversationRef | null>(null);
  const draftsRef = useRef<DraftMap>({});
  const snapshotRef = useRef<SessionSnapshot | null>(null);
  const statusRef = useRef<Status>(emptyStatus);
  const appliedHash = useRef<string | null>(null);
  const targetKey = useRef('');
  const readLoop = useRef({
    inflight: null as ReadJob | null,
    queued: false,
    ack: {} as Record<string, number>,
    gen: 0,
    failures: 0,
    timer: null as number | null,
    recovering: false,
    bootstrapFresh: false,
  });
  const listLoop = useRef({
    inflight: false,
    queued: null as string | null,
    timer: null as number | null,
    gen: 0,
  });

  clientRef.current = client;
  bootstrapRef.current = bootstrap;
  forumRef.current = forumId;
  screenRef.current = screen;
  conversationRef.current = conversation;
  draftsRef.current = drafts;
  snapshotRef.current = snapshot;
  statusRef.current = status;

  function patchStatus(recipe: (current: Status) => Status) {
    const next = recipe(statusRef.current);
    if (next === statusRef.current) return;
    statusRef.current = next;
    setStatus(next);
  }

  function updateDrafts(recipe: (current: DraftMap) => DraftMap) {
    const next = recipe(draftsRef.current);
    if (next === draftsRef.current) return;
    draftsRef.current = next;
    setDrafts(next);
  }

  function clearReadTimer() {
    const loop = readLoop.current;
    if (loop.timer === null) return;
    window.clearTimeout(loop.timer);
    loop.timer = null;
  }

  function retarget(key: string) {
    if (targetKey.current === key) return;
    targetKey.current = key;
    const loop = readLoop.current;
    loop.gen += 1;
    loop.failures = 0;
    loop.bootstrapFresh = false;
    loop.queued = false;
    clearReadTimer();
    patchStatus((current) => (
      current.reconnecting || current.reconnectingKey
        ? { ...current, reconnecting: false, reconnectingKey: null }
        : current
    ));
  }

  function selectedSession(): { forumId: string; sessionId: string } | null {
    if (screenRef.current !== 'conversation') return null;
    const current = conversationRef.current;
    if (!current || current.kind !== 'session') return null;
    return { forumId: current.forumId, sessionId: current.sessionId };
  }

  function isCurrent(job: ReadJob): boolean {
    if (!mounted.current) return false;
    if (job.gen !== readLoop.current.gen) return false;
    const selected = selectedSession();
    return selected !== null
      && selected.forumId === job.forumId
      && selected.sessionId === job.sessionId;
  }

  function requestRead() {
    if (!mounted.current || !selectedSession() || !pageVisible()) return;
    const key = currentDraftKey();
    if (key && statusRef.current.deletes[key]) return;
    const loop = readLoop.current;
    clearReadTimer();
    if (loop.recovering || loop.inflight) {
      loop.queued = true;
      return;
    }
    const selected = selectedSession();
    if (!selected || !pageVisible()) return;
    loop.queued = false;
    const job: ReadJob = {
      forumId: selected.forumId,
      sessionId: selected.sessionId,
      gen: loop.gen,
      ack: loop.ack[sessionDraftKey(selected.forumId, selected.sessionId)] ?? 0,
    };
    loop.inflight = job;
    void clientRef.current.getSession(job.forumId, job.sessionId).then(
      (loaded) => {
        if (loop.inflight === job) loop.inflight = null;
        succeedRead(job, loaded);
      },
      (error: unknown) => {
        if (loop.inflight === job) loop.inflight = null;
        failRead(job, error);
      },
    );
  }

  function resumeQueued() {
    if (!readLoop.current.queued) return;
    requestRead();
  }

  function scheduleAfter(loaded: SessionSnapshot) {
    clearReadTimer();
    if (readLoop.current.queued) {
      requestRead();
      return;
    }
    if (!pageVisible()) return;
    const selected = selectedSession();
    if (!selected || !loaded.generation.active) return;
    if (loaded.forum.id !== selected.forumId || loaded.session_id !== selected.sessionId) return;
    readLoop.current.timer = window.setTimeout(() => {
      readLoop.current.timer = null;
      requestRead();
    }, pollDelayMs);
  }

  function applySnapshot(loaded: SessionSnapshot, proves: boolean) {
    const previous = snapshotRef.current;
    snapshotRef.current = loaded;
    setSnapshot(loaded);
    const key = sessionDraftKey(loaded.forum.id, loaded.session_id);
    patchStatus((current) => {
      const holds = proves && current.holds[key] && !current.holds[key].inspected
        ? { ...current.holds, [key]: { inspected: true } }
        : current.holds;
      const pendingText = { ...current.pendingText };
      const stopping = { ...current.stopping };
      let notices = current.notices;
      if (proves) {
        delete pendingText[key];
        if (!loaded.generation.active) {
          delete stopping[key];
          if (!current.stops[key] && notices[key] === stopRequestedNotice) {
            notices = { ...notices };
            if (holds[key]) notices[key] = unknownSendNotice;
            else delete notices[key];
          }
        }
      }
      if (current.blockedKey === key) {
        notices = { ...current.notices };
        delete notices[key];
      }
      if (holds[key] && !notices[key]) notices = { ...notices, [key]: unknownSendNotice };
      return {
        ...current,
        awaitingKey: current.awaitingKey === key ? null : current.awaitingKey,
        holds,
        pendingText,
        stopping,
        notices,
        reconnecting: false,
        reconnectingKey: null,
        blockedKey: current.blockedKey === key ? null : current.blockedKey,
      };
    });
    const same = previous !== null
      && previous.forum.id === loaded.forum.id
      && previous.session_id === loaded.session_id;
    const titleChanged = same && previous.session_label !== loaded.session_label;
    const completed = same && previous.generation.active && !loaded.generation.active;
    if (titleChanged || completed) refreshList(loaded.forum.id);
  }

  function succeedRead(job: ReadJob, loaded: SessionSnapshot) {
    if (!isCurrent(job)) {
      resumeQueued();
      return;
    }
    if (loaded.forum.id !== job.forumId || loaded.session_id !== job.sessionId) {
      failRead(job, new ChaProtocolError());
      return;
    }
    const loop = readLoop.current;
    if (loop.failures > 0 && !loop.bootstrapFresh) {
      if (!pageVisible()) return;
      loop.recovering = true;
      const key = sessionDraftKey(job.forumId, job.sessionId);
      patchStatus((current) => ({
        ...current,
        reconnecting: true,
        reconnectingKey: key,
      }));
      void reloadBootstrap().then(
        () => {
          loop.recovering = false;
          if (!isCurrent(job)) {
            resumeQueued();
            return;
          }
          loop.bootstrapFresh = true;
          requestRead();
        },
        (error: unknown) => {
          loop.recovering = false;
          failRead(job, error);
        },
      );
      return;
    }
    loop.failures = 0;
    loop.bootstrapFresh = false;
    applySnapshot(loaded, job.ack === (loop.ack[sessionDraftKey(job.forumId, job.sessionId)] ?? 0));
    scheduleAfter(loaded);
  }

  function failRead(job: ReadJob, error: unknown) {
    if (!isCurrent(job)) {
      resumeQueued();
      return;
    }
    const key = sessionDraftKey(job.forumId, job.sessionId);
    clearReadTimer();
    readLoop.current.bootstrapFresh = false;
    const kind = classifyReadFailure(error);
    if (kind === 'missing') {
      missingSession(chaWebMessage(error, 'The conversation could not be loaded.'));
      return;
    }
    const delay = kind === 'retry' ? readRetryDelayMs(readLoop.current.failures + 1) : null;
    if (delay !== null) {
      readLoop.current.failures += 1;
      patchStatus((current) => ({
        ...current,
        reconnecting: true,
        reconnectingKey: key,
        blockedKey: current.blockedKey === key ? null : current.blockedKey,
      }));
      if (pageVisible()) readLoop.current.timer = window.setTimeout(() => {
        readLoop.current.timer = null;
        if (!isCurrent(job)) return;
        requestRead();
      }, delay);
      return;
    }
    readLoop.current.queued = false;
    patchStatus((current) => ({
      ...current,
      reconnecting: false,
      reconnectingKey: null,
      blockedKey: key,
      notices: {
        ...current.notices,
        [key]: chaWebMessage(error, 'The conversation could not be loaded.'),
      },
    }));
  }

  function pumpList() {
    if (!mounted.current) return;
    const loop = listLoop.current;
    const next = loop.queued;
    if (!next || loop.inflight) return;
    loop.queued = null;
    refreshList(next);
  }

  function invalidateList() {
    const loop = listLoop.current;
    loop.gen += 1;
    loop.queued = null;
    if (loop.timer !== null) window.clearTimeout(loop.timer);
    loop.timer = null;
    setListCanRetry(false);
  }

  function runList(forum: string, gen: number, attempt: number) {
    const loop = listLoop.current;
    if (!mounted.current || gen !== loop.gen) return;
    loop.inflight = true;
    const request = attempt > 0
      ? reloadBootstrap().then(() => (
        mounted.current && gen === loop.gen && pageVisible()
          ? clientRef.current.listSessions(forum)
          : null
      ))
      : clientRef.current.listSessions(forum);
    void request.then(
      (rows) => {
        loop.inflight = false;
        if (!mounted.current) return;
        if (gen !== loop.gen || rows === null) {
          pumpList();
          return;
        }
        if (forumRef.current === forum) {
          setSessions(rows);
          setListError((current) => current === reconnectingNotice ? null : current);
          setListCanRetry(false);
          inspectNewDraft(forum);
        }
        pumpList();
      },
      (error: unknown) => {
        loop.inflight = false;
        if (!mounted.current) return;
        if (gen !== loop.gen || forumRef.current !== forum) {
          pumpList();
          return;
        }
        const failure = classifyReadFailure(error);
        const delay = failure === 'retry' ? readRetryDelayMs(attempt + 1) : null;
        if (delay !== null && screenRef.current === 'list' && pageVisible()) {
          setListError(reconnectingNotice);
          loop.timer = window.setTimeout(() => {
            loop.timer = null;
            runList(forum, gen, attempt + 1);
          }, delay);
          return;
        }
        setListError(chaWebMessage(error, 'Sessions could not be loaded.'));
        setListCanRetry(failure !== 'missing');
        if (failure === 'missing') void reloadBootstrap().catch(() => undefined);
        pumpList();
      },
    );
  }

  function inspectNewDraft(forum: string) {
    if (screenRef.current !== 'list' || forumRef.current !== forum) return;
    const key = newDraftKey(forum);
    patchStatus((current) => {
      const hold = current.holds[key];
      if (!hold || hold.inspected) return current;
      return { ...current, holds: { ...current.holds, [key]: { inspected: true } } };
    });
  }

  function refreshList(forum: string) {
    if (!mounted.current || !forum) return;
    const loop = listLoop.current;
    if (loop.timer !== null) {
      window.clearTimeout(loop.timer);
      loop.timer = null;
    }
    if (loop.inflight) {
      loop.queued = screenRef.current === 'list' ? forumRef.current : forum;
      loop.gen += 1;
      return;
    }
    const gen = ++loop.gen;
    runList(forum, gen, 0);
  }

  async function reloadBootstrap() {
    const loaded = await clientRef.current.getBootstrap();
    if (!mounted.current) return;
    bootstrapRef.current = loaded;
    setBootstrap(loaded);
    setForumId((current) => {
      const next = forumIsValid(loaded, current) ? current : defaultForumId(loaded);
      forumRef.current = next;
      return next;
    });
  }

  function noteAck(key: string) {
    const loop = readLoop.current;
    loop.ack[key] = (loop.ack[key] ?? 0) + 1;
    if (currentDraftKey() === key) requestRead();
  }

  function viewingDraft(forum: string): boolean {
    return screenRef.current === 'conversation'
      && conversationRef.current?.kind === 'draft'
      && conversationRef.current.forumId === forum;
  }

  function missingSession(message: string) {
    clearReadTimer();
    const loop = readLoop.current;
    loop.queued = false;
    loop.failures = 0;
    loop.recovering = false;
    setListError(message);
    screenRef.current = 'list';
    setScreen('list');
    retarget('');
    void reloadBootstrap().catch(() => undefined);
  }

  function writeMissing(key: string, forum: string, message: string) {
    patchStatus((current) => ({
      ...current,
      notices: { ...current.notices, [key]: message },
    }));
    refreshList(forum);
    void reloadBootstrap().catch(() => undefined);
    if (currentDraftKey() === key) missingSession(message);
  }

  function currentDraftKey(): string | null {
    if (screenRef.current !== 'conversation' || !conversationRef.current) return null;
    return conversationKey(conversationRef.current);
  }

  function assignSession(forum: string, session: string) {
    invalidateList();
    if (forumRef.current !== forum) setSessions([]);
    const next: ConversationRef = { kind: 'session', forumId: forum, sessionId: session };
    conversationRef.current = next;
    screenRef.current = 'conversation';
    forumRef.current = forum;
    setConversation(next);
    setAudioKey(sessionDraftKey(forum, session));
    setScreen('conversation');
    setForumId(forum);
    patchStatus((state) => ({ ...state, awaitingKey: sessionDraftKey(forum, session) }));
    const current = snapshotRef.current;
    if (!current || current.forum.id !== forum || current.session_id !== session) {
      snapshotRef.current = null;
      setSnapshot(null);
    }
    retarget(`${forum}/${session}`);
    requestRead();
  }

  function showList() {
    invalidateList();
    screenRef.current = 'list';
    setScreen('list');
    retarget('');
  }

  function applyHistory(hash: string) {
    appliedHash.current = hash;
    const loaded = bootstrapRef.current;
    if (!loaded) return;
    const route = parseChawebHash(hash);
    if (route.kind === 'list') {
      showList();
      return;
    }
    if (route.kind === 'unknown' || sessionUnavailable(loaded, route.forumId, route.sessionId)) {
      setListError('That conversation is not available.');
      showList();
      return;
    }
    assignSession(route.forumId, route.sessionId);
  }

  function chooseForum(next: string) {
    invalidateList();
    setSessions([]);
    forumRef.current = next;
    setForumId(next);
    setListError(null);
  }

  function openSession(sessionId: string) {
    const forum = forumRef.current;
    const hash = sessionHash(forum, sessionId);
    appliedHash.current = hash;
    if (window.location.hash !== hash) {
      window.history.pushState(null, '', locationUrl(hash));
    }
    setListError(null);
    assignSession(forum, sessionId);
  }

  function newSession() {
    const forum = forumRef.current;
    if (!forum) return;
    invalidateList();
    appliedHash.current = '';
    if (window.location.hash) window.history.pushState(null, '', locationUrl());
    const next: ConversationRef = { kind: 'draft', forumId: forum };
    conversationRef.current = next;
    screenRef.current = 'conversation';
    setConversation(next);
    setAudioKey(newDraftKey(forum));
    setScreen('conversation');
    setListError(null);
    retarget('');
  }

  function showSessions() {
    const current = conversationRef.current;
    if (current) {
      forumRef.current = current.forumId;
      setForumId(current.forumId);
    }
    showList();
  }

  function onDraft(text: string) {
    const key = currentDraftKey();
    if (!key) return;
    updateDrafts((current) => editDraft(current, key, text));
  }

  function send(submittedText?: string) {
    if (submittedText !== undefined) onDraft(submittedText);
    if (statusRef.current.deletes[currentDraftKey() ?? '']) return;
    const control = commandControl(buildCommand(
      statusRef.current,
      conversationRef.current,
      screenRef.current,
      draftsRef.current,
      snapshotRef.current,
      bootstrapRef.current,
    ));
    if (control.mode !== 'send' || control.disabled) return;
    const current = conversationRef.current;
    if (!current || screenRef.current !== 'conversation') return;
    if (current.kind === 'draft') {
      const key = newDraftKey(current.forumId);
      const draft = draftsRef.current[key] ?? { text: '', revision: 0 };
      const forum = current.forumId;
      const { text, revision } = draft;
      patchStatus((state) => {
        const notices = { ...state.notices };
        delete notices[key];
        return { ...state, creating: { ...state.creating, [forum]: true }, notices };
      });
      void clientRef.current.createSession(forum, text).then(
        (created) => createdOk(forum, revision, text, created),
        (error: unknown) => createdFail(forum, error),
      );
      return;
    }
    const key = sessionDraftKey(current.forumId, current.sessionId);
    const draft = draftsRef.current[key] ?? { text: '', revision: 0 };
    const forum = current.forumId;
    const session = current.sessionId;
    const { text, revision } = draft;
    patchStatus((state) => {
      const notices = { ...state.notices };
      delete notices[key];
      return { ...state, inputs: { ...state.inputs, [key]: true }, notices };
    });
    void clientRef.current.submitInput(forum, session, text).then(
      () => inputOk(forum, key, revision, text),
      (error: unknown) => inputFail(forum, key, error),
    );
  }

  function stop() {
    if (statusRef.current.deletes[currentDraftKey() ?? '']) return;
    const control = commandControl(buildCommand(
      statusRef.current,
      conversationRef.current,
      screenRef.current,
      draftsRef.current,
      snapshotRef.current,
      bootstrapRef.current,
    ));
    if (control.mode !== 'stop' || control.disabled) return;
    const current = conversationRef.current;
    if (!current || current.kind !== 'session') return;
    const forum = current.forumId;
    const session = current.sessionId;
    const key = sessionDraftKey(forum, session);
    patchStatus((state) => ({
      ...state,
      stops: { ...state.stops, [key]: true },
      notices: { ...state.notices, [key]: stopRequestedNotice },
    }));
    void clientRef.current.stopSession(forum, session).then(
      () => stopOk(key),
      (error: unknown) => stopFail(forum, key, error),
    );
  }

  function canDelete() {
    const command = buildCommand(
      statusRef.current, conversationRef.current, screenRef.current,
      draftsRef.current, snapshotRef.current, bootstrapRef.current,
    );
    return screenRef.current === 'conversation'
      && command.kind === 'session' && command.snapshotReady
      && !command.inputPending && !command.stopPending && !command.stopping
      && !command.stateUnknown && !command.sendBlocked
      && !statusRef.current.deletes[currentDraftKey() ?? ''];
  }

  function deleteSession(confirmedKey: string) {
    const current = conversationRef.current;
    if (!current || current.kind !== 'session'
        || conversationKey(current) !== confirmedKey || !canDelete()) return;
    const { forumId: forum, sessionId: session } = current;
    const key = confirmedKey;
    clearReadTimer();
    // Ignore any snapshot that was requested before deletion began.
    readLoop.current.gen += 1;
    readLoop.current.queued = false;
    patchStatus((state) => ({
      ...state,
      deletes: { ...state.deletes, [key]: true },
      notices: { ...state.notices, [key]: 'Deleting' },
    }));
    void clientRef.current.deleteSession(forum, session).then(
      () => deletedOk(forum, session, key),
      (error: unknown) => {
        if (!mounted.current) return;
        if (classifyWriteFailure(error) === 'missing') {
          deletedOk(forum, session, key);
          return;
        }
        patchStatus((state) => ({
          ...state,
          deletes: { ...state.deletes, [key]: false },
          notices: { ...state.notices, [key]: chaWebMessage(error, 'The session could not be deleted.') },
        }));
        if (currentDraftKey() === key) requestRead();
      },
    );
  }

  function deletedOk(forum: string, session: string, key: string) {
    if (!mounted.current) return;
    updateDrafts((state) => {
      const next = { ...state };
      delete next[key];
      return next;
    });
    patchStatus((state) => {
      const deletes = { ...state.deletes };
      const notices = { ...state.notices };
      delete deletes[key];
      delete notices[key];
      return { ...state, deletes, notices };
    });
    setSessions((rows) => forumRef.current === forum
      ? rows.filter((row) => row.id !== session) : rows);
    if (sameChawebPlace(window.location.hash, sessionHash(forum, session))) {
      appliedHash.current = '';
      window.history.replaceState(null, '', locationUrl());
    }
    const selected = conversationRef.current;
    if (selected && conversationKey(selected) === key) {
      conversationRef.current = null;
      snapshotRef.current = null;
      forumRef.current = forum;
      setConversation(null);
      setSnapshot(null);
      setForumId(forum);
      setListError(null);
      showList();
      setListKick((count) => count + 1);
    } else {
      refreshList(forum);
    }
  }

  function createdOk(forum: string, revision: number, text: string, created: CreateSessionResult) {
    if (!mounted.current) return;
    const from = newDraftKey(forum);
    const key = sessionDraftKey(forum, created.id);
    updateDrafts((current) => applyAcknowledgement(current, from, revision, key));
    patchStatus((state) => ({
      ...state,
      creating: { ...state.creating, [forum]: false },
      pendingText: { ...state.pendingText, [key]: text },
    }));
    refreshList(forum);
    if (!viewingDraft(forum)) {
      noteAck(key);
      return;
    }
    const hash = sessionHash(forum, created.id);
    appliedHash.current = hash;
    if (window.location.hash !== hash) {
      window.history.pushState(null, '', locationUrl(hash));
    }
    const next: ConversationRef = { kind: 'session', forumId: forum, sessionId: created.id };
    conversationRef.current = next;
    screenRef.current = 'conversation';
    forumRef.current = forum;
    snapshotRef.current = null;
    setConversation(next);
    setForumId(forum);
    setSnapshot(null);
    retarget(`${forum}/${created.id}`);
    noteAck(key);
  }

  function createdFail(forum: string, error: unknown) {
    if (!mounted.current) return;
    const key = newDraftKey(forum);
    patchStatus((state) => ({
      ...state,
      creating: { ...state.creating, [forum]: false },
    }));
    const failure = classifyWriteFailure(error);
    if (failure === 'rejected') {
      patchStatus((state) => ({
        ...state,
        notices: { ...state.notices, [key]: chaWebMessage(error, 'The request failed.') },
      }));
      return;
    }
    if (failure === 'missing') {
      writeMissing(key, forum, chaWebMessage(error, 'The request failed.'));
      return;
    }
    patchStatus((state) => ({
      ...state,
      holds: { ...state.holds, [key]: { inspected: false } },
      notices: { ...state.notices, [key]: unknownSendNotice },
    }));
    refreshList(forum);
  }

  function inputOk(forum: string, key: string, revision: number, text: string) {
    if (!mounted.current) return;
    updateDrafts((current) => applyAcknowledgement(current, key, revision));
    patchStatus((state) => ({
      ...state,
      inputs: { ...state.inputs, [key]: false },
      pendingText: { ...state.pendingText, [key]: text },
    }));
    refreshList(forum);
    noteAck(key);
  }

  function inputFail(forum: string, key: string, error: unknown) {
    if (!mounted.current) return;
    patchStatus((state) => ({
      ...state,
      inputs: { ...state.inputs, [key]: false },
    }));
    const failure = classifyWriteFailure(error);
    if (failure === 'rejected') {
      patchStatus((state) => ({
        ...state,
        notices: { ...state.notices, [key]: chaWebMessage(error, 'The request failed.') },
      }));
      return;
    }
    if (failure === 'missing') {
      writeMissing(key, forum, chaWebMessage(error, 'The request failed.'));
      return;
    }
    patchStatus((state) => ({
      ...state,
      holds: { ...state.holds, [key]: { inspected: false } },
      notices: { ...state.notices, [key]: unknownSendNotice },
    }));
    noteAck(key);
  }

  function stopOk(key: string) {
    if (!mounted.current) return;
    patchStatus((state) => {
      return {
        ...state,
        stops: { ...state.stops, [key]: false },
        stopping: { ...state.stopping, [key]: true },
      };
    });
    noteAck(key);
  }

  function stopFail(forum: string, key: string, error: unknown) {
    if (!mounted.current) return;
    patchStatus((state) => ({
      ...state,
      stops: { ...state.stops, [key]: false },
    }));
    const failure = classifyWriteFailure(error);
    if (failure === 'missing') {
      writeMissing(key, forum, chaWebMessage(error, 'The request failed.'));
      return;
    }
    patchStatus((state) => ({
      ...state,
      stopping: failure === 'unknown' ? { ...state.stopping, [key]: true } : state.stopping,
      notices: {
        ...state.notices,
        [key]: chaWebMessage(error, 'The request failed.'),
      },
    }));
    noteAck(key);
  }

  function allowSend() {
    const key = currentDraftKey();
    if (!key || !statusRef.current.holds[key]?.inspected) return;
    patchStatus((state) => {
      const holds = { ...state.holds };
      const notices = { ...state.notices };
      delete holds[key];
      if (notices[key] === unknownSendNotice) delete notices[key];
      return { ...state, holds, notices };
    });
  }

  function retryRead() {
    const selected = selectedSession();
    if (!selected) return;
    const key = sessionDraftKey(selected.forumId, selected.sessionId);
    readLoop.current.failures = 0;
    patchStatus((state) => {
      const notices = { ...state.notices };
      delete notices[key];
      return {
        ...state,
        notices,
        blockedKey: null,
        reconnecting: true,
        reconnectingKey: key,
      };
    });
    requestRead();
  }

  function retryList() {
    setListCanRetry(false);
    setListError(reconnectingNotice);
    setListKick((count) => count + 1);
  }

  function retryStartup() {
    setLoading(true);
    setStartupError(null);
    setStartupCanRetry(false);
    setRetry((count) => count + 1);
  }

  const bootstrapReady = bootstrap !== null;

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      clearReadTimer();
      readLoop.current.gen += 1;
      const loop = listLoop.current;
      loop.gen += 1;
      if (loop.timer !== null) window.clearTimeout(loop.timer);
    };
  }, []);

  useEffect(() => {
    let active = true;
    let timer: number | null = null;
    let nextAttempt: number | null = null;
    setLoading(true);
    setStartupError(null);
    setStartupCanRetry(false);

    const run = (failures: number) => {
      timer = null;
      nextAttempt = null;
      void client.getBootstrap().then(
        (loaded) => {
          if (!active) return;
          bootstrapRef.current = loaded;
          setBootstrap(loaded);
          setForumId((current) => {
            const next = current || defaultForumId(loaded);
            forumRef.current = next;
            return next;
          });
          setStartupError(null);
          setStartupCanRetry(false);
          setLoading(false);
        },
        (error: unknown) => {
          if (!active) return;
          const delay = classifyReadFailure(error) === 'retry' ? readRetryDelayMs(failures) : null;
          if (delay !== null) {
            setLoading(false);
            setStartupError(reconnectingNotice);
            setStartupCanRetry(false);
            nextAttempt = failures + 1;
            if (pageVisible()) timer = window.setTimeout(() => run(failures + 1), delay);
            return;
          }
          setLoading(false);
          setStartupError(chaWebMessage(error, 'ChaWeb could not load.'));
          setStartupCanRetry(true);
        },
      );
    };

    function onVisibility() {
      if (!pageVisible()) {
        if (timer !== null) window.clearTimeout(timer);
        timer = null;
      } else if (nextAttempt !== null) {
        if (timer !== null) window.clearTimeout(timer);
        run(nextAttempt);
      }
    }
    document.addEventListener('visibilitychange', onVisibility);
    run(1);
    return () => {
      active = false;
      if (timer !== null) window.clearTimeout(timer);
      document.removeEventListener('visibilitychange', onVisibility);
    };
  }, [client, retry]);

  useEffect(() => {
    if (!bootstrapReady) return;
    let active = true;
    applyHistory(window.location.hash);
    setRouteReady(true);
    function onHistory() {
      if (!active) return;
      if (appliedHash.current !== null
          && sameChawebPlace(window.location.hash, appliedHash.current)) {
        return;
      }
      applyHistory(window.location.hash);
    }
    window.addEventListener('hashchange', onHistory);
    window.addEventListener('popstate', onHistory);
    return () => {
      active = false;
      window.removeEventListener('hashchange', onHistory);
      window.removeEventListener('popstate', onHistory);
    };
    // Route handling stays on the first validated bootstrap. Later refreshes
    // must not repeat the startup navigation.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [bootstrapReady]);

  useEffect(() => {
    if (!routeReady || screen !== 'list' || !forumId) return;
    refreshList(forumId);
    // List loads follow the visible forum. refreshList reads the latest client.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [routeReady, screen, forumId, listKick]);

  useEffect(() => {
    function onVisibility() {
      if (document.visibilityState === 'hidden') {
        clearReadTimer();
        invalidateList();
        return;
      }
      const forum = forumRef.current;
      if (forum) refreshList(forum);
      const selected = selectedSession();
      if (selected) patchStatus((state) => ({
        ...state,
        awaitingKey: sessionDraftKey(selected.forumId, selected.sessionId),
      }));
      requestRead();
    }
    document.addEventListener('visibilitychange', onVisibility);
    return () => document.removeEventListener('visibilitychange', onVisibility);
    // The listener calls the latest forum and read loop through refs.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const active = conversation && screen === 'conversation' ? conversation : null;
  const activeKey = active ? conversationKey(active) : '';
  const command = buildCommand(
    status,
    conversation,
    screen,
    drafts,
    snapshot,
    bootstrap,
  );
  const control = commandControl(command);
  const snapshotReady = active?.kind === 'session'
    && snapshot !== null
    && snapshot.forum.id === active.forumId
    && snapshot.session_id === active.sessionId;
  const notice = activeKey
    ? (status.reconnecting && status.reconnectingKey === activeKey
      ? reconnectingNotice
      : status.notices[activeKey]
        ?? (snapshotReady ? snapshot?.notice ?? null : null))
    : null;
  const showSending = Boolean(
    (active?.kind === 'draft' && status.creating[active.forumId])
    || (activeKey && status.inputs[activeKey]),
  );
  const pendingText = status.pendingText[activeKey] ?? null;
  const deleting = Boolean(status.deletes[activeKey]);

  return {
    bootstrap,
    loading,
    startupError,
    startupCanRetry,
    retryStartup,
    forumId,
    sessions,
    listError,
    listCanRetry,
    retryList,
    screen,
    snapshot: snapshotReady ? snapshot : null,
    draft: activeKey ? drafts[activeKey]?.text ?? '' : '',
    notice,
    showSending,
    pendingText,
    voiceBlocked: !active || deleting || command.generationActive
      || command.createPending || command.inputPending || command.stopPending
      || command.stopping || command.stateUnknown || command.sendBlocked,
    mode: control.mode,
    commandDisabled: control.disabled || deleting,
    deleting,
    deleteDisabled: !canDelete(),
    deleteSession,
    allowSend: status.holds[activeKey]?.inspected ? allowSend : null,
    retryConversation: status.blockedKey === activeKey && !status.reconnecting ? retryRead : null,
    sessionKey: activeKey || 'none',
    // A new draft keeps its audio selection when its first Send creates the session.
    audioKey: active ? audioKey : '',
    currentSessionId: conversation?.kind === 'session' && conversation.forumId === forumId
      ? conversation.sessionId
      : null,
    chooseForum,
    openSession,
    newSession,
    showSessions,
    onDraft,
    send,
    stop,
  };
}
