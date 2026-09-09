import {
  createContext,
  useContext,
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type ChangeEvent,
  type InputHTMLAttributes,
  type ReactNode,
  type RefObject,
} from 'react';

import { transliterateRussianChange } from './transliteration';

type TextField = HTMLInputElement | HTMLTextAreaElement;

interface TransliterationMode {
  enabled: boolean;
  toggle(): void;
}

const TransliterationModeContext = createContext<TransliterationMode | null>(null);

export function TransliterationProvider({ children }: { children: ReactNode }) {
  const [enabled, setEnabled] = useState(false);
  const mode = {
    enabled,
    toggle() {
      setEnabled((current) => !current);
    },
  };

  useEffect(() => {
    const toggleOnShortcut = (event: KeyboardEvent) => {
      if (
        (event.code !== 'KeyY' && event.key.toLowerCase() !== 'y')
        || !event.ctrlKey
        || !event.shiftKey
        || event.altKey
        || event.metaKey
        || event.repeat
      ) return;

      event.preventDefault();
      setEnabled((current) => !current);
    };
    window.addEventListener('keydown', toggleOnShortcut);
    return () => window.removeEventListener('keydown', toggleOnShortcut);
  }, []);

  return (
    <TransliterationModeContext.Provider value={mode}>
      {children}
    </TransliterationModeContext.Provider>
  );
}

export interface Transliteration<T extends TextField> {
  enabled: boolean;
  field: RefObject<T | null>;
  toggle(): void;
  convert(event: ChangeEvent<T>, current: string): string;
}

/** Latin-to-Russian typing for one controlled text field. */
export function useTransliteration<T extends TextField>(value: string): Transliteration<T> {
  const mode = useContext(TransliterationModeContext);
  const [localEnabled, setLocalEnabled] = useState(false);
  const field = useRef<T | null>(null);
  const pendingSelection = useRef<number | null>(null);
  const enabled = mode?.enabled ?? localEnabled;

  useLayoutEffect(() => {
    const input = field.current;
    if (!input) return;
    if (pendingSelection.current !== null && document.activeElement === input) {
      input.setSelectionRange(pendingSelection.current, pendingSelection.current);
    }
    pendingSelection.current = null;
  }, [value]);

  return {
    enabled,
    field,
    toggle() {
      if (mode) mode.toggle();
      else setLocalEnabled((current) => !current);
      field.current?.focus();
    },
    convert(event, current) {
      const next = event.target.value;
      const composing = 'isComposing' in event.nativeEvent
        && event.nativeEvent.isComposing === true;
      if (!enabled || composing) {
        pendingSelection.current = null;
        return next;
      }

      const change = transliterateRussianChange(
        current,
        next,
        event.target.selectionStart ?? next.length,
      );
      pendingSelection.current = change.selection;
      return change.value;
    },
  };
}

export function TransliterationToggle<T extends TextField>({
  disabled,
  transliteration,
}: {
  disabled?: boolean;
  transliteration: Transliteration<T>;
}) {
  return (
    <button
      aria-label="Latin to Russian transliteration"
      aria-pressed={transliteration.enabled}
      className="cha-transliteration-toggle"
      disabled={disabled}
      onClick={transliteration.toggle}
      title={`${transliteration.enabled ? 'Disable' : 'Enable'} Latin to Russian transliteration (Ctrl+Shift+Y)`}
      type="button"
    >
      A→Я
    </button>
  );
}

interface TransliteratingInputProps extends Omit<
  InputHTMLAttributes<HTMLInputElement>,
  'id' | 'onChange' | 'value'
> {
  id: string;
  label: ReactNode;
  onValueChange(value: string): void;
  value: string;
}

/** A human-facing name or short-text field with its own Russian typing mode. */
export function TransliteratingInput({
  disabled,
  id,
  label,
  onValueChange,
  value,
  ...inputProps
}: TransliteratingInputProps) {
  const transliteration = useTransliteration<HTMLInputElement>(value);
  return (
    <div className="cha-transliterating-input">
      <label htmlFor={id}>{label}</label>
      <div className="cha-transliterating-field">
        <input
          {...inputProps}
          disabled={disabled}
          id={id}
          onChange={(event) => onValueChange(transliteration.convert(event, value))}
          ref={transliteration.field}
          value={value}
        />
        <TransliterationToggle disabled={disabled} transliteration={transliteration} />
      </div>
    </div>
  );
}
