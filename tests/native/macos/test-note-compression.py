#!/usr/bin/env python3
"""Run background compression in the real native host with a delayed provider."""
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading
import zlib

REPOSITORY = Path(__file__).resolve().parents[3]


def main():
    requests = []

    class Provider(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            text = body['messages'][-1]['content']
            requests.append(text)
            if len(requests) == 1 or 'SLOW_FOLDER' in text:
                # Hold the response until native cancellation closes the socket.
                # A test-only bound prevents a failed probe from hanging cleanup.
                self.connection.settimeout(20)
                assert self.connection.recv(1) == b'', 'Expected provider cancellation'
                return
            result = json.dumps({'choices': [{'message': {'content': 'Compressed result'}, 'finish_reason': 'stop'}]}).encode()
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(result)))
                self.end_headers()
                self.wfile.write(result)
            except (BrokenPipeError, ConnectionResetError):
                pass

    with tempfile.TemporaryDirectory(prefix='cha-compression-') as directory:
        root = Path(directory)
        subprocess.run([str(REPOSITORY / 'build/ninja/cha_prepare_test_vault'), str(root)], check=True)
        server = ThreadingHTTPServer(('127.0.0.1', 0), Provider)
        with closing(sqlite3.connect(root / 'test.sqlite3')) as database, database:
            provider_path = 'system/providers/test/config.toml'
            provider = database.execute('SELECT content FROM config WHERE name=?', (provider_path,)).fetchone()
            assert provider, 'Test provider missing'
            # Replace the provider fixture with a local endpoint, without credentials.
            database.execute('UPDATE config SET content=? WHERE name=?', (
                f'display_name = "Test"\nmode = "net"\nhost = "127.0.0.1"\nport = {server.server_port}\nhttps = false\napi = "chat_completions"\nmodel = "test"\nstream = false\n', provider_path))
            database.execute('INSERT INTO config VALUES (?,?)', ('system/notes/config.toml', 'compression_provider = "test"\ncompression_reasoning_effort = "none"\n'))

            def note(parent, name, text):
                document = {'type': 'doc', 'content': [{'type': 'paragraph', 'content': [{'type': 'text', 'text': text + 'x' * 4096}]}]}
                database.execute("INSERT INTO note_items(parent_id,kind,name,content_json) VALUES (?,'note',?,?)", (parent, name, zlib.compress(json.dumps(document).encode())))

            note(None, 'Single', 'SINGLE')
            folder = database.execute("INSERT INTO note_items(kind,name) VALUES ('folder','Batch')").lastrowid
            nested = database.execute("INSERT INTO note_items(parent_id,kind,name) VALUES (?,'folder','Nested folder')", (folder,)).lastrowid
            note(folder, 'A slow', 'SLOW_FOLDER')
            note(nested, 'B nested', 'FAST_NESTED')
        threading.Thread(target=server.serve_forever, daemon=True).start()
        environment = dict(os.environ, CHA_NATIVE_TEST_CONFIG=str(root), CHA_NATIVE_TEST_HOST=str(root / 'CompressionTestHost.app'),
            CHA_NATIVE_PARITY_SCRIPT=str(REPOSITORY / 'tests/native/note-compression.js'))
        try:
            subprocess.run(['tests/native/macos/run.sh', 'parity', '--timeout-ms', '45000'], cwd=REPOSITORY, env=environment, check=True)
            with closing(sqlite3.connect(root / 'test.sqlite3')) as database:
                saved = database.execute('SELECT name FROM note_items WHERE compressed_text IS NOT NULL ORDER BY name').fetchall()
                assert saved == [('B nested',), ('Single',)], saved
                events = database.execute('SELECT severity FROM event_history ORDER BY id').fetchall()
                assert sorted(events) == sorted([('info',)] * 6 + [('success',)] * 2), events
            assert len(requests) == 4, requests
        finally:
            server.shutdown()
            server.server_close()


if __name__ == '__main__':
    main()
