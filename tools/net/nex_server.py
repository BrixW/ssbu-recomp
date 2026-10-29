#!/usr/bin/env python3
"""nex_server.py: our own game server for the game's Online menu (phase 1 of docs/ONLINE_MENU_RE.md).

The game (SSBU_ONLINE_MENU=1, runtime/online_menu.inc) reaches g<id>-lp1.s.n.srv.nintendo.net on TCP port 443 with
nn::ssl in the clear (no TLS: both ends are ours), asks for a WebSocket with protocol "NEX" and speaks PRUDP "lite" in
binary messages: one PRUDP packet per message, no checksum, no encryption. Over PRUDP come RMC calls (remote method
calls of the NEX protocols).

Layers (each logs what it does not know, so the next step is read from the log):
    WebSocket   HTTP upgrade, frames (the client's are masked, ours are not)
    PRUDP lite  SYN -> SYN-ACK, CONNECT -> CONNECT-ACK, DATA -> ACK + RMC, PING -> ACK, DISCONNECT -> ACK
    RMC         request {size u32, protocol u8 | 0x80, call id u32, method u32, body}; a known (protocol, method) has a
                handler; any other is answered Core::NotImplemented and its body is logged in full

Formats: the public NEX documentation (Kinnay's NintendoClients wiki); every field this file relies on that the game has
not yet confirmed is marked UNCONFIRMED.

    py -3 tools/net/nex_server.py [--bind 127.0.0.1] [--port 443] [--log nex_server.log]
"""
import argparse
import base64
import datetime
import hashlib
import hmac
import os
import socket
import struct
import threading
import time

WS_GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# PRUDP
LITE_MAGIC = 0x80
TYPE_SYN, TYPE_CONNECT, TYPE_DATA, TYPE_DISCONNECT, TYPE_PING = 0, 1, 2, 3, 4
TYPE_NAMES = {0: "SYN", 1: "CONNECT", 2: "DATA", 3: "DISCONNECT", 4: "PING", 8: "KEEPALIVE?"}
FLAG_ACK, FLAG_RELIABLE, FLAG_NEED_ACK, FLAG_HAS_SIZE, FLAG_MULTI_ACK = 1, 2, 4, 8, 0x200
OPT_SUPPORT, OPT_SIGNATURE, OPT_FRAGMENT, OPT_INITIAL_SEQ, OPT_MAX_SUBSTREAM = 0, 1, 2, 3, 4

# RMC
NOT_IMPLEMENTED = 0x80010002            # Core::NotImplemented with the error bit
DATASTORE_NOT_FOUND = 0x80690004        # DataStore::NotFound: the answer to a DataStore method we do not know


def hexdump(b, n=128):
    return b[:n].hex(" ") + (" ..." if len(b) > n else "")


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


# ------------------------------------------------------------------ PRUDP lite
class Packet:
    """magic 0x80, options length u8, payload length u16, stream types u8 (source << 4 | destination), source port u8,
    destination port u8, fragment id u8, type and flags u16 (flags << 4 | type), sequence id u16, options, payload."""

    def __init__(self, type_, flags=0, seq=0, src_type=10, dst_type=10, src_port=1, dst_port=0, fragment=0,
                 options=None, payload=b""):
        self.type, self.flags, self.seq = type_, flags, seq
        self.src_type, self.dst_type, self.src_port, self.dst_port = src_type, dst_type, src_port, dst_port
        self.fragment, self.options, self.payload = fragment, options or [], payload

    @staticmethod
    def decode(b):
        if len(b) < 12 or b[0] != LITE_MAGIC:
            raise ValueError("not a PRUDP lite packet: %s" % hexdump(b, 16))
        optlen, plen, types, sport, dport, frag, tf, seq = struct.unpack_from("<BHBBBBHH", b, 1)
        if 12 + optlen + plen != len(b):
            raise ValueError("lengths %d + %d do not match %d bytes" % (optlen, plen, len(b)))
        opts, o = [], 12
        while o < 12 + optlen:
            oid, osz = b[o], b[o + 1]
            opts.append((oid, b[o + 2:o + 2 + osz]))
            o += 2 + osz
        return Packet(tf & 0xf, tf >> 4, seq, types >> 4, types & 0xf, sport, dport, frag, opts, b[12 + optlen:])

    def encode(self):
        opts = b"".join(struct.pack("<BB", oid, len(v)) + v for oid, v in self.options)
        return struct.pack("<BBHBBBBHH", LITE_MAGIC, len(opts), len(self.payload), self.src_type << 4 | self.dst_type,
                           self.src_port, self.dst_port, self.fragment, self.flags << 4 | self.type, self.seq) + opts + self.payload

    def option(self, oid):
        for i, v in self.options:
            if i == oid:
                return v
        return None

    def __str__(self):
        fl = "|".join(n for f, n in ((FLAG_ACK, "ACK"), (FLAG_RELIABLE, "RELIABLE"), (FLAG_NEED_ACK, "NEED_ACK"),
                                     (FLAG_HAS_SIZE, "HAS_SIZE"), (FLAG_MULTI_ACK, "MULTI_ACK")) if self.flags & f)
        opts = " ".join("%d=%s" % (i, v.hex()) for i, v in self.options)
        return "%s%s seq %d ports %d->%d streams %d->%d frag %d%s%s" % (
            TYPE_NAMES.get(self.type, self.type), "[" + fl + "]" if fl else "", self.seq, self.src_port, self.dst_port,
            self.src_type, self.dst_type, self.fragment, " opts " + opts if opts else "",
            " payload %d" % len(self.payload) if self.payload else "")


