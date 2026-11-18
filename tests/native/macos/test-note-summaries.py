#!/usr/bin/env python3
"""Run note summaries in the native host against a local fake provider."""
from collections import Counter
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import plistlib
import shutil
import sqlite3
import subprocess
import tempfile
import threading
import time
import zlib

REPOSITORY = Path(__file__).resolve().parents[3]
PROMPTS = {
    'overview': 'You write the overview at the start of a summary of a book or of a set of',
    'section': 'You write one section of a summary of a book or of a set of texts.',
    'l2': 'You make a short, structured version of a text for a human reader.',
}
ANSWERS = {
    'l1': 'L1 result',
    'l2': '- L2 result\n\n| Key | Value |\n| --- | --- |\n| Color | Blue |',
    'section': 'Section result',
}
SOURCE_MARKERS = ('SINGLE_ORIGINAL', 'FIRST_CHAPTER', 'SECOND_CHAPTER', 'EXCLUDED', 'SMALL')
# Compression, L1, L2, and L3 sections use this system prompt and send their own prompt after the note.
SOURCE_SYSTEM_PROMPT = 'The user message contains a note.'


def folder_summaries(database):
    return database.execute('''SELECT id, parent_id, name, content_json
        FROM note_items WHERE folder_summary=1''').fetchall()


def build_host(root, contents):
    executable = contents / 'MacOS/CHASummaryTest'
    executable.parent.mkdir(parents=True)
    (contents / 'Frameworks').mkdir()
    runtime = REPOSITORY / 'build/ninja/libChaRuntime.dylib'
    shutil.copy2(runtime, contents / 'Frameworks' / runtime.name)
    with (contents / 'Info.plist').open('wb') as file:
        plistlib.dump(dict(CFBundleExecutable=executable.name,
                          CFBundleIdentifier='com.michaelpopov.cha.summarytest',
                          CFBundleName='CHASummaryTest', CFBundlePackageType='APPL',
                          LSUIElement=True), file)
    # The existing instrumentation calls normal NSApp termination.
    subprocess.run(['xcrun', 'swiftc', '-swift-version', '5', '-parse-as-library', '-O',
                    '-D', 'CHA_NATIVE_INSTRUMENTATION', '-module-cache-path', str(root / 'modules'),
                    '-framework', 'AppKit', '-framework', 'WebKit',
                    '-import-objc-header', str(REPOSITORY / 'packaging/host/runtime_bridge.h'),
                    '-L', str(runtime.parent), '-lChaRuntime', '-Xlinker', '-rpath',
                    '-Xlinker', '@executable_path/../Frameworks',
                    *[str(REPOSITORY / path) for path in [
                        'packaging/macos/main.swift', 'packaging/macos/feasibility.swift',
                        'packaging/macos/native_bridge.swift', 'tests/native/macos/persistence_probe.swift']],
                    '-o', str(executable)], check=True)
    return executable


