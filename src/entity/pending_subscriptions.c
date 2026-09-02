/**
 * pending_subscriptions.c - park subscriptions until their component resolves.
 */

#include "pending_subscriptions.h"
#include "component_registry.h"
#include "../core/logging.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char engine_name[COMPONENT_MAX_NAME_LEN];
    uint64_t entity;
    uint32_t events;
    uint32_t flags;
    int lua_ref;
    void *lua_state;
    uint64_t real_id;   /* valid once bound */
    uint16_t salt;
    bool active;
    bool bound;
} PendingSubscription;

static PendingSubscription g_pending[PENDING_SUBSCRIPTIONS_MAX];
static uint16_t g_next_salt = 1;
static int g_live_count = 0;
static int g_parked_count = 0;
static PendingSubscriptionBindFn g_binder = NULL;
static bool g_overflow_warned = false;

/* Handle layout mirrors entity_events.c's pool_pack: salt in the high half,
 * slot in the low half. The salt is what keeps a recycled slot from answering
 * to a handle the mod stashed before unsubscribing. */
static uint32_t pack_handle(int slot) {
    return ((uint32_t)g_pending[slot].salt << 16) | (uint32_t)(slot & 0xFFFF);
}

static PendingSubscription *resolve_handle(uint32_t handle) {
    if (handle == PENDING_SUB_INVALID) return NULL;
    uint16_t slot = (uint16_t)(handle & 0xFFFF);
    uint16_t salt = (uint16_t)((handle >> 16) & 0xFFFF);
    if (slot >= PENDING_SUBSCRIPTIONS_MAX) return NULL;
    PendingSubscription *e = &g_pending[slot];
    if (!e->active || e->salt != salt) return NULL;
    return e;
}

static void free_slot(PendingSubscription *e) {
    if (!e->bound) g_parked_count--;
    e->active = false;
    e->bound = false;
    e->lua_ref = 0;
    e->lua_state = NULL;
    e->real_id = 0;
    e->engine_name[0] = '\0';
    g_live_count--;
}

void pending_subscriptions_set_binder(PendingSubscriptionBindFn binder) {
    g_binder = binder;
}

uint32_t pending_subscriptions_add(const char *engine_name,
                                   uint64_t entity,
                                   uint32_t events,
                                   uint32_t flags,
                                   int lua_ref,
                                   void *lua_state) {
    if (!engine_name || !*engine_name) return PENDING_SUB_INVALID;

    for (int i = 0; i < PENDING_SUBSCRIPTIONS_MAX; i++) {
        if (g_pending[i].active) continue;

        PendingSubscription *e = &g_pending[i];
        memset(e, 0, sizeof(*e));
        snprintf(e->engine_name, sizeof(e->engine_name), "%s", engine_name);
        e->entity = entity;
        e->events = events;
        e->flags = flags;
        e->lua_ref = lua_ref;
        e->lua_state = lua_state;
        e->salt = g_next_salt++;
        if (g_next_salt == 0) g_next_salt = 1;
        e->active = true;
        e->bound = false;
        g_live_count++;
        g_parked_count++;
        return pack_handle(i);
    }

    /* Report the overflow rather than dropping quietly: a silently discarded
     * subscription looks to the mod exactly like a component that never fires,
     * which is the failure this whole mechanism exists to stop misdiagnosing.
     * Warn once so a bootstrap wave does not flood the log. */
    if (!g_overflow_warned) {
        g_overflow_warned = true;
        log_message("[ERROR] [PendingSubs] Table full (%d entries) — cannot park "
                    "subscription for %s. Further overflows are not logged.",
                    PENDING_SUBSCRIPTIONS_MAX, engine_name);
    } else {
        log_message("[ERROR] [PendingSubs] Table full — dropped %s", engine_name);
    }
    return PENDING_SUB_INVALID;
}

int pending_subscriptions_flush(const char *engine_name, uint16_t type_index) {
    if (!engine_name || !*engine_name) return 0;
    if (type_index == COMPONENT_INDEX_UNDEFINED) return 0;
    if (g_parked_count == 0) return 0;

    int bound = 0;
    for (int i = 0; i < PENDING_SUBSCRIPTIONS_MAX; i++) {
        PendingSubscription *e = &g_pending[i];
        if (!e->active || e->bound) continue;
        if (strcmp(e->engine_name, engine_name) != 0) continue;

        uint64_t real_id = 0;
        if (g_binder) {
            real_id = g_binder(type_index, e->entity, e->events, e->flags,
                               e->lua_ref, e->lua_state);
        }

        if (real_id == 0) {
            /* Binder owns the ref on failure; drop the slot so a hopeless
             * entry is not retried on every later index update. */
            log_message("[ERROR] [PendingSubs] Failed to bind %s (index=%u) — "
                        "subscription dropped", engine_name, (unsigned)type_index);
            free_slot(e);
            continue;
        }

        /* The slot outlives binding on purpose: the mod holds the pending id,
         * so unsubscribe has to keep resolving through it. Ownership of
         * lua_ref moves to the real subscription here. */
        e->bound = true;
        e->real_id = real_id;
        e->lua_ref = 0;
        g_parked_count--;
        bound++;
    }

    if (bound > 0) {
        log_message("[INFO] [PendingSubs] Bound %d deferred subscription(s) to %s "
                    "(index=%u)", bound, engine_name, (unsigned)type_index);
    }
    return bound;
}

PendingUnsubResult pending_subscriptions_remove(uint32_t handle,
                                                int *out_lua_ref,
                                                uint64_t *out_real_id) {
    PendingSubscription *e = resolve_handle(handle);
    if (!e) return PENDING_UNSUB_NOT_FOUND;

    if (e->bound) {
        if (out_real_id) *out_real_id = e->real_id;
        free_slot(e);
        return PENDING_UNSUB_FORWARD;
    }

    if (out_lua_ref) *out_lua_ref = e->lua_ref;
    free_slot(e);
    return PENDING_UNSUB_UNPARKED;
}

void pending_subscriptions_reset(PendingSubscriptionReleaseFn release) {
    for (int i = 0; i < PENDING_SUBSCRIPTIONS_MAX; i++) {
        PendingSubscription *e = &g_pending[i];
        if (!e->active) continue;
        if (!e->bound && release) {
            release(e->lua_ref, e->lua_state);
        }
        free_slot(e);
    }
    g_live_count = 0;
    g_parked_count = 0;
    g_overflow_warned = false;
}

int pending_subscriptions_pending_count(void) {
    return g_parked_count;
}

int pending_subscriptions_live_count(void) {
    return g_live_count;
}
