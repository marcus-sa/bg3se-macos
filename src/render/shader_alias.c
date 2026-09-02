/**
 * shader_alias.c — see shader_alias.h.
 *
 * Two distinct things break shader lookups for Windows-authored mods on macOS,
 * and they compose:
 *
 *  1. A cloned material embeds a UUID in the MATERIAL name
 *     (`CHAR_Hair_<uuid>_STI_DEF`). The clone's shaders are byte-identical
 *     recompiles of the base material's, so dropping the segment is exact.
 *
 *  2. A mod that re-ships a stock Larian material keeps the material name but
 *     moves it under its own namespace:
 *
 *       mod : Public/DemonHunter_<uuid>/Assets/Materials/Effects/Decal/X.bshd
 *       base: Public/Shared/            Assets/Materials/Effects/Decal/X.bshd
 *
 *     Shader names reaching GetShader are full paths, so the mod copy is a
 *     different key even though it names the same material. On Windows the
 *     mod's own DXBC self-registers under that key; on macOS it cannot,
 *     because these mods ship no Metal shaders at all -- the DemonHunter pak
 *     carries 954 _DX11, 954 _Vulkan and 915 _DX12 descriptors and zero
 *     _Metal. Every material it declares therefore misses.
 *
 * A miss returns the invalid-ShaderID sentinel, which reaches the renderer as
 * a null pipeline: `ls::DecalObject::Render` then faults on a null deref
 * (KERN_INVALID_ADDRESS at 0x30, EmissiveRenderStage worker thread) -- the
 * "click Immolation Aura, game dies instantly" crash. Rewriting the namespace
 * to `Shared` resolves all six of that effect's shaders against Materials.pak
 * (verified by parsing the pak file tables).
 */

#include "shader_alias.h"

#include <string.h>

bool shader_alias_strip_uuid(const char *name, char *out, size_t out_size) {
    static const int groups[] = {8, 4, 4, 4, 12};
    if (!name || !out) return false;

    size_t len = strlen(name);
    for (size_t i = 0; i + 37 <= len; i++) {
        if (name[i] != '_') continue;
        size_t p = i + 1;
        bool ok = true;
        for (int g = 0; g < 5 && ok; g++) {
            for (int k = 0; k < groups[g]; k++, p++) {
                char c = name[p];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                      || (c >= 'A' && c <= 'F'))) { ok = false; break; }
            }
            if (ok && g < 4) {
                if (name[p] != '-') { ok = false; }
                p++;
            }
        }
        if (!ok) continue;
        /* Must end the name or be followed by another underscore part. A `/`
         * here means it is a mod FOLDER, not a material clone: stripping it
         * would name a directory that does not exist. */
        if (name[p] != '\0' && name[p] != '_') continue;
        size_t base_len = i + (len - p);
        if (base_len + 1 > out_size) return false;
        memcpy(out, name, i);
        memcpy(out + i, name + p, len - p + 1);
        return true;
    }
    return false;
}

bool shader_alias_base_root(const char *name, char *out, size_t out_size) {
    static const char kShared[] = "Shared";
    if (!name || !out) return false;

    /* Last path component equal to "Public/". */
    const char *pub = NULL;
    for (const char *p = name; (p = strstr(p, "Public/")) != NULL; p++) {
        if (p == name || p[-1] == '/') pub = p;
    }
    if (!pub) return false;

    const char *seg = pub + 7;                  /* the <folder> component */
    const char *slash = strchr(seg, '/');
    if (!slash || slash == seg) return false;

    size_t seg_len = (size_t)(slash - seg);
    if (seg_len == sizeof(kShared) - 1
        && memcmp(seg, kShared, seg_len) == 0) {
        return false;                           /* already the base namespace */
    }

    size_t head = (size_t)(seg - name);
    size_t tail = strlen(slash);
    if (head + (sizeof(kShared) - 1) + tail + 1 > out_size) return false;

    memcpy(out, name, head);
    memcpy(out + head, kShared, sizeof(kShared) - 1);
    memcpy(out + head + sizeof(kShared) - 1, slash, tail + 1);
    return true;
}

int shader_alias_candidates(const char *name,
                            char out[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX]) {
    int n = 0;
    if (!name) return 0;

    /* 1. material-name clone, same namespace */
    if (shader_alias_strip_uuid(name, out[n], PATH_MAX)) n++;

    /* 2. stock material re-shipped under a mod namespace */
    if (shader_alias_base_root(name, out[n], PATH_MAX)) {
        char rooted[PATH_MAX];
        memcpy(rooted, out[n], strlen(out[n]) + 1);
        n++;
        /* 3. both at once: a clone inside a mod namespace */
        if (shader_alias_strip_uuid(rooted, out[n], PATH_MAX)) n++;
    }

    /* Drop anything identical to the original or to an earlier candidate. */
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i], name) == 0) continue;
        bool dup = false;
        for (int j = 0; j < kept && !dup; j++) {
            dup = strcmp(out[i], out[j]) == 0;
        }
        if (dup) continue;
        if (kept != i) memcpy(out[kept], out[i], strlen(out[i]) + 1);
        kept++;
    }
    return kept;
}
