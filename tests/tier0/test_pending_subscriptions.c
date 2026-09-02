/*
 * Tier 0 tests for the pending subscription table.
 *
 * Stands in for the live sequence that cannot be replayed offline: a mod parks
 * a subscription at PAK load, the ECS assigns the component its index seconds
 * later, and the subscription must bind exactly then — or, if the mod
 * unsubscribed in between, must never bind and must not strand its Lua callback
 * reference.
 *
 * The fake binder records what it was asked to bind and hands back synthetic
 * subscription ids, so "did it fire?" and "was the ref released?" are both
 * directly observable without a Lua state.
 */

#include "test_harness.h"
#include "pending_subscriptions.h"
#include "component_registry.h"

/* ── Fake binder / release recorder ──────────────────────────────── */

typedef struct {
    uint16_t type_index;
    uint64_t entity;
    uint32_t events;
    uint32_t flags;
    int lua_ref;
} BindCall;

#define MAX_RECORDED 16

static BindCall g_binds[MAX_RECORDED];
static int g_bind_count;
static int g_released[MAX_RECORDED];
static int g_release_count;
static bool g_bind_should_fail;
static uint64_t g_next_fake_id;

static uint64_t fake_bind(uint16_t type_index, uint64_t entity, uint32_t events,
                          uint32_t flags, int lua_ref, void *lua_state) {
    (void)lua_state;
    if (g_bind_count < MAX_RECORDED) {
        g_binds[g_bind_count++] = (BindCall){ type_index, entity, events, flags, lua_ref };
    }
    if (g_bind_should_fail) {
        /* Contract: the binder owns the ref once it has failed. */
        if (g_release_count < MAX_RECORDED) g_released[g_release_count++] = lua_ref;
        return 0;
    }
    return g_next_fake_id++;
}

static void fake_release(int lua_ref, void *lua_state) {
    (void)lua_state;
    if (g_release_count < MAX_RECORDED) g_released[g_release_count++] = lua_ref;
}

static bool was_released(int lua_ref) {
    for (int i = 0; i < g_release_count; i++) {
        if (g_released[i] == lua_ref) return true;
    }
    return false;
}

static void reset_fixture(void) {
    pending_subscriptions_reset(NULL);
    pending_subscriptions_set_binder(fake_bind);
    g_bind_count = 0;
    g_release_count = 0;
    g_bind_should_fail = false;
    g_next_fake_id = 0x1000;
}

#define JOIN_EVENT "esv::combat::JoinEventOneFrameComponent"
#define CC_STATE   "eoc::character_creation::StateComponent"

/* ── Park, then bind when the index is learned ───────────────────── */

TEST(parked_subscription_binds_on_flush) {
    reset_fixture();

    uint32_t h = pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 77, NULL);
    ASSERT_NE(h, PENDING_SUB_INVALID);
    ASSERT_EQ(pending_subscriptions_pending_count(), 1);
    ASSERT_EQ(g_bind_count, 0);

    ASSERT_EQ(pending_subscriptions_flush(JOIN_EVENT, 1341), 1);
    ASSERT_EQ(g_bind_count, 1);
    ASSERT_EQ(g_binds[0].type_index, 1341);
    ASSERT_EQ(g_binds[0].lua_ref, 77);
    ASSERT_EQ(pending_subscriptions_pending_count(), 0);
}

TEST(flush_carries_entity_events_and_flags_through) {
    reset_fixture();

    pending_subscriptions_add(JOIN_EVENT, 0xABCDEF, 3, 2, 5, NULL);
    ASSERT_EQ(pending_subscriptions_flush(JOIN_EVENT, 42), 1);

    ASSERT_EQ(g_binds[0].entity, 0xABCDEFull);
    ASSERT_EQ(g_binds[0].events, 3u);
    ASSERT_EQ(g_binds[0].flags, 2u);
}

TEST(flush_of_another_component_leaves_entry_parked) {
    reset_fixture();

    pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 9, NULL);
    ASSERT_EQ(pending_subscriptions_flush(CC_STATE, 900), 0);
    ASSERT_EQ(g_bind_count, 0);
    ASSERT_EQ(pending_subscriptions_pending_count(), 1);
}

TEST(flush_binds_every_entry_for_the_component) {
    reset_fixture();

    pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 1, NULL);
    pending_subscriptions_add(JOIN_EVENT, 0, 2, 0, 2, NULL);
    pending_subscriptions_add(CC_STATE, 0, 1, 0, 3, NULL);

    ASSERT_EQ(pending_subscriptions_flush(JOIN_EVENT, 1341), 2);
    ASSERT_EQ(pending_subscriptions_pending_count(), 1);
}

