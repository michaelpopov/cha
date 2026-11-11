import { describe, expect, it, vi } from 'vitest';

import type { NotesClient, SummaryTarget } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { noteDocument, noteItem, paragraph, textNode } from '../test/notes';
import { noteDocumentByteLimit } from './documentBytes';
import { summarizeAndWait } from './summaries';

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(done => { resolve = done; });
  return { promise, resolve };
}

const folder = noteItem({ id: 9, kind: 'folder' });
const alpha = { id: 1, name: 'Alpha * #', has_l1: false, has_l2: false };
const beta = { id: 2, name: 'Beta', has_l1: true, has_l2: true };
const heading = (name: string) => ({ type: 'heading', attrs: { level: 1 }, content: [textNode(name)] });

function setup(targets: SummaryTarget[] = [beta, alpha]) {
  const calls = {
    summaryPlan: vi.fn<NotesClient['summaryPlan']>(async () => ({ notes: targets })),
    summaryGenerate: vi.fn<NotesClient['summaryGenerate']>(async (id, part) =>
      part === 'l3_overview' ? 'Overview' : part === 'l3_section' ? `Section ${id}` : 'Summary'),
    summaryCommit: vi.fn<NotesClient['summaryCommit']>(async () => {}),
    summaryAbandon: vi.fn<NotesClient['summaryAbandon']>(async () => {}),
    summaryCancel: vi.fn<NotesClient['summaryCancel']>(async () => {}),
  };
  const client = fixtureClient(calls);
  const run: Parameters<typeof summarizeAndWait>[2] = { requested: false };
  const checkContext = vi.fn();
  const progress = vi.fn();
  return {
    ...calls, run, checkContext, progress,
    start: (source = folder) => summarizeAndWait(client, source, run, checkContext, progress),
  };
}

