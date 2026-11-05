import { act, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { expect, it, vi } from 'vitest';
import type { ChaClient, ForumLibrary } from '../api/client';
import { DetailRefreshProvider } from '../detailRefresh';
import { initialAppState } from '../state/view';
import { bootstrapFixture, fixtureClient, forumDetailFixture } from '../test/fixtures';
import { ForumLibraryScreen, ForumMembersScreen } from './ForumScreens';

const state = { ...initialAppState, bootstrap: bootstrapFixture, currentForumId: 'lobby' };
const keys = [
  { id: 'openai', display_name: 'OpenAI work', has_value: true, cloud_store: 'openai' as const, used_by: [] },
  { id: 'gemini', display_name: 'Gemini work', has_value: true, cloud_store: 'google' as const, used_by: [] },
];
const stores = { items: [{ id: 'vs_a', name: 'Research' }, { id: 'vs_b', name: 'Research' }, { id: 'vs_c', name: 'Archive' }] };
const library: ForumLibrary = { api_key_id: 'openai', store_ids: ['vs_a'], provider_id: null, catalogue: {} };
function setup(overrides: Partial<ChaClient> = {}, saved: ForumLibrary | null = null) {
  const updateForumLibrary = vi.fn<ChaClient['updateForumLibrary']>(async (_id, library) => ({ ...forumDetailFixture, library }));
  const client = fixtureClient({ getForum: async () => ({ ...forumDetailFixture, library: saved }),
    listApiKeys: async () => keys, listCloudStores: async () => stores,
    listProviders: async () => [{ id: 'sol', display_name: 'Sol', model: 'm', host: 'h' }],
    getCloudStore: async (_key, id) => ({ id, name: `Name ${id}`, description: `Description ${id}`, created_at: '2026-11-03T09:15:00Z' }),
    updateForumLibrary, ...overrides });
  const dispatch = vi.fn();
  const view = render(<ForumLibraryScreen client={client} state={state} dispatch={dispatch} navigate={dispatch} />);
  return { client, updateForumLibrary, dispatch, ...view };
}

it('opens Library from Members and returns to Members', async () => {
  const dispatch = vi.fn();
  const view = render(<ForumMembersScreen client={fixtureClient()} state={state} dispatch={dispatch} navigate={dispatch} />);
  await userEvent.click(screen.getByRole('button', { name: 'Library' }));
  expect(dispatch).toHaveBeenCalledWith({ type: 'show-forum-library' });
  view.unmount();
  setup();
  await userEvent.click(screen.getByRole('button', { name: 'Members' }));
});

it('starts with None, filters keys, limits OpenAI stores, and saves an optional provider', async () => {
  const { updateForumLibrary, dispatch } = setup({ listApiKeys: async () => [...keys,
    { ...keys[0], id: 'empty', display_name: 'Empty', has_value: false },
    { ...keys[0], id: 'models', display_name: 'Models', cloud_store: 'none' }] });
  const key = await screen.findByLabelText('API key');
  expect(key).toHaveValue('');
  await screen.findByRole('option', { name: 'OpenAI work' });
  expect(screen.queryByRole('option', { name: 'Models' })).not.toBeInTheDocument();
  expect(screen.queryByRole('option', { name: 'Empty' })).not.toBeInTheDocument();
  await userEvent.selectOptions(key, 'openai');
  const first = await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  expect(screen.getByLabelText('Default provider')).toHaveValue('');
  await userEvent.click(first);
  await userEvent.click(screen.getByRole('checkbox', { name: 'Research (vs_b)' }));
  expect(screen.getByRole('checkbox', { name: 'Archive' })).toBeDisabled();
  await userEvent.selectOptions(screen.getByLabelText('Default provider'), 'sol');
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(updateForumLibrary).toHaveBeenCalledWith('lobby', { api_key_id: 'openai', store_ids: ['vs_a', 'vs_b'], provider_id: 'sol', catalogue: {
    vs_a: { name: 'Name vs_a', description: 'Description vs_a' },
    vs_b: { name: 'Name vs_b', description: 'Description vs_b' },
  } });
  expect(dispatch).toHaveBeenCalledWith(expect.objectContaining({ type: 'forum-updated' }));
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  await userEvent.click(screen.getByRole('checkbox', { name: 'Research (vs_a)' }));
  await userEvent.click(screen.getByRole('checkbox', { name: 'Research (vs_b)' }));
  expect(screen.getByLabelText('Default provider')).toBeVisible();
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
});

it('restores selections, saves None after provider failure, and retains a failed save draft', async () => {
  const save = vi.fn<ChaClient['updateForumLibrary']>().mockRejectedValueOnce(new Error('failure'))
    .mockImplementation(async (_id, library) => ({ ...forumDetailFixture, library }));
  setup({ listProviders: async () => { throw new Error('failure'); }, updateForumLibrary: save }, { ...library, provider_id: 'sol' });
  expect(await screen.findByRole('checkbox', { name: 'Research (vs_a)' })).toBeChecked();
  await screen.findByText('Providers could not be loaded.');
  expect(screen.getByLabelText('Default provider')).toHaveValue('sol');
  expect(screen.getByRole('option', { name: 'sol' })).toBeInTheDocument();
  await userEvent.selectOptions(screen.getByLabelText('Default provider'), '');
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  await screen.findByText('Library could not be saved.');
  expect(screen.getByRole('checkbox', { name: 'Research (vs_a)' })).toBeChecked();
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(save).toHaveBeenLastCalledWith('lobby', { ...library, catalogue: {
    vs_a: { name: 'Name vs_a', description: 'Description vs_a' },
  } });
});

it('clears Library while store discovery fails', async () => {
  const { updateForumLibrary } = setup({ listCloudStores: async () => { throw new Error('failure'); } }, library);
  await screen.findByText('Cloud stores could not be loaded.');
  await userEvent.selectOptions(screen.getByLabelText('API key'), '');
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(updateForumLibrary).toHaveBeenCalledWith('lobby', null);
});

it('shows an unavailable saved key without listing its stores or providers', async () => {
  const listCloudStores = vi.fn(async () => stores);
  const listProviders = vi.fn(async () => []);
  setup({ listCloudStores, listProviders }, { ...library, api_key_id: 'deleted' });
  expect(await screen.findByRole('option', { name: 'deleted — Unavailable' })).toBeInTheDocument();
  expect(screen.getByRole('alert')).toHaveTextContent('Select an available API key or None to clear the Library.');
  expect(screen.queryByText('Loading cloud stores…')).not.toBeInTheDocument();
  expect(listCloudStores).not.toHaveBeenCalled();
  expect(listProviders).not.toHaveBeenCalled();
});

it('requires removal of missing stores', async () => {
  setup({}, { ...library, store_ids: ['vs_missing'] });
  const missing = await screen.findByRole('checkbox', { name: 'vs_missing — Unavailable' });
  expect(missing).toBeChecked();
  await userEvent.click(screen.getByRole('checkbox', { name: 'Archive' }));
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  await userEvent.click(missing);
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
});

it('ignores late key responses and resets provider on key changes; Gemini permits three stores', async () => {
  let resolve!: (value: typeof stores) => void;
  const pending = new Promise<typeof stores>((done) => { resolve = done; });
  const geminiStores = { items: [1, 2, 3].map((id) => ({ id: `fileSearchStores/${id}`, name: `Gemini ${id}` })) };
  setup({ listCloudStores: async (id) => id === 'openai' ? pending : geminiStores });
  const key = await screen.findByLabelText('API key');
  await screen.findByRole('option', { name: 'OpenAI work' });
  await userEvent.selectOptions(key, 'openai');
  await userEvent.selectOptions(key, 'gemini');
  await screen.findByRole('checkbox', { name: 'Gemini 1' });
  await act(async () => resolve(stores));
  expect(screen.queryByRole('checkbox', { name: 'Archive' })).not.toBeInTheDocument();
  for (const id of [1, 2, 3]) await userEvent.click(screen.getByRole('checkbox', { name: `Gemini ${id}` }));
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  expect(screen.getByLabelText('Default provider')).toHaveValue('');
});

it('keeps dirty drafts stale until reloaded after a detail refresh', async () => {
  let saved = library;
  const client = fixtureClient({ getForum: async () => ({ ...forumDetailFixture, library: saved }),
    listApiKeys: async () => keys, listCloudStores: async () => stores, listProviders: async () => [] });
  const renderAt = (epoch: number) => <DetailRefreshProvider value={{ epoch, refreshing: false, failed: false, stale: false, retry() {} }}>
    <ForumLibraryScreen client={client} state={state} dispatch={vi.fn()} navigate={vi.fn()} />
  </DetailRefreshProvider>;
  const view = render(renderAt(0));
  await userEvent.click(await screen.findByRole('checkbox', { name: 'Archive' }));
  saved = { ...library, store_ids: ['vs_b'] };
  view.rerender(renderAt(1));
  await screen.findByText('This item changed. Load the new values to save.');
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  await userEvent.click(screen.getByRole('button', { name: 'Load new values' }));
  await waitFor(() => expect(screen.getByRole('checkbox', { name: 'Research (vs_b)' })).toBeChecked());
  expect(screen.getByRole('checkbox', { name: 'Archive' })).not.toBeChecked();
});

it('fills an existing catalogue once, keeps empty entries, and reuses entries for provider edits', async () => {
  const getCloudStore = vi.fn<ChaClient['getCloudStore']>(async (_key, id) => ({
    id, name: '', description: '', created_at: '',
  }));
  const { updateForumLibrary } = setup({ getCloudStore }, library);
  await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  await waitFor(() => expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled());
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(getCloudStore).toHaveBeenCalledExactlyOnceWith('openai', 'vs_a');
  expect(updateForumLibrary).toHaveBeenLastCalledWith('lobby', { ...library, catalogue: { vs_a: { name: '', description: '' } } });
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  await userEvent.selectOptions(screen.getByLabelText('Default provider'), 'sol');
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(getCloudStore).toHaveBeenCalledTimes(1);
  expect(updateForumLibrary).toHaveBeenLastCalledWith('lobby', { ...library, provider_id: 'sol', catalogue: { vs_a: { name: '', description: '' } } });
});

it('reads only newly selected stores and prunes removed entries', async () => {
  const getCloudStore = vi.fn<ChaClient['getCloudStore']>(async (_key, id) => ({ id, name: 'New', description: 'Full text', created_at: '' }));
  const saved = { ...library, catalogue: { vs_a: { name: 'Saved', description: 'Original' }, vs_removed: { name: 'Old', description: '' } } };
  const { updateForumLibrary } = setup({ getCloudStore }, saved);
  await userEvent.click(await screen.findByRole('checkbox', { name: 'Research (vs_b)' }));
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(getCloudStore).toHaveBeenCalledExactlyOnceWith('openai', 'vs_b');
  expect(updateForumLibrary).toHaveBeenLastCalledWith('lobby', { ...saved, store_ids: ['vs_a', 'vs_b'], catalogue: {
    vs_a: saved.catalogue.vs_a, vs_b: { name: 'New', description: 'Full text' },
  } });
});

it('saves settings after a failed read and keeps Save enabled for a retry', async () => {
  const getCloudStore = vi.fn<ChaClient['getCloudStore']>().mockRejectedValueOnce(new Error('secret provider detail'))
    .mockResolvedValue({ id: 'vs_a', name: 'Read', description: 'Success', created_at: '' });
  const { updateForumLibrary } = setup({ getCloudStore }, library);
  await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  await screen.findByText('Library saved. Could not read Research (vs_a).');
  expect(updateForumLibrary).toHaveBeenLastCalledWith('lobby', library);
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  expect(screen.queryByText('secret provider detail')).not.toBeInTheDocument();
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(getCloudStore).toHaveBeenCalledTimes(2);
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
});

it('uses the saved key identity, and switching away and back can restore a clean draft', async () => {
  const getCloudStore = vi.fn<ChaClient['getCloudStore']>(async (_key, id) => ({ id, name: 'New key', description: '', created_at: '' }));
  const { updateForumLibrary } = setup({ getCloudStore }, { ...library, catalogue: { vs_a: { name: 'Old key', description: '' } } });
  await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  await userEvent.selectOptions(screen.getByLabelText('API key'), 'gemini');
  await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  await userEvent.selectOptions(screen.getByLabelText('API key'), 'openai');
  await userEvent.click(await screen.findByRole('checkbox', { name: 'Research (vs_a)' }));
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
  expect(getCloudStore).not.toHaveBeenCalled();
  await userEvent.selectOptions(screen.getByLabelText('API key'), 'gemini');
  await userEvent.click(await screen.findByRole('checkbox', { name: 'Research (vs_a)' }));
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(getCloudStore).toHaveBeenCalledExactlyOnceWith('gemini', 'vs_a');
  expect(updateForumLibrary).toHaveBeenLastCalledWith('lobby', { ...library, api_key_id: 'gemini', catalogue: { vs_a: { name: 'New key', description: '' } } });
});

it.each([false, true])('captures the Forum across navigation and rejects a changed vault (changed=%s)', async (changed) => {
  let epoch = 1;
  let finish!: (value: Awaited<ReturnType<ChaClient['getCloudStore']>>) => void;
  const getCloudStore = vi.fn<ChaClient['getCloudStore']>(() => new Promise((resolve) => { finish = resolve; }));
  const { updateForumLibrary, unmount } = setup({ getCloudStore, contextEpoch: () => epoch }, library);
  await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(screen.getByLabelText('API key')).toBeDisabled();
  expect(screen.getByLabelText('Default provider')).toBeDisabled();
  expect(screen.getByRole('checkbox', { name: 'Research (vs_a)' })).toBeDisabled();
  expect(screen.getByRole('button', { name: 'Saving…' })).toBeDisabled();
  if (changed) epoch++;
  else unmount();
  await act(async () => finish({ id: 'vs_a', name: 'Name', description: 'Text', created_at: '' }));
  if (changed) {
    await screen.findByText('The vault changed. Start again in the current vault.');
    expect(updateForumLibrary).not.toHaveBeenCalled();
  } else {
    expect(updateForumLibrary).toHaveBeenCalledExactlyOnceWith('lobby', { ...library, catalogue: { vs_a: { name: 'Name', description: 'Text' } } });
  }
});

it('sends no further store read after the vault changes between reads', async () => {
  let epoch = 1;
  let finish!: (value: Awaited<ReturnType<ChaClient['getCloudStore']>>) => void;
  const getCloudStore = vi.fn<ChaClient['getCloudStore']>(() => new Promise((resolve) => { finish = resolve; }));
  const { updateForumLibrary } = setup({ getCloudStore, contextEpoch: () => epoch }, { ...library, store_ids: ['vs_a', 'vs_b'] });
  await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(getCloudStore).toHaveBeenCalledExactlyOnceWith('openai', 'vs_a');
  epoch++;
  await act(async () => finish({ id: 'vs_a', name: 'Name', description: 'Text', created_at: '' }));
  await screen.findByText('The vault changed. Start again in the current vault.');
  expect(getCloudStore).toHaveBeenCalledTimes(1);
  expect(updateForumLibrary).not.toHaveBeenCalled();
});

it('does not mark a missing catalogue dirty during a detail refresh', async () => {
  let saved = library;
  const client = fixtureClient({ getForum: async () => ({ ...forumDetailFixture, library: saved }),
    listApiKeys: async () => keys, listCloudStores: async () => stores, listProviders: async () => [] });
  const renderAt = (epoch: number) => <DetailRefreshProvider value={{ epoch, refreshing: false, failed: false, stale: false, retry() {} }}>
    <ForumLibraryScreen client={client} state={state} dispatch={vi.fn()} navigate={vi.fn()} />
  </DetailRefreshProvider>;
  const view = render(renderAt(0));
  await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  saved = { ...library, catalogue: { vs_a: { name: 'Filled elsewhere', description: '' } } };
  view.rerender(renderAt(1));
  await waitFor(() => expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled());
  expect(screen.queryByText('This item changed. Load the new values to save.')).not.toBeInTheDocument();
});

it('reads missing stores sequentially and publishes the catalogue only after the final write succeeds', async () => {
  const pending: ((value: Awaited<ReturnType<ChaClient['getCloudStore']>>) => void)[] = [];
  const getCloudStore = vi.fn<ChaClient['getCloudStore']>(() => new Promise((resolve) => { pending.push(resolve); }));
  const updateForumLibrary = vi.fn<ChaClient['updateForumLibrary']>().mockRejectedValueOnce(new Error('write failed'))
    .mockImplementation(async (_id, library) => ({ ...forumDetailFixture, library }));
  const { dispatch } = setup({ getCloudStore, updateForumLibrary }, { ...library, store_ids: ['vs_a', 'vs_b'] });
  await screen.findByRole('checkbox', { name: 'Research (vs_a)' });
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(getCloudStore).toHaveBeenCalledTimes(1);
  expect(updateForumLibrary).not.toHaveBeenCalled();
  await act(async () => pending[0]({ id: 'vs_a', name: 'A', description: 'Text A', created_at: '' }));
  expect(getCloudStore).toHaveBeenNthCalledWith(2, 'openai', 'vs_b');
  expect(updateForumLibrary).not.toHaveBeenCalled();
  await act(async () => pending[1]({ id: 'vs_b', name: 'B', description: 'Text B', created_at: '' }));
  await screen.findByText('Library could not be saved.');
  expect(dispatch).not.toHaveBeenCalled();
  expect(screen.getByRole('button', { name: 'Save' })).toBeEnabled();
  expect(screen.getByRole('checkbox', { name: 'Research (vs_a)' })).toBeChecked();
  await userEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(getCloudStore).toHaveBeenNthCalledWith(3, 'openai', 'vs_a');
  await act(async () => pending[2]({ id: 'vs_a', name: 'A', description: 'New text A', created_at: '' }));
  await act(async () => pending[3]({ id: 'vs_b', name: 'B', description: 'New text B', created_at: '' }));
  expect(updateForumLibrary).toHaveBeenLastCalledWith('lobby', { ...library, store_ids: ['vs_a', 'vs_b'], catalogue: {
    vs_a: { name: 'A', description: 'New text A' }, vs_b: { name: 'B', description: 'New text B' },
  } });
  expect(dispatch).toHaveBeenCalledWith(expect.objectContaining({ type: 'forum-updated' }));
  expect(screen.getByRole('button', { name: 'Save' })).toBeDisabled();
});
