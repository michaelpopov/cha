#!/usr/bin/env python3
"""Run note summaries in the native host against a local fake provider."""
from collections import Counter
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
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


def folder_summaries(database):
    return database.execute('''SELECT id, parent_id, name, content_json
        FROM note_items WHERE folder_summary=1''').fetchall()


def main():
    with tempfile.TemporaryDirectory(prefix='cha-summaries-') as directory:
        root = Path(directory)
        database_path = root / 'test.sqlite3'
        assets = root / 'assets'
        shutil.copytree(os.environ.get('CHA_NATIVE_ASSETS', REPOSITORY / 'webapp/dist'), assets)
        requests = Counter()
        chat_requests = []
        first_summary = []
        second_summary = []
        cancelled = threading.Event()
        request_lock = threading.Lock()

        class Provider(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                messages = body['messages']
                if body['model'] == 'fake':
                    chat_requests.append('\n'.join(message['content'] for message in messages))
                    answer = 'Attachment received.'
                else:
                    assert messages[0]['role'] == 'system', messages[0]['role']
                    system = messages[0]['content']
                    part = next((part for part, prompt in PROMPTS.items() if system.startswith(prompt)), 'l1')
                    source = messages[-1]['content']
                    # Chat Completions can wrap the source in a speaker prefix.
                    marker = 'overview' if part == 'overview' else next(
                        (marker for marker in SOURCE_MARKERS if marker in source), 'unknown')
                    with request_lock:
                        requests[part, marker] += 1
                        count = requests[part, marker]
                    hold = part == 'section' and marker == 'FIRST_CHAPTER' and count == 3
                    if (part == 'overview' and count == 2) or hold:
                        with closing(sqlite3.connect(database_path)) as database:
                            snapshot = folder_summaries(database)
                        (second_summary if hold else first_summary).extend(snapshot)
                    if hold:
                        # Signal the browser through the native host's asset loader.
                        (assets / 'summary-request-held').touch()
                        # Wait for native cancellation to close the provider socket.
                        self.connection.settimeout(20)
                        if self.connection.recv(1) == b'':
                            cancelled.set()
                        return
                    # Keep the per-note progress visible long enough for the UI probe.
                    time.sleep(0.25)
                    answer = f'Overview result {count}' if part == 'overview' else ANSWERS[part]
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

            threading.Thread(target=server.serve_forever, daemon=True).start()
            environment = dict(os.environ, CHA_NATIVE_TEST_CONFIG=str(root),
                CHA_NATIVE_ASSETS=str(assets),
                CHA_NATIVE_TEST_HOST=str(root / 'SummariesTestHost.app'),
                CHA_NATIVE_PARITY_SCRIPT=str(REPOSITORY / 'tests/native/note-summaries.js'))
            try:
                subprocess.run(['tests/native/macos/run.sh', 'parity', '--timeout-ms', '45000'],
                               cwd=REPOSITORY, env=environment, check=True)
                with closing(sqlite3.connect(database_path)) as database:
                    versions = database.execute('''SELECT n.name, v.level
                        FROM note_items n JOIN note_items v ON v.parent_id=n.id
                        WHERE v.compressed=1 AND v.level IN (1,2) ORDER BY n.name, v.level''').fetchall()
                    assert versions == [(name, level) for name in ('A First chapter', 'B Second chapter', 'Single')
                                        for level in (1, 2)], versions
                    summary = folder_summaries(database)
                    assert database.execute('SELECT 1 FROM event_history WHERE severity=? AND message=?', (
                        'info', f'L3 section cancelled for “A First chapter” (note {first_chapter}).')).fetchone(), (
                        'The vault did not record cancellation of the held request')
                assert len(first_summary) == len(second_summary) == len(summary) == 1, (
                    len(first_summary), len(second_summary), len(summary))
                assert first_summary[0][:3] == second_summary[0][:3] == (summary[0][0], folder, 'Summary')
                assert first_summary[0][3] != second_summary[0][3], 'Second run did not replace the Summary content'
                assert summary == second_summary, 'Cancelled run changed the Summary note'
                assert cancelled.wait(2), 'Held provider request was not cancelled'
                expected = Counter({(part, marker): 1 for part in ('l1', 'l2')
                                    for marker in ('SINGLE_ORIGINAL', 'FIRST_CHAPTER', 'SECOND_CHAPTER')})
                expected.update({('section', 'FIRST_CHAPTER'): 3, ('section', 'SECOND_CHAPTER'): 2,
                                 ('overview', 'overview'): 2})
                assert requests == expected, requests
                assert len(chat_requests) == 1, len(chat_requests)
                assert original in chat_requests[0], 'Chat provider did not receive the original note'
                assert 'L1 result' not in chat_requests[0] and 'L2 result' not in chat_requests[0]
                print('Verified saved versions, Summary identity and cancellation, provider counts, and chat attachment.')
            except (subprocess.CalledProcessError, AssertionError):
                print('Summary requests:', requests, flush=True)
                with closing(sqlite3.connect(database_path)) as database:
                    print('Vault events:', database.execute(
                        'SELECT severity, message FROM event_history ORDER BY id').fetchall(), flush=True)
                raise
            finally:
                server.shutdown()


if __name__ == '__main__':
    main()
