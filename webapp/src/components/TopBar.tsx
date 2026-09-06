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
import { CheckIcon, CloseIcon, EditIcon, FileUpIcon, SidebarIcon } from './Icons';

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
        onChange={(event) => setDraft(event.target.value)}
        onFocus={(event) => event.currentTarget.select()}
        onKeyDown={(event) => {
          if (event.key === 'Escape') cancel();
        }}
        value={draft}
      />
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
  onCharacterDefinitionUpdated(): void;
  onForumDefinitionUpdated(): void;
  state: AppState;
  title: string | null;
}

export function TopBar({
  client,
  dispatch,
  onCharacterDefinitionUpdated,
  onForumDefinitionUpdated,
  state,
  title,
}: TopBarProps) {
  const personaId = state.inspectedPersonaId;
  const personaName = state.bootstrap?.personas.find(({ id }) => id === personaId)?.display_name;
  const characterId = state.inspectedCharacterId;
  const characterName = state.bootstrap?.characters.find(
    ({ id }) => id === characterId,
  )?.display_name;
  const forumId = state.currentForumId;
  const forumName = state.bootstrap?.forums.find(({ id }) => id === forumId)?.display_name;

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
  }

  return (
    <header className="cha-topbar">
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
      {state.mainView === 'character-detail'
        && state.characterSettingsAvailable
        && characterId
        && (
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
        )}
      {state.mainView === 'forum-detail'
        && state.forumEditingAvailable
        && forumId
        && (
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
        )}
    </header>
  );
}
