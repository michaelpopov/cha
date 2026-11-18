import { ChaError, type NoteItem, type NotesClient, type SummaryTarget } from '../api/client';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { markdownNoteDocument, summaryConversionError } from './summaryImport';
import { compareNoteSiblings } from './tree';

type Section = { name: string; markdown: string };

// Headings are built from names, so Markdown characters in a name stay literal.
function heading(text: string) {
  return { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text }] };
}

function titledDocument(title: string, markdown: string, sections: Section[] = []) {
  const content: unknown[] = [heading(title), ...markdownNoteDocument(markdown).content as unknown[]];
  for (const section of sections) {
    content.push(heading(section.name), ...markdownNoteDocument(section.markdown).content as unknown[]);
  }
  const document = { type: 'doc', content };
  if (noteDocumentByteLength(document) > noteDocumentByteLimit) throw new Error(summaryConversionError);
  return document;
}

export async function runSummaryBatch(
  client: NotesClient,
  source: NoteItem,
  admitted: SummaryTarget[],
  view: {
    checkContext(): void;
    commit: NotesClient['summaryCommit'];
  },
): Promise<void> {
  const targets = admitted.map(target => ({ ...target, kind: 'note' as const })).sort(compareNoteSiblings);
  const sectionCount = targets.filter(target => target.levels.includes('l3')).length;
  let sectionFailed = false;
  // Sections keep their place in tree order, whichever line finishes first.
  const sections: Section[] = [];

  async function makeLevel({ id, name }: SummaryTarget, part: 'l1' | 'l2') {
    const markdown = await client.summaryGenerate(id, part);
    view.checkContext();
    let document;
    try {
      document = part === 'l2' ? titledDocument(name, markdown) : markdownNoteDocument(markdown);
    } catch {
      await client.summaryAbandon(id, part);
      return;
    }
    await view.commit(id, part, document);
  }

  // Individual failures are recorded in Events. A lost vault or connection stops the line.
  async function step(work: () => Promise<void>) {
    view.checkContext();
    try {
      await work();
    } catch (failure) {
      if (failure instanceof ChaError && ['vault_changed', 'server_stopping', 'application_unavailable'].includes(failure.code))
        throw failure;
    }
    view.checkContext();
  }

  // Keep each note's steps in order so the provider can reuse its cached prefix.
  let next = 0;
  async function line() {
    while (next < targets.length) {
      const index = next++;
      const target = targets[index];
      const { id, name } = target;
      await step(() => client.summaryLine(id, false));
      if (target.levels.includes('compressed')) await step(async () => {
        const text = await client.summaryGenerate(id, 'compressed');
        view.checkContext();
        await view.commit(id, 'compressed', text);
      });
      if (target.levels.includes('l1')) await step(() => makeLevel(target, 'l1'));
      if (target.levels.includes('l2')) await step(() => makeLevel(target, 'l2'));
      if (!sectionFailed && target.levels.includes('l3')) await step(async () => {
        try {
          sections[index] = { name, markdown: await client.summaryGenerate(id, 'l3_section') };
        } catch (failure) {
          sectionFailed = true;
          throw failure;
        }
      });
      await step(() => client.summaryLine(id, true));
    }
  }
  const settled = await Promise.allSettled(Array.from({ length: Math.min(3, targets.length) }, line));
  for (const result of settled) if (result.status === 'rejected') throw result.reason;

  const complete = sections.filter(Boolean);
  if (sectionFailed || sectionCount === 0 || complete.length !== sectionCount) return;
  const text = complete.map(({ name, markdown }) => `# ${name}\n\n${markdown}`).join('\n\n');
  await step(async () => {
    const overview = await client.summaryGenerate(source.id, 'l3_overview', text);
    view.checkContext();
    let document;
    try {
      document = titledDocument(source.name, overview, complete);
    } catch {
      await client.summaryAbandon(source.id, 'l3');
      return;
    }
    await view.commit(source.id, 'l3', document);
  });
}
