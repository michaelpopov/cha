import { useEffect, useRef, useState, type ReactNode } from 'react';
import { publicErrorMessage, type NoteAudioClient, type NoteAudioVoice } from '../api/client';
import { TextToSpeechError, TextToSpeechSession } from '../textToSpeech';

function capitalize(value: string): string {
  return value.charAt(0).toUpperCase() + value.slice(1);
}

const excludedVoiceRole = /\b(?:tutor|training voiceover|tech advisor|digital assistant|concierge|consierge|commercial voiceover|call center agent|authoritative advisor)\b/i;

const samples = {
  en: 'The chief task in life is simply this: to identify and separate matters so that I can say clearly to myself which are externals not under my control.',
  ru: 'Мороз и солнце, день чудесный, ещё ты дремлешь, друг прелестный.',
};

function sampleText(languageCode?: string): string | undefined {
  const language = languageCode?.split('-')[0].toLowerCase();
  return language === 'en' || language === 'ru' ? samples[language] : undefined;
}

function isAvailableVoice(voice: NoteAudioVoice): boolean {
  return !!sampleText(voice.language_code)
    && !excludedVoiceRole.test(voice.display_name)
    && !excludedVoiceRole.test(voice.persona ?? '');
}

export function NoteAudioVoicePicker({ client, value, onChange, disabled, hidePicker = false, children, actions }: {
  client: NoteAudioClient; value: string; onChange(value: string): void; disabled: boolean;
  hidePicker?: boolean; children?: ReactNode; actions: ReactNode;
}) {
  const selector = useRef<HTMLSelectElement>(null);
  const [catalog, setCatalog] = useState<{ voices: NoteAudioVoice[] } | { error: string } | null>(null);
  const preview = useRef<TextToSpeechSession | null>(null);
  const [previewing, setPreviewing] = useState(false);
  const [previewError, setPreviewError] = useState<string | null>(null);

  useEffect(() => {
    let disposed = false;
    setCatalog(null);
    onChange('');
    if (hidePicker) return;
    void (async () => {
      await Promise.resolve();
      if (disposed) return;
      try {
        const voices = await client.listVoices();
        if (!disposed) setCatalog({
          voices: voices.filter(isAvailableVoice).sort((a, b) => a.display_name.localeCompare(b.display_name)),
        });
      } catch (failure) {
        if (!disposed) setCatalog({ error: publicErrorMessage(failure, 'Could not load voices. Check the audio service in Notes settings.') });
      }
    })();
    return () => { disposed = true; };
  }, [client, onChange, hidePicker]);

  useEffect(() => {
    setPreviewing(false);
    setPreviewError(null);
    return () => {
      const session = preview.current;
      preview.current = null;
      session?.stop();
    };
  }, [client, value, disabled, hidePicker]);

  const voices = catalog && 'voices' in catalog ? catalog.voices : [];
  const error = catalog && 'error' in catalog ? catalog.error : null;
  const unavailable = disabled || voices.length === 0;
  useEffect(() => { if (!unavailable) selector.current?.focus(); }, [unavailable]);

  const selected = voices.find(voice => voice.id === value);
  const language = selected?.language_code;
  const sample = sampleText(language);
  const region = selected?.region_code;
  const traits = [
    selected?.pitch && `${capitalize(selected.pitch)} pitch`,
    region && !language?.toUpperCase().endsWith(`-${region.toUpperCase()}`) ? region : undefined,
  ].filter(Boolean).join(' · ');
  const persona = [selected?.persona, selected?.context].filter(Boolean).join(' · ');

  async function togglePreview() {
    if (preview.current) {
      preview.current.stop();
      preview.current = null;
      setPreviewing(false);
      return;
    }
    if (!selected || !sample || unavailable) return;
    setPreviewError(null);
    const session = new TextToSpeechSession({
      text: sample,
      nativeSpeech: {
        preview: (text, _voice, signal) => client.previewVoice(selected.id, text, signal),
        release: id => client.release(id).catch(() => {}),
      },
      onEnded: () => {
        if (preview.current !== session) return;
        preview.current = null;
        setPreviewing(false);
      },
      onError: failure => setPreviewError(failure.message),
    });
    preview.current = session;
    setPreviewing(true);
    try {
      await session.play();
    } catch (failure) {
      if (preview.current !== session) return;
      session.stop();
      preview.current = null;
      setPreviewing(false);
      setPreviewError(failure instanceof TextToSpeechError ? failure.message
        : publicErrorMessage(failure, 'Could not play the voice preview.'));
    }
  }

  return <>
    {!hidePicker && <>
      <label>Voice<select ref={selector} className="cha-form-control" value={value} disabled={unavailable}
        aria-busy={!catalog} onChange={event => onChange(event.target.value)}>
        <option value="">{!catalog ? 'Loading voices…' : error ? 'Voices unavailable'
          : voices.length ? 'Select voice' : 'No voices available'}</option>
        {voices.map(voice => <option key={voice.id} value={voice.id}>
          {[voice.display_name, voice.gender && capitalize(voice.gender), voice.language_code, voice.accent]
            .filter(Boolean).join(' · ')}
        </option>)}
      </select></label>
      <div className="cha-note-audio-details" aria-live="polite">
        {traits && <p className="cha-note-audio-traits">{traits}</p>}
        {persona && <p className="cha-note-audio-traits">{persona}</p>}
        {selected?.description && <p>{selected.description}</p>}
      </div>
      {error && <p className="cha-error-message" role="alert">{error}</p>}
      {previewError && <p className="cha-error-message" role="alert">{previewError}</p>}
    </>}
    {children}
    <div className="cha-dialog-actions">
      {!hidePicker && <button className="cha-button cha-note-audio-listen" type="button"
        disabled={unavailable || !sample} onClick={() => void togglePreview()}>{previewing ? 'Stop' : 'Listen'}</button>}
      {actions}
    </div>
  </>;
}