# ------------------------------------------------------------------ RMC
class Rmc:
    def __init__(self, protocol, call, method, body):
        self.protocol, self.call, self.method, self.body = protocol, call, method, body

    @staticmethod
    def decode(b):
        size, = struct.unpack_from("<I", b)
        if size + 4 != len(b):
            raise ValueError("RMC size %d, %d bytes" % (size, len(b)))
        proto, o = b[4], 5
        if not proto & 0x80:
            raise ValueError("RMC response from the client (protocol byte %02x)" % proto)
        proto &= 0x7f
        if proto == 0x7f:
            proto, = struct.unpack_from("<H", b, o)
            o += 2
        call, method = struct.unpack_from("<II", b, o)
        return Rmc(proto, call, method, b[o + 8:])


def rmc_proto_bytes(proto, request):
    flag = 0x80 if request else 0
    return bytes([0x7f | flag]) + struct.pack("<H", proto) if proto >= 0x7f else bytes([proto | flag])


def rmc_ok(req, body):
    b = rmc_proto_bytes(req.protocol, False) + b"\x01" + struct.pack("<II", req.call, req.method | 0x8000) + body
    return struct.pack("<I", len(b)) + b


def rmc_error(req, code):
    b = rmc_proto_bytes(req.protocol, False) + b"\x00" + struct.pack("<II", code, req.call)
    return struct.pack("<I", len(b)) + b


# ------------------------------------------------------------------ NEX data (NEX 4: structures carry a header)
class Reader:
    def __init__(self, b):
        self.b, self.o = b, 0

    def u(self, f):
        v = struct.unpack_from("<" + f, self.b, self.o)[0]
        self.o += struct.calcsize(f)
        return v

    def raw(self, n):
        v = self.b[self.o:self.o + n]
        self.o += n
        return v

    def string(self):
        return self.raw(self.u("H")).rstrip(b"\0").decode("utf-8", "replace")

    def buffer(self):
        return self.raw(self.u("I"))

    def struct_header(self):
        return self.u("B"), self.u("I")          # version, length


class Writer:
    def __init__(self):
        self.parts = []

    def u(self, f, v):
        self.parts.append(struct.pack("<" + f, v))
        return self

    def raw(self, b):
        self.parts.append(b)
        return self

    def string(self, t):
        b = t.encode() + b"\0"
        return self.u("H", len(b)).raw(b)

    def buffer(self, b):
        return self.u("I", len(b)).raw(b)

    def bytes(self):
        return b"".join(self.parts)

    def structure(self, body, version=0):
        return self.u("B", version).u("I", len(body)).raw(body)


def nex_datetime(t=None):
    t = t or datetime.datetime.now(datetime.timezone.utc)
    return t.second | t.minute << 6 | t.hour << 12 | t.day << 17 | t.month << 22 | t.year << 26


def rc4(key, data):
    S, j = list(range(256)), 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 255
        S[i], S[j] = S[j], S[i]
    out, i, j = bytearray(), 0, 0
    for c in data:
        i = (i + 1) & 255
        j = (j + S[i]) & 255
        S[i], S[j] = S[j], S[i]
        out.append(c ^ S[(S[i] + S[j]) & 255])
    return bytes(out)


