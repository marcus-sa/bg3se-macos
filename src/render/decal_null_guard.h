#ifndef DECAL_NULL_GUARD_H
#define DECAL_NULL_GUARD_H

#include <stdbool.h>

// Skips a decal draw whose pipeline failed to compile, instead of letting
// ls::DecalObject::Render dereference the null. See the .c for the mechanism.
bool decal_null_guard_init(void *binary_base);

#endif /* DECAL_NULL_GUARD_H */
