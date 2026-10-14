import { useEffect, useState, type FormEvent } from 'react';
import {
  publicErrorMessage,
  type ChaClient,
  type StyleDetail,
  type StyleUpdate,
} from '../api/client';
import { DetailRefreshProvider, StaleNotice, useFormReload } from '../detailRefresh';
import { useLoad } from '../useLoad';
import { voiceClasses } from './characterAppearance';
import { ConfirmDialog } from './ConfirmDialog';
import { EditableTitle } from './DetailActions';
import { CharacterIcon, ChevronLeftIcon, PlusIcon } from './Icons';
import { TransliteratingInput } from './TransliterationMode';
import {
  BackToSettings,
  LoadFailure,
  SettingsRow,
  UsedBy,
  type SettingsScreenProps,
} from './SettingsShared';

const loadStyles = (client: ChaClient) => client.listStyles();

export function StylesScreen({ client, navigate }: SettingsScreenProps) {
  const { data: styles, error, retry } = useLoad(
    client, loadStyles, 'Styles could not be loaded.',
  );
  return (
    <section className="cha-screen cha-navigation" aria-label="Styles settings">
      <BackToSettings navigate={navigate} />
      {styles === null && !error && <p className="cha-state-message" role="status">Loading styles…</p>}
      {error && <LoadFailure message={error} retry={retry} />}
      {styles && <div className="cha-list"><SettingsRow description="Start with a neutral character style" icon={<PlusIcon />} label="New style" onClick={() => navigate({ type: 'show-settings-new-style' })} />{styles.length === 0 && <p className="cha-empty-list">No styles configured</p>}{styles.map((style) => (
        <SettingsRow description={`${style.font} · ${style.weight} · ${style.size}`} icon={<CharacterIcon />} key={style.id} label={style.display_name} onClick={() => navigate({ type: 'inspect-style', styleId: style.id, styleName: style.display_name })} />
      ))}</div>}
    </section>
  );
}

export function NewStyleScreen({ client, navigate }: SettingsScreenProps) {
  const [name, setName] = useState('');
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState<string | null>(null);

  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!name.trim() || saving) return;
    setSaving(true);
    setError(null);
    try {
      const created = await client.createStyle({ display_name: name.trim() });
      navigate({ type: 'inspect-style', styleId: created.id, styleName: created.display_name });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The style could not be created.'));
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New style settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-styles' })} type="button"><ChevronLeftIcon /><span>Styles</span></button>
      <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
        <fieldset disabled={saving}><TransliteratingInput autoFocus className="cha-form-control" id="cha-new-style-name" label="Name" onValueChange={setName} placeholder="e.g. Editorial" value={name} /></fieldset>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={saving} onClick={() => navigate({ type: 'show-settings-styles' })} type="button">Cancel</button><button className="cha-button cha-button-primary" disabled={!name.trim() || saving} type="submit">{saving ? 'Creating…' : 'Create style'}</button></div>
      </form>
    </section>
  );
}

function styleUpdate(detail: StyleDetail): StyleUpdate {
  const { id: _id, used_by: _usedBy, writable: _writable, ...update } = detail;
  return update;
}