/* A second index update must not re-bind an entry that already bound —
 * the callback would then fire twice per event. */
TEST(second_flush_does_not_rebind) {
    reset_fixture();

    pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 11, NULL);
    ASSERT_EQ(pending_subscriptions_flush(JOIN_EVENT, 1341), 1);
    ASSERT_EQ(pending_subscriptions_flush(JOIN_EVENT, 1341), 0);
    ASSERT_EQ(g_bind_count, 1);
}

/* COMPONENT_INDEX_UNDEFINED is not news; binding to it would put the
 * subscription on a nonexistent slot. */
TEST(flush_with_undefined_index_is_a_noop) {
    reset_fixture();

    pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 12, NULL);
    ASSERT_EQ(pending_subscriptions_flush(JOIN_EVENT, COMPONENT_INDEX_UNDEFINED), 0);
    ASSERT_EQ(g_bind_count, 0);
    ASSERT_EQ(pending_subscriptions_pending_count(), 1);
}

/* ── Unsubscribing while still parked ────────────────────────────── */

TEST(unparking_hands_back_the_callback_ref) {
    reset_fixture();

    uint32_t h = pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 55, NULL);
    int ref = -1;
    uint64_t real_id = 0;

    ASSERT_EQ(pending_subscriptions_remove(h, &ref, &real_id), PENDING_UNSUB_UNPARKED);
    ASSERT_EQ(ref, 55);
    ASSERT_EQ(pending_subscriptions_pending_count(), 0);
    ASSERT_EQ(pending_subscriptions_live_count(), 0);
}

/* The regression this whole path exists to prevent: a mod that subscribes and
 * unsubscribes before the component resolves must not have its callback bound
 * (and fired) four seconds later. */
TEST(unparked_subscription_never_binds) {
    reset_fixture();

    uint32_t h = pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 55, NULL);
    int ref = -1;
    pending_subscriptions_remove(h, &ref, NULL);

    ASSERT_EQ(pending_subscriptions_flush(JOIN_EVENT, 1341), 0);
    ASSERT_EQ(g_bind_count, 0);
}

TEST(unparking_twice_reports_not_found) {
    reset_fixture();

    uint32_t h = pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 55, NULL);
    pending_subscriptions_remove(h, NULL, NULL);
    ASSERT_EQ(pending_subscriptions_remove(h, NULL, NULL), PENDING_UNSUB_NOT_FOUND);
}

/* A recycled slot must not answer to the old handle. */
TEST(stale_handle_does_not_hit_recycled_slot) {
    reset_fixture();

    uint32_t old = pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 1, NULL);
    pending_subscriptions_remove(old, NULL, NULL);
    uint32_t fresh = pending_subscriptions_add(CC_STATE, 0, 1, 0, 2, NULL);

    ASSERT_NE(old, fresh);
    ASSERT_EQ(pending_subscriptions_remove(old, NULL, NULL), PENDING_UNSUB_NOT_FOUND);
    ASSERT_EQ(pending_subscriptions_pending_count(), 1);
}

TEST(unknown_handle_reports_not_found) {
    reset_fixture();
    ASSERT_EQ(pending_subscriptions_remove(0xDEADBEEF, NULL, NULL),
              PENDING_UNSUB_NOT_FOUND);
    ASSERT_EQ(pending_subscriptions_remove(PENDING_SUB_INVALID, NULL, NULL),
              PENDING_UNSUB_NOT_FOUND);
}

/* ── Unsubscribing after the bind ────────────────────────────────── */

/* The mod holds the id it got at bootstrap, so that id has to keep resolving
 * once the parked entry has turned into a real subscription. */
TEST(unsubscribe_after_bind_forwards_to_the_real_id) {
    reset_fixture();

    uint32_t h = pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 3, NULL);
    pending_subscriptions_flush(JOIN_EVENT, 1341);

    uint64_t real_id = 0;
    ASSERT_EQ(pending_subscriptions_remove(h, NULL, &real_id), PENDING_UNSUB_FORWARD);
    ASSERT_EQ(real_id, 0x1000ull);
    ASSERT_EQ(pending_subscriptions_live_count(), 0);
}

/* ── Bounding and overflow ───────────────────────────────────────── */

