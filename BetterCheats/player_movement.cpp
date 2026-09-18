#include "player_movement.h"
#include "plugin_helpers.h"
#include "plugin_config.h"
#include "cheat_menu.h"
#include "keybind_picker.h"
#include "game_context.h"
#include "session_config.h"
#include "ui_widgets.h"

#include "Chimera_classes.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// No-clip is the classic UE ghost cheat, done entirely through the generated
// SDK — no AOB patterns, no detours:
//
//   * the capsule stops colliding, so nothing blocks the character
//   * the movement mode becomes MOVE_Flying, which skips gravity and the floor
//     checks that would otherwise drop the character out of the world
//   * bCheatFlying tells the movement component this flying is deliberate, so a
//     listen host does not correct it back
//
// Horizontal movement still comes from the game's own input; the game only
// steers on the horizontal plane, so Space / Left Ctrl are fed in here as an
// extra vertical axis.
//
// Everything that touches a UObject runs on the game thread (Tick).
// RenderImGui() runs on the render thread and only ever reads the snapshot.

namespace BetterCheats::Panels::Movement
{
	namespace
	{
		constexpr float kDefaultFlySpeedMultiplier = 3.0f;
		constexpr float kMinFlySpeedMultiplier     = 1.0f;
		constexpr float kMaxFlySpeedMultiplier     = 25.0f;

		// Used only if the movement component ships with no fly speed of its own —
		// UCharacterMovementComponent's own default, in cm/s.
		constexpr float kFallbackMaxFlySpeed = 600.0f;

		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags  = (1 << 6) | (1 << 9) | (3 << 13);
		constexpr int kColumnFixed = 1 << 4; // ImGuiTableColumnFlags_WidthFixed

		// A value is applied only when it differs from the game default, so there is
		// no separate enable toggle to keep in sync -- resetting a row IS disabling
		// it. Mirrors player_weapons.cpp.
		constexpr float kActiveEpsilon = 0.0001f;

		// ---------------------------------------------------------------------
		// Wanted state — written from the render thread and the keybind callback,
		// read on the game thread.
		// ---------------------------------------------------------------------
		std::atomic<bool>  g_noClipWanted{ false };
		std::atomic<float> g_flySpeedMultiplier{ kDefaultFlySpeedMultiplier };

		// Flight and wall-clipping are separate concerns. F9 toggles FLIGHT; this
		// decides whether the capsule also stops colliding while flying. Off gives you
		// flight that still bumps into the world, which is what you want for getting
		// around a base without falling through it.
		std::atomic<bool>  g_passThroughWalls{ true };

		// ---------------------------------------------------------------------
		// Applied state — game thread only.
		//
		// g_appliedTo is compared, never dereferenced blind: the pawn is replaced
		// on respawn and the old one is destroyed, so state belonging to a
		// character that is no longer the local pawn is dropped rather than
		// restored.
		// ---------------------------------------------------------------------
		bool                         g_active    = false;
		SDK::ACrCharacterPlayerBase* g_appliedTo = nullptr;

		SDK::ECollisionEnabled g_savedCollision      = SDK::ECollisionEnabled::QueryAndPhysics;
		float                  g_savedMaxFlySpeed    = kFallbackMaxFlySpeed;
		float                  g_savedFlyingBraking  = 0.0f;
		bool                   g_savedCheatFlying    = false;

		SDK::ACrCharacterPlayerBase* GetLocalCharacter()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc || !pc->Pawn) return nullptr;

			// See player_attributes.cpp's GetLocalCharacter — the pawn isn't a
			// Chimera character until it's actually possessed.
			SDK::UClass* characterClass = SDK::ACrCharacterPlayerBase::StaticClass();
			if (!characterClass || !pc->Pawn->IsA(characterClass)) return nullptr;

