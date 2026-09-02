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
    ALIAS_DECAL,     /* + namespace rewriting, decals only (default) */
    ALIAS_FULL       /* + namespace rewriting for every category */
} AliasMode;

/* Clones only. The two mechanisms here have different track records, and a
 * control run with aliasing disabled entirely finally separated them:
 *
 *   UUID strip (same namespace) -- collapses a mod's cloned material onto the
 *     copy it was cloned from, inside the mod's OWN namespace. Proven good:
 *     it is what stopped the Arch Traitor set freezing the UI, and the window
 *     where it ran alone produced no corruption reports.
 *
 *   Namespace rewrite (Public/<mod>/ -> Public/Shared/) -- repoints a mod
 *     material at Larian's. Screen corruption appeared with it and tracked its
 *     breadth through every narrowing attempted (by category, by render
 *     variant); disabling substitution entirely made it stop.
 *
 * Name equality is not data-layout equality: the mod compiled its own shaders
 * from its own copy of a material, and handing that material's parameter block
 * to Larian's current Metal build of the same-named shader binds fine and then
 * reads the wrong constants and texture slots. That is the corruption, and no
 * amount of scoping fixes it -- the substitution itself is unsound.
 *
 * The cost is honest and localized: DemonHunter's effects have no Metal
 * shaders at all, so they miss, and a decal miss still faults in
 * ls::DecalObject::Render. A crash confined to one mod's effects beats
 * corrupting every frame the game draws. */
static AliasMode s_mode = ALIAS_UUID;


/* Render variants never substituted. Comma-separated, BG3SE_SHADER_ALIAS_SKIP. */
#define MAX_SKIP_VARIANTS 8
static char s_skip[MAX_SKIP_VARIANTS][16];
static int  s_skip_count = 0;

static void skip_variants_init(void) {
    /* Default: skip nothing. A miss is never free -- it either reaches the
     * renderer as a null pipeline (crash) or leaves a pipeline-cache entry
     * whose compile never completes, which rf::IAppStage::AddPipelineState
     * waits on in an unbounded yield loop (hang). Skipping _VEL produced 130
     * misses in one session: the game stopped bricking but crawled -- a save
     * that rendered only the player for a long stretch before the UI appeared,
     * and an Immolation Aura cast that took forever. The screen corruption it
     * was meant to fix was still there, so _VEL was never the cause. */
    const char *e = getenv("BG3SE_SHADER_ALIAS_SKIP");
    if (!e) return;
    while (*e && s_skip_count < MAX_SKIP_VARIANTS) {
        while (*e == ',' || *e == ' ') e++;
        size_t n = 0;
        while (e[n] && e[n] != ',' && e[n] != ' ') n++;
        if (n > 0 && n < sizeof(s_skip[0])) {
            memcpy(s_skip[s_skip_count], e, n);
            s_skip[s_skip_count][n] = '\0';
            s_skip_count++;
        }
        e += n;
    }
}

static bool variant_is_skipped(const char *name) {
    char v[16];
    if (s_skip_count == 0) return false;
    if (!shader_alias_variant(name, v, sizeof(v))) return false;
    for (int i = 0; i < s_skip_count; i++) {
        if (strcmp(v, s_skip[i]) == 0) return true;
    }
    return false;
}

/* Larian keys material category off the containing folder, and the rewrite
 * preserves it, so the source path tells us the category. */
static bool is_decal_material(const char *name) {
    return strstr(name, "/Decal/") != NULL;
}

static void alias_mode_init(void) {
    const char *e = getenv("BG3SE_SHADER_ALIAS");
    if (!e || !*e) return;
    if (strcmp(e, "off") == 0)        s_mode = ALIAS_OFF;
    else if (strcmp(e, "uuid") == 0)  s_mode = ALIAS_UUID;
    else if (strcmp(e, "decal") == 0) s_mode = ALIAS_DECAL;
    else if (strcmp(e, "full") == 0)  s_mode = ALIAS_FULL;
    else LOG_CORE_INFO("ShaderCloneShim: unknown BG3SE_SHADER_ALIAS='%s' "
                       "(want off|uuid|decal|full); keeping uuid", e);
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

    if (variant_is_skipped(name)) {
        LOG_CORE_INFO("ShaderCloneShim: MISS '%s' (variant not aliased)", name);
        return SHADERID_INVALID;
    }

    bool rewrite = (s_mode == ALIAS_FULL)
                || (s_mode == ALIAS_DECAL && is_decal_material(name));

    if (rewrite) {
        n = shader_alias_candidates(name, cands);
    } else if (s_mode != ALIAS_OFF) {
        if (shader_alias_strip_uuid(name, cands[0], PATH_MAX)) n = 1;
    }

    for (int i = 0; i < n; i++) {
        uint32_t base_fs = fixed_string_intern(cands[i], -1);
        if (base_fs == FS_NULL_INDEX) continue;

        uint64_t base_id = s_orig(mgr, &base_fs);
        if (base_id != SHADERID_INVALID) {
            LOG_CORE_INFO("ShaderCloneShim: '%s' -> '%s' (id=0x%llx)",
                          name, cands[i], (unsigned long long)base_id);
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

    uint64_t resolved = resolve_alias(mgr, name);
    return resolved == SHADERID_INVALID ? id : resolved;
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
    skip_variants_init();
    s_installed = true;
    LOG_CORE_INFO("ShaderCloneShim: installed (mode=%s)",
                  s_mode == ALIAS_OFF   ? "off"
                : s_mode == ALIAS_UUID  ? "uuid (material-name clones only)"
                : s_mode == ALIAS_DECAL ? "decal (clones + namespace rewrite for decals)"
                                        : "full (clones + namespace rewrite, all categories)");
    if (s_skip_count > 0) {
        char buf[128]; size_t o = 0;
        for (int i = 0; i < s_skip_count && o < sizeof(buf) - 1; i++) {
            o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s%s",
                                  i ? "," : "", s_skip[i]);
        }
        LOG_CORE_INFO("ShaderCloneShim: variants never aliased: %s", buf);
    }
    return true;
}
