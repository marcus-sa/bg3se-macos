/**
 * pipeline_wait_guard.c — engine bugfix: unbounded pipeline-cache wait.
 *
 * rf::IAppStage::AddPipelineState caches pipeline entries by descriptor hash.
 * A losing racer that finds an existing entry waits for its compiled pipeline:
 *
 *   106410f44  ldr  x8, [x20, #0x8]   ; entry->pipeline
 *   106410f48  cbnz x8, 1064114c4     ; ready -> return entry (x20)
 *   106410f4c  bl   _pthread_yield_np
 *   106410f50  b    106410f44         ; wait forever
 *
 * But the producer stores the compile result UNCONDITIONALLY, null included
 * (106411250: str x0, [x20, #0x8] right after CreatePipelineState) — so a
 * failed compile leaves a permanently-null entry and every later request for
 * the same descriptor spins here forever. Any shader missing on macOS produces
 * exactly that: 61 misses in one session, then the game froze mid-frame with
 * no crash report, log simply stopping.
 *
 * The first version of this fix retargeted the back-branch straight to the
 * exit, so a waiter yielded ONCE and returned. That over-corrected: it also
 * fired for compiles legitimately in flight during parallel loads, whose
 * consumers then cached the null — origin models and character-creation list
 * tiles stopped rendering. A wait bounded at one iteration is not a bounded
 * wait; it needs a deadline.
 *
 * So retarget the back-branch into a stub that asks us whether to keep going:
 *
 *   stub+0x00  ldr  x16, stub+0x20    ; literal: &pipeline_wait_should_continue
 *   stub+0x04  mov  x0, x20           ; the cache entry being waited on
 *   stub+0x08  blr  x16
 *   stub+0x0c  cbz  x0, stub+0x14     ; 0 -> give up
 *   stub+0x10  b    106410f44         ; keep waiting
 *   stub+0x14  b    1064114c4         ; exit, returns x20 as a failed compile
 *   stub+0x20  .quad <fn>
 *
 * Calling C here is safe precisely because the loop already contains
 * `bl _pthread_yield_np`: x0-x18 and x30 are dead across it by construction,
 * and the live state (x19-x21) is callee-saved, which our function preserves.
 *
 * Giving up returns the entry with a null pipeline — the same value the
 * winning racer returns when its compile fails, and null_pipeline_guard.c now
 * makes the draw sites skip rather than dereference it.
 */

#include "pipeline_wait_guard.h"
#include "../core/logging.h"
#include "../hooks/arm64_hook.h"
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define ADDR_BACK_BRANCH  0x106410f50ULL  /* b 106410f44 */
#define ADDR_LOOP_HEAD    0x106410f44ULL
#define ADDR_EXIT         0x1064114c4ULL
#define ADDR_STUB         0x1059fa348ULL  /* 24 words of brk #0x1, 8-aligned */

#define ORIG_BACK_BRANCH  0x17FFFFFDu     /* b 106410f44 */
#define INSN_BRK1         0xD4200020u
#define STUB_WORDS        10              /* 8 insns + 8-byte literal */

/* How long a waiter tolerates a not-yet-compiled pipeline. Long enough that a
 * compile genuinely in flight during a parallel load still wins (the bug this
 * replaces gave it one yield), short enough that a permanently-null entry does
 * not read as a hang. */
#define WAIT_BUDGET_NS  (2ull * 1000ull * 1000ull * 1000ull)

static _Thread_local const void *s_entry = NULL;
static _Thread_local uint64_t s_start = 0;
static int s_gave_up_logged = 0;

/* Returns non-zero to keep waiting, zero to give up. Called from the stub. */
__attribute__((used))
static uint64_t pipeline_wait_should_continue(const void *entry) {
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);

    if (entry != s_entry) {        /* a different wait — start its clock */
        s_entry = entry;
        s_start = now;
        return 1;
    }
    if (now - s_start > WAIT_BUDGET_NS) {
        s_entry = NULL;
        if (s_gave_up_logged < 8) {
            s_gave_up_logged++;
            LOG_CORE_INFO("PipelineWaitGuard: gave up after %llums waiting on "
                          "pipeline entry %p (failed compile); returning it "
                          "unbuilt so the draw is skipped",
                          (unsigned long long)(WAIT_BUDGET_NS / 1000000ull),
                          entry);
        }
        return 0;
    }
    return 1;
}

static uint32_t enc_b(uint64_t from, uint64_t to) {
    return 0x14000000u | (uint32_t)((((int64_t)to - (int64_t)from) >> 2) & 0x03FFFFFFu);
}

static bool s_installed = false;

bool pipeline_wait_guard_init(void *binary_base) {
    if (s_installed) return true;

    uintptr_t slide = (uintptr_t)binary_base - 0x100000000ULL;
    uint32_t *back = (uint32_t *)(ADDR_BACK_BRANCH + slide);
    uint32_t *stub = (uint32_t *)(ADDR_STUB + slide);

    if (*back != ORIG_BACK_BRANCH) {
        LOG_CORE_INFO("PipelineWaitGuard: NOT applied — 0x%llx reads 0x%08x, "
                      "expected 0x%08x (different build?)",
                      (unsigned long long)ADDR_BACK_BRANCH, *back,
                      ORIG_BACK_BRANCH);
        return false;
    }
    for (int i = 0; i < STUB_WORDS; i++) {
        if (stub[i] != INSN_BRK1) {
            LOG_CORE_INFO("PipelineWaitGuard: NOT applied — padding at "
                          "0x%llx+%d reads 0x%08x, expected 0x%08x",
                          (unsigned long long)ADDR_STUB, i * 4, stub[i],
                          INSN_BRK1);
            return false;
        }
    }

    uint64_t fn = (uint64_t)(uintptr_t)&pipeline_wait_should_continue;
    const uint32_t words[8] = {
        0x58000110u,                                   /* ldr x16, stub+0x20 */
        0xAA1403E0u,                                   /* mov x0, x20        */
        0xD63F0200u,                                   /* blr x16            */
        0xB4000040u,                                   /* cbz x0, stub+0x14  */
        enc_b(ADDR_STUB + 0x10, ADDR_LOOP_HEAD),
        enc_b(ADDR_STUB + 0x14, ADDR_EXIT),
        0xD503201Fu,                                   /* nop                */
        0xD503201Fu,                                   /* nop                */
    };
    for (int i = 0; i < 8; i++) {
        if (!arm64_write_instruction(&stub[i], words[i])) {
            LOG_CORE_INFO("PipelineWaitGuard: stub write failed at +%d", i * 4);
            return false;
        }
    }
    /* The literal the LDR above reads, at stub+0x20 (8-byte aligned). */
    if (!arm64_write_instruction(&stub[8], (uint32_t)(fn & 0xFFFFFFFFu)) ||
        !arm64_write_instruction(&stub[9], (uint32_t)(fn >> 32))) {
        LOG_CORE_INFO("PipelineWaitGuard: literal write failed");
        return false;
    }

    if (!arm64_write_instruction(back, enc_b(ADDR_BACK_BRANCH, ADDR_STUB))) {
        LOG_CORE_INFO("PipelineWaitGuard: back-branch write failed");
        return false;
    }

    s_installed = true;
    LOG_CORE_INFO("PipelineWaitGuard: pipeline-cache wait bounded at %llums "
                  "(failed compiles no longer freeze requesters)",
                  (unsigned long long)(WAIT_BUDGET_NS / 1000000ull));
    return true;
}
