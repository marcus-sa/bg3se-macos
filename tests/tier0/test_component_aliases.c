/*
 * Tier 0: BG3SE short component name -> engine class name table.
 *
 * resolve_component_type() in entity_events.c probes <prefix><name><suffix>,
 * which cannot reach a component whose engine name carries an inner namespace
 * or an infix — "CombatantJoinEvent" can never reach
 * esv::combat::JoinEventOneFrameComponent by probing. Because
 * Ext.Entity.OnCreate raises on an unresolvable name, and mods call it at file
 * scope, one missing alias aborts the rest of the chunk: Expansion's
 * BootstrapServer.lua stopped registering at line 2421 for exactly this reason.
 *
 * The lookup binary-searches, so the table's sort order is load-bearing: an
 * out-of-order row is silently unreachable and looks identical to "not an
 * alias". That is asserted here rather than trusted.
 */

#include "test_harness.h"
#include "component_aliases.h"

TEST(alias_resolves_combatant_join_event) {
    const char *e = component_alias_lookup("CombatantJoinEvent");
    ASSERT_NOT_NULL(e);
    ASSERT_STR_EQ(e, "esv::combat::JoinEventOneFrameComponent");
}

TEST(alias_resolves_inner_namespace_one_frame_components) {
    /* One-frame/event components mods commonly subscribe to; every one of these
     * carries an inner namespace, an infix, or both, so none is reachable by
     * prefix probing. */
    ASSERT_STR_EQ(component_alias_lookup("SpellCastEvent"),
                  "eoc::spell_cast::CastEventOneFrameComponent");
    ASSERT_STR_EQ(component_alias_lookup("SpellCastFinishedEvent"),
                  "eoc::spell_cast::FinishedEventOneFrameComponent");
    ASSERT_STR_EQ(component_alias_lookup("ServerDeathRequest"),
                  "esv::death::DeathRequestOneFrameComponent");
    ASSERT_STR_EQ(component_alias_lookup("ServerResurrectedEvent"),
                  "esv::death::ResurrectedEventOneFrameComponent");
    ASSERT_STR_EQ(component_alias_lookup("ServerRollStartRequest"),
                  "esv::active_roll::StartRequestOneFrameComponent");
    ASSERT_STR_EQ(component_alias_lookup("HitNotificationRequest"),
                  "esv::hit::HitNotificationRequestOneFrameComponent");
}

TEST(alias_resolves_added_spells) {
    ASSERT_STR_EQ(component_alias_lookup("AddedSpells"),
                  "eoc::spell::AddedSpellsComponent");
    ASSERT_STR_EQ(component_alias_lookup("SpellContainer"),
                  "eoc::spell::ContainerComponent");
}

TEST(alias_disambiguates_level) {
    /* The probe reaches eoc:: before ls::, so "Level" resolved to
     * eoc::LevelComponent before this table existed. Windows binds "Level" to
     * ls::LevelComponent and gives eoc::LevelComponent its own name. Both must
     * stay reachable, under the names Windows uses. */
    ASSERT_STR_EQ(component_alias_lookup("Level"), "ls::LevelComponent");
    ASSERT_STR_EQ(component_alias_lookup("EocLevel"), "eoc::LevelComponent");
}

TEST(alias_unknown_and_degenerate_names_return_null) {
    ASSERT_NULL(component_alias_lookup("NoSuchComponentName"));
    ASSERT_NULL(component_alias_lookup(""));
    ASSERT_NULL(component_alias_lookup(NULL));
    /* A full engine name is not an alias key; callers match those directly. */
    ASSERT_NULL(component_alias_lookup("esv::combat::JoinEventOneFrameComponent"));
}

TEST(alias_table_is_sorted_and_unique) {
    size_t n = component_alias_count();
    ASSERT_TRUE(n > 500);
    for (size_t i = 1; i < n; i++) {
        const char *prev = component_alias_short_at(i - 1);
        const char *cur = component_alias_short_at(i);
        ASSERT_NOT_NULL(prev);
        ASSERT_NOT_NULL(cur);
        ASSERT_TRUE(strcmp(prev, cur) < 0);
    }
    ASSERT_NULL(component_alias_short_at(n));
    ASSERT_NULL(component_alias_engine_at(n));
}

TEST(alias_every_row_is_lookupable_and_well_formed) {
    size_t n = component_alias_count();
    for (size_t i = 0; i < n; i++) {
        const char *s = component_alias_short_at(i);
        const char *e = component_alias_engine_at(i);
        ASSERT_NOT_NULL(e);
        /* A short name that already contains "::" would shadow a full engine
         * name at the alias step and hand back a different component. */
        ASSERT_NULL(strstr(s, "::"));
        /* Every target must be namespace-qualified: the resolver looks it up in
         * the registry, whose keys are always engine class names. */
        ASSERT_NOT_NULL(strstr(e, "::"));
        ASSERT_STR_EQ(component_alias_lookup(s), e);
    }
}

void register_component_alias_tests(void);
void register_component_alias_tests(void) {
    printf("--- Component Alias Tests ---\n");
    RUN_TEST(alias_resolves_combatant_join_event);
    RUN_TEST(alias_resolves_inner_namespace_one_frame_components);
    RUN_TEST(alias_resolves_added_spells);
    RUN_TEST(alias_disambiguates_level);
    RUN_TEST(alias_unknown_and_degenerate_names_return_null);
    RUN_TEST(alias_table_is_sorted_and_unique);
    RUN_TEST(alias_every_row_is_lookupable_and_well_formed);
}
