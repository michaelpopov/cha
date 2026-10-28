import { fireEvent, render, screen } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { describe, expect, it, vi } from 'vitest';
import type { SessionSnapshot } from '../api/client';
import { snapshotFixture } from '../test/fixtures';
import { ChatToolbar, toolbarCharacterId } from './ChatToolbar';

function snapshot(): SessionSnapshot {
  return {
    ...snapshotFixture,
    characters: [snapshotFixture.characters[0], { ...snapshotFixture.characters[0], id: 'other', display_name: 'Other' }],
    chat_settings: {
      model_options: [
        { id: 'test', display_name: 'FW Kimi', model: 'a-very-long-service-model-id', host: 'example.com' },
        { id: 'other', display_name: 'Other preset', model: 'another-long-id', host: 'example.com' },
      ],
      characters: [
        { character_id: 'assistant', provider_id: 'test', reasoning_effort: 'high' },
        { character_id: 'other', provider_id: 'other', reasoning_effort: 'low' },
      ],
    },
  };
}

type Entry = SessionSnapshot['transcript'][number];

function entry(kind: Entry['kind'], id: string): Entry {
  return { id: 1, kind, participant_id: id, addressed_to: id, addressed_to_name: id,
    display_name: id, text: '', status: 'complete', created_at: null };
}

describe('toolbar character selection', () => {
  it('uses foreground, then target, then transcript, then forum default and first member', () => {
    const value = snapshot();
    value.generation = { ...value.generation, active: true, character_id: 'other' };
    expect(toolbarCharacterId(value)).toBe('other');
    value.generation.active = false;
    value.transcript = [entry('character', 'other')];
    expect(toolbarCharacterId(value)).toBe('assistant');
    value.default_character_id = '*';
    expect(toolbarCharacterId(value)).toBe('other');
    value.default_character_id = '-';
    value.transcript.push(entry('human', '-'), entry('notice', 'assistant'), entry('character', 'removed'));
    expect(toolbarCharacterId(value)).toBe('other');
    value.transcript = [entry('human', 'other')];
    expect(toolbarCharacterId(value)).toBe('other');
    value.transcript = [entry('error', 'other')];
    expect(toolbarCharacterId(value)).toBe('other');
    value.transcript = [];
    expect(toolbarCharacterId(value)).toBe('assistant');
    value.forum = { ...value.forum, default_character_id: 'removed' };
    expect(toolbarCharacterId(value)).toBe('assistant');
    value.characters = [];
    expect(toolbarCharacterId(value)).toBeUndefined();
  });

  it('ignores an obsolete foreground and survives reconnect using the snapshot alone', () => {
    const value = snapshot();
    value.default_character_id = '*';
    value.generation = { ...value.generation, active: true, character_id: 'removed' };
    value.transcript = [entry('character', 'other')];
    expect(toolbarCharacterId(JSON.parse(JSON.stringify(value)))).toBe('other');
  });
});

describe('Chat toolbar', () => {
  it('has exactly three accessible controls, short labels and explicit single-field resets', async () => {
    const onCharacter = vi.fn();
    const onSettings = vi.fn();
    const value = snapshot();
    const { rerender } = render(<ChatToolbar snapshot={value} disabled={false}
      onCharacter={onCharacter} onSettings={onSettings} />);
    expect(screen.getAllByRole('combobox')).toHaveLength(3);
    expect(screen.getByLabelText('Model')).toHaveDisplayValue('FW Kimi');
    expect(screen.getByLabelText('Effort')).toHaveDisplayValue('High');
    expect(screen.queryByText('a-very-long-service-model-id')).toBeNull();
    expect(screen.queryByText('All characters')).toBeNull();
    expect(screen.queryByText('Self-notes')).toBeNull();
    const user = userEvent.setup();
    await user.selectOptions(screen.getByLabelText('Model'), 'other');
    expect(onSettings).toHaveBeenLastCalledWith('assistant', { provider_id: 'other' });
    expect(screen.getByLabelText('Model')).toHaveValue('test');
    await user.selectOptions(screen.getByLabelText('Model'), '');
    expect(onSettings).toHaveBeenLastCalledWith('assistant', { provider_id: null });
    await user.selectOptions(screen.getByLabelText('Effort'), '');
    expect(onSettings).toHaveBeenLastCalledWith('assistant', { reasoning_effort: null });
    await user.selectOptions(screen.getByLabelText('Effort'), 'xhigh');
    expect(onSettings).toHaveBeenLastCalledWith('assistant', { reasoning_effort: 'xhigh' });
    await user.selectOptions(screen.getByLabelText('Character'), 'other');
    expect(onCharacter).toHaveBeenCalledWith('other');
    rerender(<ChatToolbar snapshot={{ ...value, default_character_id: 'other' }} disabled={false}
      onCharacter={onCharacter} onSettings={onSettings} />);
    expect(screen.getByLabelText('Model')).toHaveDisplayValue('Other preset');
    expect(screen.getByLabelText('Effort')).toHaveDisplayValue('Low');
  });

  it('edits the displayed individual while All characters remains the target', () => {
    const value = snapshot();
    value.default_character_id = '*';
    value.transcript = [entry('human', 'other')];
    const onSettings = vi.fn();
    render(<ChatToolbar snapshot={value} disabled={false} onCharacter={vi.fn()} onSettings={onSettings} />);
    fireEvent.change(screen.getByLabelText('Effort'), { target: { value: 'high' } });
    expect(onSettings).toHaveBeenCalledWith('other', { reasoning_effort: 'high' });
    expect(value.default_character_id).toBe('*');
  });

  it('keeps confirmed values visible while disabled', () => {
    render(<ChatToolbar snapshot={snapshot()} disabled onCharacter={vi.fn()} onSettings={vi.fn()} />);
    expect(screen.getByLabelText('Model')).toHaveDisplayValue('FW Kimi');
    for (const select of screen.getAllByRole('combobox')) expect(select).toBeDisabled();
  });
});
