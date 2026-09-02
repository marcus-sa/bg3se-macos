/**
 * shader_clone_shim.c — alias clone-named shaders to their base shader.
 *
 * The Windows toolkit clones a base material per preset (hair packs, head
 * packs: `CHAR_Hair_<uuid>`), and the engine derives the 15 shader names from
 * the REGISTERED material name — so a clone requests
 * `CHAR_Hair_<uuid>_STI_DEF` from ls::ShaderManager. On Windows the clone's
 * DXBC files self-register; on macOS shaders are 170-byte descriptors into a
 * prebuilt Materials.metallib, the registry only knows base names, and
 * GetShader misses. The miss produces a pipeline cache entry whose compile
 * never completes, and rf::IAppStage::AddPipelineState waits on it in an
 * unbounded pthread_yield loop — the "select a modded hair and the game
 * freezes at 300% CPU" hang (sampled live, twice, 8s apart, identical stack).
 *
 * Fix at the source: hook GetShader; on a miss, retry the base-shader names
 * shader_alias.c derives from the requested one. The clone's shaders ARE the
 * base shaders (byte-identical recompiles — verified against the Windows
 * Materials.pak), so aliasing is semantically exact. No pak edits needed.
 *
 * The same miss also has a second, deadlier outcome than the hang: an invalid
 * ShaderID reaching the renderer as a null pipeline faults in
 * ls::DecalObject::Render on the EmissiveRenderStage worker. See shader_alias.c
 * for the two name shapes that miss and why rewriting them is exact.
 */

#include "shader_clone_shim.h"
#include "shader_alias.h"
#include "../core/logging.h"
#include "../strings/fixed_string.h"
#include <dobby.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>

// ls::ShaderManager::GetShader(FixedString const&) — local symbol, exact-build
// address (4.1.1.7398727). Returns a 64-bit ShaderID; a miss returns the
// sentinel below (observed at the AppliedMaterial::LoadShaders call sites:
// `mov x8, #-0x40000000000000; cmp x0, x8`).
#define ADDR_GETSHADER          0x106134840ULL
#define SHADERID_INVALID        0xFFC0000000000000ULL

// First instruction of GetShader on the verified build, checked before
// patching so an unknown binary gets no hook (fail closed).
#define GETSHADER_PROLOGUE_0    0xD10303FFu   /* sub sp, sp, #0xc0 */

typedef uint64_t (*GetShaderFn)(void *mgr, const uint32_t *name_fs);
static GetShaderFn s_orig = NULL;
static bool s_installed = false;

/* How hard to try on a miss. Namespace rewriting is newer and far broader than
 * the original UUID strip -- one DemonHunter session aliased 132 distinct
 * materials -- so it gets a switch. Every offline check says the rewrite is
 * exact (clone .bshd files are byte-identical to the plain one, and every
 * alias preserves material name and category), but a wrong shader shows up as
 * GPU garbage, and GPU garbage on this platform has already taken down
 * WindowServer once. BG3SE_SHADER_ALIAS=uuid restores the pre-rewrite
 * behaviour; =off disables aliasing entirely. */
typedef enum {
    ALIAS_OFF = 0,   /* never substitute; log the miss */
    ALIAS_UUID,      /* material-name clones only, same namespace */
    ALIAS_FULL       /* + Public/<mod>/ -> Public/Shared/ rewriting */
} AliasMode;

static AliasMode s_mode = ALIAS_FULL;

static void alias_mode_init(void) {
    const char *e = getenv("BG3SE_SHADER_ALIAS");
    if (!e || !*e) return;
    if (strcmp(e, "off") == 0)       s_mode = ALIAS_OFF;
    else if (strcmp(e, "uuid") == 0) s_mode = ALIAS_UUID;
    else if (strcmp(e, "full") == 0) s_mode = ALIAS_FULL;
    else LOG_CORE_INFO("ShaderCloneShim: unknown BG3SE_SHADER_ALIAS='%s' "
                       "(want off|uuid|full); keeping full", e);
}

/* Cold path only. Kept out of fake_GetShader so the hot path -- every shader
 * lookup the engine makes, on its render and worker threads -- does not
 * reserve the candidate buffers. Inlined, the 3 x PATH_MAX of scratch here
 * grew fake_GetShader's frame from ~1KB to 3104 bytes (`sub sp, sp, #0xc20`),
 * charged unconditionally at entry, long before the miss check. Thread stacks
 * we do not own are not the place to spend 3KB per call for a path that runs
 * on well under 1% of them. */
__attribute__((noinline))
static uint64_t resolve_alias(void *mgr, const char *name) {
    char cands[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX];
    int n = 0;

    if (s_mode == ALIAS_FULL) {
        n = shader_alias_candidates(name, cands);
    } else if (s_mode == ALIAS_UUID) {
        if (shader_alias_strip_uuid(name, cands[0], PATH_MAX)) n = 1;
    }

    for (int i = 0; i < n; i++) {
        uint32_t base_fs = fixed_string_intern(cands[i], -1);
        if (base_fs == FS_NULL_INDEX) continue;

        uint64_t base_id = s_orig(mgr, &base_fs);
        if (base_id != SHADERID_INVALID) {
            LOG_CORE_INFO("ShaderCloneShim: '%s' -> '%s'", name, cands[i]);
            return base_id;
        }
    }

    // Genuine miss: no base shader exists under any alias. These are the names
    // whose invalid IDs end up in pipeline descriptors -- log them, because a
    // null pipeline is a renderer crash, not a dropped draw.
    LOG_CORE_INFO("ShaderCloneShim: MISS '%s' (%d alias candidate(s) tried)",
                  name, n);
    return SHADERID_INVALID;
}

static uint64_t fake_GetShader(void *mgr, const uint32_t *name_fs) {
    uint64_t id = s_orig(mgr, name_fs);
    if (id != SHADERID_INVALID || !name_fs) {
        return id;
    }

    // Shader names arrive as full filesystem paths, not bare material names:
    // a Steam library path plus Public/<mod-uuid>/Assets/Materials/... reaches
    // ~310 chars before the material name even starts, so every buffer in
    // resolve_alias is PATH_MAX and the helpers fail closed above it.
    const char *name = fixed_string_resolve(*name_fs);
    if (!name) {
        LOG_CORE_INFO("ShaderCloneShim: MISS for unresolvable FixedString 0x%x",
                      *name_fs);
        return id;
    }

    return resolve_alias(mgr, name);
}

bool shader_clone_shim_init(void *binary_base) {
    if (s_installed) return true;

    uintptr_t addr = (uintptr_t)binary_base + (ADDR_GETSHADER - 0x100000000ULL);
    uint32_t first = *(const uint32_t *)addr;
    if (first != GETSHADER_PROLOGUE_0) {
        LOG_CORE_INFO("ShaderCloneShim: NOT installed — GetShader prologue "
                      "reads 0x%08x, expected 0x%08x (different build?)",
                      first, GETSHADER_PROLOGUE_0);
        return false;
    }

    if (DobbyHook((void *)addr, (void *)fake_GetShader,
                  (void **)&s_orig) != 0 || !s_orig) {
        LOG_CORE_INFO("ShaderCloneShim: DobbyHook failed");
        return false;
    }

    alias_mode_init();
    s_installed = true;
    LOG_CORE_INFO("ShaderCloneShim: installed (mode=%s)",
                  s_mode == ALIAS_OFF  ? "off"
                : s_mode == ALIAS_UUID ? "uuid (material-name clones only)"
                                       : "full (clones + mod-namespace rewrite)");
    return true;
}
