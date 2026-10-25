import { render, screen } from '@testing-library/react';
import { useLayoutEffect, useRef } from 'react';
import { expect, it, vi } from 'vitest';

import { DetailActions } from './DetailActions';

it('keeps a file validation error raised before passive effects run and resets it for another file', () => {
  const onSave = vi.fn(async () => {});
  const text = vi.fn();
  const file = new File(['binary'], 'portrait.jpg', { type: 'image/jpeg' });
  Object.defineProperty(file, 'text', { value: text });

  function UploadOnMount({ name }: { name: string }) {
    const root = useRef<HTMLDivElement>(null);
    useLayoutEffect(() => {
      // A DOM control is usable as soon as it mounts, before passive effects.
      const input = root.current!.querySelector('input[type="file"]')!;
      Object.defineProperty(input, 'files', { value: [file] });
      input.dispatchEvent(new Event('change', { bubbles: true }));
    }, []);
    return <div ref={root}>
      <DetailActions name={name} subject="File" editor={{
        title: 'Edit file', value: 'Original text', uploadLabel: 'Replace file', onSave,
      }} />
    </div>;
  }

  const view = render(<UploadOnMount name="CHARACTER.md" />);
  expect(screen.getByRole('alert')).toHaveTextContent('Choose a Markdown or text file.');
  expect(text).not.toHaveBeenCalled();
  expect(onSave).not.toHaveBeenCalled();
  view.rerender(<UploadOnMount name="CHARACTER.md" />);
  expect(screen.getByRole('alert')).toHaveTextContent('Choose a Markdown or text file.');
  view.rerender(<UploadOnMount name="NOTES.md" />);
  expect(screen.queryByRole('alert')).toBeNull();
});
