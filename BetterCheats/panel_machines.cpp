#include "panel_machines.h"
#include "machine_power.h"
#include "drone_audio.h"

namespace BetterCheats::Panels
{
	void RenderMachines_Crafters(IModLoaderImGui* imgui)
	{
		static bool instantBuild   = false;
		static bool noResourceCost = false;

		imgui->SeparatorText("Crafters");
		imgui->TextDisabled("No options yet.");
	}

	void RenderMachines_Power(IModLoaderImGui* imgui)
	{
		Power::RenderImGui(imgui);
	}

	void RenderMachines_LogisticDrones(IModLoaderImGui* imgui)
	{
		DroneAudio::RenderImGui(imgui);
	}

	void RenderMachines_RailDrones(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Rail Drones");
		imgui->TextDisabled("No options yet.");
	}
}
