import type { NoteItem, NotesClient, SummaryTarget } from '../api/client';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { markdownNoteDocument, summaryConversionError } from './summaryImport';
import { compareNoteSiblings } from './tree';

type SummaryRun = {
  requested: boolean;
  cancelJob?: () => void;
};

type SummaryProgress = {
  message: string;
  count?: string;
  cancelEnabled: boolean;
};

type Section = { name: string; markdown: string };

function folderSummaryDocument(overview: string, sections: Section[]) {
  const content = markdownNoteDocument(overview).content as unknown[];
  for (const { name, markdown } of sections) {
    content.push({
      type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: name }],
    });
    for (const node of markdownNoteDocument(markdown).content as unknown[]) content.push(node);
  }
  const document = { type: 'doc', content };
  if (noteDocumentByteLength(document) > noteDocumentByteLimit) throw new Error(summaryConversionError);
  return document;
}

export async function summarizeAndWait(
  client: NotesClient,
  source: NoteItem,
  run: SummaryRun,
  checkContext: () => void,
  onProgress: (progress: SummaryProgress) => void,
): Promise<{ targetIds: number[]; folderSummaryCommitted: boolean }> {
  const result = { targetIds: [] as number[], folderSummaryCommitted: false };
  if (run.requested) return result;

  let cancellation: Promise<void> | undefined;
  run.cancelJob = () => {
    try {
      checkContext();
    } catch {
      // Changing vaults already cancels generation in the old vault.
      return;
    }
    cancellation ??= client.summaryCancel().catch(() => {});
  };
  const folder = source.kind === 'folder';
  const sections: Section[] = [];

  async function makeLevel({ id }: SummaryTarget, part: 'l1' | 'l2') {
    const markdown = await client.summaryGenerate(id, part);
    if (run.requested) return;
    checkContext();
    let document;
    try {
      document = markdownNoteDocument(markdown);
    } catch {
      await client.summaryAbandon(id, part);
      return;
    }
    await client.summaryCommit(id, part, document);
  }

  try {
    checkContext();
    const plan = await client.summaryPlan(source.id);
    checkContext();
    const targets = plan.notes.map(target => ({ ...target, kind: 'note' as const })).sort(compareNoteSiblings);
    result.targetIds = targets.map(target => target.id);
    let makeL3 = folder && targets.length > 0;

    async function makeSection({ id, name }: SummaryTarget) {
      try {
        sections.push({ name, markdown: await client.summaryGenerate(id, 'l3_section') });
      } catch {
        makeL3 = false;
      }
    }

    for (const [index, target] of targets.entries()) {
      if (run.requested) break;
      checkContext();
      onProgress({
        message: `Summarizing “${target.name}”…`,
        count: folder ? `Note ${index + 1} of ${targets.length}` : undefined,
        cancelEnabled: true,
      });
      if (run.requested) break;
      const calls = [];
      if (!folder || !target.has_l1) calls.push(makeLevel(target, 'l1'));
      if (!folder || !target.has_l2) calls.push(makeLevel(target, 'l2'));
      if (makeL3) calls.push(makeSection(target));
      await Promise.allSettled(calls);
      checkContext();
    }

    if (!makeL3 || run.requested) return result;
    checkContext();
    const message = 'Writing the Summary note…';
    onProgress({ message, cancelEnabled: true });
    if (run.requested) return result;
    const text = sections.map(({ name, markdown }) => `# ${name}\n\n${markdown}`).join('\n\n');
    try {
      const overview = await client.summaryGenerate(source.id, 'l3_overview', text);
      if (run.requested) return result;
      checkContext();
      let document;
      try {
        document = folderSummaryDocument(overview, sections);
      } catch {
        await client.summaryAbandon(source.id, 'l3');
        return result;
      }
      onProgress({ message, cancelEnabled: false });
      if (!run.requested) {
        checkContext();
        await client.summaryCommit(source.id, 'l3', document);
        result.folderSummaryCommitted = true;
      }
    } catch {
      // The backend reports failures from generation, commit, and abandon.
    } finally {
      checkContext();
    }
    return result;
  } finally {
    run.cancelJob = undefined;
    await cancellation;
  }
}
