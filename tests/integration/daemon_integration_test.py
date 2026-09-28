#!/usr/bin/env python3
"""HTTP integration tests for Linux/macOS: make itest-daemon.

Requires nginx on PATH (or --nginx), Python 3, and the two CMake targets
cha-daemon and cha_prepare_test_vault. Uses only temporary test vaults, Unix
sockets and a fake provider on 127.0.0.1; never uses the installed nginx
service or live providers. nginx's own scgi_params file is used, as in a
deployment; pass --scgi-params when nginx -V does not show where it is.
"""

import argparse
import http.client
import http.server
import json
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
KEY = "Bearer " + "a" * 64
BOB_KEY = "Bearer " + "b" * 64
MISSING_KEY = "Bearer " + "c" * 64
PROBE_KEY = "Bearer " + "d" * 64
# Words in the newest user message that control the fake provider.
HOLD = "HOLD-PROVIDER"
FAIL = "FAIL-PROVIDER"
PROVIDER_REPLY = "Provider reply"


def user(text):
    return {"role": "user", "content": text}


def chat(messages, stream=False, model="lobby", **fields):
    return dict(model=model, messages=messages, stream=stream, **fields)


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


def read_scgi(connection):
    """Returns (header block, [(name, value)], body) of one SCGI request."""
    data = b""

    def more():
        nonlocal data
        chunk = connection.recv(65536)
        if not chunk:
            raise AssertionError("The SCGI request ended early")
        data += chunk

    while b":" not in data:
        more()
    size, _, data = data.partition(b":")
    size = int(size)
    while len(data) < size + 1:
        more()
    block, comma, data = data[:size], data[size:size + 1], data[size + 1:]
    if comma != b",":
        raise AssertionError("The SCGI header block does not end with ','")
    items = [item.decode() for item in block.split(b"\0")[:-1]]
    fields = list(zip(items[0::2], items[1::2]))
    length = int(dict(fields)["CONTENT_LENGTH"])
    while len(data) < length:
        more()
    return block, fields, data


