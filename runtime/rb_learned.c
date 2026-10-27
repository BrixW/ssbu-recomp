/* Track R4 (plan/phases/R4_state_map.md sections 6-9): THE LEARNED-SET SAVE. The snapshot ring (r1_ring.c) on the NORMAL game
 * build (ssbu_game_eh_o1.dll + the normal script modules + ssbu_native.exe): no second, "marked" compile of the game DLL and of
 * the modules is needed for rollback. Its own translation unit: what it shares with r1_state.c / r1_ring.c (windows, not-simulation
 * maps, the ring's page ids) is declared in r1_internal.h. No game content in this file: only addresses of the game's allocator entry points
 * and of three allocation sites (13.0.4 addresses, through GA(); hostrt.c maps them to 13.0.5).
 *
 * When: whenever the ring is wanted (SSBU_RB_NET online rollback / its loop test, SSBU_RB_SIM) and NO game DLL with store marks
 * is loaded (ssbu_game_r1.dll of older installs, the study's ssbu_game_p15r1.dll, SSBU_R1_GAMEDLL): with such a DLL the marks
 * are used as before (the fallback while the marked build exists). SSBU_LS=0: never (the ring then needs the marked DLL);
 * SSBU_LS=1: also when the ring is not wanted (SSBU_R1 tests).
 *   - at the ring's base (r1's full save, every guest thread held) every committed read-write page of window 1 (the game's
 *     region) and window 2 (image + modules) that is simulation (not a host stack, not audio output) is made READ-ONLY;
 *   - the first write of such a page faults into ls_write_fault_handler: the page becomes writable for good and joins the LEARNED SET (a
 *     list of page ids). Before it joins, its bytes are still those of the base (nothing wrote it), which the shadow holds:
 *     every older state the ring has finds the right value there;
 *   - a capture takes EVERY page of the learned set (the copy the marked ring makes of the pages marked since the last
 *     capture); a load copies every learned page back (the pages written since the newest capture are among them);
 *   - host code that writes guest memory through the KERNEL (file reads: nnfs.c read_at, nnalbum.c streams) cannot fault: it
 *     calls r1_wrote first, which opens the pages (writable, learned); a decommit makes its pages learned (the ring records
 *     "decommitted"); a commit during the fight makes its new pages read-only (their first write faults). User-mode writers
 *     outside the game code (CRT memcpy, libopus, the exe's helpers, nnldn.c receives) fault like the game code and are served;
 *   - at the end of the fight every page still read-only is made writable again (menus, loads: no protection); the next
 *     match's base (r1_ring_rebase) protects everything again with an empty learned set.
 *   SSBU_LS_CMP (default ON since R3 net3; =0 off): a capture compares each learned page with its newest value first (in parallel, the ring's copy threads)
 *   and copies only the pages that changed (less ring memory, smaller loads; the compare reads both copies).
 *   SSBU_LS_DECAY=<n> (implies CMP; default off): every n captures the pages unchanged for n captures go read-only again.
 * THE PRESENTATION POOL (on with the learned set): the blocks the game's jemalloc would make for three allocation sites proven
 *   not simulation state (R4 sections 3 and 8: the graphics 16 MB buffer, an 8 MB block, the 1.4 MB blocks; list "rt_big3",
 *   built in) are served from a separate reservation of this exe that the ring never sees (not in a window, not an extra:
 *   < 16 GB): never saved, never protected; the pool's bookkeeping is saved with the state (ppool_bookkeeping, an extra). Hooks on
 *   jemalloc's aligned_alloc / posix_memalign / realloc / free / malloc_size in main. A free of a block that is not live there
 *   (a re-run pass freeing again what a real pass freed) is counted and ignored. SSBU_LS_ROUTE=<site file> (lines "<caller>
 *   [<caller's caller>]", hex 13.0.4 addresses) replaces the built-in list (test); SSBU_LS_ROUTE=0: no pool.
 * Hooks (r1_ring.c / r1_state.c): mark_setup (end, threads held) -> ls_start_after_base(); r1_ring_capture / r1_ring_load:
 *   ls_capture_pages / ls_for_each_learned_page instead of rg_sweep when g_ls_active, ls_after_capture; r1_ring_rebase -> ls_new_base(); rgs_step (fight
 *   over) -> ls_fight_over(); r1_on_commit / r1_on_decommit / r1_wrote -> ls_range_event; ra_init: g_mark = mark_bind() || ls_use_exe_maps();
 *   r1_init -> ppool_init(); collect() -> ppool_bookkeeping; rgs_report -> ls_report(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "r1_internal.h"
/* ================================================================ PART 1: THE LEARNED SET
 * What this file needs from r1_state.c and r1_ring.c, and what it gives them, is listed in r1_internal.h:
 *   r1_state.c  PG, WIN2 / WN2, g_win1 / g_wn1 (the windows), g_ns1 / g_ns2 (not-simulation maps), g_dc (decommitted flags),
 *               g_r / g_nr (the ranges r1 saves), pm1 pm2 plo1 pn1 pn2 and r1_m1 r1_m2 r1_lo1 r1_n1 r1_n2 (the mark maps),
 *               cp_add / cp_run / g_ncj / g_cp_cmp (the parallel page compare), g_audio_free, is_async, now_ms, LOG
 *   r1_ring.c   rg_current_slot_pages (pages recorded so far by the capture in progress), rg_src / rg_addr (a page's newest ring
 *               value / its address), rgs_wanted
 *   hostrt.h    Ctx, ImportFn, GA, hook_install, guest_thread_info */
static int g_ls_enabled = -1;                  /* tri-state: -1 not decided yet, 0 off, 1 on (ls_enabled) */
int g_ls_active;                        /* 1 from a base until the fight is over: pages are protected and the set is being built */
static int g_ls_compare_before_copy;           /* SSBU_LS_CMP (default 1): a capture compares each learned page with its newest value first */
static int g_ls_fight_over_done;               /* ls_fight_over already ran for this fight */
static volatile uint8_t *g_ls_page_state;      /* per page id (window 1 pages, then window 2 pages): 0 not tracked, 1 read-only, 2 learned, 3 being opened */
static size_t g_ls_win1_pages, g_ls_total_pages;     /* pages in window 1; pages in both windows */
static uint32_t *g_ls_learned_ids;             /* the learned set: page id + 1 (0 = being added), in the order learned */
static volatile LONG g_ls_learned_count;       /* entries used in g_ls_learned_ids */
static LONG g_ls_learned_capacity = 1 << 21;   /* entries the list can hold */
static volatile LONG g_ls_list_overflow;       /* pages that could not be added because the list was full */
/* statistics, printed by ls_report; none of them changes what the set does */
static volatile LONG64 g_ls_faults, g_ls_fault_mod[5], g_ls_wrote_open, g_ls_dec_learn, g_ls_commit_prot, g_ls_spin, g_ls_refault, g_ls_protfail;
static long long g_ls_prot_pages, g_ls_bases, g_ls_unprot_pages, g_ls_capture_count, g_ls_cap_pages, g_ls_cap_same; static double g_ls_prot_ms, g_ls_cmp_ms;
static HMODULE g_ls_game_module, g_ls_exe_module;      /* module handles, to tell which module a faulting instruction is in */
volatile LONG64 g_read_retry;                    /* nnfs.c read_at: reads repeated after ERROR_NOACCESS (r1_read_retry_note) */
typedef struct { uint64_t off; char mod[40]; long long n; } LsWriter;      /* a writer outside the game code: module, offset, first writes counted */
static LsWriter g_ls_writers[64]; static int g_ls_nwriters; static SRWLOCK g_ls_writers_lock = SRWLOCK_INIT;
/* Returns 1 when a game DLL that carries store marks is loaded (ssbu_game_r1.dll of older installs, the study's
 * ssbu_game_p15r1.dll, or the DLL named by SSBU_R1_GAMEDLL). Then the marks say which pages were written and the learned
 * set is not used. */
