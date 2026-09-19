#pragma once

#include "Chimera_classes.hpp"

// Shared by player_weapons.cpp and player_movement.cpp: neither file can set a
// GAS attribute's BaseValue (no reflected setter -- see the weapon/movement
// audits), so a cheat slider has to observe whatever the ability system
// aggregated (weapon attachments, LEMs) and layer its own change on top of
// CurrentValue instead of stomping it.
namespace BetterCheats
{
	class ComposedAttribute
	{
	public:
		enum class Mode { Multiply, Add, Absolute };

		// `owner` identifies the attribute-set instance the attribute lives on, so a
		// respawn that reallocates it is detected instead of composing onto a stale
		// baseline. `amount` is the slider/toggle value; Multiply and Add treat it as
		// relative to the game's own value, Absolute replaces it outright.
		void Apply(const void* owner, SDK::FGameplayAttributeData& attr, float amount, Mode mode,
		           float minFinal, float maxFinal);

		// Hands CurrentValue back to the game if our last write is still sitting there
		// untouched. If the game already re-aggregated over it, that value is newer
		// than ours and is left alone.
		void Release(const void* owner, SDK::FGameplayAttributeData& attr);

		// Owner gone (respawn, world change) -- drop state, write nothing. The
		// attribute reference itself may already be dangling, so Forget never touches it.
		void Forget();

	private:
		const void* m_owner   = nullptr;
		float       m_game    = 0.0f;   // last known game-aggregated value (attachments/LEMs included)
		float       m_written = 0.0f;   // last value we wrote, so a re-aggregation is detectable
		bool        m_active  = false;
	};
}
