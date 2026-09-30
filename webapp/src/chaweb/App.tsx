import { useState } from 'react';

import { useVisualViewport } from './viewport';
import { visibleForums } from './route';
import { Conversation } from './conversation';
import { SessionsView } from './sessions';
import { useChaweb } from './useChaweb';
import type { ChaWebClient } from './client';

const emptyTranscript: [] = [];

export function App({ client }: { client: ChaWebClient }) {
  const viewport = useVisualViewport();
  const model = useChaweb(client);
  const [expanded, setExpanded] = useState(false);
  const frame = {
    top: viewport.offsetTop,
    height: viewport.height,
  };

  if (model.loading) {
    return <div className="chaweb-app" style={frame}><p className="chaweb-status">Loading</p></div>;
  }

  if (!model.bootstrap) {
    return (
      <div className="chaweb-app" style={frame}>
        <p className="chaweb-alert" role="alert">{model.startupError}</p>
        {model.startupCanRetry && (
          <button className="chaweb-new-session" onClick={model.retryStartup} type="button">
            Retry
          </button>
        )}
      </div>
    );
  }

  const forums = visibleForums(model.bootstrap);
  if (forums.length === 0) {
    return (
      <div className="chaweb-app" style={frame}>
        <p className="chaweb-alert" role="alert">No forums are available.</p>
      </div>
    );
  }

  return (
    <div className="chaweb-app" style={frame}>
      {model.screen === 'list' ? (
        <SessionsView
          currentSessionId={model.currentSessionId}
          error={model.listError}
          forumId={model.forumId}
          forums={forums}
          onForum={model.chooseForum}
          onNewSession={model.newSession}
          onOpen={model.openSession}
          onRetry={model.listCanRetry ? model.retryList : undefined}
          sessions={model.sessions}
        />
      ) : (
        <Conversation
          characters={model.snapshot?.characters ?? []}
          commandDisabled={model.commandDisabled}
          draft={model.draft}
          entries={model.snapshot?.transcript ?? emptyTranscript}
          expanded={expanded}
          mode={model.mode}
          notice={model.notice}
          onDraft={model.onDraft}
          onExpanded={setExpanded}
          onRetry={model.retryConversation ?? undefined}
          onAllowSend={model.allowSend ?? undefined}
          onSend={model.send}
          onSessions={model.showSessions}
          onStop={model.stop}
          pendingText={model.pendingText}
          personas={model.bootstrap.personas}
          sessionKey={model.sessionKey}
          showSending={model.showSending}
          viewportHeight={viewport.height}
        />
      )}
    </div>
  );
}
