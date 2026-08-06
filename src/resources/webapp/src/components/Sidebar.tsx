import type { MainView } from '../state/view';
import {
  CharacterIcon,
  ForumsIcon,
  PersonasIcon,
} from './Icons';

interface SidebarProps {
  activeView: MainView;
  onNavigate: (view: MainView) => void;
}

const navigation = [
  { view: 'personas' as const, label: 'Personas', icon: PersonasIcon },
  { view: 'characters' as const, label: 'Characters', icon: CharacterIcon },
  { view: 'forums' as const, label: 'Forums', icon: ForumsIcon },
];

export function Sidebar({ activeView, onNavigate }: SidebarProps) {
  return (
    <aside className="cha-sidebar" aria-label="Sidebar">
      <div className="cha-brand">cha</div>
      <nav className="cha-sidebar-nav" aria-label="Primary">
        {navigation.map(({ view, label, icon: NavigationIcon }) => (
          <button
            className={`cha-side-action ${activeView === view ? 'is-current' : ''}`}
            key={view}
            onClick={() => onNavigate(view)}
            type="button"
          >
            <NavigationIcon />
            <span>{label}</span>
          </button>
        ))}
      </nav>
      <div className="cha-section-label">Recent</div>
      <div className="cha-recents" aria-label="Recent sessions">
        <p className="cha-empty-list">No recent sessions</p>
      </div>
    </aside>
  );
}
