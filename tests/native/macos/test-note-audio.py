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
    records = {}
    lock = threading.Lock()
    first_release = threading.Event()
    first_waiting = threading.Event()
    requests = []
    submitted = []
    batches = {}
    outputs = {}

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
                # Fail before any remote batch exists. The accepted native job
                # must retry this upload and complete without another UI start.
                if len(requests) == 1:
                    self.send_response(503)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                with lock:
                    token = f"{len(records) + 1:032x}"
                    records["files/" + token] = []
                self.respond({}, headers={"x-goog-upload-url": f"http://127.0.0.1:{self.server.server_port}/secret/{token}"})
            elif self.path.startswith("/secret/"):
                assert self.headers.get("x-goog-api-key") is None
                current = [json.loads(line) for line in data.splitlines()]
                assert all(record["request"]["generationConfig"]["speechConfig"]["voiceConfig"]["voice"] == "Kore" for record in current)
                name = "files/" + self.path.rsplit("/", 1)[1]
                with lock:
                    records[name] = current
                self.respond({"file": {"name": name}})
            elif self.path.endswith(":batchGenerateContent"):
                with lock:
                    current = records[json.loads(data)["batch"]["inputConfig"]["fileName"]]
                    text = current[0]["request"]["contents"][0]["parts"][0]["text"]
                    submitted.append(text)
                    batch_id = "batches/" + f"{len(batches) + 1:032x}"
                    output_id = "files/" + f"{len(batches) + 1:032x}"
                    operation = {"name": batch_id, "done": True}
                    if text == "Fail audio":
                        operation["error"] = {"message": "fake failure"}
                    else:
                        operation["response"] = {"responsesFile": output_id}
                    batches[batch_id] = operation
                    outputs[output_id] = current
                self.respond(operation)
            else:
                self.send_error(404)

        def do_GET(self):
            requests.append(self.path)
            if self.path.startswith("/v1beta/batches/"):
                self.respond(batches[self.path.removeprefix("/v1beta/")])
                return
            # Keep A stalled until the UI has observed B's completion and exits.
            output = self.path.removeprefix("/download/v1beta/").split(":download")[0]
            current = outputs[output]
            text = current[0]["request"]["contents"][0]["parts"][0]["text"]
            if text == "Folder first":
                first_waiting.set()
                assert first_release.wait(20), "B did not finish independently"
            else:
                time.sleep(1.4)
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
            failed = folder("Folder error")
            note(failed, "A saved", "Before error")
            note(failed, "B failing", "Fail audio")
            note(failed, "C remaining", "After error")
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
            assert len(requests) == 5, requests
            assert requests[:2] == ["/upload/v1beta/files"] * 2, requests
            assert [record["request"]["contents"][0]["parts"][0]["text"] for record in next(iter(records.values()))] == [
                "Audio check", "First paragraph with audio.", "Second paragraph with audio."]
            with closing(sqlite3.connect(root / "test.sqlite3")) as database, database:
                database.execute("INSERT INTO note_audio SELECT ?,ordinal,mime_type,audio FROM note_audio WHERE note_id=(SELECT id FROM note_items WHERE name='Audio check')", (existing,))
            # Reopen a registered attempt against the same saved remote batch.
            # Play another note while it completes to check affected-note delivery.
            with closing(sqlite3.connect(root / "test.sqlite3")) as database, database:
                original = database.execute("SELECT content_json FROM note_items WHERE name='Audio check'").fetchone()[0]
                recovery_id = database.execute("INSERT INTO note_items(kind,name,content_json,read_only) VALUES ('note','Recovery check',?,1)", (original,)).lastrowid
                database.execute("INSERT INTO note_audio_attempts(note_id,note_name,clip_count,service_origin,api_key_id,batch_id) VALUES (?,?,?,?,?,?)",
                    (recovery_id, "Recovery check", 3, f"http://127.0.0.1:{server.server_port}", "api_key_1", next(iter(batches))))
                assert database.execute("SELECT severity FROM event_history ORDER BY id").fetchall() == [("info",), ("success",)]
                database.execute("INSERT INTO event_history(recorded_at,severity,message) VALUES (?,'info',?)",
                    (int(time.time()), f'Audio batch started for “Recovery check” (note {recovery_id}): 3 clips, voice Kore.'))
            recovery_source = root / "recovery.js"
            recovery_source.write_text(source.read_text() + "\nconst playExisting = nativeParity;\nnativeParity = () => playExisting({ offline: true });\n")
            environment["CHA_NATIVE_PARITY_SCRIPT"] = str(recovery_source)
            post_count = sum(path.startswith(("/upload/", "/secret", "/v1beta/models/")) for path in requests)
            run_host()
            assert sum(path.startswith(("/upload/", "/secret", "/v1beta/models/")) for path in requests) == post_count, requests
            assert any(path.startswith("/v1beta/batches/") for path in requests), requests
            with closing(sqlite3.connect(root / "test.sqlite3")) as database:
                assert database.execute("SELECT COUNT(*) FROM note_audio_attempts").fetchone()[0] == 0
                assert database.execute("SELECT COUNT(*) FROM note_audio WHERE note_id=?", (recovery_id,)).fetchone()[0] == 3
                assert database.execute("SELECT COUNT(*) FROM event_history WHERE severity='success'").fetchone()[0] == 2
                assert database.execute("SELECT COUNT(*) FROM event_history WHERE severity='info'").fetchone()[0] == 2
            folder_source = root / "folder.js"
            folder_source.write_text(source.read_text() + "\nnativeParity = nativeFolderAudio;\n")
            environment["CHA_NATIVE_PARITY_SCRIPT"] = str(folder_source)
            try:
                run_host()
            finally:
                first_release.set()
            assert first_waiting.is_set(), "A never reached its download gate"
            with closing(sqlite3.connect(root / "test.sqlite3")) as database:
                saved = database.execute("SELECT DISTINCT name FROM note_audio JOIN note_items ON note_id=id WHERE name IN ('A first','D last')").fetchall()
                assert saved == [("D last",)], saved
                assert database.execute("SELECT note_name FROM note_audio_attempts").fetchall() == [("A first",)]
            # Reopen A's saved batch after B completed. Inspect the database only
            # with the native process stopped, using the UI for live observations.
            folder_source.write_text(source.read_text() + "\nnativeParity = () => nativeFolderAudio({ recover: true });\n")
            post_count = len(submitted)
            run_host()
            assert len(submitted) == post_count, submitted
            assert sorted(submitted) == sorted(["Audio check", "Folder first", "Folder last", "Before error", "Fail audio", "After error"]), submitted
            with closing(sqlite3.connect(root / "test.sqlite3")) as database, database:
                saved = database.execute("SELECT name,COUNT(*) FROM note_audio JOIN note_items ON note_id=id GROUP BY note_id ORDER BY name").fetchall()
                assert saved == [("A first", 1), ("A saved", 1), ("Audio check", 3), ("C remaining", 1), ("D last", 1), ("F existing", 3), ("Recovery check", 3)], saved
                assert database.execute("SELECT COUNT(*) FROM event_history").fetchone()[0] == 16
                assert database.execute("SELECT COUNT(*) FROM note_audio_attempts").fetchone()[0] == 0
            server.shutdown()
            server.server_close()
            thread.join()
            # Restart the host with the same vault and no running service.
            offline_source = root / "offline.js"
            offline_source.write_text(source.read_text() + "\nconst runAudioProbe = nativeParity;\nnativeParity = () => runAudioProbe({ offline: true });\n")
            environment["CHA_NATIVE_PARITY_SCRIPT"] = str(offline_source)
            run_host()
            print("PASS background acceptance, automatic submission retry after HTTP 503, registered restart without POST, unrelated playback, independent folder completion/skip/error/focus, and offline playback", flush=True)
        finally:
            server.shutdown()
            server.server_close()
            thread.join()


if __name__ == "__main__":
    main()
