import { useEffect, useRef, useState } from 'react';

import {
  ChaProtocolError,
  type Bootstrap,
  type CreateSessionResult,
  type SessionListing,
  type SessionSnapshot,
} from '../api/client';
import { welcomeSessionId } from '../state/route';
import { welcomeSnapshot, welcomeSubmission, type WelcomeTurn } from '../welcomeTurn';
import { chaWebMessage, type ChaWebBootstrap, type ChaWebClient } from './client';
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
  isWelcomeSession,
  parseChawebHash,
  sameChawebPlace,
  sessionHash,
  sessionUnavailable,
} from './route';

const pollDelayMs = 1_000;

type ConversationRef =
  | { kind: 'draft'; forumId: string }
  | { kind: 'session'; forumId: string; sessionId: string };

// Request state of one new draft or stored session, keyed by its draft key.
interface ConversationStatus {
  notice?: string;
  // Accepted input that no snapshot has shown yet.
  pendingText?: string;
  // Creation for a draft, or input for a stored session.
  sending?: boolean;
  stopPending?: boolean;
  stopping?: boolean;
  deleting?: boolean;
  deleteError?: string;
  // A write had an unknown result. Send waits until the user sees fresh state.
  unverified?: boolean;
}

interface Status {
  awaitingKey: string | null;
  reconnectingKey: string | null;
  blockedKey: string | null;
  conversations: Record<string, ConversationStatus>;
}

interface ReadJob {
  forumId: string;
  sessionId: string;
  gen: number;
  ack: number;
}

const emptyStatus: Status = {
  awaitingKey: null,
  reconnectingKey: null,
  blockedKey: null,
  conversations: {},
};

// An undefined value in `change` clears that field.
function withConversation(status: Status, key: string, change: ConversationStatus): Status {
  return {
    ...status,
    conversations: {
      ...status.conversations,
      [key]: { ...status.conversations[key], ...change },
    },
  };
}

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
  return bootstrap.forums.some((forum) => forum.id === forumId);
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
  const current = key ? status.conversations[key] : undefined;
  return {
    kind,
    forumValid: forumIsValid(bootstrap, forumId),
    text: drafts[key]?.text ?? '',
    sending: Boolean(current?.sending),
    stopPending: Boolean(current?.stopPending),
    stopping: Boolean(current?.stopping),
    generationActive: Boolean(snapshotReady && snapshot?.generation.active),
    snapshotReady,
    stateUnknown: kind === 'session' && (
      !snapshotReady || status.reconnectingKey !== null || status.blockedKey === key
      || status.awaitingKey === key
      || current?.pendingText !== undefined
    ),
    sendBlocked: Boolean(current?.unverified),
  };
}

