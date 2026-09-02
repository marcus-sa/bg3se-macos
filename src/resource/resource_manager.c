/**
 * resource_manager.c - Resource Manager Implementation for BG3SE-macOS
 *
 * Provides access to the game's ResourceManager for resource lookup.
 */

#include "resource_manager.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../core/offset_table.h"
#include "../strings/fixed_string.h"
#include <string.h>
#include <strings.h>

// ============================================================================
// Constants and Offsets (Discovered via Ghidra - Dec 21, 2025)
// ============================================================================

// ls::ResourceManager::m_ptr global singleton
// Originally discovered from InitEngine disassembly (Dec 21, 2025).
// Re-derived 2026-07-28 via nm for game build 4.1.1.7209685 (was 0x08a8f070).
// Audited by tests/harness/test_offset_audit.py.
#define OFFSET_RESOURCEMANAGER_PTR  0x08a97070

// ResourceBank offsets within ResourceManager
#define RESOURCEMANAGER_BANK0_OFFSET  0x28  // Primary bank
#define RESOURCEMANAGER_BANK1_OFFSET  0x30  // Secondary bank

// ResourceContainer structure offsets
#define RESOURCECONTAINER_BANKS_OFFSET  0x08  // Array of banks indexed by type

// ResourceBank structure offsets
#define RESOURCEBANK_BUCKETCOUNT_OFFSET 0x08  // Hash bucket count

// Sanity bound on a bank's bucket count. Real banks are in the thousands; a
// slot reading far above this is not a bank we can hand to the game.
#define RESOURCE_MAX_BUCKETS            (1u << 22)

// ============================================================================
// Type Name Table
// ============================================================================

static const char* s_resource_type_names[RESOURCE_TYPE_COUNT] = {
    "Unknown0",
    "Visual",
    "VisualSet",
    "Animation",
    "AnimationSet",
    "Texture",
    "Material",
    "Physics",
    "Effect",
    "Script",
    "Sound",
    "Lighting",
    "Atmosphere",
    "AnimationBlueprint",
    "MeshProxy",
    "MaterialSet",
    "BlendSpace",
    "FCurve",
    "Timeline",
    "Dialog",
    "VoiceBark",
    "TileSet",
    "IKRig",
    "Skeleton",
    "VirtualTexture",
    "TerrainBrush",
    "ColorList",
    "CharacterVisual",
    "MaterialPreset",
    "SkinPreset",
    "ClothCollider",
    "DiffusionProfile",
    "LightCookie",
    "TimelineScene",
    "SkeletonMirrorTable"
};

// ============================================================================
// Module State
// ============================================================================

static struct {
    bool initialized;
    void* main_binary_base;

    // Cached pointers (read lazily)
    void** resource_manager_ptr;  // Points to global slot
} g_resource = {0};

// ============================================================================
// Initialization
// ============================================================================

bool resource_manager_init(void *main_binary_base) {
    if (g_resource.initialized) {
        return true;
    }

    if (!main_binary_base) {
        log_message("[Resource] ERROR: main_binary_base is NULL");
        return false;
    }

    g_resource.main_binary_base = main_binary_base;

    const VersionOffsets *off = offset_table_get();
    if (off && off->resource_mgr_ptr) {
        g_resource.resource_manager_ptr = (void**)offset_table_resolve(off->resource_mgr_ptr);
    } else {
        g_resource.resource_manager_ptr = NULL;
    }

    log_message("[Resource] Resource manager initialized");
    log_message("[Resource]   Base: %p", main_binary_base);
    log_message("[Resource]   ResourceManager::m_ptr at offset 0x%x -> %p",
                OFFSET_RESOURCEMANAGER_PTR, (void*)g_resource.resource_manager_ptr);

    g_resource.initialized = true;
    return true;
}

bool resource_manager_ready(void) {
    if (!g_resource.initialized || !g_resource.resource_manager_ptr) {
        return false;
    }

    // Check if the global pointer is valid
    void* mgr = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)g_resource.resource_manager_ptr, &mgr)) {
        return false;
    }

    return mgr != NULL;
}

// ============================================================================
// Type Utilities
// ============================================================================

