import { spawn } from 'node:child_process';
import { cp, mkdir, mkdtemp, rm, stat, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repository = resolve(project, '../../..');
const executable = resolve(repository, 'build/ninja/chaweb');
const temporary = await mkdtemp(resolve(tmpdir(), 'cha-webapp-e2e-'));
const application = resolve(temporary, 'application');
const workspace = resolve(temporary, 'workspace');
const apiPort = Number(process.env.CHA_E2E_PORT ?? '8080');
const modelPort = apiPort + 2;

await cp(resolve(project, 'e2e/fixtures/workspace'), workspace, { recursive: true });

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
await new Promise((resolveListen, reject) => {
  modelServer.once('error', reject);
  modelServer.listen(modelPort, '127.0.0.1', resolveListen);
});

const provider = `[provider]
host = "127.0.0.1"
port = ${modelPort}
mode = "net"
model = "browser-test"
stream = true
https = false

[logging]
file = "logs/cha.log"
level = "off"
`;
await writeFile(resolve(workspace, 'workspace.toml'), provider);
await writeFile(
  resolve(workspace, 'characters/guide/character.toml'),
  `display_name = "Guide"
description = "A deterministic test character"
host = "127.0.0.1"
port = ${modelPort}
mode = "net"
model = "browser-test"
stream = true
https = false
`,
);
await writeFile(
  resolve(workspace, 'forums/lobby/members/character_defaults.toml'),
  `host = "127.0.0.1"
port = ${modelPort}
mode = "net"
model = "browser-test"
stream = true
https = false
`,
);

// The real bundle, not a placeholder: the served suite loads the application
// from this server with no development server anywhere in the path.
const bundle = resolve(project, 'dist');
try {
  await stat(resolve(bundle, 'index.html'));
} catch {
  throw new Error(`No browser build at ${bundle}; run 'npm run build' first.`);
}
await mkdir(application, { recursive: true });
await cp(bundle, resolve(application, 'web'), { recursive: true });

const child = spawn(executable, [
  '--root', application,
  '--workspace', workspace,
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
await rm(temporary, { recursive: true, force: true });
process.exitCode = exitCode;
