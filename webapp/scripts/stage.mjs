import { cp, mkdir, rename, rm, stat } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const project = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repository = resolve(project, '..');
const arguments_ = process.argv.slice(2);

let root = resolve(repository, 'bin');
for (let index = 0; index < arguments_.length; index += 1) {
  if (arguments_[index] !== '--root' || index + 1 >= arguments_.length) {
    throw new Error('Usage: npm run stage -- [--root <application-directory>]');
  }
  root = resolve(arguments_[++index]);
}

const source = resolve(project, 'dist');
const destination = resolve(root, 'web');
const temporary = resolve(root, `.web-stage-${process.pid}`);
const backup = resolve(root, `.web-previous-${process.pid}`);

await stat(resolve(source, 'index.html'));
await mkdir(root, { recursive: true });
await rm(temporary, { recursive: true, force: true });
await cp(source, temporary, { recursive: true });

let movedPrevious = false;
try {
  try {
    await rename(destination, backup);
    movedPrevious = true;
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }
  await rename(temporary, destination);
  if (movedPrevious) await rm(backup, { recursive: true, force: true });
} catch (error) {
  await rm(temporary, { recursive: true, force: true });
  if (movedPrevious) await rename(backup, destination);
  throw error;
}
