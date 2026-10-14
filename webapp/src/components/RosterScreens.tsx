import {
  useCallback,
  useEffect,
  useRef,
  useState,
  type Dispatch,
  type ReactNode,
} from 'react';
import { publicErrorMessage, type ChaClient } from '../api/client';
import { DetailRefreshProvider, sameDetail, useDetailRefresh } from '../detailRefresh';
import { type AppState, type DataAction, type NavigationAction } from '../state/view';
import { ChevronLeftIcon, ChevronRightIcon } from './Icons';
import { Markdown } from './Markdown';

export interface NavigationScreenProps {
  state: AppState;
  dispatch: Dispatch<DataAction>;
  navigate: Dispatch<NavigationAction>;
}

// Personas, characters, and character files use the same navigation rows.
export function RosterRow({ description, displayName, onSelect }: {
  description?: string;
  displayName: string;
  onSelect?(): void;
}) {
  const content = (
    <>
      <span className="cha-roster-copy">
        <span className="cha-roster-name">{displayName}</span>
        {description && <span className="cha-roster-description">{description}</span>}
      </span>
      {onSelect && <ChevronRightIcon className="cha-chevron" />}
    </>
  );
  return onSelect
    ? <button className="cha-roster-row" onClick={onSelect} type="button">{content}</button>
    : <div className="cha-roster-row">{content}</div>;
}

interface RosterDetailCopy {
  absent: string;
  loading: string;
  failed: string;
}

interface RosterDetailScreenProps<Value> {
  ariaLabel: string;
  backLabel: string;
  copy: RosterDetailCopy;
  fallbackTitle?: string;
  // Stable across renders, so reading one entry does not restart itself.
  load(subjectId: string): Promise<Value>;
  onLoaded?(value: Value, subjectId: string): void;
  render(value: Value, update: (value: Value) => void): ReactNode;
  onBack(): void;
  subjectId: string | null;
  // Facts the roster already knows, shown above the content and while it is
  // still loading. A persona or character has none; a forum names its cast.
  subtitle?: ReactNode;
  toolbarAction?: ReactNode | ((value: Value, update: (value: Value) => void) => ReactNode);
}

export function RosterDetailScreen<Value>({
  ariaLabel,
  backLabel,
  copy,
  fallbackTitle,
  load,
  onLoaded,
  onBack,
  render,
  subjectId,
  subtitle,
  toolbarAction,
}: RosterDetailScreenProps<Value>) {
  const refresh = useDetailRefresh();
  const [value, setValue] = useState<Value | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [requestVersion, setRequestVersion] = useState(0);
  const [refreshing, setRefreshing] = useState(false);
  const valueRef = useRef<Value | null>(null);
  const seenSubject = useRef<string | null>(null);
  const updateValue = useCallback((next: Value | null) => {
    valueRef.current = next;
    setValue(next);
  }, []);
  const retry = () => setRequestVersion((version) => version + 1);

  useEffect(() => {
    if (!subjectId) return;
    let current = true;
    const identityChanged = seenSubject.current !== subjectId;
    const background = !identityChanged && valueRef.current !== null;
    seenSubject.current = subjectId;
    if (!background) {
      updateValue(null);
      setError(null);
    }
    setRefreshing(background);
    void load(subjectId).then(
      (loaded) => {
        if (!current) return;
        setError(null);
        setRefreshing(false);
        if (!background || !sameDetail(valueRef.current, loaded)) {
          updateValue(loaded);
          onLoaded?.(loaded, subjectId);
        }
      },
      (failure: unknown) => {
        if (!current) return;
        setRefreshing(false);
        setError(publicErrorMessage(failure, copy.failed));
      },
    );
    return () => {
      current = false;
    };
  }, [copy.failed, load, onLoaded, refresh.epoch, requestVersion, subjectId, updateValue]);

  const detailRefresh = { epoch: refresh.epoch, refreshing, failed: error !== null, stale: false, retry };

  return (
    <section className="cha-screen cha-navigation" aria-label={ariaLabel}>
      <div className="cha-detail-toolbar">
        <button className="cha-back-row" onClick={onBack} type="button">
          <ChevronLeftIcon />
          <span>{backLabel}</span>
        </button>
        {typeof toolbarAction === 'function'
          ? value !== null && (
            <DetailRefreshProvider value={detailRefresh}>
              {toolbarAction(value, updateValue)}
            </DetailRefreshProvider>
          )
          : toolbarAction}
      </div>
      {subtitle}
      {value === null && fallbackTitle && (
        <div className="cha-detail-actions"><h1>{fallbackTitle}</h1></div>
      )}
      {!subjectId && <p className="cha-state-message">{copy.absent}</p>}
      {subjectId && value === null && !error && (
        <p className="cha-state-message" role="status">{copy.loading}</p>
      )}
      {error && (
        <div className="cha-state-message cha-error-message" role="alert">
          <p>{error}</p>
          <button
            className="cha-button cha-button-ghost"
            onClick={retry}
            type="button"
          >
            Try again
          </button>
        </div>
      )}
      {/* The title and text editor below save this screen's detail. */}
      {value !== null && (
        <DetailRefreshProvider value={detailRefresh}>
          {render(value, updateValue)}
        </DetailRefreshProvider>
      )}
    </section>
  );
}

export function rosterMarkdown(markdown: string, empty: string): ReactNode {
  return markdown.trim() === ''
    ? <p className="cha-state-message">{empty}</p>
    : <Markdown source={markdown} />;
}

export interface RosterDetailProps extends NavigationScreenProps {
  client: ChaClient;
}

// The lists hold only names the workspace could resolve, so a saved name it no
// longer can would match no option and read as though nothing were set. It is
// still what the file says — and still what a save resubmits — so it is listed,
// marked, and left selectable.
export function unresolvedOption(
  options: readonly { id: string; label: string }[],
  saved: string | null,
): { id: string; label: string } | null {
  if (saved === null || options.some(({ id }) => id === saved)) return null;
  return { id: saved, label: `${saved} (not available)` };
}
