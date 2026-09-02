/**
 * shader_alias.h — base-shader name candidates for a failed shader lookup.
 *
 * Pure string logic, no game or Dobby dependency, so it is covered by the
 * tier0 suite. Used by shader_clone_shim.c on a ls::ShaderManager::GetShader
 * miss.
 */
#ifndef SHADER_ALIAS_H
#define SHADER_ALIAS_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>

#define SHADER_ALIAS_MAX_CANDIDATES 3

/* Strip one `_<8-4-4-4-12 hex uuid>` segment from a shader name. The segment
 * must end the name or be followed by `_`, so a mod-folder UUID (which is
 * followed by `/`) is deliberately NOT stripped -- see shader_alias_base_root.
 * Returns true and writes the shortened name when a segment was removed. */
bool shader_alias_strip_uuid(const char *name, char *out, size_t out_size);

/* Rewrite the `Public/<folder>/` path component to `Public/Shared/`, mapping a
 * mod-namespaced material onto the stock Larian one it was copied from.
 * Returns false when there is no such component, or it is already `Shared`. */
bool shader_alias_base_root(const char *name, char *out, size_t out_size);

/* Extract the render-variant token from a shader name: the last `_`-separated
 * token before the `_Metal.bshd` tail (DEF, DEP, DEPS, DEPST, EMI, FOR, VEL,
 * BAKE...). Returns false when the name has no such shape. */
bool shader_alias_variant(const char *name, char *out, size_t out_size);

/* Fill `out` with the names to retry, in priority order, and return how many
 * were written (0 when `name` has no alias shape at all). */
int shader_alias_candidates(const char *name,
                            char out[SHADER_ALIAS_MAX_CANDIDATES][PATH_MAX]);

#endif /* SHADER_ALIAS_H */
