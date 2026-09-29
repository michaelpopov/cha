import { useCallback, useEffect, useRef, useState } from 'react';

import {
  type Bootstrap,
  type SessionListing,
  type SessionSnapshot,
} from '../api/client';
import { ChaWebError, chaWebMessage, type ChaWebClient } from './client';
import { Conversation } from './conversation';
import {
  defaultForumId,
  parseChawebHash,
  sameChawebPlace,
  sessionHash,
  sessionUnavailable,
  visibleForums,
} from './route';
import { SessionsView } from './sessions';
import { useVisualViewport } from './viewport';

const emptyTranscript: SessionSnapshot['transcript'] = [];

type ConversationRef =
  | { kind: 'draft'; forumId: string }
  | { kind: 'session'; forumId: string; sessionId: string };

function draftKey(conversation: ConversationRef): string {
  return conversation.kind === 'draft'
    ? `new:${conversation.forumId}`
    : `session:${conversation.forumId}/${conversation.sessionId}`;
}

function locationUrl(hash = ''): string {
  return `${window.location.pathname}${window.location.search}${hash}`;
}

export function App({ client }: { client: ChaWebClient }) {
  const viewport = useVisualViewport();
  const [bootstrap, setBootstrap] = useState<Bootstrap | null>(null);
  const [loading, setLoading] = useState(true);
  const [startupError, setStartupError] = useState<string | null>(null);
  const [retry, setRetry] = useState(0);
  const [routeReady, setRouteReady] = useState(false);
  const [screen, setScreen] = useState<'list' | 'conversation'>('list');
  const [forumId, setForumId] = useState('');
  const [conversation, setConversation] = useState<ConversationRef | null>(null);
  const [drafts, setDrafts] = useState<Record<string, string>>({});
  const [expanded, setExpanded] = useState(false);
  const [sessions, setSessions] = useState<SessionListing[]>([]);
  const [listError, setListError] = useState<string | null>(null);
  const [listVersion, setListVersion] = useState(0);
  const [snapshot, setSnapshot] = useState<SessionSnapshot | null>(null);
  const [conversationError, setConversationError] = useState<string | null>(null);
  const appliedHash = useRef<string | null>(null);
  const loadToken = useRef(0);

  useEffect(() => {
    let active = true;
    setLoading(true);
    setStartupError(null);
    void client.getBootstrap().then((loaded) => {
      if (!active) return;
      setBootstrap(loaded);
      setForumId((current) => current || defaultForumId(loaded));
      setLoading(false);
    }).catch((error: unknown) => {
      if (!active) return;
      setBootstrap(null);
      setStartupError(chaWebMessage(error, 'ChaWeb could not load.'));
      setLoading(false);
    });
    return () => {
      active = false;
    };
  }, [client, retry]);

  const loadSession = useCallback((forum: string, session: string) => {
    const token = ++loadToken.current;
    setConversation({ kind: 'session', forumId: forum, sessionId: session });
    setForumId(forum);
    setScreen('conversation');
    setSnapshot(null);
    setConversationError(null);
    setListError(null);
    void client.getSession(forum, session).then((loaded) => {
      if (token !== loadToken.current) return;
      if (loaded.forum.id !== forum || loaded.session_id !== session) return;
      setSnapshot(loaded);
    }).catch((error: unknown) => {
      if (token !== loadToken.current) return;
      setSnapshot(null);
      const message = chaWebMessage(error, 'The conversation could not be loaded.');
      if (error instanceof ChaWebError && error.status === 404) {
        setListError(message);
        setScreen('list');
        setListVersion((version) => version + 1);
        return;
      }
      setConversationError(message);
    });
  }, [client]);

  const applyHistory = useCallback((hash: string) => {
    appliedHash.current = hash;
    if (!bootstrap) return;
    const route = parseChawebHash(hash);
    if (route.kind === 'list') {
      setScreen('list');
      return;
    }
    if (route.kind === 'unknown'
        || sessionUnavailable(bootstrap, route.forumId, route.sessionId)) {
      setListError('That conversation is not available.');
      setScreen('list');
      setListVersion((version) => version + 1);
      return;
    }
    loadSession(route.forumId, route.sessionId);
  }, [bootstrap, loadSession]);

  useEffect(() => {
    if (!bootstrap) return;
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
  }, [applyHistory, bootstrap]);

  useEffect(() => {
    if (!routeReady || !bootstrap || screen !== 'list' || !forumId) return;
    let active = true;
    void client.listSessions(forumId).then((loaded) => {
      if (!active) return;
      setSessions(loaded);
    }).catch((error: unknown) => {
      if (!active) return;
      setSessions([]);
      setListError(chaWebMessage(error, 'Sessions could not be loaded.'));
    });
    return () => {
      active = false;
    };
  }, [bootstrap, client, forumId, listVersion, routeReady, screen]);

  function chooseForum(next: string) {
    setForumId(next);
    setListError(null);
  }

  function openSession(sessionId: string) {
    const hash = sessionHash(forumId, sessionId);
    appliedHash.current = hash;
    if (window.location.hash !== hash) {
      window.history.pushState(null, '', locationUrl(hash));
    }
    loadSession(forumId, sessionId);
  }

  function newSession() {
    if (!forumId) return;
    appliedHash.current = '';
    if (window.location.hash) window.history.pushState(null, '', locationUrl());
    setConversation({ kind: 'draft', forumId });
    setSnapshot(null);
    setConversationError(null);
    setScreen('conversation');
  }

  function showSessions() {
    if (conversation) setForumId(conversation.forumId);
    setScreen('list');
  }

  function updateDraft(value: string) {
    if (!conversation) return;
    const key = draftKey(conversation);
    setDrafts((current) => ({ ...current, [key]: value }));
  }

  const frame = {
    top: viewport.offsetTop,
    height: viewport.height,
  };

  if (loading) {
    return <div className="chaweb-app" style={frame}><p className="chaweb-status">Loading</p></div>;
  }

  if (!bootstrap) {
    return (
      <div className="chaweb-app" style={frame}>
        <p className="chaweb-alert" role="alert">{startupError}</p>
        <button className="chaweb-new-session" onClick={() => setRetry((count) => count + 1)} type="button">
          Retry
        </button>
      </div>
    );
  }

  const forums = visibleForums(bootstrap);
  if (forums.length === 0) {
    return (
      <div className="chaweb-app" style={frame}>
        <p className="chaweb-alert" role="alert">No forums are available.</p>
      </div>
    );
  }

  const draft = conversation ? drafts[draftKey(conversation)] ?? '' : '';
  const entries = snapshot?.transcript ?? emptyTranscript;
  const notice = conversationError || snapshot?.notice || null;
  const currentSessionId = conversation?.kind === 'session' && conversation.forumId === forumId
    ? conversation.sessionId
    : null;
  const sessionKey = conversation
    ? draftKey(conversation)
    : 'none';

  return (
    <div className="chaweb-app" style={frame}>
      {screen === 'list' ? (
        <SessionsView
          currentSessionId={currentSessionId}
          error={listError}
          forumId={forumId}
          forums={forums}
          onForum={chooseForum}
          onNewSession={newSession}
          onOpen={openSession}
          sessions={sessions}
        />
      ) : (
        <Conversation
          characters={snapshot?.characters ?? []}
          commandDisabled={false}
          draft={draft}
          entries={entries}
          expanded={expanded}
          mode="send"
          notice={notice}
          onDraft={updateDraft}
          onExpanded={setExpanded}
          onSend={() => {}}
          onSessions={showSessions}
          onStop={() => {}}
          personas={bootstrap.personas}
          sessionKey={sessionKey}
          viewportHeight={viewport.height}
        />
      )}
    </div>
  );
}
