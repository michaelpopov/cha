import { hotkeysCoreFeature, syncDataLoaderFeature } from '@headless-tree/core';
import { useTree } from '@headless-tree/react';
import { fireEvent, render, screen } from '@testing-library/react';
import { useRef, useState } from 'react';
import { describe, expect, it } from 'vitest';
import {
  buildNoteIndex,
  compareNoteSiblings,
  focusAfterRemoval,
  noteInSubtree,
  noteItemPath,
  type NoteSibling,
  type NoteTreeItem,
} from './tree';

type Row = NoteSibling & { id: string };

function sortedIds(ids: string[], rows: Record<string, Row>): string[] {
  return [...ids].sort((left, right) => compareNoteSiblings(rows[left], rows[right]));
}

describe('note sibling order', () => {
  it('puts folders first, then localeCompare, then code points', () => {
    const folder = { name: 'b', kind: 'folder' as const };
    const note = { name: 'a', kind: 'note' as const };
    expect(compareNoteSiblings(folder, note)).toBeLessThan(0);
    expect(compareNoteSiblings(
      { name: 'a', kind: 'note' },
      { name: 'b', kind: 'note' },
    )).toBeLessThan(0);
    expect(compareNoteSiblings(
      { name: 'same', kind: 'note' },
      { name: 'same', kind: 'note' },
    )).toBe(0);
    const names: string[] = ['\u00e9', 'e\u0301'];
    const composed = names[0];
    const decomposed = names[1];
    if (composed.localeCompare(decomposed) === 0 && composed !== decomposed) {
      expect(compareNoteSiblings(
        { name: composed, kind: 'note' },
        { name: decomposed, kind: 'note' },
      )).toBe(composed < decomposed ? -1 : 1);
    }
  });
});

describe('note metadata index', () => {
  const items: NoteTreeItem[] = [
    { id: 2, parent_id: null, kind: 'note', name: 'Aardvark' },
    { id: 1, parent_id: null, kind: 'folder', name: 'Study' },
    { id: 4, parent_id: 3, kind: 'note', name: 'Plan' },
    { id: 3, parent_id: 1, kind: 'folder', name: 'Philosophy' },
    { id: 8, parent_id: 99, kind: 'note', name: 'Orphan' },
  ];

  it('sorts folders first and attaches a missing parent to the root', () => {
    const { children } = buildNoteIndex(items);
    expect(children.root).toEqual(['1', '2', '8']);
    expect(children['1']).toEqual(['3']);
    expect(children['3']).toEqual(['4']);
    expect(children['4']).toEqual([]);
  });

  it('builds the full note path', () => {
    expect(noteItemPath(items, 4)).toBe('Study / Philosophy / Plan');
    expect(noteItemPath(items, 8)).toBe('Orphan');
  });

  it('reports subtree membership and focus', () => {
    expect(noteInSubtree(items, 1, 4)).toBe(true);
    expect(noteInSubtree(items, 1, 2)).toBe(false);
    expect(noteInSubtree(items, 4, 4)).toBe(true);
    expect(noteInSubtree(items, 1, null)).toBe(false);
    expect(focusAfterRemoval(items, 4)).toBe(3);
    expect(focusAfterRemoval(items, 3)).toBe(1);
    expect(focusAfterRemoval(items, 1)).toBe(2);
    expect(focusAfterRemoval(items, 2)).toBe(8);
  });
});

