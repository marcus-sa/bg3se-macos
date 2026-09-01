/**
 * functor_types.h - Stats Functor System Types
 *
 * Data structures for the game's functor execution system.
 * Used for hooking damage, healing, status effects, and combat mechanics.
 *
 * References:
 * - Windows BG3SE: BG3Extender/GameDefinitions/Stats/Functors.h
 * - Windows BG3SE: BG3Extender/GameDefinitions/Hit.h
 * - Ghidra offsets: ghidra/offsets/FUNCTORS.md
 */

#ifndef FUNCTOR_TYPES_H
#define FUNCTOR_TYPES_H

#include <stdint.h>
#include <stdbool.h>

// Forward declarations
typedef struct EntityRef EntityRef;
typedef struct HitDesc HitDesc;
typedef struct AttackDesc AttackDesc;
typedef struct HitResult HitResult;

// =============================================================================
// Functor Context Type Enum
// =============================================================================

typedef enum {
    FUNCTOR_CTX_ATTACK_TARGET = 0,
    FUNCTOR_CTX_ATTACK_POSITION = 1,
    FUNCTOR_CTX_MOVE = 2,
    FUNCTOR_CTX_TARGET = 3,
    FUNCTOR_CTX_NEARBY_ATTACKED = 4,
    FUNCTOR_CTX_NEARBY_ATTACKING = 5,
    FUNCTOR_CTX_EQUIP = 6,
    FUNCTOR_CTX_SOURCE = 7,
    FUNCTOR_CTX_INTERRUPT = 8,
} FunctorContextType;

// =============================================================================
// Functor Type IDs (partial list of ~50 types)
// =============================================================================

typedef enum {
    FUNCTOR_ID_CUSTOM_DESCRIPTION = 0,
    FUNCTOR_ID_APPLY_STATUS = 1,
    FUNCTOR_ID_SURFACE_CHANGE = 2,
    FUNCTOR_ID_RESURRECT = 3,
    FUNCTOR_ID_SABOTAGE = 4,
    FUNCTOR_ID_SUMMON = 5,
    FUNCTOR_ID_FORCE = 6,
    FUNCTOR_ID_DOUSE = 7,
    FUNCTOR_ID_SWAP_PLACES = 8,
    FUNCTOR_ID_PICKUP = 9,
    FUNCTOR_ID_CREATE_SURFACE = 10,
    FUNCTOR_ID_CREATE_CONE_SURFACE = 11,
    FUNCTOR_ID_REMOVE_STATUS = 12,
    FUNCTOR_ID_DEAL_DAMAGE = 13,
    FUNCTOR_ID_EXECUTE_WEAPON_FUNCTORS = 14,
    FUNCTOR_ID_REGAIN_HIT_POINTS = 15,
    FUNCTOR_ID_TELEPORT_SOURCE = 16,
    FUNCTOR_ID_SET_STATUS_DURATION = 17,
    FUNCTOR_ID_USE_SPELL = 18,
    FUNCTOR_ID_USE_ACTION_RESOURCE = 19,
    FUNCTOR_ID_USE_ATTACK = 20,
    FUNCTOR_ID_CREATE_EXPLOSION = 21,
    FUNCTOR_ID_BREAK_CONCENTRATION = 22,
    FUNCTOR_ID_APPLY_EQUIPMENT_STATUS = 23,
    FUNCTOR_ID_RESTORE_RESOURCE = 24,
    FUNCTOR_ID_SPAWN = 25,
    FUNCTOR_ID_STABILIZE = 26,
    FUNCTOR_ID_UNLOCK = 27,
    FUNCTOR_ID_RESET_COMBAT_TURN = 28,
    FUNCTOR_ID_REMOVE_AURA_BY_CHILD_STATUS = 29,
    FUNCTOR_ID_SUMMON_IN_INVENTORY = 30,
    FUNCTOR_ID_SPAWN_IN_INVENTORY = 31,
    FUNCTOR_ID_REMOVE_UNIQUE_STATUS = 32,
    FUNCTOR_ID_DISARM_WEAPON = 33,
    FUNCTOR_ID_DISARM_AND_STEAL_WEAPON = 34,
    FUNCTOR_ID_SWITCH_DEATH_TYPE = 35,
    FUNCTOR_ID_TRIGGER_RANDOM_CAST = 36,
    FUNCTOR_ID_GAIN_TEMPORARY_HIT_POINTS = 37,
    FUNCTOR_ID_FIRE_PROJECTILE = 38,
    FUNCTOR_ID_SHORT_REST = 39,
    FUNCTOR_ID_CREATE_ZONE = 40,
    FUNCTOR_ID_DO_TELEPORT = 41,
    FUNCTOR_ID_REGAIN_TEMPORARY_HIT_POINTS = 42,
    FUNCTOR_ID_REMOVE_STATUS_BY_LEVEL = 43,
    FUNCTOR_ID_SURFACE_CLEAR_LAYER = 44,
    FUNCTOR_ID_UNSUMMON = 45,
    FUNCTOR_ID_CREATE_WALL = 46,
    FUNCTOR_ID_COUNTERSPELL = 47,
    FUNCTOR_ID_ADJUST_ROLL = 48,
    FUNCTOR_ID_SPAWN_EXTRA_PROJECTILES = 49,
    FUNCTOR_ID_KILL = 50,
    FUNCTOR_ID_TUTORIAL_EVENT = 51,
    FUNCTOR_ID_DROP = 52,
    FUNCTOR_ID_RESET_COOLDOWNS = 53,
    FUNCTOR_ID_SET_ROLL = 54,
    FUNCTOR_ID_SET_DAMAGE_RESISTANCE = 55,
    FUNCTOR_ID_SET_REROLL = 56,
    FUNCTOR_ID_SET_ADVANTAGE = 57,
    FUNCTOR_ID_SET_DISADVANTAGE = 58,
    FUNCTOR_ID_MAXIMIZE_ROLL = 59,
    FUNCTOR_ID_CAMERA_WAIT = 60,
    FUNCTOR_ID_EXTENDER = 61,
} FunctorId;

