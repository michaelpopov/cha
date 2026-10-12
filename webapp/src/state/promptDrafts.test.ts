import { describe, expect, it } from 'vitest';

import {
  adoptChatDraft,
  chatDraftKey,
  clearPromptDrafts,
  editChatDraft,
  editNotesDraft,
  emptyPromptDrafts,
  visibleChatDraft,
} from './promptDrafts';

describe('prompt drafts', () => {
  it('keeps one chat slot and one notes string', () => {
    const drafts = emptyPromptDrafts();
    const welcome = chatDraftKey('Personal', 'entrance', 'welcome');
    const withChat = editChatDraft(drafts, welcome, 'Hello');
    const withNotes = editNotesDraft(withChat, 'A note');

    expect(withNotes).toEqual({
      chat: { key: welcome, text: 'Hello' },
      notes: 'A note',
    });
    expect(visibleChatDraft(withNotes, welcome)).toBe('Hello');
    expect(visibleChatDraft(withNotes, chatDraftKey('Personal', 'lobby', 'planning'))).toBe('');
    expect(editChatDraft(withNotes, welcome, 'Hello there').notes).toBe('A note');
  });

  it('shows an unbound chat draft and binds it on the next edit', () => {
    const drafts = editChatDraft(emptyPromptDrafts(), '', 'Unsent');
    const welcome = chatDraftKey('Personal', 'entrance', 'welcome');

    expect(visibleChatDraft(drafts, welcome)).toBe('Unsent');
    expect(editChatDraft(drafts, welcome, 'Unsent').chat.key).toBe(welcome);
  });

  it('replaces the chat slot only when the conversation key differs', () => {
    const welcome = chatDraftKey('Personal', 'entrance', 'welcome');
    const planning = chatDraftKey('Personal', 'lobby', 'planning');
    const drafts = editNotesDraft(editChatDraft(emptyPromptDrafts(), welcome, 'Hello'), 'A note');

    expect(adoptChatDraft(drafts, welcome)).toBe(drafts);
    expect(adoptChatDraft(drafts, planning)).toEqual({
      chat: { key: planning, text: '' },
      notes: 'A note',
    });
  });

  it('clears both drafts together', () => {
    const drafts = editNotesDraft(
      editChatDraft(emptyPromptDrafts(), chatDraftKey('Personal', 'entrance', 'welcome'), 'Hello'),
      'A note',
    );
    expect(clearPromptDrafts()).toEqual(emptyPromptDrafts());
    expect(clearPromptDrafts()).not.toBe(drafts);
  });
});
