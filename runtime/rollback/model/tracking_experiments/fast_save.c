/* OLDER EXPERIMENT, round 2 (SSBU_R1_FAST=1): the fast save. Kept because tests/r1_*.py use it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "../game_state_internal.h"
static void fast_check(void);

/* ---------------------------------------------------------------- R1 round 2: the fast save (SSBU_R1_FAST=1)
 * The first SSBU_R1_LEARN cycles (default 5) are full ones (above): they learn the pages the simulation changes (union
 * of the pages that differed at the restores) and the pages other threads write on their own (async: changed during a
 * 30 ms hold of main while the other threads RUN: audio, vsync counter, loaders), which are left out. From then on a
 * save copies only the learned pages + the catch table + the fibers' host state + main's registers, with no thread
 * held; a restore copies them back. SSBU_R1_FAST_CHECK=1 also takes the full snapshot and, after the fast restore,
 * counts the pages that still differ (the learned set missed them). */
uint8_t **g_async; size_t g_nasync, g_async_cap;
static int cmp_ptr(const void *a, const void *b) { uint8_t *x = *(uint8_t *const *)a, *y = *(uint8_t *const *)b; return x < y ? -1 : x > y; }
static int g_use_async = -1;
void async_add(uint8_t *a) {
    for (size_t i = 0; i < g_nasync; i++) if (g_async[i] == a) return;
    if (g_nasync >= g_async_cap) { g_async_cap = g_async_cap ? g_async_cap * 2 : 4096; g_async = realloc(g_async, g_async_cap * sizeof *g_async); }   /* (threads are not held here: after unpark) */
    g_async[g_nasync++] = a;
    qsort(g_async, g_nasync, sizeof *g_async, cmp_ptr);
}
int is_async(uint8_t *a) { if (g_use_async < 0) g_use_async = getenv("SSBU_R1_FAST_ASYNC") != NULL || g_audio_free; return g_use_async && g_nasync && bsearch(&a, g_async, g_nasync, sizeof *g_async, cmp_ptr) != NULL; }
void async_learn(void) {               /* main held 30 ms, the others running: what do they write? (ranges collected, buffers allocated) */
    for (int i = 0; i < g_memory_map.range_count; i++) memcpy(g_memory_map.ranges[i].s, g_memory_map.ranges[i].a, g_memory_map.ranges[i].n);
    Sleep(30);
    size_t added = 0;
    for (int i = 0; i < g_memory_map.range_count; i++)
        for (size_t p = 0; p < g_memory_map.ranges[i].n / PG; p++)
            if (memcmp(g_memory_map.ranges[i].a + p * PG, g_memory_map.ranges[i].s + p * PG, PG)) {
                uint8_t *a = g_memory_map.ranges[i].a + p * PG;
                if (is_async(a)) continue;
                if (g_nasync >= g_async_cap) { g_async_cap = g_async_cap ? g_async_cap * 2 : 4096; g_async = realloc(g_async, g_async_cap * sizeof *g_async); }
                g_async[g_nasync++] = a; added++;
                qsort(g_async, g_nasync, sizeof *g_async, cmp_ptr);
            }
    LOG("r1: async pages (written by other threads while main was held 30 ms, threads running): %zu new, %zu in all\n", added, g_nasync);
}
static uint8_t **g_fpages; size_t g_nfp, g_fcap; static uint8_t *g_fbuf; static uint8_t g_catch_copy[8192 * 16];
static Ctx g_fctx;

