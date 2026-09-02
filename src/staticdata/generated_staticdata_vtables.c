// Generated from the BG3 4.1.1.7398727 arm64 binary — do not edit by hand.
//
// Reproduce with:
//   nm -arch arm64 <binary> | grep ' __ZTV' | c++filt
// and take the "vtable for <engine class>" address minus the 0x100000000 image
// base, for each engine class in generated_staticdata_registry.c.
//
// An object's vptr is this offset PLUS 0x10, not the symbol address itself:
// every one of these vtables begins with an all-zero offset-to-top slot and an
// all-zero typeinfo slot (the binary is built without RTTI), so the Itanium ABI
// address point sits at +0x10. Verified by dumping the first six quadwords of
// all 105 vtables out of __DATA_CONST,__const: offset-to-top and typeinfo are 0
// for all of them, and slot 0/1 are the adjacent D1/D0 destructor pair.
//
// Why this table exists: ls::TypeId<T, ls::ImmutableDataHeadmaster>::m_TypeIndex
// is a guarded function-local static that ships as 0 with a zero guard variable
// (checked for all 105 types). Until the game first resolves that type, reading
// the global yields 0 — a perfectly valid index belonging to whichever type
// registered first — so an index-only lookup silently hands back ANOTHER type's
// bank. Matching the bank's vptr against the type's own vtable is what makes the
// answer trustworthy, and lets a type still be found when its index global has
// not been initialised.
//
// See ghidra/offsets/STATICDATA_HEADMASTER_LOOKUP.md.
#include "staticdata_registry.h"
#include <stddef.h>

