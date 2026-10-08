import { useState } from 'react';

import type { VaultUploadCheck } from '../api/client';
import { ConfirmDialog } from '../components/ConfirmDialog';
import { PasswordDialog } from '../components/PasswordDialog';
import { DownloadIcon, FileUpIcon, MergeIcon, WrenchIcon } from '../components/Icons';
import { ChaWebError, chaWebMessage, type ChaWebBootstrap, type ChaWebClient } from './client';

type Operation = 'upload' | 'download' | 'merge';

export function VaultActions({ bootstrap, client, blocked, onBusy, onRefresh, onOpenAssistant }: {
  bootstrap: ChaWebBootstrap;
  client: ChaWebClient;
  blocked: boolean;
  onBusy(busy: boolean): void;
  onRefresh(): Promise<void>;
  onOpenAssistant(): void;
}) {
  const [pending, setPending] = useState<Operation | 'check' | null>(null);
  const [confirmation, setConfirmation] = useState<'upload' | 'download' | null>(null);
  const [uploadCheck, setUploadCheck] = useState<VaultUploadCheck | null>(null);
  const [passwordPrompt, setPasswordPrompt] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const disabled = blocked || pending !== null || !bootstrap.capabilities?.can_transfer_r2;

  function begin(operation: Operation | 'check') {
    setPending(operation);
    onBusy(true);
    setError(null);
  }

  function finish() {
    setPending(null);
    onBusy(false);
  }

  async function run(operation: Operation, check?: VaultUploadCheck, password?: string) {
    setConfirmation(null);
    setPasswordPrompt(false);
    begin(operation);
    try {
      if (operation === 'upload') await client.uploadVault(check!);
      else if (operation === 'download') await client.downloadVault();
      else await client.mergeParentVault(password);
    } catch (failure) {
      if (operation === 'merge' && failure instanceof ChaWebError
          && failure.code === 'source_vault_password_required') {
        setPasswordPrompt(true);
        setError(password ? 'The parent vault password was not accepted.' : null);
      } else {
        setError(chaWebMessage(failure, 'The vault operation failed.'));
      }
      finish();
      return;
    }
    if (operation !== 'upload') {
      try {
        await onRefresh();
      } catch (failure) {
        setError(chaWebMessage(failure, 'The session list could not be refreshed. Reload this page.'));
      }
    }
    finish();
  }

  async function requestUpload() {
    if (disabled) return;
    begin('check');
    try {
      const check = await client.checkVaultUpload();
      setUploadCheck(check);
      if (check.status === 'match') {
        await run('upload', check);
        return;
      }
      setConfirmation('upload');
    } catch (failure) {
      setError(chaWebMessage(failure, 'The upload check failed.'));
    }
    finish();
  }

  const status = pending === 'check' ? 'Checking…'
    : pending === 'upload' ? 'Uploading…'
    : pending === 'download' ? 'Downloading…'
    : pending === 'merge' ? 'Downloading and merging parent…' : null;

  return (
    <>
      <div aria-label="Vault actions" className="chaweb-vault-actions" role="group">
        <button aria-label="Upload" className="chaweb-icon-button" disabled={disabled}
          onClick={() => void requestUpload()} title="Upload" type="button"><FileUpIcon /></button>
        <button aria-label="Download" className="chaweb-icon-button" disabled={disabled}
          onClick={() => setConfirmation('download')} title="Download" type="button"><DownloadIcon /></button>
        <button aria-label="Parent merge" className="chaweb-icon-button"
          disabled={disabled || !bootstrap.vault_parent} onClick={() => void run('merge')}
          title="Parent merge" type="button"><MergeIcon /></button>
        <button aria-label="Assistant" className="chaweb-icon-button"
          disabled={blocked || pending !== null} onClick={onOpenAssistant}
          title="Assistant" type="button"><WrenchIcon /></button>
      </div>
      {status && <p className="chaweb-status" role="status">{status}</p>}
      {error && !passwordPrompt && <p className="chaweb-alert" role="alert">{error}</p>}
      {confirmation === 'upload' && uploadCheck && <ConfirmDialog
        className="chaweb-delete-dialog" confirmLabel="Upload" initialFocus="cancel"
        message={uploadCheck.status === 'missing'
          ? `No R2 version is recorded for “${bootstrap.vault_name}”. Upload and overwrite any existing R2 vault?`
          : uploadCheck.etag === null
          ? `The R2 copy of “${bootstrap.vault_name}” no longer exists. Upload it again?`
          : `The R2 version of “${bootstrap.vault_name}” has changed. Upload and overwrite it?`}
        onCancel={() => setConfirmation(null)} onConfirm={() => void run('upload', uploadCheck)}
        title="Upload vault?"
      />}
      {confirmation === 'download' && <ConfirmDialog
        className="chaweb-delete-dialog" confirmLabel="Download" initialFocus="cancel"
        message={`Download “${bootstrap.vault_name}” from R2 and replace its local database? CHA will save the current database with a .bac suffix and reload the vault.`}
        onCancel={() => setConfirmation(null)} onConfirm={() => void run('download')}
        title="Download vault?"
      />}
      {passwordPrompt && <PasswordDialog
        className="chaweb-delete-dialog" error={error} name={bootstrap.vault_parent ?? 'parent vault'}
        onCancel={() => { setPasswordPrompt(false); setError(null); }}
        onSubmit={(password) => void run('merge', undefined, password)}
      />}
    </>
  );
}
