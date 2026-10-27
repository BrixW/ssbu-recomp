# Rollback (MVC-style layout)

Online rollback for the recompiled game: each tick the game runs a frame with the remote player's pad GUESSED; when the real
pad arrives and differs, the game state is put back to before that frame and the frames run again.

| Layer | Folder | Job | May call |
|---|---|---|---|
| Controller | `controller/` | The per-tick loop: read pads, run a frame, notice a wrong guess, order a load, run the frames again | model, services |
| Model | `model/` | The data: pad history (`input_history.c`), the saved game state (`game_state.c`), the ring of the last 8 states (`snapshot_ring.c`), which pages the game writes (`page_tracker_learned.c`) | nothing above it |
| Services | `services/` | Things the controller uses that know nothing about memory: wire format, clock sync, the network session and match handshake | nothing above it |

Public contract of the game-free parts: `rollback.h`. What `page_tracker_learned.c` shares with `game_state.c` / `snapshot_ring.c`:
`model/game_state_internal.h`.

Where each file used to be: `rb.h` -> `rollback.h`, `rb_input.c` -> `model/input_history.c`, `rb_learned.c` -> `model/page_tracker_learned.c`,
`r1_ring.c` -> `model/snapshot_ring.c`, `r1_state.c` -> `model/game_state.c`, `r1_internal.h` -> `model/game_state_internal.h`,
`rb_rollback.c` -> `controller/rollback_controller.c`, `rb_net.c` -> `services/net_session.c`, `rb_wire.c` -> `services/wire_format.c`,
`rb_timesync.c` -> `services/clock_sync.c`, `rb_hs.h` -> `services/handshake.h`.

Still to do: `game_state.c` (about 2000 lines) and `snapshot_ring.c` (about 1000) are one translation unit with `rollback_controller.c`;
the simulation / profiler / verifier code in `snapshot_ring.c` moves to `debug/`.
