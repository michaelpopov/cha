import { useEffect, useLayoutEffect, useRef, useState } from 'react';

import { useVisualViewport } from './viewport';
import { visibleForums } from './route';
import { Conversation } from './conversation';
import { SessionsView } from './sessions';
import { useChaweb } from './useChaweb';
import { useReadAloud } from './useReadAloud';
import { useVoiceInput } from './useVoiceInput';
import type { ChaWebClient } from './client';
import { NotesView, followLocalLink } from './notes';
import { NoteView } from './note';

const emptyTranscript: [] = [];

export function App({ client }: { client: ChaWebClient }) {
  const viewport = useVisualViewport();
  const model = useChaweb(client);
  const reading = model.screen === 'note';
  useLayoutEffect(() => {
    document.documentElement.classList.toggle('chaweb-reading', reading);
    return () => document.documentElement.classList.remove('chaweb-reading');
  }, [reading]);
  const speech = useReadAloud(client,
    model.screen === 'conversation' ? model.snapshot : null,
    model.bootstrap?.vault_name, model.deleting, model.audioKey);
  const voice = useVoiceInput(client, model.sessionKey, model.draft,
    model.onDraft, model.send, model.voiceBlocked, speech.busy);
  const [expanded, setExpanded] = useState(false);
  // Voice mode is one switch for automatic audio and the microphone.
  function toggleVoiceMode() {
    const on = !speech.automatic && !voice.enabled;
    if (speech.automatic !== on) speech.toggleAutomatic();
    voice.setListening(on);
  }
  // A part can stop by itself: the microphone on a hidden page, or audio
  // after an error. Then stop the other part too.
  const voiceParts = useRef({ automatic: false, listening: false });
  useEffect(() => {
    const before = voiceParts.current;
    voiceParts.current = { automatic: speech.automatic, listening: voice.enabled };
    if (before.automatic && !speech.automatic) voice.setListening(false);
    else if (before.listening && !voice.enabled && speech.automatic) speech.toggleAutomatic();
  }, [speech.automatic, voice.enabled]);

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

  return (
    <div className={`chaweb-app${reading ? ' chaweb-reading-app' : ''}`} style={reading ? undefined : frame}>
      {model.screen === 'list' ? (
        <SessionsView
          onDelete={model.deleteSession}
          canDelete={model.canDelete}
          isDeleting={model.isDeleting}
          currentSessionId={model.currentSessionId}
          error={model.listError}
          forumId={model.forumId}
          entranceForumId={model.bootstrap.entrance_forum_id}
          forums={forums}
          onForum={model.chooseForum}
          onNewSession={model.newSession}
          onOpen={model.openSession}
          onRetry={model.listCanRetry ? model.retryList : undefined}
          sessions={model.sessions}
        />
      ) : model.screen === 'notes' ? (
        <NotesView model={model.notes} listRef={model.noteList} navigate={model.navigate} />
      ) : model.screen === 'note' ? (
        <NoteView model={model.notes} />
      ) : (
        <Conversation
          loaded={model.snapshot !== null}
          contextKey={model.focusKey}
          promptFocus={model.promptFocus}
          onPromptFocused={model.consumePromptFocus}
          speech={{ ...speech, toggleAutomatic: toggleVoiceMode }}
          voice={voice}
          characters={model.snapshot?.characters ?? []}
          commandDisabled={model.commandDisabled}
          deleting={model.deleting}
          draft={model.draft}
          entries={model.snapshot?.transcript ?? emptyTranscript}
          expanded={expanded}
          mode={model.mode}
          notice={model.notice}
          onDraft={model.onDraft}
          onExpanded={setExpanded}
          onRetry={model.retryConversation ?? undefined}
          onSend={voice.send}
          onStop={model.stop}
          pendingText={model.pendingText}
          personas={model.bootstrap.personas}
          sessionKey={model.sessionKey}
          showSending={model.showSending}
          viewportHeight={viewport.height}
        />
      )}
      <nav className="chaweb-navigation" aria-label="Main navigation">
        <a href="#/" aria-current={model.screen === 'list' || model.screen === 'conversation' ? 'page' : undefined}
          onClick={(event) => followLocalLink(event, () => model.showRoot('chats'))}>Chats</a>
        <a href={model.notesRootHash}
          aria-current={model.screen === 'notes' || model.screen === 'note' ? 'page' : undefined}
          onClick={(event) => followLocalLink(event, () => model.showRoot('notes'))}>Notes</a>
      </nav>
    </div>
  );
}
