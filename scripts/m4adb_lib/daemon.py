"""Persistent USB-Serial/JTAG bridge for m4adb.

The ESP32-S3 can reset when a CDC handle is opened repeatedly.  This module
keeps exactly one SerialTransport open and exposes a 0600 Unix socket; CLI
invocations use the socket and never reopen the hardware port.
"""

from __future__ import annotations

import hashlib
import os
import select
import socket
import sys
import time
from pathlib import Path
from typing import Optional

from .transport import SerialTransport


def socket_path_for_port(port: str) -> Path:
    """Return a stable, per-port socket path without leaking the full path."""
    digest = hashlib.sha1(port.encode("utf-8", errors="replace")).hexdigest()[:12]
    return Path("/tmp") / f"m4adb-{digest}.sock"


class DaemonTransport:
    """Transport implementation backed by a local daemon Unix socket."""

    def __init__(self, path: Path, timeout: float = 3.0) -> None:
        self.path = Path(path)
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect(str(self.path))
        self.sock.setblocking(False)

    def write(self, data: str) -> None:
        if not data.endswith("\n"):
            data += "\n"
        self.sock.sendall(data.encode("utf-8", errors="replace"))

    def read(self, timeout: float = 0.05) -> str:
        readable, _, _ = select.select([self.sock], [], [], max(0.0, timeout))
        if not readable:
            return ""
        try:
            data = self.sock.recv(8192)
        except BlockingIOError:
            return ""
        if not data:
            raise RuntimeError("m4adb daemon disconnected")
        return data.decode("utf-8", errors="replace")

    def close(self) -> None:
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


def daemon_alive(path: Path, timeout: float = 0.25) -> bool:
    try:
        t = DaemonTransport(path, timeout=timeout)
        t.close()
        return True
    except OSError:
        return False


def stop_daemon(path: Path, timeout: float = 1.0) -> bool:
    """Ask a daemon to stop; returns false when no daemon is listening."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(str(path))
        s.sendall(b"@M4ADBD/1 shutdown\n")
        s.close()
        return True
    except OSError:
        return False


class BridgeDaemon:
    """Single-owner serial forwarder.  It never opens a second USB handle."""

    def __init__(self, port: str, baud: int, path: Path) -> None:
        self.port = port
        self.baud = int(baud)
        self.path = Path(path)
        self.serial: Optional[SerialTransport] = None
        self.listener: Optional[socket.socket] = None
        self.peer: Optional[socket.socket] = None
        self.running = True

    def _close_peer(self) -> None:
        if self.peer is not None:
            try:
                self.peer.close()
            except OSError:
                pass
            self.peer = None

    def _cleanup(self) -> None:
        self._close_peer()
        if self.listener is not None:
            try:
                self.listener.close()
            except OSError:
                pass
            self.listener = None
        if self.serial is not None:
            self.serial.close()
            self.serial = None
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass

    def serve(self, ready_timeout: float = 60.0) -> int:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            if self.path.exists():
                if daemon_alive(self.path):
                    print(f"daemon already running: {self.path}", file=sys.stderr, flush=True)
                    return 2
                self.path.unlink()
            self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.listener.bind(str(self.path))
            os.chmod(self.path, 0o600)
            self.listener.listen(1)
            self.listener.settimeout(0.05)
            # Bind the arbitration socket before touching USB.  A concurrent
            # CLI invocation now observes the listener and reuses it instead
            # of opening a second CDC handle (which can reset the board).
            self.serial = SerialTransport(self.port, self.baud)
            # The only hardware handshake/open happens here, once per daemon.
            from .client import Client

            Client(self.serial, default_timeout=3).wait_ready(timeout=ready_timeout)
            print(f"m4adb daemon ready socket={self.path} port={self.port}", flush=True)
            while self.running:
                if self.peer is None:
                    try:
                        self.peer, _ = self.listener.accept()
                        self.peer.setblocking(False)
                    except socket.timeout:
                        pass
                    except OSError:
                        break
                if self.peer is not None:
                    try:
                        readable, _, _ = select.select([self.peer], [], [], 0)
                        if readable:
                            data = self.peer.recv(8192)
                            if not data:
                                self._close_peer()
                            elif data == b"@M4ADBD/1 shutdown\n":
                                self.running = False
                            else:
                                self.serial.write(data.decode("utf-8", errors="replace"))
                    except (BlockingIOError, ConnectionError, OSError):
                        self._close_peer()
                # Pump device output continuously, even between CLI calls.
                try:
                    data = self.serial.read(0.01)
                    if data and self.peer is not None:
                        self.peer.sendall(data.encode("utf-8", errors="replace"))
                except (ConnectionError, OSError):
                    self._close_peer()
        except Exception as exc:  # pragma: no cover - exercised on real host
            print(f"m4adb daemon error: {exc}", file=sys.stderr, flush=True)
            return 1
        finally:
            self._cleanup()
        return 0