			return static_cast<SDK::ACrCharacterPlayerBase*>(pc->Pawn);
		}

		// Key state is polled rather than bound: the modloader exposes press and
		// release events, not "is held", and holding Space is exactly what
		// ascending needs. Only listen while this process owns the foreground
		// window, or the character climbs while the player is in another app.
		bool IsGameForeground()
		{
			HWND foreground = GetForegroundWindow();
			if (!foreground) return false;

			DWORD pid = 0;
			GetWindowThreadProcessId(foreground, &pid);
			return pid == GetCurrentProcessId();
		}

		bool IsKeyHeld(int virtualKey)
		{
			return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
		}

		void Enable(SDK::ACrCharacterPlayerBase* character)
		{
			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			SDK::UCapsuleComponent* capsule = character->CapsuleComponent;
			if (!move || !capsule)
				return;

			g_savedCollision     = capsule->GetCollisionEnabled();
			g_savedMaxFlySpeed   = move->MaxFlySpeed > 0.0f ? move->MaxFlySpeed : kFallbackMaxFlySpeed;
			g_savedFlyingBraking = move->BrakingDecelerationFlying;
			g_savedCheatFlying   = move->bCheatFlying != 0;

			if (g_passThroughWalls.load())
				capsule->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
			move->bCheatFlying = 1;
			move->SetMovementMode(SDK::EMovementMode::MOVE_Flying, 0);

			g_active    = true;
			g_appliedTo = character;

			LOG_INFO("Movement: no-clip enabled.");
		}

		void Disable(SDK::ACrCharacterPlayerBase* character)
		{
			g_active    = false;
			g_appliedTo = nullptr;

			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			SDK::UCapsuleComponent* capsule = character->CapsuleComponent;

			if (capsule)
				capsule->SetCollisionEnabled(g_savedCollision);

			if (move)
			{
				move->MaxFlySpeed               = g_savedMaxFlySpeed;
				move->BrakingDecelerationFlying = g_savedFlyingBraking;
				move->bCheatFlying              = g_savedCheatFlying ? 1 : 0;

				// Falling rather than walking: the character is usually in mid-air
				// when no-clip goes off, and the engine drops it into walking as
				// soon as it finds a floor.
				move->SetMovementMode(SDK::EMovementMode::MOVE_Falling, 0);
			}

			LOG_INFO("Movement: no-clip disabled.");
		}

		void MaintainNoClip(SDK::ACrCharacterPlayerBase* character)
		{
			SDK::UCharacterMovementComponent* move = character->CharacterMovement;
			if (!move)
				return;

			// Zipline, slide and dash all drive the movement mode themselves, so
			// take it back whenever something else has changed it.
			if (move->MovementMode != SDK::EMovementMode::MOVE_Flying)
				move->SetMovementMode(SDK::EMovementMode::MOVE_Flying, 0);

			// Honour a mid-flight change to the wall-clipping toggle rather than making
			// the player land and take off again.
			if (SDK::UCapsuleComponent* capsule = character->CapsuleComponent)
			{
				const bool wantGhost = g_passThroughWalls.load();
				const bool isGhost   = capsule->GetCollisionEnabled() == SDK::ECollisionEnabled::NoCollision;
				if (wantGhost && !isGhost)
					capsule->SetCollisionEnabled(SDK::ECollisionEnabled::NoCollision);
				else if (!wantGhost && isGhost)
					capsule->SetCollisionEnabled(g_savedCollision);
			}

			const float maxSpeed = g_savedMaxFlySpeed * g_flySpeedMultiplier.load();
			move->MaxFlySpeed = maxSpeed;

			// Flying braking ships at 0 on most components, which means coasting
			// forever once you let go. Brake hard instead so the character parks
			// where the player stopped steering.
			move->BrakingDecelerationFlying = maxSpeed * 4.0f;

			if (CheatMenu::IsOpen() || !IsGameForeground())
				return;

			float vertical = 0.0f;
			if (IsKeyHeld(VK_SPACE))                                 vertical += 1.0f;
			if (IsKeyHeld(VK_LCONTROL) || IsKeyHeld(VK_RCONTROL))    vertical -= 1.0f;

			if (vertical != 0.0f)
				character->AddMovementInput(SDK::FVector(0.0, 0.0, 1.0), vertical, true);
		}

		// ---------------------------------------------------------------------
		// Snapshot — populated on the game thread (Tick), read on the ImGui
		// render thread. Never touch SDK objects from RenderImGui().
		// ---------------------------------------------------------------------
		struct MovementSnapshot
		{
			bool  characterFound = false;
			bool  active         = false;
			float baseFlySpeed   = kFallbackMaxFlySpeed;
		};

		std::mutex       g_snapshotMutex;
		MovementSnapshot g_snapshot;

		void RefreshSnapshot(SDK::ACrCharacterPlayerBase* character)
		{
			MovementSnapshot snap;
			snap.characterFound = character != nullptr;
			snap.active         = g_active;
			snap.baseFlySpeed   = g_active ? g_savedMaxFlySpeed : kFallbackMaxFlySpeed;

			if (!g_active && character && character->CharacterMovement && character->CharacterMovement->MaxFlySpeed > 0.0f)
				snap.baseFlySpeed = character->CharacterMovement->MaxFlySpeed;

			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			g_snapshot = snap;
		}

		// The combo we actually registered with. Config::GetNoClipKey() hands back
		// a shared static buffer, so it is read once here rather than from the
		// render thread every frame — and unregistering must use the same string.
		// Written by the rebind control on the render thread, read there and on
		// shutdown, hence the lock.
		std::mutex g_keyMutex;
		char       g_noClipKey[64] = "";

		void ReadNoClipKey(char* out, size_t outSize)
		{
			std::lock_guard<std::mutex> lock(g_keyMutex);
			snprintf(out, outSize, "%s", g_noClipKey);
		}

		void OnNoClipKeyPressed(EModKey /*key*/, EModKeyEvent /*event*/)
		{
			if (!GameContext::IsInChimeraMain() || !GameContext::AreCheatsAllowed())
				return;

			g_noClipWanted.store(!g_noClipWanted.load());
		}

		// Swaps the live registration and persists the new combo. The loader
		// matches an unregistration against the name the bind was created with, so
		// handing it the string we last registered is enough even if it has been
		// rebound from the loader's own config UI in the meantime.
		void ApplyNoClipKey(const char* combo)
		{
			IPluginSelf* self = GetSelf();
			if (!self || !self->hooks || !self->hooks->Input || !combo || !*combo)
				return;

			char previous[64];
			{
				std::lock_guard<std::mutex> lock(g_keyMutex);
				if (strcmp(g_noClipKey, combo) == 0)
					return;

				snprintf(previous, sizeof(previous), "%s", g_noClipKey);
				snprintf(g_noClipKey, sizeof(g_noClipKey), "%s", combo);
			}

			if (previous[0])
				self->hooks->Input->UnregisterKeybindByName(previous, EModKeyEvent::Pressed, &OnNoClipKeyPressed);

			self->hooks->Input->RegisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
			BetterCheatsConfig::Config::SetNoClipKey(combo);

			LOG_INFO("Movement: no-clip keybind is now %s.", combo);
		}

		// =======================================================================
		// Attribute overrides — FGameplayAttributeData on three attribute sets
		// that are DIRECT members of ACrCharacterPlayerBase (no ability-system
		// traversal): MovementAttributes, MovementSpeedMultiplierAttributes and
		// GemAttributes. Same enable-by-differing-from-default model, joined
		// slider+box row and drawn reset button as player_weapons.cpp; there is
		// no per-weapon-style profile here, just one flat set of player values.
		// =======================================================================

		struct AttrDef
		{
			const char* label;
			const char* key;
			float       defaultValue;
			float       minValue;
			float       maxValue;
			float       step;
			const char* format;
			const char* tooltip;
		};

		// Grouped so the group's row range is a contiguous slice — see kAttrs below.
		enum AttrIndex : int
		{
			// "Speed" group
			kAttrMoveSpeed = 0, kAttrMoveSpeedMax, kAttrMoveSpeedMin, kAttrSprintSpeed,
			// "Jump & Dodge" group
			kAttrJumpHeight, kAttrDoubleJumpCost, kAttrDodgeCost,
			// "Stamina & Survival" group
			kAttrStaminaRegen, kAttrSlideStaminaRegen, kAttrFallDamage, kAttrZiplineSpeed,
			kAttrCount
		};

		constexpr int kSpeedGroupFirst   = kAttrMoveSpeed,      kSpeedGroupCount   = 4;
		constexpr int kJumpGroupFirst    = kAttrJumpHeight,     kJumpGroupCount    = 3;
		constexpr int kStaminaGroupFirst = kAttrStaminaRegen,   kStaminaGroupCount = 4;

		const AttrDef kAttrs[kAttrCount] = {
			{ "Move Speed",           "moveSpeed",         1.0f, 0.10f, 10.0f, 0.05f, "%.2fx",
			  "CurrentMovementSpeedMultiplier -- this is what walking/running actually reads." },
			{ "Move Speed Cap",       "moveSpeedMax",      1.0f, 0.10f, 10.0f, 0.05f, "%.2fx",
			  "Ceiling Move Speed is clamped to. Raise this alongside Move Speed or a high\n"
			  "override gets clamped back down." },
			{ "Move Speed Floor",     "moveSpeedMin",      1.0f, 0.00f,  5.0f, 0.05f, "%.2fx",
			  "Floor Move Speed is clamped to. Raise this so slows and debuffs can never\n"
			  "drop you below it." },
			{ "Sprint Speed",         "sprintSpeed",       1.0f, 0.10f,  5.0f, 0.05f, "%.2fx",
			  "Multiplies sprint speed on top of Move Speed." },
			{ "Jump Height",          "jumpHeight",        1.0f, 0.10f,  8.0f, 0.05f, "%.2fx",
			  "Multiplies jump height/impulse." },
			{ "Double Jump Cost",     "doubleJumpCost",    1.0f, 0.00f,  3.0f, 0.05f, "%.2fx",
			  "Stamina cost multiplier for double jump. 0 makes it free." },
			{ "Dodge Cost",           "dodgeCost",         1.0f, 0.00f,  3.0f, 0.05f, "%.2fx",
			  "Stamina cost multiplier for dodge/dash. 0 makes it free." },
			{ "Stamina Regen",        "staminaRegen",      1.0f, 0.10f, 10.0f, 0.05f, "%.2fx",
			  "Stamina regeneration rate." },
			{ "Slide Stamina Regen",  "slideStaminaRegen", 1.0f, 0.10f, 10.0f, 0.05f, "%.2fx",
			  "Stamina regeneration rate while sliding." },
			{ "Fall Damage",          "fallDamage",        1.0f, 0.00f,  3.0f, 0.05f, "%.2fx",
			  "Fall damage taken multiplier. 0 removes fall damage entirely." },
			{ "Zipline Speed",        "ziplineSpeed",      1.0f, 0.10f,  5.0f, 0.05f, "%.2fx",
			  "Travel speed multiplier while riding a zipline." },
		};

		float g_attrValues[kAttrCount];
		bool  g_attrValuesInit = false;

		void EnsureAttrDefaults()
		{
			if (g_attrValuesInit) return;
			g_attrValuesInit = true;
			for (int a = 0; a < kAttrCount; ++a)
				g_attrValues[a] = kAttrs[a].defaultValue;
		}

		bool IsAttrActive(int attr)
		{
			return std::fabs(g_attrValues[attr] - kAttrs[attr].defaultValue) > kActiveEpsilon;
		}

		std::string AttrConfigKey(const char* leaf)
		{
			return std::string("playerMovement.attr.") + leaf;
		}

		void WriteAttribute(SDK::FGameplayAttributeData& attribute, float value)
		{
			attribute.BaseValue    = value;
			attribute.CurrentValue = value;
		}

		// The three attribute sets are different C++ classes, so a single
		// pointer-to-member array (as player_weapons.cpp uses for its one class)
		// can't span all of them without type erasure. Three small typed binding
		// tables instead -- still declarative, still a one-line add per attribute.
		using MoveAttr  = SDK::FGameplayAttributeData SDK::UCrMovementAttributeSet::*;
		using SpeedAttr = SDK::FGameplayAttributeData SDK::UCrMovementSpeedMultiplierAttributeSet::*;
		using GemAttr   = SDK::FGameplayAttributeData SDK::UCrGemAttributeSet::*;

		struct MoveBinding  { int attr; MoveAttr  member; };
		struct SpeedBinding { int attr; SpeedAttr member; };
		struct GemBinding   { int attr; GemAttr   member; };

		const MoveBinding kMoveBindings[] = {
			{ kAttrSprintSpeed, &SDK::UCrMovementAttributeSet::SprintSpeedMultiplier },
			{ kAttrJumpHeight,  &SDK::UCrMovementAttributeSet::JumpMultiplier },
		};

		const SpeedBinding kSpeedBindings[] = {
			{ kAttrMoveSpeed,    &SDK::UCrMovementSpeedMultiplierAttributeSet::CurrentMovementSpeedMultiplier },
			{ kAttrMoveSpeedMax, &SDK::UCrMovementSpeedMultiplierAttributeSet::MaxMovementSpeedMultiplier },
			{ kAttrMoveSpeedMin, &SDK::UCrMovementSpeedMultiplierAttributeSet::MinMovementSpeedMultiplier },
		};

		const GemBinding kGemBindings[] = {
			{ kAttrStaminaRegen,      &SDK::UCrGemAttributeSet::StaminaRegenMultiplier },
			{ kAttrSlideStaminaRegen, &SDK::UCrGemAttributeSet::SlideStaminaRegenMultiplier },
			{ kAttrDodgeCost,         &SDK::UCrGemAttributeSet::DodgeCostMultiplier },
			{ kAttrDoubleJumpCost,    &SDK::UCrGemAttributeSet::DoubleJumpCostMultiplier },
			{ kAttrFallDamage,        &SDK::UCrGemAttributeSet::FallDamageMultiplier },
			{ kAttrZiplineSpeed,      &SDK::UCrGemAttributeSet::ZiplineSpeedMultiplier },
		};

		// Same contract as SafeMultiplier, for the gameplay-attribute half.
		float SafeAttrValue(int attr)
		{
			float v = g_attrValues[attr];
			if (!(v == v))                 v = kAttrs[attr].defaultValue;   // NaN
			if (v < kAttrs[attr].minValue) v = kAttrs[attr].minValue;
			if (v > kAttrs[attr].maxValue) v = kAttrs[attr].maxValue;
			return v;
		}

		void ApplyAttributeOverrides(SDK::ACrCharacterPlayerBase* character)
		{
			try
			{
				if (SDK::UCrMovementAttributeSet* set = character->MovementAttributes)
					for (const MoveBinding& b : kMoveBindings)
						if (IsAttrActive(b.attr))
							WriteAttribute(set->*b.member, SafeAttrValue(b.attr));

				if (SDK::UCrMovementSpeedMultiplierAttributeSet* set = character->MovementSpeedMultiplierAttributes)
					for (const SpeedBinding& b : kSpeedBindings)
						if (IsAttrActive(b.attr))
							WriteAttribute(set->*b.member, SafeAttrValue(b.attr));

				if (SDK::UCrGemAttributeSet* set = character->GemAttributes)
					for (const GemBinding& b : kGemBindings)
						if (IsAttrActive(b.attr))
							WriteAttribute(set->*b.member, SafeAttrValue(b.attr));
			}
			catch (...) {}
		}

		// ---------------------------------------------------------------------
		// Presets. Values only -- applying one never introduces a new enable
		// state, it just writes into the same rows a player could set by hand.
		// ---------------------------------------------------------------------
		// =====================================================================
		// Raw movement fields.
		//
		// Everything above is a gameplay-attribute multiplier with a known neutral
		// default of 1.0. The fields below are plain engine/config floats living on the
		// player's OWN movement component and character actor -- per-player instances,
		// never a CDO, data asset or developer-settings singleton, so nothing here can
		// reach another player, another weapon, or the world.
		//
		// Their shipped values cannot be known from the headers (SprintMoveSpeed might
		// be 600, MaxDashDistance 900), so each one is captured ONCE as a baseline and
		// the UI edits a MULTIPLIER of it. That buys a lot: one row widget, one config
		// shape and one reset rule across the whole panel, and a saved multiplier stays
		// meaningful even if a game update retunes the number underneath it.
		//
		// The baseline is persisted per save. Re-capturing it while an override was live
		// would bake a cheated value in as "stock" and permanently destroy the reset
		// path -- and after a plugin hot-reload the component cannot tell the two apart,
		// which is exactly when it would happen.
		// =====================================================================
		enum RawIndex : int
		{
			kRawNormalSpeed = 0, kRawSprintSpeed, kRawCrouchSpeed, kRawADSSpeed,
			kRawAccel, kRawSprintRamp, kRawMaxAccel, kRawGroundFriction, kRawBrakingWalk,

			kRawJumpZ, kRawGravity, kRawAirControl, kRawCoyote, kRawStepHeight, kRawAirFriction,

			kRawDashDistance, kRawDashDuration, kRawDashCooldown, kRawDashEnergy, kRawAirDashes,

			kRawSlideMaxSpeed, kRawSlideImpulse, kRawSlideGravity, kRawSlideFriction,

			kRawZipSpeed, kRawZipAccel,

			kRawMaxEnergy, kRawRegenDelay, kRawSprintDrain, kRawRegenRate,
			kRawJumpEnergy, kRawDoubleJumpEnergy,
			kRawCount
		};

		struct RawDef
		{
			const char* label;
			const char* key;
			float       minValue;
			float       maxValue;
			const char* tooltip;
		};

		const RawDef kRaws[kRawCount] = {
			{ "Walk",              "normalSpeed",   0.10f, 10.0f, "Base ground speed. The sprint state machine feeds this into the\nmovement component every frame, so this is the value worth changing\nrather than fighting MaxWalkSpeed directly." },
			{ "Sprint",            "sprintSpeed",   0.10f, 10.0f, "Speed while sprinting." },
			{ "Crouch",            "crouchSpeed",   0.10f, 10.0f, "Speed while crouched." },
			{ "Aiming",            "adsSpeed",      0.10f, 10.0f, "Speed while aiming down sights." },
			{ "Acceleration",      "accel",         0.10f, 20.0f, "How fast you reach top speed and how fast you stop." },
			{ "Sprint Ramp Time",  "sprintRamp",    0.00f,  5.0f, "Time spent accelerating into a sprint. 0 makes sprint instant." },
			{ "Max Acceleration",  "maxAccel",      0.10f, 20.0f, "Movement component acceleration ceiling." },
			{ "Ground Friction",   "groundFric",    0.00f,  5.0f, "Grip. Lower is slidier; 0 is ice." },
			{ "Braking (ground)",  "brakeWalk",     0.00f,  5.0f, "Deceleration when you stop walking. 0 means you coast." },

			{ "Jump Velocity",     "jumpZ",         0.10f, 10.0f, "Upward impulse of a jump." },
			{ "Gravity Scale",     "gravity",       0.00f,  5.0f, "Gravity applied to you only. 0 is weightless; below 1 is moon-like." },
			{ "Air Control",       "airControl",    0.00f, 20.0f, "How much steering you have mid-air." },
			{ "Coyote Time",       "coyote",        0.00f, 20.0f, "Grace period to still jump after walking off an edge." },
			{ "Step Height",       "stepHeight",    0.10f, 20.0f, "How tall a ledge you walk up without jumping." },
			{ "Air Friction",      "airFriction",   0.00f,  5.0f, "Lateral friction while falling." },

			{ "Dash Distance",     "dashDist",      0.10f, 10.0f, "How far a dash carries you." },
			{ "Dash Duration",     "dashTime",      0.10f,  5.0f, "How long the dash takes. Shorter with the same distance is faster." },
			{ "Dash Cooldown",     "dashCd",        0.00f,  3.0f, "Delay between dashes. 0 removes the cooldown." },
			{ "Dash Energy Cost",  "dashEnergy",    0.00f,  3.0f, "Energy a dash costs. 0 makes it free." },
			{ "Max Air Dashes",    "airDashes",     1.00f, 20.0f, "How many dashes you get before touching the ground again." },

			{ "Max Slide Speed",   "slideMax",      0.10f, 10.0f, "Top speed a slide can reach." },
			{ "Slide Impulse",     "slideImpulse",  0.10f, 10.0f, "Kick you get entering a slide." },
			{ "Slide Gravity",     "slideGravity",  0.00f,  5.0f, "Downhill pull while sliding." },
			{ "Slide Friction",    "slideFric",     0.00f,  5.0f, "Slide drag. 0 means a slide never scrubs speed." },

			{ "Base Speed",        "zipSpeed",      0.10f, 10.0f, "Travel speed on a zipline (your rider, not the zipline itself)." },
			{ "Ramp Up",           "zipAccel",      0.10f, 10.0f, "How fast you get up to zipline speed." },

			{ "Max Energy",        "maxEnergy",     0.10f, 20.0f, "Size of the stamina pool. Sprint, jump and dash all draw from it." },
			{ "Regen Delay",       "regenDelay",    0.00f,  3.0f, "Pause before energy starts refilling. 0 regenerates immediately." },
			{ "Sprint Drain",      "sprintDrain",   0.00f,  3.0f, "Energy burned while sprinting. 0 makes sprinting free." },
			{ "Regen Rate",        "regenRate",     0.10f, 20.0f, "How fast energy refills." },
			{ "Jump Energy Cost",  "jumpEnergy",    0.00f,  3.0f, "Energy a jump costs. 0 makes it free." },
			{ "Double Jump Energy","dblJumpEnergy", 0.00f,  3.0f, "Energy a double jump costs. 0 makes it free." },
		};

		struct RawGroup { const char* title; const char* tableId; int first; int count; };
		const RawGroup kRawGroups[] = {
			{ "Ground Speed", "##raw_ground", kRawNormalSpeed,   9 },
			{ "Jump & Air",   "##raw_air",    kRawJumpZ,         6 },
			{ "Dash",         "##raw_dash",   kRawDashDistance,  5 },
			{ "Slide",        "##raw_slide",  kRawSlideMaxSpeed, 4 },
			{ "Zipline",      "##raw_zip",    kRawZipSpeed,      2 },
			{ "Energy",       "##raw_energy", kRawMaxEnergy,     6 },
		};
		constexpr int kRawGroupCount = static_cast<int>(sizeof(kRawGroups) / sizeof(kRawGroups[0]));

		// Reset is queued rather than written from the UI: the panel renders on the
		// render thread and every SDK write in this module happens from Tick.
		bool  g_rawRestoreWanted[kRawCount] = {};

		float g_rawValues[kRawCount];      // the multiplier the player edits
		float g_rawBaseline[kRawCount];    // the game's shipped value, captured once
		bool  g_rawValuesInit    = false;
		bool  g_rawBaselineReady = false;

		void EnsureRawDefaults()
		{
			if (g_rawValuesInit) return;
			g_rawValuesInit = true;
			for (int r = 0; r < kRawCount; ++r)
			{
				g_rawValues[r]   = 1.0f;
				g_rawBaseline[r] = 0.0f;
			}
		}

		bool IsRawActive(int raw)
		{
			return g_rawBaselineReady && g_rawValuesInit
			    && std::fabs(g_rawValues[raw] - 1.0f) > kActiveEpsilon;
		}

		std::string RawConfigKey(const char* leaf)
		{
			return std::string("playerMovement.raw.") + leaf;
		}

		std::string RawBaselineKey(const char* leaf)
		{
			return std::string("playerMovement.baseline.") + leaf;
		}

		// Pointer-to-member tables. Two owners, both per-player instances.
		using CompF = float SDK::UCrCharacterMovementComponent::*;
		using CharF = float SDK::ACrCharacterPlayerBase::*;

		struct CompBind { int raw; CompF member; };
		struct CharBind { int raw; CharF member; };

		const CompBind kCompBinds[] = {
			{ kRawMaxAccel,       &SDK::UCrCharacterMovementComponent::MaxAcceleration },
			{ kRawGroundFriction, &SDK::UCrCharacterMovementComponent::GroundFriction },
			{ kRawBrakingWalk,    &SDK::UCrCharacterMovementComponent::BrakingDecelerationWalking },
			{ kRawJumpZ,          &SDK::UCrCharacterMovementComponent::JumpZVelocity },
			{ kRawGravity,        &SDK::UCrCharacterMovementComponent::GravityScale },
			{ kRawAirControl,     &SDK::UCrCharacterMovementComponent::AirControl },
			{ kRawStepHeight,     &SDK::UCrCharacterMovementComponent::MaxStepHeight },
			{ kRawAirFriction,    &SDK::UCrCharacterMovementComponent::FallingLateralFriction },
			{ kRawCoyote,         &SDK::UCrCharacterMovementComponent::CoyoteTime },
			{ kRawDashDistance,   &SDK::UCrCharacterMovementComponent::MaxDashDistance },
			{ kRawDashDuration,   &SDK::UCrCharacterMovementComponent::DashDuration },
			{ kRawDashCooldown,   &SDK::UCrCharacterMovementComponent::DashCooldownSeconds },
			{ kRawDashEnergy,     &SDK::UCrCharacterMovementComponent::DashEnergyCostBase },
			{ kRawSlideMaxSpeed,  &SDK::UCrCharacterMovementComponent::MaxSlideSpeed },
			{ kRawSlideImpulse,   &SDK::UCrCharacterMovementComponent::SlideEnterImpulse },
			{ kRawSlideGravity,   &SDK::UCrCharacterMovementComponent::SlideGravityForce },
			{ kRawSlideFriction,  &SDK::UCrCharacterMovementComponent::SlideFrictionFactor },
			{ kRawZipSpeed,       &SDK::UCrCharacterMovementComponent::DefaultZiplineSpeed },
			{ kRawZipAccel,       &SDK::UCrCharacterMovementComponent::DefaultZiplineAcceleration },
			{ kRawJumpEnergy,     &SDK::UCrCharacterMovementComponent::JumpEnergyCost },
		};
		constexpr int kCompBindCount = static_cast<int>(sizeof(kCompBinds) / sizeof(kCompBinds[0]));

		const CharBind kCharBinds[] = {
			{ kRawNormalSpeed,      &SDK::ACrCharacterPlayerBase::NormalMoveSpeed },
			{ kRawSprintSpeed,      &SDK::ACrCharacterPlayerBase::SprintMoveSpeed },
			{ kRawCrouchSpeed,      &SDK::ACrCharacterPlayerBase::CrouchedMoveSpeed },
			{ kRawADSSpeed,         &SDK::ACrCharacterPlayerBase::ADSMoveSpeed },
			{ kRawAccel,            &SDK::ACrCharacterPlayerBase::AccelerationAndDeceleration },
			{ kRawSprintRamp,       &SDK::ACrCharacterPlayerBase::DurationOfAccelerationToSprint },
			{ kRawSprintDrain,      &SDK::ACrCharacterPlayerBase::EnergyLoweringMultiplierDuringSprint },
			{ kRawRegenRate,        &SDK::ACrCharacterPlayerBase::NormalEnergyRestorationRate },
			{ kRawDoubleJumpEnergy, &SDK::ACrCharacterPlayerBase::DoubleJumpEnergyUsage },
		};
		constexpr int kCharBindCount = static_cast<int>(sizeof(kCharBinds) / sizeof(kCharBinds[0]));

		// Captures every field's shipped value exactly once, then persists it so a
		// hot-reload restores the baseline instead of re-reading an already-cheated
		// component. See the header comment -- this is the one thing that must not go
		// wrong, because a bad baseline makes reset permanently wrong.
		void CaptureRawBaseline(SDK::ACrCharacterPlayerBase* character)
		{
			if (g_rawBaselineReady) return;

			SDK::UCrCharacterMovementComponent* move = character->CrCharacterMovementComponent;
			if (!move) return;

			for (int i = 0; i < kCompBindCount; ++i)
				g_rawBaseline[kCompBinds[i].raw] = move->*kCompBinds[i].member;

			for (int i = 0; i < kCharBindCount; ++i)
				g_rawBaseline[kCharBinds[i].raw] = character->*kCharBinds[i].member;

			g_rawBaseline[kRawAirDashes] = static_cast<float>(move->MaxAirDashesToExecute);

			if (SDK::UCrEnergyAttributeSet* energy = character->EnergyAttributes)
				g_rawBaseline[kRawMaxEnergy] = energy->MaxEnergy.BaseValue;

			if (SDK::UCrEnergyLogicComponent* logic = character->EnergyLogicComponent)
				g_rawBaseline[kRawRegenDelay] = logic->DelayBeforeEnergyRegeneration;

			g_rawBaselineReady = true;

			for (int r = 0; r < kRawCount; ++r)
				SessionConfig::Set(RawBaselineKey(kRaws[r].key), g_rawBaseline[r]);
			SessionConfig::Set("playerMovement.baselineCaptured", true);

			LOG_INFO("Movement: captured stock baseline for %d raw fields.", kRawCount);
		}

		// Nothing reaches a game field without passing through here.
		//
		// A row declares its own safe range, and the movement-critical ones (the gait
		// speeds and acceleration) declare a MINIMUM above zero -- so no combination of
		// stale config, a corrupt value or a bad edit can multiply the player's speed by
		// zero and strand them. Rows where zero is a legitimate setting (gravity, dash
		// cooldown, sprint drain) declare a minimum of 0 and keep it.
		float SafeMultiplier(int raw)
		{
			float m = g_rawValues[raw];
			if (!(m == m))                   m = 1.0f;   // NaN
			if (m < kRaws[raw].minValue)     m = kRaws[raw].minValue;
			if (m > kRaws[raw].maxValue)     m = kRaws[raw].maxValue;
			return m;
		}

		void ApplyRawOverrides(SDK::ACrCharacterPlayerBase* character)
		{
			// g_rawValuesInit matters as much as the baseline: a zero-initialised
			// multiplier table reads as "every row active" and multiplies the player's
			// speed by 0.0, which looks exactly like the game freezing.
			if (!g_rawBaselineReady || !g_rawValuesInit) return;

			SDK::UCrCharacterMovementComponent* move = character->CrCharacterMovementComponent;
			if (!move) return;

			for (int i = 0; i < kCompBindCount; ++i)
			{
				const int r = kCompBinds[i].raw;
				if (IsRawActive(r))
					move->*kCompBinds[i].member = g_rawBaseline[r] * SafeMultiplier(r);
			}

			for (int i = 0; i < kCharBindCount; ++i)
			{
				const int r = kCharBinds[i].raw;
				if (IsRawActive(r))
					character->*kCharBinds[i].member = g_rawBaseline[r] * SafeMultiplier(r);
			}

			if (IsRawActive(kRawAirDashes))
			{
				const float want = g_rawBaseline[kRawAirDashes] * SafeMultiplier(kRawAirDashes);
				move->MaxAirDashesToExecute = static_cast<int>(want + 0.5f);
			}

			if (IsRawActive(kRawMaxEnergy))
				if (SDK::UCrEnergyAttributeSet* energy = character->EnergyAttributes)
					WriteAttribute(energy->MaxEnergy, g_rawBaseline[kRawMaxEnergy] * SafeMultiplier(kRawMaxEnergy));

			if (IsRawActive(kRawRegenDelay))
				if (SDK::UCrEnergyLogicComponent* logic = character->EnergyLogicComponent)
					logic->DelayBeforeEnergyRegeneration =
						g_rawBaseline[kRawRegenDelay] * SafeMultiplier(kRawRegenDelay);
		}

		// Reset writes the captured baseline back once, because leaving the multiplier
		// at 1.0 only stops us overwriting the field -- it does not undo the last value
		// we wrote into it.
		void RestoreRawField(SDK::ACrCharacterPlayerBase* character, int raw)
		{
			if (!character || !g_rawBaselineReady) return;

			SDK::UCrCharacterMovementComponent* move = character->CrCharacterMovementComponent;
			if (!move) return;

			for (int i = 0; i < kCompBindCount; ++i)
				if (kCompBinds[i].raw == raw) { move->*kCompBinds[i].member = g_rawBaseline[raw]; return; }

			for (int i = 0; i < kCharBindCount; ++i)
				if (kCharBinds[i].raw == raw) { character->*kCharBinds[i].member = g_rawBaseline[raw]; return; }

			if (raw == kRawAirDashes)
				move->MaxAirDashesToExecute = static_cast<int>(g_rawBaseline[raw] + 0.5f);
			else if (raw == kRawMaxEnergy)
			{
				if (SDK::UCrEnergyAttributeSet* energy = character->EnergyAttributes)
					WriteAttribute(energy->MaxEnergy, g_rawBaseline[raw]);
			}
			else if (raw == kRawRegenDelay)
			{
				if (SDK::UCrEnergyLogicComponent* logic = character->EnergyLogicComponent)
					logic->DelayBeforeEnergyRegeneration = g_rawBaseline[raw];
			}
		}

		struct PresetVal { int attr; float value; };
		struct PresetRaw { int raw;  float value; };

		struct Preset
		{
			const char* label;
			const char* tooltip;
			const char* credit;   // original mod this is modelled on, or null
			PresetVal   vals[6];
			PresetRaw   raws[8];  // component/actor fields, same multiplier model
		};

		const Preset kPresets[] = {
			{ "My Player Enhancements",
			  "Speed, jump, dash and slide with faster stamina regen -- a moderate\n"
			  "all-round mobility boost.",
			  "Recreates the pre-Update-2 pak mod 'My Player Enhancements'",
			  { {kAttrMoveSpeed,1.25f},{kAttrSprintSpeed,1.25f},{kAttrJumpHeight,1.35f},
			    {kAttrDodgeCost,0.50f},{kAttrStaminaRegen,1.75f},{kAttrSlideStaminaRegen,1.75f} },
			  { {kRawNormalSpeed,1.25f},{kRawSprintSpeed,1.25f},{kRawJumpZ,1.35f},
			    {kRawDashCooldown,0.60f},{kRawSlideMaxSpeed,1.25f},{kRawRegenRate,1.75f},{-1,0.0f} } },

			{ "Sonic Mode",
			  "Very high move, sprint and zipline speed. Raises the speed cap too, so\n"
			  "the override actually sticks instead of getting clamped back down.",
			  nullptr,
			  { {kAttrMoveSpeed,5.00f},{kAttrMoveSpeedMax,5.00f},{kAttrSprintSpeed,5.00f},
			    {kAttrZiplineSpeed,3.00f},{-1,0.0f} },
			  { {kRawNormalSpeed,5.00f},{kRawSprintSpeed,5.00f},{kRawCrouchSpeed,3.00f},
			    {kRawAccel,3.00f},{kRawMaxAccel,3.00f},{kRawZipSpeed,3.00f},
			    {kRawSprintRamp,0.10f},{-1,0.0f} } },

			{ "Moon Hop",
			  "Low gravity for real -- long floaty jumps, full air control and no fall\n"
			  "damage, with near-free double jumps.",
			  nullptr,
			  { {kAttrFallDamage,0.00f},{kAttrJumpHeight,4.00f},{kAttrDoubleJumpCost,0.05f},{-1,0.0f} },
			  { {kRawGravity,0.20f},{kRawJumpZ,2.50f},{kRawAirControl,4.00f},
			    {kRawAirFriction,0.30f},{kRawDoubleJumpEnergy,0.05f},{-1,0.0f} } },

			{ "Endless Momentum",
			  "Near-free dodges and double jumps backed by near-instant stamina regen --\n"
			  "spam both without ever running dry.",
			  nullptr,
			  { {kAttrDodgeCost,0.05f},{kAttrDoubleJumpCost,0.05f},
			    {kAttrStaminaRegen,8.00f},{kAttrSlideStaminaRegen,8.00f},{-1,0.0f} },
			  { {kRawDashEnergy,0.05f},{kRawDashCooldown,0.00f},{kRawJumpEnergy,0.05f},
			    {kRawDoubleJumpEnergy,0.05f},{kRawRegenRate,8.00f},{kRawRegenDelay,0.00f},
			    {kRawSprintDrain,0.00f},{-1,0.0f} } },
		};
		constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

		void ResetAllAttrs()
		{
			for (int a = 0; a < kAttrCount; ++a)
			{
				g_attrValues[a] = kAttrs[a].defaultValue;
				SessionConfig::Set(AttrConfigKey(kAttrs[a].key), kAttrs[a].defaultValue);
			}
			for (int r = 0; r < kRawCount; ++r)
			{
				g_rawValues[r] = 1.0f;
				SessionConfig::Set(RawConfigKey(kRaws[r].key), 1.0f);
				g_rawRestoreWanted[r] = true;   // put the stock value back, not just stop writing
			}
		}

		// Changing the overall Move Speed carries the individual gait speeds with it.
		// They are what the game actually reads, so a master multiplier that left them
		// behind would look applied and do nothing. Each one stays editable afterwards.
		void CascadeMoveSpeed(float value)
		{
			static const int kGaits[] = { kRawNormalSpeed, kRawSprintSpeed, kRawCrouchSpeed, kRawADSSpeed };
			for (int g : kGaits)
			{
				float v = value;
				if (v < kRaws[g].minValue) v = kRaws[g].minValue;
				if (v > kRaws[g].maxValue) v = kRaws[g].maxValue;
				g_rawValues[g] = v;
				SessionConfig::Set(RawConfigKey(kRaws[g].key), v);
			}
		}

		void ApplyPreset(const Preset& preset)
		{
			for (const PresetVal& v : preset.vals)
			{
				if (v.attr < 0) break;
				g_attrValues[v.attr] = v.value;
				SessionConfig::Set(AttrConfigKey(kAttrs[v.attr].key), v.value);
			}
			for (const PresetRaw& r : preset.raws)
			{
				if (r.raw < 0) break;
				g_rawValues[r.raw] = r.value;
				SessionConfig::Set(RawConfigKey(kRaws[r.raw].key), r.value);
			}
			LOG_INFO("Movement: applied preset '%s'.", preset.label);
		}

		// One row: label (dimmed at default) + joined slider/box + drawn reset
		// button. Copied from player_weapons.cpp, including the number-box width
		// computed from CalcTextSize rather than a fixed pixel constant -- fixed
		// widths clip under this user's FontScale 1.50.
		// Draws the label cell. A row with advanced detail underneath gets a disclosure
		// arrow; rows without one are padded by the same width so every label in the
		// group still lines up. Children are indented one step.
		void DrawRowLabel(IModLoaderImGui* imgui, const char* label, const char* tooltip,
		                  bool active, int depth, bool* openFlag)
		{
			const float frameH = imgui->GetFrameHeight();

			if (depth > 0)
				imgui->Indent(frameH * static_cast<float>(depth));

			if (openFlag)
			{
				if (imgui->ArrowButton("##expand", *openFlag ? 3 : 1))   // 3 = Down, 1 = Right
					*openFlag = !*openFlag;
				imgui->SameLine(0.0f, -1.0f);
			}
			else
			{
				imgui->Indent(frameH);   // keep labels aligned with the arrowed rows
			}

			if (active) imgui->Text(label);
			else        imgui->TextDisabled(label);
			if (tooltip && imgui->IsItemHovered())
				imgui->SetTooltip(tooltip);

			if (!openFlag)
				imgui->Unindent(frameH);

			if (depth > 0)
				imgui->Unindent(frameH * static_cast<float>(depth));
		}

		void RenderAttrRow(IModLoaderImGui* imgui, int attr, int depth = 0, bool* openFlag = nullptr)
		{
			const AttrDef& def   = kAttrs[attr];
			float&         value = g_attrValues[attr];
			const bool     active = IsAttrActive(attr);

			imgui->PushIDInt(attr);
			imgui->TableNextRow(0, 0.0f);

			imgui->TableSetColumnIndex(0);
			DrawRowLabel(imgui, def.label, def.tooltip, active, depth, openFlag);

			imgui->TableSetColumnIndex(1);

			float availX = 0.0f, availY = 0.0f;
			imgui->GetContentRegionAvail(&availX, &availY);

			char widest[32];
			snprintf(widest, sizeof(widest), def.format,
				(def.maxValue >= 100.0f) ? -888.0f : -88.88f);
			float textW = 0.0f, textH = 0.0f;
			imgui->CalcTextSize(widest, &textW, &textH, false, -1.0f);

			const float frameH  = imgui->GetFrameHeight();
			const float numBoxW = textW + (frameH * 2.0f) + (frameH * 0.9f); // text + 2 steppers + padding
			const float sliderW = (availX > numBoxW + frameH * 2.0f)
				? (availX - numBoxW)
				: (availX * 0.55f);

			bool changed = false;
			imgui->SetNextItemWidth(sliderW);
			if (imgui->SliderFloat("##slider", &value, def.minValue, def.maxValue, def.format))
				changed = true;

			imgui->SameLine(0.0f, 0.0f);
			imgui->SetNextItemWidth(-1.0f);
			if (imgui->InputFloat("##num", &value, def.step, def.step * 10.0f, def.format))
				changed = true;

			if (changed)
			{
				if (value < def.minValue) value = def.minValue;
				if (value > def.maxValue) value = def.maxValue;
				SessionConfig::Set(AttrConfigKey(def.key), value);

				if (attr == kAttrMoveSpeed)
					CascadeMoveSpeed(value);
			}

			imgui->TableSetColumnIndex(2);
			if (BetterCheats::UI::ResetButton(imgui, "##reset"))
			{
				value = def.defaultValue;
				SessionConfig::Set(AttrConfigKey(def.key), value);

				if (attr == kAttrMoveSpeed)
					CascadeMoveSpeed(value);
			}
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Reset to the game default (turns this row off).");

			imgui->PopID();
		}

		void RenderAttrGroup(IModLoaderImGui* imgui, const char* tableId, int first, int count)
		{
			if (!imgui->BeginTable(tableId, 3, kTableFlags))
				return;

			imgui->TableSetupColumn("Attribute", 0, 0.36f);
			imgui->TableSetupColumn("Value",     0, 0.54f);
			imgui->TableSetupColumn("",          0, 0.10f);

			for (int a = first; a < first + count; ++a)
				RenderAttrRow(imgui, a);

			imgui->EndTable();
		}

		void RenderRawRow(IModLoaderImGui* imgui, int raw, int depth = 1, bool* openFlag = nullptr)
		{
			const RawDef& def   = kRaws[raw];
			float&        value = g_rawValues[raw];
			const bool    active = IsRawActive(raw);

			char tip[512];
			if (g_rawBaselineReady)
				snprintf(tip, sizeof(tip), "%s\n\nStock value: %.3f\nApplied: %.3f",
					def.tooltip, g_rawBaseline[raw], g_rawBaseline[raw] * value);
			else
				snprintf(tip, sizeof(tip), "%s\n\n(Stock value not read yet - load a save to enable.)",
					def.tooltip);

			imgui->PushIDInt(1000 + raw);
			imgui->TableNextRow(0, 0.0f);

			imgui->TableSetColumnIndex(0);
			DrawRowLabel(imgui, def.label, tip, active, depth, openFlag);

			imgui->TableSetColumnIndex(1);
			imgui->BeginDisabled(!g_rawBaselineReady);

			float availX = 0.0f, availY = 0.0f;
			imgui->GetContentRegionAvail(&availX, &availY);

			float textW = 0.0f, textH = 0.0f;
			imgui->CalcTextSize("-88.88x", &textW, &textH, false, -1.0f);

			const float frameH  = imgui->GetFrameHeight();
			const float numBoxW = textW + (frameH * 2.0f) + (frameH * 0.9f);
			const float sliderW = (availX > numBoxW + frameH * 2.0f)
				? (availX - numBoxW)
				: (availX * 0.55f);

			bool changed = false;
			imgui->SetNextItemWidth(sliderW);
			if (imgui->SliderFloat("##slider", &value, def.minValue, def.maxValue, "%.2fx"))
				changed = true;

			imgui->SameLine(0.0f, 0.0f);
			imgui->SetNextItemWidth(-1.0f);
			if (imgui->InputFloat("##num", &value, 0.05f, 0.5f, "%.2fx"))
				changed = true;

			if (changed)
			{
				if (value < def.minValue) value = def.minValue;
				if (value > def.maxValue) value = def.maxValue;
				SessionConfig::Set(RawConfigKey(def.key), value);
			}
			imgui->EndDisabled();

			imgui->TableSetColumnIndex(2);
			if (BetterCheats::UI::ResetButton(imgui, "##reset"))
			{
				value = 1.0f;
				SessionConfig::Set(RawConfigKey(def.key), value);
				g_rawRestoreWanted[raw] = true;
			}
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Put the game's stock value back (turns this row off).");

			imgui->PopID();
		}

		// ---------------------------------------------------------------------
		// Tuning rows.
		//
		// The original three groups are kept. Where a headline control has finer
		// detail behind it, that control carries a disclosure arrow and its advanced
		// rows sit indented underneath -- so Move Speed is one row until you ask it
		// for the individual gaits, friction and acceleration.
		// ---------------------------------------------------------------------
		enum RowKind : int { RowAttr = 0, RowRaw = 1 };

		enum OpenId : int
		{
			kOpenMoveSpeed = 0, kOpenAccel, kOpenJump, kOpenDoubleJump, kOpenDodge,
			kOpenStamina, kOpenSlide, kOpenZipline, kOpenCount
		};
		bool g_rowOpen[kOpenCount] = {};

		struct ChildRef { int kind; int index; };
		struct TuneRow  { int kind; int index; const ChildRef* kids; int kidCount; int openId; };
		struct TuneGroup{ const char* title; const char* tableId; const TuneRow* rows; int count; };

		// Children sit with the control they are bound to, not with the system that
		// happens to own the underlying field. An action's energy cost belongs with the
		// action (you tune a dash as one thing); only the shared pool lives under stamina.
		// Air control and air friction go with dash because that is what they change the
		// feel of in practice.
		const ChildRef kKidsMoveSpeed[] = {
			{ RowRaw, kRawNormalSpeed }, { RowRaw, kRawSprintSpeed },
			{ RowRaw, kRawCrouchSpeed }, { RowRaw, kRawADSSpeed },
		};
		const ChildRef kKidsAccel[] = {
			{ RowRaw, kRawMaxAccel }, { RowRaw, kRawSprintRamp },
			{ RowRaw, kRawGroundFriction }, { RowRaw, kRawBrakingWalk },
		};
		const ChildRef kKidsJump[] = {
			{ RowRaw, kRawJumpZ }, { RowRaw, kRawGravity },
			{ RowRaw, kRawCoyote }, { RowRaw, kRawStepHeight },
			{ RowRaw, kRawJumpEnergy },
		};
		const ChildRef kKidsDoubleJump[] = {
			{ RowRaw, kRawDoubleJumpEnergy },
		};
		const ChildRef kKidsDodge[] = {
			{ RowRaw, kRawDashDistance }, { RowRaw, kRawDashDuration },
			{ RowRaw, kRawDashCooldown }, { RowRaw, kRawDashEnergy },
			{ RowRaw, kRawAirDashes },
			{ RowRaw, kRawAirControl }, { RowRaw, kRawAirFriction },
		};
		const ChildRef kKidsStamina[] = {
			{ RowRaw, kRawRegenRate }, { RowRaw, kRawRegenDelay },
			{ RowRaw, kRawSprintDrain }, { RowRaw, kRawMaxEnergy },
		};
		const ChildRef kKidsSlide[] = {
			{ RowRaw, kRawSlideMaxSpeed }, { RowRaw, kRawSlideImpulse },
			{ RowRaw, kRawSlideGravity }, { RowRaw, kRawSlideFriction },
		};
		const ChildRef kKidsZipline[] = {
			{ RowRaw, kRawZipSpeed }, { RowRaw, kRawZipAccel },
		};

		#define KIDS(a) a, static_cast<int>(sizeof(a) / sizeof(a[0]))

		const TuneRow kRowsSpeed[] = {
			{ RowAttr, kAttrMoveSpeed,    KIDS(kKidsMoveSpeed), kOpenMoveSpeed },
			{ RowRaw,  kRawAccel,         KIDS(kKidsAccel),     kOpenAccel },
			{ RowAttr, kAttrMoveSpeedMax, nullptr, 0, -1 },
			{ RowAttr, kAttrMoveSpeedMin, nullptr, 0, -1 },
			{ RowAttr, kAttrSprintSpeed,  nullptr, 0, -1 },
		};

		const TuneRow kRowsJump[] = {
			{ RowAttr, kAttrJumpHeight,     KIDS(kKidsJump),       kOpenJump },
			{ RowAttr, kAttrDoubleJumpCost, KIDS(kKidsDoubleJump), kOpenDoubleJump },
			{ RowAttr, kAttrDodgeCost,      KIDS(kKidsDodge),      kOpenDodge },
		};

		const TuneRow kRowsStamina[] = {
			{ RowAttr, kAttrStaminaRegen,      KIDS(kKidsStamina), kOpenStamina },
			{ RowAttr, kAttrSlideStaminaRegen, KIDS(kKidsSlide),   kOpenSlide },
			{ RowAttr, kAttrZiplineSpeed,      KIDS(kKidsZipline), kOpenZipline },
			{ RowAttr, kAttrFallDamage,        nullptr, 0, -1 },
		};

		const TuneGroup kTuneGroups[] = {
			{ "Speed",              "##tg_speed",   KIDS(kRowsSpeed) },
			{ "Jump & Dodge",       "##tg_jump",    KIDS(kRowsJump) },
			{ "Stamina & Survival", "##tg_stamina", KIDS(kRowsStamina) },
		};
		#undef KIDS
		constexpr int kTuneGroupCount = static_cast<int>(sizeof(kTuneGroups) / sizeof(kTuneGroups[0]));

		void RenderTuneGroups(IModLoaderImGui* imgui)
		{
			if (!g_rawBaselineReady)
				imgui->TextDisabled("Advanced rows stay greyed until the game's stock values are read - load a save.");

			for (int g = 0; g < kTuneGroupCount; ++g)
			{
				const TuneGroup& grp = kTuneGroups[g];

				imgui->Spacing();
				imgui->SeparatorText(grp.title);
				imgui->Spacing();

				if (!imgui->BeginTable(grp.tableId, 3, kTableFlags))
					continue;

				imgui->TableSetupColumn("Attribute", 0, 0.36f);
				imgui->TableSetupColumn("Value",     0, 0.54f);
				imgui->TableSetupColumn("",          0, 0.10f);

				for (int r = 0; r < grp.count; ++r)
				{
					const TuneRow& row = grp.rows[r];
					bool* openFlag = (row.openId >= 0) ? &g_rowOpen[row.openId] : nullptr;

					if (row.kind == RowAttr) RenderAttrRow(imgui, row.index, 0, openFlag);
					else                     RenderRawRow(imgui, row.index, 0, openFlag);

					if (!openFlag || !*openFlag)
						continue;

					for (int k = 0; k < row.kidCount; ++k)
					{
						if (row.kids[k].kind == RowAttr) RenderAttrRow(imgui, row.kids[k].index, 1, nullptr);
						else                             RenderRawRow(imgui, row.kids[k].index, 1, nullptr);
					}
				}

				imgui->EndTable();
			}
		}

	}

	void Initialize()
	{
		IPluginSelf* self = GetSelf();
		if (!self || !self->hooks || !self->hooks->Input)
			return;

		{
			std::lock_guard<std::mutex> lock(g_keyMutex);
			snprintf(g_noClipKey, sizeof(g_noClipKey), "%s", BetterCheatsConfig::Config::GetNoClipKey());
		}

		char combo[64];
		ReadNoClipKey(combo, sizeof(combo));

		self->hooks->Input->RegisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
	}

	void Shutdown()
	{
		IPluginSelf* self = GetSelf();
		if (self && self->hooks && self->hooks->Input)
		{
			char combo[64];
			ReadNoClipKey(combo, sizeof(combo));

			if (combo[0])
				self->hooks->Input->UnregisterKeybindByName(combo, EModKeyEvent::Pressed, &OnNoClipKeyPressed);
		}

		// Leaving a character permanently non-colliding would outlive the plugin,
		// so put it back — but only if the pawn we changed is still the live one.
		try
		{
			if (g_active)
			{
				SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
				if (character && character == g_appliedTo)
					Disable(character);
			}
		}
		catch (...) {}

		g_active    = false;
		g_appliedTo = nullptr;
		g_noClipWanted.store(false);
	}

	void Tick(float /*deltaSeconds*/)
	{
		EnsureAttrDefaults();
		EnsureRawDefaults();

		try
		{
			SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();

			// Respawn or a world change replaces the pawn; the one we altered is
			// gone, so forget it rather than writing through a freed pointer.
			if (g_appliedTo && g_appliedTo != character)
			{
				g_active    = false;
				g_appliedTo = nullptr;
			}

			if (!character)
			{
				RefreshSnapshot(nullptr);
				return;
			}

			const bool wanted = g_noClipWanted.load();
			if (wanted && !g_active)
				Enable(character);
			else if (!wanted && g_active)
				Disable(character);

			if (g_active)
				MaintainNoClip(character);

			// Independent of no-clip -- these are gameplay-effect re-evaluated
			// attributes, so an active override has to be re-asserted every tick.
			ApplyAttributeOverrides(character);

			// Raw component/actor fields. Baseline first, and only ever once: it is
			// what reset restores, so capturing it while an override was live would
			// bake a cheated value in as stock.
			CaptureRawBaseline(character);

			for (int r = 0; r < kRawCount; ++r)
				if (g_rawRestoreWanted[r])
				{
					RestoreRawField(character, r);
					g_rawRestoreWanted[r] = false;
				}

			ApplyRawOverrides(character);

			RefreshSnapshot(character);
		}
		catch (...)
		{
			LOG_ERROR("Movement: exception while driving no-clip.");
		}
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		float speed = SessionConfig::Get("playerMovement.flySpeedMultiplier", kDefaultFlySpeedMultiplier);
		if (speed < kMinFlySpeedMultiplier) speed = kMinFlySpeedMultiplier;
		if (speed > kMaxFlySpeedMultiplier) speed = kMaxFlySpeedMultiplier;

		g_flySpeedMultiplier.store(speed);
		g_passThroughWalls.store(SessionConfig::Get("playerMovement.passThroughWalls", true));

		EnsureAttrDefaults();
		EnsureRawDefaults();
		for (int a = 0; a < kAttrCount; ++a)
		{
			float value = SessionConfig::Get(AttrConfigKey(kAttrs[a].key), kAttrs[a].defaultValue);
			if (value < kAttrs[a].minValue) value = kAttrs[a].minValue;
			if (value > kAttrs[a].maxValue) value = kAttrs[a].maxValue;
			g_attrValues[a] = value;
		}

		// Raw multipliers, plus the stock baseline they multiply. Restoring a persisted
		// baseline is what stops a plugin hot-reload re-reading an already-cheated
		// component and calling those values "stock".
		EnsureRawDefaults();
		const bool haveBaseline = SessionConfig::Get("playerMovement.baselineCaptured", false);
		for (int r = 0; r < kRawCount; ++r)
		{
			float value = SessionConfig::Get(RawConfigKey(kRaws[r].key), 1.0f);
			if (value < kRaws[r].minValue) value = kRaws[r].minValue;
			if (value > kRaws[r].maxValue) value = kRaws[r].maxValue;
			g_rawValues[r] = value;

			g_rawBaseline[r] = SessionConfig::Get(RawBaselineKey(kRaws[r].key), 0.0f);
		}
		g_rawBaselineReady = haveBaseline;

		LOG_INFO("Movement: applied saved config for session '%s' (baseline %s).",
			SessionConfig::GetSessionName().c_str(), haveBaseline ? "restored" : "not yet captured");
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		EnsureAttrDefaults();
		EnsureRawDefaults();

		MovementSnapshot snap;
		{
			std::lock_guard<std::mutex> lock(g_snapshotMutex);
			snap = g_snapshot;
		}

		imgui->SeparatorText("Fly / No-Clip Movement");

		if (!snap.characterFound)
		{
			imgui->TextDisabled("Player character not found -- load into a game session.");
			return;
		}

		char currentKey[64];
		ReadNoClipKey(currentKey, sizeof(currentKey));

		bool  noClip = g_noClipWanted.load();
		bool  ghost  = g_passThroughWalls.load();
		float speed  = g_flySpeedMultiplier.load();

		if (imgui->BeginTable("##noclip_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option", 0,            0.45f);
			imgui->TableSetupColumn("Value",  0,            0.55f);

			// Row 1: Fly Mode toggle + hotkey picker
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Fly Mode Toggle:");
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Toggle vertical flight mode. Ascend with Space, descend with Left Ctrl.");

			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("Active##noclip", &noClip))
				g_noClipWanted.store(noClip);

			imgui->SameLine(0.0f, 12.0f);
			imgui->Text("Hotkey:");
			imgui->SameLine(0.0f, 6.0f);
			char pickedKey[64];
			if (Keybind::RenderPicker(imgui, "noclip_key_picker", currentKey, pickedKey, sizeof(pickedKey)))
				ApplyNoClipKey(pickedKey);

			imgui->SameLine(0.0f, 6.0f);
			if (imgui->SmallButton("Reset##key"))
			{
				Keybind::CancelCapture();
				ApplyNoClipKey(BetterCheatsConfig::kDefaultNoClipKey);
			}

			// Row 2: Pass through walls (clipping)
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Pass Through Walls (Clipping)");
			if (imgui->IsItemHovered())
				imgui->SetTooltip("On: Disable capsule collision to fly through terrain & walls.\n"
				                  "Off: Retain collision so you can fly around without clipping through floors.");

			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("Enabled##passwalls", &ghost))
			{
				g_passThroughWalls.store(ghost);
				SessionConfig::Set("playerMovement.passThroughWalls", ghost);
			}

			// Row 3: Fly Speed
			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Fly Speed Multiplier");
			imgui->TableSetColumnIndex(1);
			imgui->SetNextItemWidth(-1.0f);
			if (imgui->SliderFloat("##fly_speed", &speed, kMinFlySpeedMultiplier, kMaxFlySpeedMultiplier, "%.1fx"))
			{
				g_flySpeedMultiplier.store(speed);
				SessionConfig::Set("playerMovement.flySpeedMultiplier", speed);
			}

			imgui->EndTable();
		}

		imgui->Spacing();

		// Flashing Warning Banner when clipping is enabled
		if (ghost)
		{
			ULONGLONG ticks = GetTickCount64();
			bool flash = ((ticks / 350) % 2) == 0;
			imgui->Spacing();
			if (flash)
			{
				imgui->TextColored(1.0f, 0.25f, 0.25f, 1.0f, "[!] WARNING: CLIPPING / PASS THROUGH WALLS IS ACTIVE!");
				imgui->TextColored(1.0f, 0.85f, 0.2f, 1.0f, "Disabling Fly mode while below ground geometry will cause you to fall through the world!");
			}
			else
			{
				imgui->TextColored(1.0f, 0.65f, 0.0f, 1.0f, "[!] WARNING: CLIPPING / PASS THROUGH WALLS IS ACTIVE!");
				imgui->TextColored(0.95f, 0.95f, 0.4f, 1.0f, "Disabling Fly mode while below ground geometry will cause you to fall through the world!");
			}
		}
		else
		{
			imgui->Spacing();
			imgui->TextDisabled("Collision enabled: Safe from falling through terrain when Fly is toggled off.");
		}

		imgui->Spacing();
		char buffer[220];
		float topCms = snap.baseFlySpeed * speed;
		float topKmh = topCms * 0.036f;
		float topMph = topCms * 0.0223693629f;
		snprintf(buffer, sizeof(buffer), "Top speed: %.1f km/h  (%.1f mph | %.0f cm/s)", topKmh, topMph, topCms);
		imgui->TextDisabled(buffer);

		imgui->Spacing();
		imgui->TextWrapped("Look and move as usual to fly horizontally; hold Space to rise and Left Ctrl to descend. Vertical input is ignored while this menu is open.");

		// ---- attribute presets -------------------------------------------------
		imgui->Spacing();
		imgui->SeparatorText("Presets");
		imgui->TextDisabled("Presets:");
		for (int i = 0; i < kPresetCount; ++i)
		{
			const Preset& preset = kPresets[i];

			imgui->SameLine(0.0f, -1.0f);
			if (imgui->SmallButton(preset.label))
				ApplyPreset(preset);
			if (imgui->IsItemHovered())
			{
				char tip[512];
				snprintf(tip, sizeof(tip), "%s%s%s",
					preset.tooltip,
					preset.credit ? "\n\n" : "",
					preset.credit ? preset.credit : "");
				imgui->SetTooltip(tip);
			}
		}

		// ---- tuning rows --------------------------------------------------------
		imgui->Spacing();
		imgui->TextDisabled("A value differing from the default is applied. Reset a row to turn it off.");
		imgui->TextDisabled("Rows with an arrow open up the finer controls behind them.");
		RenderTuneGroups(imgui);

		imgui->Spacing();
		if (imgui->SmallButton("Reset all movement attributes"))
			ResetAllAttrs();

		imgui->Spacing();
		imgui->Separator();
		imgui->TextDisabled("'My Player Enhancements' preset modelled on the pak mod of the same name that Update 2 broke.");
	}
}
