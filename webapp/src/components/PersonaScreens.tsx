import {
  useCallback,
  useEffect,
  useState,
  type FormEvent,
} from 'react';
import { publicErrorMessage, type PersonaDetail } from '../api/client';
import { StaleNotice, useFormReload } from '../detailRefresh';
import { DetailActions, EditableTitle } from './DetailActions';
import { ChevronLeftIcon, ChevronRightIcon, PlusIcon } from './Icons';
import { BackToSettings } from './SettingsShared';
import { TransliteratingInput } from './TransliterationMode';
import {
  RosterDetailScreen,
  rosterMarkdown,
  RosterRow,
  unresolvedOption,
  type NavigationScreenProps,
  type RosterDetailProps,
} from './RosterScreens';

export function PersonasScreen({ state, navigate }: NavigationScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Personas navigation">
      <BackToSettings navigate={navigate} />
      <div className="cha-roster">
        <button
          className="cha-list-action"
          onClick={() => navigate({ type: 'show-new-persona' })}
          type="button"
        >
          <span className="cha-list-icon"><PlusIcon /></span>
          <span className="cha-list-copy">
            <span className="cha-primary-line">New persona</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
        {state.bootstrap?.personas.map((persona) => (
          <RosterRow
            description={persona.description}
            displayName={persona.display_name}
            key={persona.id}
            onSelect={() => navigate({ type: 'inspect-persona', personaId: persona.id })}
          />
        ))}
      </div>
    </section>
  );
}

