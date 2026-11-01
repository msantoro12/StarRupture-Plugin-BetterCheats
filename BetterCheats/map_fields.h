#pragma once

#include "plugin_interface.h"
#include "Chimera_classes.hpp"
#include "plugin_map.h"

// Every game-class member BetterCheats reads or writes through the SDK, taken
// from the loader's map for the running game build (see plugin_map.h).
// Resolved once in OnPluginLoadHooks; read on the game thread with
// field.In(object), which is null when the object is null or the member did
// not resolve. The key is the class that declares the member.

#define BC_FIELD(Class, Member) SR_FIELD(decltype(SDK::Class::Member), Class, Member, 0)

namespace BetterCheats::Fields
{
	// ACrCharacterPlayerBase
	inline auto CrCharacterPlayerBase_ADSMoveSpeed = BC_FIELD(ACrCharacterPlayerBase, ADSMoveSpeed);
	inline auto CrCharacterPlayerBase_AccelerationAndDeceleration = BC_FIELD(ACrCharacterPlayerBase, AccelerationAndDeceleration);
	inline auto CrCharacterPlayerBase_CrouchedMoveSpeed = BC_FIELD(ACrCharacterPlayerBase, CrouchedMoveSpeed);
	inline auto CrCharacterPlayerBase_DoubleJumpEnergyUsage = BC_FIELD(ACrCharacterPlayerBase, DoubleJumpEnergyUsage);
	inline auto CrCharacterPlayerBase_DurationOfAccelerationToSprint = BC_FIELD(ACrCharacterPlayerBase, DurationOfAccelerationToSprint);
	inline auto CrCharacterPlayerBase_EnergyLoweringMultiplierDuringSprint = BC_FIELD(ACrCharacterPlayerBase, EnergyLoweringMultiplierDuringSprint);
	inline auto CrCharacterPlayerBase_NormalEnergyRestorationRate = BC_FIELD(ACrCharacterPlayerBase, NormalEnergyRestorationRate);
	inline auto CrCharacterPlayerBase_NormalMoveSpeed = BC_FIELD(ACrCharacterPlayerBase, NormalMoveSpeed);
	inline auto CrCharacterPlayerBase_SprintMoveSpeed = BC_FIELD(ACrCharacterPlayerBase, SprintMoveSpeed);

	// UCrCharacterMovementComponent
	inline auto CrCharacterMovementComponent_CoyoteTime = BC_FIELD(UCrCharacterMovementComponent, CoyoteTime);
	inline auto CrCharacterMovementComponent_DashCooldownSeconds = BC_FIELD(UCrCharacterMovementComponent, DashCooldownSeconds);
	inline auto CrCharacterMovementComponent_DashDuration = BC_FIELD(UCrCharacterMovementComponent, DashDuration);
	inline auto CrCharacterMovementComponent_DashEnergyCostBase = BC_FIELD(UCrCharacterMovementComponent, DashEnergyCostBase);
	inline auto CrCharacterMovementComponent_DefaultZiplineAcceleration = BC_FIELD(UCrCharacterMovementComponent, DefaultZiplineAcceleration);
	inline auto CrCharacterMovementComponent_DefaultZiplineSpeed = BC_FIELD(UCrCharacterMovementComponent, DefaultZiplineSpeed);
	inline auto CrCharacterMovementComponent_JumpEnergyCost = BC_FIELD(UCrCharacterMovementComponent, JumpEnergyCost);
	inline auto CrCharacterMovementComponent_MaxDashDistance = BC_FIELD(UCrCharacterMovementComponent, MaxDashDistance);
	inline auto CrCharacterMovementComponent_MaxSlideSpeed = BC_FIELD(UCrCharacterMovementComponent, MaxSlideSpeed);
	inline auto CrCharacterMovementComponent_SlideEnterImpulse = BC_FIELD(UCrCharacterMovementComponent, SlideEnterImpulse);
	inline auto CrCharacterMovementComponent_SlideFrictionFactor = BC_FIELD(UCrCharacterMovementComponent, SlideFrictionFactor);
	inline auto CrCharacterMovementComponent_SlideGravityForce = BC_FIELD(UCrCharacterMovementComponent, SlideGravityForce);

	// UCrGemAttributeSet
	inline auto CrGemAttributeSet_DodgeCostMultiplier = BC_FIELD(UCrGemAttributeSet, DodgeCostMultiplier);
	inline auto CrGemAttributeSet_DoubleJumpCostMultiplier = BC_FIELD(UCrGemAttributeSet, DoubleJumpCostMultiplier);
	inline auto CrGemAttributeSet_FallDamageMultiplier = BC_FIELD(UCrGemAttributeSet, FallDamageMultiplier);
	inline auto CrGemAttributeSet_SlideStaminaRegenMultiplier = BC_FIELD(UCrGemAttributeSet, SlideStaminaRegenMultiplier);
	inline auto CrGemAttributeSet_StaminaRegenMultiplier = BC_FIELD(UCrGemAttributeSet, StaminaRegenMultiplier);
	inline auto CrGemAttributeSet_ZiplineSpeedMultiplier = BC_FIELD(UCrGemAttributeSet, ZiplineSpeedMultiplier);

	// UCrMovementAttributeSet
	inline auto CrMovementAttributeSet_JumpMultiplier = BC_FIELD(UCrMovementAttributeSet, JumpMultiplier);
	inline auto CrMovementAttributeSet_SprintSpeedMultiplier = BC_FIELD(UCrMovementAttributeSet, SprintSpeedMultiplier);

