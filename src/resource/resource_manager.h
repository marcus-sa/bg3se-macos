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

/* Bank indices are MEASURED against this build, not inherited from Windows.
 *
 * Every type is one higher than the Windows ls::EResourceType ordering. Four
 * independent measurements agree, taken by looking known resources up by UUID
 * across all 34 indices and seeing which bank answers:
 *
 *   AnimationSet  3 -> 4   (two vanilla UUIDs; the original lone correction)
 *   Visual        0 -> 1   (VisualBank .lsf from a mod's Content/[PAK]_* dir)
 *   Texture       4 -> 5   (TextureBank .lsf, same source)
 *   Material      5 -> 6   (MaterialBank .lsf, 8 of 8 landed here)
 *
 * The AnimationSet override was never a special case — it was one symptom of a
 * table-wide off-by-one, which is why every other type read the neighbouring
 * bank and Ext.Resource.Get returned nil (or the wrong kind) for real UUIDs.
 * Index 0 is whatever this build places ahead of Visual; it is populated, so it
 * gets a name rather than being folded away. */
typedef enum {
    RESOURCE_UNKNOWN_0 = 0,
    RESOURCE_VISUAL = 1,
    RESOURCE_VISUAL_SET = 2,
    RESOURCE_ANIMATION = 3,
    RESOURCE_ANIMATION_SET = 4,
    RESOURCE_TEXTURE = 5,
    RESOURCE_MATERIAL = 6,
    RESOURCE_PHYSICS = 7,
    RESOURCE_EFFECT = 8,
    RESOURCE_SCRIPT = 9,
    RESOURCE_SOUND = 10,
    RESOURCE_LIGHTING = 11,
    RESOURCE_ATMOSPHERE = 12,
    RESOURCE_ANIMATION_BLUEPRINT = 13,
    RESOURCE_MESH_PROXY = 14,
    RESOURCE_MATERIAL_SET = 15,
    RESOURCE_BLEND_SPACE = 16,
    RESOURCE_FCURVE = 17,
    RESOURCE_TIMELINE = 18,
    RESOURCE_DIALOG = 19,
    RESOURCE_VOICE_BARK = 20,
    RESOURCE_TILE_SET = 21,
    RESOURCE_IK_RIG = 22,
    RESOURCE_SKELETON = 23,
    RESOURCE_VIRTUAL_TEXTURE = 24,
    RESOURCE_TERRAIN_BRUSH = 25,
    RESOURCE_COLOR_LIST = 26,
    RESOURCE_CHARACTER_VISUAL = 27,
    RESOURCE_MATERIAL_PRESET = 28,
    RESOURCE_SKIN_PRESET = 29,
    RESOURCE_CLOTH_COLLIDER = 30,
    RESOURCE_DIFFUSION_PROFILE = 31,
    RESOURCE_LIGHT_COOKIE = 32,
    RESOURCE_TIMELINE_SCENE = 33,
    RESOURCE_SKELETON_MIRROR_TABLE = 34,
    RESOURCE_TYPE_COUNT = 35
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
void resource_dump_status(void);

/**
 * Dump resources of a type to log.
 *
 * @param type Resource type
 * @param max_count Maximum resources to dump (-1 for all)
 */
void resource_dump_type(ResourceBankType type, int max_count);

#endif // RESOURCE_MANAGER_H