const StaticDataVtableEntry g_staticdata_vtables[] = {
    { "eoc::AbilityDistributionPresetManager", 0x086b5d18ULL },
    { "eoc::AbilityListManager", 0x086b5e20ULL },
    { "eoc::ActionResourceTypes", 0x086b6698ULL },
    { "eoc::ActionResourceGroupManager", 0x086b6568ULL },
    { "ls::AnimationSetPriorityManager", 0x086a6790ULL },
    { "ls::ShortNameManager", 0x0886fd00ULL },
    { "eoc::ShortNameCategoryManager", 0x086ee278ULL },
    { "eoc::ApprovalRatingManager", 0x086b6b68ULL },
    { "eoc::AreaLevelOverrideManager", 0x086b6c70ULL },
    { "eoc::AvatarContainerTemplateManager", 0x086b6e38ULL },
    { "eoc::BackgroundManager", 0x086b6f18ULL },
    { "eoc::background::Goals", 0x086b7020ULL },
    { "eoc::calendar::DayRanges", 0x086b7a28ULL },
    { "eoc::CampChestTemplateManager", 0x086b7b30ULL },
    { "eoc::CharacterCreationAccessorySetManager", 0x086b7cb0ULL },
    { "eoc::CharacterCreationAppearanceMaterialManager", 0x086b7db8ULL },
    { "eoc::CharacterCreationAppearanceVisualManager", 0x086b7ec0ULL },
    { "eoc::CharacterCreationEyeColorManager", 0x086b8080ULL },
    { "eoc::CharacterCreationEquipmentIconsManager", 0x086b82b8ULL },
    { "eoc::CharacterCreationIconSettingsManager", 0x086b83c0ULL },
    { "eoc::CharacterCreationMaterialOverrideManager", 0x086b8010ULL },
    { "eoc::CharacterCreationPassiveAppearanceManager", 0x086b84c8ULL },
    { "eoc::CharacterCreationPresetManager", 0x086b85d0ULL },
    { "eoc::CharacterCreationSharedVisualManager", 0x086b86d8ULL },
    { "eoc::CharacterCreationVOLineManager", 0x086b87e0ULL },
    { "eoc::CinematicArenaFrequencyGroupManager", 0x086b8a50ULL },
    { "eoc::ClassDescriptions", 0x086b8b58ULL },
    { "eoc::ColorDefinitions", 0x086a5ca0ULL },
    { "eoc::CompanionPresetManager", 0x086b91e8ULL },
    { "eoc::ConditionErrorDescriptionManager", 0x086a5d80ULL },
    { "eoc::customdice::TemplatesManager", 0x086b9670ULL },
    { "eoc::DLCManager", 0x086b9778ULL },
    { "eoc::DeathTypeEffectsManager", 0x086b9898ULL },
    { "eoc::DifficultyClassManager", 0x086b9c50ULL },
    { "eoc::DisturbanceProperties", 0x086a5e60ULL },
    { "eoc::EncumbranceTypesManager", 0x086ba1b0ULL },
    { "eoc::EquipmentListManager", 0x086da060ULL },
    { "eoc::EquipmentTypes", 0x086da168ULL },
    { "eoc::ExperienceRewards", 0x086a5f40ULL },
    { "eoc::FactionContainer", 0x086da4d0ULL },
    { "eoc::FeatManager", 0x086da5b0ULL },
    { "eoc::FeatDescriptionManager", 0x086da6b8ULL },
    { "eoc::feat::SoundStateManager", 0x086da7c0ULL },
    { "eoc::hotbar::FixedHotBarSlots", 0x086da8c8ULL },
    { "ls::FlagManager", 0x0885e838ULL },
    { "eoc::flag::SoundStateManager", 0x086da9d0ULL },
    { "eoc::GodManager", 0x086daf70ULL },
    { "eoc::GoldRewards", 0x086a6020ULL },
    { "eoc::GossipContainer", 0x086db0c8ULL },
    { "eoc::ItemThrowParamsManager", 0x086db3d8ULL },
    { "eoc::itemwall::TemplatesManager", 0x086db4e8ULL },
    { "eoc::LevelMapValues", 0x086db6b8ULL },
    { "eoc::LimbsMappingManager", 0x086dba10ULL },
    { "eoc::LongRestCosts", 0x086dbb18ULL },
    { "eoc::ManagedStatusVFXContainer", 0x086dbc20ULL },
    { "eoc::MultiEffectInfoRegistry", 0x086b9db8ULL },
    { "eoc::one_time_reward::RewardManager", 0x086dd230ULL },
    { "eoc::OriginManager", 0x086dd338ULL },
    { "eoc::OriginIntroEntityManager", 0x086dd440ULL },
    { "eoc::PassiveListManager", 0x086dd558ULL },
    { "eoc::PassivesVFXManager", 0x086a6100ULL },
    { "eoc::photo_mode::BlueprintOverrideManager", 0x086dd898ULL },
    { "eoc::photo_mode::ColourGradings", 0x086b8cd8ULL },
    { "eoc::photo_mode::DecorFrames", 0x086b99a0ULL },
    { "eoc::photo_mode::EmoteAnimations", 0x086b9e98ULL },
    { "eoc::photo_mode::EmoteCollections", 0x086b9fa0ULL },
    { "eoc::photo_mode::EmotePoses", 0x086ba0a8ULL },
    { "eoc::photo_mode::FaceExpressions", 0x086da3a0ULL },
    { "eoc::photo_mode::FaceExpressionCollections", 0x086da298ULL },
    { "eoc::photo_mode::Stickers", 0x086f0fa8ULL },
    { "eoc::photo_mode::Vignettes", 0x086fa308ULL },
    { "eoc::ProgressionManager", 0x086dd9b8ULL },
    { "eoc::ProgressionDescriptionManager", 0x086ddb30ULL },
    { "eoc::ProjectileDefaultContainer", 0x086ddc10ULL },
    { "eoc::RaceManager", 0x086dde80ULL },
    { "eoc::RandomCastOutcomes", 0x086a61e0ULL },
    { "eoc::ruleset::Rulesets", 0x086de1d8ULL },
    { "eoc::ruleset::RulesetModifiers", 0x086de0f8ULL },
    { "eoc::ruleset::RulesetModifierOptions", 0x086de168ULL },
    { "eoc::ruleset::RulesetSelectionPresets", 0x086de248ULL },
    { "eoc::ruleset::RulesetValues", 0x086de2b8ULL },
    { "eoc::ScriptMaterialParameterOverrideManager", 0x086de9f8ULL },
    { "eoc::ScriptMaterialPresetOverrideManager", 0x086a6800ULL },
    { "esv::shapeshift::rules::Rulebook", 0x086a66b0ULL },
    { "eoc::SkillListManager", 0x086ee380ULL },
    { "eoc::sound::SpellTrajectoryRules", 0x086efcc8ULL },
    { "eoc::SpellListManager", 0x086ef340ULL },
    { "eoc::SpellMetaConditionManager", 0x086ef448ULL },
    { "eoc::status::SoundStateManager", 0x086f0e70ULL },
    { "eoc::SurfaceCursorMessageManager", 0x086a62c0ULL },
    { "eoc::tadpole_tree::TadpolePowersTree", 0x086a6330ULL },
    { "ls::TagManager", 0x08875260ULL },
    { "eoc::tag::SoundStateManager", 0x086f1800ULL },
    { "eoc::TooltipExtraTextManager", 0x086f9730ULL },
    { "eoc::TooltipUpcastDescriptionManager", 0x086f9838ULL },
    { "eoc::projectile::TrajectoryRules", 0x086f99e0ULL },
    { "eoc::tutorial::EntriesManager", 0x086a6410ULL },
    { "eoc::tutorial::TutorialEventManager", 0x086fa000ULL },
    { "eoc::tutorial::ModalEntriesManager", 0x086a64f0ULL },
    { "eoc::tutorial::UnifiedEntriesManager", 0x086a65d0ULL },
    { "eoc::VFXContainer", 0x086fa200ULL },
    { "ls::VisualLocatorAttachmentManager", 0x086a6870ULL },
    { "eoc::VoiceManager", 0x086fa410ULL },
    { "eoc::WeaponAnimationSetData", 0x086fa518ULL },
    { "eoc::weight::WeightCategories", 0x086fa620ULL },
    { NULL, 0 }
};

const int g_staticdata_vtable_count = 105;
