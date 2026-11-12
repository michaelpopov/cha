import { useEffect, useRef, useState } from 'react';
import { publicErrorMessage, type NoteAudioClient, type NoteAudioVoice } from '../api/client';

function capitalize(value: string): string {
  return value.charAt(0).toUpperCase() + value.slice(1);
}

const excludedVoiceRole = /\b(?:tutor|training voiceover|tech advisor|digital assistant|concierge|consierge|commercial voiceover|call center agent|authoritative advisor)\b/i;

function isAvailableVoice(voice: NoteAudioVoice): boolean {
  const language = voice.language_code?.split('-')[0].toLowerCase();
  return (language === 'en' || language === 'ru')
    && !excludedVoiceRole.test(voice.display_name)
    && !excludedVoiceRole.test(voice.persona ?? '');
}

export function NoteAudioVoicePicker({ client, value, onChange, disabled }: {
  client: NoteAudioClient; value: string; onChange(value: string): void; disabled: boolean;
}) {
  const selector = useRef<HTMLSelectElement>(null);
  const [catalog, setCatalog] = useState<{ voices: NoteAudioVoice[] } | { error: string } | null>(null);

  useEffect(() => {
    let disposed = false;
    setCatalog(null);
    onChange('');
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
  }, [client, onChange]);

  const voices = catalog && 'voices' in catalog ? catalog.voices : [];
  const error = catalog && 'error' in catalog ? catalog.error : null;
  const unavailable = disabled || voices.length === 0;
  useEffect(() => { if (!unavailable) selector.current?.focus(); }, [unavailable]);

  const selected = voices.find(voice => voice.id === value);
  const language = selected?.language_code;
  const region = selected?.region_code;
  const traits = [
    selected?.pitch && `${capitalize(selected.pitch)} pitch`,
    region && !language?.toUpperCase().endsWith(`-${region.toUpperCase()}`) ? region : undefined,
  ].filter(Boolean).join(' · ');
  const persona = [selected?.persona, selected?.context].filter(Boolean).join(' · ');

  return <>
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
  </>;
}
