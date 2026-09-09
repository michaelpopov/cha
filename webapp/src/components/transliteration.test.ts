import { describe, expect, it } from 'vitest';

import { transliterateRussian, transliterateRussianChange } from './transliteration';

describe('Russian transliteration', () => {
  it('uses the longest sequence and preserves capitalization', () => {
    expect(transliterateRussian('Shhuka, Ya, YO, zh, ch, sh'))
      .toBe('Щука, Я, Ё, ж, ч, ш');
  });

  it('uses plus to separate sequences and converts soft and hard signs', () => {
    expect(transliterateRussian("s+hodit', raj+on, pod#ezd, '' ##"))
      .toBe('сходить, район, подъезд, Ь Ъ');
  });

  it('continues a sequence that was converted by the previous keystroke', () => {
    expect(transliterateRussianChange('с', 'сh', 2)).toEqual({ value: 'ш', selection: 1 });
    expect(transliterateRussianChange('Ш', 'ШH', 2)).toEqual({ value: 'Щ', selection: 1 });
    expect(transliterateRussianChange('ы', 'ыa', 2)).toEqual({ value: 'я', selection: 1 });
  });

  it('leaves existing text alone and honors an interactive separator', () => {
    expect(transliterateRussianChange('hello с+', 'hello с+h', 9))
      .toEqual({ value: 'hello сх', selection: 8 });
  });

  // `ye` is deliberately not a sequence of its own. It has to stay ы + е so the
  // plural adjective ending types directly; `je` carries э instead, and the
  // separator reaches the rare й + е.
  it('keeps ye as two letters so common endings survive', () => {
    expect(transliterateRussian('novye starye krasnye')).toBe('новые старые красные');
    expect(transliterateRussian('jetot')).toBe('этот');
    expect(transliterateRussian('foj+e')).toBe('фойе');
  });

  it('leaves a deletion alone rather than converting the shortened text', () => {
    expect(transliterateRussianChange('привет', 'приве', 5))
      .toEqual({ value: 'приве', selection: 5 });
    // Deleting into a position whose neighbours could combine must not convert
    // them: nothing was inserted, so there is no new fragment to read.
    expect(transliterateRussianChange('сhto', 'сh', 2)).toEqual({ value: 'сh', selection: 2 });
  });

  it('converts an insertion in the middle and leaves the caret after it', () => {
    // `ma` typed between `дo` and `й`, with the tail untouched.
    expect(transliterateRussianChange('дой', 'доmaй', 4))
      .toEqual({ value: 'домай', selection: 4 });
  });

  it('converts a pasted fragment and puts the caret at its end', () => {
    expect(transliterateRussianChange('', 'shhuka', 6))
      .toEqual({ value: 'щука', selection: 4 });
    // A paste that lands before existing text keeps that text and still reports
    // the caret at the end of what was converted, not at the end of the value.
    expect(transliterateRussianChange('мир', 'privet мир', 7))
      .toEqual({ value: 'привет мир', selection: 7 });
  });
});
