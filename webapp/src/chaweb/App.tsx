import { useEffect, useState } from 'react';

import { ConfirmDialog } from '../components/ConfirmDialog';

import { useVisualViewport } from './viewport';
import { visibleForums } from './route';
import { Conversation } from './conversation';
import { SessionsView } from './sessions';
import { useChaweb } from './useChaweb';
import { useReadAloud } from './useReadAloud';
import { useVoiceInput } from './useVoiceInput';
import type { ChaWebClient } from './client';
import { VaultActions } from './vaultActions';

const emptyTranscript: [] = [];

export function App({ client }: { client: ChaWebClient }) {
  const viewport = useVisualViewport();
  const model = useChaweb(client);
  const speech = useReadAloud(client,
    model.screen === 'conversation' ? model.snapshot : null,
    model.bootstrap?.vault_name, model.deleting, model.audioKey);
  const voice = useVoiceInput(client, model.sessionKey, model.draft,
    model.onDraft, model.send, model.voiceBlocked, speech.busy);
  const [expanded, setExpanded] = useState(false);
  const [vaultBusy, setVaultBusy] = useState(false);
  const [confirmDelete, setConfirmDelete] = useState<{
    key: string; title: string; message: string;
  } | null>(null);
  useEffect(() => {
    setConfirmDelete(null);
  }, [model.screen, model.sessionKey]);
  function toggleAutomaticAudio() {
    const enabled = !speech.automatic;
    speech.toggleAutomatic();
    voice.setListening(enabled);
  }

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
    <div className="chaweb-app" style={frame}>
      {model.screen === 'list' ? (
        <SessionsView
          disabled={vaultBusy}
          currentSessionId={model.currentSessionId}
          error={model.listError}
          forumId={model.forumId}
          forums={forums}
          onForum={model.chooseForum}
          onNewSession={model.newSession}
          onOpen={model.openSession}
          onOpenWelcome={model.openWelcome}
          welcomeCurrent={model.welcomeCurrent}
          onRetry={model.listCanRetry ? model.retryList : undefined}
          sessions={model.sessions}
          vaultActions={<VaultActions
            blocked={model.vaultBlocked}
            bootstrap={model.bootstrap}
            client={client}
            onBusy={setVaultBusy}
            onRefresh={model.refreshVault}
          />}
        />
      ) : (
        <Conversation
          speech={{ ...speech, toggleAutomatic: toggleAutomaticAudio }}
          voice={voice}
          characters={model.snapshot?.characters ?? []}
          commandDisabled={model.commandDisabled}
          deleteDisabled={model.deleteDisabled}
          deleting={model.deleting}
          draft={model.draft}
          entries={model.snapshot?.transcript ?? emptyTranscript}
          expanded={expanded}
          mode={model.mode}
          notice={model.notice}
          onDraft={model.onDraft}
          onDelete={() => {
            if (model.deleteDisabled || !model.snapshot) return;
            setConfirmDelete({
              key: model.sessionKey,
              title: `Delete session “${model.snapshot.session_label}”?`,
              message: [
                'This cannot be undone.',
                model.snapshot.generation.active ? 'The current reply will be stopped.' : '',
                model.draft ? 'The unsent prompt will also be discarded.' : '',
              ].filter(Boolean).join(' '),
            });
          }}
          onExpanded={setExpanded}
          onRetry={model.retryConversation ?? undefined}
          onSend={voice.send}
          onSessions={model.showSessions}
          onStop={model.stop}
          pendingText={model.pendingText}
          personas={model.bootstrap.personas}
          sessionKey={model.sessionKey}
          showSending={model.showSending}
          viewportHeight={viewport.height}
        />
      )}
      {confirmDelete && model.screen === 'conversation'
        && confirmDelete.key === model.sessionKey && (
        <ConfirmDialog
          className="chaweb-delete-dialog"
          confirmLabel="Delete"
          initialFocus="cancel"
          message={confirmDelete.message}
          onCancel={() => setConfirmDelete(null)}
          onConfirm={() => {
            model.deleteSession(confirmDelete.key);
            setConfirmDelete(null);
          }}
          title={confirmDelete.title}
        />
      )}
    </div>
  );
}
