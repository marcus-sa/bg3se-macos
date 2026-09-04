#ifndef EFFECT_LIST_GUARD_H
#define EFFECT_LIST_GUARD_H

#include <stdbool.h>

// Makes ls::EffectComponent::ForceStop step over a sub-effect entry whose
// pointer is not a usable address, instead of dereferencing it. See the .c.
// Returns true if the site was patched.
bool effect_list_guard_init(void *binary_base);

#endif /* EFFECT_LIST_GUARD_H */
