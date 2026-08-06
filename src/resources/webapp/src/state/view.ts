export type MainView =
  | 'chat'
  | 'personas'
  | 'characters'
  | 'character-detail'
  | 'forums'
  | 'sessions'
  | 'new-session';

export const navigationTitles: Partial<Record<MainView, string>> = {
  personas: 'Personas',
  characters: 'Characters',
  'character-detail': 'Assistant',
  forums: 'Forums',
  sessions: 'Sessions',
  'new-session': 'New session',
};
