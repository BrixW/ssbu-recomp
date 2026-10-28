/* STATE MEASUREMENT (SSBU_STATE_MEASURE=1; unset or 0 = off, nothing runs). SPDX-License-Identifier: GPL-3.0-or-later
 * Milestone M0 of the save-state plan: measure, without changing behaviour, whether the invariant a save state needs actually holds.
 *
 * The invariant: when a state is loaded, every simulation thread is parked at the SAME wait, with the SAME host call chain, and
 * the same wait objects (events, semaphores, queues) hold the same values, as when that state was captured. If it holds, restoring
 * guest memory + each thread's registers is enough. If it does not hold for some thread or object, that is what a rollback leaves
 * out of step, and the numbers here say which one and how often.
 *
 * At every capture (guest threads held) it records, per guest thread: a fingerprint of the host call chain (the return addresses
 * into the lifted game and module DLLs found on the thread's host stack, hashed in order) and, per nn::os object, its scalar state
 * (signalled, count, length). At every load, after the threads are held again and BEFORE anything is restored, it compares:
 *   - the wait the thread is blocked at now against the one it had at the capture (the same test park() makes),
 *   - the call-chain fingerprint,
 *   - the guest registers: the callee-saved ones (restore_thread_regs puts those back) and all the others,
 *   - every object's scalar state.
 * The tables are printed every 100 loads and at the end of the run (rgs_report / rbg_report). A host stack is scanned for words that
 * point into the lifted code (a word that only looks like one is possible: the same words give the same hash, so a comparison
 * between two moments of the same thread is still meaningful, but a hash is not proof of identical frames). */
#include "../model/game_state_internal.h"
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

#define SM_SLOTS 32                  /* captures kept: more than the ring's depth */
#define SM_MAXTH 256
#define SM_MAXOBJ 8192
#define SM_NAMES 64
#define SM_TOPOBJ 16

typedef struct { const Ctx *key; uint64_t chain; int depth; } SmThread;      /* depth -1: could not be read, -2: thread not held */
typedef struct { uint64_t key; int sig, cnt; uint64_t len; } SmObj;
typedef struct { long long frame; long long seq; int nth, nobj; SmThread th[SM_MAXTH]; SmObj obj[SM_MAXOBJ]; } SmEntry;
typedef struct { char name[40]; long loads, same_wait, other_wait, chain_same, chain_diff, chain_unknown, callee_diff, other_diff; } SmStat;
typedef struct { uint64_t key; long times; int sig0, cnt0, sig1, cnt1; uint64_t len0, len1; } SmTop;
typedef struct { uint64_t lo, hi; } SmMod;

static int g_sm_on = -1;
static SmEntry *g_sm; static long long g_sm_seq;
static SmStat g_sm_stat[SM_NAMES]; static int g_sm_nstat;
static SmTop g_sm_top[SM_TOPOBJ]; static int g_sm_ntop;
static long g_sm_loads, g_sm_loads_no_entry, g_sm_obj_loads_changed, g_sm_obj_changed_total, g_sm_obj_new_total, g_sm_obj_changed_max, g_sm_obj_capped;
static SmMod g_sm_mods[256]; static int g_sm_nmods; static ULONGLONG g_sm_mods_t;
static SmObj g_sm_now[SM_MAXOBJ]; static int g_sm_nnow;

static int sm_enabled(void) {
    if (g_sm_on < 0) {
        const char *e = getenv("SSBU_STATE_MEASURE"); g_sm_on = e && e[0] && e[0] != '0';
        if (g_sm_on) { g_sm = calloc(SM_SLOTS, sizeof *g_sm); if (!g_sm) g_sm_on = 0; else LOG("r1: STATE MEASURE on (SSBU_STATE_MEASURE): per-thread wait / call chain / registers and every nn::os object are compared at each load\n"); }
    }
    return g_sm_on;
}

/* The lifted code lives in ssbu_game*.dll and lua2cpp_*.dll: the address ranges of those modules, refreshed every 3 s (modules are loaded at run time). */
static void sm_modules(void) {
    ULONGLONG now = GetTickCount64();
    if (g_sm_nmods && now - g_sm_mods_t < 3000) return;
    g_sm_mods_t = now; g_sm_nmods = 0;
    HMODULE mods[512]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof mods[0] && g_sm_nmods < 256; i++) {
        char nm[64]; MODULEINFO mi;
        if (!GetModuleBaseNameA(GetCurrentProcess(), mods[i], nm, sizeof nm)) continue;
        if (_strnicmp(nm, "ssbu_game", 9) && _strnicmp(nm, "lua2cpp", 7)) continue;
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        g_sm_mods[g_sm_nmods].lo = (uint64_t)(uintptr_t)mi.lpBaseOfDll; g_sm_mods[g_sm_nmods].hi = g_sm_mods[g_sm_nmods].lo + mi.SizeOfImage; g_sm_nmods++;
    }
}
static int sm_in_lifted(uint64_t v) { for (int i = 0; i < g_sm_nmods; i++) if (v >= g_sm_mods[i].lo && v < g_sm_mods[i].hi) return 1; return 0; }

