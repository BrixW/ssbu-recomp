/* CALL TRACE (SSBU_TRACE_CALL=<function>,<call site>; both hex, 13.0.4 addresses; unset = off). Debugging aid: which lookup fails, and with what key. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Hooks one guest function and logs the calls that come from one call site: the arguments x0..x3, the value returned in x0, the pass the
 * simulation is on and whether it is a re-run pass. Made for a crash that is deterministic with SSBU_RB_SIM_PERTURB: the function
 * f_7100081a30 walks a binary search tree (std::map-like: root at [x0], node key at +48, children at +32 / +40, the key built from x1 x2 x3) and the
 * caller f_710007cdf0 reads [result + 0x4e] without testing it: the crash is a lookup that found nothing. The trace says which key was not found,
 * and the same key's earlier lookups say whether the entry was there before a rollback.
 * Output: the first 40 calls from the site, then every call that returns 0, up to 400 lines in all. */
#include "../model/game_state_internal.h"

static ImportFn g_ct_orig; static uint64_t g_ct_site; static long g_ct_calls, g_ct_misses, g_ct_lines;

static void ct_hook(Ctx *c, void *u) {
    (void)u;
    uint64_t ret = c->x[30], m = c->x[0], a1 = c->x[1], a2 = c->x[2], a3 = c->x[3];
    g_ct_orig(c);
    if (ret != g_ct_site + 4 && ret != g_ct_site) return;
    g_ct_calls++; uint64_t res = c->x[0];
    if (!res) g_ct_misses++;
    if ((g_ct_calls <= 40 || !res) && g_ct_lines++ < 400)
        LOG("r1: CALL TRACE #%ld pass %lld%s: map/root %llx key x1 %llx x2 %llx x3 %llx -> %s %llx (misses so far %ld)\n", g_ct_calls, g_rgs_next, g_ra_phase == 2 ? " RE-RUN" : "",
            (unsigned long long)m, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, res ? "found" : "NOT FOUND", (unsigned long long)res, g_ct_misses);
}

/* BAD RETURN ADDRESS TRACE (SSBU_TRACE_BADLR=<function>, hex): logs every entry of the function whose x30 is not a code address of the guest (outside 0x7100000000..0x7300000000).
 * Made for a std::terminate where the unwinder found a data pointer in a function's saved x30: the function had been entered without a return address. c->fn still holds
 * the CALLER's function at that moment, so the line names who called it that way, with the pass and whether it is a re-run. First 50 lines. */
