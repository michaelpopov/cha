#!/usr/bin/env python3
"""Exercise the real native host with a disposable vault and a fake gem_audio.

Build webapp/dist, cha_macos_runtime, and cha_prepare_test_vault first.
On Windows, build CHA/CHATest and cha_prepare_test_vault instead.
"""
import base64
from contextlib import closing, contextmanager
import io
import json
import math
import os
from pathlib import Path
import sqlite3
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


REPOSITORY = Path(__file__).resolve().parents[3]


@contextmanager
def temporary_vault():
    directory = tempfile.TemporaryDirectory(prefix="cha-note-audio-")
    try:
        yield directory.name
    finally:
        try:
            directory.cleanup()
        except PermissionError:
            if sys.platform != "win32":
                raise
            print("WebView2 profile cleanup deferred: Windows still holds a cache directory.", flush=True)


def main():
    records = []
    requests = []
    submitted = []

    class Service(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def respond(self, data, content_type="application/json", headers=None):
            if isinstance(data, dict):
                data = json.dumps(data).encode()
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            for name, value in (headers or {}).items():
                self.send_header(name, value)
            self.end_headers()
            try:
                self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass  # Stop can close an in-flight local download.

        def do_POST(self):
            requests.append(self.path)
            data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            if self.path == "/upload/v1beta/files":
                assert self.headers.get("x-goog-api-key") == "fake-audio-key"
                self.respond({}, headers={"x-goog-upload-url": f"http://127.0.0.1:{self.server.server_port}/secret"})
            elif self.path == "/secret":
                assert self.headers.get("x-goog-api-key") is None
                records[:] = [json.loads(line) for line in data.splitlines()]
                assert all(record["request"]["generationConfig"]["speechConfig"]["voiceConfig"]["voice"] == "Kore" for record in records)
                self.respond({"file": {"name": "files/" + "a" * 32}})
            elif self.path.endswith(":batchGenerateContent"):
                text = records[0]["request"]["contents"][0]["parts"][0]["text"]
                submitted.append(text)
                if text == "Fail audio":
                    self.respond({"name": "batches/" + "b" * 32, "done": True, "error": {"message": "fake failure"}})
                    return
                self.respond({"name": "batches/" + "b" * 32, "done": True,
                              "response": {"responsesFile": "files/" + "c" * 32}})
            else:
                self.send_error(404)

        def do_GET(self):
            requests.append(self.path)
            # Keep work active long enough to check modality and UI repaint.
            current = list(records)
            text = current[0]["request"]["contents"][0]["parts"][0]["text"]
            time.sleep(4 if text == "Stop audio" else 1.4)
            lines = []
            for index, record in enumerate(current):
                frames = int((0.6 if index == 0 else 2) * 24000)
                data = io.BytesIO()
                with wave.open(data, "wb") as audio:
                    audio.setparams((1, 2, 24000, frames, "NONE", "not compressed"))
                    audio.writeframes(b"".join(struct.pack("<h", int(1500 * math.sin(
                        2 * math.pi * (300 + index * 120) * frame / 24000))) for frame in range(frames)))
                lines.append(json.dumps({"key": record["key"], "response": {"candidates": [{"content": {
                    "parts": [{"inlineData": {"mimeType": "audio/wav", "data": base64.b64encode(data.getvalue()).decode()}}]
                }}]}}))
            self.respond(("\n".join(reversed(lines)) + "\n").encode(), "application/jsonl")

    with temporary_vault() as directory:
        windows = sys.platform == "win32"
        root = Path(directory) / "config" if windows else Path(directory)
        root.mkdir(exist_ok=True)
        subprocess.run([str(REPOSITORY / ("build/ninja/cha_prepare_test_vault.exe" if windows else "build/ninja/cha_prepare_test_vault")), str(root)], check=True)
        server = ThreadingHTTPServer(("127.0.0.1", 0), Service)
        with closing(sqlite3.connect(root / "test.sqlite3")) as database, database:
            database.execute("INSERT INTO config VALUES (?,?)", ("system/keys/api_key_1/config.toml",
                'type = "models"\ndisplay_name = "Audio test"\nvalue = "fake-audio-key"\n'))
            database.execute("INSERT INTO config VALUES (?,?)", ("system/notes/config.toml",
                f'compression_reasoning_effort = "high"\naudio_url = "http://127.0.0.1:{server.server_port}"\naudio_api_key = "api_key_1"\n'))
            document = {"type": "doc", "content": [
                {"type": "heading", "attrs": {"level": 1}, "content": [{"type": "text", "text": "Audio check"}]},
                {"type": "paragraph", "content": [{"type": "text", "text": "First paragraph with audio."}]},
                {"type": "paragraph", "content": [{"type": "text", "text": "Second paragraph with audio."}]},
            ]}
            database.execute("INSERT INTO note_items (kind,name,content_json,read_only) VALUES ('note','Audio check',?,1)",
                             (zlib.compress(json.dumps(document).encode()),))
            def folder(name):
                return database.execute("INSERT INTO note_items (kind,name) VALUES ('folder',?)", (name,)).lastrowid

            def note(parent, name, text, read_only=True, kind="note"):
                document = {"type": "doc", "content": [{"type": "paragraph", "content": [{"type": "text", "text": text}]}] if text else []}
                return database.execute("INSERT INTO note_items (parent_id,kind,name,content_json,read_only) VALUES (?,?,?,?,?)",
                    (parent, kind, name, zlib.compress(json.dumps(document).encode()), read_only)).lastrowid

            success = folder("Folder success")
            note(success, "A first", "Folder first")
            note(success, "B empty", "")
            note(success, "C oversized", "x" * 5001)
            note(success, "D last", "Folder last")
            note(success, "E editable", "Do not generate", False)
            existing = note(success, "F existing", "Already generated")
            note(success, "ANNOTATIONS", "Do not generate", kind="annotation")
            nested = database.execute("INSERT INTO note_items (parent_id,kind,name) VALUES (?,'folder','Subfolder')", (success,)).lastrowid
            note(nested, "Nested", "Do not generate")
            stopped = folder("Folder stop")
            note(stopped, "A stopping", "Stop audio")
            note(stopped, "B remaining", "Do not generate")
            failed = folder("Folder error")
            note(failed, "A saved", "Before error")
            note(failed, "B failing", "Fail audio")
            note(failed, "C remaining", "Do not generate")
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        source = REPOSITORY / "tests/native/note-audio.js"
        environment = dict(os.environ, CHA_NATIVE_TEST_CONFIG=str(root.parent if windows else root),
                           CHA_NATIVE_TEST_HOST=str(root / "AudioTestHost.app"), CHA_NATIVE_PARITY_SCRIPT=str(source))

        def run_host():
            command = ["powershell", "-ExecutionPolicy", "Bypass", "-File", "tests/native/windows/run.ps1", "-Expect", "audio"] if windows else ["tests/native/macos/run.sh", "parity", "--timeout-ms", "45000"]
            subprocess.run(command, cwd=REPOSITORY, env=environment, check=True)

        try:
            run_host()
            assert len(requests) == 4, requests
            assert [record["request"]["contents"][0]["parts"][0]["text"] for record in records] == [
                "Audio check", "First paragraph with audio.", "Second paragraph with audio."]
            with closing(sqlite3.connect(root / "test.sqlite3")) as database, database:
                database.execute("INSERT INTO note_audio SELECT ?,ordinal,mime_type,audio FROM note_audio WHERE note_id=(SELECT id FROM note_items WHERE name='Audio check')", (existing,))
            folder_source = root / "folder.js"
            folder_source.write_text(source.read_text() + "\nnativeParity = nativeFolderAudio;\n")
            environment["CHA_NATIVE_PARITY_SCRIPT"] = str(folder_source)
            run_host()
            assert submitted == ["Audio check", "Folder first", "Folder last", "Stop audio", "Before error", "Fail audio"], submitted
            with closing(sqlite3.connect(root / "test.sqlite3")) as database, database:
                saved = database.execute("SELECT name,COUNT(*) FROM note_audio JOIN note_items ON note_id=id GROUP BY note_id ORDER BY name").fetchall()
                assert saved == [("A first", 1), ("A saved", 1), ("Audio check", 3), ("D last", 1), ("F existing", 3)], saved
            server.shutdown()
            server.server_close()
            thread.join()
            # Restart the host with the same vault and no running service.
            offline_source = root / "offline.js"
            offline_source.write_text(source.read_text() + "\nconst runAudioProbe = nativeParity;\nnativeParity = () => runAudioProbe({ offline: true });\n")
            environment["CHA_NATIVE_PARITY_SCRIPT"] = str(offline_source)
            run_host()
            print("PASS folder success/skip/Stop/error/focus, single-note generation, native M4A playback, and offline restart", flush=True)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
