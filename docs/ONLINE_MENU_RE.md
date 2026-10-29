<!-- The game's own Online menu (Battle Arenas) against our servers: what the game does, found by tracing it. -->
# Online menu: reverse-engineering notes

Goal: the game's own Online > Battle Arenas screens working against our lobby; the fights are played with our rollback
and count for the ladder. Plan: phase 0 (this file) finds what the game speaks; phase 1 answers it.

## How to trace it

```
py -3 tools/net/nex_probe.py --log nex_probe.log
py -3 scripts/run.py --userdata <saves> --env SSBU_ONLINE=off --env SSBU_ONLINE_MENU=1 --log om.log
```
then Online in the game's main menu. `runtime/online_menu.inc` (only with `SSBU_ONLINE_MENU=1`):
- nn::nifm: the network request is accepted, the address is 127.0.0.1;
- nn::account: a network service account, a JWT-shaped ID token (not signed), `EnsureNetworkServiceAccountIdTokenCacheAsync`
  finished at once;
- every name is logged; a name under `nintendo.net` resolves to `SSBU_ONLINE_MENU_SERVER` (default 127.0.0.1), any other
  name is left to the offline resolver (which fails): the game never reaches a server of Nintendo;
- nn::ssl carries the bytes in the clear (no TLS: both ends are ours); sockets, connects and the first 64 packets are
  logged (`[online-menu]` lines).

## What the game does (13.0.4, 2026-10-29)

1. Online in the main menu: `LoadCachedNintendoAccountInfo`, `EnsureNetworkServiceAccountIdTokenCacheAsync`,
   `LoadNetworkServiceAccountIdTokenCache` (twice), then the game server's name.
2. Name: `g23380901-%.s.n.srv.nintendo.net` through `nsd::ResolveEx` (the `%` is the environment, `lp1` on a console;
   our ResolveEx leaves it as it is), then `GetAddrInfo` and `GetHostByNameWithOptions`.
3. **TCP port 443**, nn::ssl with that host name, then an HTTP WebSocket upgrade:
   ```
   GET / HTTP/1.1
   Host: g23380901-%.s.n.srv.nintendo.net
   Upgrade: websocket
   Connection: upgrade
   Sec-WebSocket-Key: NEX
   Sec-WebSocket-Version: 13
   Sec-WebSocket-Protocol: NEX
   ```
   It accepts a standard `101 Switching Protocols` answer (Sec-WebSocket-Accept computed from the key "NEX").
4. First binary WebSocket message, 18 bytes, resent about every 1 to 2 s for 30 s:
   ```
   80 06 00 00 aa 1f 01 00 40 00 00 00 00 04 06 00 00 00
   ```
   Read as a PRUDP "lite" packet (the NEX transport over WebSocket; layout from the public NEX documentation, still to be
   confirmed by a working answer): magic 0x80, options length 6, payload length 0, stream types 0xaa (source and
   destination 10), source port 0x1f, destination port 1, fragment 0, type/flags 0x0040 (type 0 = SYN, flag NEED_ACK),
   sequence 0, options `00 04 06 00 00 00` (option 0 "supported functions", 4 bytes).
5. No answer to the SYN: the socket is closed after 30 s and the game shows **2306-0502**
   (`nn::err::ShowError`, caller 0x7103346e9c). Without a listener on 443 the same code comes at once.

## Decision

**B1 (our own NEX server)**: the transport is plain enough. TLS is removed in our nn::ssl, WebSocket is standard, and
the NEX payload starts with a PRUDP lite SYN. Next (phase 1): `tools/net/nex_server.py` answers SYN / CONNECT, then the
authentication server's RMC calls, logging every protocol and method it does not know.

## Open

- The `%` in the host name (`lp1` on a console): harmless while we answer every name.
- What follows the SYN-ACK: the authentication (ticket) step, then the secure server address the game connects to next.
- Pia's NAT check (`nncs1/2-%.n.n.srv.nintendo.net`): not asked yet.
