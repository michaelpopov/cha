import { useEffect, type Dispatch } from 'react';

import { ChaError, type ChaClient } from './api/client';
import type { AppAction, AppState, SessionActivity } from './state/view';

// The native live stream follows one session. Check only sessions left
// generating, and stop checking as soon as their generation ends.
export function useSessionActivity(client: ChaClient, state: AppState, dispatch: Dispatch<AppAction>) {
  const sessions = state.sessionActivity;
  const connectedForum = state.streamStatus === 'connected' ? state.activeConversation?.forumId : undefined;
  const connectedSession = connectedForum ? state.activeConversation?.sessionId : undefined;
  useEffect(() => {
    const background = sessions.filter((session) => session.status === 'generating'
      && (session.forumId !== connectedForum || session.sessionId !== connectedSession));
    if (!background.length) return;
    let canceled = false;
    let warned = false;
    let timer: ReturnType<typeof setTimeout>;
    const finish = (session: SessionActivity, deleted = false) => {
      if (canceled) return;
      const { forumId, sessionId } = session;
      dispatch({ type: deleted ? 'session-deleted' : 'session-activity-ended', forumId, sessionId });
    };
    const check = async () => {
      await Promise.all(background.map(async (session) => {
        try {
          const current = await client.getSessionSnapshot(session.forumId, session.sessionId);
          if (current.forum.id === session.forumId && current.session_id === session.sessionId
              && !current.generation.active) finish(session);
        } catch (failure: unknown) {
          // Unselected sessions retire after generation ends. Do not reopen one
          // merely to check its status, or mistake a transport error for completion.
          if (failure instanceof ChaError && failure.code === 'session_not_live') finish(session);
          else if (failure instanceof ChaError && failure.code === 'not_found') finish(session, true);
          else if (!warned && !(failure instanceof ChaError && failure.code === 'session_stopping')) {
            warned = true;
            console.warn('Session activity could not be checked.', failure);
          }
        }
      }));
      if (!canceled) timer = setTimeout(() => { void check(); }, 1_000);
    };
    void check();
    return () => {
      canceled = true;
      clearTimeout(timer);
    };
  }, [client, dispatch, sessions, connectedForum, connectedSession]);
}
