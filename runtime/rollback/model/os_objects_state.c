/* THE nn::os OBJECTS SECTION OF THE SAVE STATE (SSBU_STATE_OBJS=1; unset or 0 = off). SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The game's events and semaphores are host objects (os_threads.c: Obj, a signalled flag and a counter each), not guest memory, so a load of the
 * guest memory leaves them as they are NOW. The state measurement (debug/state_measure.c) found that at about half of the loads one event differs
 * from what it was at the capture (signalled 1 -> 0). This section records the signalled flag and the count of every event / semaphore at each
 * capture and puts them back at the load of that capture; os_r1_obj_set wakes the object's waiters so they test it again.
 *
 * Not covered, on purpose: message queues (their buffer is a host heap block), reader/writer locks and mutexes (the owner is a host thread id inside
 * the object's lock), thread and fiber objects (the fibers have their own section). An object made after the capture is left alone.
 * Cost: one walk of the object table per capture (about 0.15 ms) and per load. */
#include "game_state_internal.h"

#define OS_SLOTS 32                  /* captures kept: more than the ring's depth */
#define OS_MAXOBJ 8192

typedef struct { uint64_t key; int sig, cnt; } OsObjRec;
typedef struct { long long frame, seq; int n; OsObjRec *rec; } OsObjEntry;
static OsObjEntry g_os_ent[OS_SLOTS]; static long long g_os_seq;
static int g_os_on = -1;
static OsObjRec g_os_now[OS_MAXOBJ]; static int g_os_nnow;
static long long g_os_loads, g_os_changed, g_os_equal, g_os_skipped, g_os_noentry;

static int os_objs_enabled(void) { if (g_os_on < 0) { const char *e = getenv("SSBU_STATE_OBJS"); g_os_on = e && e[0] && e[0] != '0'; if (g_os_on) LOG("r1: STATE OBJS on (SSBU_STATE_OBJS): events and semaphores are recorded at each capture and put back at each load\n"); } return g_os_on; }
static void os_objs_cb(uint64_t key, int sig, int cnt, uint64_t len, uint64_t thread, uint64_t stack, uint64_t stack_size) {
    (void)stack; (void)stack_size;
    if (thread || len) return;                                   /* threads, fibers and message queues are not part of this section */
    if (g_os_nnow >= OS_MAXOBJ) return;
    g_os_now[g_os_nnow].key = key; g_os_now[g_os_nnow].sig = sig; g_os_now[g_os_nnow].cnt = cnt; g_os_nnow++;
}
static int os_rec_cmp(const void *a, const void *b) { uint64_t x = ((const OsObjRec *)a)->key, y = ((const OsObjRec *)b)->key; return x < y ? -1 : x > y; }

/* Called by r1_ring_capture with every guest thread held: records every event / semaphore. */
void osobj_capture(long long frame) {
    if (!os_objs_enabled()) return;
    g_os_nnow = 0; os_r1_objs(os_objs_cb);
    qsort(g_os_now, (size_t)g_os_nnow, sizeof g_os_now[0], os_rec_cmp);
    OsObjEntry *e = &g_os_ent[g_os_seq % OS_SLOTS];
    if (!e->rec) e->rec = malloc(OS_MAXOBJ * sizeof *e->rec);
    if (!e->rec) return;
    memcpy(e->rec, g_os_now, (size_t)g_os_nnow * sizeof g_os_now[0]);
    e->n = g_os_nnow; e->frame = frame; e->seq = ++g_os_seq;
}

/* Called by r1_ring_load with every guest thread held, after the memory is restored: puts back what capture `frame` recorded. */
void osobj_load(long long frame) {
    if (!os_objs_enabled()) return;
    const OsObjEntry *e = NULL;
    for (int i = 0; i < OS_SLOTS; i++) if (g_os_ent[i].rec && g_os_ent[i].frame == frame && g_os_ent[i].seq && (!e || g_os_ent[i].seq > e->seq)) e = &g_os_ent[i];
    g_os_loads++;
    if (!e) { g_os_noentry++; return; }
    int changed = 0;
    for (int i = 0; i < e->n; i++) {
        int r = os_r1_obj_set(e->rec[i].key, e->rec[i].sig, e->rec[i].cnt);
        if (r == 1) { changed++; g_os_changed++; } else if (r == 2) g_os_equal++; else g_os_skipped++;
    }
    if (g_os_loads % 100 == 0) LOG("r1: STATE OBJS: %lld loads: objects put back %lld (%.2f per load), already equal %lld, skipped %lld (lock taken or missing), loads without a capture entry %lld\n",
        g_os_loads, g_os_changed, (double)g_os_changed / g_os_loads, g_os_equal, g_os_skipped, g_os_noentry);
    (void)changed;
}