describe('headless tree', () => {
  function Harness() {
    const [tick, setTick] = useState(0);
    const store = useRef({
      rows: {
        root: { id: 'root', name: 'root', kind: 'folder' },
        '1': { id: '1', name: 'Alpha', kind: 'folder' },
        '2': { id: '2', name: 'Beta', kind: 'folder' },
        '3': { id: '3', name: 'Aardvark', kind: 'note' },
        '4': { id: '4', name: 'Empty', kind: 'folder' },
        '5': { id: '5', name: 'Plan', kind: 'note' },
      } as Record<string, Row>,
      children: {
        root: ['3', '1', '4', '2'],
        '1': ['5'],
        '2': [],
        '3': [],
        '4': [],
        '5': [],
      } as Record<string, string[]>,
      activated: [] as string[],
      extras: [] as string[],
    });
    const tree = useTree<Row>({
      rootItemId: 'root',
      features: [syncDataLoaderFeature, hotkeysCoreFeature],
      getItemName: (item) => item.getItemData().name,
      isItemFolder: (item) => item.getItemData().kind === 'folder',
      dataLoader: {
        getItem: (id) => store.current.rows[id],
        getChildren: (id) => sortedIds(store.current.children[id] ?? [], store.current.rows),
      },
      hotkeys: {
        customActivate: {
          hotkey: 'Enter',
          handler: (_event, instance) => instance.getFocusedItem().primaryAction(),
        },
      },
      onPrimaryAction: (item) => store.current.activated.push(item.getId()),
    });
    const host = window as unknown as {
      __notesTree?: typeof tree;
      __notesActivated?: string[];
      __notesExtras?: string[];
      __notesRows?: Record<string, Row>;
      __notesChildren?: Record<string, string[]>;
    };
    host.__notesTree = tree;
    host.__notesActivated = store.current.activated;
    host.__notesExtras = store.current.extras;
    host.__notesRows = store.current.rows;
    host.__notesChildren = store.current.children;
    return (
      <div>
        <button type="button" onClick={() => setTick(tick + 1)}>Refresh</button>
        <div {...tree.getContainerProps('Notes')}>
          {tree.getItems().map((item) => {
            const props = item.getProps();
            return (
              <div
                key={item.getId()}
                {...props}
                onClick={(event) => {
                  props.onClick?.(event);
                  store.current.extras.push(item.getId());
                }}
              >
                {item.getItemName()}
              </div>
            );
          })}
        </div>
      </div>
    );
  }

  function press(code: string): void {
    const tree = screen.getByRole('tree');
    fireEvent.keyDown(tree, { key: code, code });
    fireEvent.keyUp(tree, { key: code, code });
  }

  it('loads synchronously, moves focus, and activates with a composed Enter key', () => {
    render(<Harness />);
    const items = screen.getAllByRole('treeitem');
    expect(items.map((item) => item.textContent)).toEqual(['Alpha', 'Beta', 'Empty', 'Aardvark']);
    expect(items[0]).toHaveAttribute('aria-level', '1');
    expect(items[0]).toHaveAttribute('aria-expanded', 'false');
    expect(items[0]).toHaveAttribute('tabindex', '0');
    expect(items[1]).toHaveAttribute('tabindex', '-1');
    expect(items[3]).not.toHaveAttribute('aria-expanded');
    expect(items[0]).toHaveAttribute('aria-setsize', '4');
    expect(items[0]).toHaveAttribute('aria-posinset', '1');
    expect(screen.getByRole('tree')).toHaveAttribute('role', 'tree');

    press('ArrowDown');
    expect(screen.getAllByRole('treeitem')[1]).toHaveAttribute('tabindex', '0');
    press('ArrowRight');
    expect(screen.getByRole('treeitem', { name: 'Beta' })).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getAllByRole('treeitem')).toHaveLength(4);
    press('Enter');
    expect((window as unknown as { __notesActivated: string[] }).__notesActivated).toEqual(['2']);
    press('ArrowLeft');
    expect(screen.getByRole('treeitem', { name: 'Beta' })).toHaveAttribute('aria-expanded', 'false');
  });

  it('expands an empty folder and composes the row click', () => {
    render(<Harness />);
    fireEvent.click(screen.getByRole('treeitem', { name: 'Empty' }));
    expect(screen.getByRole('treeitem', { name: 'Empty' })).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getAllByRole('treeitem').map((item) => item.textContent)).toEqual([
      'Alpha', 'Beta', 'Empty', 'Aardvark',
    ]);
    expect((window as unknown as { __notesExtras: string[] }).__notesExtras).toEqual(['4']);
    expect((window as unknown as { __notesActivated: string[] }).__notesActivated).toEqual(['4']);
  });

  it('rebuilds after scheduleRebuildTree on the next render', () => {
    render(<Harness />);
    const host = window as unknown as {
      __notesRows: Record<string, Row>;
      __notesChildren: Record<string, string[]>;
      __notesTree: { scheduleRebuildTree: () => void };
    };
    host.__notesRows['6'] = { id: '6', name: 'Zulu', kind: 'note' };
    host.__notesChildren.root.push('6');
    host.__notesChildren['6'] = [];
    host.__notesTree.scheduleRebuildTree();
    fireEvent.click(screen.getByRole('button', { name: 'Refresh' }));
    expect(screen.getAllByRole('treeitem').map((item) => item.textContent)).toEqual([
      'Alpha', 'Beta', 'Empty', 'Aardvark', 'Zulu',
    ]);
  });

  it('shows a nested note under an expanded folder', () => {
    render(<Harness />);
    press('Home');
    press('ArrowRight');
    const items = screen.getAllByRole('treeitem');
    expect(items.map((item) => item.textContent)).toEqual(['Alpha', 'Plan', 'Beta', 'Empty', 'Aardvark']);
    expect(screen.getByRole('treeitem', { name: 'Plan' })).toHaveAttribute('aria-level', '2');
    expect(screen.getByRole('treeitem', { name: 'Plan' })).not.toHaveAttribute('aria-expanded');
  });
});
