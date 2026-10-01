#!/usr/bin/env python3
"""HTTP integration tests for Linux/macOS: make itest-daemon.

Requires nginx on PATH (or --nginx), Python 3, and the two CMake targets
cha-daemon and cha_prepare_test_vault. Uses only temporary test vaults, Unix
sockets and a fake provider on 127.0.0.1; never uses the installed nginx
service or live providers. nginx's own scgi_params and mime.types files are
used, as in a deployment; pass --scgi-params or --mime-types when they are
not beside the nginx configuration.
"""

import argparse
import gzip
import http.client
import http.server
import json
from pathlib import Path
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
# Words in the newest user message that control the fake provider.
HOLD = "HOLD-PROVIDER"
FAIL = "FAIL-PROVIDER"
PROVIDER_REPLY = "Provider reply"


class UnixHTTPConnection(http.client.HTTPConnection):
    """HTTP over the Unix socket that the test nginx listens on."""

    def __init__(self, path, timeout=10):
        super().__init__("localhost", timeout=timeout)
        self.socket_path = path

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.socket_path)


def newest_user_text(body):
    try:
        messages = json.loads(body).get("messages", [])
    except ValueError:
        return ""
    for message in reversed(messages):
        if message.get("role") == "user":
            content = message.get("content", "")
            if isinstance(content, list):
                content = " ".join(part.get("text", "") for part in content
                                   if isinstance(part, dict))
            return content
    return ""


class FakeProvider:
    """Chat Completions provider on 127.0.0.1. It records the newest user
    message of each request. HOLD waits until `released` is set; FAIL returns
    HTTP 500; any other text gets PROVIDER_REPLY."""

    def __init__(self):
        self.prompts = []
        self.changed = threading.Condition()
        self.released = threading.Event()
        provider = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                prompt = newest_user_text(body)
                with provider.changed:
                    provider.prompts.append(prompt)
                    provider.changed.notify_all()
                if HOLD in prompt:
                    provider.released.wait(30)
                if FAIL in prompt:
                    status, reply = 500, b""
                else:
                    status, reply = 200, json.dumps({"choices": [{"message": {
                        "role": "assistant", "content": PROVIDER_REPLY}}]}).encode()
                try:
                    self.send_response(status)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(reply)))
                    self.end_headers()
                    self.wfile.write(reply)
                except (BrokenPipeError, ConnectionResetError):
                    pass  # CHA cancelled this request

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def received(self, text):
        with self.changed:
            return any(text in prompt for prompt in self.prompts)

    def wait_for(self, text, timeout=10):
        with self.changed:
            if not self.changed.wait_for(
                    lambda: any(text in prompt for prompt in self.prompts), timeout):
                raise AssertionError(f"The provider did not receive {text!r}")

    def close(self):
        self.released.set()
        self.server.shutdown()
        self.server.server_close()


class DaemonHarness(unittest.TestCase):
    def setUp(self):
        # macOS has a 104-byte Unix socket path limit; its default TMPDIR is long.
        self.temporary = tempfile.TemporaryDirectory(prefix="cha-it-", dir="/tmp")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.processes = []

    def logs(self):
        return "\n".join(f"{path.name}:\n{path.read_text(errors='replace')}"
                         for path in sorted(self.directory.glob("*.log")))

    def spawn(self, name, command):
        with (self.directory / f"{name}.log").open("ab") as log:
            process = subprocess.Popen(command, stdout=log, stderr=log)
        self.processes.append(process)
        self.addCleanup(self.stop, process)
        return process

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
                raise AssertionError("Process did not stop after SIGTERM")

    def prepare_config(self, directory_name, provider=None):
        config = self.directory / directory_name
        command = [str(self.build_dir / "cha_prepare_test_vault")]
        if provider is not None:
            command += ["--provider-port", str(provider.port)]
        subprocess.run(command + [str(config)], check=True, capture_output=True, timeout=15)
        return config

    def start_daemon(self, name, config=None):
        if config is None:
            config = self.directory / name
            if not config.exists():
                self.prepare_config(name)
        return self.spawn(name, [
            sys.executable, str(ROOT / "scripts/run_daemon.py"),
            str(self.directory / f"{name}.sock"), str(self.build_dir / "cha-daemon"),
            "--config", str(config),
        ])