describe('summarizeAndWait', () => {
  it('runs one folder target at a time in tree order, with up to three parallel calls per target', async () => {
    const test = setup();
    const level = deferred<string>();
    test.summaryGenerate.mockImplementationOnce(() => level.promise);
    const done = test.start();

    await vi.waitFor(() => expect(test.summaryGenerate.mock.calls).toEqual([
      [1, 'l1'], [1, 'l2'], [1, 'l3_section'],
    ]));
    level.resolve('Level one');
    expect(await done).toEqual({ targetIds: [1, 2], folderSummaryCommitted: true });
    expect(test.summaryPlan).toHaveBeenCalledExactlyOnceWith(9);
    expect(test.summaryGenerate.mock.calls).toEqual([
      [1, 'l1'], [1, 'l2'], [1, 'l3_section'], [2, 'l3_section'],
      [9, 'l3_overview', '# Alpha * #\n\nSection 1\n\n# Beta\n\nSection 2'],
    ]);
    expect(test.summaryCommit).toHaveBeenCalledWith(1, 'l2', noteDocument(heading(alpha.name), paragraph('Summary')));
    expect(test.summaryCommit).toHaveBeenLastCalledWith(9, 'l3', noteDocument(
      heading(folder.name), paragraph('Overview'),
      heading(alpha.name), paragraph('Section 1'), heading(beta.name), paragraph('Section 2'),
    ));
    expect(test.progress.mock.calls.map(([value]) => value)).toEqual([
      { message: 'Summarizing “Alpha * #”…', count: 'Note 1 of 2', cancelEnabled: true },
      { message: 'Summarizing “Beta”…', count: 'Note 2 of 2', cancelEnabled: true },
      { message: 'Writing the Summary note…', cancelEnabled: true },
      { message: 'Writing the Summary note…', cancelEnabled: false },
    ]);
    expect(test.progress.mock.invocationCallOrder.at(-1)).toBeLessThan(test.summaryCommit.mock.invocationCallOrder.at(-1)!);
  });

  it('replaces both levels of a single note even when they exist', async () => {
    const test = setup([beta]);
    expect(await test.start(noteItem({ id: beta.id }))).toEqual({
      targetIds: [2], folderSummaryCommitted: false,
    });
    expect(test.summaryGenerate.mock.calls).toEqual([[2, 'l1'], [2, 'l2']]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([[2, 'l1'], [2, 'l2']]);
  });

  it('continues levels after a failed level and drops the overview and later sections after a failed section', async () => {
    const test = setup([{ ...beta, has_l1: false, has_l2: false }, alpha]);
    test.summaryGenerate.mockImplementation(async (id, part) => {
      if (id === 1 && (part === 'l1' || part === 'l3_section')) throw new Error('Provider refused');
      return 'Summary';
    });
    expect(await test.start()).toEqual({ targetIds: [1, 2], folderSummaryCommitted: false });
    expect(test.summaryGenerate.mock.calls).toEqual([
      [1, 'l1'], [1, 'l2'], [1, 'l3_section'], [2, 'l1'], [2, 'l2'],
    ]);
    expect(test.summaryCommit.mock.calls.map(([id, part]) => [id, part])).toEqual([
      [1, 'l2'], [2, 'l1'], [2, 'l2'],
    ]);
  });

  it('makes no generation calls for an empty plan', async () => {
    const test = setup([]);
    expect(await test.start()).toEqual({ targetIds: [], folderSummaryCommitted: false });
    expect(test.summaryGenerate).not.toHaveBeenCalled();
    expect(test.summaryCommit).not.toHaveBeenCalled();
    expect(test.progress).not.toHaveBeenCalled();
  });

  it('cancels active generation, ignores late output, and waits for a commit already sent', async () => {
    const test = setup();
    const late = deferred<string>();
    const commit = deferred<void>();
    test.summaryGenerate.mockImplementationOnce(() => late.promise);
    test.summaryCommit.mockImplementationOnce(() => commit.promise);
    const completed = vi.fn();
    const done = test.start().then(completed);

    await vi.waitFor(() => expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(
      1, 'l2', noteDocument(heading(alpha.name), paragraph('Summary')),
    ));
    test.run.requested = true;
    test.run.cancelJob?.();
    expect(test.summaryCancel).toHaveBeenCalledOnce();
    late.resolve('Late L1');
    await late.promise;
    expect(completed).not.toHaveBeenCalled();
    commit.resolve();
    await done;
    expect(test.summaryCommit).toHaveBeenCalledOnce();
    expect(test.summaryGenerate.mock.calls).toEqual([[1, 'l1'], [1, 'l2'], [1, 'l3_section']]);
    expect(test.run.cancelJob).toBeUndefined();
  });

  it('ignores an overview that arrives after Cancel', async () => {
    const test = setup([beta]);
    const overview = deferred<string>();
    test.summaryGenerate.mockResolvedValueOnce('Section').mockReturnValueOnce(overview.promise);
    const done = test.start();
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenLastCalledWith(9, 'l3_overview', '# Beta\n\nSection'));
    test.run.requested = true;
    test.run.cancelJob?.();
    overview.resolve('Overview');
    expect(await done).toEqual({ targetIds: [2], folderSummaryCommitted: false });
    expect(test.summaryCancel).toHaveBeenCalledOnce();
    expect(test.summaryCommit).not.toHaveBeenCalled();
  });

  it('abandons the correct part after a conversion failure and continues other levels', async () => {
    const test = setup([alpha]);
    test.summaryGenerate.mockImplementation(async (_id, part) => part === 'l1' || part === 'l3_section' ? '' : 'Valid');
    expect(await test.start()).toEqual({ targetIds: [1], folderSummaryCommitted: false });
    expect(test.summaryAbandon.mock.calls).toEqual([[1, 'l1'], [9, 'l3']]);
    expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(1, 'l2', noteDocument(heading(alpha.name), paragraph('Valid')));
  });

  it('abandons L3 when individually valid documents exceed the combined size limit', async () => {
    const test = setup([beta]);
    const markdown = '```\n' + 'a'.repeat(noteDocumentByteLimit / 2) + '\n```';
    test.summaryGenerate.mockResolvedValue(markdown);
    expect(await test.start()).toEqual({ targetIds: [2], folderSummaryCommitted: false });
    expect(test.summaryAbandon).toHaveBeenCalledExactlyOnceWith(9, 'l3');
    expect(test.summaryCommit).not.toHaveBeenCalled();
  });

  it.each(['Late level', ''])('discards a level reply after a vault change (%j)', async markdown => {
    const test = setup();
    const level = deferred<string>();
    test.summaryGenerate.mockImplementationOnce(() => level.promise);
    const changed = new Error('The vault changed');
    const done = test.start();
    await vi.waitFor(() => expect(test.summaryCommit).toHaveBeenCalledExactlyOnceWith(
      1, 'l2', noteDocument(heading(alpha.name), paragraph('Summary')),
    ));
    level.resolve(markdown);
    test.checkContext.mockImplementation(() => { throw changed; });

    await expect(done).rejects.toBe(changed);
    expect(test.summaryGenerate.mock.calls).toEqual([[1, 'l1'], [1, 'l2'], [1, 'l3_section']]);
    expect(test.summaryCommit).toHaveBeenCalledOnce();
    expect(test.summaryAbandon).not.toHaveBeenCalled();
    expect(test.run.cancelJob).toBeUndefined();
  });

  it('discards the overview reply after a vault change', async () => {
    const test = setup([beta]);
    const overview = deferred<string>();
    test.summaryGenerate.mockResolvedValueOnce('Section').mockReturnValueOnce(overview.promise);
    const done = test.start();
    await vi.waitFor(() => expect(test.summaryGenerate).toHaveBeenLastCalledWith(9, 'l3_overview', '# Beta\n\nSection'));
    const changed = new Error('The vault changed');
    overview.resolve('Overview');
    test.checkContext.mockImplementation(() => { throw changed; });

    await expect(done).rejects.toBe(changed);
    expect(test.summaryCommit).not.toHaveBeenCalled();
    expect(test.summaryAbandon).not.toHaveBeenCalled();
  });
});
