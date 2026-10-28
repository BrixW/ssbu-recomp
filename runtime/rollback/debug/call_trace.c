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

/* Called from r1_init: installs the hook when SSBU_TRACE_CALL is set. */
void call_trace_init(void) {
    const char *e = getenv("SSBU_TRACE_CALL"); if (!e || !e[0] || !g_log) return;
    unsigned long long fn = 0, site = 0;
    if (sscanf(e, "%llx,%llx", &fn, &site) != 2) { LOG("r1: CALL TRACE: SSBU_TRACE_CALL=<function>,<call site> (hex)\n"); return; }
    g_ct_site = (uint64_t)site;
    int ok = hook_install(GA(fn), ct_hook, NULL, &g_ct_orig);
    LOG("r1: CALL TRACE: hook on f_%llx for calls from %llx: %s\n", fn, site, ok ? "installed" : "NOT installed");
}
