import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type FormEvent,
} from 'react';
import { ChaError, publicErrorMessage, type ChaClient, type ForumLibrary, type ForumDetail, type ForumSummary } from '../api/client';
import { useLoad } from '../useLoad';
import { cloudStoreLabels } from './CloudStoreSettings';
import { LoadFailure } from './SettingsShared';
import { StaleNotice, useDetailRefresh, useFormReload } from '../detailRefresh';
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
      <div className="cha-detail-toolbar">
        <button
          className="cha-back-row"
          onClick={() => navigate({ type: 'show-forum-detail' })}
          type="button"
        >
          <ChevronLeftIcon />
          <span>{forum?.display_name ?? 'Forum'}</span>
        </button>
        <button className="cha-detail-link" onClick={() => navigate({ type: 'show-forum-library' })} type="button">
          <span>Library</span><ChevronRightIcon />
        </button>
      </div>
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

const loadLibraryKeys = (client: ChaClient) => client.listApiKeys();
const librarySignature = (library: ForumLibrary | null) => JSON.stringify(library && {
  api_key_id: library.api_key_id, store_ids: [...library.store_ids].sort(), provider_id: library.provider_id,
});

export function ForumLibraryScreen(props: RosterDetailProps) {
  const { client, state, navigate } = props;
  const load = useCallback((id: string) => client.getForum(id), [client]);
  return <RosterDetailScreen
    {...props}
    key={state.currentForumId}
    subjectId={state.currentForumId}
    ariaLabel="Forum library navigation"
    backLabel="Members"
    onBack={() => navigate({ type: 'show-forum-members' })}
    copy={{ absent: 'No forum is selected.', loading: 'Loading library…', failed: 'Library could not be loaded.' }}
    load={load}
    render={(detail, update) => <ForumLibraryForm {...props} detail={detail} onSaved={update} />}
  />;
}

