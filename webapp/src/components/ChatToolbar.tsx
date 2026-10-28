import type { CharacterSettingsPatch, Effort, SessionSnapshot } from '../api/client';

// Derive the active individual from the same snapshot used to render the chat.
export function toolbarCharacterId(snapshot: SessionSnapshot): string | undefined {
  const member = (id: string | undefined) => snapshot.characters.some((item) => item.id === id);
  if (snapshot.generation.active && member(snapshot.generation.character_id)) {
    return snapshot.generation.character_id;
  }
  if (member(snapshot.default_character_id)) return snapshot.default_character_id;
  for (let i = snapshot.transcript.length - 1; i >= 0; --i) {
    const entry = snapshot.transcript[i];
    const id = entry.kind === 'human' ? entry.addressed_to
      : entry.kind === 'character' || entry.kind === 'error' ? entry.participant_id : undefined;
    if (member(id)) return id;
  }
  if (member(snapshot.forum.default_character_id)) return snapshot.forum.default_character_id;
  return snapshot.characters[0]?.id;
}

const efforts: [Effort, string][] = [
  ['none', 'None'], ['low', 'Low'], ['medium', 'Medium'], ['high', 'High'], ['xhigh', 'Extra high'],
];

export function ChatToolbar({ snapshot, disabled, onCharacter, onSettings }: {
  snapshot: SessionSnapshot;
  disabled: boolean;
  onCharacter(id: string): void;
  onSettings(id: string, patch: CharacterSettingsPatch): void;
}) {
  const characterId = toolbarCharacterId(snapshot);
  const settings = snapshot.chat_settings.characters.find((item) => item.character_id === characterId);
  if (!settings || !characterId) return null;
  return (
    <div className="cha-chat-toolbar">
      <select aria-label="Character" disabled={disabled} value={characterId}
        onChange={(event) => onCharacter(event.target.value)}>
        {snapshot.characters.map((item) => (
          <option key={item.id} value={item.id}>{item.display_name}</option>
        ))}
      </select>
      <select aria-label="Model" disabled={disabled} value={settings.provider_id}
        onChange={(event) => onSettings(characterId, { provider_id: event.target.value || null })}>
        <option value="">Default</option>
        {snapshot.chat_settings.model_options.map((item) => (
          <option key={item.id} value={item.id}>{item.display_name}</option>
        ))}
      </select>
      <select aria-label="Effort" disabled={disabled} value={settings.reasoning_effort}
        onChange={(event) => onSettings(characterId, { reasoning_effort: (event.target.value || null) as Effort | null })}>
        <option value="">Default</option>
        {efforts.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
      </select>
    </div>
  );
}
