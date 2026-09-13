import {
  useEffect,
  useRef,
  useState,
  type ChangeEvent,
  type Dispatch,
  type FormEvent,
} from 'react';

import { publicErrorMessage, type ChaClient } from '../api/client';
import type { AppAction, AppState } from '../state/view';
import { ConfirmDialog } from './ConfirmDialog';
import {
  CheckIcon,
  CloseIcon,
  EditIcon,
  FileUpIcon,
  SidebarIcon,
  SkullBonesIcon,
  TextLinesIcon,
} from './Icons';
import { TextEditorDialog } from './TextEditorDialog';
import { TransliterationToggle, useTransliteration } from './TransliterationMode';

interface EditableTitleProps {
  available: boolean;
  id: string | null;
  name: string | undefined;
  onSave(displayName: string): Promise<void>;
  subject: string;
}

function EditableTitle({ available, id, name, onSave, subject }: EditableTitleProps) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(name ?? '');
  const transliteration = useTransliteration<HTMLInputElement>(draft);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setEditing(false);
    setDraft(name ?? '');
    setError(null);
  }, [id, name]);

  if (!id || !name) return null;
  if (!available) return <h1>{name}</h1>;

  function cancel() {
    setDraft(name ?? '');
    setEditing(false);
    setError(null);
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    const displayName = draft.trim();
    if (!displayName || saving) return;
    if (displayName === name) {
      setEditing(false);
      return;
    }
    setSaving(true);
    setError(null);
    try {
      await onSave(displayName);
      setEditing(false);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, `${subject} name could not be saved.`));
    } finally {
      setSaving(false);
    }
  }

  if (!editing) {
    return (
      <button
        aria-label={`Rename ${name}`}
        className="cha-persona-title-trigger"
        onClick={() => setEditing(true)}
        type="button"
      >
        <span>{name}</span>
        <EditIcon />
      </button>
    );
  }

  const lowerSubject = subject.toLowerCase();
  return (
    <form className="cha-persona-title-form" onSubmit={(event) => void save(event)}>
      <input
        aria-label={`${subject} name`}
        autoFocus
        disabled={saving}
        onChange={(event) => setDraft(transliteration.convert(event, draft))}
        onFocus={(event) => event.currentTarget.select()}
        onKeyDown={(event) => {
          if (event.key === 'Escape') cancel();
        }}
        ref={transliteration.field}
        value={draft}
      />
      <TransliterationToggle disabled={saving} transliteration={transliteration} />
      <button
        aria-label={`Save ${lowerSubject} name`}
        className="cha-title-icon-action"
        disabled={saving || draft.trim() === ''}
        type="submit"
      >
        <CheckIcon />
      </button>
      <button
        aria-label="Cancel renaming"
        className="cha-title-icon-action"
        disabled={saving}
        onClick={cancel}
        type="button"
      >
        <CloseIcon />
      </button>
      {error && <span className="cha-persona-title-error" role="alert">{error}</span>}
    </form>
  );
}

interface DefinitionUploadProps {
  ariaLabel: string;
  failureMessage: string;
  id: string;
  onUpload(markdown: string): Promise<void>;
}

function DefinitionUpload({ ariaLabel, failureMessage, id, onUpload }: DefinitionUploadProps) {
  const input = useRef<HTMLInputElement>(null);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => setError(null), [id]);

  async function replaceFromFile(event: ChangeEvent<HTMLInputElement>) {
    const file = event.target.files?.[0];
    if (!file || saving) return;
    setSaving(true);
    setError(null);
    try {
      await onUpload(await file.text());
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, failureMessage));
    } finally {
      setSaving(false);
      if (input.current) input.current.value = '';
    }
  }

  return (
    <div className="cha-definition-topbar-action">
      <input
        accept=".md,.txt,text/markdown,text/plain"
        className="cha-file-input"
        onChange={(event) => void replaceFromFile(event)}
        ref={input}
        type="file"
      />
      <button
        aria-label={ariaLabel}
        className="cha-compact-icon-action"
        disabled={saving}
        onClick={() => input.current?.click()}
        type="button"
      >
        <FileUpIcon />
      </button>
      {error && <span className="cha-definition-upload-error" role="alert">{error}</span>}
    </div>
  );
}

interface TopBarProps {
  client: ChaClient;
  dispatch: Dispatch<AppAction>;
  onDeleteCharacter(characterId: string): Promise<void>;
  onDeleteForum(forumId: string): Promise<void>;
  onDeletePersona(personaId: string): Promise<void>;
  onCharacterDefinitionUpdated(): void;
  onForumDefinitionUpdated(): void;
  onPersonaDefinitionUpdated(): void;
  onProviderUpdated(): void;
  onStyleUpdated(): void;
  onVoiceUpdated(): void;
  state: AppState;
  title: string | null;
}