	// UCrMovementSpeedMultiplierAttributeSet
	inline auto CrMovementSpeedMultiplierAttributeSet_CurrentMovementSpeedMultiplier = BC_FIELD(UCrMovementSpeedMultiplierAttributeSet, CurrentMovementSpeedMultiplier);
	inline auto CrMovementSpeedMultiplierAttributeSet_MaxMovementSpeedMultiplier = BC_FIELD(UCrMovementSpeedMultiplierAttributeSet, MaxMovementSpeedMultiplier);
	inline auto CrMovementSpeedMultiplierAttributeSet_MinMovementSpeedMultiplier = BC_FIELD(UCrMovementSpeedMultiplierAttributeSet, MinMovementSpeedMultiplier);

	// UCrWeaponAttributeSet
	inline auto CrWeaponAttributeSet_ADS_TransitionSpeedModMultiplier = BC_FIELD(UCrWeaponAttributeSet, ADS_TransitionSpeedModMultiplier);
	inline auto CrWeaponAttributeSet_DamageFallOffModMultiplier = BC_FIELD(UCrWeaponAttributeSet, DamageFallOffModMultiplier);
	inline auto CrWeaponAttributeSet_DamageModMultiplier = BC_FIELD(UCrWeaponAttributeSet, DamageModMultiplier);
	inline auto CrWeaponAttributeSet_FireRateModMultiplier = BC_FIELD(UCrWeaponAttributeSet, FireRateModMultiplier);
	inline auto CrWeaponAttributeSet_MaxMagAmmoModOffset = BC_FIELD(UCrWeaponAttributeSet, MaxMagAmmoModOffset);
	inline auto CrWeaponAttributeSet_PossibleEnemiesHitPerTrace = BC_FIELD(UCrWeaponAttributeSet, PossibleEnemiesHitPerTrace);
	inline auto CrWeaponAttributeSet_RecoilModMultiplier = BC_FIELD(UCrWeaponAttributeSet, RecoilModMultiplier);
	inline auto CrWeaponAttributeSet_ReloadSpeedModMultiplier = BC_FIELD(UCrWeaponAttributeSet, ReloadSpeedModMultiplier);
	inline auto CrWeaponAttributeSet_SpreadModMultiplier = BC_FIELD(UCrWeaponAttributeSet, SpreadModMultiplier);
	inline auto CrWeaponAttributeSet_SwayModMultiplier = BC_FIELD(UCrWeaponAttributeSet, SwayModMultiplier);

	// Asks the loader for every row above. False when any did not resolve; the
	// loader has then recorded the failure and refuses the plugin.
	inline bool Resolve(IPluginSelf* self, IPluginHookScanner* scanner)
	{
		return PluginMap::ResolveFields(scanner, self,
			CrCharacterPlayerBase_ADSMoveSpeed, CrCharacterPlayerBase_AccelerationAndDeceleration, CrCharacterPlayerBase_CrouchedMoveSpeed,
			CrCharacterPlayerBase_DoubleJumpEnergyUsage, CrCharacterPlayerBase_DurationOfAccelerationToSprint, CrCharacterPlayerBase_EnergyLoweringMultiplierDuringSprint,
			CrCharacterPlayerBase_NormalEnergyRestorationRate, CrCharacterPlayerBase_NormalMoveSpeed, CrCharacterPlayerBase_SprintMoveSpeed,
			CrCharacterMovementComponent_CoyoteTime, CrCharacterMovementComponent_DashCooldownSeconds, CrCharacterMovementComponent_DashDuration,
			CrCharacterMovementComponent_DashEnergyCostBase, CrCharacterMovementComponent_DefaultZiplineAcceleration, CrCharacterMovementComponent_DefaultZiplineSpeed,
			CrCharacterMovementComponent_JumpEnergyCost, CrCharacterMovementComponent_MaxDashDistance, CrCharacterMovementComponent_MaxSlideSpeed,
			CrCharacterMovementComponent_SlideEnterImpulse, CrCharacterMovementComponent_SlideFrictionFactor, CrCharacterMovementComponent_SlideGravityForce,
			CrGemAttributeSet_DodgeCostMultiplier, CrGemAttributeSet_DoubleJumpCostMultiplier, CrGemAttributeSet_FallDamageMultiplier,
			CrGemAttributeSet_SlideStaminaRegenMultiplier, CrGemAttributeSet_StaminaRegenMultiplier, CrGemAttributeSet_ZiplineSpeedMultiplier,
			CrMovementAttributeSet_JumpMultiplier, CrMovementAttributeSet_SprintSpeedMultiplier, CrMovementSpeedMultiplierAttributeSet_CurrentMovementSpeedMultiplier,
			CrMovementSpeedMultiplierAttributeSet_MaxMovementSpeedMultiplier, CrMovementSpeedMultiplierAttributeSet_MinMovementSpeedMultiplier, CrWeaponAttributeSet_ADS_TransitionSpeedModMultiplier,
			CrWeaponAttributeSet_DamageFallOffModMultiplier, CrWeaponAttributeSet_DamageModMultiplier, CrWeaponAttributeSet_FireRateModMultiplier,
			CrWeaponAttributeSet_MaxMagAmmoModOffset, CrWeaponAttributeSet_PossibleEnemiesHitPerTrace, CrWeaponAttributeSet_RecoilModMultiplier,
			CrWeaponAttributeSet_ReloadSpeedModMultiplier, CrWeaponAttributeSet_SpreadModMultiplier, CrWeaponAttributeSet_SwayModMultiplier
		);
	}
}
