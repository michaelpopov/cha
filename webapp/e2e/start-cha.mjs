import { spawn } from 'node:child_process';
import { rmSync } from 'node:fs';
import { cp, lstat, mkdir, mkdtemp, rm, stat, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repository = resolve(project, '..');
const publishedWorkspace = process.env.CHA_E2E_WORKSPACE
  ? resolve(process.env.CHA_E2E_WORKSPACE)
  : null;
const publishedDatabase = process.env.CHA_E2E_DATABASE
  ? resolve(process.env.CHA_E2E_DATABASE)
  : null;
const databaseArtifacts = publishedDatabase
  ? [
      publishedDatabase,
      `${publishedDatabase}-wal`,
      `${publishedDatabase}-shm`,
      `${publishedDatabase}-journal`,
      `${publishedDatabase}.cha-lock`,
    ]
  : [];
for (const [label, path] of [
  ['workspace', publishedWorkspace],
  ...databaseArtifacts.map((path) => ['database artifact', path]),
]) {
  if (!path) continue;
  try {
    await lstat(path);
    throw new Error(`Refusing to replace an existing E2E ${label}: ${path}`);
  } catch (error) {
    if (error?.code !== 'ENOENT') throw error;
  }
}
if (publishedWorkspace) {
  await mkdir(dirname(publishedWorkspace), { recursive: true });
}
if (publishedDatabase) {
  await mkdir(dirname(publishedDatabase), { recursive: true });
}
const temporary = await mkdtemp(resolve(tmpdir(), 'cha-webapp-e2e-'));
let child;
let cleaned = false;
function cleanupFiles() {
  if (cleaned) return;
  cleaned = true;
  if (publishedWorkspace) rmSync(publishedWorkspace, { recursive: true, force: true });
  for (const path of databaseArtifacts) rmSync(path, { force: true });
  rmSync(temporary, { recursive: true, force: true });
}
process.once('exit', () => {
  if (child?.exitCode === null) child.kill('SIGTERM');
  cleanupFiles();
});
const packagedApplication = process.env.CHA_E2E_APPLICATION_ROOT
  ? resolve(process.env.CHA_E2E_APPLICATION_ROOT)
  : null;
const application = packagedApplication ?? resolve(temporary, 'application');
const executable = packagedApplication
  ? resolve(application, 'chaweb')
  : resolve(repository, 'build/ninja/chaweb');
const workspace = publishedWorkspace ?? resolve(temporary, 'workspace');
const database = publishedDatabase ?? resolve(temporary, 'cha.sqlite3');
const apiPort = Number(process.env.CHA_E2E_PORT ?? '8080');
const modelPort = apiPort + 2;

await cp(resolve(project, 'e2e/fixtures/workspace'), workspace, {
  recursive: true,
  force: false,
  errorOnExist: true,
});

// A local OpenAI-shaped stream is slow enough for the browser to observe and
// stop an active generation, while remaining deterministic and offline.
const modelServer = createServer((request, response) => {
  if (request.method !== 'POST' || request.url !== '/v1/chat/completions') {
    response.writeHead(404).end();
    return;
  }
  const chunks = [];
  request.on('data', (chunk) => chunks.push(chunk));
  request.on('end', () => {
    let content = 'Deterministic browser-test response.';
    try {
      const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
      const last = Array.isArray(body.messages) ? body.messages.at(-1) : undefined;
      if (last && typeof last.content === 'string') content = last.content;
    } catch {
      response.writeHead(400).end();
      return;
    }

    response.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache',
      Connection: 'keep-alive',
    });
    const pieces = content.match(/[\s\S]{1,12}/g) ?? [''];
    let index = 0;
    let timer;
    const writeNext = () => {
      if (response.destroyed) return;
      if (index === pieces.length) {
        response.end('data: [DONE]\n\n');
        return;
      }
      response.write(`data: ${JSON.stringify({
        choices: [{ delta: { content: pieces[index] } }],
      })}\n\n`);
      index += 1;
      timer = setTimeout(writeNext, 30);
    };
    response.on('close', () => clearTimeout(timer));
    writeNext();
  });
});
// The mock server's port is the one setting that cannot be committed, so the
// harness rewrites the named provider selected by the character definition.
await writeFile(
  resolve(workspace, 'system/providers/test/config.toml'),
  `host = "127.0.0.1"
port = ${modelPort}
mode = "net"
model = "browser-test"
api = "chat_completions"
web_search = "off"
stream = true
https = false
`,
);

// The real bundle, not a placeholder: the served suite loads the application
// from this server with no development server anywhere in the path.
if (packagedApplication) {
  for (const required of [
    'chaweb',
    'import-seed/app.toml',
    'start-cha.sh',
    'web/index.html',
  ]) {
    try {
      await stat(resolve(application, required));
    } catch {
      throw new Error(`Assembled application is missing ${required}: ${application}`);
    }
  }
} else {
  const bundle = resolve(project, 'dist');
  try {
    await stat(resolve(bundle, 'index.html'));
  } catch {
    throw new Error(`No browser build at ${bundle}; run 'npm run build' first.`);
  }
  await mkdir(application, { recursive: true });
  await cp(bundle, resolve(application, 'web'), { recursive: true });
}

const importer = spawn(executable, [
  '--data', database,
  '--import', workspace,
], { stdio: 'inherit' });
const importCode = await new Promise((resolveExit, reject) => {
  importer.once('error', reject);
  importer.once('exit', (code, signal) => {
    resolveExit(code ?? (signal ? 1 : 0));
  });
});
if (importCode !== 0) {
  throw new Error(`Offline import failed with exit code ${importCode}`);
}
await rm(workspace, { recursive: true, force: true });

await new Promise((resolveListen, reject) => {
  modelServer.once('error', reject);
  modelServer.listen(modelPort, '127.0.0.1', resolveListen);
});

child = spawn(executable, [
  '--root', application,
  '--data', database,
  '--host', '127.0.0.1',
  '--port', String(apiPort),
  // Short enough to exercise an actual unload in a browser test, but longer
  // than the client's first reconnect delay so ordinary recovery tests do not
  // race the test-only lifecycle setting.
  '--test-idle-grace-ms', '1000',
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

await new Promise((resolveClose) => modelServer.close(resolveClose));
if (publishedWorkspace) await rm(publishedWorkspace, { recursive: true, force: true });
for (const path of databaseArtifacts) await rm(path, { force: true });
await rm(temporary, { recursive: true, force: true });
cleaned = true;
process.exitCode = exitCode;
