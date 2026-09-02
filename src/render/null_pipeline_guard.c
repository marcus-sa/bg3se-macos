/**
 * null_pipeline_guard.c — engine bugfix: draws dereference a null pipeline.
 *
 * A shader that does not exist on macOS (any Windows-authored mod shipping no
 * _Metal.bshd) yields an invalid ShaderID, CreatePipelineState correctly
 * returns null, and rf::IAppStage::AddPipelineState stores that null in the
 * cache entry unconditionally (see pipeline_wait_guard.c). Consumers then read
 * it back and walk into it.
 *
 * There is more than one such consumer. Three crashed in sequence -- fixing
 * each one only moved the crash to the next -- so the whole family is patched
 * here rather than one report at a time:
 *
 *   ls::DecalObject::Render        105dce840  ldrb w9, [x8, #0x30]   -> 0x30
 *     x8 = entry->pipeline, loaded at 105dce83c from [entry + 0x8].
 *     Stack: DecalObject::Render -> RenderableObjectSet::Render ->
 *     EmissiveRenderStage::ExecuteWTKernel, on a WT_n worker.
 *
 *   rf::metal::MetalRenderer::PreDraw  10645f734  ldr x8, [x21, #0x20] -> 0x20
 *     x21 is null here. Stack: PreDraw -> RendererCommandBuffer::Submit ->
 *     GameRenderView::FinishRecording, on the main thread.
 *
 * Guarding only the decal moved the crash to PreDraw rather than fixing it,
 * which is why both are patched here. PreDraw is the general path — every draw
 * binds through it — so it covers far more than decals.
 *
 * Each site becomes a four-instruction stub written into `brk #0x1` padding
 * between functions (unreachable filler, well inside B's +-128MB range), with
 * the faulting instruction retargeted to it:
 *
 *   stub+0x0  cbz  <reg>, stub+0xc   ; null -> take the skip path
 *   stub+0x4  <the displaced original load>
 *   stub+0x8  b    <resume>
 *   stub+0xc  b    <skip>
 *
 * The skip targets differ in kind, and both are chosen to be safe:
 *
 *   Decal   -> 105dce98c, the function epilogue: a plain register restore plus
 *              ret, no destructors, no cleanup calls. It also skips
 *              rf::Model::Draw at 105dce988, which is the point.
 *   PreDraw -> 10645f778, the engine's OWN skip path. The instruction right
 *              after the fault is `cbz x8, 10645f778`, so the engine already
 *              handles this draw being unbindable; it just cannot survive the
 *              pointer itself being null. 10645f778 opens with
 *              `ldrsb w8, [x20]`, so it overwrites x8 immediately and does not
 *              care that we arrive without having loaded it.
 *
 * Every address and opcode is verified against the running image before any
 * write, so an unrecognised build is left untouched, and each stub is written
 * before its fault site is retargeted, so it is never reachable half-built.
 */

#include "null_pipeline_guard.h"
#include "../core/logging.h"
#include "../hooks/arm64_hook.h"
#include <stdint.h>

#define INSN_BRK1 0xD4200020u   /* brk #0x1 */
#define STUB_WORDS 4

typedef struct {
    const char *name;
    uint64_t fault;        /* instruction that dereferences the null   */
    uint32_t fault_insn;   /* its expected encoding                    */
    uint32_t reg;          /* register that must be non-null           */
    uint64_t resume;       /* continue here when it is not null        */
    uint64_t skip;         /* go here when it is null                  */
    uint64_t stub;         /* 4 words of brk padding to build the stub */
} GuardSite;

/* Every renderable that unpacks pipeline fields into PrepareDrawData before
 * calling rf::metal::MetalRenderer::PreDraw. Found by scanning for the idiom
 * (ldrb w<n>, [x<m>, #0x30] followed by reads at +0x8/+0x20 of the same
 * register) and keeping the *::Render(RenderObjectData const&, ...) family --
 * fixing them as they crash is whack-a-mole, and three of these had already
 * bitten in sequence: decal, then PreDraw, then particle.
 *
 * Every skip target is that function's own epilogue, and each is the identical
 * canonical shape -- `ldur x8, [x29, #-N]` loading the stack canary, then the
 * __stack_chk_guard compare, register restore, ret. Verified to contain no
 * `bl` between the skip target and the ret, so no destructor or cleanup call
 * is bypassed; the draw submission is all that gets skipped. */