// =============================================================================
// Entity Reference (16 bytes on ARM64)
// =============================================================================

struct EntityRef {
    uint64_t Handle;      // 0x00: EntityHandle (salt + index)
    void*    World;       // 0x08: EntityWorld pointer
};

// =============================================================================
// ActionOriginator - Tracks origin of an action
// Estimated ~64 bytes based on Windows reference
// =============================================================================

typedef struct {
    uint64_t ActionGuid[2];        // 0x00: 16-byte GUID
    uint64_t InterruptGuid[2];     // 0x10: 16-byte GUID
    uint64_t PassiveGuid[2];       // 0x20: 16-byte GUID
    uint32_t CanApplyConcentration;// 0x30
    uint8_t  _pad[28];             // 0x34: Alignment padding
} ActionOriginator;                // Total: ~0x50 (80 bytes)

// =============================================================================
// ContextData - Base class for all functor contexts
// =============================================================================

typedef struct {
    void*              vtable;           // 0x00: Virtual table pointer
    FunctorContextType Type;             // 0x08: Context type enum
    int32_t            StoryActionId;    // 0x0C
    uint32_t           PropertyContext;  // 0x10
    uint32_t           _pad0;            // 0x14: Alignment
    ActionOriginator   Originator;       // 0x18: ~80 bytes
    void*              ClassResources;   // 0x68: GuidResourceBankBase*
    uint64_t           HistoryEntity;    // 0x70: EntityHandle
    uint64_t           StatusSource;     // 0x78: EntityHandle
    void*              EntityToThoth;    // 0x80: HashMap pointer
    uint64_t           _pad1;            // 0x88
    int32_t            field_90;         // 0x90
    uint8_t            ConditionCategory;// 0x94
    uint8_t            _pad2[3];         // 0x95-0x97
} ContextData;                           // Total: ~0x98 (152 bytes)

// =============================================================================
// AttackTargetContextData - Most common context for attacks
// =============================================================================

