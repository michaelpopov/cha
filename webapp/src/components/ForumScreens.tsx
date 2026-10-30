import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type FormEvent,
} from 'react';
import { publicErrorMessage, type ForumDetail, type ForumSummary } from '../api/client';
import { StaleNotice, useDetailRefresh } from '../detailRefresh';
import { DetailActions, EditableTitle } from './DetailActions';
import { ChevronLeftIcon, ChevronRightIcon, MoreIcon, PlusIcon } from './Icons';
import { SystemPromptDialog } from './SystemPromptDialog';
import { BackToSettings } from './SettingsShared';
import { TransliteratingInput } from './TransliterationMode';
import { FileList } from './FileScreens';
import {
  RosterDetailScreen,
  RosterRow,
  type NavigationScreenProps,
  type RosterDetailProps,
} from './RosterScreens';

export function forumMemberNames(forum: ForumSummary): string {
  return forum.members.map(({ display_name }) => display_name).join(', ') || 'No characters';
}

// Who is in a forum describes it as well as prose does, so a forum that
// configures no short description keeps naming its cast rather than showing a
// bare row. Either way the line answers the same question the other rosters
// answer with a description.
export function forumRosterDescription(forum: ForumSummary): string {
  return forum.description ?? forumMemberNames(forum);
}

export function ForumsScreen({ state, navigate }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Forums navigation">
      <BackToSettings navigate={navigate} />
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => navigate({ type: 'show-new-forum' })}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New forum</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
        {state.bootstrap?.forums.map((forum) => (
          <RosterRow
            description={forumRosterDescription(forum)}
            displayName={forum.display_name}
            key={forum.id}
            onSelect={() => navigate({ type: 'select-forum', forumId: forum.id })}
          />
        ))}
      </div>
    </section>
  );
}