type DeleteSubject = {
  id: string;
  kind: 'persona' | 'character' | 'forum';
  name: string;
};

function deleteMessage({ kind, name }: DeleteSubject): string {
  if (kind === 'persona') {
    return `Delete “${name}”? This permanently removes its profile. This cannot be undone.`;
  }
  if (kind === 'character') {
    return `Delete “${name}”? This permanently removes its definition and settings. Existing chat transcripts are kept. This cannot be undone.`;
  }
  return `Delete “${name}”? This permanently removes the forum and all of its sessions. This cannot be undone.`;
}

function editorTitle(kind: DeleteSubject['kind']): string {
  if (kind === 'persona') return 'Edit persona profile';
  if (kind === 'character') return 'Edit character definition';
  return 'Edit forum description';
}

export function TopBar({
  client,
  dispatch,
  onDeleteCharacter,
  onDeleteForum,
  onDeletePersona,
  onCharacterDefinitionUpdated,
  onForumDefinitionUpdated,
  onPersonaDefinitionUpdated,
  onProviderUpdated,
  onStyleUpdated,
  onVoiceUpdated,
  state,
  title,
}: TopBarProps) {
  const [confirmingDelete, setConfirmingDelete] = useState<DeleteSubject | null>(null);
  const [deleting, setDeleting] = useState(false);
  const [deleteError, setDeleteError] = useState<string | null>(null);
  const [editorSubject, setEditorSubject] = useState<DeleteSubject | null>(null);
  const [editorText, setEditorText] = useState('');
  const [editorLoading, setEditorLoading] = useState(false);
  const [editorReady, setEditorReady] = useState(false);
  const [editorSaving, setEditorSaving] = useState(false);
  const [editorError, setEditorError] = useState<string | null>(null);
  const personaId = state.inspectedPersonaId;
  const personaName = state.bootstrap?.personas.find(({ id }) => id === personaId)?.display_name;
  const characterId = state.inspectedCharacterId;
  const characterName = state.bootstrap?.characters.find(
    ({ id }) => id === characterId,
  )?.display_name;
  const forumId = state.currentForumId;
  const forumName = state.bootstrap?.forums.find(({ id }) => id === forumId)?.display_name;
  const providerId = state.inspectedProviderId;
  const providerName = state.inspectedProviderName ?? undefined;
  const styleId = state.inspectedStyleId;
  const styleName = state.inspectedStyleName ?? undefined;
  const voiceId = state.inspectedVoiceId;
  const voiceName = state.inspectedVoiceName ?? undefined;
  const apiKeyId = state.inspectedApiKeyId;
  const apiKeyName = state.inspectedApiKeyName ?? undefined;
  const vaultName = state.inspectedVaultName ?? undefined;

  useEffect(() => {
    setConfirmingDelete(null);
    setDeleting(false);
    setDeleteError(null);
    setEditorSubject(null);
    setEditorText('');
    setEditorLoading(false);
    setEditorReady(false);
    setEditorSaving(false);
    setEditorError(null);
  }, [apiKeyId, characterId, forumId, personaId, providerId, state.mainView, styleId, vaultName, voiceId]);

  let titleControl = title && <h1>{title}</h1>;
  if (state.mainView === 'persona-detail') {
    titleControl = (
      <EditableTitle
        available={state.personaEditingAvailable}
        id={personaId}
        name={personaName}
        onSave={async (displayName) => {
          const persona = await client.updatePersona(personaId!, { display_name: displayName });
          dispatch({ type: 'persona-updated', persona });
        }}
        subject="Persona"
      />
    );
  } else if (state.mainView === 'character-detail') {
    titleControl = (
      <EditableTitle
        available={state.characterSettingsAvailable}
        id={characterId}
        name={characterName}
        onSave={async (displayName) => {
          const character = await client.updateCharacterDefinition(characterId!, {
            display_name: displayName,
          });
          dispatch({ type: 'character-updated', character });
        }}
        subject="Character"
      />
    );
  } else if (state.mainView === 'forum-detail') {
    titleControl = (
      <EditableTitle
        available={state.forumEditingAvailable}
        id={forumId}
        name={forumName}
        onSave={async (displayName) => {
          const forum = await client.updateForum(forumId!, { display_name: displayName });
          dispatch({ type: 'forum-updated', forum });
        }}
        subject="Forum"
      />
    );
  } else if (state.mainView === 'settings-provider') {
    titleControl = (
      <EditableTitle
        available={state.providerEditingAvailable}
        id={providerId}
        name={providerName}
        onSave={async (displayName) => {
          const provider = await client.getProvider(providerId!);
          const { id: _id, used_by: _usedBy, writable: _writable, ...update } = provider;
          const saved = await client.updateProvider(providerId!, {
            ...update,
            display_name: displayName,
          });
          dispatch({
            type: 'provider-updated',
            providerId: saved.id,
            providerName: saved.display_name,
            writable: saved.writable,
          });
          onProviderUpdated();
        }}
        subject="Provider"
      />
    );
  } else if (state.mainView === 'settings-style') {
    titleControl = (
      <EditableTitle
        available={state.styleEditingAvailable}
        id={styleId}
        name={styleName}
        onSave={async (displayName) => {
          const style = (await client.listStyles()).find(({ id }) => id === styleId);
          if (!style) throw new Error('That style was not found.');
          const { id: _id, used_by: _usedBy, writable: _writable, ...update } = style;
          const saved = await client.updateStyle(styleId!, {
            ...update,
            display_name: displayName,
          });
          dispatch({
            type: 'style-updated',
            styleId: saved.id,
            styleName: saved.display_name,
            writable: saved.writable,
          });
          onStyleUpdated();
        }}
        subject="Style"
      />
    );
  } else if (state.mainView === 'settings-voice') {
    titleControl = (
      <EditableTitle
        available={state.voiceEditingAvailable}
        id={voiceId}
        name={voiceName}
        onSave={async (displayName) => {
          const voice = (await client.listVoices()).find(({ id }) => id === voiceId);
          if (!voice) throw new Error('That voice was not found.');
          const { id: _id, used_by: _usedBy, writable: _writable, ...update } = voice;
          const saved = await client.updateVoice(voiceId!, {
            ...update,
            display_name: displayName,
          });
          dispatch({
            type: 'voice-updated',
            voiceId: saved.id,
            voiceName: saved.display_name,
            writable: saved.writable,
          });
          onVoiceUpdated();
        }}
        subject="Voice"
      />
    );
  } else if (state.mainView === 'settings-api-key') {
    titleControl = (
      <EditableTitle
        available
        id={apiKeyId}
        name={apiKeyName}
        onSave={async (displayName) => {
          const saved = await client.renameApiKey(apiKeyId!, displayName);
          dispatch({
            type: 'api-key-updated',
            apiKeyId: saved.id,
            apiKeyName: saved.display_name,
          });
        }}
        subject="API key"
      />
    );
  } else if (state.mainView === 'settings-vault') {
    titleControl = (
      <EditableTitle
        available
        id={vaultName ?? null}
        name={vaultName}
        onSave={async (displayName) => {
          const saved = await client.updateVault(vaultName!, {
            display_name: displayName,
            password: null,
          });
          dispatch({
            type: 'vault-updated',
            previousName: vaultName!,
            vault: saved,
          });
        }}
        subject="Vault"
      />
    );
  }

  let deleteSubject: DeleteSubject | null = null;
  let uploadAction = null;
  if (state.mainView === 'persona-detail'
      && state.personaEditingAvailable && personaId && personaName) {
    deleteSubject = { id: personaId, kind: 'persona', name: personaName };
    uploadAction = (
      <DefinitionUpload
        ariaLabel="Replace persona description from file"
        failureMessage="Persona description could not be replaced."
        id={personaId}
        onUpload={async (personaMarkdown) => {
          const persona = await client.updatePersona(personaId, {
            persona_markdown: personaMarkdown,
          });
          dispatch({ type: 'persona-updated', persona });
          onPersonaDefinitionUpdated();
        }}
      />
    );
  } else if (state.mainView === 'character-detail'
      && state.characterSettingsAvailable && characterId && characterName) {
    deleteSubject = { id: characterId, kind: 'character', name: characterName };
    uploadAction = (
      <DefinitionUpload
        ariaLabel="Replace character definition from file"
        failureMessage="Character definition could not be replaced."
        id={characterId}
        onUpload={async (characterMarkdown) => {
          const character = await client.updateCharacterDefinition(characterId, {
            character_markdown: characterMarkdown,
          });
          dispatch({ type: 'character-updated', character });
          onCharacterDefinitionUpdated();
        }}
      />
    );
  } else if (state.mainView === 'forum-detail'
      && state.forumEditingAvailable && forumId && forumName) {
    deleteSubject = { id: forumId, kind: 'forum', name: forumName };
    uploadAction = (
      <DefinitionUpload
        ariaLabel="Replace forum definition from file"
        failureMessage="Forum definition could not be replaced."
        id={forumId}
        onUpload={async (forumMarkdown) => {
          const forum = await client.updateForum(forumId, {
            forum_markdown: forumMarkdown,
          });
          dispatch({ type: 'forum-updated', forum });
          onForumDefinitionUpdated();
        }}
      />
    );
  }

  async function confirmDelete() {
    const subject = confirmingDelete;
    if (!subject || deleting) return;
    setConfirmingDelete(null);
    setDeleting(true);
    setDeleteError(null);
    try {
      if (subject.kind === 'persona') await onDeletePersona(subject.id);
      else if (subject.kind === 'character') await onDeleteCharacter(subject.id);
      else await onDeleteForum(subject.id);
    } catch (failure: unknown) {
      const label = subject.kind[0].toUpperCase() + subject.kind.slice(1);
      setDeleteError(publicErrorMessage(failure, `${label} could not be deleted.`));
      setDeleting(false);
    }
  }

  async function openEditor(subject: DeleteSubject) {
    setEditorSubject(subject);
    setEditorText('');
    setEditorLoading(true);
    setEditorReady(false);
    setEditorError(null);
    try {
      if (subject.kind === 'persona') {
        setEditorText((await client.getPersona(subject.id)).persona_markdown);
      } else if (subject.kind === 'character') {
        setEditorText((await client.getCharacter(subject.id)).editable_markdown);
      } else {
        setEditorText((await client.getForum(subject.id)).forum_markdown);
      }
      setEditorReady(true);
    } catch (failure: unknown) {
      setEditorError(publicErrorMessage(
        failure,
        `${editorTitle(subject.kind)} could not be loaded.`,
      ));
    } finally {
      setEditorLoading(false);
    }
  }

  async function saveEditor() {
    const subject = editorSubject;
    if (!subject || !editorReady || editorLoading || editorSaving) return;
    setEditorSaving(true);
    setEditorError(null);
    try {
      if (subject.kind === 'persona') {
        const persona = await client.updatePersona(subject.id, {
          persona_markdown: editorText,
        });
        dispatch({ type: 'persona-updated', persona });
        onPersonaDefinitionUpdated();
      } else if (subject.kind === 'character') {
        const character = await client.updateCharacterDefinition(subject.id, {
          character_markdown: editorText,
        });
        dispatch({ type: 'character-updated', character });
        onCharacterDefinitionUpdated();
      } else {
        const forum = await client.updateForum(subject.id, {
          forum_markdown: editorText,
        });
        dispatch({ type: 'forum-updated', forum });
        onForumDefinitionUpdated();
      }
      setEditorSubject(null);
    } catch (failure: unknown) {
      setEditorError(publicErrorMessage(
        failure,
        `${editorTitle(subject.kind)} could not be saved.`,
      ));
    } finally {
      setEditorSaving(false);
    }
  }

  return (
    <>
      <header className={`cha-topbar ${deleteSubject ? 'has-actions' : ''}`}>
        <div className="cha-topbar-lead">
          <button
            aria-expanded={state.sidebarOpen}
            aria-label={state.sidebarOpen ? 'Hide sidebar' : 'Show sidebar'}
            className="cha-icon-action"
            onClick={() => dispatch({ type: 'toggle-sidebar' })}
            type="button"
          >
            <SidebarIcon />
          </button>
        </div>
        <div className="cha-topbar-title">{titleControl}</div>
        {/* Balances the leading control so a navigation title stays centred. */}
        {title && <div className="cha-topbar-balance" aria-hidden="true" />}
        {deleteSubject && (
          <div className="cha-topbar-actions">
            <button
              aria-label={editorTitle(deleteSubject.kind)}
              className="cha-compact-icon-action"
              onClick={() => void openEditor(deleteSubject)}
              title={editorTitle(deleteSubject.kind)}
              type="button"
            >
              <TextLinesIcon />
            </button>
            {uploadAction}
            <button
              aria-label={`Delete ${deleteSubject.name}`}
              className="cha-compact-icon-action cha-danger-icon-action"
              disabled={deleting}
              onClick={() => setConfirmingDelete(deleteSubject)}
              title={`Delete ${deleteSubject.name}`}
              type="button"
            >
              <SkullBonesIcon />
            </button>
            {deleteError && (
              <span className="cha-definition-upload-error" role="alert">{deleteError}</span>
            )}
          </div>
        )}
      </header>
      {confirmingDelete && (
        <ConfirmDialog
          confirmLabel={`Delete ${confirmingDelete.kind}`}
          message={deleteMessage(confirmingDelete)}
          onCancel={() => setConfirmingDelete(null)}
          onConfirm={() => void confirmDelete()}
          title={`Delete ${confirmingDelete.kind}?`}
        />
      )}
      {editorSubject && (
        <TextEditorDialog
          error={editorError}
          loading={editorLoading}
          onCancel={() => setEditorSubject(null)}
          onChange={setEditorText}
          onSave={() => void saveEditor()}
          ready={editorReady}
          saving={editorSaving}
          title={editorTitle(editorSubject.kind)}
          value={editorText}
        />
      )}
    </>
  );
}