def kerberos_encrypt(key, data):
    """RC4 with the key, then HMAC-MD5 of the result with the same key (the game accepts the login's ticket so made)."""
    out = rc4(key, data)
    return out + hmac.new(key, out, "md5").digest()


def kerberos_decrypt(key, data):
    """None when the HMAC does not match."""
    body, mac = data[:-16], data[-16:]
    if not hmac.compare_digest(hmac.new(key, body, "md5").digest(), mac):
        return None
    return rc4(key, body)


# ------------------------------------------------------------------ the servers' calls
SERVER_NAME = "ssbu-recomp"
SECURE_PID = 2              # the secure server's pid, the target of the login's ticket
SECURE_STREAM_PORT = 1      # UNCONFIRMED: the secure server on the same PRUDP port as the authentication server


def pid_of(username):
    """The player's NEX pid: the network service account id the game sends (runtime/online_menu.inc om_nsa_id)."""
    try:
        return int(username) & 0xffffffffffffffff
    except ValueError:
        return int.from_bytes(hashlib.sha256(username.encode()).digest()[:8], "little") | 1


def validate_and_request_ticket(client, req):
    """TicketGranting method 6 with ValidateAndRequestTicketParam {platform u32, username, AnyDataHolder
    AuthenticationInfo {token, ngs version u32, token type u8, server version u32}, skip version check bool,
    nex version u32, client version u32}. Answers ValidateAndRequestTicketResult {pid u64, ticket Buffer, secure server
    StationURL, server time DateTime, server name, source key (hex)} (UNCONFIRMED: field list and version 0)."""
    r = Reader(req.body)
    r.struct_header()
    platform, username = r.u("I"), r.string()
    holder = r.string()
    r.u("I"), r.u("I")
    r.struct_header(), r.struct_header()
    token, ngs, token_type, server_version = r.string(), r.u("I"), r.u("B"), r.u("I")
    skip, nex_version, client_version = r.u("B"), r.u("I"), r.u("I")
    pid = pid_of(username)
    client.log("%s     login: platform %d, user %s (pid %d), %s token %d bytes (ngs %d, type %d), NEX %d, client %d" % (
        client.who, platform, username, pid, holder, len(token), ngs, token_type, nex_version, client_version))
    client.pid = pid
    key = client.user_key = os.urandom(32)                   # the source key: the client decrypts its tickets with it
    session_key = os.urandom(32)
    internal = Writer().u("Q", pid).raw(session_key).bytes()     # only our secure server reads it (our own format)
    ticket = kerberos_encrypt(key, Writer().raw(session_key).u("Q", SECURE_PID).buffer(internal).bytes())
    host, port = client.local
    url = "prudps:/address=%s;port=%d;CID=1;PID=2;sid=%d;stream=10;type=2" % (host, port, SECURE_STREAM_PORT)
    body = (Writer().u("Q", pid).buffer(ticket).string(url).u("Q", nex_datetime()).string(SERVER_NAME)
            .string(key.hex()).bytes())
    client.log("%s     ticket for pid %d, secure server %s" % (client.who, pid, url))
    return Writer().structure(body).bytes()


RESULT_SUCCESS = 0x00010001


def request_ticket(client, req):
    """TicketGranting method 3 (source pid u64, target pid u64): the ticket for the secure server. Answers Result, the
    ticket Buffer and the key as hex. Encrypted with the login's source key: with a fresh key the game dropped the ticket
    and crashed on a null session key (0x71001cf570, called from 0x71002443d8)."""
    r = Reader(req.body)
    source, target = r.u("Q"), r.u("Q")
    key, session_key = client.user_key or os.urandom(32), os.urandom(32)
    internal = Writer().u("Q", source).raw(session_key).bytes()
    ticket = kerberos_encrypt(key, Writer().raw(session_key).u("Q", target).buffer(internal).bytes())
    client.log("%s     ticket from pid %d for pid %d" % (client.who, source, target))
    return Writer().u("I", RESULT_SUCCESS).buffer(ticket).string(key.hex()).bytes()


_next_cid = [1]


