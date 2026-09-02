/*
 * Tier 0: the esv::Character / esv::Item property layouts.
 *
 * These two are the only *proxy* components with derived field offsets: the ECS
 * slot is 8 bytes holding a pointer to a separately heap-allocated object, so
 * every offset below applies one dereference in. If .isProxy is ever dropped,
 * component_lookup_by_index() returns the slot address instead and
 * ServerCharacter.Template reads 0xC8 bytes into the storage page — a plausible
 * pointer, silently wrong. That flag is therefore asserted, not assumed.
 *
 * Evidence for the offsets (transcribed with full disassembly in
 * ghidra/offsets/SERVER_CHARACTER_ITEM_LAYOUT.md, build 4.1.1.7398727 arm64):
 *
 *   esv::Character::GetTemplate() @0x10516929c   ldr x0, [x0, #0xc8] / ret
 *   esv::Item::GetTemplate()      @0x105461c5c   ldr x0, [x0, #0x48] / ret
 *   sizeof from the AddComponent allocations: `mov w0, #0x1a8` @0x1052dbc2c
 *   and `mov w0, #0xb0` @0x1052d8844, each followed by the matching ctor.
 *   Proxy-ness from GetComponentDestructor<T> @0x103bdeb6c / @0x103bd304c,
 *   both of which open `ldr x19, [x0]` before destroying and freeing.
 *
 * The short names are asserted too: Windows BG3SE binds these classes to
 * "ServerCharacter" / "ServerItem" (Character.h:62, Item.h:10). The port used to
 * call them "Character" / "Item", which is the same class of port-invented
 * spelling that made entity.Level reach eoc::LevelComponent instead of
 * ls::LevelComponent.
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

static const ComponentPropertyDef *server_prop(const ComponentLayoutDef *layout,
                                               const char *name) {
    for (int i = 0; i < layout->propertyCount; i++) {
        if (strcmp(layout->properties[i].name, name) == 0) {
            return &layout->properties[i];
        }
    }
    return NULL;
}

/* Byte width the property reader will touch at prop->offset. Kept in step with
 * the FieldType values these two layouts actually use; an unexpected type makes
 * the bounds test fail loudly rather than silently skip a field. */
static size_t field_width(FieldType type) {
    switch (type) {
        case FIELD_TYPE_INT32:
        case FIELD_TYPE_UINT32:
        case FIELD_TYPE_FLOAT:
        case FIELD_TYPE_FIXEDSTRING:  return 4;
        case FIELD_TYPE_INT64:
        case FIELD_TYPE_UINT64:
        case FIELD_TYPE_ENTITY_HANDLE: return 8;
        default:                       return 0;
    }
}

static void assert_fields_fit(const ComponentLayoutDef *layout) {
    for (int i = 0; i < layout->propertyCount; i++) {
        const ComponentPropertyDef *p = &layout->properties[i];
        size_t width = field_width(p->type);
        ASSERT_TRUE(width != 0);
        ASSERT_TRUE((size_t)p->offset + width <= (size_t)layout->componentSize);
    }
}

TEST(server_character_layout_is_a_proxy_over_a_424_byte_object) {
    const ComponentLayoutDef *l = &g_esv_Character_Layout;
    ASSERT_STR_EQ(l->componentName, "esv::Character");
    ASSERT_STR_EQ(l->shortName, "ServerCharacter");
    /* sizeof(esv::Character): mov w0, #0x1a8 @0x1052dbc2c before operator new,
     * then bl esv::Character::Character. */
    ASSERT_EQ(l->componentSize, 0x1a8);
    /* The slot is 8 bytes and holds the pointer: mov w1, #0x8 into
     * ComponentFrameStorageAllocRaw @0x1052dbc74, then str x25, [x0]. */
    ASSERT_TRUE(l->isProxy);
}

TEST(server_character_template_pointers_have_derived_offsets) {
    const ComponentLayoutDef *l = &g_esv_Character_Layout;

    /* esv::Character::GetTemplate() is the whole two-instruction function
     * `ldr x0, [x0, #0xc8]; ret`. This is the field AppearanceEditEnhanced
     * reaches for as Ext.Entity.Get(uuid).ServerCharacter.Template. */
    const ComponentPropertyDef *t = server_prop(l, "Template");
    ASSERT_NOT_NULL(t);
    ASSERT_EQ(t->offset, 0xC8);
    ASSERT_EQ(t->type, FIELD_TYPE_UINT64);
    /* Writing it would desync esv::Increase/DecreaseTemplateRefCount, which
     * SetTemplate calls around its own store. */
    ASSERT_TRUE(t->readOnly);

    const ComponentPropertyDef *o = server_prop(l, "OriginalTemplate");
    ASSERT_NOT_NULL(o);
    ASSERT_EQ(o->offset, 0xD0);
    ASSERT_EQ(o->type, FIELD_TYPE_UINT64);

    const ComponentPropertyDef *s = server_prop(l, "TemplateUsedForSpells");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(s->offset, 0xD8);
    ASSERT_EQ(s->type, FIELD_TYPE_UINT64);

    /* LEGACY_CacheTemplatesIfNeeded copies [this+0xd0] into [this+0xd8]; the
     * three pointers are consecutive, and that adjacency is what lets the
     * single-site reads for 0xD0/0xD8 corroborate each other. */
    ASSERT_EQ(o->offset - t->offset, 8);
    ASSERT_EQ(s->offset - o->offset, 8);
}