static double g_fs_ms[4096], g_fr_ms[4096], g_fsh_ms[4096], g_frh_ms[4096]; static int g_nfs, g_nfr;
void fast_build(void) {
    g_nfp = 0;
    for (int j = 0; j < g_nl; j++) for (size_t p = 0; p < g_l[j].n / PG; p++) if (g_l[j].learn[p] && !is_async(g_l[j].a + p * PG)) g_nfp++;
    g_fcap = g_nfp + 16384; g_fpages = realloc(g_fpages, g_fcap * sizeof *g_fpages); g_fbuf = realloc(g_fbuf, g_fcap * PG);
    size_t k = 0;
    for (int j = 0; j < g_nl; j++) for (size_t p = 0; p < g_l[j].n / PG; p++) if (g_l[j].learn[p] && !is_async(g_l[j].a + p * PG)) g_fpages[k++] = g_l[j].a + p * PG;
    LOG("r1: FAST set: %zu pages = %.1f MB (learned %zu, %zu async left out)\n", g_nfp, g_nfp * PG / 1048576.0, g_learn_pages, g_nasync);
}
void fast_save(Ctx *c) {
    double t0 = now_ms();
    park(0);
    double th = now_ms();
    if (g_session.cycle < 10) slow_report("at the save");
    for (size_t i = 0; i < g_nfp; i++) memcpy(g_fbuf + i * PG, g_fpages[i], PG);
    if (g_catch_addr) memcpy(g_catch_copy, (void *)(uintptr_t)g_catch_addr, sizeof g_catch_copy);
    fibers_save();
    unpark();
    g_fctx = *c;
    double t1 = now_ms();
    if (g_nfs < 4096) { g_fs_ms[g_nfs] = t1 - t0; g_fsh_ms[g_nfs] = th - t0; g_nfs++; }
    if (!g_fcheck) LOG("r1: cycle %d SAVE at fight frame %lld (fast): %.3f ms (holding threads %.3f)\n", g_session.cycle, g_session.fight_frame, t1 - t0, th - t0);
}
void fast_restore(Ctx *c) {
    double t0 = now_ms();
    park(1);
    double th = now_ms();
    for (size_t i = 0; i < g_nfp; i++) memcpy(g_fpages[i], g_fbuf + i * PG, PG);
    if (g_catch_addr) memcpy((void *)(uintptr_t)g_catch_addr, g_catch_copy, sizeof g_catch_copy);
    restore_thread_regs();
    fibers_restore_all();
    double tc = now_ms();
    if (g_fcheck) fast_check();                        /* debug safety net, still holding the threads */
    th += now_ms() - tc;                               /* (its time is not counted as restore time) */
    t0 += now_ms() - tc;
    unpark();
    *c = g_fctx;
    double t1 = now_ms();
    if (g_nfr < 4096) { g_fr_ms[g_nfr] = t1 - t0; g_frh_ms[g_nfr] = th - t0; g_nfr++; }
    LOG("r1: cycle %d RESTORE at fight frame %lld (fast): %.3f ms (holding threads %.3f; %d threads not at their save wait, %d fibers not restored)\n", g_session.cycle, g_session.fight_frame, t1 - t0, th - t0, g_guest_threads.mismatch, g_guest_fibers.not_restored);
}

int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
void fast_report(void) {
    if (!g_nfs) return;
    qsort(g_fs_ms, (size_t)g_nfs, sizeof(double), cmp_d); qsort(g_fr_ms, (size_t)g_nfr, sizeof(double), cmp_d);
    qsort(g_fsh_ms, (size_t)g_nfs, sizeof(double), cmp_d); qsort(g_frh_ms, (size_t)g_nfr, sizeof(double), cmp_d);
    LOG("r1: FAST hold of the guest threads: save median %.3f p99 %.3f ms, restore median %.3f p99 %.3f ms\n", g_fsh_ms[g_nfs / 2], g_fsh_ms[g_nfs * 99 / 100], g_frh_ms[g_nfr / 2], g_frh_ms[g_nfr * 99 / 100]);
    double ss = 0, rs = 0; for (int i = 0; i < g_nfs; i++) ss += g_fs_ms[i]; for (int i = 0; i < g_nfr; i++) rs += g_fr_ms[i];
    LOG("r1: FAST timing over %d saves / %d restores: save mean %.3f median %.3f p99 %.3f max %.3f ms; restore mean %.3f median %.3f p99 %.3f max %.3f ms (%zu pages + catch table + fibers)\n",
        g_nfs, g_nfr, ss / g_nfs, g_fs_ms[g_nfs / 2], g_fs_ms[g_nfs * 99 / 100], g_fs_ms[g_nfs - 1], rs / g_nfr, g_fr_ms[g_nfr / 2], g_fr_ms[g_nfr * 99 / 100], g_fr_ms[g_nfr - 1], g_nfp);
}
/* SSBU_R1_FAST_CHECK: after a fast restore, the full snapshot taken at the same save says which pages the set missed */
static void fast_check(void) {
    size_t miss = 0; int shown = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {
        Rng *r = &g_memory_map.ranges[i];
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(r->a, &m, sizeof m) || m.State != MEM_COMMIT || (uint8_t *)m.BaseAddress + m.RegionSize < r->a + r->n) continue;
        for (size_t p = 0; p < r->n / PG; p++)
            if (!is_async(r->a + p * PG) && memcmp(r->a + p * PG, r->s + p * PG, PG)) {
                miss++;
                if (shown++ < 10) LOG("r1:   FAST missed %s\n", where(r->a + p * PG));
                memcpy(r->a + p * PG, r->s + p * PG, PG);
                if (g_nfp < g_fcap) g_fpages[g_nfp++] = r->a + p * PG;          /* the set learns it (room made in fast_build: no allocation while threads are held) */
            }
    }
    LOG("r1: FAST check: %zu pages outside the set differed after the fast restore (put back from the full snapshot)\n", miss);
}
