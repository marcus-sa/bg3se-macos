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

static uint64_t fake_GetShader(void *mgr, const uint32_t *name_fs) {
    uint64_t id = s_orig(mgr, name_fs);
    if (id != SHADERID_INVALID || !name_fs) {
        return id;
    }

    const char *name = fixed_string_resolve(*name_fs);
    if (!name) {
        LOG_CORE_INFO("ShaderCloneShim: MISS for unresolvable FixedString 0x%x",
                      *name_fs);
        return id;
    }

    // Shader names arrive as full filesystem paths, not bare material names:
    // a Steam library path plus Public/<mod-uuid>/Assets/Materials/... reaches
    // ~310 chars before the material name even starts, so every buffer here is
    // PATH_MAX and the helpers fail closed above it.
    char cands[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX];
    int n = shader_alias_candidates(name, cands);

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
    // whose invalid IDs end up in pipeline descriptors — log them, because a
    // null pipeline is a renderer crash, not a dropped draw.
    LOG_CORE_INFO("ShaderCloneShim: MISS '%s' (%d alias candidate(s) tried)",
                  name, n);
    return id;
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

    s_installed = true;
    LOG_CORE_INFO("ShaderCloneShim: clone-named shader lookups now fall back "
                  "to their base shader");
    return true;
}
