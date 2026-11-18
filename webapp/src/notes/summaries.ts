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
    compressed(id: number): Promise<void>;
  },
): Promise<void> {
  const folder = source.kind === 'folder';
  const targets = admitted.map(target => ({ ...target, kind: 'note' as const })).sort(compareNoteSiblings);
  let makeL3 = folder;
  // Sections keep their place in tree order, whichever line finishes first.
  const sections: (Section | undefined)[] = targets.map(() => undefined);

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
      if (folder && !target.has_compressed) await step(async () => {
        if (await client.summaryCompress(id)) await view.compressed(id);
      });
      if (!folder || !target.has_l1) await step(() => makeLevel(target, 'l1'));
      if (!folder || !target.has_l2) await step(() => makeLevel(target, 'l2'));
      if (makeL3) await step(async () => {
        try {
          sections[index] = { name, markdown: await client.summaryGenerate(id, 'l3_section') };
        } catch (failure) {
          makeL3 = false;
          throw failure;
        }
      });
      await step(() => client.summaryLine(id, true));
    }
  }
  const settled = await Promise.allSettled(Array.from({ length: Math.min(3, targets.length) }, line));
  for (const result of settled) if (result.status === 'rejected') throw result.reason;

  if (!makeL3 || sections.includes(undefined)) return;
  const complete = sections as Section[];
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