def register(client, req):
    """SecureConnection method 1 (List<StationURL>): the game's local addresses (prudp:/address=...;port=...;Pl=3;
    Tpt=2;natf=0;natm=0;pmp=0;sid=30;upnp=0). Answers Result, the connection id u32 and the public StationURL: the
    first local one as seen from here, with type=3, PID and RVCID."""
    r = Reader(req.body)
    urls = [r.string() for _ in range(r.u("I"))]
    cid = client.cid = _next_cid[0]
    _next_cid[0] += 1
    first = urls[0] if urls else "prudp:/address=%s;port=0" % client.peer[0]
    scheme, _, params = first.partition(":/")
    fields = dict(kv.split("=", 1) for kv in params.split(";") if "=" in kv)
    fields["address"] = client.peer[0]
    fields.update(type="3", PID=str(client.pid or 0), RVCID=str(cid))
    public = scheme + ":/" + ";".join("%s=%s" % kv for kv in sorted(fields.items()))
    client.urls = urls + [public]
    client.log("%s     register: %s -> connection id %d, public %s" % (client.who, " | ".join(urls), cid, public))
    return Writer().u("I", RESULT_SUCCESS).u("I", cid).string(public).bytes()


def get_integer_settings(client, req):
    """Utility method 7 GetIntegerSettings(index u32) -> Map<u16, s32>: no settings."""
    client.log("%s     integer settings %d: none" % (client.who, Reader(req.body).u("I")))
    return Writer().u("I", 0).bytes()


def datastore_61(client, req):
    """DataStore method 61, no parameters (a Smash method: UNKNOWN). Answered with an empty list (u32 0) to see what
    the game does with it."""
    return Writer().u("I", 0).bytes()


def matchmake_ext_58(client, req):
    """MatchmakeExtension method 58 (u32: 7 when the player searches Battle Arenas): UNKNOWN. Answered with an empty
    list (u32 0) to learn what the game does with it."""
    client.log("%s     MatchmakeExtension 58(%s): an empty list" % (client.who, req.body.hex(" ")))
    return Writer().u("I", 0).bytes()


def matchmake_ext_52(client, req):
    """MatchmakeExtension method 52, the Battle Arenas search: a MatchmakeSessionSearchCriteria-like structure
    (attribute strings "0", "1", "7"..., flags) and at the end a ResultRange {offset 0, size 50}. Answered with an empty
    list of sessions (u32 0) (UNCONFIRMED: the answer type)."""
    client.log("%s     MatchmakeExtension 52 (arena search): no arenas" % client.who)
    return Writer().u("I", 0).bytes()


HANDLERS = {
    (109, 52): matchmake_ext_52,
    (109, 58): matchmake_ext_58,
    (110, 7): get_integer_settings,
    (115, 61): datastore_61,
    (11, 1): register,
    (10, 3): request_ticket,
    (10, 6): validate_and_request_ticket,
}

# protocol id -> (name, {method id: name}); names only for the log
PROTOCOLS = {
    10: ("TicketGranting", {1: "Login", 2: "LoginEx", 3: "RequestTicket", 4: "GetPID", 5: "GetName",
                            6: "ValidateAndRequestTicketWithParam"}),
    11: ("SecureConnection", {1: "Register", 2: "RequestConnectionData", 3: "RequestUrls",
                              4: "RegisterEx", 5: "TestConnectivity", 6: "UpdateURLs", 7: "ReplaceURL",
                              8: "SendReport"}),
    14: ("Notification", {}),
    21: ("MatchMaking", {}),
    50: ("MatchmakeReferee", {}),
    109: ("MatchmakeExtension", {}),
    110: ("Utility", {1: "AcquireNexUniqueId", 7: "GetIntegerSettings", 8: "GetStringSettings"}),
    112: ("Ranking2", {}),
    115: ("DataStore", {}),
}


def rmc_name(req):
    pname, methods = PROTOCOLS.get(req.protocol, ("protocol %d" % req.protocol, {}))
    return "%s.%s" % (pname, methods.get(req.method, "method %d" % req.method))