const char* resource_type_name(ResourceBankType type) {
    if (type < 0 || type >= RESOURCE_TYPE_COUNT) {
        return NULL;
    }
    return s_resource_type_names[type];
}

// Measured corrections to s_resource_type_names' positional index.
//
// The table above mirrors the Windows ls::EResourceType ordering, but this
// macOS build does not match it everywhere. Rather than reshuffle 34 entries
// on inference, each correction here is one that was demonstrated against the
// running game: look a known resource up by UUID across every index and see
// which bank returns it.
//
// AnimationSet: two independent UUIDs resolve at index 4, not 3 - the vanilla
// HUM_M_Base (da29fce1-056a-4f86-b110-d61679c21238) that BG3AF ships as a
// default, and BG3SX's own bfa9dad2-2a5b-45cc-b770-9537badf9152. This is what
// BG3AF's AnimationSet.Get was failing on, taking BG3SX, WickedAnims and
// GrazztRing down with it.
//
// The remaining names keep their positional index because nothing has been
// measured about them either way. Add entries here as they are proven, and
// only then.
/* Empty now: the AnimationSet correction turned out to be one instance of a
 * table-wide off-by-one, which the enum itself now encodes (see the header).
 * Kept as a hook for any type that is genuinely out of sequence rather than
 * merely shifted — add an entry only once it has been measured. */
static const struct {
    const char *name;
    int index;
} s_resource_type_overrides[] = {
    { NULL, 0 }
};

int resource_type_from_name(const char* name) {
    if (!name) return -1;

    for (int i = 0; s_resource_type_overrides[i].name; i++) {
        if (strcasecmp(s_resource_type_overrides[i].name, name) == 0) {
            return s_resource_type_overrides[i].index;
        }
    }

    for (int i = 0; i < RESOURCE_TYPE_COUNT; i++) {
        if (strcasecmp(s_resource_type_names[i], name) == 0) {
            return i;
        }
    }

    return -1;
}

// ============================================================================
// Manager Access
// ============================================================================

void* resource_manager_get(void) {
    if (!g_resource.initialized || !g_resource.resource_manager_ptr) {
        return NULL;
    }

    void* mgr = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)g_resource.resource_manager_ptr, &mgr)) {
        return NULL;
    }

    return mgr;
}

void* resource_manager_get_bank(void) {
    void* mgr = resource_manager_get();
    if (!mgr) return NULL;

    void* bank = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)mgr + RESOURCEMANAGER_BANK0_OFFSET, &bank)) {
        return NULL;
    }

    return bank;
}

void* resource_manager_get_bank_secondary(void) {
    void* mgr = resource_manager_get();
    if (!mgr) return NULL;

    void* bank = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)mgr + RESOURCEMANAGER_BANK1_OFFSET, &bank)) {
        return NULL;
    }

    return bank;
}

// ============================================================================
// Resource Access
// ============================================================================

/**
 * Call ResourceContainer::GetResource function.
 * Signature: Resource* GetResource(ResourceContainer* this, uint32_t type, FixedString* id)
 */
typedef void* (*GetResourceFunc)(void* container, uint32_t type, uint32_t* fixed_string_id);

void* resource_get(ResourceBankType type, uint32_t fixed_string_id) {
    if (type < 0 || type >= RESOURCE_TYPE_COUNT) {
        return NULL;
    }

    if (!g_resource.initialized || !g_resource.main_binary_base) {
        return NULL;
    }

    void* bank = resource_manager_get_bank();
    if (!bank) {
        log_message("[Resource] ResourceBank not available");
        return NULL;
    }

    GetResourceFunc get_resource =
        (GetResourceFunc)offset_table_game_fn(GAME_FN_RESOURCE_GET);
    if (!get_resource) return NULL;

    // Validate this type's bank BEFORE handing the container to the game.
    //
    // ls::ResourceContainer::GetResource null-checks bank_array[type], but not
    // every slot that is non-null is usable here: some read as garbage, and
    // calling in with one of those faults inside the game's own hash walk.
    // A single Ext.Resource.Get with the wrong type then kills the process -
    // which is exactly what happened while probing all 34 types by hand.
    // Read the slot ourselves first; safe_memory_read_pointer cannot fault.
    void* type_bank = NULL;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)bank + RESOURCECONTAINER_BANKS_OFFSET
                + (size_t)type * sizeof(void*),
            &type_bank) || !type_bank) {
        return NULL;
    }

    // The bank must at least expose a readable, sane bucket count. A slot that
    // fails this is the one that walks away to the 100000-entry guard in
    // resource_get_count rather than terminating.
    uint32_t bucket_count = 0;
    if (!safe_memory_read_u32((mach_vm_address_t)type_bank + RESOURCEBANK_BUCKETCOUNT_OFFSET,
                              &bucket_count)
        || bucket_count == 0 || bucket_count > RESOURCE_MAX_BUCKETS) {
        LOG_CORE_DEBUG("resource_get: bank %d unusable (bucket_count=%u)",
                       (int)type, bucket_count);
        return NULL;
    }

    // Note: FixedString is passed as pointer to its hash value
    void* result = get_resource(bank, (uint32_t)type, &fixed_string_id);

    return result;
}

