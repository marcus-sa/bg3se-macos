/*
 * Tier 0 tests for component_name_resolve.
 *
 * The state that matters here — a component whose engine class this build knows
 * about but whose ComponentTypeIndex the ECS has not assigned yet — exists for
 * only a few seconds during game startup and cannot be staged against the live
 * game. The registry is therefore supplied as a table so that state is a
 * first-class fixture: FakeRow.index == COMPONENT_INDEX_UNDEFINED reproduces
 * exactly the window in which Expansion's BootstrapServer.lua subscribes to
 * CombatantJoinEvent.
 */

#include "test_harness.h"
#include "component_name_resolve.h"
#include "component_registry.h"

typedef struct {
    const char *engine_name;
    uint16_t index;
} FakeRow;

typedef struct {
    const FakeRow *rows;
    size_t count;
} FakeRegistry;

static const FakeRow *fake_find(const FakeRegistry *reg, const char *name) {
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->rows[i].engine_name, name) == 0) return &reg->rows[i];
    }
    return NULL;
}

static bool fake_has_index(const char *name, uint16_t *out_index, void *ud) {
    const FakeRow *row = fake_find((const FakeRegistry *)ud, name);
    if (!row || row->index == COMPONENT_INDEX_UNDEFINED) return false;
    *out_index = row->index;
    return true;
}

static bool fake_is_registered(const char *name, void *ud) {
    return fake_find((const FakeRegistry *)ud, name) != NULL;
}

static ComponentNameResolution resolve(const FakeRegistry *reg, const char *name,
                                       char *engine, uint16_t *index) {
    ComponentNameRegistryView view = { fake_has_index, fake_is_registered,
                                       (void *)reg };
    *index = COMPONENT_INDEX_UNDEFINED;
    engine[0] = '\0';
    return component_name_resolve(name, &view, engine,
                                  COMPONENT_MAX_NAME_LEN, index);
}

/* ── The two names from the live failure reports ─────────────────── */

/* Expansion: Ext.Entity.OnCreate("CombatantJoinEvent", ...) at file scope in
 * BootstrapServer.lua, 4.2s before the ECS assigns the index. */
TEST(combatant_join_event_pending_before_index) {
    const FakeRow rows[] = {
        { "esv::combat::JoinEventOneFrameComponent", COMPONENT_INDEX_UNDEFINED },
    };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "CombatantJoinEvent", engine, &index),
              COMPONENT_NAME_PENDING);
    ASSERT_STR_EQ(engine, "esv::combat::JoinEventOneFrameComponent");
}

TEST(combatant_join_event_resolved_after_index) {
    const FakeRow rows[] = {
        { "esv::combat::JoinEventOneFrameComponent", 1341 },
    };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "CombatantJoinEvent", engine, &index),
              COMPONENT_NAME_RESOLVED);
    ASSERT_EQ(index, 1341);
    ASSERT_STR_EQ(engine, "esv::combat::JoinEventOneFrameComponent");
}

/* AppearanceEditEnhanced: EntitySubscriptions.lua:5 subscribes to "CCState". */
TEST(cc_state_pending_before_index) {
    const FakeRow rows[] = {
        { "eoc::character_creation::StateComponent", COMPONENT_INDEX_UNDEFINED },
    };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "CCState", engine, &index), COMPONENT_NAME_PENDING);
    ASSERT_STR_EQ(engine, "eoc::character_creation::StateComponent");
}

TEST(cc_state_resolved_after_index) {
    const FakeRow rows[] = {
        { "eoc::character_creation::StateComponent", 900 },
    };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "CCState", engine, &index), COMPONENT_NAME_RESOLVED);
    ASSERT_EQ(index, 900);
}

/* The inner-namespace initialism expansion must defer too, not just the alias
 * table — it is the only path to a nested component with no alias row. */
TEST(nested_initialism_pending_before_index) {
    const FakeRow rows[] = {
        { "eoc::character_creation::WidgetComponent", COMPONENT_INDEX_UNDEFINED },
    };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "CCWidget", engine, &index), COMPONENT_NAME_PENDING);
    ASSERT_STR_EQ(engine, "eoc::character_creation::WidgetComponent");
}

/* ── A genuinely unknown name must stay an error ─────────────────── */

TEST(typo_is_unresolved) {
    const FakeRow rows[] = {
        { "eoc::HealthComponent", 12 },
    };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "Healht", engine, &index), COMPONENT_NAME_UNRESOLVED);
}

TEST(empty_registry_is_unresolved) {
    const FakeRegistry reg = { NULL, 0 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "Health", engine, &index), COMPONENT_NAME_UNRESOLVED);
}

/* ── Resolution order is preserved ───────────────────────────────── */