# ------------------------------------------------------------------ one client (one WebSocket)
class Client:
    def __init__(self, s, addr, log):
        self.s, self.log, self.who = s, log, "%s:%d" % addr
        self.send_lock = threading.Lock()
        self.seq_out = 1                 # our next DATA sequence id (UNCONFIRMED: 1 unless the client gave option 3)
        self.fragments = b""
        self.handlers = HANDLERS         # (protocol, method) -> fn(client, Rmc) -> body bytes or int error
        self.local = s.getsockname()     # the address the game reached us on (the secure server's address)
        self.peer = addr                 # the game's address as seen from here
        self.cid, self.urls = 0, []
        self.pid = None
        self.user_key = None             # the login's source key
        self.last_ports = (1, 0x1f)

    # WebSocket
    def recv_exact(self, n):
        out = b""
        while len(out) < n:
            k = self.s.recv(n - len(out))
            if not k:
                raise ConnectionError("closed")
            out += k
        return out

    def ws_read(self):
        h = self.recv_exact(2)
        op, masked, ln = h[0] & 15, h[1] >> 7, h[1] & 127
        if ln == 126:
            ln = int.from_bytes(self.recv_exact(2), "big")
        elif ln == 127:
            ln = int.from_bytes(self.recv_exact(8), "big")
        mask = self.recv_exact(4) if masked else b"\0\0\0\0"
        return op, bytes(b ^ mask[i & 3] for i, b in enumerate(self.recv_exact(ln)))

    def ws_send(self, data, op=2):
        n = len(data)
        h = bytes([0x80 | op]) + (bytes([n]) if n < 126 else bytes([126]) + n.to_bytes(2, "big") if n < 65536
                                  else bytes([127]) + n.to_bytes(8, "big"))
        with self.send_lock:
            self.s.sendall(h + data)

    def handshake(self):
        req = b""
        while b"\r\n\r\n" not in req:
            k = self.s.recv(4096)
            if not k:
                raise ConnectionError("closed before the upgrade")
            req += k
        lines = req.partition(b"\r\n\r\n")[0].decode("latin-1").split("\r\n")
        hdr = {k.strip().lower(): v.strip() for k, _, v in (l.partition(":") for l in lines[1:])}
        self.log("%s %s, Host %s, protocol %s" % (self.who, lines[0], hdr.get("host"), hdr.get("sec-websocket-protocol")))
        if hdr.get("upgrade", "").lower() != "websocket":
            self.s.sendall(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
            raise ConnectionError("not a WebSocket upgrade")
        accept = base64.b64encode(hashlib.sha1(hdr.get("sec-websocket-key", "").encode() + WS_GUID).digest())
        reply = b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n"
        if "sec-websocket-protocol" in hdr:
            reply += b"Sec-WebSocket-Protocol: " + hdr["sec-websocket-protocol"].split(",")[0].strip().encode() + b"\r\n"
        self.s.sendall(reply + b"\r\n")

    # PRUDP
    def send_packet(self, p):
        self.log("%s  <- %s" % (self.who, p))
        self.ws_send(p.encode())

    def reply_to(self, req, type_, flags, seq=None, options=None, payload=b""):
        return Packet(type_, flags, req.seq if seq is None else seq, req.dst_type, req.src_type, req.dst_port,
                      req.src_port, 0, options, payload)

    def on_packet(self, p):
        self.log("%s  -> %s" % (self.who, p))
        if p.flags & (FLAG_ACK | FLAG_MULTI_ACK):
            return                                            # the client's ACK of our packet: nothing to resend over TCP
        if p.type == TYPE_SYN:
            sig = os.urandom(16)                              # UNCONFIRMED: the lite SYN-ACK carries a 16-byte signature
            opts = [(OPT_SUPPORT, p.option(OPT_SUPPORT) or struct.pack("<I", 0))] + [(OPT_SIGNATURE, sig)]
            self.send_packet(self.reply_to(p, TYPE_SYN, FLAG_ACK, options=opts))
        elif p.type == TYPE_CONNECT:
            init = p.option(OPT_INITIAL_SEQ)
            if init:
                self.seq_out = struct.unpack("<H", init)[0]
            opts = [(i, v) for i, v in p.options if i in (OPT_SUPPORT, OPT_INITIAL_SEQ, OPT_MAX_SUBSTREAM)]
            self.send_packet(self.reply_to(p, TYPE_CONNECT, FLAG_ACK, options=opts, payload=self.secure_connect(p.payload)))
        elif p.type == TYPE_DATA:
            if p.flags & FLAG_NEED_ACK:
                self.send_packet(self.reply_to(p, TYPE_DATA, FLAG_ACK))
            self.fragments += p.payload
            if p.fragment == 0:                               # the last (or only) fragment
                data, self.fragments = self.fragments, b""
                self.on_rmc(data)
        elif p.type == TYPE_PING:
            if p.flags & FLAG_NEED_ACK:
                self.send_packet(self.reply_to(p, TYPE_PING, FLAG_ACK))
        elif p.type == TYPE_DISCONNECT:
            self.send_packet(self.reply_to(p, TYPE_DISCONNECT, FLAG_ACK))
            raise ConnectionError("DISCONNECT")
        elif p.flags & FLAG_NEED_ACK:
            # type 8: the game's keep-alive on the secure connection, every 3 s with its own sequence; without an ACK
            # the game drops the connection after about 50 s (2306-0332)
            self.send_packet(self.reply_to(p, p.type, FLAG_ACK))

    def secure_connect(self, payload):
        """The secure server's CONNECT: Buffer internal ticket (ours: pid u64, session key) + Buffer check data
        (kerberos with the session key: pid u64, connection id u32, check u32). The answer is Buffer(check + 1) in the clear (the game disconnected when it was encrypted, with or without a length)."""
        if not payload:
            return b""                                        # the authentication server: no ticket
        r = Reader(payload)
        internal, check = r.buffer(), r.buffer()
        pid, session_key = struct.unpack_from("<Q", internal)[0], internal[8:]
        plain = kerberos_decrypt(session_key, check)
        if plain is None or len(plain) < 16:
            self.log("%s     secure CONNECT: the check data does not decrypt with the ticket's session key (%s)" % (self.who, hexdump(check)))
            return b""
        cpid, cid, value = struct.unpack_from("<QII", plain)
        self.pid, self.session_key = pid, session_key
        self.log("%s     secure CONNECT: pid %d (check data pid %d), connection id %d, check %08x" % (self.who, pid, cpid, cid, value))
        return Writer().buffer(struct.pack("<I", (value + 1) & 0xffffffff)).bytes()

    def send_data(self, payload, src_port=1, dst_port=0x1f):
        p = Packet(TYPE_DATA, FLAG_RELIABLE | FLAG_NEED_ACK | FLAG_HAS_SIZE, self.seq_out, 10, 10, src_port, dst_port,
                   0, None, payload)
        self.seq_out = (self.seq_out + 1) & 0xffff
        self.send_packet(p)

    # RMC
    def on_rmc(self, data):
        try:
            req = Rmc.decode(data)
        except (ValueError, struct.error) as e:
            self.log("%s     not an RMC request (%s): %s" % (self.who, e, hexdump(data)))
            return
        fn = self.handlers.get((req.protocol, req.method))
        self.log("%s     RMC call %d %s, %d bytes%s" % (self.who, req.call, rmc_name(req), len(req.body),
                                                         "" if fn else ": NOT IMPLEMENTED (%s), body %s" % (
            "answered DataStore::NotFound" if req.protocol == 115 else "answered Core::NotImplemented", hexdump(req.body, 512))))
        try:
            out = fn(self, req) if fn else DATASTORE_NOT_FOUND if req.protocol == 115 else NOT_IMPLEMENTED
        except (ValueError, struct.error, IndexError) as e:
            self.log("%s     %s: could not read the call (%s), body %s" % (self.who, rmc_name(req), e, hexdump(req.body, 512)))
            out = NOT_IMPLEMENTED
        self.send_data(rmc_error(req, out) if isinstance(out, int) else rmc_ok(req, out), self.last_ports[0],
                       self.last_ports[1])

    def run(self):
        self.log("%s connected" % self.who)
        try:
            self.s.settimeout(300)
            self.handshake()
            while True:
                op, data = self.ws_read()
                if op == 8:
                    self.log("%s WebSocket close" % self.who)
                    return
                if op in (9, 10):
                    if op == 9:
                        self.ws_send(data, 10)
                    continue
                try:
                    p = Packet.decode(data)
                except ValueError as e:
                    self.log("%s  ?? %s" % (self.who, e))
                    continue
                self.last_ports = (p.dst_port, p.src_port)
                self.on_packet(p)
        except (ConnectionError, OSError) as e:
            self.log("%s %s" % (self.who, e))
        finally:
            self.s.close()
            self.log("%s gone" % self.who)


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
    log("nex server on TCP %s:%d (WebSocket, PRUDP lite)" % (a.bind, a.port))
    while True:
        s, addr = srv.accept()
        threading.Thread(target=Client(s, addr, log).run, daemon=True).start()


if __name__ == "__main__":
    main()