TEST(server_character_accessor_derived_fields_keep_their_widths) {
    const ComponentLayoutDef *l = &g_esv_Character_Layout;
    const ComponentPropertyDef *p;

    /* GetEntityRef() @0x10516930c is `add x0, x0, #0x20; ret`. Only the leading
     * EntityHandle of ecs::EntityRef is exposed; the trailing EntityWorld* must
     * not be read as part of it. */
    p = server_prop(l, "MyHandle");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x20);
    ASSERT_EQ(p->type, FIELD_TYPE_ENTITY_HANDLE);

    /* IsFlag() @0x105169960 is `ldr x8, [x0, #0x18]; tst x8, x1` — 64 bits, not
     * 32: a UINT32 here would silently drop every flag above bit 31. */
    p = server_prop(l, "Flags");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x18);
    ASSERT_EQ(p->type, FIELD_TYPE_UINT64);

    /* GetCurrentLevel() @0x10516a23c is `add x0, x0, #0x30; ret`, and the ctor
     * stores a 64-bit -1 across 0x30/0x34, i.e. two 4-byte FixedStrings. */
    p = server_prop(l, "Level");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x30);
    ASSERT_EQ(p->type, FIELD_TYPE_FIXEDSTRING);
    p = server_prop(l, "VisualResource");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x34);
    ASSERT_EQ(p->type, FIELD_TYPE_FIXEDSTRING);

    /* GetOwnerUserID @0x105175820 returns `ldr w0, [x19, #0x184]` and
     * GetReservedForUserID @0x1051726dc returns `ldr w0, [x19, #0x188]`:
     * two adjacent 32-bit ids, not one 64-bit value. */
    p = server_prop(l, "UserID");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x184);
    ASSERT_EQ(p->type, FIELD_TYPE_INT32);
    p = server_prop(l, "UserID2");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x188);
    ASSERT_EQ(p->type, FIELD_TYPE_INT32);

    /* GetModifiedMovementSpeed @0x104b21988 does `ldr s0, [x20, #0x18c]` —
     * an s-register load, so a single-precision float. */
    p = server_prop(l, "GeneralSpeedMultiplier");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x18C);
    ASSERT_EQ(p->type, FIELD_TYPE_FLOAT);
}

TEST(server_character_omits_the_fields_that_were_not_derivable) {
    /* Inventory is the one a mod is most likely to reach for, and the handle at
     * 0x168 that Windows' declaration order would suggest resolves through
     * GetComponent<esv::Character> — it holds a character. Exposing it as
     * Inventory would repeat the Level/Constellation mistake, so the absence is
     * asserted rather than left to a future well-meaning edit. */
    const ComponentLayoutDef *l = &g_esv_Character_Layout;
    ASSERT_NULL(server_prop(l, "Inventory"));
    ASSERT_NULL(server_prop(l, "AiActionMachine"));
    ASSERT_NULL(server_prop(l, "OsirisController"));
    ASSERT_NULL(server_prop(l, "Summons"));
}

TEST(server_item_layout_is_a_proxy_over_a_176_byte_object) {
    const ComponentLayoutDef *l = &g_esv_Item_Layout;
    ASSERT_STR_EQ(l->componentName, "esv::Item");
    ASSERT_STR_EQ(l->shortName, "ServerItem");
    /* mov w0, #0xb0 @0x1052d8844 before operator new, then bl esv::Item::Item */
    ASSERT_EQ(l->componentSize, 0xb0);
    ASSERT_TRUE(l->isProxy);
}