typedef struct {
    ContextData base;                    // 0x00: Base fields (~0x98)

    EntityRef   Caster;                  // 0x98: 16 bytes
    EntityRef   CasterProxy;             // 0xA8: 16 bytes
    EntityRef   Target;                  // 0xB8: 16 bytes
    EntityRef   TargetProxy;             // 0xC8: 16 bytes
    float       Position[3];             // 0xD8: 12 bytes
    bool        IsFromItem;              // 0xE4: 1 byte
    uint8_t     _pad0[3];                // 0xE5-0xE7

    // SpellIdWithPrototype SpellId;     // 0xE8+: Complex, ~32 bytes
    // HitDesc Hit;                      // ~0x1B8 bytes
    // AttackDesc Attack;                // ~0x28 bytes
    // Additional fields...

    uint8_t     _reserved[0x230];        // Placeholder for remaining fields
} AttackTargetContextData;               // Total: ~0x318 (792 bytes)

// =============================================================================
// AttackPositionContextData - For area attacks targeting a position
// =============================================================================

typedef struct {
    ContextData base;                    // 0x00: Base fields

    EntityRef   Caster;                  // 0x98: 16 bytes
    float       Position[3];             // 0xA8: 12 bytes
    float       HitRadius;               // 0xB4: -1.0 default
    uint8_t     _reserved[0x200];        // Placeholder
} AttackPositionContextData;

// =============================================================================
// MoveContextData - For movement/teleport functors
// =============================================================================

typedef struct {
    ContextData base;                    // 0x00: Base fields

    EntityRef   Caster;                  // 0x98
    EntityRef   Target;                  // 0xA8
    EntityRef   Source;                  // 0xB8
    float       Position[3];             // 0xC8: 12 bytes
    float       Distance;                // 0xD4: 0.0 default
    uint8_t     _reserved[0x100];        // Placeholder
} MoveContextData;

// =============================================================================
// TargetContextData - Generic target context
// =============================================================================

typedef struct {
    ContextData base;                    // 0x00: Base fields

    EntityRef   Source;                  // 0x98
    EntityRef   SourceProxy;             // 0xA8
    float       Position[3];             // 0xB8: 12 bytes
    uint8_t     StatusExitCause;         // 0xC4: default 3
    uint8_t     field_C5;                // 0xC5
    uint8_t     field_C6;                // 0xC6: default 19
    uint8_t     _pad0;                   // 0xC7
    uint8_t     _reserved[0x200];        // Placeholder
} TargetContextData;

// =============================================================================
// EquipContextData - For equipment-related functors
// =============================================================================

typedef struct {
    ContextData base;                    // 0x00: Base fields

    EntityRef   Caster;                  // 0x98
    EntityRef   Target;                  // 0xA8
    bool        UseCasterStats;          // 0xB8: default false
    uint8_t     _reserved[0x100];        // Placeholder
} EquipContextData;

// =============================================================================
// SourceContextData - Minimal context with just source
// =============================================================================

typedef struct {
    ContextData base;                    // 0x00: Base fields

    EntityRef   Source;                  // 0x98
    EntityRef   SourceProxy;             // 0xA8
    uint8_t     _reserved[0x100];        // Placeholder
} SourceContextData;

// =============================================================================
// NearbyAttackedContextData - For reaction triggers
// =============================================================================

typedef struct {
    ContextData base;                    // 0x00: Base fields

    EntityRef   OriginalSource;          // 0x98
    EntityRef   Source;                  // 0xA8
    EntityRef   SourceProxy;             // 0xB8
    EntityRef   Target;                  // 0xC8
    EntityRef   TargetProxy;             // 0xD8
    float       Position[3];             // 0xE8: 12 bytes
    bool        IsFromItem;              // 0xF4
    uint8_t     _reserved[0x220];        // Placeholder
} NearbyAttackedContextData;

// NearbyAttackingContextData inherits from NearbyAttackedContextData
typedef NearbyAttackedContextData NearbyAttackingContextData;

