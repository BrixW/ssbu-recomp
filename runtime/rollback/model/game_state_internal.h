/* The contract between the rollback state files. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * r1_state.c (the process-wide save: windows, shadow copy, page compare, thread table) includes r1_ring.c (the snapshot ring)
 * and rb_rollback.c, so those three are still ONE translation unit. rb_learned.c (which pages the game writes, and the
 * presentation pool) is its own translation unit: this header is everything it shares with the others, in both directions.
 * Nothing else may be reached across the boundary. Keep the list short; when something here can go, remove it.
 *
 * Only Windows builds use these files (the Linux / macOS build of r1_state.c is a set of stubs and has no rb_learned.c). */
#ifndef R1_INTERNAL_H
#define R1_INTERNAL_H
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

/* ---- what r1_state.c provides ------------------------------------------------------------------------------------------ */
#define PG 4096                              /* the page size every table here is counted in */
#define WIN2 0x7100000000ull                 /* window 2: the guest image and the loaded modules start here ... */
#define WN2 0x200000000ull                   /* ... and it is this many bytes long */
#define LOG(...) do { fprintf(g_log, __VA_ARGS__); fflush(g_log); } while (0)      /* a line of the r1 log file */
extern FILE *g_log;                          /* the r1 log file */

/* A run of guest memory the save covers (a, n bytes), with its copy in the shadow (s) and per-page flags. */
typedef struct { uint8_t *a; size_t n; uint8_t *s; uint8_t *dirty; uint8_t *learn; uint8_t *async; } Rng;   /* dirty / learn: one byte per page */
extern Rng g_r[];                            /* the ranges r1 saves ... */
extern int g_nr;                             /* ... and how many of them there are */

extern uint8_t *g_win1;                      /* window 1: where the game's big address region starts ... */
extern uint64_t g_wn1;                       /* ... and its size in bytes */
extern uint8_t *g_dc;                        /* window 1, one byte per page: 1 = the page is decommitted */
extern uint8_t *g_ns1, *g_ns2;               /* "not simulation" flags, one byte per page of window 1 / window 2 (host stacks, audio output) */
extern int g_audio_free;                     /* SSBU_R1_AUDIO_FREE: asynchronously written (audio) pages are left out of the save */

/* The write-mark maps. A game DLL built with store marks writes a 1 in m1 / m2 for each page it writes; pm1 pm2 plo1 pn1 pn2 point at
 * wherever that DLL keeps them. ls_use_exe_maps points them at the exe's own r1_m1 r1_m2 r1_lo1 r1_n1 r1_n2 instead, which nothing marks. */
extern uint8_t **pm1, **pm2; extern uint64_t *plo1, *pn1, *pn2;
__declspec(dllexport) extern uint8_t *r1_m1, *r1_m2;
__declspec(dllexport) extern uint64_t r1_lo1, r1_n1, r1_n2;

extern size_t g_ncj;                         /* page jobs queued for the parallel copy / compare */
extern uint8_t *g_cp_cmp;                    /* when set, the queued jobs are COMPARED (g_cp_cmp[i] = 1 if the pages differ) instead of copied */
void cp_add(uint8_t *d, const uint8_t *s);   /* queues the page copy / compare d <-> s */
void cp_run(void);                           /* runs every queued job on the main and helper threads, then empties the queue */
int is_async(uint8_t *a);                    /* 1 when page `a` is written asynchronously (audio output) and left out of the save */
double now_ms(void);                         /* milliseconds on the high-resolution clock */

int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);      /* hostrt.c: a hook on a game function */
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h);                           /* the i-th guest thread, 0 when there is none */

/* ---- what r1_ring.c provides ------------------------------------------------------------------------------------------- */
int rgs_wanted(void);                        /* 1 when the ring is wanted (online rollback, or the SSBU_RB_SIM test) */
uint8_t *rg_addr(uint32_t id);               /* address of the page with ring id `id` (window 1, window 2, then the extras) */
uint8_t *rg_src(uint32_t id);                /* the bytes of that page at the newest capture (NULL = zero page, (uint8_t *)1 = decommitted) */
long long rg_current_slot_pages(void);       /* pages the capture in progress has recorded so far (0 when none is in progress) */

/* ---- what rb_learned.c provides ---------------------------------------------------------------------------------------- */
extern int g_ls_active;                      /* 1 from a base until the fight is over: pages are protected and the learned set is being built */
extern volatile LONG64 g_read_retry;         /* nnfs.c read_at: reads repeated after ERROR_NOACCESS */

int ls_use_exe_maps(void);                   /* learned set on: use the exe's own mark maps and return 1; otherwise 0 */
void ls_start_after_base(void);              /* the base was just saved (threads held): protect the windows and start learning */
void ls_for_each_learned_page(void (*fn)(uint32_t id, int w, size_t j));      /* fn(page id, in window 2, index in the window) for each learned page */
void ls_capture_pages(void (*fn)(uint32_t id, int w, size_t j));              /* the same, but only the pages that changed since the newest capture */
void ls_after_capture(void);                 /* after a capture's copies (threads held): drop pages that stopped changing, if SSBU_LS_DECAY is set */
void ls_new_base(void);                      /* a new base inside a fight: protect everything again with an empty set */
void ls_fight_over(void);                    /* the fight ended: make every page writable again */
void ls_range_event(uint64_t a, uint64_t n, int what);      /* memory changed without a page fault: 0 host write, 1 decommit, 2 commit */
void ls_report(void);                        /* end-of-run report of the learned set and the pool */
uint8_t *ppool_bookkeeping(size_t *n);       /* the presentation pool's bookkeeping region, so it is saved with the rest; *n is its size */
void ppool_init(void);                       /* sets up the presentation pool (once, from r1_init) */

#endif
