#pragma once

#include "plugin_interface.h"

namespace BetterCheats::Panels::Weapons
{
	void RenderImGui(IModLoaderImGui* imgui);

	// Re-applies enabled weapon attribute overrides every engine tick. Gameplay
	// effects re-evaluate from BaseValue, so an override has to be re-asserted
	// rather than written once.
	void Tick(float deltaSeconds);

	// Re-applies the overrides persisted in the active session's JSON config
	// (see session_config.h). Call on the game thread after
	// SessionConfig::Reload(), e.g. from OnExperienceLoadComplete.
	void ApplySavedConfig();
}