// =============================================================================
// InterruptContextData - For interrupt system
// =============================================================================

typedef struct {
    ContextData base;                    // 0x00: Base fields

    bool        OnlyAllowRollAdjustments;// 0x98
    uint8_t     _pad0[7];                // 0x99-0x9F
    EntityRef   Source;                  // 0xA0
    EntityRef   SourceProxy;             // 0xB0
    EntityRef   Target;                  // 0xC0
    EntityRef   TargetProxy;             // 0xD0
    EntityRef   Observer;                // 0xE0
    EntityRef   ObserverProxy;           // 0xF0
    uint8_t     _reserved[0x340];        // Placeholder for RollAdjustments, Interrupt, Hit, Attack
} InterruptContextData;

// =============================================================================
// AttackDesc - Attack result summary
// =============================================================================

struct AttackDesc {
    int32_t     TotalDamageDone;         // 0x00
    int32_t     TotalHealDone;           // 0x04
    uint8_t     InitialHPPercentage;     // 0x08
    uint8_t     field_9;                 // 0x09
    uint8_t     _pad[6];                 // 0x0A-0x0F
    void*       DamageList;              // 0x10: Array<DamagePair>
    uint64_t    DamageListSize;          // 0x18
};                                       // Total: ~0x20 (32 bytes)

// =============================================================================
// HitDesc - Detailed hit information
// =============================================================================

struct HitDesc {
    int32_t     TotalDamageDone;         // 0x00
    uint8_t     DeathType;               // 0x04: DeathType enum
    uint8_t     DamageType;              // 0x05: DamageType enum
    uint8_t     CauseType;               // 0x06: CauseType enum
    uint8_t     _pad0;                   // 0x07
    float       ImpactPosition[3];       // 0x08: 12 bytes
    float       ImpactDirection[3];      // 0x14: 12 bytes
    float       ImpactForce;             // 0x20
    int32_t     ArmorAbsorption;         // 0x24
    int32_t     LifeSteal;               // 0x28
    uint32_t    EffectFlags;             // 0x2C: DamageFlags
    uint64_t    Inflicter;               // 0x30: EntityHandle
    uint64_t    InflicterOwner;          // 0x38: EntityHandle
    uint64_t    Throwing;                // 0x40: EntityHandle
    int32_t     StoryActionId;           // 0x48
    uint8_t     HitWith;                 // 0x4C: HitWith enum
    uint8_t     AttackRollAbility;       // 0x4D: AbilityId
    uint8_t     SaveAbility;             // 0x4E: AbilityId
    uint8_t     SpellAttackType;         // 0x4F
    // ... many more fields (~0x1B8 total)
    uint8_t     _reserved[0x160];        // Placeholder for remaining fields
};                                       // Total: ~0x1B0 (432 bytes)

// =============================================================================
// HitResult - Complete hit result with damage info
// =============================================================================

struct HitResult {
    HitDesc     Hit;                     // 0x00: ~0x1B0 bytes
    AttackDesc  Attack;                  // 0x1B0: ~0x20 bytes
    void*       Results;                 // 0x1D0: HitResultData*
    uint32_t    NumConditionRolls;       // 0x1D8
    uint8_t     _pad[4];                 // 0x1DC-0x1DF
};                                       // Total: ~0x1E0 (480 bytes)

// =============================================================================
// StatsFunctorBase - Base class for all functor types
// =============================================================================

typedef struct {
    void*       vtable;                  // 0x00: Virtual table
    uint32_t    UniqueName;              // 0x08: FixedString index
    uint32_t    _pad0;                   // 0x0C
    uint64_t    FunctorUuid[2];          // 0x10: 16-byte GUID
    void*       RollConditions;          // 0x20: Array<ExportedConditionalRoll>
    uint64_t    StatsConditions;         // 0x28: ConditionId
    uint32_t    PropertyContext;         // 0x30
    int32_t     StoryActionId;           // 0x34
    uint8_t     ObserverType;            // 0x38
    FunctorId   TypeId;                  // 0x3C: FunctorId enum
    uint32_t    Flags;                   // 0x40: FunctorFlags
    uint8_t     _reserved[0x40];         // Padding for derived types
} StatsFunctorBase;