class DaemonIntegration(unittest.TestCase):
    def setUp(self):
        # macOS has a 104-byte Unix socket path limit; its default TMPDIR is long.
        self.temporary = tempfile.TemporaryDirectory(prefix="cha-it-", dir="/tmp")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.processes = []
        self.daemon = self.start_daemon("alice")
        # A Unix socket, not a TCP port, so no other process can take it.
        self.nginx_socket = str(self.directory / "nginx.sock")

        # Exercise the shipped routing configuration, changing only deployment
        # paths, credentials and the TLS listener for an unprivileged local run.
        routing = (ROOT / "packaging/linux/nginx.conf.example").read_text()
        for old, new in (
            ("<alice-key>", "a" * 64),
            ("<bob-key>", "b" * 64),
            ('default               "";',
             f'"{MISSING_KEY}" missing;\n    "{PROBE_KEY}" probe;\n    default "";'),
            ("listen 443 ssl;", f"listen unix:{self.nginx_socket};"),
            ("/run/cha/", f"{self.directory}/"),
        ):
            self.assertIn(old, routing, f"nginx.conf.example no longer contains {old!r}")
            routing = routing.replace(old, new)
        shutil.copy(self.scgi_params, self.directory / "scgi_params")
        (self.directory / "nginx.conf").write_text(
            "daemon off;\nmaster_process off;\n"
            f'pid "{self.directory}/nginx.pid";\n'
            "error_log stderr info;\nevents {}\nhttp {\n"
            "access_log off;\nclient_body_temp_path body;\n"
            "scgi_temp_path scgi;\n" + routing + "\n}\n"
        )
        self.nginx = self.spawn("nginx", [
            self.nginx_binary, "-e", "stderr", "-p", str(self.directory) + "/",
            "-c", str(self.directory / "nginx.conf"),
        ])
        self.wait_ready()

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

    def use_provider(self):
        """Restarts Alice's daemon with a vault whose Guide uses self.provider."""
        self.provider = FakeProvider()
        self.addCleanup(self.provider.close)
        self.stop(self.daemon)
        self.daemon = self.start_daemon(
            "alice", self.prepare_config("alice-net", self.provider))
        self.wait_ready()

    def wait_ready(self, key=KEY):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            for process in self.processes:
                if process.poll() is not None:
                    # A deliberately stopped daemon can remain in the list.
                    self.assertEqual(process.returncode, 0, self.logs())
            try:
                status, _, _ = self.request("GET", "/v1/models", key=key)
                if status == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.05)
        self.fail("nginx/cha-daemon did not become ready\n" + self.logs())

    def request(self, method, path, body=None, key=KEY):
        headers = {}
        if key is not None:
            headers["Authorization"] = key
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        connection = UnixHTTPConnection(self.nginx_socket)
        try:
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            return response.status, response.getheader("Content-Type", ""), response.read()
        finally:
            connection.close()

    def background(self, payload, key=KEY):
        """Posts in a thread. Returns (thread, result); see finished()."""
        result = {}

        def run():
            try:
                result["response"] = self.request("POST", "/v1/chat/completions", payload, key)
            except Exception as error:  # reported by finished()
                result["error"] = error
            result["done"] = time.monotonic()

        thread = threading.Thread(target=run, daemon=True)
        thread.start()
        return thread, result

    def finished(self, item):
        thread, result = item
        thread.join(10)
        self.assertFalse(thread.is_alive(), "The request did not finish")
        if "error" in result:
            raise result["error"]
        return result["response"]

    def open_stream(self, payload):
        connection = UnixHTTPConnection(self.nginx_socket)
        self.addCleanup(connection.close)
        connection.request("POST", "/v1/chat/completions", body=json.dumps(payload).encode(),
                           headers={"Authorization": KEY, "Content-Type": "application/json"})
        response = connection.getresponse()
        self.assertEqual(response.status, 200)
        self.assertIn("text/event-stream", response.getheader("Content-Type", ""))
        return connection, response

    @staticmethod
    def events(response):
        """Yields the SSE data payloads as they arrive."""
        while line := response.readline():
            line = line.decode().rstrip("\r\n")
            if line.startswith("data: "):
                yield line[6:]

    def post(self, payload, expected=200, key=KEY):
        status, content_type, body = self.request("POST", "/v1/chat/completions", payload, key)
        self.assertEqual(status, expected, body)
        self.assertIn("application/json", content_type)
        result = json.loads(body)
        if expected != 200:
            self.assertTrue(result["error"]["message"])
            self.assertIn("type", result["error"])
            self.assertIn("code", result["error"])
        return result

    def completion(self, messages, **fields):
        result = self.post(chat(messages, **fields))
        self.assertEqual(result["object"], "chat.completion")
        self.assertEqual(result["model"], "lobby")
        choice = result["choices"][0]
        self.assertEqual(choice["finish_reason"], "stop")
        self.assertEqual(choice["message"]["role"], "assistant")
        return choice["message"]["content"]

    def tag(self, content):
        self.assertRegex(content, r"^\[//\]: # \(cha lobby/[^\s/()]+\)\n\n")
        return content.split("\n\n", 1)[0]

    def test_authentication_models_and_missing_backend(self):
        for key in (None, "Bearer wrong", "Basic " + "a" * 64):
            with self.subTest(key=key):
                self.assertEqual(self.request("GET", "/v1/models", key=key)[0], 401)
        status, content_type, body = self.request("GET", "/v1/models")
        self.assertEqual(status, 200)
        self.assertIn("application/json", content_type)
        models = json.loads(body)
        self.assertEqual(models["object"], "list")
        self.assertEqual([model["id"] for model in models["data"]], ["lobby"])
        self.assertEqual(models["data"][0]["object"], "model")
        self.assertEqual(models["data"][0]["owned_by"], "cha")
        self.assertEqual(self.request("GET", "/v1/models", key=MISSING_KEY)[0], 502)
        self.assertEqual(self.request("GET", "/v1/unknown")[0], 404)

    def test_daemon_startup_failure_returns_502(self):
        config = self.prepare_config("bob")
        (config / "test.sqlite3").unlink()
        daemon = self.start_daemon("bob")
        self.assertNotEqual(daemon.wait(timeout=10), 0)
        self.assertIn("does not exist", (self.directory / "bob.log").read_text())
        self.assertEqual(self.request("GET", "/v1/models", key=BOB_KEY)[0], 502)
        self.assertEqual(self.request("GET", "/v1/models")[0], 200)

    def test_nginx_sends_only_the_scgi_request(self):
        # A fake SCGI backend records exactly what nginx sends to a daemon.
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.addCleanup(listener.close)
        listener.bind(str(self.directory / "probe.sock"))
        listener.listen(1)
        listener.settimeout(10)
        received = {}

        def serve():
            connection, _ = listener.accept()
            with connection:
                received["request"] = read_scgi(connection)
                connection.sendall(b"Status: 200 OK\r\nContent-Type: application/json\r\n\r\n{}")

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        payload = chat([user("probe")])
        status, _, _ = self.request("POST", "/v1/chat/completions", payload, key=PROBE_KEY)
        thread.join(10)
        self.assertEqual(status, 200)
        block, fields, body = received["request"]
        names = [name for name, _ in fields]
        values = dict(fields)
        self.assertEqual(names[0], "CONTENT_LENGTH")
        self.assertEqual(len(names), len(set(names)), names)
        self.assertEqual(values["SCGI"], "1")
        self.assertEqual(values["REQUEST_METHOD"], "POST")
        self.assertEqual(values["DOCUMENT_URI"], "/v1/chat/completions")
        # nginx always adds HTTP_HOST; no other request header may pass.
        self.assertEqual([name for name in names if name.startswith("HTTP_")], ["HTTP_HOST"])
        self.assertNotIn(PROBE_KEY.split()[1].encode(), block)
        self.assertEqual(json.loads(body), payload)

    def test_turns_continue_and_survive_restart(self):
        first = self.completion([user("Hello — 世界")], temperature=0, tools=[])
        tag = self.tag(first)
        self.assertEqual(first, tag + "\n\n**Guide:** Hello — 世界")
        history = [user("Hello — 世界"), {"role": "assistant", "content": first}]
        recorded = self.completion(history + [user("@- A private note")])
        self.assertEqual(recorded, tag + "\n\n(recorded)")
        self.stop(self.daemon)
        self.assertEqual(self.daemon.returncode, 0)
        self.daemon = self.start_daemon("alice")
        self.wait_ready()
        history += [user("@- A private note"), {"role": "assistant", "content": recorded}]
        continued = self.completion(history + [user("@Guide After restart")])
        self.assertEqual(continued, tag + "\n\n**Guide:** After restart")
        fresh = self.completion([user("A separate chat")])
        self.assertNotEqual(self.tag(fresh), tag)

    def test_streaming_and_session_continuation(self):
        status, content_type, body = self.request(
            "POST", "/v1/chat/completions", chat([user("Stream me")], stream=True))
        self.assertEqual(status, 200, body)
        self.assertIn("text/event-stream", content_type)
        data = [line[6:] for line in body.decode().splitlines() if line.startswith("data: ")]
        self.assertEqual(data[-1], "[DONE]")
        chunks = [json.loads(line) for line in data[:-1]]
        self.assertGreaterEqual(len(chunks), 2)
        self.assertEqual(chunks[0]["choices"][0]["delta"]["role"], "assistant")
        self.assertEqual(chunks[-1]["choices"][0]["finish_reason"], "stop")
        content = ""
        for chunk in chunks:
            self.assertEqual(chunk["object"], "chat.completion.chunk")
            self.assertEqual(chunk["id"], chunks[0]["id"])
            self.assertEqual(chunk["model"], "lobby")
            content += chunk["choices"][0]["delta"].get("content", "")
        tag = self.tag(content)
        self.assertEqual(content, tag + "\n\n**Guide:** Stream me")
        next_reply = self.completion([
            user("Stream me"), {"role": "assistant", "content": content}, user("Next")])
        self.assertEqual(next_reply, tag + "\n\n**Guide:** Next")

    def test_stream_sends_the_tag_before_the_reply(self):
        self.use_provider()
        _, response = self.open_stream(chat([user(HOLD + " stream")], stream=True))
        events = self.events(response)
        # The provider still holds its reply, so this chunk proves that nginx
        # does not buffer the stream and that the tag comes first.
        first = json.loads(next(events))
        delta = first["choices"][0]["delta"]
        self.assertEqual(delta["role"], "assistant")
        tag = self.tag(delta["content"])
        self.provider.released.set()
        rest = list(events)
        self.assertEqual(rest[-1], "[DONE]")
        content = delta["content"] + "".join(
            json.loads(event)["choices"][0]["delta"].get("content", "") for event in rest[:-1])
        self.assertEqual(content, tag + "\n\n**Guide:** " + PROVIDER_REPLY)

    def test_requests_of_one_user_wait_for_the_running_turn(self):
        self.use_provider()
        self.start_daemon("bob", self.prepare_config("bob-net", self.provider))
        self.wait_ready(BOB_KEY)
        first = self.background(chat([user(HOLD + " first")]))
        self.provider.wait_for(HOLD + " first")
        second = self.background(chat([user("queued second")]))
        # Bob has his own daemon, so his turn does not wait for Alice's turn.
        bob = self.post(chat([user("Bob meanwhile")]), key=BOB_KEY)
        self.assertTrue(bob["choices"][0]["message"]["content"].endswith(PROVIDER_REPLY))
        time.sleep(0.5)
        self.assertTrue(first[0].is_alive())
        self.assertTrue(second[0].is_alive(), "nginx did not wait for the busy daemon")
        self.assertFalse(self.provider.received("queued second"))
        self.provider.released.set()
        for item in (first, second):
            status, _, body = self.finished(item)
            self.assertEqual(status, 200, body)
            content = json.loads(body)["choices"][0]["message"]["content"]
            self.assertTrue(content.endswith("**Guide:** " + PROVIDER_REPLY), content)
        self.assertLess(first[1]["done"], second[1]["done"])

    def test_client_disconnect_stops_the_turn(self):
        self.use_provider()
        prompt = HOLD + " then leave"
        connection, response = self.open_stream(chat([user(prompt)], stream=True))
        content = json.loads(next(self.events(response)))["choices"][0]["delta"]["content"]
        response.close()
        connection.close()
        # The provider never answers the held request. Unless the daemon
        # stops that turn, this request waits behind it and times out.
        reply = self.completion([
            user(prompt), {"role": "assistant", "content": content}, user("after leaving")])
        self.assertEqual(reply, self.tag(content) + "\n\n**Guide:** " + PROVIDER_REPLY)

    def test_request_abandoned_while_queued_never_runs(self):
        self.use_provider()
        first = self.background(chat([user(HOLD + " first")]))
        self.provider.wait_for(HOLD + " first")
        body = json.dumps(chat([user("abandoned while queued")])).encode()
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.connect(self.nginx_socket)
            client.sendall(
                b"POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\n"
                + f"Authorization: {KEY}\r\nContent-Type: application/json\r\n"
                  f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
            time.sleep(0.5)  # nginx passes the request to Alice's socket
        self.provider.released.set()
        self.assertEqual(self.finished(first)[0], 200)
        # This request waits behind the abandoned one, so the daemon has seen it.
        self.assertTrue(self.completion([user("after the queue")]).endswith(PROVIDER_REPLY))
        self.assertFalse(self.provider.received("abandoned while queued"), self.provider.prompts)

    def test_provider_failure_reaches_the_client(self):
        self.use_provider()
        result = self.post(chat([user(FAIL + " complete")]), expected=502)
        self.assertEqual(result["error"]["code"], "provider_error")
        status, content_type, body = self.request(
            "POST", "/v1/chat/completions", chat([user(FAIL + " stream")], stream=True))
        self.assertEqual(status, 200, body)
        self.assertIn("text/event-stream", content_type)
        data = [line[6:] for line in body.decode().splitlines() if line.startswith("data: ")]
        self.assertNotIn("[DONE]", data)
        self.tag(json.loads(data[0])["choices"][0]["delta"]["content"])
        self.assertIn("error", json.loads(data[-1]))

    def test_api_keys_isolate_vaults(self):
        self.start_daemon("bob")
        self.wait_ready(BOB_KEY)
        alice = self.completion([user("Alice's chat")])
        payload = chat([user("Alice's chat"), {"role": "assistant", "content": alice},
                        user("Try another vault")])
        self.post(payload, expected=404, key=BOB_KEY)
        bob = self.post(chat([user("Bob's chat")]), key=BOB_KEY)
        bob_content = bob["choices"][0]["message"]["content"]
        # Session IDs are local to a vault and can match across users.
        self.assertEqual(bob_content, self.tag(bob_content) + "\n\n**Guide:** Bob's chat")
        self.assertEqual(self.tag(self.completion(payload["messages"])), self.tag(alice))

    def test_invalid_requests_leave_daemon_usable(self):
        cases = [
            (b"{", 400), ({}, 400),
            (chat([user("Hello")], model="missing"), 404),
            (chat([user("Hello")], model="builtin-entrance"), 404),
            (chat([]), 400), (chat([{"role": "assistant", "content": "No user"}]), 400),
            (chat([{"role": "assistant", "content": "No tag"}, user("Hello")]), 400),
            (chat([{"role": "assistant", "content": "[//]: # (cha other/session)"}, user("Hello")]), 400),
            (chat([{"role": "assistant", "content": "[//]: # (cha lobby/missing)"}, user("Hello")]), 404),
        ]
        cases += [(chat([user(text)]), 400) for text in
                  ("", "/unknown", "@Nobody Hello", "@Guide", "x" * (32 * 1024 + 1))]
        for payload, expected in cases:
            with self.subTest(payload=str(payload)[:180]):
                self.post(payload, expected)
        self.assertTrue(self.completion([user("Still working")]).endswith("**Guide:** Still working"))

    def test_nginx_body_limit(self):
        # Send only the headers: nginx must reject the declared length before
        # accepting/buffering a body or forwarding it to the daemon.
        connection = UnixHTTPConnection(self.nginx_socket)
        try:
            connection.request("POST", "/v1/chat/completions", headers={
                "Authorization": KEY, "Content-Length": str(16 * 1024 * 1024 + 1),
                "Expect": "100-continue",
            })
            response = connection.getresponse()
            self.assertEqual(response.status, 413, response.read())
        finally:
            connection.close()
        self.assertEqual(self.request("GET", "/v1/models")[0], 200)


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
    args, tests = parser.parse_known_args()
    DaemonIntegration.build_dir = args.build_dir.resolve()
    DaemonIntegration.nginx_binary = shutil.which(args.nginx)
    if not DaemonIntegration.nginx_binary:
        parser.error("nginx is required; install it or pass --nginx /path/to/nginx")
    scgi_params = args.scgi_params or default_scgi_params(DaemonIntegration.nginx_binary)
    if not scgi_params or not scgi_params.is_file():
        parser.error(f"nginx's scgi_params was not found ({scgi_params}); "
                     "pass --scgi-params /path/to/scgi_params")
    DaemonIntegration.scgi_params = scgi_params
    for name in ("cha-daemon", "cha_prepare_test_vault"):
        if not (DaemonIntegration.build_dir / name).is_file():
            parser.error(f"missing {name}; run make itest-daemon to build it")
    unittest.main(argv=[sys.argv[0]] + tests, verbosity=2)
