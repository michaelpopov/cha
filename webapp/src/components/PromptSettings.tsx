import { useEffect, useRef, useState, type ChangeEvent } from 'react';

import { publicErrorMessage, type ChaClient, type PromptDetail } from '../api/client';
import { useFormReload } from '../detailRefresh';
import { readTextFile, validateTextFile } from '../textFile';
import { useLoad } from '../useLoad';
import { ChevronLeftIcon, EditIcon, FileUpIcon, TextLinesIcon } from './Icons';
import { BackToSettings, LoadFailure, SettingsRow, type SettingsScreenProps } from './SettingsShared';
import { TextEditorDialog } from './TextEditorDialog';

const loadPrompts = (client: ChaClient) => client.listPrompts();
const promptTooLarge = 'Prompt text is too large. The limit is 256 KiB.';

export function PromptsScreen({ client, navigate }: Pick<SettingsScreenProps, 'client' | 'navigate'>) {
  const { data: prompts, error, retry } = useLoad(client, loadPrompts, 'Prompts could not be loaded.');
  return (
    <section className="cha-screen cha-navigation" aria-label="Prompts">
      <BackToSettings navigate={navigate} />
      {error && <LoadFailure message={error} retry={retry} />}
      {prompts === null && !error && <p className="cha-state-message" role="status">Loading prompts…</p>}
      {prompts && <div className="cha-list">
        {prompts.map((prompt) => (
          <SettingsRow key={prompt.id} icon={<TextLinesIcon />} label={prompt.display_name}
            onClick={() => navigate({
              type: 'inspect-prompt', promptId: prompt.id, promptName: prompt.display_name,
            })} />
        ))}
      </div>}
    </section>
  );
}

export function PromptScreen({ client, navigate, state }: Pick<SettingsScreenProps, 'client' | 'navigate' | 'state'>) {
  const { id, name } = state.inspectedPrompt;
  const reload = useFormReload(id);
  const [detail, setDetail] = useState<PromptDetail | null>(null);
  const [draft, setDraft] = useState<string | null>(null);
  const [loadError, setLoadError] = useState<string | null>(null);
  const [editorError, setEditorError] = useState<string | null>(null);
  const [uploadError, setUploadError] = useState<string | null>(null);
  const [uploading, setUploading] = useState(false);
  const [saving, setSaving] = useState(false);
  const saveInProgress = useRef(false);
  const saved = useRef(0);
  const loadedEpoch = useRef<number | null>(null);
  const fileInput = useRef<HTMLInputElement>(null);

  function apply(loaded: PromptDetail) {
    setDetail(loaded);
    setDraft((current) => current === null ? null : loaded.text);
  }

  useEffect(() => {
    if (!id) return;
    let current = true;
    const version = saved.current;
    const background = reload.start();
    void (async () => {
      try {
        const epoch = client.contextEpoch();
        const loaded = await client.getPrompt(id);
        if (!current || saved.current !== version || client.contextEpoch() !== epoch) return;
        loadedEpoch.current = epoch;
        setLoadError(null);
        reload.loaded(loaded, apply);
      } catch (failure: unknown) {
        if (!current || saved.current !== version) return;
        setLoadError(publicErrorMessage(failure, 'The prompt could not be loaded.'));
        reload.fail(background);
      }
    })();
    return () => { current = false; };
  }, [client, id, reload.epoch, reload.attempt]);

  function change(value: string) {
    reload.markDirty(value !== detail?.text);
    setDraft(value);
    setEditorError(null);
  }

  function cancel() {
    if (saveInProgress.current) return;
    setDraft(null);
    setEditorError(null);
    reload.markDirty(false);
    if (reload.stale) reload.accept(apply);
  }

  async function upload(event: ChangeEvent<HTMLInputElement>) {
    const input = event.currentTarget;
    const file = input.files?.[0];
    input.value = '';
    if (!file || !detail?.writable || uploading || reload.blocked) return;
    setUploading(true);
    setUploadError(null);
    try {
      if (!/\.(txt|md)$/i.test(file.name)) throw new Error('Choose a .txt or .md file.');
      const text = await readTextFile(file, promptTooLarge, { allowEmpty: true, preserveBOM: true });
      // Match the textarea's line endings before editing.
      change(text.replace(/\r\n?/g, '\n'));
    } catch (failure: unknown) {
      setUploadError(failure instanceof Error ? failure.message : 'The file could not be read. Try again.');
    } finally {
      setUploading(false);
    }
  }

  async function save() {
    if (!id || !detail?.writable || draft === null || saveInProgress.current || reload.blocked) return;
    try {
      validateTextFile(draft, promptTooLarge, true);
    } catch (failure: unknown) {
      if (failure instanceof Error) setEditorError(failure.message);
      return;
    }
    saveInProgress.current = true;
    setSaving(true);
    setEditorError(null);
    try {
      const epoch = client.contextEpoch();
      if (epoch !== loadedEpoch.current) return;
      const updated = await client.updatePrompt(id, { text: draft });
      if (client.contextEpoch() !== epoch) return;
      saved.current += 1;
      setDetail(updated);
      setDraft(null);
      setLoadError(null);
      reload.markDirty(false);
      reload.remember(updated);
    } catch (failure: unknown) {
      setEditorError(publicErrorMessage(failure, 'The prompt could not be saved. Try again.'));
    } finally {
      saveInProgress.current = false;
      setSaving(false);
    }
  }

  return (
    <section className="cha-screen cha-navigation" aria-label={name ?? 'Prompt'}>
      <div className="cha-detail-toolbar">
        <button className="cha-back-row" type="button" onClick={() => navigate({ type: 'show-settings-prompts' })}>
          <ChevronLeftIcon /><span>Prompts</span>
        </button>
        {detail?.writable && <div className="cha-detail-action-buttons">
          <button aria-label="Edit text" title="Edit text" className="cha-title-icon-action"
            disabled={reload.blocked || uploading || draft !== null}
            onClick={() => { setUploadError(null); change(detail.text); }} type="button">
            <EditIcon />
          </button>
          <button aria-label="Upload text" title="Upload text" className="cha-title-icon-action"
            disabled={reload.blocked || uploading || draft !== null}
            onClick={() => fileInput.current?.click()} type="button">
            <FileUpIcon />
          </button>
          <input ref={fileInput} type="file" accept=".txt,.md" hidden onChange={(event) => void upload(event)} />
        </div>}
      </div>
      {loadError && <LoadFailure message={loadError} retry={reload.retry} />}
      {uploadError && <p className="cha-error-message" role="alert">{uploadError}</p>}
      {!detail && !loadError && <p className="cha-state-message" role="status">Loading prompt…</p>}
      {detail && <div className="cha-prompt-text">{detail.text}</div>}
      {draft !== null && <TextEditorDialog
        error={reload.stale ? 'This prompt changed. Cancel and reopen the editor to load the latest text.'
          : reload.refreshFailed ? 'The prompt could not be refreshed. Cancel and try again.' : editorError}
        loading={false}
        onCancel={cancel}
        onChange={change}
        onSave={() => void save()}
        placeholder={null}
        ready
        saveDisabled={reload.blocked || !detail?.writable}
        saving={saving}
        textareaLabel="Prompt text"
        title="Edit text"
        value={draft}
      />}
    </section>
  );
}