static int ls_marked_game_dll_loaded(void) {
    const char *dn = getenv("SSBU_R1_GAMEDLL");
    HMODULE m = GetModuleHandleA(dn && dn[0] ? dn : "ssbu_game_r1.dll");
    if (!m && !(dn && dn[0])) m = GetModuleHandleA("ssbu_game_p15r1.dll");
    return m && GetProcAddress(m, "r1_m1") != NULL;
}
/* Whether the learned-set save is on, decided once and logged. SSBU_LS=0: never. Otherwise only when no marked game DLL
 * is loaded and the ring is wanted (rgs_wanted) or SSBU_LS is set. */
static int ls_enabled(void) {
    if (g_ls_enabled < 0) {
        const char *e = getenv("SSBU_LS"); int marked = ls_marked_game_dll_loaded();
        g_ls_enabled = !(e && e[0] == '0') && !marked && ((e && e[0]) || rgs_wanted());
        LOG("r1: LS (the learned-set save, normal game build): %s\n", g_ls_enabled ? "ON" : e && e[0] == '0' ? "off (SSBU_LS=0)" :
            marked ? "off: a game DLL with marks is loaded, its marks are used" : "off (no rollback ring wanted)");
    }
    return g_ls_enabled;
}
/* When the learned set is on, points the mark-map pointers (pm1, pm2, plo1, pn1, pn2) at this exe's own variables
 * (r1_m1 ...). Nothing marks those but host code, so the ring relies on the learned page set instead of on marks.
 * Returns 1 when the learned set is on, 0 otherwise. */
int ls_use_exe_maps(void) {
    if (!ls_enabled()) return 0;
    pm1 = &r1_m1; pm2 = &r1_m2; plo1 = &r1_lo1; pn1 = &r1_n1; pn2 = &r1_n2;
    LOG("r1: LS: the game DLL has no marks: the ring runs on the learned page set (page protection), maps of the exe\n");
    return 1;
}
/* Page id of address `a`: window 1 pages come first ((a - g_win1) >> 12), then window 2 pages (g_ls_win1_pages +
 * (a - WIN2) >> 12). Returns 1 and sets *id when `a` is in a window, 0 otherwise (extras and every other address have no
 * learned-set state). */
static int ls_page_id(const uint8_t *a, size_t *id) {
    if (g_win1 && a >= g_win1 && a < g_win1 + g_wn1) { *id = (size_t)(a - g_win1) >> 12; return 1; }
    if ((uintptr_t)a >= WIN2 && (uintptr_t)a < WIN2 + WN2) { *id = g_ls_win1_pages + (((uintptr_t)a - WIN2) >> 12); return 1; }
    return 0;
}
/* Address of the page with id `id` (the inverse of ls_page_id). */
static uint8_t *ls_page_address(size_t id) { return id < g_ls_win1_pages ? g_win1 + id * PG : (uint8_t *)(uintptr_t)WIN2 + (id - g_ls_win1_pages) * PG; }
/* 1 when page `id` is marked as not simulation state (a host stack, audio output ...: the maps g_ns1 / g_ns2). Such
 * pages are never protected, learned or saved. */
static int ls_page_is_not_simulation(size_t id) { return id < g_ls_win1_pages ? g_ns1[id] : g_ns2[id - g_ls_win1_pages]; }
static uint32_t *g_ls_learned_capture_no;      /* parallel to g_ls_learned_ids: the number of the capture in which the page was learned or last changed */
static int g_ls_decay_every;                   /* SSBU_LS_DECAY: pages unchanged for this many captures leave the set (0 = never) */
static SRWLOCK g_ls_list_lock = SRWLOCK_INIT;  /* shared by ls_list_append, exclusive for the compaction in ls_decay_pass */
static long long g_ls_decayed, g_ls_decays; static double g_ls_decay_ms;      /* decay statistics */
/* Adds page `id` to the learned set. It is stored as id + 1 (0 means "being added right now") together with the number
 * of the current capture. Called by faulting game threads, several at once; the shared lock only keeps out
 * ls_decay_pass's compaction. When the list is full only the overflow counter grows. */
static void ls_list_append(size_t id) {
    AcquireSRWLockShared(&g_ls_list_lock);
    LONG k = InterlockedIncrement(&g_ls_learned_count) - 1;
    if (k < g_ls_learned_capacity) { g_ls_learned_capture_no[k] = (uint32_t)g_ls_capture_count; g_ls_learned_ids[k] = (uint32_t)id + 1; } else InterlockedIncrement(&g_ls_list_overflow);
    ReleaseSRWLockShared(&g_ls_list_lock);
}
/* Statistics only, changes no state of the set. For a first-write fault, finds which module the faulting instruction is in
 * and counts it: 0 game DLL, 1 a lua2cpp script module, 2 this exe, 3 anything else, 4 unknown. For modules other than the
 * game DLL it also keeps a table of (module, offset) writers (64 entries) that ls_report prints. */
static void ls_count_fault_by_module(EXCEPTION_POINTERS *e) {
    HMODULE m = NULL; char mn[MAX_PATH] = "?"; uint64_t rip = e ? e->ContextRecord->Rip : 0;
    if (!rip || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)rip, &m)) { InterlockedIncrement64(&g_ls_fault_mod[4]); return; }
    int k = m == g_ls_game_module ? 0 : m == g_ls_exe_module ? 2 : 3;
    if (k == 3) { GetModuleFileNameA(m, mn, MAX_PATH); if (strstr(mn, "lua2cpp_")) k = 1; }
    InterlockedIncrement64(&g_ls_fault_mod[k]);
    if (k < 2) return;
    const char *b = strrchr(mn, '\\'); b = b ? b + 1 : mn; if (k == 2) b = "exe";
    uint64_t off = rip - (uint64_t)(uintptr_t)m;
    AcquireSRWLockExclusive(&g_ls_writers_lock);
    int i; for (i = 0; i < g_ls_nwriters; i++) if (g_ls_writers[i].off == off && !strcmp(g_ls_writers[i].mod, b)) break;
    if (i == g_ls_nwriters && g_ls_nwriters < 64) { g_ls_writers[i].off = off; strncpy(g_ls_writers[i].mod, b, 39); g_ls_nwriters++; }
    if (i < 64) g_ls_writers[i].n++;
    ReleaseSRWLockExclusive(&g_ls_writers_lock);
}
/* Makes page `id` writable for good and puts it in the learned set. Returns 1 when the page is writable now (this call
 * did it, or it already was), 0 when the page is not tracked (state 0): the caller must then not handle the fault.
 * g_ls_page_state[id]: 0 not tracked, 1 read-only (not written since the base), 2 learned, 3 being opened by another thread
 * (this call waits for it). `e` is the exception when called from the fault handler (the fault is then counted and
 * attributed to a module), NULL when the host opens the page itself. */