TEST(table_is_bounded_and_reports_overflow) {
    reset_fixture();

    for (int i = 0; i < PENDING_SUBSCRIPTIONS_MAX; i++) {
        ASSERT_NE(pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, i, NULL),
                  PENDING_SUB_INVALID);
    }
    ASSERT_EQ(pending_subscriptions_live_count(), PENDING_SUBSCRIPTIONS_MAX);

    /* Overflow is refused, not silently dropped onto a recycled slot; the
     * caller keeps the ref and reports nil to Lua. */
    ASSERT_EQ(pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 999, NULL),
              PENDING_SUB_INVALID);
    ASSERT_EQ(pending_subscriptions_live_count(), PENDING_SUBSCRIPTIONS_MAX);

    /* Freeing one slot makes room again. */
    pending_subscriptions_reset(NULL);
    ASSERT_NE(pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 1, NULL),
              PENDING_SUB_INVALID);
}

/* Bound entries keep their slot so the mod's id stays resolvable, and that is
 * counted honestly against the bound. */
TEST(bound_entries_still_occupy_a_slot) {
    reset_fixture();

    pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 4, NULL);
    pending_subscriptions_flush(JOIN_EVENT, 1341);

    ASSERT_EQ(pending_subscriptions_pending_count(), 0);
    ASSERT_EQ(pending_subscriptions_live_count(), 1);
}

TEST(rejects_empty_engine_name) {
    reset_fixture();
    ASSERT_EQ(pending_subscriptions_add(NULL, 0, 1, 0, 1, NULL), PENDING_SUB_INVALID);
    ASSERT_EQ(pending_subscriptions_add("", 0, 1, 0, 1, NULL), PENDING_SUB_INVALID);
}

/* ── Shutdown ────────────────────────────────────────────────────── */

/* A parked entry owns its callback ref outright — nothing else will ever free
 * it, so shutdown has to. */
TEST(reset_releases_parked_refs) {
    reset_fixture();

    pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 21, NULL);
    pending_subscriptions_add(CC_STATE, 0, 1, 0, 22, NULL);
    pending_subscriptions_reset(fake_release);

    ASSERT_EQ(g_release_count, 2);
    ASSERT_TRUE(was_released(21));
    ASSERT_TRUE(was_released(22));
    ASSERT_EQ(pending_subscriptions_live_count(), 0);
}

/* A bound entry's ref moved to the real subscription, which releases it on its
 * own teardown; releasing here too would double-unref the registry slot. */
TEST(reset_does_not_release_bound_refs) {
    reset_fixture();

    pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 31, NULL);
    pending_subscriptions_flush(JOIN_EVENT, 1341);
    pending_subscriptions_reset(fake_release);

    ASSERT_EQ(g_release_count, 0);
    ASSERT_EQ(pending_subscriptions_live_count(), 0);
}

/* A binder that fails owns the ref; the table must drop the slot rather than
 * retry it on every later index update. */
TEST(failed_bind_drops_the_entry) {
    reset_fixture();
    g_bind_should_fail = true;

    pending_subscriptions_add(JOIN_EVENT, 0, 1, 0, 41, NULL);
    ASSERT_EQ(pending_subscriptions_flush(JOIN_EVENT, 1341), 0);

    ASSERT_TRUE(was_released(41));
    ASSERT_EQ(pending_subscriptions_live_count(), 0);
    ASSERT_EQ(pending_subscriptions_pending_count(), 0);
}

/* ── Registration ────────────────────────────────────────────────── */

void register_pending_subscriptions_tests(void) {
    printf("[pending_subscriptions]\n");
    RUN_TEST(parked_subscription_binds_on_flush);
    RUN_TEST(flush_carries_entity_events_and_flags_through);
    RUN_TEST(flush_of_another_component_leaves_entry_parked);
    RUN_TEST(flush_binds_every_entry_for_the_component);
    RUN_TEST(second_flush_does_not_rebind);
    RUN_TEST(flush_with_undefined_index_is_a_noop);
    RUN_TEST(unparking_hands_back_the_callback_ref);
    RUN_TEST(unparked_subscription_never_binds);
    RUN_TEST(unparking_twice_reports_not_found);
    RUN_TEST(stale_handle_does_not_hit_recycled_slot);
    RUN_TEST(unknown_handle_reports_not_found);
    RUN_TEST(unsubscribe_after_bind_forwards_to_the_real_id);
    RUN_TEST(table_is_bounded_and_reports_overflow);
    RUN_TEST(bound_entries_still_occupy_a_slot);
    RUN_TEST(rejects_empty_engine_name);
    RUN_TEST(reset_releases_parked_refs);
    RUN_TEST(reset_does_not_release_bound_refs);
    RUN_TEST(failed_bind_drops_the_entry);
}