static const GuardSite kSites[] = {
    { "DecalObject::Render",
      0x105dce840ULL, 0x3940C109u /* ldrb w9, [x8, #0x30]  */, 8,
      0x105dce844ULL, 0x105dce98cULL, 0x105e1675cULL },
    { "MetalRenderer::PreDraw",
      0x10645f734ULL, 0xF94012A8u /* ldr  x8, [x21, #0x20] */, 21,
      0x10645f738ULL, 0x10645f778ULL, 0x10623b5dcULL },
    { "FxParticleRenderBatch::Render",
      0x105e44884ULL, 0x3940C008u /* ldrb w8, [x0, #0x30]  */, 0,
      0x105e44888ULL, 0x105e44cd8ULL, 0x106148688ULL },
    { "FxBillboard::BillboardObject::Render",
      0x105e24e6cULL, 0x3940C109u /* ldrb w9, [x8, #0x30]  */, 8,
      0x105e24e70ULL, 0x105e25350ULL, 0x105a025c8ULL },
    { "FxMeshRenderList::Render",
      0x105e37ab0ULL, 0x3940C008u /* ldrb w8, [x0, #0x30]  */, 0,
      0x105e37ab4ULL, 0x105e37ea0ULL, 0x107742a7cULL },
    { "TerrainRO::Render",
      0x1061f4b64ULL, 0x3940C008u /* ldrb w8, [x0, #0x30]  */, 0,
      0x1061f4b68ULL, 0x1061f4ed0ULL, 0x1077464a0ULL },
    { "VelocityObjectData render fn",
      0x106254ec8ULL, 0x3940C008u /* ldrb w8, [x0, #0x30]  */, 0,
      0x106254eccULL, 0x1062562b0ULL, 0x107748a20ULL },
};

static uint32_t enc_b(uint64_t from, uint64_t to) {
    return 0x14000000u | (uint32_t)((((int64_t)to - (int64_t)from) >> 2) & 0x03FFFFFFu);
}

static uint32_t enc_cbz(uint64_t from, uint64_t to, uint32_t reg) {
    return 0xB4000000u
         | (uint32_t)((((((int64_t)to - (int64_t)from) >> 2)) & 0x7FFFF) << 5)
         | (reg & 31u);
}

static bool patch_site(const GuardSite *s, uintptr_t slide) {
    uint32_t *fault = (uint32_t *)(s->fault + slide);
    uint32_t *stub  = (uint32_t *)(s->stub + slide);

    if (*fault != s->fault_insn) {
        LOG_CORE_INFO("NullPipelineGuard: %s NOT patched — 0x%llx reads "
                      "0x%08x, expected 0x%08x (different build?)",
                      s->name, (unsigned long long)s->fault, *fault,
                      s->fault_insn);
        return false;
    }
    for (int i = 0; i < STUB_WORDS; i++) {
        if (stub[i] != INSN_BRK1) {
            LOG_CORE_INFO("NullPipelineGuard: %s NOT patched — padding at "
                          "0x%llx+%d reads 0x%08x, expected 0x%08x",
                          s->name, (unsigned long long)s->stub, i * 4,
                          stub[i], INSN_BRK1);
            return false;
        }
    }

    const uint32_t words[STUB_WORDS] = {
        enc_cbz(s->stub + 0x0, s->stub + 0xc, s->reg),
        s->fault_insn,
        enc_b(s->stub + 0x8, s->resume),
        enc_b(s->stub + 0xc, s->skip),
    };
    for (int i = 0; i < STUB_WORDS; i++) {
        if (!arm64_write_instruction(&stub[i], words[i])) {
            LOG_CORE_INFO("NullPipelineGuard: %s stub write failed at +%d",
                          s->name, i * 4);
            return false;
        }
    }
    if (!arm64_write_instruction(fault, enc_b(s->fault, s->stub))) {
        LOG_CORE_INFO("NullPipelineGuard: %s fault-site write failed", s->name);
        return false;
    }

    LOG_CORE_INFO("NullPipelineGuard: %s guarded (null pipeline now skips the "
                  "draw)", s->name);
    return true;
}

static bool s_installed = false;

bool null_pipeline_guard_init(void *binary_base) {
    if (s_installed) return true;

    uintptr_t slide = (uintptr_t)binary_base - 0x100000000ULL;
    int ok = 0;
    for (size_t i = 0; i < sizeof(kSites) / sizeof(kSites[0]); i++) {
        if (patch_site(&kSites[i], slide)) ok++;
    }

    s_installed = (ok > 0);
    LOG_CORE_INFO("NullPipelineGuard: %d/%zu deref sites guarded", ok,
                  sizeof(kSites) / sizeof(kSites[0]));
    return s_installed;
}