static int ls_open_page(size_t id, EXCEPTION_POINTERS *e) {
    for (;;) {
        uint8_t s = g_ls_page_state[id];
        if (s == 0) return 0;
        if (s == 2) { DWORD old; InterlockedIncrement64(&g_ls_refault); return VirtualProtect(ls_page_address(id), PG, PAGE_READWRITE, &old) ? 1 : 0; }
        if (s == 3) { InterlockedIncrement64(&g_ls_spin); YieldProcessor(); continue; }
        if (__sync_bool_compare_and_swap(&g_ls_page_state[id], (uint8_t)1, (uint8_t)3)) {
            DWORD old; int ok = VirtualProtect(ls_page_address(id), PG, PAGE_READWRITE, &old) != 0;
            if (!ok) InterlockedIncrement64(&g_ls_protfail);
            ls_list_append(id); MemoryBarrier(); g_ls_page_state[id] = 2;
            if (e) { InterlockedIncrement64(&g_ls_faults); ls_count_fault_by_module(e); }
            return ok;
        }
    }
}
/* Vectored exception handler, first in the chain. Handles only a write access violation (ExceptionInformation[0] == 1) on
 * a tracked window page: opens the page and resumes the instruction. Every other exception goes on to the next handler. */
static LONG CALLBACK ls_write_fault_handler(EXCEPTION_POINTERS *e) {
    if (!g_ls_page_state || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    size_t id; if (!ls_page_id((const uint8_t *)(uintptr_t)e->ExceptionRecord->ExceptionInformation[1], &id)) return EXCEPTION_CONTINUE_SEARCH;
    return ls_open_page(id, e) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
/* Makes page ids [a, b) read-only and sets their state to 1. One VirtualProtect call when they are contiguous in memory,
 * page by page when that fails (the run is not one region); failures are counted. */
static void ls_protect_id_range(size_t a, size_t b) {
    if (a >= b) return;
    DWORD old;
    if (VirtualProtect(ls_page_address(a), (b - a) * PG, PAGE_READONLY, &old)) { for (size_t i = a; i < b; i++) g_ls_page_state[i] = 1; g_ls_prot_pages += (long long)(b - a); return; }
    for (size_t i = a; i < b; i++) { if (VirtualProtect(ls_page_address(i), PG, PAGE_READONLY, &old)) { g_ls_page_state[i] = 1; g_ls_prot_pages++; } else g_ls_protfail++; }
}
/* At a new base, with the guest threads held: empties the learned set and makes every committed read-write simulation page
 * of both windows read-only (state 1), going by the ranges r1 collected (g_r). Pages marked not-simulation are left
 * alone. Sets g_ls_active. */
static void ls_protect_all_pages(void) {
    double t0 = now_ms(); long long before = g_ls_prot_pages;
    memset((void *)g_ls_page_state, 0, g_ls_total_pages); g_ls_learned_count = 0; g_ls_list_overflow = 0;
    for (int i = 0; i < g_nr; i++) {
        size_t id0, idn; uint8_t *a = g_r[i].a; size_t np = g_r[i].n / PG;
        if (!ls_page_id(a, &id0) || !ls_page_id(a + (np - 1) * PG, &idn) || idn != id0 + np - 1) continue;      /* not (wholly) in one window */
        size_t run = id0;
        for (size_t id = id0; id < id0 + np; id++) if (ls_page_is_not_simulation(id)) { ls_protect_id_range(run, id); run = id + 1; }
        ls_protect_id_range(run, id0 + np);
    }
    g_ls_prot_ms += now_ms() - t0; g_ls_bases++; g_ls_active = 1; g_ls_fight_over_done = 0;
    LOG("r1: LS: %lld pages (%.1f MB) made read-only in %.1f ms; learned set emptied\n", g_ls_prot_pages - before, (g_ls_prot_pages - before) * (double)PG / 1048576.0, now_ms() - t0);
}
/* Called when r1's first full save (the base) is made, threads still held. The first time: allocates the state table and
 * the list, reads the options (SSBU_LS_CMP, default on; SSBU_LS_DECAY), looks up the game DLL's and the exe's module
 * handles and installs ls_write_fault_handler. Every time: ls_protect_all_pages. Does nothing when the learned set is off
 * or a marked game DLL is loaded. */
void ls_start_after_base(void) {
    if (!ls_enabled() || pm1 != &r1_m1) return;               /* (a marked game DLL is loaded: the marks are used) */
    g_ls_win1_pages = (size_t)(g_wn1 >> 12); g_ls_total_pages = g_ls_win1_pages + (size_t)(WN2 >> 12);
    if (!g_ls_page_state) {
        g_ls_page_state = VirtualAlloc(NULL, g_ls_total_pages, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_ls_learned_ids = VirtualAlloc(NULL, (size_t)g_ls_learned_capacity * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_ls_learned_capture_no = VirtualAlloc(NULL, (size_t)g_ls_learned_capacity * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_ls_page_state || !g_ls_learned_ids || !g_ls_learned_capture_no) { LOG("r1: LS: allocation FAILED: off\n"); g_ls_page_state = NULL; g_ls_enabled = 0; return; }
        g_ls_compare_before_copy = !(getenv("SSBU_LS_CMP") && getenv("SSBU_LS_CMP")[0] == '0');      /* (R3 net3) default ON: the same tick time, a quarter of the ring memory (SSBU_LS_CMP=0: every learned page copied) */
        if (getenv("SSBU_LS_DECAY")) { g_ls_decay_every = atoi(getenv("SSBU_LS_DECAY")); if (g_ls_decay_every > 0) g_ls_compare_before_copy = 1; }
        g_ls_exe_module = GetModuleHandleA(NULL);
        {   const char *names[] = {getenv("SSBU_R1_GAMEDLL"), "ssbu_game.dll", "ssbu_game_p15n3.dll", "ssbu_game_eh_o1.dll", "ssbu_game_1305x.dll"};
            for (int i = 0; i < 5 && !g_ls_game_module; i++) if (names[i] && names[i][0]) g_ls_game_module = GetModuleHandleA(names[i]); }
        AddVectoredExceptionHandler(1, ls_write_fault_handler);
    }
    ls_protect_all_pages();
}
/* Threads held. Calls fn(id, in_window2, index_in_window) for every page of the learned set except pages marked
 * not-simulation and (with SSBU_R1_AUDIO_FREE) asynchronous pages. The ring uses it in a load (restore every learned page)
 * and in a capture that does not compare first. */
void ls_for_each_learned_page(void (*fn)(uint32_t id, int w, size_t j)) {
    LONG n = g_ls_learned_count; if (n > g_ls_learned_capacity) n = g_ls_learned_capacity;
    for (LONG k = 0; k < n; k++) {
        uint32_t v = g_ls_learned_ids[k]; if (!v) continue;     /* (being added right now by a host thread: the next capture has it) */
        size_t id = v - 1; int w = id >= g_ls_win1_pages; size_t j = w ? id - g_ls_win1_pages : id;
        if (w ? g_ns2[j] : g_ns1[j]) continue;           /* not simulation (r1_nosim after it was learned) */
        if (g_audio_free && is_async(ls_page_address(id))) continue;
        fn((uint32_t)id, w, j);
    }
}
/* SSBU_LS_CMP: the capture's pages compared first (the ring's copy threads); only the changed ones go to rg_cap_page.
 * SSBU_LS_DECAY=<n> (implies the compare): every n captures, the learned pages that did not change in the last n captures
 * are made read-only again and leave the set (their newest ring value is their present value); their next write faults
 * and brings them back. The set then follows the pages a fight writes NOW instead of every page it ever wrote. */
static uint32_t *g_ls_cmp_ids, *g_ls_cmp_list_index; static uint8_t *g_ls_cmp_changed; static size_t g_ls_cmp_capacity, g_ls_cmp_count;
/* Decay (SSBU_LS_DECAY=n), run with the threads held after a capture's copies: a learned page that did not change in the
 * last n captures (g_ls_learned_capture_no) goes read-only again and leaves the set. Its newest ring value is its present
 * value, and its next write faults and brings it back. The list is compacted under the exclusive lock. */
static void ls_decay_pass(void) {
    double t0 = now_ms(); long long nd = 0;
    AcquireSRWLockExclusive(&g_ls_list_lock);
    LONG n = g_ls_learned_count; if (n > g_ls_learned_capacity) n = g_ls_learned_capacity; LONG m = 0;
    for (LONG k = 0; k < n; k++) {
        uint32_t v = g_ls_learned_ids[k]; if (!v) continue;
        size_t id = v - 1; int w = id >= g_ls_win1_pages; size_t j = w ? id - g_ls_win1_pages : id; DWORD old;
        if ((uint32_t)g_ls_capture_count - g_ls_learned_capture_no[k] >= (uint32_t)g_ls_decay_every && g_ls_page_state[id] == 2 && !ls_page_is_not_simulation(id) && (w || !g_dc[j]) &&
            VirtualProtect(ls_page_address(id), PG, PAGE_READONLY, &old)) { g_ls_page_state[id] = 1; nd++; continue; }
        g_ls_learned_ids[m] = v; g_ls_learned_capture_no[m] = g_ls_learned_capture_no[k]; m++;
    }
    g_ls_learned_count = m;
    ReleaseSRWLockExclusive(&g_ls_list_lock);
    g_ls_decayed += nd; g_ls_decays++; g_ls_decay_ms += now_ms() - t0;
    if (g_ls_decays <= 3 || g_ls_decays % 20 == 0) LOG("r1: LS decay %lld: %lld pages read-only again in %.2f ms, set now %ld pages\n", g_ls_decays, nd, now_ms() - t0, (long)m);
}
/* Called by r1_ring_capture instead of the marked-page sweep. Without the compare, fn gets every learned page. With it
 * (the default), every learned page is queued for a parallel compare with its newest ring value (cp_add; a page that is
 * zero or decommitted is taken as it is) and fn gets only the pages that changed; a changed page's capture number is
 * refreshed so the decay keeps it in the set. */
void ls_capture_pages(void (*fn)(uint32_t id, int w, size_t j)) {
    g_ls_capture_count++;
    if (!g_ls_compare_before_copy) { long long b = rg_current_slot_pages(); ls_for_each_learned_page(fn); g_ls_cap_pages += rg_current_slot_pages() - b; return; }
    double t0 = now_ms();
    if (g_ls_cmp_capacity < (size_t)g_ls_learned_count + 1024) { g_ls_cmp_capacity = (size_t)g_ls_learned_count * 2 + 65536; free(g_ls_cmp_ids); free(g_ls_cmp_list_index); free(g_ls_cmp_changed); g_ls_cmp_ids = malloc(g_ls_cmp_capacity * 4); g_ls_cmp_list_index = malloc(g_ls_cmp_capacity * 4); g_ls_cmp_changed = malloc(g_ls_cmp_capacity); }
    g_ls_cmp_count = 0; g_ncj = 0;
    LONG n = g_ls_learned_count; if (n > g_ls_learned_capacity) n = g_ls_learned_capacity; if ((size_t)n > g_ls_cmp_capacity) n = (LONG)g_ls_cmp_capacity;
    for (LONG k = 0; k < n; k++) {
        uint32_t v = g_ls_learned_ids[k]; if (!v) continue;
        uint32_t id = v - 1; int w = id >= g_ls_win1_pages; size_t j = w ? id - g_ls_win1_pages : id;
        if (w ? g_ns2[j] : g_ns1[j]) continue;
        if (g_audio_free && is_async(ls_page_address(id))) continue;
        uint8_t *src = rg_src(id);
        g_ls_cmp_list_index[g_ls_cmp_count] = (uint32_t)k;
        if (src == NULL || src == (uint8_t *)1 || (!w && g_dc[j])) { g_ls_cmp_ids[g_ls_cmp_count++] = id | 0x80000000u; continue; }   /* zero / decommitted: taken as it is */
        g_ls_cmp_ids[g_ls_cmp_count++] = id; cp_add(rg_addr(id), src);
    }
    g_cp_cmp = g_ls_cmp_changed; cp_run(); g_cp_cmp = NULL;    /* the queued (now, newest value) pairs: g_ls_cmp_changed[q] = 1 if they differ */
    g_ls_cmp_ms += now_ms() - t0;
    size_t q = 0;
    for (size_t i = 0; i < g_ls_cmp_count; i++) {
        uint32_t id = g_ls_cmp_ids[i] & 0x7fffffffu; int w = id >= g_ls_win1_pages; size_t j = w ? id - g_ls_win1_pages : id;
        if (!(g_ls_cmp_ids[i] & 0x80000000u) && !g_ls_cmp_changed[q++]) { g_ls_cap_same++; continue; }
        g_ls_learned_capture_no[g_ls_cmp_list_index[i]] = (uint32_t)g_ls_capture_count;
        fn(id, w, j); g_ls_cap_pages++;
    }
}
/* Called after a capture's copies, threads held: runs the decay every g_ls_decay_every captures. */
void ls_after_capture(void) {
    if (g_ls_active && g_ls_decay_every > 0 && g_ls_capture_count % g_ls_decay_every == 0) ls_decay_pass();
}
/* A new base inside a fight (r1_ring_rebase, threads held): protects everything again with an empty learned set. */
void ls_new_base(void) {
    if (!g_ls_page_state) return;
    ls_protect_all_pages();
}
/* The fight ended: every page still read-only is made writable again (menus and loading need no protection) and its state
 * goes back to 0. Logs the totals once per fight; does nothing when already done. */
void ls_fight_over(void) {
    if (!g_ls_page_state || g_ls_fight_over_done) return;
    g_ls_active = 0; g_ls_fight_over_done = 1;
    double t0 = now_ms(); long long n = 0;
    for (size_t a = 0; a < g_ls_total_pages;) {
        if (g_ls_page_state[a] != 1) { a++; continue; }
        size_t b = a; while (b < g_ls_total_pages && g_ls_page_state[b] == 1 && (b == a || ls_page_address(b) == ls_page_address(b - 1) + PG)) b++;
        DWORD old;
        if (VirtualProtect(ls_page_address(a), (b - a) * PG, PAGE_READWRITE, &old)) n += (long long)(b - a);
        else for (size_t i = a; i < b; i++) if (VirtualProtect(ls_page_address(i), PG, PAGE_READWRITE, &old)) n++;
        for (size_t i = a; i < b; i++) g_ls_page_state[i] = 0;
        a = b;
    }
    g_ls_unprot_pages += n;
    LOG("r1: LS: fight over: %lld pages writable again in %.1f ms; learned set %ld pages (%.1f MB); faults %lld (game DLL %lld, script modules %lld, exe %lld, other %lld, ?%lld)\n",
        n, now_ms() - t0, (long)g_ls_learned_count, g_ls_learned_count * (double)PG / 1048576.0, (long long)g_ls_faults, (long long)g_ls_fault_mod[0], (long long)g_ls_fault_mod[1], (long long)g_ls_fault_mod[2], (long long)g_ls_fault_mod[3], (long long)g_ls_fault_mod[4]);
}
/* Called for memory changes that do not come as a page fault, while the learned set is active. what = 0: host code wrote
 * guest memory through the kernel (file reads, which cannot fault): read-only pages are opened. 1: a decommit: the pages
 * become learned (the ring records "decommitted"). 2: a commit during the fight: new pages go read-only unless they are
 * learned. Addresses outside both windows and not-simulation pages are ignored. */
void ls_range_event(uint64_t a, uint64_t n, int what) {
    if (!g_ls_page_state || !n || !g_ls_active) return;
    size_t run = (size_t)-1, id = 0;
    for (uint64_t p = a & ~(uint64_t)(PG - 1); p < a + n; p += PG) {
        if (!ls_page_id((const uint8_t *)(uintptr_t)p, &id)) continue;
        if (ls_page_is_not_simulation(id)) continue;
        if (what == 0) { if (g_ls_page_state[id] == 1 || g_ls_page_state[id] == 3) { ls_open_page(id, NULL); InterlockedIncrement64(&g_ls_wrote_open); } }
        else if (what == 1) { if (g_ls_page_state[id] == 1) ls_open_page(id, NULL); else if (g_ls_page_state[id] == 0) { g_ls_page_state[id] = 2; ls_list_append(id); } InterlockedIncrement64(&g_ls_dec_learn); }
        else if (g_ls_page_state[id] <= 1) { DWORD old; if (VirtualProtect((void *)(uintptr_t)p, PG, PAGE_READONLY, &old)) { g_ls_page_state[id] = 1; InterlockedIncrement64(&g_ls_commit_prot); } }   /* (a commit over committed pages makes them read-write again) */
        (void)run;
    }
}
/* ================================================================ PART 2: THE PRESENTATION POOL (SSBU_LS_ROUTE)
 * (a custom allocator for three jemalloc call sites that are not simulation state; independent of PART 1 except that it is
 * only set up when the learned set is on)
 * Its BLOCKS are never saved; its BOOKKEEPING (which block is live, the bump pointer, the size classes) is: one 2 MB region
 * that r1 copies whole as an "extra" (collect() -> ppool_bookkeeping), so a load puts it back with the rest of the state, as the
 * ring did for jemalloc's own metadata when these sites were only left out (rb_state_map.c SSBU_SM_XSITE, the proof): a block
 * the simulation freed after the loaded frame is live again (a second free of it in the re-run is a normal free), a block made
 * after it is free again. Blocks are found by bitmap (no free list inside the blocks: their bytes are not rolled back). */
#define PPOOL_RESERVE (8ull << 30)                              /* < 16 GB: r1 takes reservations of >= 16 GB as game regions */
#define PPOOL_CHUNK (1ull << 20)
#define PPOOL_NCHUNKS (PPOOL_RESERVE / PPOOL_CHUNK)
#define PPOOL_NCLASSES 16                                        /* classes 16 B << c, c < 16 (up to 512 KB); larger: whole 1 MB chunks */
#define PPOOL_CHUNKS_PER_CLASS 512                                       /* chunks per class at most */
#define PPOOL_BITMAP_BYTES (1792u << 10)
typedef struct {
    uint64_t bump, bm_used; long long live, made;
    uint32_t ncls[PPOOL_NCLASSES], cur[PPOOL_NCLASSES], list[PPOOL_NCLASSES][PPOOL_CHUNKS_PER_CLASS];
    uint8_t cls[PPOOL_NCHUNKS];                                 /* 0 unused, 1 + class, 100 large head, 101 large tail, 102 large head freed */
    uint32_t lsz[PPOOL_NCHUNKS], bmo[PPOOL_NCHUNKS];
    uint8_t bm[PPOOL_BITMAP_BYTES];
} PPoolMeta;
static uint8_t *g_ppool_blocks; static PPoolMeta *g_ppool_meta;
static SRWLOCK g_ppool_lock = SRWLOCK_INIT;
static long long g_ppool_blocks_made, g_ppool_bytes, g_ppool_frees, g_ppool_double_frees, g_ppool_reallocs, g_ppool_large_blocks, g_ppool_no_room;
#define PPOOL_MAX_SITES 1024
static uint64_t g_ppool_sites[PPOOL_MAX_SITES][2]; static int g_ppool_nsites; static long long g_ppool_site_hits[PPOOL_MAX_SITES], g_ppool_site_double_frees[PPOOL_MAX_SITES];
/* The pool's bookkeeping region (PPoolMeta) and its size rounded up to whole pages, so r1 copies it as an extra with the rest
 * of the state. Size 0 and NULL when there is no pool. */
uint8_t *ppool_bookkeeping(size_t *n) { *n = g_ppool_meta ? (sizeof(PPoolMeta) + PG - 1) & ~(size_t)(PG - 1) : 0; return (uint8_t *)g_ppool_meta; }
/* 1 when address `p` lies inside the pool's reservation. */
static int ppool_contains(uint64_t p) { return g_ppool_blocks && p >= (uint64_t)(uintptr_t)g_ppool_blocks && p < (uint64_t)(uintptr_t)g_ppool_blocks + PPOOL_RESERVE; }
/* Lock held. A block of `n` bytes aligned to `align` from the pool, or 0 when there is no room (the caller then asks
 * jemalloc). Blocks up to 512 KB come from 1 MB chunks cut into size classes of 16 B << c, free slots found by a per-chunk
 * bitmap; larger ones take whole chunks (a freed large block with the same chunk count is reused). */
static uint64_t ppool_alloc(uint64_t n, uint64_t align) {
    PPoolMeta *m = g_ppool_meta;
    if (align < 16) align = 16;
    uint64_t need = n > align ? n : align; if (need < 16) need = 16;
    if (need <= (PPOOL_CHUNK >> 1)) {
        int c = 0; while ((16ull << c) < need) c++;
        uint64_t sz = 16ull << c, slots = PPOOL_CHUNK / sz, words = (slots + 63) / 64;
        for (uint32_t t = 0; t < m->ncls[c]; t++) {
            uint32_t idx = (m->cur[c] + t) % m->ncls[c], ch = m->list[c][idx];
            uint64_t *w = (uint64_t *)(m->bm + m->bmo[ch]);
            for (uint64_t k = 0; k < words; k++) {
                if (w[k] == ~0ull) continue;
                uint64_t bit = (uint64_t)__builtin_ctzll(~w[k]), slot = k * 64 + bit;
                if (slot >= slots) break;
                w[k] |= 1ull << bit; m->cur[c] = idx; m->live++; m->made++; g_ppool_blocks_made++; g_ppool_bytes += (long long)sz;
                return (uint64_t)(uintptr_t)g_ppool_blocks + (uint64_t)ch * PPOOL_CHUNK + slot * sz;
            }
        }
        uint32_t bytes = (uint32_t)((words * 8 + 7) & ~7ull);
        if (m->ncls[c] >= PPOOL_CHUNKS_PER_CLASS || m->bump >= PPOOL_NCHUNKS || m->bm_used + bytes > PPOOL_BITMAP_BYTES) { g_ppool_no_room++; return 0; }
        uint32_t ch = (uint32_t)m->bump;
        if (!VirtualAlloc(g_ppool_blocks + (uint64_t)ch * PPOOL_CHUNK, PPOOL_CHUNK, MEM_COMMIT, PAGE_READWRITE)) { g_ppool_no_room++; return 0; }
        m->bump++; m->bmo[ch] = (uint32_t)m->bm_used; m->bm_used += bytes; memset(m->bm + m->bmo[ch], 0, bytes);
        m->cls[ch] = (uint8_t)(1 + c); m->list[c][m->ncls[c]] = ch; m->cur[c] = m->ncls[c]; m->ncls[c]++;
        *(uint64_t *)(m->bm + m->bmo[ch]) = 1; m->live++; m->made++; g_ppool_blocks_made++; g_ppool_bytes += (long long)sz;
        return (uint64_t)(uintptr_t)g_ppool_blocks + (uint64_t)ch * PPOOL_CHUNK;
    }
    if (align > PPOOL_CHUNK) return 0;
    uint32_t nch = (uint32_t)((need + PPOOL_CHUNK - 1) / PPOOL_CHUNK);
    for (uint64_t ch = 0; ch < m->bump; ch++) if (m->cls[ch] == 102 && m->lsz[ch] == nch) { m->cls[ch] = 100; m->live++; m->made++; g_ppool_blocks_made++; g_ppool_large_blocks++; return (uint64_t)(uintptr_t)g_ppool_blocks + ch * PPOOL_CHUNK; }
    if (m->bump + nch > PPOOL_NCHUNKS || !VirtualAlloc(g_ppool_blocks + m->bump * PPOOL_CHUNK, (uint64_t)nch * PPOOL_CHUNK, MEM_COMMIT, PAGE_READWRITE)) { g_ppool_no_room++; return 0; }
    uint64_t c0 = m->bump; m->bump += nch;
    m->cls[c0] = 100; for (uint32_t i = 1; i < nch; i++) m->cls[c0 + i] = 101; m->lsz[c0] = nch;
    m->live++; m->made++; g_ppool_blocks_made++; g_ppool_large_blocks++; g_ppool_bytes += (long long)nch * (long long)PPOOL_CHUNK;
    return (uint64_t)(uintptr_t)g_ppool_blocks + c0 * PPOOL_CHUNK;
}
/* Size in bytes of the live block at `p` (its class size, or its chunks); 0 when it is not a live block start. */
static uint64_t ppool_block_size(uint64_t p) {
    uint64_t off = p - (uint64_t)(uintptr_t)g_ppool_blocks; uint64_t ch = off / PPOOL_CHUNK; uint8_t k = g_ppool_meta->cls[ch];
    if (k >= 1 && k <= PPOOL_NCLASSES) return 16ull << (k - 1);
    if (k == 100) return (uint64_t)g_ppool_meta->lsz[ch] * PPOOL_CHUNK;
    return 0;
}
/* Lock held. Frees the block at `p`. Returns 1 when it was live and 0 for a second free of the same block, which is counted
 * and ignored (a re-run pass frees again what a real pass already freed). */
static int ppool_free(uint64_t p) {
    PPoolMeta *m = g_ppool_meta; uint64_t off = p - (uint64_t)(uintptr_t)g_ppool_blocks, ch = off / PPOOL_CHUNK; uint8_t k = m->cls[ch];
    if (k >= 1 && k <= PPOOL_NCLASSES) {
        uint64_t sz = 16ull << (k - 1), o = off % PPOOL_CHUNK, slot = o / sz; uint64_t *w = (uint64_t *)(m->bm + m->bmo[ch]);
        if (o % sz || !(w[slot / 64] & (1ull << (slot % 64)))) { g_ppool_double_frees++; return 0; }
        w[slot / 64] &= ~(1ull << (slot % 64)); m->live--; g_ppool_frees++; return 1;
    }
    if (k == 100 && !(off % PPOOL_CHUNK)) { m->cls[ch] = 102; m->live--; g_ppool_frees++; return 1; }
    g_ppool_double_frees++; return 0;
}
/* which guest threads make each routed site's blocks (a site the sound thread allocates from is rolled back with that thread) */
static const char *g_ppool_site_thread_name[PPOOL_MAX_SITES][4]; static long long g_ppool_site_thread_count[PPOOL_MAX_SITES][4];
static __declspec(thread) const char *t_ppool_thread_name; static __declspec(thread) int t_ppool_thread_named;
/* Lock held. For the report: counts which guest thread allocated from routed site `s` (the current thread's name is looked up
 * once per thread; up to 4 names per site). */
static void ppool_note_allocating_thread(int s) {
    if (!t_ppool_thread_named) {
        Ctx *tc; const char *nm; HANDLE h; DWORD me = GetCurrentThreadId(); t_ppool_thread_name = "host";
        for (int i = 0; guest_thread_info(i, &tc, &nm, &h); i++) if (h && GetThreadId(h) == me) { t_ppool_thread_name = nm ? nm : "?"; break; }
        t_ppool_thread_named = 1;
    }
    for (int k = 0; k < 4; k++) {
        if (g_ppool_site_thread_name[s][k] == t_ppool_thread_name || (g_ppool_site_thread_name[s][k] && !strcmp(g_ppool_site_thread_name[s][k], t_ppool_thread_name))) { g_ppool_site_thread_count[s][k]++; return; }
        if (!g_ppool_site_thread_name[s][k]) { g_ppool_site_thread_name[s][k] = t_ppool_thread_name; g_ppool_site_thread_count[s][k] = 1; return; }
    }
}
/* Index of the routed site whose caller address is `lr` (and whose caller's caller is `lr2` when the site lists one), or -1
 * when the call does not come from a routed site. */
static int ppool_find_site(uint64_t lr, uint64_t lr2) {
    for (int i = 0; i < g_ppool_nsites; i++) if (g_ppool_sites[i][0] == lr && (!g_ppool_sites[i][1] || g_ppool_sites[i][1] == lr2)) return i;
    return -1;
}
/* which site a pool block came from (for the second-free report): a small table of block -> site */
#define PPOOL_BLOCK_TABLE_N (1u << 20)
static uint64_t *g_ppool_block_ptr; static int16_t *g_ppool_block_site;
/* Remembers that block `p` came from routed site `s` (hash table, linear probing over 64 slots), for the second-free report. */
static void ppool_block_site_put(uint64_t p, int s) { if (!g_ppool_block_ptr) return; uint32_t h = (uint32_t)((p * 0x9e3779b97f4a7c15ull) >> 44) & (PPOOL_BLOCK_TABLE_N - 1); for (int i = 0; i < 64; i++) { uint32_t k = (h + i) & (PPOOL_BLOCK_TABLE_N - 1); if (!g_ppool_block_ptr[k] || g_ppool_block_ptr[k] == p) { g_ppool_block_ptr[k] = p; g_ppool_block_site[k] = (int16_t)s; return; } } }
/* The routed site block `p` came from, or -1 when unknown. */
static int ppool_block_site_get(uint64_t p) { if (!g_ppool_block_ptr) return -1; uint32_t h = (uint32_t)((p * 0x9e3779b97f4a7c15ull) >> 44) & (PPOOL_BLOCK_TABLE_N - 1); for (int i = 0; i < 64; i++) { uint32_t k = (h + i) & (PPOOL_BLOCK_TABLE_N - 1); if (g_ppool_block_ptr[k] == p) return g_ppool_block_site[k]; if (!g_ppool_block_ptr[k]) return -1; } return -1; }
static ImportFn g_ppool_orig_aligned_alloc, g_ppool_orig_posix_memalign, g_ppool_orig_realloc, g_ppool_orig_free, g_ppool_orig_malloc_size;
/* The return address of the hooked function's caller, read through the frame pointer (x29), or 0 when the frame pointer does
 * not look valid. */
static uint64_t caller_of_caller(Ctx *c) { uint64_t fp = c->x[29]; return (fp > 0x10000 && !(fp & 7)) ? *(uint64_t *)(uintptr_t)(fp + 8) : 0; }
/* Hook on the game's jemalloc aligned_alloc(align, size): a call from a routed site is served from the pool, any other call
 * (or one the pool has no room for) goes to the original. */
static void ppool_hook_aligned_alloc(Ctx *c, void *u) {
    (void)u; int s = ppool_find_site(c->x[30], caller_of_caller(c));
    if (s < 0) { g_ppool_orig_aligned_alloc(c); return; }
    AcquireSRWLockExclusive(&g_ppool_lock); uint64_t r = ppool_alloc(c->x[1], c->x[0]); if (r) { g_ppool_site_hits[s]++; ppool_note_allocating_thread(s); ppool_block_site_put(r, s); } ReleaseSRWLockExclusive(&g_ppool_lock);
    if (!r) { g_ppool_orig_aligned_alloc(c); return; }
    c->x[0] = r;
}
/* Hook on posix_memalign(&p, align, size): as ppool_hook_aligned_alloc, the block's address is written through the pointer. */
static void ppool_hook_posix_memalign(Ctx *c, void *u) {
    (void)u; int s = ppool_find_site(c->x[30], caller_of_caller(c));
    if (s < 0) { g_ppool_orig_posix_memalign(c); return; }
    uint64_t mp = c->x[0];
    AcquireSRWLockExclusive(&g_ppool_lock); uint64_t r = ppool_alloc(c->x[2], c->x[1]); if (r) { g_ppool_site_hits[s]++; ppool_note_allocating_thread(s); ppool_block_site_put(r, s); } ReleaseSRWLockExclusive(&g_ppool_lock);
    if (!r || !mp) { g_ppool_orig_posix_memalign(c); return; }
    *(uint64_t *)(uintptr_t)mp = r; c->x[0] = 0;
}
/* Hook on realloc(p, size): a block that lives in the pool is resized inside it (copied to a new pool block when it grows);
 * realloc(NULL, size) from a routed site is served from the pool; every other call goes to the original. */
static void ppool_hook_realloc(Ctx *c, void *u) {
    (void)u; uint64_t p = c->x[0], n = c->x[1];
    if (p && ppool_contains(p)) {
        AcquireSRWLockExclusive(&g_ppool_lock);
        uint64_t have = ppool_block_size(p), r = p; int s = ppool_block_site_get(p);
        if (n > have || !n) { r = ppool_alloc(n ? n : 16, 16); if (r) { memcpy((void *)(uintptr_t)r, (void *)(uintptr_t)p, (size_t)(have < n ? have : n)); ppool_free(p); ppool_block_site_put(r, s); } }
        g_ppool_reallocs++;
        ReleaseSRWLockExclusive(&g_ppool_lock);
        c->x[0] = r; return;
    }
    int s = p ? -1 : ppool_find_site(c->x[30], caller_of_caller(c));
    if (s < 0) { g_ppool_orig_realloc(c); return; }
    AcquireSRWLockExclusive(&g_ppool_lock); uint64_t r = ppool_alloc(n ? n : 16, 16); if (r) { g_ppool_site_hits[s]++; ppool_note_allocating_thread(s); ppool_block_site_put(r, s); } ReleaseSRWLockExclusive(&g_ppool_lock);
    if (!r) { g_ppool_orig_realloc(c); return; }
    c->x[0] = r;
}
/* Hook on free(p): a pool block is freed in the pool (a second free is counted and ignored); anything else goes to the original. */
static void ppool_hook_free(Ctx *c, void *u) {
    (void)u; uint64_t p = c->x[0];
    if (!ppool_contains(p)) { g_ppool_orig_free(c); return; }
    AcquireSRWLockExclusive(&g_ppool_lock); if (!ppool_free(p)) { int s = ppool_block_site_get(p); if (s >= 0) g_ppool_site_double_frees[s]++; } ReleaseSRWLockExclusive(&g_ppool_lock);
}
/* Hook on malloc_size(p): the size of a pool block, answered from the pool; anything else goes to the original. */
static void ppool_hook_malloc_size(Ctx *c, void *u) {
    (void)u; uint64_t p = c->x[0];
    if (!ppool_contains(p)) { g_ppool_orig_malloc_size(c); return; }
    AcquireSRWLockShared(&g_ppool_lock); c->x[0] = ppool_block_size(p); ReleaseSRWLockShared(&g_ppool_lock);
}
/* Called from r1_init. Sets up the presentation pool when the learned set is on and SSBU_LS_ROUTE is not "0" or empty: reads
 * the list of routed sites (the built-in rt_big3, or the file named by SSBU_LS_ROUTE, for tests), reserves the blocks'
 * region (never saved) and the bookkeeping region (saved with the state), allocates the block-to-site table and installs
 * the five jemalloc hooks. */
void ppool_init(void) {
    const char *fn = getenv("SSBU_LS_ROUTE");
    if (!ls_enabled() || (fn && (!*fn || !strcmp(fn, "0")))) return;
    if (fn) {                                            /* (test) a site file instead of the built-in list */
        FILE *f = fopen(fn, "r"); if (!f) { LOG("r1: LS: presentation pool: site file %s NOT found\n", fn); return; }
        char ln[256]; unsigned long long a, b;
        while (fgets(ln, sizeof ln, f) && g_ppool_nsites < PPOOL_MAX_SITES) {
            if (ln[0] == '#') continue;
            int k = sscanf(ln, "%llx %llx", &a, &b); if (k < 1) continue;
            g_ppool_sites[g_ppool_nsites][0] = GA(a); g_ppool_sites[g_ppool_nsites][1] = k > 1 ? GA(b) : 0; g_ppool_nsites++;
        }
        fclose(f);
    } else {                                             /* rt_big3 (R4 section 8.0 item 4: IDENTICAL on 12 matches, 11 fighters) */
        static const uint64_t big3[3][2] = {
            {0x7103816944ull, 0},                        /* the graphics 16 MB buffer */
            {0x7103559e08ull, 0x71000a3fd8ull},          /* an 8 MB block */
            {0x7103760cf8ull, 0x7100081980ull},          /* the 1.4 MB blocks */
        };
        for (int i = 0; i < 3; i++) { g_ppool_sites[g_ppool_nsites][0] = GA(big3[i][0]); g_ppool_sites[g_ppool_nsites][1] = big3[i][1] ? GA(big3[i][1]) : 0; g_ppool_nsites++; }
        fn = "the built-in list (rt_big3)";
    }
    for (uint64_t hint = 0x40000000000ull; !g_ppool_blocks && hint < 0x80000000000ull; hint += 0x400000000ull)      /* away from window 2 (0x7100000000 + 8 GB) */
        g_ppool_blocks = VirtualAlloc((void *)(uintptr_t)hint, PPOOL_RESERVE, MEM_RESERVE, PAGE_READWRITE);
    for (uint64_t hint = 0x80000000000ull; !g_ppool_meta && hint < 0xC0000000000ull; hint += 0x10000000ull)
        g_ppool_meta = VirtualAlloc((void *)(uintptr_t)hint, sizeof(PPoolMeta), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_ppool_block_ptr = calloc(PPOOL_BLOCK_TABLE_N, 8); g_ppool_block_site = calloc(PPOOL_BLOCK_TABLE_N, 2);
    if (!g_ppool_blocks || !g_ppool_meta) { LOG("r1: LS: presentation pool: allocation FAILED: off\n"); g_ppool_nsites = 0; return; }
    int h = !!hook_install(GA(0x710392dce0ull), ppool_hook_aligned_alloc, NULL, &g_ppool_orig_aligned_alloc);
    h += !!hook_install(GA(0x710392dc40ull), ppool_hook_posix_memalign, NULL, &g_ppool_orig_posix_memalign);
    h += !!hook_install(GA(0x710392dd80ull), ppool_hook_realloc, NULL, &g_ppool_orig_realloc);
    h += !!hook_install(GA(0x710392e590ull), ppool_hook_free, NULL, &g_ppool_orig_free);
    h += !!hook_install(GA(0x710392e8a0ull), ppool_hook_malloc_size, NULL, &g_ppool_orig_malloc_size);
    LOG("r1: LS: presentation pool on: %d sites from %s, blocks %p + %.0f GB (never saved), bookkeeping %p + %.1f MB (saved with the state), jemalloc hooks %d of 5\n", g_ppool_nsites, fn, (void *)g_ppool_blocks, PPOOL_RESERVE / 1073741824.0, (void *)g_ppool_meta, sizeof(PPoolMeta) / 1048576.0, h);
}
/* End-of-run report of the learned set and of the pool: counts, first-write faults by module, decay, capture costs and
 * pool use per site. Prints nothing when neither was used. */
void ls_report(void) {
    if (!g_ls_page_state && !g_ppool_nsites) return;
    if (g_ls_page_state) {
        LOG("r1: LS report: bases %lld (%.1f ms protecting), pages made read-only %lld, writable again at fight ends %lld; learned set %ld pages (%.1f MB; list overflow %ld); first-write faults %lld: game DLL %lld, script modules %lld, exe %lld, other %lld, unknown %lld; repeated faults %lld, busy waits %lld, protect failures %lld; kernel-write opens (r1_wrote) %lld, decommits learned %lld, commits made read-only %lld; file reads repeated after ERROR_NOACCESS %lld\n",
            g_ls_bases, g_ls_prot_ms, g_ls_prot_pages, g_ls_unprot_pages, (long)g_ls_learned_count, g_ls_learned_count * (double)PG / 1048576.0, (long)g_ls_list_overflow, (long long)g_ls_faults, (long long)g_ls_fault_mod[0], (long long)g_ls_fault_mod[1], (long long)g_ls_fault_mod[2], (long long)g_ls_fault_mod[3], (long long)g_ls_fault_mod[4],
            (long long)g_ls_refault, (long long)g_ls_spin, (long long)g_ls_protfail, (long long)g_ls_wrote_open, (long long)g_ls_dec_learn, (long long)g_ls_commit_prot, (long long)g_read_retry);
        if (g_ls_decays) LOG("r1: LS decay every %d captures: %lld runs, %lld pages made read-only again (%.0f per run), %.3f ms per run\n", g_ls_decay_every, g_ls_decays, g_ls_decayed, (double)g_ls_decayed / g_ls_decays, g_ls_decay_ms / g_ls_decays);
        LOG("r1: LS captures %lld: pages taken %.0f per capture%s (compare %.3f ms per capture, %.0f unchanged pages skipped per capture)\n", g_ls_capture_count, g_ls_capture_count ? (double)g_ls_cap_pages / g_ls_capture_count : 0, g_ls_compare_before_copy ? " (changed only)" : " (the whole set)",
            g_ls_capture_count ? g_ls_cmp_ms / g_ls_capture_count : 0, g_ls_capture_count ? (double)g_ls_cap_same / g_ls_capture_count : 0);
        for (int i = 0; i < g_ls_nwriters; i++) LOG("r1: LS   writer not in the game code: %s+%llx  %lld first writes\n", g_ls_writers[i].mod, (unsigned long long)g_ls_writers[i].off, g_ls_writers[i].n);
    }
    if (g_ppool_nsites) {
        LOG("r1: LS presentation pool: %lld blocks made (%lld large), %.1f MB, live now %lld, chunks %llu, bitmap %.0f KB; frees %lld, frees of a block not live %lld, reallocs %lld, no room (went to jemalloc) %lld\n", g_ppool_blocks_made, g_ppool_large_blocks, g_ppool_bytes / 1048576.0, g_ppool_meta->live, (unsigned long long)g_ppool_meta->bump, g_ppool_meta->bm_used / 1024.0, g_ppool_frees, g_ppool_double_frees, g_ppool_reallocs, g_ppool_no_room);
        for (int i = 0; i < g_ppool_nsites; i++) if (g_ppool_site_hits[i] || g_ppool_site_double_frees[i]) LOG("r1: LS   site %llx %llx: %lld blocks, %lld second frees; threads %s %lld, %s %lld, %s %lld, %s %lld\n", (unsigned long long)g_ppool_sites[i][0], (unsigned long long)g_ppool_sites[i][1], g_ppool_site_hits[i], g_ppool_site_double_frees[i], g_ppool_site_thread_name[i][0] ? g_ppool_site_thread_name[i][0] : "-", g_ppool_site_thread_count[i][0], g_ppool_site_thread_name[i][1] ? g_ppool_site_thread_name[i][1] : "-", g_ppool_site_thread_count[i][1], g_ppool_site_thread_name[i][2] ? g_ppool_site_thread_name[i][2] : "-", g_ppool_site_thread_count[i][2], g_ppool_site_thread_name[i][3] ? g_ppool_site_thread_name[i][3] : "-", g_ppool_site_thread_count[i][3]);
    }
}