export function NewForumScreen({
  state,
  navigate,
  client,
  onForumsChanged,
}: RosterDetailProps & { onForumsChanged?(): void }) {
  const personas = state.bootstrap?.personas ?? [];
  const [name, setName] = useState('');
  const [personaId, setPersonaId] = useState(personas[0]?.id ?? '');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const trimmedName = name.trim();

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!trimmedName || !personaId || saving) return;
    setSaving(true);
    setError(null);
    try {
      const forum = await client.createForum({
        display_name: trimmedName,
        persona_id: personaId,
      });
      onForumsChanged?.();
      navigate({ type: 'forum-created', forum });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The forum could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New forum navigation">
      <button
        className="cha-back-row"
        onClick={() => navigate({ type: 'show-forums' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Forums</span>
      </button>
      <form className="cha-settings-form" onSubmit={(event) => void submit(event)}>
        <TransliteratingInput
          autoComplete="off"
          autoFocus
          className="cha-form-control"
          disabled={saving}
          id="cha-forum-name"
          label="Name"
          onValueChange={setName}
          placeholder="e.g. Brain Trust"
          type="text"
          value={name}
        />
        <label htmlFor="cha-forum-persona">Persona
          <select
            className="cha-form-control"
            disabled={saving}
            id="cha-forum-persona"
            onChange={(event) => setPersonaId(event.target.value)}
            value={personaId}
          >
            {personas.map((persona) => (
              <option key={persona.id} value={persona.id}>{persona.display_name}</option>
            ))}
          </select>
        </label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={saving}
            onClick={() => navigate({ type: 'show-forums' })}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!trimmedName || !personaId || saving}
            type="submit"
          >
            Create forum
          </button>
        </div>
      </form>
    </section>
  );
}

// Members and the persona a forum speaks as, shown under its name. Both are
// already in bootstrap, so this needs no request of its own.
function ForumCast({ forum }: { forum: ForumSummary }) {
  return (
    <p className="cha-forum-cast">
      {forumMemberNames(forum)}
      {' · speaking as '}
      {forum.default_persona_display_name}
    </p>
  );
}

export function ForumDetailScreen({
  state,
  dispatch,
  navigate,
  client,
  onDelete,
  onForumsChanged,
}: RosterDetailProps & { onDelete(forumId: string): Promise<void>; onForumsChanged?(): void }) {
  const load = useCallback((forumId: string) => client.getForum(forumId), [client]);
  const onLoaded = useCallback((detail: ForumDetail, forumId: string) => {
    dispatch({ type: 'forum-detail-loaded', forumId, writable: detail.writable });
  }, [dispatch]);
  const forum = state.bootstrap?.forums.find(({ id }) => id === state.currentForumId);
  return (
    <RosterDetailScreen
      key={state.currentForumId}
      ariaLabel="Forum detail navigation"
      backLabel="Sessions"
      fallbackTitle={forum?.display_name}
      copy={{
        absent: 'No forum is selected.',
        loading: 'Loading forum…',
        failed: 'Forum detail could not be loaded.',
      }}
      load={load}
      onLoaded={onLoaded}
      render={(detail, update) => <>
        <div className="cha-detail-actions">
          <EditableTitle
            available={detail.writable} id={detail.id} name={detail.display_name} subject="Forum"
            onSave={async (display_name) => {
              const saved = await client.updateForum(detail.id, { display_name });
              update(saved);
              dispatch({ type: 'forum-updated', forum: saved });
              onForumsChanged?.();
            }}
          />
          {detail.writable && <DetailActions
            name={detail.display_name} subject="Forum"
            deleteMessage={`Delete “${detail.display_name}”? This permanently removes the forum, all of its sessions, and its Notes folder and contents. This cannot be undone.`}
            onDelete={() => onDelete(detail.id)}
          />}
        </div>
        <FileList
          filenames={detail.markdown_files}
          references={detail.file_references}
          onSelectReference={reference => navigate({ type: 'inspect-forum-file-reference',
            forumId: detail.id, reference })}
          writable={detail.writable}
          onNew={() => navigate({ type: 'show-new-forum-file' })}
          onSelect={(filename) => navigate({ type: 'inspect-forum-file', forumId: state.currentForumId!, filename })}
        />
      </>}
      onBack={() => navigate({ type: 'show-sessions' })}
      subjectId={state.currentForumId}
      subtitle={forum && <ForumCast forum={forum} />}
      toolbarAction={state.inspectedForum.writable ? (
        <button
          className="cha-detail-link"
          onClick={() => navigate({ type: 'show-forum-members' })}
          type="button"
        >
          <span>Members</span>
          <ChevronRightIcon />
        </button>
      ) : undefined}
    />
  );
}

export function ForumMembersScreen({
  state,
  dispatch,
  navigate,
  client,
}: RosterDetailProps) {
  const forumId = state.currentForumId;
  const forum = state.bootstrap?.forums.find(({ id }) => id === forumId);
  const available = state.bootstrap?.characters ?? [];
  const personas = state.bootstrap?.personas ?? [];
  const memberKey = forum?.members.map(({ id }) => id).sort().join('\0') ?? '';
  const [selected, setSelected] = useState<Set<string>>(
    () => new Set(forum?.members.map(({ id }) => id)),
  );
  const [personaId, setPersonaId] = useState(forum?.default_persona_id ?? '');
  const [preview, setPreview] = useState<{ id: string; display_name: string } | null>(null);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [stale, setStale] = useState(false);
  const refresh = useDetailRefresh();
  const dirtyRef = useRef(false);
  const baseline = useRef<{ forumId: string | null; key: string; persona: string } | null>(null);

  const dirty = baseline.current !== null && baseline.current.forumId === forumId && (
    [...selected].sort().join('\0') !== baseline.current.key
    || personaId !== baseline.current.persona
  );
  dirtyRef.current = dirty;
  const current = {
    forumId: forumId ?? null,
    key: memberKey,
    persona: forum?.default_persona_id ?? '',
  };

  // Replaces the draft with the forum's current members and persona.
  function loadForum(loaded: ForumSummary | undefined) {
    baseline.current = {
      forumId: forumId ?? null,
      key: loaded?.members.map(({ id }) => id).sort().join('\0') ?? '',
      persona: loaded?.default_persona_id ?? '',
    };
    setSelected(new Set(loaded?.members.map(({ id }) => id)));
    setPersonaId(baseline.current.persona);
    setError(null);
    setStale(false);
  }

  useEffect(() => {
    const previous = baseline.current;
    if (previous
        && previous.forumId === current.forumId
        && previous.key === current.key
        && previous.persona === current.persona) {
      setStale(false);
      return;
    }
    if (previous && previous.forumId === current.forumId && dirtyRef.current) {
      setStale(true);
      return;
    }
    loadForum(forum);
  }, [forum, forumId, memberKey, refresh.epoch]);

  function toggle(characterId: string) {
    setSelected((current) => {
      const next = new Set(current);
      if (next.has(characterId)) next.delete(characterId);
      else next.add(characterId);
      return next;
    });
    setError(null);
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!forumId || !dirty || selected.size === 0 || !personaId || saving
        || stale || refresh.refreshing || refresh.failed) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateForumMembers(forumId, {
        character_ids: [...selected],
        persona_id: personaId,
      });
      loadForum(updated);
      dispatch({ type: 'forum-updated', forum: updated });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Forum members could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="Forum members navigation">
      <button
        className="cha-back-row"
        onClick={() => navigate({ type: 'show-forum-detail' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>{forum?.display_name ?? 'Forum'}</span>
      </button>
      {!forum && <p className="cha-state-message">No forum is selected.</p>}
      {forum && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <label htmlFor="cha-forum-members-persona">Persona
            <select
              className="cha-form-control"
              disabled={saving}
              id="cha-forum-members-persona"
              onChange={(event) => {
                setPersonaId(event.target.value);
                setError(null);
              }}
              value={personaId}
            >
              {personas.map((persona) => (
                <option key={persona.id} value={persona.id}>{persona.display_name}</option>
              ))}
            </select>
          </label>
          <div className="cha-member-list">
            {available.map((character) => (
              <div className="cha-member-row" key={character.id}>
                <label>
                  <input
                    checked={selected.has(character.id)}
                    disabled={saving}
                    onChange={() => toggle(character.id)}
                    type="checkbox"
                  />
                  <span>{character.display_name}</span>
                </label>
                <button
                  aria-label={`Show system prompt for ${character.display_name}`}
                  className="cha-member-prompt-button"
                  onClick={() => setPreview(character)}
                  title="Show system prompt"
                  type="button"
                >
                  <MoreIcon />
                </button>
              </div>
            ))}
          </div>
          {error && <p className="cha-error-message" role="alert">{error}</p>}
          {stale && <StaleNotice onReload={() => loadForum(forum)} />}
          {refresh.failed && (
            <div className="cha-state-message cha-error-message" role="alert">
              <p>Forum members could not be reloaded.</p>
              <button className="cha-button cha-button-ghost" onClick={refresh.retry} type="button">Try again</button>
            </div>
          )}
          <div className="cha-settings-form-actions">
            <button
              className="cha-button cha-button-primary"
              disabled={!dirty || selected.size === 0 || !personaId || saving || stale || refresh.refreshing || refresh.failed}
              type="submit"
            >
              Save
            </button>
          </div>
        </form>
      )}
      {forumId && preview && (
        <SystemPromptDialog
          characterId={preview.id}
          characterName={preview.display_name}
          client={client}
          forumId={forumId}
          key={`${forumId}:${preview.id}`}
          onClose={() => setPreview(null)}
        />
      )}
    </section>
  );
}
