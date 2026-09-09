const russianSequences = new Map<string, string>([
  ['shh', 'щ'],
  ['zh', 'ж'],
  ['ch', 'ч'],
  ['sh', 'ш'],
  ['jo', 'ё'],
  ['yo', 'ё'],
  ['je', 'э'],
  ['ju', 'ю'],
  ['yu', 'ю'],
  ['ja', 'я'],
  ['ya', 'я'],
  ["''", 'Ь'],
  ['##', 'Ъ'],
  ['a', 'а'],
  ['b', 'б'],
  ['v', 'в'],
  ['g', 'г'],
  ['d', 'д'],
  ['e', 'е'],
  ['z', 'з'],
  ['i', 'и'],
  ['j', 'й'],
  ['k', 'к'],
  ['l', 'л'],
  ['m', 'м'],
  ['n', 'н'],
  ['o', 'о'],
  ['p', 'п'],
  ['r', 'р'],
  ['s', 'с'],
  ['t', 'т'],
  ['u', 'у'],
  ['f', 'ф'],
  ['h', 'х'],
  ['x', 'х'],
  ['c', 'ц'],
  ['w', 'щ'],
  ['y', 'ы'],
  ['q', 'я'],
  ['ö', 'ё'],
  ['ä', 'э'],
  ['ü', 'ю'],
  ['#', 'ъ'],
  ["'", 'ь'],
]);

// A sequence can begin as a complete Cyrillic letter and become a different
// one after the next keystroke: typing `s` first shows с, then `h` changes it
// to ш. These are the possible continuations of an already converted letter.
const russianContinuations = new Map<string, string>([
  ['сh', 'ш'],
  ['зh', 'ж'],
  ['цh', 'ч'],
  ['шh', 'щ'],
  ['йo', 'ё'],
  ['йe', 'э'],
  ['йu', 'ю'],
  ['йa', 'я'],
  ['ыo', 'ё'],
  ['ыu', 'ю'],
  ['ыa', 'я'],
  ["ь'", 'Ь'],
  ['ъ#', 'Ъ'],
]);

function preserveInitialCase(value: string, source: string): string {
  const first = source[0];
  return first && first !== first.toLowerCase() ? value.toUpperCase() : value;
}

/** Convert a pasted or newly inserted Latin fragment to Russian. */
export function transliterateRussian(text: string): string {
  let result = '';

  for (let index = 0; index < text.length;) {
    // A plus separates two otherwise ambiguous sequences and is not included
    // in the result: `s+hodit'` becomes `сходить` rather than `шодить`.
    if (text[index] === '+') {
      index += 1;
      continue;
    }

    let matched = false;
    for (let length = 3; length > 0; length -= 1) {
      const source = text.slice(index, index + length);
      const key = source.toLowerCase();
      const replacement = russianSequences.get(key);
      if (!replacement) continue;
      result += key === "''" || key === '##'
        ? replacement
        : preserveInitialCase(replacement, source);
      index += length;
      matched = true;
      break;
    }

    if (!matched) {
      result += text[index];
      index += 1;
    }
  }

  return result;
}

export interface TransliterationChange {
  value: string;
  selection: number;
}

/**
 * Transliterate only the fragment inserted by one browser edit. Existing text
 * is left alone, which lets the mode be enabled in the middle of a draft.
 */
export function transliterateRussianChange(
  previous: string,
  next: string,
  selection: number,
): TransliterationChange {
  let start = 0;
  while (start < previous.length && start < next.length && previous[start] === next[start]) {
    start += 1;
  }

  let previousEnd = previous.length;
  let nextEnd = next.length;
  while (
    previousEnd > start
    && nextEnd > start
    && previous[previousEnd - 1] === next[nextEnd - 1]
  ) {
    previousEnd -= 1;
    nextEnd -= 1;
  }

  const inserted = next.slice(start, nextEnd);
  if (!inserted || inserted === '+') return { value: next, selection };

  let prefix = next.slice(0, start);
  const suffix = next.slice(nextEnd);
  let converted: string;

  // A plus remains visible until the user types the character it separates.
  // At that point it is removed and prevents the new character from combining
  // with the Cyrillic letter before it.
  if (prefix.endsWith('+')) {
    prefix = prefix.slice(0, -1);
    converted = transliterateRussian(inserted);
  } else {
    const previousLetter = prefix.at(-1);
    const nextLetter = inserted[0];
    const continuationKey = previousLetter && nextLetter
      ? `${previousLetter.toLowerCase()}${nextLetter.toLowerCase()}`
      : '';
    const continuation = russianContinuations.get(continuationKey);

    if (continuation && previousLetter) {
      prefix = prefix.slice(0, -1);
      const fixedUppercase = continuationKey === "ь'" || continuationKey === 'ъ#';
      const head = fixedUppercase
        ? continuation
        : preserveInitialCase(continuation, previousLetter);
      converted = head + transliterateRussian(inserted.slice(1));
    } else {
      converted = transliterateRussian(inserted);
    }
  }

  const convertedEnd = prefix.length + converted.length;
  const distanceAfterInsertion = Math.max(0, selection - nextEnd);
  return {
    value: prefix + converted + suffix,
    selection: convertedEnd + distanceAfterInsertion,
  };
}
