import type { CharacterAppearance } from '../api/client';

// A character speaks in its own hand so a reader can tell one voice from another
// without reading every name. Only the departures from the interface's own
// settings are named, so an unstyled character adds no classes at all.
export function voiceClasses(appearance: CharacterAppearance | undefined): string {
  if (!appearance) return '';
  const classes: string[] = [];
  if (appearance.font !== 'sans') classes.push(`cha-font-${appearance.font}`);
  if (appearance.style === 'italic') classes.push('cha-slant-italic');
  if (appearance.weight !== 'normal') classes.push(`cha-weight-${appearance.weight}`);
  if (appearance.size !== 'normal') classes.push(`cha-scale-${appearance.size}`);
  if (appearance.text_color !== 'normal') classes.push(`cha-color-${appearance.text_color}`);
  return classes.length > 0 ? ` ${classes.join(' ')}` : '';
}