/* Hash, in order, of the words on the suspended thread's host stack that point into the lifted code (its guest call chain). */
static void sm_chain(const Th *t, uint64_t *chain, int *depth) {
    *chain = 0; *depth = -2;
    if (!t->held || !t->h) return;
    CONTEXT cx; memset(&cx, 0, sizeof cx); cx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(t->h, &cx)) { *depth = -1; return; }
    uint64_t rsp = cx.Rsp & ~7ull; MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)rsp, &mi, sizeof mi) || mi.State != MEM_COMMIT) { *depth = -1; return; }
    uint64_t end = (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize; if (end > rsp + 65536) end = rsp + 65536;
    uint64_t h = 0xcbf29ce484222325ull; int n = 0;
    __try { for (uint64_t p = rsp; p + 8 <= end; p += 8) { uint64_t v = *(volatile uint64_t *)(uintptr_t)p; if (sm_in_lifted(v)) { for (int b = 0; b < 8; b++) { h ^= (v >> (8 * b)) & 0xff; h *= 0x100000001b3ull; } n++; } } }
    __except (EXCEPTION_EXECUTE_HANDLER) { *depth = -1; return; }
    *chain = h; *depth = n;
}

static void sm_obj_cb(uint64_t key, int sig, int cnt, uint64_t len, uint64_t thread, uint64_t stack, uint64_t stack_size) {
    (void)stack; (void)stack_size;
    if (thread) return;                                          /* thread and fiber objects: their state is the thread's, measured apart */
    if (g_sm_nnow >= SM_MAXOBJ) { g_sm_obj_capped++; return; }
    SmObj *o = &g_sm_now[g_sm_nnow++]; o->key = key; o->sig = sig; o->cnt = cnt; o->len = len;
}
static int sm_obj_cmp(const void *a, const void *b) { uint64_t x = ((const SmObj *)a)->key, y = ((const SmObj *)b)->key; return x < y ? -1 : x > y; }
static void sm_objects_now(void) { g_sm_nnow = 0; os_r1_objs(sm_obj_cb); qsort(g_sm_now, (size_t)g_sm_nnow, sizeof g_sm_now[0], sm_obj_cmp); }

/* Called by r1_ring_capture with every guest thread held, right after the capture's metadata. */
void sm_capture(long long frame) {
    if (!sm_enabled()) return;
    sm_modules();
    SmEntry *e = &g_sm[g_sm_seq % SM_SLOTS]; e->frame = frame; e->seq = ++g_sm_seq;
    e->nth = 0;
    for (int i = 0; i < g_guest_threads.count && e->nth < SM_MAXTH; i++) {
        const Th *t = &g_guest_threads.table[i];
        SmThread *s = &e->th[e->nth++]; s->key = t->c; sm_chain(t, &s->chain, &s->depth);
    }
    sm_objects_now();
    memcpy(e->obj, g_sm_now, (size_t)g_sm_nnow * sizeof g_sm_now[0]); e->nobj = g_sm_nnow;
}

static SmStat *sm_stat(const char *name) {
    for (int i = 0; i < g_sm_nstat; i++) if (!strcmp(g_sm_stat[i].name, name)) return &g_sm_stat[i];
    if (g_sm_nstat >= SM_NAMES) return &g_sm_stat[SM_NAMES - 1];
    SmStat *s = &g_sm_stat[g_sm_nstat++]; memset(s, 0, sizeof *s); snprintf(s->name, sizeof s->name, "%s", name ? name : "?"); return s;
}
static void sm_top_note(const SmObj *was, const SmObj *now) {
    for (int i = 0; i < g_sm_ntop; i++) if (g_sm_top[i].key == was->key) { SmTop *t = &g_sm_top[i]; t->times++; t->sig1 = now->sig; t->cnt1 = now->cnt; t->len1 = now->len; return; }
    if (g_sm_ntop < SM_TOPOBJ) { SmTop *t = &g_sm_top[g_sm_ntop++]; t->key = was->key; t->times = 1; t->sig0 = was->sig; t->cnt0 = was->cnt; t->len0 = was->len; t->sig1 = now->sig; t->cnt1 = now->cnt; t->len1 = now->len; }
}
static int sm_top_cmp(const void *a, const void *b) { long x = ((const SmTop *)a)->times, y = ((const SmTop *)b)->times; return x < y ? 1 : x > y ? -1 : 0; }

/* Called by r1_ring_load after the threads are held at the waits of capture `frame` and before anything is restored: the table
 * g_guest_threads is that capture's, `regs` of each entry is its guest context then, `same` tells whether the thread is back at the same wait. */