void* resource_get_by_name(ResourceBankType type, const char* name) {
    if (!name) return NULL;

    // Try to get FixedString ID for the name
    // fixed_string_intern looks up or creates the string in the global table
    uint32_t fs_id = fixed_string_intern(name, -1);
    if (fs_id == 0 || fs_id == 0xFFFFFFFF) {
        // String not in table - resource doesn't exist
        return NULL;
    }

    return resource_get(type, fs_id);
}

// ============================================================================
// Resource Iteration (for GetAll)
// ============================================================================

/**
 * ResourceContainer structure (from Ghidra decompilation):
 *   +0x00: vtable
 *   +0x08: bank_array[34]  - array of pointers to ResourceBank per type
 *
 * Each ResourceBank has:
 *   +0x08: bucket_count
 *   +0x10: bucket_array (hash table buckets)
 *   +0x20: SRWKernelLock
 *
 * Each bucket entry has:
 *   +0x00: next_entry
 *   +0x08: hash
 *   +0x10: resource_ptr
 */

int resource_get_count(ResourceBankType type) {
    // Resource containers use hash tables, not flat arrays
    // We need to iterate to count
    if (type < 0 || type >= RESOURCE_TYPE_COUNT) {
        return -1;
    }

    void* bank = resource_manager_get_bank();
    if (!bank) {
        return -1;
    }

    // Get the type-specific bank
    // bank_array is at bank + 0x08, indexed by type * 8
    void* type_bank = NULL;
    mach_vm_address_t type_bank_addr = (mach_vm_address_t)bank + RESOURCECONTAINER_BANKS_OFFSET + (type * sizeof(void*));
    if (!safe_memory_read_pointer(type_bank_addr, &type_bank)) {
        return -1;
    }

    if (!type_bank) {
        return 0;  // No resources of this type
    }

    // Read bucket count at +0x08
    uint32_t bucket_count = 0;
    if (!safe_memory_read_u32((mach_vm_address_t)type_bank + RESOURCEBANK_BUCKETCOUNT_OFFSET,
                              &bucket_count)) {
        return -1;
    }

    // Read bucket array at +0x10
    void* buckets = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)type_bank + 0x10, &buckets)) {
        return -1;
    }

    if (!buckets || bucket_count == 0) {
        return 0;
    }

    // Count entries by traversing all buckets
    int count = 0;
    for (uint32_t i = 0; i < bucket_count && i < 10000; i++) {
        void* entry = NULL;
        if (!safe_memory_read_pointer((mach_vm_address_t)buckets + (i * sizeof(void*)), &entry)) {
            continue;
        }

        // Traverse linked list in this bucket
        while (entry && count < 100000) {
            count++;

            // Read next pointer at +0x00
            void* next = NULL;
            if (!safe_memory_read_pointer((mach_vm_address_t)entry, &next)) {
                break;
            }
            entry = next;
        }
    }

    return count;
}