export function NewPersonaScreen({
  navigate,
  client,
}: RosterDetailProps) {
  const [name, setName] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const trimmedName = name.trim();

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!trimmedName || saving) return;
    setSaving(true);
    setError(null);
    try {
      const persona = await client.createPersona({ display_name: trimmedName });
      navigate({ type: 'persona-created', persona });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The persona could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New persona navigation">
      <button
        className="cha-back-row"
        onClick={() => navigate({ type: 'show-personas' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Personas</span>
      </button>
      <form className="cha-settings-form" onSubmit={(event) => void submit(event)}>
        <TransliteratingInput
          autoComplete="off"
          autoFocus
          className="cha-form-control"
          disabled={saving}
          id="cha-persona-name"
          label="Persona name"
          onValueChange={setName}
          placeholder="e.g. Project manager"
          type="text"
          value={name}
        />
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={saving}
            onClick={() => navigate({ type: 'show-personas' })}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!trimmedName || saving}
            type="submit"
          >
            Create persona
          </button>
        </div>
      </form>
    </section>
  );
}

export function PersonaDetailScreen({ state, dispatch, navigate, client }: RosterDetailProps) {
  const id = state.inspectedPersona.id;
  const load = useCallback((id: string) => client.getPersona(id), [client]);
  const onLoaded = useCallback((detail: PersonaDetail, personaId: string) => {
    dispatch({ type: 'persona-detail-loaded', personaId, writable: detail.writable });
  }, [dispatch]);

  return (
    <RosterDetailScreen
      key={id}
      ariaLabel="Persona detail navigation"
      backLabel="Personas"
      fallbackTitle={state.bootstrap?.personas.find((persona) => persona.id === id)?.display_name}
      copy={{
        absent: 'No persona is selected.',
        loading: 'Loading persona…',
        failed: 'Persona detail could not be loaded.',
      }}
      load={load}
      onLoaded={onLoaded}
      onBack={() => navigate({ type: 'show-personas' })}
      subjectId={id}
      toolbarAction={state.inspectedPersona.writable ? (
        <button className="cha-detail-link" onClick={() => navigate({ type: 'show-persona-settings' })} type="button">
          <span>Settings</span><ChevronRightIcon />
        </button>
      ) : undefined}
      render={(detail, update) => {
        async function saveMarkdown(persona_markdown: string) {
          const saved = await client.updatePersona(detail.id, { persona_markdown });
          update(saved);
          dispatch({ type: 'persona-updated', persona: saved });
        }
        return <>
          <div className="cha-detail-actions">
            <EditableTitle
              available={detail.writable} id={detail.id} name={detail.display_name} subject="Persona"
              onSave={async (display_name) => {
                const saved = await client.updatePersona(detail.id, { display_name });
                update(saved);
                dispatch({ type: 'persona-updated', persona: saved });
              }}
            />
            {detail.writable && <DetailActions
              name={detail.display_name} subject="Persona"
              deleteMessage={`Delete “${detail.display_name}”? This permanently removes its profile. This cannot be undone.`}
              onDelete={async () => {
                await client.deletePersona(detail.id);
                navigate({ type: 'persona-deleted', personaId: detail.id });
              }}
              editor={{ title: 'Edit persona profile', value: detail.persona_markdown,
                uploadLabel: 'Replace persona description from file', onSave: saveMarkdown }}
            />}
          </div>
          {rosterMarkdown(detail.persona_markdown, 'This persona has no PERSONA.md description.')}
        </>;
      }}
    />
  );
}

export function PersonaSettingsScreen({
  state,
  dispatch,
  navigate,
  client,
}: RosterDetailProps) {
  const personaId = state.inspectedPersona.id;
  const persona = state.bootstrap?.personas.find(({ id }) => id === personaId);
  const [detail, setDetail] = useState<PersonaDetail | null>(null);
  const [style, setStyle] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const reload = useFormReload(personaId || null);

  function applyPersona(loaded: PersonaDetail) {
    setDetail(loaded);
    setStyle(loaded.style);
  }

  useEffect(() => {
    if (!personaId) return;
    let current = true;
    const background = reload.start();
    if (!background) {
      setDetail(null);
      setError(null);
    }
    void client.getPersona(personaId).then(
      (loaded) => {
        if (!current) return;
        reload.loaded(loaded, applyPersona);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'Persona settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => {
      current = false;
    };
  }, [client, personaId, reload.epoch, reload.attempt]);

  function closeSettings() {
    if (personaId) navigate({ type: 'inspect-persona', personaId });
    else navigate({ type: 'show-personas' });
  }

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!personaId || !detail || saving) return;
    if (style === detail.style) return;
    setSaving(true);
    setError(null);
    try {
      const saved = await client.updatePersona(personaId, {
        style,
      });
      dispatch({ type: 'persona-updated', persona: saved });
      applyPersona(saved);
      reload.remember(saved);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Persona settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }

  const unresolvedStyle = detail && unresolvedOption(detail.available_styles, detail.style);
  const dirty = detail !== null && style !== detail.style;
  reload.markDirty(dirty);

  return (
    <section className="cha-screen cha-navigation" aria-label="Persona settings">
      <button className="cha-back-row" onClick={closeSettings} type="button">
        <ChevronLeftIcon />
        <span>{persona?.display_name ?? 'Persona'}</span>
      </button>
      {!personaId && <p className="cha-state-message">No persona is selected.</p>}
      {personaId && detail === null && !error && (
        <p className="cha-state-message" role="status">Loading persona settings…</p>
      )}
      {error && (
        <div className="cha-state-message cha-error-message" role="alert">
          <p>{error}</p>
          {(detail === null || reload.refreshFailed) && (
            <button
              className="cha-button cha-button-ghost"
              onClick={reload.retry}
              type="button"
            >
              Try again
            </button>
          )}
        </div>
      )}
      {detail && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <label htmlFor="cha-persona-style">Style
            <select
              className="cha-form-control"
              disabled={saving}
              id="cha-persona-style"
              onChange={(event) => setStyle(event.target.value === '' ? null : event.target.value)}
              value={style ?? ''}
            >
              <option value="">No style</option>
              {detail.available_styles.map((option) => (
                <option key={option.id} value={option.id}>{option.label}</option>
              ))}
              {unresolvedStyle && (
                <option value={unresolvedStyle.id}>{unresolvedStyle.label}</option>
              )}
            </select>
          </label>
          {reload.stale && <StaleNotice onReload={() => reload.accept(applyPersona)} />}
          <div className="cha-settings-form-actions">
            <button
              className="cha-button cha-button-ghost"
              onClick={closeSettings}
              type="button"
            >
              Cancel
            </button>
            <button
              className="cha-button cha-button-primary"
              disabled={!dirty || saving || reload.blocked}
              type="submit"
            >
              Save
            </button>
          </div>
        </form>
      )}
    </section>
  );
}