TEST(server_item_derived_fields_keep_their_offsets_and_widths) {
    const ComponentLayoutDef *l = &g_esv_Item_Layout;
    const ComponentPropertyDef *p;

    /* esv::Item::GetTemplate() @0x105461c5c is `ldr x0, [x0, #0x48]; ret`. */
    p = server_prop(l, "Template");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x48);
    ASSERT_EQ(p->type, FIELD_TYPE_UINT64);
    ASSERT_TRUE(p->readOnly);

    p = server_prop(l, "OriginalTemplate");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x50);

    /* GetEntityRef() @0x105461ccc, IsFlag() @0x1054625f0, GetCurrentLevel()
     * @0x105463058 — the same three shapes as esv::Character, at the same
     * offsets, which is itself a cross-check on both derivations. */
    p = server_prop(l, "MyHandle");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x20);
    ASSERT_EQ(p->type, FIELD_TYPE_ENTITY_HANDLE);
    p = server_prop(l, "Flags");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x18);
    ASSERT_EQ(p->type, FIELD_TYPE_UINT64);
    p = server_prop(l, "Level");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x30);
    ASSERT_EQ(p->type, FIELD_TYPE_FIXEDSTRING);

    /* SetStatsId @0x105469838 writes the FixedString at 0x9c and stores the
     * looked-up stats object at 0x78. */
    p = server_prop(l, "Stats");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x9C);
    ASSERT_EQ(p->type, FIELD_TYPE_FIXEDSTRING);
    p = server_prop(l, "StatsObject");
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->offset, 0x78);

    /* The four manager pointers are typed by the callee each is passed to:
     * ItemMachine::CreateState, PlanManager::OnSuspend,
     * ls::VariableManager::Visit, StatusMachine::SavegameVisit. They are
     * consecutive, which is what makes the single-callee reads corroborate. */
    ASSERT_EQ(server_prop(l, "ItemMachine")->offset, 0x58);
    ASSERT_EQ(server_prop(l, "PlanManager")->offset, 0x60);
    ASSERT_EQ(server_prop(l, "VariableManager")->offset, 0x68);
    ASSERT_EQ(server_prop(l, "StatusManager")->offset, 0x70);
}

TEST(server_item_omits_the_divergent_tail) {
    /* Windows declares TreasureLevel/Amount/Flags2 at the tail; this build has
     * one int32 at 0xa4 and four separate bytes at 0xa8..0xab, so the Windows
     * order stops holding there. SetAmount @0x10546883c writes no member at all
     * — it queues an inventory StackSystem request. */
    const ComponentLayoutDef *l = &g_esv_Item_Layout;
    ASSERT_NULL(server_prop(l, "Amount"));
    ASSERT_NULL(server_prop(l, "TreasureLevel"));
    ASSERT_NULL(server_prop(l, "Flags2"));
    ASSERT_NULL(server_prop(l, "Vitality"));
    ASSERT_NULL(server_prop(l, "PreviousLevel"));
}

TEST(server_object_fields_stay_inside_the_derived_object_size) {
    /* component_property.c bounds-checks reads against componentSize. A field
     * added past the end would be rejected at runtime and read nil, which looks
     * exactly like "component not present". */
    assert_fields_fit(&g_esv_Character_Layout);
    assert_fields_fit(&g_esv_Item_Layout);
}

TEST(client_object_components_are_marked_as_proxies) {
    /* Same slot-holds-a-pointer shape, proven by each type's
     * GetComponentDestructor opening `ldr x19, [x0]`:
     *   ecl::Character  @0x100eaa7d0   ecl::Item       @0x100e41804
     *   ecl::Projectile @0x100e3fd04   ecl::Scenery    @0x100e3e514
     * They carry no properties yet, but a flat lookup would hand any future
     * field the storage page instead of the object. */
    ASSERT_TRUE(g_ClientCharacterComponent_Layout.isProxy);
    ASSERT_TRUE(g_ClientItemComponent_Layout.isProxy);
    ASSERT_TRUE(g_ClientProjectileComponent_Layout.isProxy);
    ASSERT_TRUE(g_ClientSceneryComponent_Layout.isProxy);

    /* esv::Projectile is deliberately NOT a proxy layout: no field offsets were
     * derived for it, so the layout still models the 8-byte slot itself and
     * ProjectilePtr is the pointer the slot holds. Flipping isProxy without
     * re-deriving the offsets would make ProjectilePtr read the object's vtable
     * pointer instead. */
    ASSERT_FALSE(g_esv_Projectile_Layout.isProxy);
    ASSERT_EQ(g_esv_Projectile_Layout.componentSize, 0x08);
    ASSERT_STR_EQ(g_esv_Projectile_Layout.shortName, "ServerProjectile");
}

void register_server_object_layout_tests(void);
void register_server_object_layout_tests(void) {
    printf("--- Server Object Layout Tests ---\n");
    RUN_TEST(server_character_layout_is_a_proxy_over_a_424_byte_object);
    RUN_TEST(server_character_template_pointers_have_derived_offsets);
    RUN_TEST(server_character_accessor_derived_fields_keep_their_widths);
    RUN_TEST(server_character_omits_the_fields_that_were_not_derivable);
    RUN_TEST(server_item_layout_is_a_proxy_over_a_176_byte_object);
    RUN_TEST(server_item_derived_fields_keep_their_offsets_and_widths);
    RUN_TEST(server_item_omits_the_divergent_tail);
    RUN_TEST(server_object_fields_stay_inside_the_derived_object_size);
    RUN_TEST(client_object_components_are_marked_as_proxies);
}