int resource_iterate_all(ResourceBankType type, ResourceIteratorCallback callback, void* user_data) {
    if (type < 0 || type >= RESOURCE_TYPE_COUNT || !callback) {
        return 0;
    }

    void* bank = resource_manager_get_bank();
    if (!bank) {
        return 0;
    }

    // Get the type-specific bank
    void* type_bank = NULL;
    mach_vm_address_t type_bank_addr = (mach_vm_address_t)bank + RESOURCECONTAINER_BANKS_OFFSET + (type * sizeof(void*));
    if (!safe_memory_read_pointer(type_bank_addr, &type_bank)) {
        return 0;
    }

    if (!type_bank) {
        return 0;
    }

    // Read bucket count at +0x08
    uint32_t bucket_count = 0;
    if (!safe_memory_read_u32((mach_vm_address_t)type_bank + 0x08, &bucket_count)) {
        return 0;
    }

    // Read bucket array at +0x10
    void* buckets = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)type_bank + 0x10, &buckets)) {
        return 0;
    }

    if (!buckets || bucket_count == 0) {
        return 0;
    }

    // Iterate all buckets
    int count = 0;
    for (uint32_t i = 0; i < bucket_count && i < 10000; i++) {
        void* entry = NULL;
        if (!safe_memory_read_pointer((mach_vm_address_t)buckets + (i * sizeof(void*)), &entry)) {
            continue;
        }

        // Traverse linked list in this bucket
        while (entry && count < 100000) {
            // Read resource pointer at +0x10
            void* resource = NULL;
            if (safe_memory_read_pointer((mach_vm_address_t)entry + 0x10, &resource) && resource) {
                count++;
                if (!callback(resource, type, user_data)) {
                    return count;  // Callback requested stop
                }
            }

            // Read next pointer at +0x00
            void* next = NULL;
            if (!safe_memory_read_pointer((mach_vm_address_t)entry, &next)) {
                break;
            }
            entry = next;
        }
    }

    return count;
}

// ============================================================================
// Debugging
// ============================================================================

void resource_dump_status(void) {
    log_message("[Resource] Resource Manager Status:");
    log_message("  Initialized: %s", g_resource.initialized ? "yes" : "no");
    log_message("  Base: %p", g_resource.main_binary_base);

    void* mgr = resource_manager_get();
    log_message("  ResourceManager: %p", mgr);

    if (mgr) {
        void* bank0 = resource_manager_get_bank();
        void* bank1 = resource_manager_get_bank_secondary();
        log_message("  ResourceBank[0] (primary): %p", bank0);
        log_message("  ResourceBank[1] (secondary): %p", bank1);

        // Show counts per type (first few)
        log_message("  Resource counts by type:");
        for (int i = 0; i < RESOURCE_TYPE_COUNT && i < 10; i++) {
            int count = resource_get_count((ResourceBankType)i);
            log_message("    %s: %d", s_resource_type_names[i], count);
        }
    }
}

// Callback for dump
static bool dump_resource_callback(void* resource, ResourceBankType type, void* user_data) {
    (void)type;
    int* count_ptr = (int*)user_data;
    int max_count = count_ptr[1];
    int current = count_ptr[0];

    if (max_count >= 0 && current >= max_count) {
        return false;  // Stop iteration
    }

    // Try to get the resource ID (usually at +0x00 or +0x08)
    uint32_t id = 0;
    if (safe_memory_read_u32((mach_vm_address_t)resource + 0x08, &id)) {
        const char* name = fixed_string_resolve(id);
        if (name) {
            log_message("    [%d] %p: %s", current, resource, name);
        } else {
            log_message("    [%d] %p: (id=0x%08x)", current, resource, id);
        }
    } else {
        log_message("    [%d] %p", current, resource);
    }

    count_ptr[0]++;
    return true;
}

void resource_dump_type(ResourceBankType type, int max_count) {
    if (type < 0 || type >= RESOURCE_TYPE_COUNT) {
        log_message("[Resource] Invalid type: %d", type);
        return;
    }

    int total = resource_get_count(type);
    log_message("[Resource] Dumping %s resources (total: %d, max: %d):",
                s_resource_type_names[type], total, max_count);

    int counts[2] = {0, max_count};
    resource_iterate_all(type, dump_resource_callback, counts);
}


