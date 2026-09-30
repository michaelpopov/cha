import { expect, it } from 'vitest';

import { ChaWebError } from './client';
import { classifyWriteFailure } from './outcome';

it.each([
  [422, 'invalid_argument', 'rejected'],
  [400, 'prompt_too_large', 'rejected'],
  [415, 'invalid_argument', 'rejected'],
  [413, undefined, 'rejected'],
  [404, 'not_found', 'missing'],
  [422, undefined, 'unknown'],
  [404, undefined, 'unknown'],
  [500, 'invalid_argument', 'unknown'],
  [500, 'command_timeout', 'unknown'],
] as const)('classifies write status %i and code %s as %s', (status, code, expected) => {
  expect(classifyWriteFailure(new ChaWebError(status, 'The request failed.', code))).toBe(expected);
});