export function useChaweb(client: ChaWebClient) {
  const [bootstrap, setBootstrap] = useState<ChaWebBootstrap | null>(null);
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
  const welcomeTurn = useRef<WelcomeTurn | null>(null);
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
      current.reconnectingKey ? { ...current, reconnectingKey: null } : current
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
    if (key && statusRef.current.conversations[key]?.deleting) return;
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

  function listForumId(): string {
    const loaded = bootstrapRef.current;
    const current = forumRef.current;
    if (loaded && current && current !== loaded.entrance_forum_id) return current;
    return loaded ? defaultForumId(loaded) : '';
  }

  function refreshWelcome() {
    void reloadBootstrap().catch(() => undefined);
    const forum = listForumId();
    if (forum) refreshList(forum);
  }

  function welcomeIdentity(forum: string, session: string): boolean {
    const loaded = bootstrapRef.current;
    return loaded !== null && isWelcomeSession(loaded, forum, session);
  }

  function markWelcomePending(forum: string, session: string) {
    if (!welcomeIdentity(forum, session)) return;
    welcomeTurn.current = welcomeSubmission(welcomeTurn.current, `${forum}/${session}`);
  }

  function noteWelcomeSnapshot(loaded: SessionSnapshot) {
    if (!welcomeIdentity(loaded.forum.id, loaded.session_id)) return;
    const next = welcomeSnapshot(
      welcomeTurn.current, `${loaded.forum.id}/${loaded.session_id}`, loaded.generation.active);
    welcomeTurn.current = next.turn;
    if (next.ended) refreshWelcome();
  }

  function applySnapshot(loaded: SessionSnapshot, proves: boolean) {
    const previous = snapshotRef.current;
    snapshotRef.current = loaded;
    setSnapshot(loaded);
    const key = sessionDraftKey(loaded.forum.id, loaded.session_id);
    patchStatus((current) => {
      const next = { ...current.conversations[key] };
      if (current.blockedKey === key) delete next.notice;
      if (proves) {
        delete next.pendingText;
        // Fresh state is visible. Send is allowed again; the warning stays.
        if (next.unverified) {
          delete next.unverified;
          next.notice = unknownSendNotice;
        }
        // An idle snapshot settles earlier local notices, such as Stop
        // requested or a Stop failure. Its own notice shows instead.
        if (!loaded.generation.active) {
          delete next.stopping;
          if (!next.sending && !next.stopPending && next.notice !== unknownSendNotice) {
            delete next.notice;
          }
        }
      }
      return {
        awaitingKey: current.awaitingKey === key ? null : current.awaitingKey,
        reconnectingKey: null,
        blockedKey: current.blockedKey === key ? null : current.blockedKey,
        conversations: { ...current.conversations, [key]: next },
      };
    });
    const same = previous !== null
      && previous.forum.id === loaded.forum.id
      && previous.session_id === loaded.session_id;
    const titleChanged = same && previous.session_label !== loaded.session_label;
    const completed = same && previous.generation.active && !loaded.generation.active;
    noteWelcomeSnapshot(loaded);
    if (welcomeIdentity(loaded.forum.id, loaded.session_id)) {
      if (titleChanged) refreshWelcome();
    } else if (titleChanged || completed) {
      refreshList(loaded.forum.id);
    }
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
      patchStatus((current) => ({ ...current, reconnectingKey: key }));
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
    const reconnecting = statusRef.current.reconnectingKey
      === sessionDraftKey(job.forumId, job.sessionId);
    applySnapshot(loaded, job.ack === (loop.ack[sessionDraftKey(job.forumId, job.sessionId)] ?? 0));
    if (reconnecting && welcomeIdentity(job.forumId, job.sessionId)) refreshWelcome();
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
    patchStatus((current) => withConversation(
      { ...current, reconnectingKey: null, blockedKey: key },
      key,
      { notice: chaWebMessage(error, 'The conversation could not be loaded.') },
    ));
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
    patchStatus((current) => (
      current.conversations[key]?.unverified
        ? withConversation(current, key, { unverified: undefined })
        : current
    ));
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

  async function refreshVault() {
    invalidateList();
    retarget('');
    screenRef.current = 'list';
    setScreen('list');
    conversationRef.current = null;
    snapshotRef.current = null;
    setConversation(null);
    setSnapshot(null);
    setSessions([]);
    setListError(null);
    patchStatus(() => emptyStatus);
    appliedHash.current = '';
    window.history.replaceState(null, '', locationUrl());
    await reloadBootstrap();
    refreshList(forumRef.current);
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
    patchStatus((current) => withConversation(current, key, { notice: message }));
    refreshList(forum);
    void reloadBootstrap().catch(() => undefined);
    if (currentDraftKey() === key) missingSession(message);
  }

  function currentDraftKey(): string | null {
    if (screenRef.current !== 'conversation' || !conversationRef.current) return null;
    return conversationKey(conversationRef.current);
  }

  function assignSession(forum: string, session: string) {
    const welcome = welcomeIdentity(forum, session);
    invalidateList();
    if (!welcome && forumRef.current !== forum) setSessions([]);
    const next: ConversationRef = { kind: 'session', forumId: forum, sessionId: session };
    conversationRef.current = next;
    screenRef.current = 'conversation';
    if (!welcome) {
      forumRef.current = forum;
      setForumId(forum);
    }
    setConversation(next);
    setAudioKey(sessionDraftKey(forum, session));
    setScreen('conversation');
    patchStatus((state) => ({ ...state, awaitingKey: sessionDraftKey(forum, session) }));
    const current = snapshotRef.current;
    if (!current || current.forum.id !== forum || current.session_id !== session) {
      snapshotRef.current = null;
      setSnapshot(null);
    }
    retarget(`${forum}/${session}`);
    requestRead();
    if (welcome) {
      welcomeTurn.current = { key: `${forum}/${session}`, active: false, pending: false };
      refreshWelcome();
    }
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
    const loaded = bootstrapRef.current;
    if (current && loaded) {
      const forum = current.forumId === loaded.entrance_forum_id
        ? defaultForumId(loaded) : current.forumId;
      if (forum) {
        forumRef.current = forum;
        setForumId(forum);
      }
    }
    showList();
  }

  function openWelcome() {
    const loaded = bootstrapRef.current;
    if (!loaded) return;
    const forum = loaded.entrance_forum_id;
    const session = welcomeSessionId;
    if (!isWelcomeSession(loaded, forum, session)) return;
    const hash = sessionHash(forum, session);
    appliedHash.current = hash;
    if (window.location.hash !== hash) {
      window.history.pushState(null, '', locationUrl(hash));
    }
    setListError(null);
    assignSession(forum, session);
  }

  function onDraft(text: string) {
    const key = currentDraftKey();
    if (!key) return;
    updateDrafts((current) => editDraft(current, key, text));
  }

  function send(submittedText?: string) {
    if (submittedText !== undefined) onDraft(submittedText);
    if (statusRef.current.conversations[currentDraftKey() ?? '']?.deleting) return;
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
    if (current.kind === 'session') markWelcomePending(current.forumId, current.sessionId);
    if (current.kind === 'draft') {
      const key = newDraftKey(current.forumId);
      const draft = draftsRef.current[key] ?? { text: '', revision: 0 };
      const forum = current.forumId;
      const { text, revision } = draft;
      patchStatus((state) => withConversation(state, key, { sending: true, notice: undefined, deleteError: undefined }));
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
    patchStatus((state) => withConversation(state, key, { sending: true, notice: undefined, deleteError: undefined }));
    void clientRef.current.submitInput(forum, session, text).then(
      () => inputOk(forum, key, revision, text),
      (error: unknown) => inputFail(forum, key, error),
    );
  }

  function stop() {
    if (statusRef.current.conversations[currentDraftKey() ?? '']?.deleting) return;
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
    markWelcomePending(forum, session);
    patchStatus((state) => withConversation(
      state, key, { stopPending: true, notice: stopRequestedNotice, deleteError: undefined },
    ));
    void clientRef.current.stopSession(forum, session).then(
      () => stopOk(key),
      (error: unknown) => stopFail(forum, key, error),
    );
  }

  function canDelete() {
    const current = conversationRef.current;
    const loaded = bootstrapRef.current;
    if (current?.kind === 'session' && loaded
        && isWelcomeSession(loaded, current.forumId, current.sessionId)) {
      return false;
    }
    const command = buildCommand(
      statusRef.current, conversationRef.current, screenRef.current,
      draftsRef.current, snapshotRef.current, bootstrapRef.current,
    );
    return screenRef.current === 'conversation'
      && command.kind === 'session' && command.snapshotReady
      && !command.sending && !command.stopPending && !command.stopping
      && !command.stateUnknown && !command.sendBlocked
      && !statusRef.current.conversations[currentDraftKey() ?? '']?.deleting;
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
    patchStatus((state) => withConversation(state, key, { deleting: true, deleteError: undefined, notice: 'Deleting' }));
    void clientRef.current.deleteSession(forum, session).then(
      () => deletedOk(forum, session, key),
      (error: unknown) => {
        if (!mounted.current) return;
        if (classifyWriteFailure(error) === 'missing') {
          deletedOk(forum, session, key);
          return;
        }
        patchStatus((state) => withConversation(state, key, {
          deleting: undefined,
          notice: undefined,
          deleteError: chaWebMessage(error, 'The session could not be deleted.'),
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
      const conversations = { ...state.conversations };
      delete conversations[key];
      return { ...state, conversations };
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
    patchStatus((state) => withConversation(
      withConversation(state, from, { sending: undefined }),
      key,
      { pendingText: text },
    ));
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
    patchStatus((state) => withConversation(state, key, { sending: undefined }));
    const failure = classifyWriteFailure(error);
    if (failure === 'rejected') {
      patchStatus((state) => withConversation(
        state, key, { notice: chaWebMessage(error, 'The request failed.') },
      ));
      return;
    }
    if (failure === 'missing') {
      writeMissing(key, forum, chaWebMessage(error, 'The request failed.'));
      return;
    }
    patchStatus((state) => withConversation(
      state, key, { unverified: true, notice: unknownSendNotice },
    ));
    refreshList(forum);
  }

  function inputOk(forum: string, key: string, revision: number, text: string) {
    if (!mounted.current) return;
    updateDrafts((current) => applyAcknowledgement(current, key, revision));
    patchStatus((state) => withConversation(
      state, key, { sending: undefined, pendingText: text },
    ));
    const session = conversationRef.current?.kind === 'session'
      ? conversationRef.current.sessionId : '';
    if (!welcomeIdentity(forum, session)) refreshList(forum);
    noteAck(key);
  }

  function inputFail(forum: string, key: string, error: unknown) {
    if (!mounted.current) return;
    patchStatus((state) => withConversation(state, key, { sending: undefined }));
    const failure = classifyWriteFailure(error);
    if (failure === 'rejected') {
      patchStatus((state) => withConversation(
        state, key, { notice: chaWebMessage(error, 'The request failed.') },
      ));
      return;
    }
    if (failure === 'missing') {
      writeMissing(key, forum, chaWebMessage(error, 'The request failed.'));
      return;
    }
    patchStatus((state) => withConversation(
      state, key, { unverified: true, notice: unknownSendNotice },
    ));
    noteAck(key);
  }

  function stopOk(key: string) {
    if (!mounted.current) return;
    patchStatus((state) => withConversation(
      state, key, { stopPending: undefined, stopping: true },
    ));
    noteAck(key);
  }

  function stopFail(forum: string, key: string, error: unknown) {
    if (!mounted.current) return;
    patchStatus((state) => withConversation(state, key, { stopPending: undefined }));
    const failure = classifyWriteFailure(error);
    if (failure === 'missing') {
      writeMissing(key, forum, chaWebMessage(error, 'The request failed.'));
      return;
    }
    patchStatus((state) => withConversation(state, key, {
      ...(failure === 'unknown' ? { stopping: true } : {}),
      notice: chaWebMessage(error, 'The request failed.'),
    }));
    noteAck(key);
  }

  function retryRead() {
    const selected = selectedSession();
    if (!selected) return;
    const key = sessionDraftKey(selected.forumId, selected.sessionId);
    readLoop.current.failures = 0;
    patchStatus((state) => withConversation(
      { ...state, blockedKey: null, reconnectingKey: key },
      key,
      { notice: undefined },
    ));
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
      if (selected && welcomeIdentity(selected.forumId, selected.sessionId)) refreshWelcome();
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
  const activeStatus = activeKey ? status.conversations[activeKey] : undefined;
  const notice = activeKey
    ? (status.reconnectingKey === activeKey
      ? reconnectingNotice
      : activeStatus?.deleteError ?? activeStatus?.notice
        ?? (snapshotReady ? snapshot?.notice ?? null : null))
    : null;
  const showSending = Boolean(activeStatus?.sending);
  const pendingText = activeStatus?.pendingText ?? null;
  const deleting = Boolean(activeStatus?.deleting);

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
      || command.sending || command.stopPending
      || command.stopping || command.stateUnknown || command.sendBlocked,
    mode: control.mode,
    commandDisabled: control.disabled || deleting,
    deleting,
    deleteDisabled: !canDelete(),
    deleteSession,
    retryConversation: status.blockedKey === activeKey && status.reconnectingKey === null
      ? retryRead
      : null,
    sessionKey: activeKey || 'none',
    // A new draft keeps its audio selection when its first Send creates the session.
    audioKey: active ? audioKey : '',
    currentSessionId: conversation?.kind === 'session' && conversation.forumId === forumId
      ? conversation.sessionId
      : null,
    chooseForum,
    openSession,
    openWelcome,
    welcomeCurrent: conversation?.kind === 'session'
      && bootstrap !== null
      && isWelcomeSession(bootstrap, conversation.forumId, conversation.sessionId),
    newSession,
    showSessions,
    refreshVault,
    vaultBlocked: Object.values(status.conversations).some((current) => (
      current.sending || current.stopPending || current.deleting
    )),
    onDraft,
    send,
    stop,
  };
}
