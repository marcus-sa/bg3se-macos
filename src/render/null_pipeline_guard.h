#ifndef NULL_PIPELINE_GUARD_H
#define NULL_PIPELINE_GUARD_H

#include <stdbool.h>

// Makes a draw whose pipeline failed to compile skip itself, instead of
// dereferencing the null. Patches every known deref site; see the .c.
// Returns true if at least one site was patched.
bool null_pipeline_guard_init(void *binary_base);

#endif /* NULL_PIPELINE_GUARD_H */
