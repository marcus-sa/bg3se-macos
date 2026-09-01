/*
 * Tier 0: DealDamage hook ABI layout.
 *
 * The DealDamage hook takes ten of its seventeen arguments on the stack. Apple's
 * arm64 ABI packs stack arguments to their natural size rather than giving each
 * an 8-byte slot, so the WIDTH of every small parameter fixes the byte offset of
 * everything behind it. Getting one of them wrong does not fault at the boundary
 * — it hands the original function four shifted arguments, two of which are
 * pointers, which is the same failure class as the dropped result_out register
 * in docs/bugs/wave2-functor-crash-analysis.md.
 *
 * The expected offsets below are read directly off the 4.1.1.7398727 call site
 * at 0x1049e5890, which stores each argument at a literal displacement:
 *   str  x23, [sp]        stp x24, x25, [sp, #0x8]    stp x26, x8,  [sp, #0x18]
 *   strb w9,  [sp, #0x28] str wzr,      [sp, #0x2c]   strb w9,      [sp, #0x30]
 *   stp  x8, x9, [sp, #0x38]
 * See ghidra/offsets/DEALDAMAGE_HOOKS.md.
 */

#include "test_harness.h"
#include <stddef.h>
#include "functor_types.h"

TEST(deal_damage_stack_args_match_call_site) {
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, originator),         0x00);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, classDescriptions),  0x08);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, hit),                0x10);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, attack),             0x18);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, sourceHandle2),      0x20);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, hitWith),            0x28);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, conditionRollIndex), 0x2c);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, entityDamagedEvent), 0x30);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, sourceHandle3),      0x38);
    ASSERT_EQ(offsetof(DealDamageStackArgsLayout, spellId2),           0x40);
}

/*
 * hitWith is the one parameter whose type could plausibly be "fixed" to int by
 * someone reading eoc::EHitWith as an enum. The call site emits `strb`, not
 * `str`, so it is one byte; pin that independently of the offsets above so the
 * failure names the cause.
 */
TEST(deal_damage_hitwith_is_one_byte) {
    DealDamageStackArgsLayout layout;
    ASSERT_EQ(sizeof layout.hitWith, (size_t)1);
    ASSERT_EQ(sizeof layout.entityDamagedEvent, (size_t)1);
    ASSERT_EQ(sizeof layout.conditionRollIndex, (size_t)4);
}

/*
 * HitResult is what result_out points at. Ext.Stats.ExecuteFunctors allocates
 * that buffer itself, so FUNCTOR_RESULT_BUFSZ must stay above the real size.
 * Observed on 7398727 in both StatsFunctorDealDamage::Execute (0x105773558) and
 * ExecuteStatsFunctor (0x10577e650): HitDesc at +0, AttackDesc at +0x1a8,
 * a 0x40-byte zeroed results block at +0x1c8, u32 at +0x208 — 0x20c bytes.
 */
TEST(functor_result_buffer_covers_hit_result) {
    ASSERT_TRUE(FUNCTOR_RESULT_BUFSZ >= 0x20c);
}

void register_deal_damage_abi_tests(void) {
    printf("DealDamage hook ABI:\n");
    RUN_TEST(deal_damage_stack_args_match_call_site);
    RUN_TEST(deal_damage_hitwith_is_one_byte);
    RUN_TEST(functor_result_buffer_covers_hit_result);
}
