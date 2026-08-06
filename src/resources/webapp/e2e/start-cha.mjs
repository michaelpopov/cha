import { spawn } from 'node:child_process';
import { cp, mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repository = resolve(project, '../../..');
const executable = resolve(repository, 'build/ninja/chaweb');
const temporary = await mkdtemp(resolve(tmpdir(), 'cha-webapp-e2e-'));
const application = resolve(temporary, 'application');
const workspace = resolve(temporary, 'workspace');

await cp(resolve(project, 'e2e/fixtures/workspace'), workspace, { recursive: true });
await mkdir(resolve(application, 'web/assets'), { recursive: true });
await writeFile(
  resolve(application, 'web/index.html'),
  '<!doctype html><html><body>e2e shell</body></html>\n',
);

const child = spawn(executable, [
  '--root', application,
  '--workspace', workspace,
  '--host', '127.0.0.1',
  '--port', '8080',
], { stdio: 'inherit' });

let stopping = false;
async function stop(signal = 'SIGTERM') {
  if (stopping) return;
  stopping = true;
  if (child.exitCode === null) child.kill(signal);
}

process.on('SIGINT', () => void stop('SIGINT'));
process.on('SIGTERM', () => void stop('SIGTERM'));

const exitCode = await new Promise((resolveExit, reject) => {
  child.once('error', reject);
  child.once('exit', (code, signal) => {
    resolveExit(code ?? (signal ? 1 : 0));
  });
});

await rm(temporary, { recursive: true, force: true });
process.exitCode = exitCode;