function ForumLibraryForm({ client, dispatch, detail, onSaved }: RosterDetailProps & {
  detail: ForumDetail; onSaved(detail: ForumDetail): void;
}) {
  const [draft, setDraft] = useState<ForumLibrary | null>(detail.library);
  const [baseline, setBaseline] = useState(detail.library);
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const reload = useFormReload(detail.id);
  const refresh = useDetailRefresh();
  const dirty = librarySignature(draft) !== librarySignature(baseline);
  reload.markDirty(dirty);
  function apply(value: ForumDetail) {
    setDraft(value.library);
    setBaseline(value.library);
    setError(null);
  }
  useEffect(() => { reload.loaded(detail, apply); }, [detail]);
  const keys = useLoad(client, loadLibraryKeys, 'API keys could not be loaded.');
  const eligible = keys.data?.filter((key) => key.has_value && ['openai', 'google'].includes(key.cloud_store)) ?? [];
  const keyId = draft?.api_key_id ?? '';
  const key = eligible.find(({ id }) => id === keyId);
  // An unavailable saved key has no stores or providers to list.
  const listedKeyId = key?.id ?? '';
  const loadStores = useCallback(async (loader: ChaClient) => ({
    keyId: listedKeyId, items: listedKeyId ? (await loader.listCloudStores(listedKeyId)).items : [],
  }), [listedKeyId]);
  const loadProviders = useCallback(async (loader: ChaClient) => ({
    keyId: listedKeyId, items: listedKeyId ? await loader.listProviders(listedKeyId) : [],
  }), [listedKeyId]);
  const storesLoad = useLoad(client, loadStores, 'Cloud stores could not be loaded.');
  const providersLoad = useLoad(client, loadProviders, 'Providers could not be loaded.');
  const stores = storesLoad.data?.keyId === keyId ? storesLoad.data.items : null;
  const providers = providersLoad.data?.keyId === keyId ? providersLoad.data.items : null;
  const labels = stores ? cloudStoreLabels(stores) : [];
  const missingStores = stores ? draft?.store_ids.filter((id) => !stores.some((store) => store.id === id)) ?? [] : [];
  const unavailableProvider = draft?.provider_id && providers && !providers.some(({ id }) => id === draft.provider_id);
  const valid = draft === null || (!!key && !!stores && !storesLoad.error
    && draft.store_ids.length > 0 && missingStores.length === 0
    && new Set(draft.store_ids).size === draft.store_ids.length
    && (key.cloud_store !== 'openai' || draft.store_ids.length <= 2)
    && (draft.provider_id === null || (!!providers && !providersLoad.error && !unavailableProvider)));
  const blocked = saving || !detail.writable || reload.blocked || refresh.refreshing || refresh.failed;
  const missingCatalogue = draft && draft.store_ids.some((id) => baseline?.api_key_id !== draft.api_key_id
    || !Object.hasOwn(baseline.catalogue, id));
  const canSave = (dirty || missingCatalogue) && valid && !blocked;
  async function save(event: FormEvent) {
    event.preventDefault();
    if (!canSave) return;
    setSaving(true);
    setError(null);
    try {
      const epoch = client.contextEpoch();
      const requireContext = () => {
        if (client.contextEpoch() !== epoch)
          throw new ChaError('vault_changed', 'The vault changed. Start again in the current vault.');
      };
      const forumId = detail.id;
      const catalogue: ForumLibrary['catalogue'] = Object.create(null);
      const failed: string[] = [];
      if (draft) {
        for (const id of draft.store_ids) {
          if (baseline?.api_key_id === draft.api_key_id && Object.hasOwn(baseline.catalogue, id)) {
            catalogue[id] = baseline.catalogue[id];
            continue;
          }
          requireContext();
          try {
            const store = await client.getCloudStore(draft.api_key_id, id);
            catalogue[id] = { name: store.name, description: store.description };
          } catch {
            failed.push(labels[stores?.findIndex((store) => store.id === id) ?? -1] || id);
          }
        }
      }
      requireContext();
      const saved = await client.updateForumLibrary(forumId, draft && { ...draft, catalogue });
      reload.remember(saved);
      apply(saved);
      onSaved(saved);
      dispatch({ type: 'forum-updated', forum: saved });
      if (failed.length) setError(`Library saved. Could not read ${failed.join(', ')}.`);
    } catch (failure) {
      setError(publicErrorMessage(failure, 'Library could not be saved.'));
    } finally { setSaving(false); }
  }
  function toggle(id: string) {
    setDraft((current) => current && ({ ...current, store_ids: current.store_ids.includes(id)
      ? current.store_ids.filter((value) => value !== id) : [...current.store_ids, id] }));
    setError(null);
  }
  return <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
    <label>API key
      <select className="cha-form-control" disabled={saving || !detail.writable} value={keyId}
        onChange={(event) => {
          setDraft(event.target.value ? { api_key_id: event.target.value, store_ids: [], provider_id: null, catalogue: {} } : null);
          setError(null);
        }}>
        <option value="">None</option>
        {keyId && !key && <option value={keyId}>{keys.data ? `${keyId} — Unavailable` : keyId}</option>}
        {eligible.map((key) => <option key={key.id} value={key.id}>{key.display_name}</option>)}
      </select>
    </label>
    {keys.error && <LoadFailure message={keys.error} retry={keys.retry} />}
    {keys.data && eligible.length === 0 && <p className="cha-state-message">No cloud store keys</p>}
    {draft && <>
      {keys.data && !key && <p className="cha-error-message" role="alert">Select an available API key or None to clear the Library.</p>}
      {key && !stores && !storesLoad.error && <p className="cha-state-message" role="status">Loading cloud stores…</p>}
      {key && storesLoad.error && <LoadFailure message={storesLoad.error} retry={storesLoad.retry} />}
      {stores && <>
        <div role="group" aria-label="Cloud stores">
          <label>Cloud stores</label>
          {stores.length === 0 && <p className="cha-state-message">No cloud stores</p>}
          <div className="cha-member-list">
            {[...stores.map((store, index) => ({ id: store.id, label: labels[index] })),
              ...missingStores.map((id) => ({ id, label: `${id} — Unavailable` }))].map((store) => (
              <div className="cha-member-row" key={store.id}><label>
                <input type="checkbox" checked={draft.store_ids.includes(store.id)}
                  disabled={saving || !detail.writable || (key?.cloud_store === 'openai' && draft.store_ids.length >= 2 && !draft.store_ids.includes(store.id))}
                  onChange={() => toggle(store.id)} />
                <span>{store.label}</span>
              </label></div>
            ))}
          </div>
        </div>
        <label>Default provider
          <select className="cha-form-control" disabled={saving || !detail.writable} value={draft.provider_id ?? ''}
            onChange={(event) => { setDraft({ ...draft, provider_id: event.target.value || null }); setError(null); }}>
            <option value="">None</option>
            {draft.provider_id && !providers?.some(({ id }) => id === draft.provider_id)
              && <option value={draft.provider_id}>{providers ? `${draft.provider_id} — Unavailable` : draft.provider_id}</option>}
            {providers?.map((provider) => <option key={provider.id} value={provider.id}>{provider.display_name}</option>)}
          </select>
        </label>
        {unavailableProvider && <p className="cha-error-message" role="alert">Select an available provider or None.</p>}
        {providers && providers.length === 0 && <p className="cha-state-message">No providers for this API key</p>}
        {providersLoad.error && <LoadFailure message={providersLoad.error} retry={providersLoad.retry} />}
      </>}
    </>}
    {error && <p className="cha-error-message" role="alert">{error}</p>}
    {reload.stale && <StaleNotice onReload={() => reload.accept(apply)} />}
    <div className="cha-settings-form-actions">
      <button className="cha-button cha-button-primary" disabled={!canSave} type="submit">{saving ? 'Saving…' : 'Save'}</button>
    </div>
  </form>;
}