def main():
    with tempfile.TemporaryDirectory(prefix='cha-summaries-') as directory:
        root = Path(directory)
        database_path = root / 'test.sqlite3'
        contents = root / 'CHASummaryTest.app/Contents'
        assets = contents / 'Resources/web'
        shutil.copytree(os.environ.get('CHA_NATIVE_ASSETS', REPOSITORY / 'webapp/dist'), assets)
        executable = build_host(root, contents)
        probe = root / 'summaries.js'
        probe.write_text((REPOSITORY / 'tests/native/note-summaries.js').read_text()
                         + '\nawait nativeParity(mode);\nreturn "quit";\n')
        requests = Counter()
        chat_requests = []
        system_errors = []
        first_summary = []
        second_summary = []
        cancelled = threading.Event()
        release_first = threading.Event()
        stop_watching = threading.Event()
        request_lock = threading.Lock()

        class Provider(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                messages = body['messages']
                if body['model'] == 'fake':
                    chat_requests.append('\n'.join(message['content'] for message in messages))
                    release_first.set()
                    answer = 'Attachment received.'
                else:
                    assert messages[0]['role'] == 'system', messages[0]['role']
                    system = messages[0]['content']
                    source = messages[-1]['content']
                    # The default L1 prompt is a copy of the compression prompt, so a Compress step counts as L1.
                    part = 'overview' if system.startswith(PROMPTS['overview']) else next(
                        (part for part in ('section', 'l2') if PROMPTS[part] in source), 'l1')
                    if part != 'overview' and not system.startswith(SOURCE_SYSTEM_PROMPT):
                        system_errors.append(system[:80])
                    # Chat Completions can wrap the source in a speaker prefix.
                    marker = 'overview' if part == 'overview' else next(
                        (marker for marker in SOURCE_MARKERS if marker in source), 'unknown')
                    with request_lock:
                        requests[part, marker] += 1
                        count = requests[part, marker]
                        if (part == 'section'
                                and requests[part, 'FIRST_CHAPTER'] == requests[part, 'SECOND_CHAPTER'] == 3
                                and requests[part, 'EXCLUDED'] == 1):
                            (assets / 'summary-request-held').touch()
                    if part == 'l1' and marker == 'SINGLE_ORIGINAL' and count == 1:
                        (assets / 'single-request-held').touch()
                        if not release_first.wait(20):
                            return
                    if part == 'l1' and marker == 'SINGLE_ORIGINAL' and count == 2:
                        self.send_response(503)
                        self.send_header('Content-Length', '0')
                        self.end_headers()
                        self.wfile.flush()
                        # Give the worker time to enter its first 10-second wait.
                        time.sleep(0.2)
                        (assets / 'retry-backoff').touch()
                        return
                    hold = part == 'section' and marker == 'FIRST_CHAPTER' and count == 3
                    if (part == 'overview' and count == 2) or hold:
                        with closing(sqlite3.connect(database_path)) as database:
                            snapshot = folder_summaries(database)
                        (second_summary if hold else first_summary).extend(snapshot)
                    if hold:
                        # Wait for native cancellation to close the provider socket.
                        self.connection.settimeout(20)
                        if self.connection.recv(1) == b'':
                            cancelled.set()
                        return
                    answer = f'Overview result {count}' if part == 'overview' else ANSWERS[part]
                    if part == 'l1' and marker == 'SINGLE_ORIGINAL' and count == 3:
                        answer = 'L1 result between steps'
                result = json.dumps({'choices': [{'message': {'content': answer}, 'finish_reason': 'stop'}]}).encode()
                try:
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(result)))
                    self.end_headers()
                    self.wfile.write(result)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        with ThreadingHTTPServer(('127.0.0.1', 0), Provider) as server:
            subprocess.run([str(REPOSITORY / 'build/ninja/cha_prepare_test_vault'),
                            '--provider-port', str(server.server_port), str(root)], check=True)
            with closing(sqlite3.connect(database_path)) as database, database:
                provider_path = 'system/providers/test/config.toml'
                updated = database.execute('UPDATE config SET content=? WHERE name=?', (
                    f'display_name = "Test"\nmode = "net"\nhost = "127.0.0.1"\nport = {server.server_port}\n'
                    'https = false\napi = "chat_completions"\nmodel = "test"\nstream = false\n', provider_path))
                assert updated.rowcount == 1, 'Test provider missing'
                database.execute('INSERT INTO config VALUES (?,?)', ('system/notes/config.toml',
                    'compression_provider = "test"\ncompression_reasoning_effort = "none"\n'))

                def note(parent, name, text, excluded=False):
                    document = {'type': 'doc', 'content': [
                        {'type': 'paragraph', 'content': [{'type': 'text', 'text': text}]}]}
                    return database.execute('''INSERT INTO note_items
                        (parent_id,kind,name,content_json,summary_excluded) VALUES (?,'note',?,?,?)''',
                        (parent, name, zlib.compress(json.dumps(document).encode()), excluded)).lastrowid

                original = 'SINGLE_ORIGINAL ' + 'x' * 4096
                assert note(None, 'Single', original) == 1, 'Update the chat reference in note-summaries.js'
                folder = database.execute("INSERT INTO note_items(kind,name) VALUES ('folder','Book')").lastrowid
                first_chapter = note(folder, 'A First chapter', 'FIRST_CHAPTER ' + 'x' * 4096)
                note(folder, 'B Second chapter', 'SECOND_CHAPTER ' + 'x' * 4096)
                note(folder, 'C Excluded', 'EXCLUDED ' + 'x' * 4096, excluded=True)
                note(folder, 'D Small', 'SMALL')
                foreground = note(None, 'Foreground', 'Foreground original')

            threading.Thread(target=server.serve_forever, daemon=True).start()

            def watch_finishes():
                with closing(sqlite3.connect(database_path)) as database:
                    seen = 0
                    while not stop_watching.wait(0.05):
                        if not database.execute("SELECT 1 FROM sqlite_master WHERE name='event_history'").fetchone():
                            continue
                        count = database.execute("""SELECT COUNT(*) FROM event_history
                            WHERE message LIKE 'Summarization finished for %'""").fetchone()[0]
                        for number in range(seen + 1, count + 1):
                            (assets / f'batch-finished-{number}').touch()
                        seen = count

            watcher = threading.Thread(target=watch_finishes)
            watcher.start()
            environment = dict(os.environ, CHA_PERSISTENCE_CONFIG=str(root),
                CHA_PERSISTENCE_SCRIPT=str(probe), CHA_PERSISTENCE_EXPECT='{}')

            def launch(mode):
                result = subprocess.run([str(executable)], cwd=REPOSITORY,
                    env=dict(environment, CHA_PERSISTENCE_MODE=mode),
                    capture_output=True, text=True, timeout=60)
                assert result.returncode == 0, result.stdout + result.stderr
                assert 'PASS persistence probe' in result.stdout, result.stdout + result.stderr

            try:
                launch('summarize')
                with closing(sqlite3.connect(database_path)) as database:
                    versions = database.execute('''SELECT n.name, v.level
                        FROM note_items n JOIN note_items v ON v.parent_id=n.id
                        WHERE v.compressed=1 AND v.level IN (1,2) ORDER BY n.name, v.level''').fetchall()
                    assert versions == [(name, level) for name in ('A First chapter', 'B Second chapter', 'C Excluded', 'Single')
                                        for level in (1, 2)], versions
                    # C was included again just before quit. Its completed versions survive cancellation.
                    compressed = database.execute('''SELECT n.name FROM note_items n
                        JOIN note_items v ON v.parent_id=n.id
                        WHERE v.compressed=1 AND v.level=0 ORDER BY n.name''').fetchall()
                    assert compressed == [('A First chapter',), ('B Second chapter',), ('C Excluded',)], compressed
                    summary = folder_summaries(database)
                    assert database.execute('SELECT 1 FROM event_history WHERE severity=? AND message=?', (
                        'info', f'L3 section cancelled for “A First chapter” (note {first_chapter}).')).fetchone(), (
                        'The vault did not record cancellation of the held request')
                    lifecycle = database.execute("""SELECT message FROM event_history
                        WHERE severity='info' AND message LIKE 'Summarization %' ORDER BY id""").fetchall()
                    book = f'“Book” (folder {folder})'
                    assert [row[0] for row in lifecycle] == [
                        'Summarization started for “Single” (note 1), 1 note.',
                        'Summarization finished for “Single” (note 1).',
                        f'Summarization started for {book}, 2 notes.',
                        f'Summarization finished for {book}.',
                        f'Summarization started for {book}, 2 notes.',
                        f'Summarization finished for {book}.',
                        f'Summarization started for {book}, 3 notes.',
                        f'Summarization cancelled for {book}.',
                    ], lifecycle
                    foreground_document = database.execute('SELECT content_json FROM note_items WHERE id=?',
                                                         (foreground,)).fetchone()[0]
                    assert b'Foreground edit' in zlib.decompress(foreground_document)
                    single_l1 = database.execute('SELECT content_json FROM note_items WHERE parent_id=1 AND level=1').fetchone()
                assert len(first_summary) == len(second_summary) == len(summary) == 1, (
                    len(first_summary), len(second_summary), len(summary))
                assert first_summary[0][:3] == second_summary[0][:3] == (summary[0][0], folder, 'Summary')
                assert first_summary[0][3] != second_summary[0][3], 'Second run did not replace the Summary content'
                assert summary == second_summary, 'Cancelled run changed the Summary note'
                assert cancelled.wait(2), 'Held provider request was not cancelled'
                launch('reopen')
                expected = Counter({(part, marker): 1 for part in ('l1', 'l2')
                                    for marker in ('SINGLE_ORIGINAL', 'FIRST_CHAPTER', 'SECOND_CHAPTER')})
                # Each chapter's Compress step in the first folder run.
                expected.update({('l1', 'FIRST_CHAPTER'): 1, ('l1', 'SECOND_CHAPTER'): 1,
                                 ('l1', 'EXCLUDED'): 2, ('l2', 'EXCLUDED'): 1})
                expected.update({('section', 'FIRST_CHAPTER'): 3, ('section', 'SECOND_CHAPTER'): 3,
                                 ('section', 'EXCLUDED'): 1,
                                 ('overview', 'overview'): 2})
                assert requests == expected, requests
                launch('retry')
                launch('between-steps')
                launch('final-commit')
                launch('reopen-final')
                expected.update({('l1', 'SINGLE_ORIGINAL'): 2, ('overview', 'overview'): 1})
                expected.update(('section', marker) for marker in ('FIRST_CHAPTER', 'SECOND_CHAPTER', 'EXCLUDED'))
                assert requests == expected, requests
                with closing(sqlite3.connect(database_path)) as database:
                    assert database.execute('SELECT content_json FROM note_items WHERE parent_id=1 AND level=1').fetchone() == single_l1
                    terminal = [row[0] for row in database.execute("""SELECT message FROM event_history
                        WHERE message LIKE 'Summarization cancelled for %' ORDER BY id""")]
                    assert terminal == [f'Summarization cancelled for {book}.',
                        'Summarization cancelled for “Single” (note 1).',
                        'Summarization cancelled for “Single” (note 1).',
                        f'Summarization cancelled for {book}.'], terminal
                    assert database.execute("""SELECT COUNT(*) FROM event_history
                        WHERE message = 'L1 cancelled for “Single” (note 1).'""").fetchone() == (1,)
                assert not system_errors, system_errors
                assert len(chat_requests) == 1, len(chat_requests)
                assert original in chat_requests[0], 'Chat provider did not receive the original note'
                assert 'L1 result' not in chat_requests[0] and 'L2 result' not in chat_requests[0]
                print('Verified foreground use, saved versions, and batch cancellation through normal quit and reopen.')
            except (subprocess.CalledProcessError, AssertionError):
                print('Summary requests:', requests, flush=True)
                with closing(sqlite3.connect(database_path)) as database:
                    print('Vault events:', database.execute(
                        'SELECT severity, message FROM event_history ORDER BY id').fetchall(), flush=True)
                raise
            finally:
                stop_watching.set()
                watcher.join()
                server.shutdown()


if __name__ == '__main__':
    main()