// =============================================================================
// StatsFunctorList - Container for functor chain
// =============================================================================

typedef struct {
    void*       vtable;                  // 0x00
    void*       Elements;                // 0x08: Array<Functor*>
    uint64_t    Count;                   // 0x10
    uint32_t    UniqueName;              // 0x18: FixedString index
    uint8_t     _reserved[0x20];         // Padding
} StatsFunctorList;

// =============================================================================
// Function Addresses (ARM64 macOS)
// =============================================================================

// The game build the hook addresses and their wrapper ABIs were verified
// against. The functions are nm-visible LOCAL symbols (plain `nm`, without
// `-g`), but symbol resolution only proves the address, not the ABI. main.c
// therefore gates hook installation on an exact match against this constant,
// independent of BG3_KNOWN_VERSION. Unknown builds remain fail-closed.
// 7398727: Wave 2C re-verified the nine ExecuteStatsFunctors entry windows
// byte-identical and their wrapper register contracts unchanged
// (ghidra/offsets/ABI_REVIEW_7398727.md §1); the two DealDamage targets added
// 2026-09-02 were derived and ABI-verified directly on this build
// (ghidra/offsets/DEALDAMAGE_HOOKS.md). Eleven installed hooks in total.
// Addresses come from the offset table row.
#define FUNCTOR_ADDRS_VERIFIED_BUILD "4.1.1.7398727"

// HISTORICAL (7209685 VAs) — no longer consumed by any hook installer;
// runtime resolution goes through offset_table_game_fn(). Kept as the
// documented anchor set for the migration manifest. Do not reintroduce
// direct uses; the values below are stale on 7398727.
// Main dispatcher
#define ADDR_EXECUTE_STATS_FUNCTOR             0x10577399c

// Context-specific handlers
#define ADDR_EXECUTE_FUNCTORS_ATTACK_TARGET    0x10577787c
#define ADDR_EXECUTE_FUNCTORS_ATTACK_POSITION  0x105777bd0
#define ADDR_EXECUTE_FUNCTORS_MOVE             0x1057796c0
#define ADDR_EXECUTE_FUNCTORS_TARGET           0x10577a87c
#define ADDR_EXECUTE_FUNCTORS_NEARBY_ATTACKED  0x10577e43c
#define ADDR_EXECUTE_FUNCTORS_NEARBY_ATTACKING 0x10577fb0c
#define ADDR_EXECUTE_FUNCTORS_EQUIP            0x10578098c
#define ADDR_EXECUTE_FUNCTORS_SOURCE           0x1057829f4
#define ADDR_EXECUTE_FUNCTORS_INTERRUPT        0x105786548

// Damage processing
#define ADDR_PROCESS_DEAL_DAMAGE_FUNCTORS      0x10537e8b4

// esv::functor::Result::~Result — local symbol, so dlsym cannot reach it.
// Needed by Ext.Stats.ExecuteFunctors: we allocate the hidden result_out
// ourselves (upstream Hit.h names its type: HitResult = HitDesc + AttackDesc +
// HashMap<FunctorId,int32> + u32 ≈ 0x218 on this layout; a zero-filled buffer
// is a validly default-constructed value since every field default is zero),
// and the functors allocate into its HashMap/Arrays, so the ENGINE dtor must
// run afterwards or every call leaks. Its own body confirms the shape: it
// frees a heap pointer read from result+0x1F8.
#define ADDR_FUNCTOR_RESULT_DTOR               0x1010c0b08
// Upper bound for the stack allocation; must exceed sizeof(HitResult).
#define FUNCTOR_RESULT_BUFSZ                   0x400

// =============================================================================
// Function Type Definitions
// =============================================================================

typedef struct EntityWorld EntityWorld;

