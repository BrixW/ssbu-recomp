/* OLDER EXPERIMENT, round 4 (SSBU_R1_MARK_PROT=1): finds who writes without marking, by page protection.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "../game_state_internal.h"

/* ---------------------------------------------------------------- R1 round 4: who writes without marking (SSBU_R1_MARK_PROT=1, debug)
 * After a marked save every committed page of the windows is made read-only; the first write to a page faults here:
 * a page the lifted code marked just before its store is simply made writable again; a page NOT marked is a miss:
 * the writer (host thread, guest thread name, module + offset of the faulting instruction, the guest function the
 * thread entered last) is counted, the page is marked and made writable. At the restore everything is writable again. */

static Site g_site[512]; static int g_nsite;
static SRWLOCK g_site_lock = SRWLOCK_INIT;
static volatile LONG g_prot_faults, g_prot_misses;
int win_index(uint8_t *a, uint8_t **map, size_t *pi) {
    if (g_win1 && a >= g_win1 && a < g_win1 + g_wn1) { *map = *pm1; *pi = (size_t)(a - g_win1) >> 12; return 1; }
    if ((uintptr_t)a >= WIN2 && (uintptr_t)a < WIN2 + WN2) { *map = *pm2; *pi = (size_t)((uintptr_t)a - WIN2) >> 12; return 1; }
    return 0;
}
LONG CALLBACK prot_veh(EXCEPTION_POINTERS *e) {
    if (!g_prot_on || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *a = (uint8_t *)(uintptr_t)e->ExceptionRecord->ExceptionInformation[1], *map; size_t pi;
    if (!win_index(a, &map, &pi)) return EXCEPTION_CONTINUE_SEARCH;
    uint8_t *pg = (uint8_t *)((uintptr_t)a & ~(uintptr_t)(PG - 1));
    InterlockedIncrement(&g_prot_faults);
    if (!map[pi]) {                                     /* a write that did not mark */
        InterlockedIncrement(&g_prot_misses);
        uint64_t rip = e->ContextRecord->Rip;
        HMODULE m = NULL; char mn[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)rip, &m)) GetModuleFileNameA(m, mn, MAX_PATH);
        const char *base = strrchr(mn, '\\'); base = base ? base + 1 : mn;
        Ctx *tc; const char *nm; HANDLE h; const char *thr = "main or host"; uint64_t gfn = 0;
        for (int i = 0; guest_thread_info(i, &tc, &nm, &h); i++) if (h && GetThreadId(h) == GetCurrentThreadId()) { thr = nm ? nm : "?"; gfn = tc->fn; }
        uint64_t off = rip - (uint64_t)(uintptr_t)m, ret = *(uint64_t *)(uintptr_t)e->ContextRecord->Rsp, caller = ret - (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
        AcquireSRWLockExclusive(&g_site_lock);
        int i; for (i = 0; i < g_nsite; i++) if (g_site[i].rip == off && g_site[i].caller == caller && !strcmp(g_site[i].mod, base) && g_site[i].thr == thr) break;
        if (i == g_nsite && g_nsite < 512) { g_site[i].rip = off; g_site[i].caller = caller; strncpy(g_site[i].mod, base, 47); g_site[i].thr = thr; g_site[i].gfn = gfn; g_nsite++; }
        if (i < 512) g_site[i].n++;
        ReleaseSRWLockExclusive(&g_site_lock);
        map[pi] = 1;
    }
    DWORD old; VirtualProtect(pg, PG, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}
void prot_all(int on) {                          /* the committed RW parts of the saved ranges inside the windows */
    double t0 = now_ms(); size_t n = 0;
    for (int i = 0; i < g_nr; i++) {
        uint8_t *map; size_t pi;
        if (!win_index(g_r[i].a, &map, &pi)) continue;
        MEMORY_BASIC_INFORMATION mq;
        for (uint8_t *p = g_r[i].a; p < g_r[i].a + g_r[i].n && VirtualQuery(p, &mq, sizeof mq); p = (uint8_t *)mq.BaseAddress + mq.RegionSize) {
            uint8_t *x = (uint8_t *)mq.BaseAddress, *y = x + mq.RegionSize;
            if (x < g_r[i].a) x = g_r[i].a;
            if (y > g_r[i].a + g_r[i].n) y = g_r[i].a + g_r[i].n;
            if (mq.State != MEM_COMMIT) continue;
            if (on && (mq.Protect & 0xff) != PAGE_READWRITE) continue;
            DWORD old; VirtualProtect(x, (size_t)(y - x), on ? PAGE_READONLY : PAGE_READWRITE, &old); n += (size_t)(y - x);
        }
    }
    g_prot_on = on;
    if (g_cycle < 3) LOG("r1: PROT %s: %.1f MB in %.1f ms\n", on ? "read-only" : "writable again", n / 1048576.0, now_ms() - t0);
}
void prot_open(uint64_t a, uint64_t n) {        /* a host writer that marked first (kernel reads): its pages writable again */
    uint8_t *map; size_t pi;
    for (uint64_t p = a & ~(uint64_t)(PG - 1); p < a + n; p += PG)
        if (win_index((uint8_t *)(uintptr_t)p, &map, &pi)) { MEMORY_BASIC_INFORMATION mq; DWORD old; if (VirtualQuery((void *)(uintptr_t)p, &mq, sizeof mq) && mq.State == MEM_COMMIT && (mq.Protect & 0xff) == PAGE_READONLY) VirtualProtect((void *)(uintptr_t)p, PG, PAGE_READWRITE, &old); }
}
void prot_report(void) {
    LOG("r1: PROT: %ld first-write faults, %ld of them on pages NOT marked; writers (module+offset, thread, last guest function, count):\n", g_prot_faults, g_prot_misses);
    for (int k = 0; k < g_nsite; k++) LOG("r1:   %s+%llx  [%s]  f_%llx  %d\n", g_site[k].mod, (unsigned long long)g_site[k].rip, g_site[k].thr, (unsigned long long)g_site[k].gfn, g_site[k].n);
}
void mark_report(void) {
    if (!g_nms) return;
    double s = 0, r = 0, sc = 0; size_t ps = 0, pr = 0;
    for (int i = 0; i < g_nms; i++) { s += g_ms_s[i]; sc += g_ms_scan[i]; ps += g_pg_s[i]; }
    for (int i = 0; i < g_nmr; i++) { r += g_ms_r[i]; pr += g_pg_r[i]; }
    qsort(g_ms_s, (size_t)g_nms, sizeof(double), cmp_d); qsort(g_ms_r, (size_t)g_nmr, sizeof(double), cmp_d);
    LOG("r1: MARK timing: %d saves: mean %.3f ms (of it the map sweep + page copy %.3f), median %.3f, p99 %.3f, %.0f pages per save (incl. the frames since the last restore); "
        "%d restores: mean %.3f ms, median %.3f, p99 %.3f, %.0f pages per restore; extras %.1f MB each time; check: %lld missed pages in %d checked restores\n",
        g_nms, s / g_nms, sc / g_nms, g_ms_s[g_nms / 2], g_ms_s[g_nms * 99 / 100], (double)ps / g_nms,
        g_nmr, g_nmr ? r / g_nmr : 0, g_nmr ? g_ms_r[g_nmr / 2] : 0, g_nmr ? g_ms_r[g_nmr * 99 / 100] : 0, g_nmr ? (double)pr / g_nmr : 0, g_xbytes / 1048576.0, g_miss_total, g_miss_cycles);
    if (g_mcheck) LOG("r1: MARK check: of the %lld missed pages %lld were marked again since the restore (written during the hold by a thread that is not held)\n", g_miss_total, g_miss_remarked);
}
/* R1 diagnosis: on an access violation, which host thread and which fiber's host stack (by RSP) it happened on */
static void veh_fib_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber);
static uint64_t g_veh_rsp; static int g_veh_hits;
static void veh_fib_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    if (!fiber) return;
    uint64_t sb = *(uint64_t *)((uint8_t *)fiber + 0x10), da = *(uint64_t *)((uint8_t *)fiber + 0x20);
    if (g_veh_rsp >= da && g_veh_rsp < sb) {
        int i; for (i = 0; i < g_nfib; i++) if (g_fib[i].key == key) break;
        LOG("r1: CRASH on the host stack of fiber %llx (guest stack %llx..%llx, ctx %p fn %llx sp %llx tid-now %lu; at the last save: %s)\n", (unsigned long long)key,
            (unsigned long long)stack, (unsigned long long)(stack + size), (void *)c, c ? (unsigned long long)c->fn : 0ull, c ? (unsigned long long)c->x[31] : 0ull, (unsigned long)os_r1_fiber_tid(key),
            i == g_nfib ? "not in the list" : !g_fib[i].c ? "never run" : g_fib[i].susp ? "switched away (host state saved)" : "running");
        g_veh_hits++;
    }
}
LONG CALLBACK r1_veh(EXCEPTION_POINTERS *e) {
    static volatile LONG n;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if ((code != EXCEPTION_ACCESS_VIOLATION && code != 0xC00000FFu && code != 0xC0000409u && code != 0xC0000028u) || (g_cycle == 0 && g_phase < 2)) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&n) > 2) return EXCEPTION_CONTINUE_SEARCH;
    g_veh_rsp = e->ContextRecord->Rsp; g_veh_hits = 0;
    Ctx *tc; const char *nm; HANDLE h; const char *tn = "main or host";
    for (int i = 0; guest_thread_info(i, &tc, &nm, &h); i++) if (h && GetThreadId(h) == GetCurrentThreadId()) tn = nm ? nm : "?";
    LOG("r1: CRASH %08lx at rip %llx addr %llx, host thread %lu (%s), rsp %llx, phase %d cycle %d\n", (unsigned long)code, (unsigned long long)e->ContextRecord->Rip,
        (unsigned long long)e->ExceptionRecord->ExceptionInformation[1], (unsigned long)GetCurrentThreadId(), tn, (unsigned long long)g_veh_rsp, g_phase, g_cycle);
    { HMODULE m = NULL; char mn[MAX_PATH] = "?"; if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)e->ContextRecord->Rip, &m)) GetModuleFileNameA(m, mn, MAX_PATH);
      LOG("r1: CRASH module %s +%llx; rcx %llx rdx %llx; TEB fiber data %p; [rsp] %llx %llx %llx %llx\n", mn, (unsigned long long)(e->ContextRecord->Rip - (uint64_t)(uintptr_t)m), (unsigned long long)e->ContextRecord->Rcx, (unsigned long long)e->ContextRecord->Rdx, GetCurrentFiber(),
          (unsigned long long)((uint64_t *)(uintptr_t)g_veh_rsp)[0], (unsigned long long)((uint64_t *)(uintptr_t)g_veh_rsp)[1], (unsigned long long)((uint64_t *)(uintptr_t)g_veh_rsp)[2], (unsigned long long)((uint64_t *)(uintptr_t)g_veh_rsp)[3]); }
    os_r1_fibers(veh_fib_cb);
    if (!g_veh_hits) LOG("r1: CRASH rsp is on no fiber's host stack\n");
    return EXCEPTION_CONTINUE_SEARCH;
}
