import { useEffect, useRef, useState } from 'react';
import { ChaError, ChaProtocolError, publicErrorMessage,
  type AudioDownloadRequest, type AudioDownloadStatus, type ChaClient } from './api/client';

function sameStatus(left: AudioDownloadStatus | null, right: AudioDownloadStatus): boolean {
  return left !== null && left.cached_entry_ids.length === right.cached_entry_ids.length
    && left.cached_entry_ids.every((id, index) => id === right.cached_entry_ids[index])
    && left.downloads.length === right.downloads.length && left.downloads.every((job, index) => {
      const other = right.downloads[index];
      return job.entry_id === other.entry_id && job.state === other.state && job.error === other.error;
    });
}

// Stopping a screen observer never stops core jobs.
export function useAudioDownloads(client: ChaClient, forum: string | undefined,
  session: string | undefined, vault: string | undefined, clearCount: number) {
  const [status, setStatus] = useState<AudioDownloadStatus | null>(null);
  const controller = useRef<{ submit(id: number, request: AudioDownloadRequest): Promise<void>;
    refresh(missingEntryId?: number): void; clear(): void } | null>(null);
  const observedClear = useRef(clearCount);
  useEffect(() => {
    setStatus(null);
    if (!forum || !session || !vault) return;
    let current = true;
    let timer: ReturnType<typeof setTimeout> | undefined;
    let reading = false;
    let mutations = 0;
    let validity: { current: boolean } | null = null;
    const admissions = new Set<{ current: boolean }>();
    const pending = new Set<number>();
    let initialized = false;
    let refreshNeeded = false;
    function invalidate() {
      if (validity) validity.current = false;
      clearTimeout(timer);
      refreshNeeded = true;
    }
    function schedule() {
      clearTimeout(timer);
      if (current && mutations === 0 && (!initialized || pending.size > 0)) timer = setTimeout(() => void read(), 1000);
    }
    async function read() {
      if (!current || mutations > 0) return;
      if (reading) { refreshNeeded = true; return; }
      reading = true;
      refreshNeeded = false;
      const valid = { current: true };
      validity = valid;
      try {
        const result = await client.getAudioDownloads(forum!, session!, vault!);
        if (current && valid.current) {
          initialized = true;
          pending.clear();
          for (const job of result.downloads) if (job.state !== 'failed') pending.add(job.entry_id);
          setStatus((old) => sameStatus(old, result) ? old : result);
        }
      } catch (failure) {
        // Maintenance and transport failures can recover. A stale vault,
        // missing session, denied access, or incompatible API cannot.
        if (current && valid.current && (failure instanceof ChaProtocolError
          || (failure instanceof ChaError && failure.status >= 400 && failure.status < 500))) {
          initialized = true;
          pending.clear();
          setStatus((old) => ({ cached_entry_ids: [], downloads: (old?.downloads ?? []).map((job) =>
            job.state === 'failed' ? job : { ...job, state: 'failed',
              error: publicErrorMessage(failure, 'Audio status is unavailable. Try again.') }) }));
        }
      }
      finally {
        reading = false;
        if (current && mutations === 0) {
          if (refreshNeeded) void read();
          else schedule();
        }
      }
    }
    const observer = {
      async submit(id: number, request: AudioDownloadRequest) {
        invalidate();
        mutations += 1;
        const acceptedValid = { current: true };
        admissions.add(acceptedValid);
        pending.add(id);
        setStatus((old) => ({ cached_entry_ids: old?.cached_entry_ids ?? [],
          downloads: [...(old?.downloads ?? []).filter((job) => job.entry_id !== id), { entry_id: id, state: 'queued' }] }));
        try {
          const accepted = await client.startAudioDownload(forum!, session!, id, request);
          if (current && acceptedValid.current) {
            if (accepted.cached) pending.delete(id);
            setStatus((old) => ({
              cached_entry_ids: accepted.cached ? [...new Set([...(old?.cached_entry_ids ?? []), id])] : old?.cached_entry_ids ?? [],
              downloads: [...(old?.downloads ?? []).filter((job) => job.entry_id !== id),
                ...(accepted.cached ? [] : [{ entry_id: id, state: accepted.state! }])],
            }));
          }
        } catch (failure) {
          if (current && acceptedValid.current) {
            // Show admission errors at the page level only. A fresh status
            // read can still discover work accepted before disconnect.
            pending.delete(id);
            setStatus((old) => ({ cached_entry_ids: old?.cached_entry_ids ?? [],
              downloads: (old?.downloads ?? []).filter((job) => job.entry_id !== id) }));
            throw failure;
          }
        } finally {
          admissions.delete(acceptedValid);
          mutations -= 1;
          if (current && mutations === 0) void read();
        }
      },
      refresh(missingEntryId?: number) {
        invalidate();
        if (missingEntryId !== undefined) setStatus((old) => old && ({ ...old,
          cached_entry_ids: old.cached_entry_ids.filter((id) => id !== missingEntryId) }));
        if (mutations === 0) void read();
      },
      clear() {
        for (const valid of admissions) valid.current = false;
        setStatus(null);
        pending.clear();
        initialized = false;
        observer.refresh();
      },
    };
    controller.current = observer;
    void read();
    return () => {
      current = false;
      invalidate();
      if (controller.current === observer) controller.current = null;
    };
  }, [client, forum, session, vault]);
  useEffect(() => {
    if (observedClear.current !== clearCount) {
      observedClear.current = clearCount;
      controller.current?.clear();
    }
  }, [clearCount]);
  return { status,
    submit: (id: number, request: AudioDownloadRequest) => controller.current?.submit(id, request),
    refresh: (missingEntryId?: number) => controller.current?.refresh(missingEntryId) };
}