export function StyleScreen({
  client,
  dispatch,
  navigate,
  state,
}: SettingsScreenProps) {
  const id = state.inspectedStyle.id;
  const [detail, setDetail] = useState<StyleDetail | null>(null);
  const [draft, setDraft] = useState<StyleUpdate | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [saving, setSaving] = useState(false);
  const [deleting, setDeleting] = useState(false);
  const [confirming, setConfirming] = useState(false);
  const reload = useFormReload(id);

  function applyStyle(loaded: StyleDetail) {
    setDetail(loaded);
    setDraft(styleUpdate(loaded));
    dispatch({
      type: 'style-detail-loaded',
      styleId: loaded.id,
      styleName: loaded.display_name,
      writable: loaded.writable,
    });
  }

  useEffect(() => {
    let current = true;
    const background = reload.start();
    if (!background) {
      setDetail(null);
      setDraft(null);
      setError(null);
    }
    if (!id) return () => { current = false; };
    void client.listStyles().then(
      (styles) => {
        if (!current) return;
        const loaded = styles.find((style) => style.id === id);
        if (!loaded) {
          setError('That style was not found.');
          reload.fail(background);
          return;
        }
        setError(null);
        reload.loaded(loaded, applyStyle);
      },
      (failure: unknown) => {
        if (!current) return;
        setError(publicErrorMessage(failure, 'Style settings could not be loaded.'));
        reload.fail(background);
      },
    );
    return () => { current = false; };
  }, [client, dispatch, id, reload.epoch, reload.attempt]);

  function change<Key extends keyof StyleUpdate>(key: Key, value: StyleUpdate[Key]) {
    setDraft((current) => current ? { ...current, [key]: value } : current);
  }
  async function save(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!id || !draft || saving || deleting) return;
    setSaving(true);
    setError(null);
    try {
      const updated = await client.updateStyle(id, draft);
      setDetail(updated);
      setDraft(styleUpdate(updated));
      reload.remember(updated);
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'Style settings could not be saved.'));
    } finally {
      setSaving(false);
    }
  }
  async function remove() {
    setConfirming(false);
    if (!id || saving || deleting) return;
    setDeleting(true);
    setError(null);
    try {
      await client.deleteStyle(id);
      navigate({ type: 'show-settings-styles' });
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, 'The style could not be deleted.'));
      setDeleting(false);
    }
  }
  const dirty = Boolean(detail && draft && JSON.stringify(styleUpdate(detail)) !== JSON.stringify(draft));
  reload.markDirty(dirty);
  function resetStyle() {
    if (reload.stale) {
      reload.accept(applyStyle);
      return;
    }
    if (detail) setDraft(styleUpdate(detail));
  }
  const appearance = draft && {
    font: draft.font,
    style: draft.style,
    weight: draft.weight,
    size: draft.size,
    text_color: draft.text_color,
  };
  return (
    <section className="cha-screen cha-navigation" aria-label="Style settings">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-settings-styles' })} type="button"><ChevronLeftIcon /><span>Styles</span></button>
      {(detail || state.inspectedStyle.name) && <div className="cha-detail-actions">
        {detail ? <DetailRefreshProvider value={reload.context}><EditableTitle
          available={detail.writable} disabled={saving || deleting}
          id={detail.id} name={detail.display_name} subject="Style"
          onSave={async (display_name) => {
            setSaving(true);
            try {
              const saved = await client.updateStyle(detail.id, { ...styleUpdate(detail), display_name });
              setDetail(saved);
              setDraft((current) => current && ({ ...current, display_name: saved.display_name }));
              reload.remember(saved);
              dispatch({ type: 'style-updated', styleId: saved.id,
                styleName: saved.display_name, writable: saved.writable });
            } finally {
              setSaving(false);
            }
          }}
        /></DetailRefreshProvider> : <h1>{state.inspectedStyle.name}</h1>}
      </div>}
      {!id && <p className="cha-state-message">No style is selected.</p>}
      {id && !detail && !error && <p className="cha-state-message" role="status">Loading style…</p>}
      {error && (!detail || reload.refreshFailed) && <LoadFailure message={error} retry={reload.retry} />}
      {detail && draft && (
        <form className="cha-settings-form" onSubmit={(event) => void save(event)}>
          <fieldset aria-label="Style settings" disabled={saving || deleting || !detail.writable}>
            <div className="cha-settings-form-grid">
              <label>Font<select className="cha-form-control" onChange={(event) => change('font', event.target.value as StyleUpdate['font'])} value={draft.font}><option value="sans">Sans</option><option value="serif">Serif</option><option value="mono">Mono</option></select></label>
              <label>Slant<select className="cha-form-control" onChange={(event) => change('style', event.target.value as StyleUpdate['style'])} value={draft.style}><option value="normal">Normal</option><option value="italic">Italic</option></select></label>
              <label>Weight<select className="cha-form-control" onChange={(event) => change('weight', event.target.value as StyleUpdate['weight'])} value={draft.weight}><option value="light">Light</option><option value="normal">Normal</option><option value="medium">Medium</option><option value="semibold">Semibold</option><option value="bold">Bold</option></select></label>
              <label>Size<select className="cha-form-control" onChange={(event) => change('size', event.target.value as StyleUpdate['size'])} value={draft.size}><option value="small">Small</option><option value="normal">Normal</option><option value="large">Large</option></select></label>
              <label>Text color<select className="cha-form-control" onChange={(event) => change('text_color', event.target.value as StyleUpdate['text_color'])} value={draft.text_color}><option value="normal">Normal</option><option value="muted">Muted</option><option value="accent">Accent</option></select></label>
            </div>
          </fieldset>
          <p className={`cha-style-sample cha-message-text${appearance ? voiceClasses(appearance) : ''}`}>The chief task in life is this…</p>
          <UsedBy empty="Nothing uses this style." items={detail.used_by} />
          {error && !reload.refreshFailed && <p className="cha-error-message" role="alert">{error}</p>}
          {reload.stale && <StaleNotice onReload={() => reload.accept(applyStyle)} />}
          <div className="cha-settings-form-actions"><button className="cha-button cha-button-ghost" disabled={(!dirty && !reload.stale) || saving || deleting} onClick={resetStyle} type="button">Reset</button><button className="cha-button cha-button-primary" disabled={!dirty || saving || deleting || !detail.writable || reload.blocked} type="submit">{saving ? 'Saving…' : 'Save style'}</button></div>
          <div className="cha-settings-form-actions">
            <button className="cha-button cha-button-danger" disabled={saving || deleting || !detail.writable} onClick={() => setConfirming(true)} type="button">{deleting ? 'Deleting…' : 'Delete style'}</button>
          </div>
        </form>
      )}
      {confirming && (
        <ConfirmDialog
          confirmLabel="Delete style"
          message={`Delete “${detail?.display_name ?? 'this style'}”? This cannot be undone.`}
          onCancel={() => setConfirming(false)}
          onConfirm={() => void remove()}
          title="Delete style?"
        />
      )}
    </section>
  );
}