void sm_load(long long frame) {
    if (!sm_enabled()) return;
    sm_modules();
    const SmEntry *e = NULL;
    for (int i = 0; i < SM_SLOTS; i++) if (g_sm[i].frame == frame && g_sm[i].seq && (!e || g_sm[i].seq > e->seq)) e = &g_sm[i];
    g_sm_loads++;
    if (!e) { g_sm_loads_no_entry++; return; }
    for (int i = 0; i < g_guest_threads.count; i++) {
        const Th *t = &g_guest_threads.table[i];
        SmStat *s = sm_stat(t->name); s->loads++;
        if (t->same) s->same_wait++; else s->other_wait++;
        const SmThread *was = NULL;
        for (int k = 0; k < e->nth; k++) if (e->th[k].key == t->c) { was = &e->th[k]; break; }
        uint64_t chain; int depth; sm_chain(t, &chain, &depth);
        if (!was || was->depth < 0 || depth < 0) s->chain_unknown++;
        else if (was->chain == chain && was->depth == depth) s->chain_same++;
        else s->chain_diff++;
        const Ctx *now = t->c, *saved = &t->regs;
        int callee = memcmp(&now->x[19], &saved->x[19], 13 * sizeof now->x[0]) || memcmp(&now->v[8], &saved->v[8], 8 * sizeof now->v[0]);
        int other = memcmp(&now->x[0], &saved->x[0], 19 * sizeof now->x[0]) || memcmp(&now->v[0], &saved->v[0], 8 * sizeof now->v[0]) || memcmp(&now->v[16], &saved->v[16], 16 * sizeof now->v[0])
                    || now->N != saved->N || now->Z != saved->Z || now->C != saved->C || now->V != saved->V || now->tpidr != saved->tpidr;
        if (callee) s->callee_diff++;
        if (other) s->other_diff++;
    }
    sm_objects_now();
    int changed = 0, added = 0;
    for (int i = 0; i < g_sm_nnow; i++) {
        const SmObj *n = &g_sm_now[i]; const SmObj *w = (const SmObj *)bsearch(n, e->obj, (size_t)e->nobj, sizeof e->obj[0], sm_obj_cmp);
        if (!w) { added++; continue; }
        if (w->sig != n->sig || w->cnt != n->cnt || w->len != n->len) { changed++; sm_top_note(w, n); }
    }
    if (changed) g_sm_obj_loads_changed++;
    g_sm_obj_changed_total += changed; g_sm_obj_new_total += added; if (changed > g_sm_obj_changed_max) g_sm_obj_changed_max = changed;
    if (g_sm_loads % 100 == 0) sm_report(0);
}

static int sm_stat_cmp(const void *a, const void *b) {
    const SmStat *x = a, *y = b; long p = x->chain_diff + x->other_wait, q = y->chain_diff + y->other_wait; return p < q ? 1 : p > q ? -1 : 0;
}
/* The tables: one line per thread name, then the objects. final = 1 at the end of the run. */
void sm_report(int final) {
    if (!g_sm_on || !g_sm_loads) return;
    SmStat sorted[SM_NAMES]; memcpy(sorted, g_sm_stat, sizeof sorted); qsort(sorted, (size_t)g_sm_nstat, sizeof sorted[0], sm_stat_cmp);
    LOG("r1: STATE MEASURE %s: %ld loads (%ld without a capture entry for that frame). Per thread name, over the loads:\n", final ? "FINAL" : "so far", g_sm_loads, g_sm_loads_no_entry);
    for (int i = 0; i < g_sm_nstat && i < 24; i++) {
        const SmStat *s = &sorted[i];
        LOG("r1:   %-26s loads %ld | back at the same wait %ld, other wait or not parked %ld | call chain same %ld, DIFFERENT %ld, unknown %ld | callee regs differ %ld, other regs differ %ld\n",
            s->name, s->loads, s->same_wait, s->other_wait, s->chain_same, s->chain_diff, s->chain_unknown, s->callee_diff, s->other_diff);
    }
    SmTop top[SM_TOPOBJ]; memcpy(top, g_sm_top, sizeof top); qsort(top, (size_t)g_sm_ntop, sizeof top[0], sm_top_cmp);
    LOG("r1:   nn::os objects: loads with a changed object %ld of %ld; changed per load: mean %.1f, max %ld; objects made since the capture per load: mean %.1f%s\n",
        g_sm_obj_loads_changed, g_sm_loads - g_sm_loads_no_entry, g_sm_loads > g_sm_loads_no_entry ? (double)g_sm_obj_changed_total / (g_sm_loads - g_sm_loads_no_entry) : 0.0, g_sm_obj_changed_max,
        g_sm_loads > g_sm_loads_no_entry ? (double)g_sm_obj_new_total / (g_sm_loads - g_sm_loads_no_entry) : 0.0, g_sm_obj_capped ? " (object list was capped: some objects not compared)" : "");
    for (int i = 0; i < g_sm_ntop && i < 8; i++)
        LOG("r1:     object %llx changed in %ld loads (last: signalled %d -> %d, count %d -> %d, length %llu -> %llu)\n", (unsigned long long)top[i].key, top[i].times, top[i].sig0, top[i].sig1, top[i].cnt0, top[i].cnt1,
            (unsigned long long)top[i].len0, (unsigned long long)top[i].len1);
}