TEST(exact_engine_name_matches) {
    const FakeRow rows[] = { { "eoc::HealthComponent", 12 } };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "eoc::HealthComponent", engine, &index),
              COMPONENT_NAME_RESOLVED);
    ASSERT_EQ(index, 12);
}

TEST(prefix_probe_matches_short_name) {
    const FakeRow rows[] = { { "eoc::HealthComponent", 12 } };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "Health", engine, &index), COMPONENT_NAME_RESOLVED);
    ASSERT_STR_EQ(engine, "eoc::HealthComponent");
}

/* The alias table outranks probing, which is what keeps "Level" on
 * ls::LevelComponent even though the eoc:: probe would answer first. */
TEST(alias_outranks_probe) {
    const FakeRow rows[] = {
        { "eoc::LevelComponent", 7 },
        { "ls::LevelComponent", 8 },
    };
    const FakeRegistry reg = { rows, 2 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "Level", engine, &index), COMPONENT_NAME_RESOLVED);
    ASSERT_STR_EQ(engine, "ls::LevelComponent");
    ASSERT_EQ(index, 8);
}

/* Adding the deferral pass must not re-point a name that already binds: a
 * merely-registered candidate never outranks a live one, regardless of the
 * order the enumeration reaches them in. */
TEST(live_candidate_outranks_registered_one) {
    const FakeRow rows[] = {
        { "eoc::WidgetComponent", COMPONENT_INDEX_UNDEFINED },
        { "esv::WidgetComponent", 44 },
    };
    const FakeRegistry reg = { rows, 2 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "Widget", engine, &index), COMPONENT_NAME_RESOLVED);
    ASSERT_STR_EQ(engine, "esv::WidgetComponent");
    ASSERT_EQ(index, 44);
}

/* Index 0 is a real component's slot, not a sentinel: the guard from 18bde1a
 * distinguishes "unresolved" by COMPONENT_INDEX_UNDEFINED only, so a component
 * that genuinely owns index 0 must still resolve. */
TEST(index_zero_is_a_valid_binding) {
    const FakeRow rows[] = { { "eoc::HealthComponent", 0 } };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "Health", engine, &index), COMPONENT_NAME_RESOLVED);
    ASSERT_EQ(index, 0);
}

/* An alias whose target this build does not carry must fall through to probing
 * rather than fail outright. */
TEST(alias_miss_falls_through_to_probe) {
    const FakeRow rows[] = { { "eoc::LevelComponent", 7 } };
    const FakeRegistry reg = { rows, 1 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;

    ASSERT_EQ(resolve(&reg, "Level", engine, &index), COMPONENT_NAME_RESOLVED);
    ASSERT_STR_EQ(engine, "eoc::LevelComponent");
}

TEST(overlong_name_is_unresolved) {
    const FakeRegistry reg = { NULL, 0 };
    char engine[COMPONENT_MAX_NAME_LEN];
    uint16_t index;
    char huge[COMPONENT_MAX_NAME_LEN];
    memset(huge, 'A', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    ASSERT_EQ(resolve(&reg, huge, engine, &index), COMPONENT_NAME_UNRESOLVED);
}

TEST(rejects_undersized_output_buffer) {
    const FakeRow rows[] = { { "eoc::HealthComponent", 12 } };
    const FakeRegistry reg = { rows, 1 };
    ComponentNameRegistryView view = { fake_has_index, fake_is_registered,
                                       (void *)&reg };
    char small[16];
    uint16_t index = 0;

    ASSERT_EQ(component_name_resolve("Health", &view, small, sizeof(small), &index),
              COMPONENT_NAME_UNRESOLVED);
}

/* ── Registration ────────────────────────────────────────────────── */

void register_component_name_resolve_tests(void) {
    printf("[component_name_resolve]\n");
    RUN_TEST(combatant_join_event_pending_before_index);
    RUN_TEST(combatant_join_event_resolved_after_index);
    RUN_TEST(cc_state_pending_before_index);
    RUN_TEST(cc_state_resolved_after_index);
    RUN_TEST(nested_initialism_pending_before_index);
    RUN_TEST(typo_is_unresolved);
    RUN_TEST(empty_registry_is_unresolved);
    RUN_TEST(exact_engine_name_matches);
    RUN_TEST(prefix_probe_matches_short_name);
    RUN_TEST(alias_outranks_probe);
    RUN_TEST(live_candidate_outranks_registered_one);
    RUN_TEST(index_zero_is_a_valid_binding);
    RUN_TEST(alias_miss_falls_through_to_probe);
    RUN_TEST(overlong_name_is_unresolved);
    RUN_TEST(rejects_undersized_output_buffer);
}