// ============================================================================
// Resource presence probe (diagnostic)
// ============================================================================
//
// Mod-supplied visuals live in Public/<mod>/Content/[PAK]_<name>/<guid>.lsf and
// must reach the Visual/Material banks for an item to render its own mesh. When
// they do not, the item falls back to the vanilla template it declares with
// `using`, which is why a modded outfit renders as an entirely different,
// vanilla one. Whether they loaded is a fact we can read rather than infer.

#include <stdlib.h>
#include <stdio.h>
#include "../template/template_manager.h"

void resource_probe_tick(void) {
    static int s_state = 0;        /* 0 = unstarted, 1 = probing, 2 = done */
    static int s_attempts = 0;
    static const char *s_list = NULL;

    if (s_state == 2) return;
    if (s_state == 0) {
        s_list = getenv("BG3SE_RESOURCE_PROBE");
        if (!s_list || !*s_list) { s_state = 2; return; }
        s_state = 1;
    }
    if (!resource_manager_ready()) return;

    /* Banks fill as the session loads; retry for a while before reporting. */
    int any = 0;
    for (int t = 0; t < RESOURCE_TYPE_COUNT; t++) {
        if (resource_get_count((ResourceBankType)t) > 0) { any = 1; break; }
    }
    if (!any && ++s_attempts < 600) return;

    log_message("[Resource] probe: scanning all %d bank indices "
                "(after %d tick(s))", RESOURCE_TYPE_COUNT, s_attempts);
    if (template_manager_ready()) {
        for (int m = 0; m < TEMPLATE_MANAGER_COUNT; m++) {
            log_message("[Resource]   templates[%d] count=%d", m,
                        template_get_count((TemplateManagerType)m));
        }
    } else {
        log_message("[Resource]   template manager NOT READY");
    }
    for (int t = 0; t < RESOURCE_TYPE_COUNT; t++) {
        int c = resource_get_count((ResourceBankType)t);
        if (c > 0) {
            log_message("[Resource]   bank[%2d] %-24s count=%d", t,
                        s_resource_type_names[t] ? s_resource_type_names[t] : "?",
                        c);
        }
    }

    char buf[1024];
    size_t n = strlen(s_list);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, s_list, n);
    buf[n] = '\0';

    /* Ask EVERY bank, not the two we assume. This file already records that
     * the positional index does not match this build everywhere, and that the
     * only reliable way to place a resource is to look it up by UUID across
     * all indices and see which one answers -- exactly what AnimationSet's
     * correction was derived from. Assuming Visual=0 / Material=5 produced an
     * inverted, meaningless first result. */
    for (char *tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) {
        char hits[256];
        size_t o = 0;
        hits[0] = '\0';
        for (int t = 0; t < RESOURCE_TYPE_COUNT && o < sizeof(hits) - 32; t++) {
            if (resource_get_by_name((ResourceBankType)t, tok)) {
                o += (size_t)snprintf(hits + o, sizeof(hits) - o, "%s%d(%s)",
                                      o ? "," : "", t,
                                      s_resource_type_names[t] ?
                                          s_resource_type_names[t] : "?");
            }
        }
        /* Also ask the template manager. RootTemplates are not resources and
         * will never appear in a bank, so "missing from every bank" says
         * nothing about them -- but whether an item's RootTemplate resolved is
         * exactly what decides if it renders its own mesh or falls back to the
         * vanilla parent it declares with `using`. */
        const char *tmpl = "n/a (template manager not ready)";
        static const char *mgr_names[TEMPLATE_MANAGER_COUNT] = {
            "GlobalBank", "Local", "Cache", "LocalCache"
        };
        char tbuf[128];
        if (template_manager_ready()) {
            tmpl = "NOT FOUND IN ANY TEMPLATE MANAGER";
            for (int m = 0; m < TEMPLATE_MANAGER_COUNT; m++) {
                if (template_get_by_guid((TemplateManagerType)m, tok)) {
                    snprintf(tbuf, sizeof(tbuf), "FOUND in %s", mgr_names[m]);
                    tmpl = tbuf;
                    break;
                }
            }
        }
        log_message("[Resource] probe %s -> banks: %s | template: %s", tok,
                    hits[0] ? hits : "none", tmpl);
    }
    s_state = 2;
}
