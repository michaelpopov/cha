const wordEdge = String.raw`[\p{L}\p{N}_]`;
const punctuation = String.raw`[,.!?;:]`;

const dictationCommands = [
  {
    phrase: 'exclamation (?:mark|point|sign)|восклицательный знак|знак восклицания',
    replacement: '!',
  },
  {
    phrase: 'question mark|вопросительный знак|знак вопроса',
    replacement: '?',
  },
  { phrase: 'comma|запятая', replacement: ',' },
  { phrase: 'period|full stop|точка', replacement: '.' },
  { phrase: 'new line|новая строка|с новой строки', replacement: '\n' },
].map(({ phrase, replacement }) => ({
  pattern: new RegExp(
    `(?:${punctuation}[ \\t]*)?[ \\t]*(?<!${wordEdge})(?:${phrase})`
      + `(?!${wordEdge})(?:[ \\t]*${punctuation})?`,
    'giu',
  ),
  replacement,
}));

// Shared by OpenAI's first-delta helper and the xAI per-piece adapter.
export function normalizeDictationCommands(transcription: string): string {
  let result = transcription;
  for (const command of dictationCommands) {
    result = result.replace(command.pattern, (match, offset: number, source: string) => {
      if (command.replacement === '\n') return '\n';
      const next = source[offset + match.length];
      return next && !/\s/.test(next) ? `${command.replacement} ` : command.replacement;
    });
  }
  return result.replace(/[ \t]*\n[ \t]*/g, '\n');
}

export function appendTranscription(current: string, transcription: string): string {
  const addition = normalizeDictationCommands(transcription).trim();
  if (!addition) return current;
  if (!current || /\s$/.test(current) || /^[,.;:!?)}\]]/.test(addition)) {
    return current + addition;
  }
  return `${current} ${addition}`;
}

function needsSpace(previous: string, next: string): boolean {
  return previous !== ''
    && !/\s/.test(previous)
    && !/^[\s,.;:!?)}\]]/.test(next);
}

// xAI pieces are already normalized. The first composer addition uses this so a
// newline is kept. Later xAI additions concatenate the prepared piece.
export function appendPreparedTranscription(current: string, prepared: string): string {
  if (!prepared) return current;
  const previous = current.length > 0 ? current[current.length - 1] ?? '' : '';
  return needsSpace(previous, prepared) ? `${current} ${prepared}` : current + prepared;
}

// Normalize one native piece. Strip spaces and tabs at the ends, keep line
// breaks, and add a separator from the last character already sent.
export function prepareDictationPiece(raw: string, previousCharacter: string): string | null {
  const normalized = normalizeDictationCommands(raw).replace(/^[ \t]+|[ \t]+$/g, '');
  if (!normalized) return null;
  return needsSpace(previousCharacter, normalized) ? ` ${normalized}` : normalized;
}
