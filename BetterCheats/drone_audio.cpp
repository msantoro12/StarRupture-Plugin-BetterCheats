#include "drone_audio.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "ui_widgets.h"

#include "Chimera_classes.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace BetterCheats::Panels::DroneAudio
{
	namespace
	{
		constexpr float kActiveEpsilon = 0.0001f;

		// Re-applied on an interval rather than every frame: drones spawn and despawn
		// constantly, so newly created ones need catching, but walking every actor in
		// the world 60 times a second to do it would be silly.
		constexpr float kReapplySeconds = 0.5f;

		// One slider per sound the drone actually makes, so the constant idle hum can be
		// taken down without also losing the movement and rotation cues that tell you
		// what the drone is doing.
		enum VolIndex : int { kVolIdle = 0, kVolMovement, kVolRotation, kVolStation, kVolCount };

		struct VolDef { const char* label; const char* key; const char* tooltip; };

		const VolDef kVols[kVolCount] = {
			{ "Idle hum",       "IdleVolume",
			  "The constant drone that plays whenever the drone is out.\n"
			  "This is usually the one worth turning down." },
			{ "Movement",       "MovementVolume",
			  "Plays while the drone is travelling." },
			{ "Rotation",       "RotationVolume",
			  "Plays while the drone turns." },
			{ "Drone stations", "StationVolume",
			  "Building-side drone audio -- stations and their kin. Filtered to\n"
			  "drone-named actors, so it will not silence the rest of your base." },
		};

		float g_vol[kVolCount] = { 1.0f, 1.0f, 1.0f, 1.0f };
		float g_master         = 1.0f;   // convenience: writes all four at once
		float g_timer          = 0.0f;
		int   g_lastCount      = 0;
		int   g_writesLastPass = 0;   // how many components actually needed changing

		bool IsVolActive(int v)
		{
			return std::fabs(g_vol[v] - 1.0f) > kActiveEpsilon;
		}

		bool IsActive()
		{
			for (int v = 0; v < kVolCount; ++v)
				if (IsVolActive(v)) return true;
			return false;
		}

		// Compare before writing.
		//
		// SetVolumeMultiplier is not free of side effects -- re-asserting a value the
		// component already had, twice a second, was audible as a faint cycling
		// on/off artifact even with the volume at zero. Reading VolumeMultiplier back
		// and writing only on a real difference makes the steady state completely
		// silent, and still catches a drone that spawned since the last pass or a
		// value the game reset underneath us.
		void SetComponentVolume(SDK::UAudioComponent* audio, float volume)
		{
			if (!audio) return;
			if (std::fabs(audio->VolumeMultiplier - volume) <= kActiveEpsilon) return;

			audio->SetVolumeMultiplier(volume);
			++g_writesLastPass;
		}

		bool NameHasDrone(SDK::UObject* obj)
		{
			if (!obj) return false;

			std::string name = obj->GetName();
			for (char& c : name)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');

			return name.find("drone") != std::string::npos;
		}

		void ApplyToWorld()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return; }
			if (!world) return;

			int touched = 0;
			g_writesLastPass = 0;

			// The piloted drone -- three separately addressable components.
			{
				SDK::TArray<SDK::AActor*> actors{};
				SDK::UGameplayStatics::GetAllActorsOfClass(
					world, SDK::ACrCharacterDroneBase::StaticClass(), &actors);

				for (int32_t i = 0; i < actors.Num(); ++i)
				{
					auto* drone = static_cast<SDK::ACrCharacterDroneBase*>(actors[i]);
					if (!drone) continue;

					SetComponentVolume(drone->IdleSound,     g_vol[kVolIdle]);
					SetComponentVolume(drone->MovementSound, g_vol[kVolMovement]);
					SetComponentVolume(drone->RotationSound, g_vol[kVolRotation]);
					++touched;
				}
			}

			// Drone stations and anything else drone-named carrying building audio.
			// Name-filtered so this does not silence every machine in the base.
			{
				SDK::TArray<SDK::AActor*> actors{};
				SDK::UGameplayStatics::GetAllActorsOfClass(
					world, SDK::ACrBuildingActorBase::StaticClass(), &actors);

				for (int32_t i = 0; i < actors.Num(); ++i)
				{
					auto* building = static_cast<SDK::ACrBuildingActorBase*>(actors[i]);
					if (!building || !NameHasDrone(building)) continue;

					SDK::TArray<SDK::UAudioComponent*>& sounds = building->StateAudioComponents;
					for (int32_t s = 0; s < sounds.Num(); ++s)
						SetComponentVolume(sounds[s], g_vol[kVolStation]);

					++touched;
				}
			}

			g_lastCount = touched;
		}

		// Set from any thread; consumed by Tick on the game thread.
		//
		// ApplyToWorld calls GetAllActorsOfClass, which constructs an FActorIterator,
		// which asserts it is on the game thread. Calling it straight from a slider
		// handler -- so a drag was audible immediately -- ran it on the RENDER thread
		// and tripped that assert:
		//   FActorIteratorState  [EngineUtils.h:183]  <- check
		//   UGameplayStatics::execGetAllActorsOfClass
		//   DroneAudio::ApplyToWorld                  <- us
		//   DroneAudio::RenderVolRow                  <- render thread
		// Same rule the movement panel states at the top of its file: RenderImGui only
		// ever reads, never touches a UObject.
		bool g_applyWanted = false;

		void RequestApply()
		{
			g_applyWanted = true;
		}

		void ApplyNow()
		{
			try { ApplyToWorld(); }
			catch (...) {}
		}

		void StoreVol(int v)
		{
			if (g_vol[v] < 0.0f) g_vol[v] = 0.0f;
			if (g_vol[v] > 1.0f) g_vol[v] = 1.0f;
			BetterCheatsConfig::Config::WriteDroneVolume(kVols[v].key, g_vol[v]);
		}

		void LoadFromConfig()
		{
			for (int v = 0; v < kVolCount; ++v)
			{
				float value = BetterCheatsConfig::Config::ReadDroneVolume(kVols[v].key);
				if (value < 0.0f) value = 0.0f;
				if (value > 1.0f) value = 1.0f;
				g_vol[v] = value;
			}
		}

		// The master is a convenience that writes every channel, not a separate scaling
		// layer -- keeping it as one more multiplier on top would mean two numbers to
		// reason about for every sound and no way to read the real value off a row.
		void ApplyMaster(float value)
		{
			for (int v = 0; v < kVolCount; ++v)
			{
				g_vol[v] = value;
				StoreVol(v);
			}

		}

		// Renders one volume row: label, joined slider + number box, reset.
		void RenderVolRow(IModLoaderImGui* imgui, const char* label, const char* tooltip,
		                  float* value, bool active, int id, void (*onChange)(float))
		{
			imgui->PushIDInt(id);
			imgui->TableNextRow(0, 0.0f);

			imgui->TableSetColumnIndex(0);
			if (active) imgui->Text(label);
			else        imgui->TextDisabled(label);
			if (tooltip && imgui->IsItemHovered())
				imgui->SetTooltip(tooltip);

			imgui->TableSetColumnIndex(1);

			float availX = 0.0f, availY = 0.0f;
			imgui->GetContentRegionAvail(&availX, &availY);

			// Measured, not a fixed pixel count -- FontScale is user-configurable.
			float textW = 0.0f, textH = 0.0f;
			imgui->CalcTextSize("-88.88", &textW, &textH, false, -1.0f);

			const float frameH  = imgui->GetFrameHeight();
			const float numBoxW = textW + (frameH * 2.0f) + (frameH * 0.9f);
			const float sliderW = (availX > numBoxW + frameH * 2.0f)
				? (availX - numBoxW)
				: (availX * 0.55f);

			bool changed = false;
			imgui->SetNextItemWidth(sliderW);
			if (imgui->SliderFloat("##slider", value, 0.0f, 1.0f, "%.2f"))
				changed = true;

			imgui->SameLine(0.0f, 0.0f);
			imgui->SetNextItemWidth(-1.0f);
			if (imgui->InputFloat("##num", value, 0.05f, 0.25f, "%.2f"))
				changed = true;

			if (changed)
			{
				if (*value < 0.0f) *value = 0.0f;
				if (*value > 1.0f) *value = 1.0f;
				onChange(*value);
				RequestApply();   // picked up by Tick on the game thread
			}

			imgui->TableSetColumnIndex(2);
			if (BetterCheats::UI::ResetButton(imgui, "##reset"))
			{
				*value = 1.0f;
				onChange(1.0f);
				RequestApply();
			}
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Back to the game's own volume.");

			imgui->PopID();
		}

		void OnMasterChanged(float v) { g_master = v; ApplyMaster(v); }
		void OnIdleChanged(float)     { g_vol[kVolIdle]     = g_vol[kVolIdle];     StoreVol(kVolIdle); }
		void OnMoveChanged(float)     { StoreVol(kVolMovement); }
		void OnRotChanged(float)      { StoreVol(kVolRotation); }
		void OnStationChanged(float)  { StoreVol(kVolStation); }
	}

	void Initialize()
	{
		for (int v = 0; v < kVolCount; ++v)
			g_vol[v] = 1.0f;
		g_master    = 1.0f;
		g_timer     = 0.0f;
		g_lastCount = 0;

		LoadFromConfig();
	}

	// Fired by the loader when someone edits these in its own settings UI (or the
	// config file). Re-read and apply so a change there takes effect without a
	// restart, exactly as it does from the panel.
	void OnConfigChanged(const char* section, const char* /*key*/, const char* /*newValue*/)
	{
		if (!section || std::string(section) != "DroneAudio")
			return;

		LoadFromConfig();
		RequestApply();
	}

	void ApplySavedConfig()
	{
		LoadFromConfig();
		if (IsActive())
			RequestApply();
	}

	void Tick(float deltaSeconds)
	{
		// A queued request is honoured immediately, so dragging a slider still sounds
		// instant -- it just crosses to the game thread first.
		if (g_applyWanted)
		{
			g_applyWanted = false;
			g_timer       = 0.0f;
			ApplyNow();
			return;
		}

		if (!IsActive())
			return;

		g_timer += deltaSeconds;
		if (g_timer < kReapplySeconds)
			return;

		g_timer = 0.0f;
		ApplyNow();
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		imgui->SeparatorText("Drone Audio");
		imgui->TextDisabled("0.00 is silent, 1.00 is the game's own mix. Re-applied twice a second so");
		imgui->TextDisabled("drones that spawn later are caught too.");
		imgui->Spacing();

		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags = (1 << 6) | (1 << 9) | (3 << 13);

		if (imgui->BeginTable("##drone_audio_table", 3, kTableFlags))
		{
			imgui->TableSetupColumn("Sound", 0, 0.36f);
			imgui->TableSetupColumn("Volume", 0, 0.54f);
			imgui->TableSetupColumn("",       0, 0.10f);

			RenderVolRow(imgui, "All drone audio",
				"Sets every row below at once. Adjust an individual row afterwards and\n"
				"it keeps its own value -- this is a shortcut, not an extra layer.",
				&g_master, IsActive(), 0, &OnMasterChanged);

			RenderVolRow(imgui, kVols[kVolIdle].label,     kVols[kVolIdle].tooltip,
				&g_vol[kVolIdle],     IsVolActive(kVolIdle),     1, &OnIdleChanged);
			RenderVolRow(imgui, kVols[kVolMovement].label, kVols[kVolMovement].tooltip,
				&g_vol[kVolMovement], IsVolActive(kVolMovement), 2, &OnMoveChanged);
			RenderVolRow(imgui, kVols[kVolRotation].label, kVols[kVolRotation].tooltip,
				&g_vol[kVolRotation], IsVolActive(kVolRotation), 3, &OnRotChanged);
			RenderVolRow(imgui, kVols[kVolStation].label,  kVols[kVolStation].tooltip,
				&g_vol[kVolStation],  IsVolActive(kVolStation),  4, &OnStationChanged);

			imgui->EndTable();
		}

		if (g_lastCount > 0)
		{
			char line[96];
			snprintf(line, sizeof(line), "  %d drone audio source(s) adjusted", g_lastCount);
			imgui->TextDisabled(line);
		}
	}
}
