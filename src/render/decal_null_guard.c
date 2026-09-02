/**
 * decal_null_guard.c — engine bugfix: decal draw dereferences a null pipeline.
 *
 * A shader that does not exist on macOS (any Windows-authored mod that ships
 * no _Metal.bshd) yields an invalid ShaderID, CreatePipelineState correctly
 * returns null for it, and rf::IAppStage::AddPipelineState stores that null in
 * the cache entry unconditionally. ls::DecalObject::Render then reads it back
 * and walks straight into it:
 *
 *   105dce830  ldr  x8, [x23]         ; render object
 *   105dce834  add  x8, x8, #0x40
 *   105dce838  ldr  x8, [x8]          ; pipeline-cache entry   <- join target
 *   105dce83c  ldr  x8, [x8, #0x8]    ; entry->pipeline == NULL
 *   105dce840  ldrb w9, [x8, #0x30]   ; FAULT
 *
 * Exactly matches the crash: EXC_BAD_ACCESS / KERN_INVALID_ADDRESS at 0x30 on
 * the EmissiveRenderStage worker thread (WT_5), frames DecalObject::Render ->
 * RenderableObjectSet::Render -> EmissiveRenderStage::ExecuteWTKernel. This is
 * the "cast Immolation Aura, game dies instantly" crash.
 *
 * The draw cannot be salvaged -- there is no pipeline to draw it with -- so
 * skip it. The function's epilogue at 105dce98c is a plain register restore
 * plus ret, with no destructors and no cleanup calls, so branching there
 * abandons the draw cleanly. It also skips rf::Model::Draw at 105dce988,
 * which is the point.
 *
 * The check cannot be written in place: 105dce838 is the target of three
 * branches, so the load pair above it cannot be folded to free a slot. Instead
 * retarget the faulting instruction into a four-instruction stub placed in the
 * `brk #0x1` padding between two functions 0x47f1c bytes away -- unreachable
 * filler, well inside B's +-128MB range:
 *
 *   stub+0x0  cbz  x8, stub+0xc     ; null pipeline -> bail
 *   stub+0x4  ldrb w9, [x8, #0x30]  ; the displaced original
 *   stub+0x8  b    105dce844        ; resume
 *   stub+0xc  b    105dce98c        ; epilogue: skip the draw, return
 *
 * Every address and opcode below is verified against the running image before
 * anything is written, so an unrecognised build is left untouched.
 */

#include "decal_null_guard.h"
#include "../core/logging.h"
#include "../hooks/arm64_hook.h"
#include <stdint.h>

#define ADDR_FAULT_INSN   0x105dce840ULL  /* ldrb w9, [x8, #0x30]  */
#define ADDR_RESUME       0x105dce844ULL  /* str  wzr, [sp, #0xa0] */
#define ADDR_EPILOGUE     0x105dce98cULL  /* ldp  x29, x30, [sp, #0x110] */
#define ADDR_STUB         0x105e1675cULL  /* 4 x brk #0x1 padding  */

#define INSN_LDRB_W9_X8_30  0x3940C109u   /* ldrb w9, [x8, #0x30] */
#define INSN_BRK1           0xD4200020u   /* brk  #0x1            */

#define STUB_WORDS 4

/* b <target> from <from> */
static uint32_t enc_b(uint64_t from, uint64_t to) {
    int64_t delta = (int64_t)to - (int64_t)from;
    return 0x14000000u | (uint32_t)((delta >> 2) & 0x03FFFFFFu);
}

/* cbz x8, <target> from <from> */
static uint32_t enc_cbz_x8(uint64_t from, uint64_t to) {
    int64_t delta = (int64_t)to - (int64_t)from;
    return 0xB4000000u | (uint32_t)(((delta >> 2) & 0x7FFFFu) << 5) | 8u;
}

static bool s_installed = false;

bool decal_null_guard_init(void *binary_base) {
    if (s_installed) return true;

    uintptr_t slide = (uintptr_t)binary_base - 0x100000000ULL;
    uint32_t *fault = (uint32_t *)(ADDR_FAULT_INSN + slide);
    uint32_t *stub  = (uint32_t *)(ADDR_STUB + slide);

    if (*fault != INSN_LDRB_W9_X8_30) {
        LOG_CORE_INFO("DecalNullGuard: NOT applied — 0x%llx reads 0x%08x, "
                      "expected 0x%08x (different build?)",
                      (unsigned long long)ADDR_FAULT_INSN, *fault,
                      INSN_LDRB_W9_X8_30);
        return false;
    }
    for (int i = 0; i < STUB_WORDS; i++) {
        if (stub[i] != INSN_BRK1) {
            LOG_CORE_INFO("DecalNullGuard: NOT applied — padding at 0x%llx+%d "
                          "reads 0x%08x, expected brk #1 (0x%08x)",
                          (unsigned long long)ADDR_STUB, i * 4, stub[i],
                          INSN_BRK1);
            return false;
        }
    }

    const uint32_t words[STUB_WORDS] = {
        enc_cbz_x8(ADDR_STUB + 0x0, ADDR_STUB + 0xc),
        INSN_LDRB_W9_X8_30,
        enc_b(ADDR_STUB + 0x8, ADDR_RESUME),
        enc_b(ADDR_STUB + 0xc, ADDR_EPILOGUE),
    };

    /* Stub first: until the fault site is retargeted it is unreachable, so a
     * partial write can never be executed. */
    for (int i = 0; i < STUB_WORDS; i++) {
        if (!arm64_write_instruction(&stub[i], words[i])) {
            LOG_CORE_INFO("DecalNullGuard: stub write failed at +%d", i * 4);
            return false;
        }
    }
    if (!arm64_write_instruction(fault, enc_b(ADDR_FAULT_INSN, ADDR_STUB))) {
        LOG_CORE_INFO("DecalNullGuard: fault-site write failed");
        return false;
    }

    s_installed = true;
    LOG_CORE_INFO("DecalNullGuard: decals with a failed pipeline compile are "
                  "now skipped instead of dereferencing null");
    return true;
}
