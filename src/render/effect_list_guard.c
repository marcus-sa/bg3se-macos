/**
 * effect_list_guard.c — engine bugfix: effect teardown walks a dangling entry.
 *
 * Distinct from null_pipeline_guard.c, and it has to be. Those seven sites are
 * all the same shape -- a draw unpacks a pipeline the compiler never produced,
 * finds null, and walks into it -- and all seven are on the RENDER path, so
 * skipping the draw is a complete answer. This one is neither:
 *
 *   ls::EffectComponent::ForceStop  105de0554  ldrb w10, [x10, #0x2d]
 *     Stack: ForceStop -> FxParticleSystem::Reload -> EffectsManager::
 *     ProcessInvoke -> SystemUpdate<ls::EffectsManager>, on the GameThread.
 *
 * That is the effect SIMULATION, not the renderer, which is why every render
 * guard missed it. A mod whose materials have no _Metal.bshd (DemonHunter is
 * the standing example) still gets its effects constructed, ticked and torn
 * down -- we only ever suppressed their draws. The teardown is where it bites.
 *
 * The faulting loop walks an array of sub-effect pointers and reads a flag
 * byte from each:
 *
 *   105de0550  ldr  x10, [x8]           ; x10 = elements[i]
 *   105de0554  ldrb w10, [x10, #0x2d]   ; <- faults
 *   105de0558  cbnz w10, 105de05b0
 *   105de055c  add  x8, x8, #0x8        ; next element
 *   105de0560  subs x9, x9, #0x8
 *   105de0564  b.ne 105de0550
 *
 * The observed x10 was 0x6ccde3653c7f9c4f: not null, so a cbz guard of the
 * kind null_pipeline_guard.c installs passes straight through it. It is data
 * read as a pointer -- a freed entry the list still holds.
 *
 * The check is therefore a validity test, not a null test, and the skip target
 * is the loop's own increment at 105de055c: a bogus entry is stepped over and
 * iteration continues, so the remaining sub-effects still get stopped.
 *
 *   stub+0x00  tbnz x10, #47, stub+0x10   ; bit 47 set -> not a user address
 *   stub+0x04  cbz  x10, stub+0x10        ; null -> also unusable
 *   stub+0x08  ldrb w10, [x10, #0x2d]     ; the displaced original
 *   stub+0x0c  b    105de0558             ; resume
 *   stub+0x10  b    105de055c             ; skip to the next element
 *
 * Bit 47 is the test because arm64 macOS user VAs are 47-bit: every mapped
 * pointer this list can legitimately hold has bits 63:47 clear, and the
 * garbage observed here does not.
 *
 * HONEST LIMIT: this contains the symptom, it does not fix the cause. The list
 * holding a freed entry is the actual bug, and an entry that was freed and
 * then reused still looks like a valid pointer -- it passes both tests here
 * and is read as a live sub-effect. This buys a game that survives the mod's
 * effects; it does not make the effect state correct.
 *
 * The address and opcode are verified against the running image before any
 * write, so an unrecognised build is left untouched, and the stub is written
 * before the fault site is retargeted, so it is never reachable half-built.
 */

#include "effect_list_guard.h"
#include "../core/logging.h"
#include "../hooks/arm64_hook.h"
#include <stdint.h>

#define INSN_BRK1   0xD4200020u   /* brk #0x1 */
#define STUB_WORDS  5

/* ls::EffectComponent::ForceStop on the verified build (4.1.1.7398727). */
#define FAULT_ADDR  0x105de0554ULL
#define FAULT_INSN  0x3940B54Au   /* ldrb w10, [x10, #0x2d] */
#define FAULT_REG   10u
#define RESUME_ADDR 0x105de0558ULL
#define SKIP_ADDR   0x105de055cULL

/* 24 words of brk padding; we use 5. Verified unused by the seven stubs in
 * null_pipeline_guard.c, and 3.9MB from the fault site -- well inside B's
 * +-128MB range. */
#define STUB_ADDR   0x1059fa348ULL

/* Address bits 63:47 must be clear for a mapped user pointer on arm64 macOS. */
#define PTR_TAG_BIT 47u

static uint32_t enc_b(uint64_t from, uint64_t to) {
    return 0x14000000u | (uint32_t)((((int64_t)to - (int64_t)from) >> 2) & 0x03FFFFFFu);
}

static uint32_t enc_cbz(uint64_t from, uint64_t to, uint32_t reg) {
    return 0xB4000000u
         | (uint32_t)(((((int64_t)to - (int64_t)from) >> 2) & 0x7FFFF) << 5)
         | (reg & 31u);
}

static uint32_t enc_tbnz(uint64_t from, uint64_t to, uint32_t reg, uint32_t bit) {
    return 0x37000000u
         | ((bit >> 5) & 1u) << 31
         | ((bit & 31u) << 19)
         | (uint32_t)(((((int64_t)to - (int64_t)from) >> 2) & 0x3FFFu) << 5)
         | (reg & 31u);
}

static bool s_installed = false;

bool effect_list_guard_init(void *binary_base) {
    if (s_installed) return true;

    uintptr_t slide = (uintptr_t)binary_base - 0x100000000ULL;
    uint32_t *fault = (uint32_t *)(FAULT_ADDR + slide);
    uint32_t *stub  = (uint32_t *)(STUB_ADDR + slide);

    if (*fault != FAULT_INSN) {
        LOG_CORE_INFO("EffectListGuard: NOT patched — 0x%llx reads 0x%08x, "
                      "expected 0x%08x (different build?)",
                      (unsigned long long)FAULT_ADDR, *fault, FAULT_INSN);
        return false;
    }
    for (int i = 0; i < STUB_WORDS; i++) {
        if (stub[i] != INSN_BRK1) {
            LOG_CORE_INFO("EffectListGuard: NOT patched — padding at 0x%llx+%d "
                          "reads 0x%08x, expected 0x%08x",
                          (unsigned long long)STUB_ADDR, i * 4, stub[i],
                          INSN_BRK1);
            return false;
        }
    }

    const uint32_t words[STUB_WORDS] = {
        enc_tbnz(STUB_ADDR + 0x0, STUB_ADDR + 0x10, FAULT_REG, PTR_TAG_BIT),
        enc_cbz (STUB_ADDR + 0x4, STUB_ADDR + 0x10, FAULT_REG),
        FAULT_INSN,
        enc_b   (STUB_ADDR + 0xc, RESUME_ADDR),
        enc_b   (STUB_ADDR + 0x10, SKIP_ADDR),
    };
    for (int i = 0; i < STUB_WORDS; i++) {
        if (!arm64_write_instruction(&stub[i], words[i])) {
            LOG_CORE_INFO("EffectListGuard: stub write failed at +%d", i * 4);
            return false;
        }
    }
    if (!arm64_write_instruction(fault, enc_b(FAULT_ADDR, STUB_ADDR))) {
        LOG_CORE_INFO("EffectListGuard: fault-site write failed");
        return false;
    }

    s_installed = true;
    LOG_CORE_INFO("EffectListGuard: EffectComponent::ForceStop guarded "
                  "(unusable sub-effect entry now skipped)");
    return true;
}
