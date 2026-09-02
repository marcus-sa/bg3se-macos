/**
 * resource_manager.h - Resource Manager for BG3SE-macOS
 *
 * Provides access to the game's ResourceManager for resource lookup.
 * Resources are game assets indexed by FixedString (not GUID like StaticData).
 *
 * Architecture:
 *   - ls::ResourceManager::m_ptr is a global singleton at 0x108a8f070
 *   - ResourceBank at +0x28 (primary) and +0x30 (secondary)
 *   - ResourceContainer::GetResource at 0x1060cc608
 *
 * Discovery (Dec 21, 2025):
 *   - Global pointer found via ADRP+LDR in InitEngine
 *   - 34 ResourceBankType values (Visual, Animation, Sound, etc.)
 */

#ifndef RESOURCE_MANAGER_H
#define RESOURCE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// ResourceBankType Enumeration (34 types)
// ============================================================================

typedef enum {
    RESOURCE_VISUAL = 0,
    RESOURCE_VISUAL_SET = 1,
    RESOURCE_ANIMATION = 2,
    RESOURCE_ANIMATION_SET = 3,
    RESOURCE_TEXTURE = 4,
    RESOURCE_MATERIAL = 5,
    RESOURCE_PHYSICS = 6,
    RESOURCE_EFFECT = 7,
    RESOURCE_SCRIPT = 8,
    RESOURCE_SOUND = 9,
    RESOURCE_LIGHTING = 10,
    RESOURCE_ATMOSPHERE = 11,
    RESOURCE_ANIMATION_BLUEPRINT = 12,
    RESOURCE_MESH_PROXY = 13,
    RESOURCE_MATERIAL_SET = 14,
    RESOURCE_BLEND_SPACE = 15,
    RESOURCE_FCURVE = 16,
    RESOURCE_TIMELINE = 17,
    RESOURCE_DIALOG = 18,
    RESOURCE_VOICE_BARK = 19,
    RESOURCE_TILE_SET = 20,
    RESOURCE_IK_RIG = 21,
    RESOURCE_SKELETON = 22,
    RESOURCE_VIRTUAL_TEXTURE = 23,
    RESOURCE_TERRAIN_BRUSH = 24,
    RESOURCE_COLOR_LIST = 25,
    RESOURCE_CHARACTER_VISUAL = 26,
    RESOURCE_MATERIAL_PRESET = 27,
    RESOURCE_SKIN_PRESET = 28,
    RESOURCE_CLOTH_COLLIDER = 29,
    RESOURCE_DIFFUSION_PROFILE = 30,
    RESOURCE_LIGHT_COOKIE = 31,
    RESOURCE_TIMELINE_SCENE = 32,
    RESOURCE_SKELETON_MIRROR_TABLE = 33,
    RESOURCE_TYPE_COUNT = 34
} ResourceBankType;

// ============================================================================
// Initialization
// ============================================================================

/**
 * Initialize the resource manager.
 * Must be called after the main binary base is known.
 *
 * @param main_binary_base Base address of the main game binary
 * @return true if initialization successful
 */
bool resource_manager_init(void *main_binary_base);

/**
 * Check if the resource manager is ready.
 *
 * @return true if ResourceManager singleton is available
 */
bool resource_manager_ready(void);

// ============================================================================
// Type Utilities
// ============================================================================

/**
 * Get the name of a resource type.
 *
 * @param type Resource type enum
 * @return Type name string (e.g., "Visual", "Sound")
 */
const char* resource_type_name(ResourceBankType type);

/**
 * Parse a type name to enum value.
 *
 * @param name Type name string (case-insensitive)
 * @return ResourceBankType enum, or -1 if not found
 */
int resource_type_from_name(const char* name);

// ============================================================================
// Manager Access
// ============================================================================

/**
 * Get the ResourceManager singleton pointer.
 *
 * @return ResourceManager pointer, or NULL if not available
 */
void* resource_manager_get(void);

/**
 * Get the primary ResourceBank (index 0, at +0x28).
 *
 * @return ResourceBank pointer, or NULL if not available
 */
void* resource_manager_get_bank(void);

/**
 * Get the secondary ResourceBank (index 1, at +0x30).
 *
 * @return ResourceBank pointer, or NULL if not available
 */
void* resource_manager_get_bank_secondary(void);

// ============================================================================
// Resource Access
// ============================================================================

/**
 * Get a resource by type and FixedString ID.
 *
 * @param type Resource type
 * @param fixed_string_id FixedString hash/index
 * @return Resource pointer, or NULL if not found
 */
void* resource_get(ResourceBankType type, uint32_t fixed_string_id);

/**
 * Get a resource by type and string name.
 * Resolves the string to a FixedString ID first.
 *
 * @param type Resource type
 * @param name Resource name string
 * @return Resource pointer, or NULL if not found
 */
void* resource_get_by_name(ResourceBankType type, const char* name);

/**
 * Get the count of resources for a type.
 * Note: May require iteration as ResourceContainer uses hash tables.
 *
 * @param type Resource type
 * @return Resource count, or -1 on error
 */
int resource_get_count(ResourceBankType type);

/**
 * Resource iterator callback type.
 *
 * @param resource Resource pointer
 * @param type Resource type
 * @param user_data User-provided context
 * @return true to continue iteration, false to stop
 */
typedef bool (*ResourceIteratorCallback)(void* resource, ResourceBankType type, void* user_data);

/**
 * Iterate all resources of a type.
 *
 * @param type Resource type
 * @param callback Callback function for each resource
 * @param user_data User context passed to callback
 * @return Number of resources iterated
 */
int resource_iterate_all(ResourceBankType type, ResourceIteratorCallback callback, void* user_data);

// ============================================================================
// Debugging
// ============================================================================

/**
 * Dump resource manager status to log.
 */
/* Diagnostic: with BG3SE_RESOURCE_PROBE set to a comma-separated list of
 * resource GUIDs, report once whether each is present in the Visual and
 * Material banks, plus each bank's total. Answers "did this mod's visual
 * resources actually load?" without guessing. Call once per tick; it retries
 * until the banks are populated, then reports and stops. */
void resource_probe_tick(void);

void resource_dump_status(void);

/**
 * Dump resources of a type to log.
 *
 * @param type Resource type
 * @param max_count Maximum resources to dump (-1 for all)
 */
void resource_dump_type(ResourceBankType type, int max_count);

#endif // RESOURCE_MANAGER_H
