#include "attribute_compose.h"

#include <algorithm>

namespace BetterCheats
{
	void ComposedAttribute::Apply(const void* owner, SDK::FGameplayAttributeData& attr, float amount,
	                               Mode mode, float minFinal, float maxFinal)
	{
		if (!m_active || m_owner != owner)
		{
			// First activation, or the attribute set changed under us -- the only
			// trustworthy source for "what the game has" is what it holds right now.
			m_game   = attr.CurrentValue;
			m_owner  = owner;
			m_active = true;
		}
		else if (attr.CurrentValue != m_written)
		{
			// Nothing here wrote this value, so the game re-aggregated (attachment or
			// LEM changed, weapon swapped). Recapture -- never from a value we wrote,
			// or a Multiply would compound on itself.
			m_game = attr.CurrentValue;
		}

		float final = 0.0f;
		switch (mode)
		{
		case Mode::Multiply: final = m_game * amount; break;
		case Mode::Add:      final = m_game + amount; break;
		case Mode::Absolute: final = amount;           break;
		}
		final = std::clamp(final, minFinal, maxFinal);

		if (attr.CurrentValue != final)
			attr.CurrentValue = final;
		m_written = final;
	}

	void ComposedAttribute::Release(const void* owner, SDK::FGameplayAttributeData& attr)
	{
		if (m_active && m_owner == owner && attr.CurrentValue == m_written)
			attr.CurrentValue = m_game;
		m_active = false;
	}

	void ComposedAttribute::Forget()
	{
		m_active = false;
	}
}
