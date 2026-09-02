/**
 * pending_subscriptions.h - subscriptions waiting for a ComponentTypeIndex
 *
 * A mod's file-scope Ext.Entity.OnCreate("CombatantJoinEvent", fn) runs at PAK
 * load, which is roughly four seconds before the ECS assigns
 * esv::combat::JoinEventOneFrameComponent its type index. The name is valid;
 * only the index is missing. This table parks such a subscription, keyed by
 * engine class name, and binds it for real the moment the index is learned
 * (component_registry.c's "Updated component" transition).
 *
 * Deliberately Lua-free: the callback reference is carried as an opaque int and
 * the lua_State as void*, so the table can be exercised in tier0 without a Lua
 * state or a live EntityWorld.
 */

#ifndef BG3SE_PENDING_SUBSCRIPTIONS_H
#define BG3SE_PENDING_SUBSCRIPTIONS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bound at 256 to match MAX_SUBSCRIPTIONS in entity_events.c: a pending entry
 * is a subscription that has not become a real one yet, so the two together can
 * never usefully exceed what the hook pool can hold. Bootstrap registers a lot
 * of subscriptions, so this is sized for the whole bootstrap wave rather than a
 * handful.
 */
#define PENDING_SUBSCRIPTIONS_MAX 256

/** Handle value meaning "not stored" (overflow, or bad arguments). */
#define PENDING_SUB_INVALID ((uint32_t)0)

/**
 * Binder invoked when a parked subscription's component becomes live.
 * Returns the real subscription id, or 0 on failure. On failure the binder owns
 * releasing lua_ref — the table only drops its slot.
 */
typedef uint64_t (*PendingSubscriptionBindFn)(uint16_t type_index,
                                              uint64_t entity,
                                              uint32_t events,
                                              uint32_t flags,
                                              int lua_ref,
                                              void *lua_state);

/** Release hook used to drop callback refs the table still owns. */
typedef void (*PendingSubscriptionReleaseFn)(int lua_ref, void *lua_state);

void pending_subscriptions_set_binder(PendingSubscriptionBindFn binder);

/**
 * Park a subscription against an engine class name.
 * @return handle (non-zero) on success, PENDING_SUB_INVALID when the table is
 *         full. The caller keeps ownership of lua_ref on failure.
 */
uint32_t pending_subscriptions_add(const char *engine_name,
                                   uint64_t entity,
                                   uint32_t events,
                                   uint32_t flags,
                                   int lua_ref,
                                   void *lua_state);

/**
 * Bind every entry parked against engine_name.
 * @return number of entries that bound successfully.
 */
int pending_subscriptions_flush(const char *engine_name, uint16_t type_index);

typedef enum {
    /* Unknown handle, or one already consumed. */
    PENDING_UNSUB_NOT_FOUND = 0,
    /* Was still parked: *out_lua_ref must be released by the caller. */
    PENDING_UNSUB_UNPARKED,
    /* Already bound: *out_real_id must be unsubscribed by the caller. */
    PENDING_UNSUB_FORWARD
} PendingUnsubResult;

/**
 * Drop a handle. The slot is freed either way; the result tells the caller
 * which resource it is now responsible for. Unparking before the component
 * resolves is the case that must not leave the callback ref behind — the
 * subscription would otherwise bind later and fire a callback the mod has
 * already disowned.
 */
PendingUnsubResult pending_subscriptions_remove(uint32_t handle,
                                                int *out_lua_ref,
                                                uint64_t *out_real_id);

/**
 * Drop every entry. `release` is called once per still-parked callback ref;
 * bound entries hold no ref of their own (ownership moved to the real
 * subscription) and are only forgotten. Pass NULL to skip releasing.
 */
void pending_subscriptions_reset(PendingSubscriptionReleaseFn release);

/** Entries still waiting for an index. */
int pending_subscriptions_pending_count(void);

/** Entries occupying a slot, parked or bound. */
int pending_subscriptions_live_count(void);

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_PENDING_SUBSCRIPTIONS_H */
