#!/usr/bin/env python3
"""nex_probe.py: what the game's Online menu sends to "its" game server (phase 0 of docs/ONLINE_MENU_RE.md).

The game (SSBU_ONLINE_MENU=1, runtime/online_menu.inc) resolves g<id>-lp1.s.n.srv.nintendo.net to our address and opens
TCP port 443 with nn::ssl in the clear (no TLS: both ends are ours). This probe accepts that connection, answers an HTTP
WebSocket upgrade if one comes, and logs every request line, header and WebSocket frame (opcode, length, first bytes).
It sends nothing else: the game then waits or gives up, which is enough to learn the first message.

    py -3 tools/net/nex_probe.py [--bind 127.0.0.1] [--port 443] [--log nex_probe.log]
"""
import argparse
import base64
import hashlib
import socket
import threading
import time

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OPCODES = {0: "continuation", 1: "text", 2: "binary", 8: "close", 9: "ping", 10: "pong"}


class Log:
    def __init__(self, path):
        self.f = open(path, "a", encoding="utf-8") if path else None
        self.lock = threading.Lock()

    def __call__(self, text):
        line = time.strftime("%H:%M:%S ") + text
        with self.lock:
            print(line, flush=True)
            if self.f:
                self.f.write(line + "\n")
                self.f.flush()


def hexdump(b, n=96):
    return b[:n].hex(" ") + (" ..." if len(b) > n else "")


def recv_exact(s, n):
    out = b""
    while len(out) < n:
        k = s.recv(n - len(out))
        if not k:
            raise ConnectionError("closed")
        out += k
    return out


def ws_frames(s, log, who):
    while True:
        h = recv_exact(s, 2)
        fin, op, masked, ln = h[0] >> 7, h[0] & 15, h[1] >> 7, h[1] & 127
        if ln == 126:
            ln = int.from_bytes(recv_exact(s, 2), "big")
        elif ln == 127:
            ln = int.from_bytes(recv_exact(s, 8), "big")
        mask = recv_exact(s, 4) if masked else b"\0\0\0\0"
        data = bytes(b ^ mask[i & 3] for i, b in enumerate(recv_exact(s, ln)))
        log("%s ws %s%s %d bytes: %s" % (who, OPCODES.get(op, op), "" if fin else " (more)", ln, hexdump(data)))
        if op == 8:
            return


def client(s, addr, log):
    who = "%s:%d" % addr
    log("%s connected" % who)
    try:
        s.settimeout(120)
        first = s.recv(4096)
        if not first:
            log("%s closed without sending" % who)
            return
        if not first[:4] in (b"GET ", b"POST", b"PUT ", b"HEAD"):
            log("%s first bytes (not HTTP; TLS would start 16 03): %s" % (who, hexdump(first)))
            while True:
                more = s.recv(4096)
                if not more:
                    break
                log("%s %d bytes: %s" % (who, len(more), hexdump(more)))
            return
        while b"\r\n\r\n" not in first:
            more = s.recv(4096)
            if not more:
                break
            first += more
        head, _, rest = first.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        log("%s HTTP request:" % who)
        for line in lines:
            log("%s   %s" % (who, line))
        if rest:
            log("%s   body %d bytes: %s" % (who, len(rest), hexdump(rest)))
        hdr = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:])}
        if hdr.get("upgrade", "").lower() != "websocket":
            s.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
            return
        accept = base64.b64encode(hashlib.sha1(hdr.get("sec-websocket-key", "").encode() + WS_GUID).digest())
        reply = b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n"
        if "sec-websocket-protocol" in hdr:
            reply += b"Sec-WebSocket-Protocol: " + hdr["sec-websocket-protocol"].split(",")[0].strip().encode() + b"\r\n"
        s.sendall(reply + b"\r\n")
        log("%s WebSocket upgrade answered" % who)
        ws_frames(s, log, who)
    except (ConnectionError, OSError) as e:
        log("%s %s" % (who, e))
    finally:
        s.close()
        log("%s gone" % who)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=443)
    ap.add_argument("--log", default="")
    a = ap.parse_args()
    log = Log(a.log)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((a.bind, a.port))
    srv.listen(8)
    log("nex probe on TCP %s:%d" % (a.bind, a.port))
    while True:
        s, addr = srv.accept()
        threading.Thread(target=client, args=(s, addr, log), daemon=True).start()


if __name__ == "__main__":
    main()
