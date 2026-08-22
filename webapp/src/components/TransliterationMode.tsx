import {
  useLayoutEffect,
  useRef,
  useState,
  type ChangeEvent,
  type RefObject,
} from 'react';

import { transliterateRussianChange } from './transliteration';

type TextField = HTMLInputElement | HTMLTextAreaElement;

export interface Transliteration<T extends TextField> {
  enabled: boolean;
  /** Attach to the field the mode types into. */
  field: RefObject<T | null>;
  toggle(): void;
  /** The value the field should take after one browser edit. */
  convert(event: ChangeEvent<T>, current: string): string;
}

/**
 * Latin-to-Russian typing for one text field. The field keeps owning its own
 * value: this only rewrites the fragment an edit inserted, and puts the caret
 * back where that fragment ended once React has rendered the shorter text.
 */
export function useTransliteration<T extends TextField>(value: string): Transliteration<T> {
  const [enabled, setEnabled] = useState(false);
  const field = useRef<T | null>(null);
  const pendingSelection = useRef<number | null>(null);

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
      setEnabled((current) => !current);
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

/** The mode switch that rides a row next to the field it applies to. */
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
      title={`${transliteration.enabled ? 'Disable' : 'Enable'} Latin to Russian transliteration`}
      type="button"
    >
      A→Я
    </button>
  );
}
