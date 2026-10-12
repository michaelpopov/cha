export interface PromptDrafts {
  chat: { key: string; text: string };
  notes: string;
}

export function emptyPromptDrafts(): PromptDrafts {
  return { chat: { key: '', text: '' }, notes: '' };
}

export function chatDraftKey(
  vault: string | undefined,
  forumId: string,
  sessionId: string,
): string {
  return `${vault ?? ''}/${forumId}/${sessionId}`;
}

// An empty chat key is not bound to a conversation yet. Show its text until
// the next edit or a successful open stores the active key.
export function visibleChatDraft(drafts: PromptDrafts, activeKey: string): string {
  if (drafts.chat.key === '' || drafts.chat.key === activeKey) return drafts.chat.text;
  return '';
}

export function editChatDraft(
  drafts: PromptDrafts,
  activeKey: string,
  text: string,
): PromptDrafts {
  return { ...drafts, chat: { key: activeKey, text } };
}

export function editNotesDraft(drafts: PromptDrafts, text: string): PromptDrafts {
  return { ...drafts, notes: text };
}

export function adoptChatDraft(drafts: PromptDrafts, key: string): PromptDrafts {
  if (drafts.chat.key === key) return drafts;
  return { ...drafts, chat: { key, text: '' } };
}

export function clearPromptDrafts(): PromptDrafts {
  return emptyPromptDrafts();
}
