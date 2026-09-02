/*
 * Tier 0: lifetime scope semantics.
 *
 * The distinction that matters is between "outside every scope" and "inside a
 * scope that has ended". They are not the same thing, and conflating them made
 * every object created during module bootstrap unusable on the line after it
 * was created:
 *
 *     local passive = Ext.Stats.Get(name)
 *     if string.find(passive.BoostConditions, ...)  -- Lifetime of StatsObject
 *                                                   -- has expired
 *
 * which aborted Expansion's shared bootstrap. It stayed hidden while
 * Ext.Stats.GetStats returned an empty list, so the loop body never ran.
 */

#include "test_harness.h"
#include "lifetime.h"

#include <lauxlib.h>
#include <lualib.h>

static lua_State *lifetime_state(void) {
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lifetime_lua_init(L);
    return L;
}

/* Bootstrap, console commands and the global chunk all run here. */
TEST(outside_every_scope_is_valid_not_expired) {
    lua_State *L = lifetime_state();

    LifetimeHandle h = lifetime_lua_get_current(L);
    ASSERT_TRUE(lifetime_lua_is_valid(L, h));

    lua_close(L);
}

TEST(inside_a_scope_returns_that_scope) {
    lua_State *L = lifetime_state();

    LifetimeHandle scope = lifetime_lua_begin_scope(L);
    ASSERT_EQ(lifetime_lua_get_current(L), scope);
    ASSERT_TRUE(lifetime_lua_is_valid(L, scope));

    lifetime_lua_end_scope(L);
    lua_close(L);
}

/* The actual expiry contract still has to hold: an object stamped inside a
 * scope must stop being valid once that scope ends. */
TEST(a_scope_handle_expires_when_the_scope_ends) {
    lua_State *L = lifetime_state();

    LifetimeHandle scope = lifetime_lua_begin_scope(L);
    ASSERT_TRUE(lifetime_lua_is_valid(L, scope));

    lifetime_lua_end_scope(L);
    ASSERT_FALSE(lifetime_lua_is_valid(L, scope));

    lua_close(L);
}

/* Leaving the outermost scope returns to bootstrap context, which is valid
 * again -- not permanently poisoned. */
TEST(returning_to_no_scope_is_valid_again) {
    lua_State *L = lifetime_state();

    lifetime_lua_begin_scope(L);
    lifetime_lua_end_scope(L);

    ASSERT_TRUE(lifetime_lua_is_valid(L, lifetime_lua_get_current(L)));

    lua_close(L);
}

/* Nesting must still expire inner scopes independently of the outer one. */
TEST(nested_scopes_expire_independently) {
    lua_State *L = lifetime_state();

    LifetimeHandle outer = lifetime_lua_begin_scope(L);
    LifetimeHandle inner = lifetime_lua_begin_scope(L);
    ASSERT_NE(outer, inner);

    lifetime_lua_end_scope(L);
    ASSERT_FALSE(lifetime_lua_is_valid(L, inner));
    ASSERT_TRUE(lifetime_lua_is_valid(L, outer));
    ASSERT_EQ(lifetime_lua_get_current(L), outer);

    lifetime_lua_end_scope(L);
    lua_close(L);
}

void register_lifetime_scope_tests(void) {
    printf("Lifetime scopes:\n");
    RUN_TEST(outside_every_scope_is_valid_not_expired);
    RUN_TEST(inside_a_scope_returns_that_scope);
    RUN_TEST(a_scope_handle_expires_when_the_scope_ends);
    RUN_TEST(returning_to_no_scope_is_valid_again);
    RUN_TEST(nested_scopes_expire_independently);
    printf("\n");
}
