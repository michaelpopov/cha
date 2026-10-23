import { Node, type JSONContent } from '@tiptap/core';
import { Fragment, Slice } from '@tiptap/pm/model';
import { compareNoteSiblings, type NoteTreeItem } from './tree';

export type AnnotationDestination = {
  entryType: 'comment' | 'highlight';
  ordinal: number;
  prefix: string;
};
export const AnnotationSource = Node.create<{
  open?: (sourceId: number, destination: AnnotationDestination) => void;
}>({
  name: 'annotationSource',
  group: 'inline',
  inline: true,
  atom: true,
  selectable: false,
  draggable: false,
  addOptions: () => ({}),
  addAttributes: () => ({ sourceId: { default: null }, entryType: { default: 'highlight' }, ordinal: { default: 1 } }),
  renderHTML: () => ['span'],
  renderText: () => '',
  addNodeView() {
    return ({ node, editor, getPos }) => {
      const dom = document.createElement('button');
      dom.type = 'button';
      dom.contentEditable = 'false';
      dom.className = 'cha-annotation-source';
      const comment = node.attrs.entryType === 'comment';
      dom.setAttribute('aria-label', comment ? 'Go to comment' : 'Go to highlight');
      const path = comment ? 'M4 5h16v11H9l-5 4z' : 'm9 11-6 6v3h9l3-3M22 12l-4 4-9-9 4-4a2.83 2.83 0 0 1 4 0l5 5a2.83 2.83 0 0 1 0 4ZM3 22h7';
      dom.innerHTML = `<svg width="16" height="16" viewBox="0 0 24 24" aria-hidden="true"><path d="${path}" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/></svg>`;
      dom.addEventListener('click', () => {
        const pos = getPos();
        if (pos === undefined) return;
        const paragraph = editor.state.doc.resolve(pos).parent;
        const prefix = paragraph.textBetween(node.nodeSize, paragraph.content.size, '', '\n');
        this.options.open?.(node.attrs.sourceId, { entryType: node.attrs.entryType, ordinal: node.attrs.ordinal, prefix });
      });
      return { dom, stopEvent: () => true };
    };
  },
});

// The backend stores note groups in ID order. Show them in folder tree order.
// A group starts at a level 1 heading, and its first source icon gives the note.
export function sortAnnotationGroups(document: JSONContent, items: readonly NoteTreeItem[]): JSONContent {
  const groups: { nodes: JSONContent[]; item?: NoteTreeItem }[] = [];
  for (const node of document.content ?? []) {
    if (groups.length === 0 || (node.type === 'heading' && node.attrs?.level === 1)) groups.push({ nodes: [] });
    const group = groups[groups.length - 1];
    group.nodes.push(node);
    const source = node.content?.find((child) => child.type === 'annotationSource');
    group.item ??= items.find((item) => item.id === source?.attrs?.sourceId);
  }
  // Groups without a known note stay at the end in their order.
  groups.sort((left, right) => (left.item && right.item
    ? compareNoteSiblings(left.item, right.item)
    : Number(!left.item) - Number(!right.item)));
  return { ...document, content: groups.flatMap((group) => group.nodes) };
}

export function withoutAnnotationSources(slice: Slice): Slice {
  function clean(fragment: Fragment): Fragment {
    const nodes: import('@tiptap/pm/model').Node[] = [];
    fragment.forEach((node) => {
      if (node.type.name !== 'annotationSource') nodes.push(node.isLeaf ? node : node.copy(clean(node.content)));
    });
    return Fragment.fromArray(nodes);
  }
  return new Slice(clean(slice.content), slice.openStart, slice.openEnd);
}