static ImportFn g_blr_orig; static long g_blr_lines, g_blr_calls;
static void blr_hook(Ctx *c, void *u) {
    (void)u; g_blr_calls++;
    uint64_t lr = c->x[30];
    if ((lr < 0x7100000000ull || lr >= 0x7300000000ull) && g_blr_lines++ < 50)
        LOG("r1: BAD LR TRACE #%ld (call %ld) pass %lld%s: entered with x30 = %llx, caller f_%llx, x0 %llx x1 %llx x2 %llx\n", g_blr_lines, g_blr_calls, g_rgs_next, g_ra_phase == 2 ? " RE-RUN" : "",
            (unsigned long long)lr, (unsigned long long)c->fn, (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    g_blr_orig(c);
}

/* NULL ARGUMENT TRACE (SSBU_TRACE_NULL=<function>, hex): logs every entry of the function whose x0 is below 0x10000 (a null or tiny "this"): the pass, whether it is a re-run, the return address x30
 * (the exact call site in the caller) and the caller's function (c->fn at that moment). Made for f_720052dc70 (L2CAgent shutdown check), which reads this+0xc0 and was entered with this = 0
 * in most of the crashes of the forced-rollback runs. First 40 lines. */
static ImportFn g_nul_orig; static long g_nul_lines, g_nul_calls; static unsigned long long g_nul_fn; static int g_nul_done;
static void nul_hook(Ctx *c, void *u) {
    (void)u; g_nul_calls++;
    if (c->x[0] < 0x10000 && g_nul_lines++ < 40)
        LOG("r1: NULL ARG TRACE #%ld (call %ld) pass %lld%s: entered with x0 = %llx, return address %llx, caller f_%llx, x1 %llx x2 %llx\n", g_nul_lines, g_nul_calls, g_rgs_next, g_ra_phase == 2 ? " RE-RUN" : "",
            (unsigned long long)c->x[0], (unsigned long long)c->x[30], (unsigned long long)c->fn, (unsigned long long)c->x[1], (unsigned long long)c->x[2]);
    g_nul_orig(c);
}
static unsigned long long g_blr_fn; static int g_blr_done;
/* Installs the SSBU_TRACE_BADLR hook. The functions of the lua2cpp_* modules do not exist until the game loads the module, so this is tried at r1_init and again from
 * every pass (rgs_step) until it works; an install that fails costs one lookup, retried every 30 passes. */
void call_trace_poll(void) {
    static int tick2; if (g_nul_fn && !g_nul_done && !(tick2++ % 30) && hook_install(GA(g_nul_fn), nul_hook, NULL, &g_nul_orig)) { g_nul_done = 1; LOG("r1: NULL ARG TRACE: hook on f_%llx installed\n", g_nul_fn); }
    static int tick; if (!g_blr_fn || g_blr_done || (tick++ % 30)) return;
    if (hook_install(GA(g_blr_fn), blr_hook, NULL, &g_blr_orig)) { g_blr_done = 1; LOG("r1: BAD LR TRACE: hook on f_%llx installed\n", g_blr_fn); }
}


/* ---------------------------------------------------------------- JOB TRACE (SSBU_TRACE_JOBS=1). Measurement for "run every job on the main thread during a rollback match".
 * The game's job system runs each job on a Windows fiber: f_710353cf40 is the entry of every job (the job is [x0+64], its class's vtable [job], its Run the vtable's entry 6). Hooked there, during
 * fights only, every job: its class (vtable), its Run, the thread it ran on, its wall time, and whether its fiber SWITCHED AWAY before it finished (it waited on something in the middle: such a job
 * cannot simply be run start to end on one thread). Every 10 s of fight: jobs per frame, job time per frame (all, and of the jobs that never switched away = their CPU time), the time per thread, and
 * the job classes by time. The question it answers: would every job of a frame fit in one thread's 16.7 ms, and which jobs wait in the middle. */
uint64_t os_fiber_cur_key(void); long os_fiber_switch_outs(uint64_t key);            /* os_threads.c */
const char *guest_thread_name_of(DWORD tid);                                           /* os_threads.c */
#define JT_TYPES 256
#define JT_THREADS 16
typedef struct { uint64_t vt, run; long long n, nsusp; double ms, ms_nosusp, max; unsigned thr_mask; } JtType;
static JtType g_jt[JT_TYPES]; static int g_njt;
static struct { DWORD tid; char name[40]; double ms; long long n; } g_jth[JT_THREADS]; static int g_njth;
static SRWLOCK g_jt_lock = SRWLOCK_INIT;
static ImportFn g_jt_orig; static int g_jt_on = -1;
static double g_jt_t0, g_jt_freq; static long long g_jt_n, g_jt_nsusp; static double g_jt_ms, g_jt_ms_nosusp;
static double jt_now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (double)q.QuadPart * 1000.0 / g_jt_freq; }
static int jt_thread(void) {                                                           /* lock held */
    DWORD me = GetCurrentThreadId();
    for (int i = 0; i < g_njth; i++) if (g_jth[i].tid == me) return i;
    if (g_njth >= JT_THREADS) return JT_THREADS - 1;
    int i = g_njth++; g_jth[i].tid = me; g_jth[i].ms = 0; g_jth[i].n = 0;
    const char *nm = guest_thread_name_of(me); snprintf(g_jth[i].name, sizeof g_jth[i].name, "%s", nm && nm[0] != '?' ? nm : "main or other");
    return i;
}
static void jt_report(double t) {                                                      /* lock held */
    double secs = (t - g_jt_t0) / 1000.0, frames = secs * 60.0; if (frames < 1) frames = 1;
    LOG("r1: JOBS: %.1f s of fight: %lld jobs (%.1f per frame); job time %.2f ms per frame, of the jobs that never switched away %.2f ms per frame; %lld jobs switched away in the middle (%.1f%%)\n",
        secs, g_jt_n, g_jt_n / frames, g_jt_ms / frames, g_jt_ms_nosusp / frames, g_jt_nsusp, g_jt_n ? 100.0 * g_jt_nsusp / g_jt_n : 0.0);
    for (int i = 0; i < g_njth; i++) LOG("r1: JOBS:   thread %s: %lld jobs, %.2f ms per frame\n", g_jth[i].name, g_jth[i].n, g_jth[i].ms / frames);
    for (int shown = 0; shown < 15; shown++) {
        int b = -1; for (int i = 0; i < g_njt; i++) if (g_jt[i].n && (b < 0 || g_jt[i].ms > g_jt[b].ms)) b = i;
        if (b < 0) break;
        JtType *k = &g_jt[b]; char th[160] = ""; size_t o = 0;
        for (int i = 0; i < g_njth && o < 140; i++) if (k->thr_mask & (1u << i)) o += (size_t)snprintf(th + o, sizeof th - o, "%s%s", o ? ", " : "", g_jth[i].name);
        LOG("r1: JOBS:   class vtable %llx run f_%llx: %lld jobs (%.1f per frame), %.3f ms each on average, max %.2f, %.2f ms per frame; switched away in the middle %lld (%.0f%%); threads: %s\n",
            (unsigned long long)k->vt, (unsigned long long)k->run, k->n, k->n / frames, k->ms / k->n, k->max, k->ms / frames, k->nsusp, 100.0 * k->nsusp / k->n, th);
        k->n = 0;                                                                      /* (shown once) */
    }
    g_njt = 0; g_njth = 0; g_jt_n = g_jt_nsusp = 0; g_jt_ms = g_jt_ms_nosusp = 0; g_jt_t0 = t;
}
static void jt_hook(Ctx *c, void *u) {
    (void)u;
    if (!ra_fight_on()) { g_jt_orig(c); return; }
    uint64_t job = rd64(c->x[0] + 64), vt = job ? rd64(job) : 0, run = vt ? rd64(vt + 48) : 0;
    uint64_t key = os_fiber_cur_key(); long s0 = os_fiber_switch_outs(key);
    double t0 = jt_now();
    g_jt_orig(c);
    double t1 = jt_now(), d = t1 - t0;
    int susp = os_fiber_switch_outs(key) != s0;
    AcquireSRWLockExclusive(&g_jt_lock);
    if (g_jt_t0 <= 0) g_jt_t0 = t0;
    int ti = jt_thread(); g_jth[ti].ms += d; g_jth[ti].n++;
    int i; for (i = 0; i < g_njt; i++) if (g_jt[i].vt == vt) break;
    if (i == g_njt && g_njt < JT_TYPES) { g_jt[i].vt = vt; g_jt[i].run = run; g_jt[i].n = g_jt[i].nsusp = 0; g_jt[i].ms = g_jt[i].ms_nosusp = g_jt[i].max = 0; g_jt[i].thr_mask = 0; g_njt++; }
    if (i < g_njt) { JtType *k = &g_jt[i]; k->n++; k->ms += d; if (d > k->max) k->max = d; k->thr_mask |= 1u << ti; if (susp) k->nsusp++; else k->ms_nosusp += d; }
    g_jt_n++; g_jt_ms += d; if (susp) g_jt_nsusp++; else g_jt_ms_nosusp += d;
    if (t1 - g_jt_t0 >= 10000.0) jt_report(t1);
    ReleaseSRWLockExclusive(&g_jt_lock);
}
static void job_trace_init(void) {
    if (g_jt_on >= 0) return;
    const char *e = getenv("SSBU_TRACE_JOBS"); g_jt_on = e && atoi(e) > 0;
    if (!g_jt_on || !g_log) return;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_jt_freq = (double)f.QuadPart;
    int ok = hook_install(GA(0x710353cf40ull), jt_hook, NULL, &g_jt_orig);
    LOG("r1: JOBS: trace of every job (f_710353cf40) during fights: hook %s\n", ok ? "installed" : "NOT installed");
}

/* Called from r1_init: installs the hook when SSBU_TRACE_CALL is set. */
void call_trace_init(void) {
    job_trace_init();                                   /* (SSBU_TRACE_JOBS) */
    { const char *n = getenv("SSBU_TRACE_NULL"); if (n && n[0] && g_log) { g_nul_fn = strtoull(n, NULL, 16); call_trace_poll(); } }
    const char *b = getenv("SSBU_TRACE_BADLR");
    if (b && b[0] && g_log) { g_blr_fn = strtoull(b, NULL, 16); call_trace_poll(); }
    const char *e = getenv("SSBU_TRACE_CALL"); if (!e || !e[0] || !g_log) return;
    unsigned long long fn = 0, site = 0;
    if (sscanf(e, "%llx,%llx", &fn, &site) != 2) { LOG("r1: CALL TRACE: SSBU_TRACE_CALL=<function>,<call site> (hex)\n"); return; }
    g_ct_site = (uint64_t)site;
    int ok = hook_install(GA(fn), ct_hook, NULL, &g_ct_orig);
    LOG("r1: CALL TRACE: hook on f_%llx for calls from %llx: %s\n", fn, site, ok ? "installed" : "NOT installed");
}
