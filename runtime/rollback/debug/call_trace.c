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

/* Called from r1_init: installs the hook when SSBU_TRACE_CALL is set. */
void call_trace_init(void) {
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
