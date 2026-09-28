#!/usr/bin/env python3
"""Run cha-daemon without systemd, for example on macOS.

Build: cmake --build --preset ninja --target cha-daemon
Run: python3 scripts/run_daemon.py /tmp/cha.sock build/ninja/cha-daemon --config /path/to/cha-config

The script does what systemd socket activation does: it binds a listening
Unix socket, puts it on file descriptor 3, sets LISTEN_FDS and LISTEN_PID,
and replaces itself with the daemon. exec keeps the process ID, so LISTEN_PID
matches the daemon. On macOS a socket path must be at most 104 bytes.
"""
import os
import socket
import sys

if len(sys.argv) < 3:
    sys.exit("usage: run_daemon.py SOCKET_PATH DAEMON [ARGS...]")

path = sys.argv[1]
if os.path.exists(path):
    os.unlink(path)
listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
listener.bind(path)
listener.listen(16)
if listener.fileno() != 3:
    os.dup2(listener.fileno(), 3)
os.set_inheritable(3, True)
os.environ["LISTEN_PID"] = str(os.getpid())
os.environ["LISTEN_FDS"] = "1"
os.execv(sys.argv[2], sys.argv[2:])