// The eight non-Interrupt overloads are free functions on 7209685 that
// RETURN esv::functor::Result. Demangled nm names omit return types, and at
// this local boundary the result is materialized as a hidden leading output
// argument in x0 (call sites do `add x0, sp, #imm` and later run
// Result::~Result on it). Machine ABI: (result_out, functors, context) in
// x0..x2. Dropping result_out shifts every register and crashes in the
// original body (2026-07-29 SIGSEGV, docs/bugs/wave2-functor-crash-analysis.md).
typedef void (*ExecuteFunctorsProc)(
    void*                   result_out,
    const StatsFunctorList* functors,
    void*                   context
);

// Interrupt has the same hidden result_out, then EntityWorld& leading:
// machine ABI (result_out, EntityWorld&, StatsFunctorList const*,
// InterruptContextData&) in x0..x3.
typedef void (*ExecuteInterruptFunctorsProc)(
    void*                   result_out,
    EntityWorld*            entityWorld,
    const StatsFunctorList* functors,
    InterruptContextData*   context
);

// Main dispatcher: the third C argument is an AttackTargetContextData&.
// WARNING (Wave 2C): this typedef omits the dispatcher's hidden x0 result
// pointer (callers do `add x0, sp, #imm` before the call, shifting the real
// args to x1..x3). It is currently UNUSED — no hook installs through it. Do
// not hook the dispatcher through this typedef without adding result_out
// first (ghidra/offsets/ABI_REVIEW_7398727.md §1).
typedef void (*ExecuteStatsFunctorProc)(
    const StatsFunctorBase*  functor,
    uint64_t                 functorId,
    AttackTargetContextData* context
);

// =============================================================================
// DealDamage / DealtDamage / BeforeDealDamage hook targets
// =============================================================================
//
// esv::functor::StatsFunctorDealDamage::Execute (the ecs::EntityRef target
// overload) is the macOS counterpart of the function Windows BG3SE hooks as
// stats::DealDamageFunctor::ApplyDamage. Identified on 4.1.1.7398727 by a
// one-for-one match of its seventeen explicit parameters against Windows'
// DealDamageFunctor__ApplyDamageProc, in order; the two parameters Windows
// could only type as `__int64 a17` and `bool` demangle here as
// `ls::ID<ecs::EntityHandleTraits> const&` and `eoc::EHitWith`.
// Derivation and instruction citations: ghidra/offsets/DEALDAMAGE_HOOKS.md.
//
// Machine ABI, verified against this build's callee prologue (0x105773558) and
// a complete call site (0x1049e5890):
//
//   x0            hidden HitResult output object   <-- x0 on this build, NOT x8
//   x1..x7        parameters 1..7
//   [x29+0x10..]  parameters 8..17
//
// Two separate ABI hazards live in that list, both of which silently corrupt
// every argument behind them rather than failing:
//
//  1. result_out. Same hidden leading output object as the nine
//     ExecuteStatsFunctors wrappers: it is invisible in the demangled name
//     because C++ does not mangle return types. Dropping it shifts x1..x7 down
//     one register and the original body runs on garbage — the exact 2026-07-29
//     SIGSEGV in docs/bugs/wave2-functor-crash-analysis.md. Accept it first,
//     forward it unchanged, never dereference it, and return what the original
//     returned (AAPCS leaves the result address in x0 on return).
//
//  2. The small stack-passed types. Apple's arm64 ABI packs stack arguments to
//     their natural size instead of giving each an 8-byte slot, so `hitWith`
//     really is one byte at +0x28 (the call site emits `strb w9,[sp,#0x28]`),
//     `conditionRollIndex` sits at +0x2c and `entityDamagedEvent` at +0x30.
//     Widening hitWith to int would push conditionRollIndex to +0x30 and drag
//     the last four arguments — including the two pointers — out of place.
//     DealDamageStackArgsLayout below pins these offsets; tier0 asserts them.
typedef void *(*DealDamageApplyDamageProc)(
    void*        result_out,
    const void*  functor,           // eoc::StatsFunctorDealDamage const&
    const void*  casterRef,         // ecs::EntityRef const&
    const void*  targetRef,         // ecs::EntityRef const&
    const void*  position,          // Vector3f const&
    bool         isFromItem,
    const void*  spellId,           // eoc::spell::SpellInfo const&
    uint32_t     storyActionId,     // ls::TypeWrap<unsigned int, ...> by value
    const void*  originator,        // eoc::ActionOriginator const&
    const void*  classDescriptions, // eoc::ClassDescriptions const&
    const void*  hit,               // eoc::HitDesc const&
    const void*  attack,            // eoc::AttackDesc const&
    const void*  sourceHandle2,     // ls::ID<ecs::EntityHandleTraits> const&
    uint8_t      hitWith,           // eoc::EHitWith  (ONE byte, see hazard 2)
    int32_t      conditionRollIndex,
    bool         entityDamagedEvent,
    const void*  sourceHandle3,     // ls::ID<ecs::EntityHandleTraits> const&
    const void*  spellId2           // eoc::spell::SpellId const&
);