CHAWEB = "/api/cha/v1"
CHAWEB_ASSETS = ROOT / "webapp" / "dist-chaweb"
CHAWEB_INDEX = CHAWEB_ASSETS / "index.html"
MISSING_CHAWEB_ASSETS = (
    f"missing ChaWeb production build: {CHAWEB_INDEX} does not exist. "
    "Run `npm --prefix webapp run build:chaweb` before this test."
)


class ChaWebIntegration(DaemonHarness):
    """One daemon, one vault, and the shipped ChaWeb nginx block on a Unix socket.

    Static responses come from the production ``webapp/dist-chaweb`` build.
    """

    def setUp(self):
        super().setUp()
        if not CHAWEB_INDEX.is_file():
            self.fail(MISSING_CHAWEB_ASSETS)
        self.assets = CHAWEB_ASSETS.resolve()
        self.daemon_socket = self.directory / "alice.sock"
        self.nginx_socket = str(self.directory / "nginx.sock")
        self.daemon = self.start_daemon("alice")
        self.start_nginx()
        self.wait_ready()

    def start_nginx(self):
        example = (ROOT / "packaging/linux/nginx-chaweb.conf.example").read_text()
        for required in (
            "scgi_request_buffering    on;",
            "scgi_buffering            on;",
            "scgi_ignore_client_abort  off;",
            'add_header Cache-Control "no-store" always;',
            "gzip_types                application/json;",
            "client_max_body_size      256k;",
            "include                   scgi_params;",
            "scgi_param HTTP_X_CHA_AUDIO_OFFSET $http_x_cha_audio_offset;",
        ):
            self.assertIn(required, example)
        self.assertNotIn("location /v1/", example)
        self.assertNotIn("Access-Control", example)
        self.assertNotIn("CHA_CONTEXT", example)
        replacements = (
            ("listen 192.168.1.10:8443 ssl;", f"listen unix:{self.nginx_socket};"),
            ("root /srv/cha/chaweb;", f'root "{self.assets}";'),
            ("unix:/run/cha/alice.sock", f"unix:{self.daemon_socket}"),
        )
        for old, new in replacements:
            self.assertIn(old, example, f"nginx-chaweb.conf.example no longer contains {old!r}")
            example = example.replace(old, new, 1)
        shutil.copy(self.scgi_params, self.directory / "scgi_params")
        config = self.directory / "nginx.conf"
        config.write_text(
            "daemon off;\nmaster_process off;\n"
            f'pid "{self.directory}/nginx.pid";\n'
            "error_log stderr info;\nevents {}\nhttp {\n"
            f'include "{self.mime_types}";\n'
            "access_log off;\n"
            "client_body_temp_path body;\n"
            "proxy_temp_path proxy;\n"
            "fastcgi_temp_path fastcgi;\n"
            "uwsgi_temp_path uwsgi;\n"
            "scgi_temp_path scgi;\n"
            + example + "\n}\n"
        )
        checked = subprocess.run(
            [self.nginx_binary, "-t", "-e", "stderr",
             "-p", str(self.directory) + "/", "-c", str(config)],
            capture_output=True, text=True, timeout=15)
        self.assertEqual(checked.returncode, 0, checked.stderr + checked.stdout)
        self.nginx = self.spawn("nginx", [
            self.nginx_binary, "-e", "stderr", "-p", str(self.directory) + "/",
            "-c", str(config),
        ])

    def wait_ready(self):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            for process in self.processes:
                if process.poll() is not None:
                    self.assertEqual(process.returncode, 0, self.logs())
            try:
                status, _, _ = self.exchange("GET", CHAWEB + "/bootstrap")
                if status == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.05)
        self.fail("nginx/cha-daemon did not become ready\n" + self.logs())

    def use_provider(self):
        """Restarts the daemon with a vault whose Guide uses self.provider."""
        self.provider = FakeProvider()
        try:
            self.stop(self.daemon)
            self.daemon = self.start_daemon(
                "alice", self.prepare_config("alice-net", self.provider))
        except Exception:
            self.provider.close()
            raise
        # Close the provider before the daemon so a held reply cannot block shutdown.
        self.addCleanup(self.provider.close)
        self.wait_ready()

    def exchange(self, method, path, body=None, header_map=None):
        """Returns (status, lowercase headers, raw body). Does not parse JSON."""
        headers = {}
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        if header_map:
            headers.update(header_map)
        connection = UnixHTTPConnection(self.nginx_socket)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            found = {}
            for key, value in response.getheaders():
                key = key.lower()
                found[key] = f"{found[key]}, {value}" if key in found else value
            return response.status, found, response.read()
        finally:
            connection.close()

    def assert_api_error(self, status, raw, expected, code):
        self.assertEqual(status, expected, raw)
        parsed = json.loads(raw)
        self.assertEqual(parsed["error"]["code"], code)
        self.assertTrue(parsed["error"]["message"])
        return parsed

    def bootstrap(self):
        status, headers, raw = self.exchange("GET", CHAWEB + "/bootstrap")
        self.assertEqual(status, 200, raw)
        self.assertIn("application/json", headers.get("content-type", ""))
        return json.loads(raw)

    def forum_id(self):
        if not hasattr(self, "_forum_id"):
            boot = self.bootstrap()
            entrance = boot["entrance_forum_id"]
            matches = [item["id"] for item in boot["forums"] if item["id"] != entrance]
            self.assertTrue(matches, boot["forums"])
            self._forum_id = matches[0]
        return self._forum_id

    def sessions_path(self, forum=None):
        return f"{CHAWEB}/forums/{forum or self.forum_id()}/sessions"

    def session_path(self, session_id, forum=None):
        return self.sessions_path(forum) + "/" + session_id

    def sessions(self):
        status, _, raw = self.exchange("GET", self.sessions_path())
        self.assertEqual(status, 200, raw)
        listed = json.loads(raw)
        self.assertIsInstance(listed, list)
        return listed

    def session_ids(self):
        return [item["id"] for item in self.sessions()]

    def create(self, text):
        status, _, raw = self.exchange("POST", self.sessions_path(), {"text": text})
        self.assertEqual(status, 201, raw)
        created = json.loads(raw)
        self.assertTrue(created["id"])
        self.assertIn("label", created)
        return created

    def snapshot(self, session_id, forum=None):
        status, _, raw = self.exchange("GET", self.session_path(session_id, forum))
        self.assertEqual(status, 200, raw)
        snap = json.loads(raw)
        self.assertEqual(snap["session_id"], session_id)
        self.assertIn("generation", snap)
        self.assertIn("transcript", snap)
        return snap

    @staticmethod
    def texts(snap):
        return [entry.get("text", "") for entry in snap["transcript"]]

    def wait_until(self, predicate, description):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            found = predicate()
            if found:
                return found
            time.sleep(0.05)
        self.fail(description + "\n" + self.logs())

    def wait_idle(self, session_id):
        def check():
            snap = self.snapshot(session_id)
            return snap if not snap["generation"]["active"] else None
        return self.wait_until(check, "generation stayed active")

    def wait_not_live(self, session_id):
        def check():
            for item in self.sessions():
                if item["id"] == session_id and not item["live"]:
                    return item
            return None
        return self.wait_until(check, f"session {session_id} stayed live")

    def wait_for_new_session(self, previous):
        def check():
            extra = [item["id"] for item in self.sessions() if item["id"] not in previous]
            return extra[0] if extra else None
        return self.wait_until(check, "no new session appeared")

    def prompt_count(self, text=None):
        with self.provider.changed:
            if text is None:
                return len(self.provider.prompts)
            return sum(text in prompt for prompt in self.provider.prompts)

    def raw_post(self, path, payload):
        raw = json.dumps(payload).encode()
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(sock.close)
        sock.settimeout(10)
        sock.connect(self.nginx_socket)
        sock.sendall(
            f"POST {path} HTTP/1.1\r\nHost: localhost\r\n"
            "Content-Type: application/json\r\n"
            f"Content-Length: {len(raw)}\r\nConnection: close\r\n\r\n".encode() + raw)
        return sock

    def test_api_path_through_nginx(self):
        boot = self.bootstrap()
        self.assertTrue(boot["entrance_forum_id"])
        self.assertTrue(boot["forums"])
        forum = self.forum_id()
        self.assertNotEqual(forum, boot["entrance_forum_id"])
        created = self.create("@- hello from nginx")
        self.assertIn(created["id"], self.session_ids())
        snap = self.snapshot(created["id"])
        self.assertEqual(snap["forum"]["id"], forum)
        self.assertTrue(any("hello from nginx" in text for text in self.texts(snap)))
        status, _, raw = self.exchange(
            "POST", self.session_path(created["id"]) + "/input", {"text": "@- second note"})
        self.assertEqual(status, 204, raw)
        self.assertEqual(raw, b"")
        status, _, raw = self.exchange(
            "POST", self.session_path(created["id"]) + "/stop", {})
        self.assertEqual(status, 204, raw)
        self.assertEqual(raw, b"")
        snap = self.snapshot(created["id"])
        self.assertTrue(any("second note" in text for text in self.texts(snap)))
        self.assertEqual(self.exchange("GET", "/v1/models")[0], 404)

    def test_audio_chunks_through_nginx(self):
        self.use_provider()
        created = self.create("Hello")
        snapshot = self.wait_idle(created["id"])
        entry = next(item for item in snapshot["transcript"]
                     if item.get("text") == PROVIDER_REPLY)
        audio = bytes(range(256)) * 280
        with sqlite3.connect(self.directory / "alice-net" / "test.sqlite3") as database:
            session_key = database.execute(
                "SELECT session_key FROM sessions WHERE session_id = ?",
                (created["id"],)).fetchone()[0]
            database.execute(
                "INSERT INTO entry_audio VALUES (?, ?, ?, ?)",
                (session_key, entry["id"], audio, "audio/mpeg"))
        path = self.session_path(created["id"]) + f"/entries/{entry['id']}/audio"

        status, headers, first = self.exchange(
            "GET", path, header_map={"X-CHA-Audio-Offset": "0"})
        self.assertEqual(status, 200)
        self.assertEqual(headers["content-type"], "audio/mpeg")
        self.assertEqual(headers["x-cha-audio-complete"], "0")
        self.assertIn("no-store", headers["cache-control"])
        self.assertEqual(len(first), 64 * 1024)
        status, headers, last = self.exchange(
            "GET", path, header_map={"X-CHA-Audio-Offset": str(len(first))})
        self.assertEqual(status, 200)
        self.assertEqual(headers["x-cha-audio-complete"], "1")
        self.assertEqual(first + last, audio)
        status, headers, body = self.exchange(
            "GET", path, header_map={"X-CHA-Audio-Offset": str(len(audio))})
        self.assertEqual(status, 200)
        self.assertEqual(headers["x-cha-audio-complete"], "1")
        self.assertEqual(body, b"")
        self.assertEqual(self.exchange(
            "GET", path, header_map={"X-CHA-Audio-Offset": "-1"})[0], 400)
        self.assertEqual(self.exchange("GET", path)[2], audio)

    def test_first_input_lifecycle(self):
        self.use_provider()
        prompt = HOLD + " first turn"
        created = self.create(prompt)
        self.provider.wait_for(prompt)
        progress = self.snapshot(created["id"])
        self.assertTrue(progress["generation"]["active"])
        self.assertTrue(any(prompt in text for text in self.texts(progress)))
        self.assertFalse(self.provider.released.is_set())
        self.provider.released.set()
        finished = self.wait_idle(created["id"])
        self.assertTrue(any(PROVIDER_REPLY in text for text in self.texts(finished)))
        self.assertEqual(self.prompt_count(prompt), 1)

        before = self.session_ids()
        prompts = self.prompt_count()
        status, _, raw = self.exchange(
            "POST", self.sessions_path(), {"text": "/not-a-command"})
        rejected = self.assert_api_error(status, raw, 422, "invalid_argument")
        self.assertIn("Unknown command", rejected["error"]["message"])
        self.assertEqual(self.session_ids(), before)
        self.assertEqual(self.prompt_count(), prompts)

        note = self.create("@- keep this note")
        self.assertIn(note["id"], self.session_ids())
        kept = self.snapshot(note["id"])
        self.assertTrue(any("keep this note" in text for text in self.texts(kept)))
        self.assertEqual(self.prompt_count(), prompts)

    def test_delete_session_through_nginx(self):
        kept = self.create("@- keep this session")
        removed = self.create("@- delete this session")
        status, headers, raw = self.exchange("DELETE", self.session_path(removed["id"]))
        self.assertEqual(status, 204, raw)
        self.assertEqual(raw, b"")
        self.assertIn("no-store", headers.get("cache-control", ""))
        self.assertEqual(self.session_ids(), [kept["id"]])
        status, _, raw = self.exchange("GET", self.session_path(removed["id"]))
        self.assert_api_error(status, raw, 404, "not_found")
        status, _, raw = self.exchange("DELETE", self.session_path(removed["id"]))
        self.assert_api_error(status, raw, 404, "not_found")

    def test_delete_active_session_through_nginx(self):
        self.use_provider()
        active = self.create(HOLD + " delete-me")
        self.provider.wait_for("delete-me")
        self.assertTrue(self.snapshot(active["id"])["generation"]["active"])
        status, _, raw = self.exchange("DELETE", self.session_path(active["id"]))
        self.assertEqual(status, 204, raw)
        self.assertEqual(raw, b"")
        self.assertNotIn(active["id"], self.session_ids())
        self.provider.released.set()
        status, _, raw = self.exchange("GET", self.session_path(active["id"]))
        self.assert_api_error(status, raw, 404, "not_found")

    def test_continuation_and_stop(self):
        self.use_provider()
        older = self.create("@- older note")
        self.wait_idle(older["id"])
        active = self.create("@- initial note")
        prompt = HOLD + " cancel-me"
        status, _, raw = self.exchange(
            "POST", self.session_path(active["id"]) + "/input", {"text": prompt})
        self.assertEqual(status, 204, raw)
        self.assertEqual(raw, b"")
        self.provider.wait_for(prompt)
        progress = self.snapshot(active["id"])
        self.assertTrue(progress["generation"]["active"])
        self.assertTrue(any(prompt in text for text in self.texts(progress)))
        self.wait_not_live(older["id"])
        reloaded = self.snapshot(older["id"])
        self.assertTrue(any("older note" in text for text in self.texts(reloaded)))
        self.assertFalse(reloaded["generation"]["active"])

        status, _, raw = self.exchange(
            "POST", self.session_path(active["id"]) + "/stop", {})
        self.assertEqual(status, 204, raw)
        self.assertEqual(raw, b"")
        stopped = self.wait_idle(active["id"])
        joined = "\n".join(self.texts(stopped))
        self.assertIn(prompt, joined)
        self.assertNotIn(PROVIDER_REPLY, joined)
        self.assertFalse(self.provider.released.is_set())

        status, _, raw = self.exchange(
            "POST", self.session_path(active["id"]) + "/stop", {})
        self.assertEqual(status, 204, raw)
        self.assertEqual(raw, b"")
        before = self.session_ids()
        status, _, raw = self.exchange(
            "POST", self.session_path("missing-session") + "/stop", {})
        self.assert_api_error(status, raw, 404, "not_found")
        self.assertEqual(self.session_ids(), before)

        self.provider.released.set()
        continued = "continue after stop"
        status, _, raw = self.exchange(
            "POST", self.session_path(active["id"]) + "/input", {"text": continued})
        self.assertEqual(status, 204, raw)
        self.assertEqual(raw, b"")
        self.provider.wait_for(continued)
        finished = self.wait_idle(active["id"])
        joined = "\n".join(self.texts(finished))
        self.assertIn("initial note", joined)
        self.assertIn(prompt, joined)
        self.assertIn(continued, joined)
        self.assertIn(PROVIDER_REPLY, joined)
        self.assertEqual(self.prompt_count(continued), 1)
        self.assertEqual(self.session_ids(), before)

    def test_request_lifetime(self):
        self.use_provider()
        prompt = HOLD + " completed-response"
        created = self.create(prompt)
        self.provider.wait_for(prompt)
        progress = self.snapshot(created["id"])
        self.assertTrue(progress["generation"]["active"])
        self.assertTrue(any(prompt in text for text in self.texts(progress)))
        self.provider.released.set()
        finished = self.wait_idle(created["id"])
        self.assertTrue(any(PROVIDER_REPLY in text for text in self.texts(finished)))
        self.assertEqual(self.prompt_count(prompt), 1)

        # The client never reads this response and must not send the input again.
        self.provider.released.clear()
        lost = HOLD + " lost-response"
        previous = set(self.session_ids())
        sock = self.raw_post(self.sessions_path(), {"text": lost})
        try:
            self.provider.wait_for(lost)
        finally:
            sock.close()
        session_id = self.wait_for_new_session(previous)
        snap = self.snapshot(session_id)
        self.assertTrue(any(lost in text for text in self.texts(snap)))
        self.assertEqual(self.prompt_count(lost), 1)
        self.provider.released.set()
        finished = self.wait_idle(session_id)
        self.assertTrue(any(PROVIDER_REPLY in text for text in self.texts(finished)))
        self.assertEqual(self.prompt_count(lost), 1)

    def test_session_survives_daemon_restart(self):
        config = self.directory / "alice"
        created = self.create("@- remember this")
        self.assertTrue(any(
            "remember this" in text for text in self.texts(self.snapshot(created["id"]))))
        self.stop(self.daemon)
        self.assertEqual(self.daemon.returncode, 0)
        self.daemon = self.start_daemon("alice", config)
        self.wait_ready()
        snap = self.snapshot(created["id"])
        self.assertEqual(snap["session_id"], created["id"])
        self.assertTrue(any("remember this" in text for text in self.texts(snap)))
        self.assertIn(created["id"], self.session_ids())

    def test_text_plain_creation_is_rejected(self):
        self.use_provider()
        before = self.session_ids()
        prompts = self.prompt_count()
        plain = {"text": "call the provider now"}
        status, _, raw = self.exchange(
            "POST", self.sessions_path(), plain, {"Content-Type": "text/plain"})
        rejected = self.assert_api_error(status, raw, 415, "invalid_argument")
        self.assertIn("application/json", rejected["error"]["message"])
        status, _, raw = self.exchange(
            "POST", self.sessions_path(), b'{"text":"also call the provider"}')
        self.assert_api_error(status, raw, 415, "invalid_argument")
        status, headers, raw = self.exchange(
            "OPTIONS", CHAWEB + "/forums/" + self.forum_id() + "/sessions",
            header_map={
                "Origin": "https://evil.example",
                "Access-Control-Request-Method": "POST",
                "Access-Control-Request-Headers": "content-type",
            })
        self.assert_api_error(status, raw, 404, "not_found")
        self.assertNotIn("access-control-allow-origin", headers)
        self.assertNotIn("access-control-allow-methods", headers)
        self.assertEqual(self.session_ids(), before)
        self.assertEqual(self.prompt_count(), prompts)
        created = self.create("@- still works")
        self.assertIn(created["id"], self.session_ids())
        self.assertEqual(self.prompt_count(), prompts)

    def test_validation_does_not_mutate(self):
        created = self.create("@- keep me")
        original = self.texts(self.snapshot(created["id"]))
        before = self.session_ids()

        def unchanged(status, raw, expected, code):
            self.assert_api_error(status, raw, expected, code)
            self.assertEqual(self.session_ids(), before)
            self.assertEqual(self.texts(self.snapshot(created["id"])), original)

        status, _, raw = self.exchange(
            "POST", self.sessions_path(), b"{", {"Content-Type": "application/json"})
        unchanged(status, raw, 400, "invalid_argument")
        status, _, raw = self.exchange(
            "POST", self.sessions_path(), {"text": "hello", "extra": True})
        unchanged(status, raw, 400, "invalid_argument")
        status, _, raw = self.exchange(
            "POST", self.session_path(created["id"]) + "/input",
            {"text": "@- no", "extra": 1})
        unchanged(status, raw, 400, "invalid_argument")
        status, _, raw = self.exchange(
            "POST", self.session_path(created["id"]) + "/stop", {"reason": "x"})
        unchanged(status, raw, 400, "invalid_argument")
        status, _, raw = self.exchange("POST", CHAWEB + "/bootstrap", {})
        unchanged(status, raw, 404, "not_found")
        status, _, raw = self.exchange(
            "GET", self.session_path(created["id"]) + "/input")
        unchanged(status, raw, 404, "not_found")
        status, _, raw = self.exchange("GET", f"{CHAWEB}/forums/missing-forum/sessions")
        unchanged(status, raw, 404, "not_found")
        entrance = self.bootstrap()["entrance_forum_id"]
        status, _, raw = self.exchange(
            "GET", self.session_path(created["id"], entrance))
        unchanged(status, raw, 404, "not_found")
        status, _, raw = self.exchange(
            "POST", self.session_path("missing-session") + "/input", {"text": "hi"})
        unchanged(status, raw, 404, "not_found")
        status, _, raw = self.exchange(
            "POST", self.sessions_path(), {"text": "x" * (32 * 1024 + 1)})
        unchanged(status, raw, 400, "prompt_too_large")

    def test_nginx_cache_limit_gateway_and_gzip(self):
        status, headers, raw = self.exchange("GET", CHAWEB + "/bootstrap")
        self.assertEqual(status, 200, raw)
        self.assertIn("no-store", headers.get("cache-control", ""))
        status, headers, raw = self.exchange("GET", CHAWEB + "/missing")
        self.assert_api_error(status, raw, 404, "not_found")
        self.assertIn("no-store", headers.get("cache-control", ""))
        status, headers, raw = self.exchange(
            "POST", self.session_path("missing-session") + "/stop", {})
        self.assertEqual(status, 404, raw)
        self.assertIn("no-store", headers.get("cache-control", ""))

        note = "gzip-check " + ("x" * 4000)
        created = self.create("@- " + note)
        status, headers, raw = self.exchange(
            "GET", self.session_path(created["id"]),
            header_map={"Accept-Encoding": "gzip"})
        self.assertEqual(status, 200, raw[:200])
        self.assertIn("no-store", headers.get("cache-control", ""))
        self.assertEqual(headers.get("content-encoding"), "gzip")
        decoded = gzip.decompress(raw)
        self.assertLess(len(raw), len(decoded))
        snap = json.loads(decoded)
        self.assertEqual(snap["session_id"], created["id"])
        self.assertTrue(any(note in entry.get("text", "") for entry in snap["transcript"]))

        connection = UnixHTTPConnection(self.nginx_socket)
        try:
            connection.request("POST", self.sessions_path(), headers={
                "Content-Type": "application/json",
                "Content-Length": str(256 * 1024 + 1),
                "Expect": "100-continue",
            })
            response = connection.getresponse()
            cache = response.getheader("Cache-Control", "")
            body = response.read()
            self.assertEqual(response.status, 413, body)
            self.assertIn("no-store", cache)
        finally:
            connection.close()
        self.assertEqual(self.exchange("GET", CHAWEB + "/bootstrap")[0], 200)

        self.stop(self.daemon)
        status, headers, raw = self.exchange("GET", CHAWEB + "/bootstrap")
        self.assertEqual(status, 502, raw[:300])
        self.assertIn("no-store", headers.get("cache-control", ""))

    def test_production_assets(self):
        # The shipped template serves the built files. Placeholder HTML is not enough.
        for path in ("/", "/index.html"):
            status, headers, raw = self.exchange("GET", path)
            self.assertEqual(status, 200, raw[:300])
            self.assertIn("text/html", headers.get("content-type", ""))
            self.assertEqual(headers.get("cache-control"), "no-cache")
            self.assertNotIn(b"__cha-bootstrap.js", raw)
            self.assertIn(
                b"width=device-width, initial-scale=1, viewport-fit=cover", raw)

        page = self.exchange("GET", "/")[2].decode()
        refs = sorted(set(re.findall(r'(?:src|href)="(/assets/[^"]+)"', page)))
        self.assertTrue(any(ref.endswith(".js") for ref in refs), page)
        self.assertTrue(any(ref.endswith(".css") for ref in refs), page)
        types = mime_by_extension(Path(self.mime_types))
        for ref in refs:
            status, headers, body = self.exchange("GET", ref)
            self.assertEqual(status, 200, ref)
            self.assertGreater(len(body), 0, ref)
            extension = ref.rsplit(".", 1)[-1]
            self.assertIn(extension, types, ref)
            self.assertIn(types[extension], headers.get("content-type", ""), ref)
            self.assertEqual(
                headers.get("cache-control"), "public, max-age=31536000, immutable", ref)

        status, headers, raw = self.exchange("GET", CHAWEB + "/bootstrap")
        self.assertEqual(status, 200, raw[:300])
        self.assertIn("no-store", headers.get("cache-control", ""))
        self.assertIn("application/json", headers.get("content-type", ""))


