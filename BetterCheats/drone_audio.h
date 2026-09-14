#pragma once

#include "plugin_interface.h"

// Drone audio volume.
//
// The constant drone hum comes from several places, so this walks all of them:
//   * ACrCharacterDroneBase   -- the piloted building drone, three named
//                                UAudioComponents (Idle / Movement / Rotation)
//   * ACrBuildingActorBase    -- drone stations and their kin, via the
//                                StateAudioComponents array
//
// Everything written here is a component INSTANCE living on an actor in the
// current world -- no CDO, no developer settings, nothing shared with another
// save. Setting the volume back to 1.0 fully restores the game's own mix.
namespace BetterCheats::Panels::DroneAudio
{
	void Initialize();
	void Tick(float deltaSeconds);
	void RenderImGui(IModLoaderImGui* imgui);
	void ApplySavedConfig();

	// Volumes live in the plugin config so the loader's own settings UI surfaces
	// them; this re-reads them when they are edited there.
	void OnConfigChanged(const char* section, const char* key, const char* newValue);
}