// Mirror of the stack-passed tail (parameters 8..17) of
// DealDamageApplyDamageProc. C struct layout on Apple arm64 reproduces the
// ABI's stack packing for this sequence, so offsetof() here equals the byte
// offset of each argument from the first stack slot. tier0 pins every offset
// to the value read off the 7398727 call site, so a future edit to the proc
// typedef that changes a parameter's width fails a test instead of shifting
// live arguments in the game.
typedef struct {
    const void* originator;         // +0x00
    const void* classDescriptions;  // +0x08
    const void* hit;                // +0x10
    const void* attack;             // +0x18
    const void* sourceHandle2;      // +0x20
    uint8_t     hitWith;            // +0x28
    int32_t     conditionRollIndex; // +0x2c
    bool        entityDamagedEvent; // +0x30
    const void* sourceHandle3;      // +0x38
    const void* spellId2;           // +0x40
} DealDamageStackArgsLayout;

// esv::StatsSystem::ApplyDamage — the macOS counterpart of the function
// Windows BG3SE hooks as esv::StatsSystem::ThrowDamageEvent for its
// BeforeDealDamage event. Two independent corroborations on 7398727: it is
// called from inside StatsFunctorDealDamage::Execute (0x105774c14), and the
// instruction immediately after that call loads
// ls::TypeId<esv::PassiveSystem, ecs::SystemsContext>::m_TypeIndex, matching
// the "near ref to PassiveSystemID" note on Windows' own binary signature for
// this call site. Its argument shape matches Windows'
// (statsSystem, temp5, hit, attack, a5, a6) with a5 typed here as EAbility.
// All six arguments are register-passed, so no stack packing is involved.
typedef void (*StatsSystemThrowDamageEventProc)(
    void*        statsSystem,
    const void*  entityView,        // ecs::EntityRefView<...> const&
    void*        hit,               // eoc::HitDesc&
    void*        attack,            // eoc::AttackDesc&
    uint8_t      ability,           // EAbility
    bool         flag
);

// (anonymous namespace)::ProcessDealDamageFunctors. All C++ const-reference
// parameters are pointers at the C ABI boundary. Only `eventIndex` is passed
// by value. Opaque context arguments must not be dereferenced without a
// separately verified layout.
//
// NO LONGER HOOKED. It used to source BeforeDealDamage/DealDamage, but it does
// not receive the Windows ApplyDamage payload, so those events carried a
// mostly-nil table. Both now come from the two targets above. The typedef and
// the ADDR_/GAME_FN_ entries stay as documented recon.
typedef void (*ProcessDealDamageFunctorsProc)(
    void*                   worldView,
    const StatsFunctorBase* functor,
    const uint64_t*         entityHandle,
    const void*             position,
    const void*             spellState,
    const void*             damageEffectFlags,
    const void*             ability,
    const void*             spellAttackType,
    const void*             dependency1,
    const void*             dependency2,
    int                     eventIndex,
    void*                   interruptEvents
);

#endif // FUNCTOR_TYPES_H
