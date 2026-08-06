import { useState } from 'react';

import { navigationTitles, type MainView } from '../state/view';
import { MenuIcon } from './Icons';
import {
  CharacterDetailScreen,
  CharactersScreen,
  ChatScreen,
  ForumsScreen,
  NewSessionScreen,
  PersonasScreen,
  SessionsScreen,
} from './Screens';
import { Sidebar } from './Sidebar';

function Screen({ view, onNavigate }: { view: MainView; onNavigate: (view: MainView) => void }) {
  switch (view) {
    case 'chat': return <ChatScreen />;
    case 'personas': return <PersonasScreen />;
    case 'characters': return <CharactersScreen onNavigate={onNavigate} />;
    case 'character-detail': return <CharacterDetailScreen onNavigate={onNavigate} />;
    case 'forums': return <ForumsScreen onNavigate={onNavigate} />;
    case 'sessions': return <SessionsScreen onNavigate={onNavigate} />;
    case 'new-session': return <NewSessionScreen onNavigate={onNavigate} />;
  }
}

export function App() {
  const [sidebarOpen, setSidebarOpen] = useState(true);
  const [view, setView] = useState<MainView>('chat');
  const title = navigationTitles[view];

  return (
    <div
      className={`cha-app ${sidebarOpen ? 'is-sidebar-open' : ''}`}
      data-sidebar={sidebarOpen ? 'open' : 'closed'}
    >
      <Sidebar activeView={view} onNavigate={setView} />
      <main className="cha-main">
        <header className="cha-topbar">
          <button
            aria-expanded={sidebarOpen}
            aria-label={sidebarOpen ? 'Hide sidebar' : 'Show sidebar'}
            className="cha-icon-action"
            onClick={() => setSidebarOpen((open) => !open)}
            type="button"
          >
            <MenuIcon />
          </button>
          <div className="cha-topbar-title">{title && <h1>{title}</h1>}</div>
          <div className="cha-empty-action" aria-hidden="true" />
        </header>
        <Screen onNavigate={setView} view={view} />
      </main>
    </div>
  );
}
