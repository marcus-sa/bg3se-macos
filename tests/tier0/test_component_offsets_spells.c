/*
 * Tier 0: the spell components that decode a DynamicArray<eoc::spell::SpellMeta>.
 *
 * A dynamic-array property carries two numbers the compiler cannot check for
 * us — the element type tag and the element stride — and both fail silently.
 * elemType 0 routes to the generic stub branch, which hands mods a table of
 * __ptr/__index/__size instead of the spell, and a wrong stride lands element N
 * inside element N-1 without faulting. eoc::spell::AddedSpellsComponent shipped
 * with elemType 0 / elemSize 0, so AddedSpells.Spells[i].SpellId was nil.
 *
 * Evidence that AddedSpellsComponent really is Array<SpellMeta> on 4.1.1.7398727
 * (transcribed in ghidra/offsets/ADDED_SPELLS_COMPONENT.md):
 *   ecs::_private::GetComponentDestructor<eoc::spell::AddedSpellsComponent>
 *     @0x101f6f9d8 is a single `b 0x1019d8f64`, i.e. a tail call into
 *     ls::DynamicArray<eoc::spell::SpellMeta,...>::~DynamicArray with x0
 *     unadjusted — array at +0x00, and nothing else in the struct owns memory.
 *   eoc::spell::ContainerComponent @0x1019d8f60 has the identical destructor;
 *   eoc::spell::BookComponent @0x1019dd728 instead does `add x0, x0, #0x8`
 *   before branching to ~DynamicArray<SpellData>, so the test discriminates
 *   both the offset and the element type.
 *   sizeof = 0x10 from `mov w1, #0x10` into ComponentFrameStorageAllocRaw at
 *   0x101f70128.
 */

#include "test_harness.h"

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wzero-length-array"
#endif
#include "component_offsets.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static const ComponentPropertyDef *find_prop(const ComponentLayoutDef *layout,
                                             const char *name) {
    for (int i = 0; i < layout->propertyCount; i++) {
        if (strcmp(layout->properties[i].name, name) == 0) {
            return &layout->properties[i];
        }
    }
    return NULL;
}

TEST(added_spells_component_decodes_spell_meta) {
    const ComponentLayoutDef *l = &g_eoc_AddedSpellsComponent_Layout;
    ASSERT_STR_EQ(l->componentName, "eoc::spell::AddedSpellsComponent");
    ASSERT_EQ(l->componentSize, 0x10);

    const ComponentPropertyDef *p = find_prop(l, "Spells");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x00);
    ASSERT_EQ(p->type, FIELD_TYPE_DYNAMIC_ARRAY);
    ASSERT_EQ(p->elemType, ELEM_TYPE_SPELL_META);
    ASSERT_EQ(p->elemSize, SPELL_META_SIZE);
    ASSERT_EQ(p->elemSize, 0x60);
}

TEST(spell_container_component_still_decodes_spell_meta) {
    /* The AddedSpells decode was justified by matching this component's
     * destructor shape, so a change here invalidates that argument. */
    const ComponentLayoutDef *l = &g_SpellContainerComponent_Layout;
    ASSERT_STR_EQ(l->componentName, "eoc::spell::ContainerComponent");
    ASSERT_EQ(l->componentSize, 0x10);

    const ComponentPropertyDef *p = find_prop(l, "Spells");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x00);
    ASSERT_EQ(p->type, FIELD_TYPE_DYNAMIC_ARRAY);
    ASSERT_EQ(p->elemType, ELEM_TYPE_SPELL_META);
    ASSERT_EQ(p->elemSize, SPELL_META_SIZE);
}

TEST(spell_array_header_offsets_match_dynamic_array) {
    /*
     * ~DynamicArray<eoc::spell::SpellMeta> @0x1019d8f64 reads buf_ with
     * `ldr x25, [x19]`, capacity_ with `ldr w8, [x0, #0x8]` and size_ with
     * `ldrsw x8, [x19, #0xc]`. Both components declare componentSize 0x10,
     * which is exactly that header — if a future edit grows either component,
     * the array is no longer the whole struct and the +0x00 offset above needs
     * re-deriving rather than assuming.
     */
    ASSERT_EQ(g_eoc_AddedSpellsComponent_Layout.componentSize,
              g_SpellContainerComponent_Layout.componentSize);
    ASSERT_EQ(g_eoc_AddedSpellsComponent_Layout.componentSize, 8 + 4 + 4);
}

void register_component_offsets_spell_tests(void);
void register_component_offsets_spell_tests(void) {
    printf("--- Spell Component Layout Tests ---\n");
    RUN_TEST(added_spells_component_decodes_spell_meta);
    RUN_TEST(spell_container_component_still_decodes_spell_meta);
    RUN_TEST(spell_array_header_offsets_match_dynamic_array);
}
