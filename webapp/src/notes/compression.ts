import { ChaError, type NotesClient } from '../api/client';

export type CompressionCancellation = {
  requested: boolean;
  cancelJob?: () => void;
};

function contextEnded(failure: unknown) {
  return failure instanceof ChaError
    && ['vault_changed', 'application_unavailable', 'server_stopping'].includes(failure.code);
}

export async function compressAndWait(
  client: NotesClient, id: number, epoch: number, folder: boolean,
  cancellation: CompressionCancellation, context: AbortSignal,
  onCancelFailed: () => void,
) {
  let queued = false;
  let checkStatus = () => { queued = true; };
  let retire = () => {};
  const changed = (event: Event) => {
    const notice = (event as CustomEvent<{ id: number; epoch: number }>).detail;
    if (notice.id === id && notice.epoch === epoch) checkStatus();
  };
  window.addEventListener('cha-note-compression-changed', changed);
  try {
    const result = await client.compressNote(id, epoch, folder);
    if ('skipped' in result) return;
    await new Promise<void>((resolve, reject) => {
      const earlyNotice = queued;
      let checking = false;
      let cancelling = false;
      let finished = false;
      function fail(failure: unknown) {
        finished = true;
        reject(failure);
      }
      function finish() {
        finished = true;
        resolve();
      }
      checkStatus = async () => {
        queued = true;
        if (checking) return;
        checking = true;
        while (queued && !cancelling && !finished) {
          queued = false;
          try {
            const status = await client.compressionStatus(id, epoch);
            if (!status.pending && !cancelling && !finished) finish();
          } catch (failure) {
            if (contextEnded(failure) && !finished) fail(failure);
            // A later notice or Cancel can settle a transient status error.
          }
        }
        checking = false;
      };
      cancellation.cancelJob = async () => {
        if (cancelling || finished) return;
        cancelling = true;
        try {
          await client.cancelCompression(id, epoch);
          finish();
        } catch (failure) {
          if (finished) return;
          if (contextEnded(failure)) fail(failure);
          else {
            cancelling = false;
            onCancelFailed();
            checkStatus();
          }
        }
      };
      retire = () => {
        fail(new ChaError('vault_changed', 'The vault changed. Start again in the current vault.'));
      };
      context.addEventListener('abort', retire, { once: true });
      if (context.aborted) retire();
      else if (cancellation.requested) cancellation.cancelJob();
      else {
        checkStatus();
        if (earlyNotice) checkStatus();
      }
    });
  } finally {
    window.removeEventListener('cha-note-compression-changed', changed);
    cancellation.cancelJob = undefined;
    context.removeEventListener('abort', retire);
  }
}