def mime_by_extension(path):
    """Map extensions from nginx's own mime.types file."""
    found = {}
    for line in path.read_text().splitlines():
        stripped = line.split("#", 1)[0].strip()
        match = re.fullmatch(r"([A-Za-z0-9.+-]+/[A-Za-z0-9.+-]+)\s+([^;]+);", stripped)
        if not match:
            continue
        for extension in match.group(2).split():
            found.setdefault(extension, match.group(1))
    return found


def default_scgi_params(nginx):
    """The scgi_params file next to nginx's configured nginx.conf."""
    output = subprocess.run([nginx, "-V"], capture_output=True, text=True).stderr
    conf = re.search(r"--conf-path=(\S+)", output)
    if conf:
        return Path(conf.group(1)).parent / "scgi_params"
    prefix = re.search(r"--prefix=(\S+)", output)
    return Path(prefix.group(1)) / "conf/scgi_params" if prefix else None


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/ninja")
    parser.add_argument("--nginx", default="nginx")
    parser.add_argument("--scgi-params", type=Path)
    parser.add_argument("--mime-types", type=Path)
    args, tests = parser.parse_known_args()
    DaemonHarness.build_dir = args.build_dir.resolve()
    DaemonHarness.nginx_binary = shutil.which(args.nginx)
    if not DaemonHarness.nginx_binary:
        parser.error("nginx is required; install it or pass --nginx /path/to/nginx")
    scgi_params = args.scgi_params or default_scgi_params(DaemonHarness.nginx_binary)
    if not scgi_params or not scgi_params.is_file():
        parser.error(f"nginx's scgi_params was not found ({scgi_params}); "
                     "pass --scgi-params /path/to/scgi_params")
    mime_types = args.mime_types or scgi_params.parent / "mime.types"
    if not mime_types.is_file():
        parser.error(f"nginx's mime.types was not found ({mime_types}); "
                     "pass --mime-types /path/to/mime.types")
    DaemonHarness.scgi_params = scgi_params.resolve()
    DaemonHarness.mime_types = mime_types.resolve()
    for name in ("cha-daemon", "cha_prepare_test_vault"):
        if not (DaemonHarness.build_dir / name).is_file():
            parser.error(f"missing {name}; run make itest-daemon to build it")
    if not CHAWEB_INDEX.is_file():
        parser.error(MISSING_CHAWEB_ASSETS)
    unittest.main(argv=[sys.argv[0]] + tests, verbosity=2)
