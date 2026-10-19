#include "player_weapons.h"
#include "plugin_helpers.h"
#include "session_config.h"
#include "aob_resolver.h"
#include "ui_widgets.h"
#include "attribute_compose.h"
#include "cheat_math.h"
#include "player_lookup.h"
#include "game_thread.h"
#include "object_ref.h"
#include "cdo_lookup.h"
#include "enemies.h"

#include "Chimera_classes.hpp"
#include "MassAIPrototypeEnemyRuntime_classes.hpp"
#include "Chimera_parameters.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace BetterCheats::Panels::Weapons
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kWeaponsTableFlags = (1 << 6) | (1 << 9) | (3 << 13);

		// Damage multiplier used by "One Hit Kill". Mirrors the flat 10000 the mining
		// laser hook uses in player_tools.cpp.
		constexpr float kOneHitKillDamage = 10000.0f;

		// A value is applied only when it differs from the game default, so there is no
		// separate enable toggle to keep in sync -- resetting a row IS disabling it.
		// See cheat_math.h for kActiveEpsilon/DiffersFromDefault, shared with Movement.

		// FGameplayAttributeData on UCrWeaponAttributeSet, reached via
		// ACrCharacterPlayerBase::WeaponAttributes. The set belongs to the character and
		// applies to whatever is equipped, so per-weapon profiles work by writing the
		// equipped weapon's profile every tick.
		using WeaponAttr = SDK::FGameplayAttributeData SDK::UCrWeaponAttributeSet::*;

		struct AttrDef
		{
			const char* label;
			const char* key;
			WeaponAttr  member;
			float       defaultValue;
			float       minValue;
			float       maxValue;
			float       step;
			const char* format;
			const char* tooltip;
		};

		enum AttrIndex : int
		{
			kAttrDamage = 0, kAttrFireRate, kAttrReload, kAttrMagazine, kAttrFalloff,
			kAttrRecoil, kAttrSpread, kAttrSway, kAttrADS, kAttrPierce, kAttrCount
		};

		const AttrDef kAttrs[kAttrCount] = {
			{ "Damage",             "damage",   &SDK::UCrWeaponAttributeSet::DamageModMultiplier,
			  1.0f, 0.10f,  25.0f, 0.05f, "%.2fx", "Multiplies the weapon's base damage. Attachment and upgrade bonuses\nstill add on top." },
			{ "Fire Rate",          "fireRate", &SDK::UCrWeaponAttributeSet::FireRateModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Multiplies the weapon's base rounds per second. Attachment and\nupgrade bonuses still add on top." },
			{ "Reload Speed",       "reload",   &SDK::UCrWeaponAttributeSet::ReloadSpeedModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Higher is faster. Multiplies the weapon's base value. Attachment and\nupgrade bonuses still add on top." },
			// .member unused: Tick() special-cases this row onto the weapon data
			// asset's BaseMagazine.Value instead (g_composedMagazine) -- the clip-size
			// getter never reads MaxMagAmmoModOffset at all. Kept here only so every
			// row still has a valid member pointer for this table's shape.
			{ "Magazine Size",      "magazine", &SDK::UCrWeaponAttributeSet::MaxMagAmmoModOffset,
			  0.0f, -100.0f, 999.0f, 1.0f, "%.0f", "Flat OFFSET added to this weapon's magazine (its real base\ncapacity, not an attachment-style modifier). Negative shrinks it." },
			{ "Damage Falloff",     "falloff",  &SDK::UCrWeaponAttributeSet::DamageFallOffModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Multiplies the weapon's base effective range before damage drops\noff. Attachment bonuses still add on top." },
			{ "Recoil",             "recoil",   &SDK::UCrWeaponAttributeSet::RecoilModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "Multiplies the weapon's base recoil. Attachment bonuses still add\non top. 0 removes recoil entirely." },
			{ "Spread",             "spread",   &SDK::UCrWeaponAttributeSet::SpreadModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "Multiplies the weapon's base spread. Attachment bonuses still add\non top. 0 is perfect accuracy." },
			{ "Sway",               "sway",     &SDK::UCrWeaponAttributeSet::SwayModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "Multiplies the weapon's base sway. Attachment bonuses still add on\ntop. 0 removes weapon sway." },
			{ "ADS Speed",          "ads",      &SDK::UCrWeaponAttributeSet::ADS_TransitionSpeedModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Multiplies the weapon's base aim-down-sights transition speed.\nAttachment bonuses still add on top." },
			{ "Enemies Hit / Shot", "pierce",   &SDK::UCrWeaponAttributeSet::PossibleEnemiesHitPerTrace,
			  1.0f, 1.00f,  10.0f, 1.0f,  "%.0f",  "Projectile piercing -- how many enemies one shot passes through." },
		};

		// Apply() mode per row, aligned with AttrIndex. Multiply/Add compose onto
		// whatever the ability system aggregated (attachments included); Absolute
		// replaces it outright, which only fits Enemies Hit/Shot -- that field is a
		// literal pierce count, not a modifier layered on top of one.
		constexpr BetterCheats::ComposedAttribute::Mode kAttrModes[kAttrCount] = {
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Damage (One Hit Kill overrides in Tick())
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Fire Rate
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Reload Speed
			BetterCheats::ComposedAttribute::Mode::Add,       // Magazine Size -- the attribute itself is an offset
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Damage Falloff
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Recoil
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Spread
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Sway
			BetterCheats::ComposedAttribute::Mode::Multiply,  // ADS Speed
			BetterCheats::ComposedAttribute::Mode::Absolute,  // Enemies Hit/Shot -- a literal count
		};

		// `match` is a lowercase substring tested against the weapon's asset name; ""
		// offers the preset on every weapon. Entries end at attr < 0.
		struct PresetVal { int attr; float value; };

		struct Preset
		{
			const char* match;    // lowercase substring of the asset name; "" = every weapon
			const char* label;
			const char* tooltip;
			const char* credit;   // original mod this is modelled on, or null
			PresetVal   vals[8];
		};

		const Preset kPresets[] = {
			{ "", "Better Weapons",
			  "Improved damage, rate of fire, magazine and range.",
			  "Modelled on 'Better Weapons' by axbhub",
			  { {kAttrDamage,1.50f},{kAttrFireRate,1.25f},{kAttrReload,1.50f},
			    {kAttrMagazine,15.0f},{kAttrFalloff,1.50f},{kAttrPierce,2.0f},{-1,0} } },

			{ "pistol", "Hand Cannon",
			  "Turns the pistol into a hand cannon: huge damage, slow heavy shots,\n"
			  "strong recoil and a smaller magazine.",
			  "Inspired by SwiftstepsKR's Hand Cannon mods",
			  { {kAttrDamage,4.00f},{kAttrFireRate,0.45f},{kAttrRecoil,1.80f},
			    {kAttrMagazine,-4.0f},{kAttrFalloff,1.50f},{kAttrPierce,3.0f},{-1,0} } },

			{ "", "Piercing",
			  "Shots pass through multiple enemies.",
			  "Modelled on SwiftstepsKR's piercing weapon mods",
			  { {kAttrPierce,3.0f},{-1,0} } },

			{ "", "Laser Beam",
			  "No recoil, no spread, no sway. Everything else left alone.",
			  nullptr,
			  { {kAttrRecoil,0.0f},{kAttrSpread,0.0f},{kAttrSway,0.0f},{-1,0} } },

			{ "", "Bullet Hose",
			  "Maximum rate of fire with a deep magazine and fast reload.",
			  nullptr,
			  { {kAttrFireRate,3.00f},{kAttrMagazine,100.0f},{kAttrReload,2.50f},
			    {kAttrSpread,0.60f},{-1,0} } },
		};
		constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

		// Grenade rows. UCrGrenadeWeaponItemDataBase extends
		// UCrWeaponItemDataBase, so a grenade shows up through the exact same
		// LastEquippedWeaponData/profile-discovery path as every gun -- this is
		// what used to render the standard 10-row kAttrs table (Damage, Fire
		// Rate, Recoil...) for a grenade, none of which mean anything for a
		// thrown charge. These rows replace that table for any profile
		// whose Profile::isGrenade is set. Charge Capacity is a GAS attribute on
		// a DIFFERENT attribute set (character->GrenadeChargeAttributes, not
		// WeaponAttributes), so they need their own member-pointer type rather
		// than reusing WeaponAttr.
		using GrenadeAttr = SDK::FGameplayAttributeData SDK::UCrGrenadeChargeAttributeSet::*;

		struct GrenadeRowDef
		{
			const char* label;
			const char* key;
			GrenadeAttr member;   // unused for Damage and Charge Cost -- see the comments on those rows below
			float       defaultValue, minValue, maxValue, step;
			const char* format;
			const char* tooltip;
		};

		enum GrenadeRowIndex : int { kGrenadeDamage = 0, kGrenadeCost, kGrenadeMax, kGrenadeRowCount };

		const GrenadeRowDef kGrenadeRows[kGrenadeRowCount] = {
			// .member unused: Damage scales BaseDamage.Value on the equipped
			// grenade TYPE's data asset, the same per-type-CDO shape as Charge Cost.
			{ "Damage", "grenadeDamage", nullptr,
			  1.0f, 0.10f, 100.0f, 0.5f, "%.2fx",
			  "Multiplies the grenade's base damage. Applies to this grenade type only." },
			// .member unused: Charge Cost composes onto the equipped grenade
			// TYPE's own data asset field (GrenadeThrowCostOfGrenadeCharge on
			// UCrGrenadeWeaponItemDataBase, Chimera_classes.hpp:17303), not a GAS
			// attribute on this set -- same per-type-CDO shape as Magazine Size's
			// BaseMagazine, just a plain float instead of FScalableFloat.Value.
			// See Tick()'s grenade block. Kept here only so this table's shape
			// matches kAttrs' own "every row has a member pointer" convention.
			{ "Charge Cost", "grenadeCost", nullptr,
			  1.0f, 0.00f, 2.00f, 0.05f, "%.2fx",
			  "Multiplies how much of the charge bar one throw uses. 0.00x throws for free." },
			// The saved key keeps its old name so existing values still load.
			{ "Charge Capacity", "grenadeMax", &SDK::UCrGrenadeChargeAttributeSet::MaxGrenadeCharge,
			  0.0f, 0.0f, 20.0f, 0.5f, "%.1f",
			  "Size of the single charge bar (the game's is 1.0). A throw uses 1.0 of it by\n"
			  "default, and the bar refills at the same speed, so a bigger bar takes longer\n"
			  "to fill. Set directly rather than multiplied; 0 leaves the game's value." },
		};

		constexpr BetterCheats::ComposedAttribute::Mode kGrenadeModes[kGrenadeRowCount] = {
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Damage
			BetterCheats::ComposedAttribute::Mode::Multiply,  // Charge Cost
			BetterCheats::ComposedAttribute::Mode::Absolute,  // Charge Capacity
		};

		// Built-in grenade presets, same shape as the weapon kPresets above but
		// indexing into kGrenadeRows instead of kAttrs. Entries end at row < 0.
		// Only the per-profile rows above are reachable here -- Fuse,
		// Blast Radius and Throw Force (kGrenadeGlobals) are one shared CDO for
		// every grenade type, not part of this per-profile table, so a preset
		// can't set them the same way it sets a profile's own rows.
		struct GrenadePresetVal { int row; float value; };

		struct GrenadePreset
		{
			const char*      label;
			const char*      tooltip;
			const char*      credit;
			GrenadePresetVal vals[4];
		};

		const GrenadePreset kGrenadePresets[] = {
			{ "Less Cost Grenade",
			  "Halves how much of the charge bar one throw uses. Charge Capacity, Fuse, Blast\n"
			  "Radius and Throw Force are untouched -- the pak this reproduces changes\n"
			  "cost only.",
			  "Modelled on 'Less Cost Grenade' (part of 'Better Grenade') by axbhub,\nNexusMod #41 for StarRupture",
			  { {kGrenadeCost, 0.5f}, {-1,0} } },
		};
		constexpr int kGrenadePresetCount = static_cast<int>(sizeof(kGrenadePresets) / sizeof(kGrenadePresets[0]));

		// Fuse / Blast Radius / Throw Force. These live on classes Dumper-7
		// only typed under the game's Server SDK dump (BP_GrenadeProjectile_
		// classes.hpp, GA_ThrowGrenade_classes.hpp), whose struct layout for
		// THIS client build is unverified and may not match. Resolved by NAME
		// via the loader's IPluginObjectProperties instead of a cast through
		// that Server-SDK C++ struct -- offset-independent, so a layout
		// mismatch can't silently read/write the wrong bytes.
		//
		// Unlike Charge Cost/Charge Capacity above, each of these three lives on
		// exactly ONE shared CDO for the whole game (one projectile class, one
		// throw ability) -- not per grenade type -- so they're global controls
		// applied every tick regardless of what's currently equipped, shown
		// identically on every grenade tab rather than stored per-Profile.
		struct GrenadeGlobalDef
		{
			const char* label;
			const char* key;            // SessionConfig leaf + compose-state key segment
			const char* className;      // CDO's class, "Default__<className>" is its object name
			const char* propertyName;   // resolved via IPluginObjectProperties::FindPropertyByName
			float       defaultValue, minValue, maxValue, step;   // slider range, multiplier space
			const char* format;
			const char* tooltip;
		};

		enum GrenadeGlobalIndex : int { kGrenadeFuse = 0, kGrenadeRadius, kGrenadeThrowForce, kGrenadeGlobalCount };

		const GrenadeGlobalDef kGrenadeGlobals[kGrenadeGlobalCount] = {
			{ "Fuse Time",    "grenadeFuse",   "BP_GrenadeProjectile_C", "TimeToExplode",
			  1.0f, 0.05f, 5.00f, 0.05f, "%.2fx",
			  "Multiplies time-to-explode. Lower pops sooner. One shared projectile\nclass, so this affects every grenade type." },
			{ "Blast Radius", "grenadeRadius", "BP_GrenadeProjectile_C", "DamageRadius",
			  1.0f, 0.10f, 5.00f, 0.05f, "%.2fx",
			  "Multiplies the explosion's damage radius. One shared projectile class,\nso this affects every grenade type." },
			{ "Throw Force",  "grenadeThrow",  "GA_ThrowGrenade_C",      "ProjectileSpeed",
			  1.0f, 0.10f, 5.00f, 0.05f, "%.2fx",
			  "Multiplies how fast a thrown grenade launches. One shared throw\nability, so this affects every grenade type." },
		};

		// Generic final-value safety clamp (not the slider's own range above,
		// which bounds the multiplier the user picks) -- the real baseline
		// magnitudes for these three fields are unverified without a live
		// in-game read, so this just guards against zero/negative/runaway
		// results rather than encoding a game-specific number.
		constexpr float kGrenadeGlobalFinalMin = 0.001f;
		constexpr float kGrenadeGlobalFinalMax = 1000000.0f;

		// Resolved by name through ResolveCdoField (cdo_lookup.h), retried
		// until the class is loaded and re-resolved if it unloads.
		CdoFieldHandle                  g_grenadeGlobalHandle[kGrenadeGlobalCount];
		BetterCheats::ComposedAttribute g_composedGrenadeGlobal[kGrenadeGlobalCount];
		std::atomic<float>              g_grenadeGlobalValue[kGrenadeGlobalCount] = { 1.0f, 1.0f, 1.0f };

		// Debug readout, parallel to g_dbgGrenadeGame/Expected -- these three
		// aren't Profile-scoped (see above), so they get their own tiny arrays
		// rather than a slot in that one.
		std::atomic<float> g_dbgGrenadeGlobalGame[kGrenadeGlobalCount];
		std::atomic<float> g_dbgGrenadeGlobalExpected[kGrenadeGlobalCount];

		// ---------------------------------------------------------------------
		// Throw-time probe: earlier read-only dumps of the grenade damage
		// GameplayEffect CDOs found no damage number on them, so the input has to
		// be on the runtime spec the projectile builds. After a throw (a grenade
		// is equipped and its charge drops) this looks for the newest live
		// BP_GrenadeProjectile_C, watches its DamageGESpec until the spec exists,
		// and logs what the spec carries -- including its SetByCaller maps, which
		// the loader's property API cannot reach, so those come from the engine
		// struct layout (see LogGrenadeSpec).
		// Log-only, one shot per session, game thread (Tick) only. The projectile
		// is held as an ObjectRef between ticks and every spec pointer is used
		// within the tick that read it. The raw reads of the spec go through
		// SEH-guarded POD copies, because the layout is unverified until a throw.
		// ---------------------------------------------------------------------
		constexpr float kThrowProbeWindowSeconds = 12.0f;  // fuse is 4 s, plus margin
		// The projectile is gone about 4.5 s after the throw, so the 5 s CdoRetryDue
		// gate would miss it. Scans run only while armed and stop once one is held.
		constexpr float kThrowProbeScanInterval  = 0.5f;
		constexpr int   kThrowProbeMaxScans      = 24;     // per window
		constexpr int   kThrowProbeMaxMisses     = 3;      // stop looking if no projectile ever shows up

		float                     g_throwProbeLastCharge   = -1.0f;
		float                     g_throwProbeLastMax      = -1.0f;
		float                     g_throwProbeWindow       = 0.0f;   // > 0 while armed
		float                     g_throwProbeScanCooldown = 0.0f;
		int                       g_throwProbeScans        = 0;      // walks this window
		int                       g_throwProbeMisses       = 0;
		bool                      g_throwProbeDone         = false;
		bool                      g_throwProbeSeen         = false;  // a projectile was found this arming
		ObjectRef<SDK::UObject>   g_throwProbeAttrs;               // the attribute set the charge was last read from
		ObjectRef<SDK::UObject>   g_throwProbeProjectile;

		void EndThrowProbe(bool projectileSeen)
		{
			g_throwProbeWindow = 0.0f;
			g_throwProbeSeen   = false;
			g_throwProbeProjectile.Reset();
			if (projectileSeen)
			{
				g_throwProbeDone = true;
			}
			else if (++g_throwProbeMisses >= kThrowProbeMaxMisses)
			{
				LOG_INFO("GrenadeDamageProbe: no projectile found after %d throws; probe disabled for this session.",
					kThrowProbeMaxMisses);
			}
		}

		// FGameplayTag is a single FName, so the name-keyed and tag-keyed
		// SetByCaller maps share one layout.
		using SetByCallerMap = UC::TMap<SDK::FName, float>;

		struct SetByCallerEntry
		{
			int32_t  nameIndex;
			uint32_t nameNumber;
			float    value;
		};

		constexpr int kSetByCallerMaxEntries   = 32;
		constexpr int kSetByCallerMaxAllocated = 64;
		constexpr int kMapImplausible = -1;   // a layout check failed
		constexpr int kMapFault       = -2;   // reading it raised an access violation

		// Copies the map's entries into `out` (room for kSetByCallerMaxEntries).
		// Returns the entry count, or kMapImplausible / kMapFault. POD locals
		// only -- SEH forbids unwindable objects in this function. Every bound
		// comes from the map's own header and is checked before it is used, and
		// the loop stops at the validated count.
		int ReadSetByCallerMap(SetByCallerMap& map, SetByCallerEntry* out, int* numOut, int* allocatedOut)
		{
			__try
			{
				const int num       = map.Num();
				const int allocated = map.NumAllocated();
				*numOut       = num;
				*allocatedOut = allocated;

				if (num < 0 || num > kSetByCallerMaxEntries)
					return kMapImplausible;
				if (allocated < num || allocated > kSetByCallerMaxAllocated || map.Max() < allocated)
					return kMapImplausible;
				if (num == 0)
					return 0;
				if (!map.IsValid() || map.GetAllocationFlags().Num() < allocated)
					return kMapImplausible;

				int count = 0;
				for (int i = 0; i < allocated && count < num; ++i)
				{
					if (!map.IsValidIndex(i))
						continue;
					out[count].nameIndex  = map[i].Key().ComparisonIndex;
					out[count].nameNumber = map[i].Key().Number;
					out[count].value      = map[i].Value();
					++count;
				}
				return count == num ? count : kMapImplausible;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return kMapFault;
			}
		}

		// The std::string lives here, outside the SEH function that calls this.
		void CopyNameToBuffer(int32_t index, uint32_t number, char* buffer, size_t capacity)
		{
			const std::string name = SDK::FName(index, number).ToString();
			std::snprintf(buffer, capacity, "%s", name.c_str());
		}

		// A name read from the engine's name pool. A bad index from a wrong
		// layout can fault there, so it is guarded like the map read.
		void ResolveName(int32_t index, uint32_t number, char* buffer, size_t capacity)
		{
			std::snprintf(buffer, capacity, "<unreadable>");
			if (index < 0)
				return;
			__try
			{
				CopyNameToBuffer(index, number, buffer, capacity);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				std::snprintf(buffer, capacity, "<unreadable>");
			}
		}

		struct SpecHeader
		{
			bool     hasDef;
			int32_t  defNameIndex;
			uint32_t defNameNumber;
			float    level;
			int32_t  stackCount;
			int32_t  modifiers;
			int32_t  modifiedAttributes;
		};

		// POD locals only (SEH). False if the spec could not be read.
		bool ReadSpecHeader(SDK::FGameplayEffectSpec* spec, SpecHeader* out)
		{
			__try
			{
				out->hasDef = spec->Def != nullptr;
				out->defNameIndex  = out->hasDef ? spec->Def->Name.ComparisonIndex : -1;
				out->defNameNumber = out->hasDef ? spec->Def->Name.Number : 0;
				out->level              = spec->Level;
				out->stackCount         = spec->StackCount;
				out->modifiers          = spec->Modifiers.Num();
				out->modifiedAttributes = spec->ModifiedAttributes.Num();
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// False after logging a single line if the map could not be read.
		bool LogSetByCallerMap(const char* label, SetByCallerMap& map)
		{
			SetByCallerEntry entries[kSetByCallerMaxEntries];
			int num = 0, allocated = 0;
			const int count = ReadSetByCallerMap(map, entries, &num, &allocated);
			if (count == kMapFault)
			{
				LOG_INFO("GrenadeDamageProbe:     %s: access violation reading the map -- layout assumption failed; giving up on this spec.", label);
				return false;
			}
			if (count == kMapImplausible)
			{
				LOG_INFO("GrenadeDamageProbe:     %s: implausible (num=%d allocated=%d) -- layout assumption failed; giving up on this spec.",
					label, num, allocated);
				return false;
			}

			LOG_INFO("GrenadeDamageProbe:     %s: %d entr%s", label, count, count == 1 ? "y" : "ies");
			for (int i = 0; i < count; ++i)
			{
				char name[128];
				ResolveName(entries[i].nameIndex, entries[i].nameNumber, name, sizeof(name));
				LOG_INFO("GrenadeDamageProbe:       '%s' = %.4f", name, entries[i].value);
			}
			return true;
		}

		void LogGrenadeSpec(const char* label, SDK::FGameplayEffectSpec* spec)
		{
			SpecHeader header;
			if (!ReadSpecHeader(spec, &header))
			{
				LOG_INFO("GrenadeDamageProbe:   %s: spec=%p access violation reading the header -- layout assumption failed; giving up on this spec.",
					label, static_cast<void*>(spec));
				return;
			}

			char def[128] = "<null>";
			if (header.hasDef)
				ResolveName(header.defNameIndex, header.defNameNumber, def, sizeof(def));

			LOG_INFO("GrenadeDamageProbe:   %s: spec=%p def='%s' level=%.4f stackCount=%d modifiers=%d modifiedAttributes=%d",
				label, static_cast<void*>(spec), def, header.level, header.stackCount,
				header.modifiers, header.modifiedAttributes);

			// Pad_1D8 is Dumper-7's gap for the two SetByCaller TMaps (name-keyed,
			// then tag-keyed), 0x50 each. The static_asserts are the layout claim.
			static_assert(sizeof(SetByCallerMap) == 0x50, "TMap layout");
			static_assert(sizeof(spec->Pad_1D8) == 2 * sizeof(SetByCallerMap), "two TMaps fill the gap");

			auto* maps = reinterpret_cast<SetByCallerMap*>(spec->Pad_1D8);
			if (LogSetByCallerMap("SetByCaller map A", maps[0]))
				LogSetByCallerMap("SetByCaller map B", maps[1]);
		}

		// Pointer to a named property's storage on `object`, or null.
		void* NamedPropertyPtr(IPluginObjectProperties* props, void* object, const char* propertyName)
		{
			PluginPropertyHandle property = props->FindPropertyOnObject(object, propertyName);
			return property ? props->GetPropertyRawPtr(object, property) : nullptr;
		}

		// FGameplayEffectSpecHandle is a TSharedPtr<FGameplayEffectSpec>: the first
		// pointer is the spec, null until the projectile has built it.
		SDK::FGameplayEffectSpec* ReadSpecHandle(IPluginObjectProperties* props, void* object, const char* propertyName)
		{
			void* handle = NamedPropertyPtr(props, object, propertyName);
			return handle ? *static_cast<SDK::FGameplayEffectSpec* const*>(handle) : nullptr;
		}

		// The newest live instance of projectileClass, or null; `total` is how many
		// live instances GObjects holds. Compares class pointers, no strings, the
		// way enemies.cpp does. GObjects order is allocation order, so the last
		// match is the newest and an exploded grenade that has not been collected
		// yet is never picked over a fresh one. Game thread only.
		SDK::UObject* FindNewestProjectile(SDK::UClass* projectileClass, int& total)
		{
			total = 0;
			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			if (!arr || !projectileClass)
				return nullptr;

			constexpr uint32_t kSkipFlags =
				static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject) |
				static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
				static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
				static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);

			SDK::UObject* newest = nullptr;
			for (int i = 0; i < arr->Num(); ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || obj->Class != projectileClass)
					continue;
				if (static_cast<uint32_t>(obj->Flags) & kSkipFlags)
					continue;
				++total;
				newest = obj;
			}
			return newest;
		}

		// ---------------------------------------------------------------------
		// Hit tracker: how much health enemies lose to one grenade, so the
		// BaseDamage scale (Grenade Damage row) can be compared in game. Armed by every
		// detected throw (not only the first), log-only, one window at a time.
		// It follows the thrown projectile each tick, tracks enemies inside
		// DamageRadius plus a margin of it, and once the projectile is gone plus a
		// short tail writes one line per enemy and a summary.
		// Game thread (Tick) only. Projectile and enemies are ObjectRefs between
		// ticks; positions and health are read within the tick that validated
		// them. Health is the Mass HP fragment the One-Hit Kill cheat writes, or
		// the actor's GAS CurrentHealth where that is all there is.
		// ---------------------------------------------------------------------
		constexpr double kHitDamageRadius  = 900.0;   // BP_GrenadeProjectile_C DamageRadius, before the Blast Radius row
		constexpr double kHitNearMargin    = 300.0;
		constexpr float  kHitTailSeconds   = 1.5f;
		constexpr float  kHitFindSeconds   = 2.0f;
		constexpr float  kHitFindInterval  = 0.1f;
		constexpr float  kHitScanInterval  = 0.5f;    // GObjects walk for candidate enemies
		constexpr float  kHitMaxSeconds    = 20.0f;
		constexpr size_t kHitMaxTracked    = 20;
		constexpr size_t kHitMaxCandidates = 512;

		struct HitTracked
		{
			ObjectRef<SDK::AActor> actor;
			std::string name;
			bool  hasMass = false, hasGas = false;
			float massBefore = 0.0f, massLast = 0.0f;
			float gasBefore  = 0.0f, gasLast  = 0.0f;
			int   massMisses = 0; // consecutive ticks the Mass fragment could not be read
			bool  gone = false;   // despawned, pooled or destroyed: counts as died
		};

		bool                                   g_hitActive        = false;
		bool                                   g_hitSeen          = false;
		bool                                   g_hitGone          = false;
		bool                                   g_hitHavePos       = false;
		float                                  g_hitElapsed       = 0.0f;
		float                                  g_hitFindCooldown  = 0.0f;
		float                                  g_hitScanCooldown  = 0.0f;
		float                                  g_hitTail          = 0.0f;
		float                                  g_hitScale         = 1.0f;
		float                                  g_hitBaseDamage    = 0.0f;
		int                                    g_hitUnreadable    = 0;
		double                                 g_hitPos[3]        = {};
		ObjectRef<SDK::UObject>                g_hitProjectile;
		std::vector<HitTracked>                g_hitTracked;
		std::vector<ObjectRef<SDK::AActor>>    g_hitCandidates;
		std::unordered_map<const SDK::UClass*, bool> g_hitAiClass;

		void ResetHitTracker()
		{
			g_hitActive = g_hitSeen = g_hitGone = g_hitHavePos = false;
			g_hitProjectile.Reset();
			g_hitTracked.clear();
			g_hitCandidates.clear();
			g_hitAiClass.clear();
		}

		void ScanHitCandidates()
		{
			g_hitCandidates.clear();
			SDK::TUObjectArray* arr = SDK::UObject::GObjects.GetTypedPtr();
			SDK::UClass* aiClass = SDK::ACrAIBase::StaticClass();
			if (!arr || !aiClass)
				return;

			constexpr uint32_t kSkipFlags =
				static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject) |
				static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
				static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
				static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);

			const SDK::UClass* lastClass = nullptr;
			bool lastIsAi = false;
			for (int i = 0; i < arr->Num() && g_hitCandidates.size() < kHitMaxCandidates; ++i)
			{
				SDK::UObject* obj = arr->GetByIndex(i);
				if (!obj || !obj->Class)
					continue;
				if (obj->Class != lastClass)
				{
					auto it = g_hitAiClass.find(obj->Class);
					if (it == g_hitAiClass.end())
						it = g_hitAiClass.emplace(obj->Class, obj->IsA(aiClass)).first;
					lastClass = obj->Class;
					lastIsAi  = it->second;
				}
				if (!lastIsAi || (static_cast<uint32_t>(obj->Flags) & kSkipFlags))
					continue;
				g_hitCandidates.emplace_back(static_cast<SDK::AActor*>(obj));
			}
		}

		// False for a pooled Mass enemy or one being destroyed: not a live world actor.
		bool HitActorLive(SDK::AActor* actor)
		{
			if (actor->bActorIsBeingDestroyed)
				return false;
			static SDK::UClass* massClass = SDK::AMassEnemyCharacterBase::StaticClass();
			if (massClass && actor->IsA(massClass) && static_cast<SDK::AMassEnemyCharacterBase*>(actor)->bIsInPool)
				return false;
			return true;
		}

		void SampleHitHealth(SDK::AActor* actor, bool& hasMass, float& mass, bool& hasGas, float& gas)
		{
			hasMass = BetterCheats::Panels::Enemies::TryReadMassHealth(actor, mass);
			hasGas  = false;
			SDK::UCrAIBaseAttributeSet* attrs = static_cast<SDK::ACrAIBase*>(actor)->BaseAttributes;
			if (attrs)
			{
				gas    = attrs->CurrentHealth.CurrentValue;
				hasGas = true;
			}
		}

		void FinishHitTracker()
		{
			int hit = 0;
			float total = 0.0f;
			for (const HitTracked& e : g_hitTracked)
			{
				const float massLost = e.massBefore - e.massLast;
				const float gasLost  = e.gasBefore  - e.gasLast;
				// The source that moved wins; Mass HP when neither did.
				const bool useGas = !e.hasMass || (e.hasGas && massLost <= 0.0f && gasLost > 0.0f);
				const float before = useGas ? e.gasBefore : e.massBefore;
				const float after  = useGas ? e.gasLast   : e.massLast;
				const float lost   = before - after;
				const bool  died   = e.gone || after <= 0.0f;
				if (lost > 0.0f)
				{
					++hit;
					total += lost;
				}
				LOG_INFO("GrenadeDamageProbe: hit '%s' health %.1f -> %.1f (lost %.1f, %s), grenade scale x%.2f, BaseDamage %.2f",
					e.name.c_str(), before, after, lost, died ? "died" : "alive", g_hitScale, g_hitBaseDamage);
			}
			LOG_INFO("GrenadeDamageProbe: summary: %d of %d tracked enem%s hit, total damage %.1f, grenade scale x%.2f, BaseDamage %.2f%s",
				hit, static_cast<int>(g_hitTracked.size()), g_hitTracked.size() == 1 ? "y" : "ies", total, g_hitScale, g_hitBaseDamage,
				g_hitUnreadable > 0 ? " (some enemies had no readable health)" : "");
			ResetHitTracker();
		}

		void StartHitTracker(float scale, float baseDamage)
		{
			if (g_hitActive)
				return;   // one window at a time
			ResetHitTracker();
			g_hitActive       = true;
			g_hitScale        = scale;
			g_hitBaseDamage   = baseDamage;
			g_hitElapsed      = 0.0f;
			g_hitFindCooldown = 0.0f;
			g_hitScanCooldown = 0.0f;
			g_hitTail         = 0.0f;
			g_hitUnreadable   = 0;
			LOG_INFO("GrenadeDamageProbe: throw detected, tracking enemies near the grenade (scale x%.2f, BaseDamage %.2f).",
				scale, baseDamage);
		}

		void TickHitTracker(float deltaSeconds)
		{
			if (!g_hitActive)
				return;
			g_hitElapsed += deltaSeconds;

			try
			{
				BetterCheats::Panels::Enemies::PrepareHealthReads(deltaSeconds);

				SDK::UObject* projectile = g_hitProjectile.Get();
				if (projectile && static_cast<SDK::AActor*>(projectile)->bActorIsBeingDestroyed)
					projectile = nullptr;

				if (!g_hitSeen)
				{
					g_hitFindCooldown -= deltaSeconds;
					if (g_hitFindCooldown <= 0.0f)
					{
						g_hitFindCooldown = kHitFindInterval;
						SDK::UObject* projectileCdo = g_grenadeGlobalHandle[kGrenadeFuse].object.Get();
						int total = 0;
						SDK::UObject* found = FindNewestProjectile(projectileCdo ? projectileCdo->Class : nullptr, total);
						if (found && !static_cast<SDK::AActor*>(found)->bActorIsBeingDestroyed)
						{
							g_hitProjectile.Set(found);
							g_hitSeen  = true;
							projectile = found;
						}
					}
					if (!g_hitSeen)
					{
						if (g_hitElapsed >= kHitFindSeconds)
						{
							LOG_INFO("GrenadeDamageProbe: no grenade projectile found after the throw; nothing tracked.");
							ResetHitTracker();
						}
						return;
					}
				}

				if (projectile)
				{
					const SDK::FVector p = static_cast<SDK::AActor*>(projectile)->K2_GetActorLocation();
					g_hitPos[0] = p.X; g_hitPos[1] = p.Y; g_hitPos[2] = p.Z;
					g_hitHavePos = true;
				}
				else if (!g_hitGone)
				{
					g_hitGone = true;
					g_hitTail = 0.0f;
				}
				else
				{
					g_hitTail += deltaSeconds;
				}

				// New enemies come into range only while the projectile is alive.
				if (g_hitHavePos && !g_hitGone && g_hitTracked.size() < kHitMaxTracked)
				{
					g_hitScanCooldown -= deltaSeconds;
					if (g_hitScanCooldown <= 0.0f)
					{
						g_hitScanCooldown = kHitScanInterval;
						ScanHitCandidates();
					}

					// The live projectile's own radius, so the Blast Radius row is followed.
					double radius = kHitDamageRadius;
					{
						IPluginHooks* hooks = GetHooks();
						IPluginObjectProperties* props = hooks ? hooks->ObjectProperties : nullptr;
						const CdoFieldHandle& rh = g_grenadeGlobalHandle[kGrenadeRadius];
						double raw = 0.0;
						if (props && rh.ok && projectile && props->GetFloatProperty(projectile, rh.property, &raw) && raw > 0.0)
							radius = raw;
					}
					const double reach = radius + kHitNearMargin;
					for (const ObjectRef<SDK::AActor>& ref : g_hitCandidates)
					{
						if (g_hitTracked.size() >= kHitMaxTracked)
							break;
						SDK::AActor* actor = ref.Get();
						if (!actor || !HitActorLive(actor))
							continue;
						bool known = false;
						for (const HitTracked& e : g_hitTracked)
							if (e.actor.Get() == actor) { known = true; break; }
						if (known)
							continue;

						const SDK::FVector loc = actor->K2_GetActorLocation();
						const double dx = loc.X - g_hitPos[0], dy = loc.Y - g_hitPos[1], dz = loc.Z - g_hitPos[2];
						if (dx * dx + dy * dy + dz * dz > reach * reach)
							continue;

						HitTracked e;
						SampleHitHealth(actor, e.hasMass, e.massBefore, e.hasGas, e.gasBefore);
						if (!e.hasMass && !e.hasGas)
						{
							++g_hitUnreadable;
							continue;
						}
						e.massLast = e.massBefore;
						e.gasLast  = e.gasBefore;
						e.actor.Set(actor);
						e.name = "<unreadable>";
						try { e.name = actor->GetName(); } catch (...) {}
						g_hitTracked.push_back(std::move(e));
					}
				}

				// Last health of every tracked enemy that is still around.
				for (HitTracked& e : g_hitTracked)
				{
					if (e.gone)
						continue;
					SDK::AActor* actor = e.actor.Get();
					if (!actor || !HitActorLive(actor))
					{
						e.gone = true;
						continue;
					}
					bool hasMass, hasGas;
					float mass = 0.0f, gas = 0.0f;
					SampleHitHealth(actor, hasMass, mass, hasGas, gas);
					// The Mass entity goes before the puppet does, so a fragment that
					// stays unreadable means the enemy died.
					if (hasMass && e.hasMass) { e.massLast = mass; e.massMisses = 0; }
					else if (e.hasMass && ++e.massMisses >= 3) e.gone = true;
					if (hasGas  && e.hasGas)  e.gasLast  = gas;
				}
			}
			catch (...)
			{
				LOG_WARN("GrenadeDamageProbe: exception while tracking the grenade hit; window dropped.");
				ResetHitTracker();
				return;
			}

			if ((g_hitGone && g_hitTail >= kHitTailSeconds) || g_hitElapsed >= kHitMaxSeconds)
				FinishHitTracker();
		}

		// Call once per tick. `attrs` is the player's grenade charge set (null if
		// none) and `grenadeEquipped` says a grenade is the equipped weapon; arming
		// needs both. A drop only counts as a throw if it is on the same attribute
		// set as last tick (not a respawn or reload), Charge Capacity did not just
		// change, and the charge is still below Max (not a clamp to a lowered Max).
		// Call before anything tops CurrentGrenadeCharge back up.
		void ProbeGrenadeThrow(SDK::UCrGrenadeChargeAttributeSet* attrs, bool grenadeEquipped, float deltaSeconds,
			float damageScale, float baseDamage)
		{
			TickHitTracker(deltaSeconds);

			// The spec dump below is one shot; the hit tracker arms on every throw.
			const bool specProbeOff = g_throwProbeDone || g_throwProbeMisses >= kThrowProbeMaxMisses;

			if (g_throwProbeWindow <= 0.0f)
			{
				if (!attrs || !grenadeEquipped)
				{
					g_throwProbeLastCharge = -1.0f;
					g_throwProbeAttrs.Reset();
					return;
				}

				const float charge = attrs->CurrentGrenadeCharge.CurrentValue;
				const float maxCharge = attrs->MaxGrenadeCharge.CurrentValue;
				const bool sameSet = g_throwProbeAttrs.Get() == attrs;
				const bool threw = sameSet && g_throwProbeLastCharge >= 0.0f
					&& charge < g_throwProbeLastCharge - 0.01f
					&& std::fabs(maxCharge - g_throwProbeLastMax) < 0.01f
					&& charge < maxCharge - 0.01f;

				if (!sameSet)
					g_throwProbeAttrs.Set(attrs);
				const float previousCharge = g_throwProbeLastCharge;
				g_throwProbeLastCharge = charge;
				g_throwProbeLastMax    = maxCharge;

				if (!threw)
					return;

				StartHitTracker(damageScale, baseDamage);
				if (specProbeOff)
					return;

				g_throwProbeWindow       = kThrowProbeWindowSeconds;
				g_throwProbeScanCooldown = 0.0f;
				g_throwProbeScans        = 0;
				LOG_INFO("GrenadeDamageProbe: charge dropped %.2f -> %.2f, watching for the thrown projectile.",
					previousCharge, charge);
			}

			g_throwProbeWindow -= deltaSeconds;

			IPluginHooks* hooks = GetHooks();
			IPluginObjectProperties* props = hooks ? hooks->ObjectProperties : nullptr;
			if (!props || !props->IsReady())
			{
				if (g_throwProbeWindow <= 0.0f)
					EndThrowProbe(false);
				return;
			}

			try
			{
				SDK::UObject* projectile = g_throwProbeProjectile.Get();

				if (!projectile && g_throwProbeSeen)
				{
					LOG_INFO("GrenadeDamageProbe: projectile was destroyed before its DamageGESpec was built.");
					EndThrowProbe(true);
					return;
				}

				// The projectile class is the Fuse Time row's CDO's class, already
				// resolved by the grenade globals block below.
				SDK::UObject* projectileCdo = g_grenadeGlobalHandle[kGrenadeFuse].object.Get();

				if (!projectile)
					g_throwProbeScanCooldown -= deltaSeconds;
				if (!projectile && g_throwProbeScanCooldown <= 0.0f && g_throwProbeScans < kThrowProbeMaxScans)
				{
					g_throwProbeScanCooldown = kThrowProbeScanInterval;
					++g_throwProbeScans;
					int total = 0;
					projectile = FindNewestProjectile(projectileCdo ? projectileCdo->Class : nullptr, total);
					if (projectile)
					{
						g_throwProbeProjectile.Set(projectile);
						g_throwProbeSeen = true;

						std::string projectileName = "<unreadable>";
						try { projectileName = projectile->GetName(); } catch (...) {}
						LOG_INFO("GrenadeDamageProbe: found projectile '%s' (newest of %d live).", projectileName.c_str(), total);
						if (void* list = NamedPropertyPtr(props, projectile, "MultiDamageClassList"))
							LOG_INFO("GrenadeDamageProbe:   MultiDamageClassList.Num() = %d",
								static_cast<UC::TArray<SDK::UClass*>*>(list)->Num());
					}
				}

				if (projectile)
				{
					SDK::FGameplayEffectSpec* damage   = ReadSpecHandle(props, projectile, "DamageGESpec");
					SDK::FGameplayEffectSpec* friendly = ReadSpecHandle(props, projectile, "FriendlyDamageGESpec");
					if (damage || friendly)
					{
						LOG_INFO("GrenadeDamageProbe: projectile spec(s) built (%.1f s left of the window):", g_throwProbeWindow);
						if (damage)   LogGrenadeSpec("DamageGESpec", damage);
						else          LOG_INFO("GrenadeDamageProbe:   DamageGESpec.Data is null");
						if (friendly) LogGrenadeSpec("FriendlyDamageGESpec", friendly);
						else          LOG_INFO("GrenadeDamageProbe:   FriendlyDamageGESpec.Data is null");
						EndThrowProbe(true);
						return;
					}
				}

				if (g_throwProbeWindow <= 0.0f)
				{
					if (g_throwProbeSeen)
						LOG_INFO("GrenadeDamageProbe: projectile found but DamageGESpec.Data stayed null for the whole window.");
					else
						LOG_INFO("GrenadeDamageProbe: no BP_GrenadeProjectile_C instance found within the window (projectile class %s).",
							projectileCdo ? "resolved" : "not resolved");
					EndThrowProbe(g_throwProbeSeen);
				}
			}
			catch (...)
			{
				LOG_WARN("GrenadeDamageProbe: exception during the throw-time probe.");
				EndThrowProbe(true);
			}
		}

		// Profiles are DISCOVERED, not hardcoded. Weapon data assets live in the paks
		// (no I_*DataItem_C classes exist in the SDK dump), so the only truthful source
		// of the roster is what the player actually equips.
		struct Profile
		{
			std::string key;        // sanitized, used as the config path segment
			std::string display;    // prettified for the tab label
			std::string raw;        // exact asset name, shown for diagnosis
			float       values[kAttrCount];
			bool        oneHitKill       = false;
			bool        infiniteMagazine = false;

			// Base stats resolved once from this weapon TYPE's own CDO --
			// `raw` already IS the CDO's object name (see kBuiltInWeapons/
			// PersistKnownWeapons), so no separate lookup key is needed.
			// Valid whether or not this weapon is currently equipped, unlike
			// g_dbgAttrGame/Expected/Base/Buffed below, which Tick() fills
			// only for the active profile -- this is what lets every tab
			// show ITS OWN weapon's base numbers instead of whatever's in
			// the player's hand. See ResolveWeaponBaseFields.
			ObjectRef<SDK::UCrWeaponItemDataBase> baseCdo;
			bool        baseResolved        = false;
			bool        baseLoggedMiss      = false;
			float       baseRetryCooldown   = 0.0f;
			float       baseDamage          = 0.0f;
			float       baseRoundsPerMinute = 0.0f;   // seconds between shots, not literal RPM -- see RealBaseForAttr
			float       baseMagazine        = 0.0f;
			float       baseRange           = 0.0f;
			int         basePierce          = 0;
			float       baseReloadTime      = 0.0f;
			float       baseSpread          = 0.0f;
			float       baseAdsSpeed        = 0.0f;

			// The multiplier delta this weapon's own attachments/LEMs last showed
			// while it WAS the equipped/active profile (unmodified - base, in the
			// same multiplier space g_dbgAttrBuffed/g_dbgAttrBase capture) --
			// indexed like kAttrs. Kept so a tab can still show a best-effort
			// estimate of this weapon's real numbers while it isn't in hand,
			// rather than nothing at all. 0 (no mods) until Tick() has actually
			// seen this weapon equipped at least once -- "estimate with no mods"
			// from the spec. Only Multiply-mode rows are ever written here
			// (Magazine composes onto a real field directly, Pierce is a literal
			// Absolute count) -- see Tick()'s write-back next to g_composed[a].
			float       cachedModsMult[kAttrCount] = {};

			// Grenade-only fields, meaningful only when isGrenade is true --
			// discovered the same way as every other weapon, just tagged so
			// RenderImGui can swap in the grenade-specific layout.
			bool        isGrenade        = false;
			float       grenadeValues[kGrenadeRowCount];
			bool        infiniteCharges  = false;

			// Saved-presets UI state -- selected/renaming/error, one
			// instance per weapon type so switching tabs doesn't leak one
			// weapon's in-progress rename into another's.
			BetterCheats::UI::SavedPresetRowState presetRow;
		};

		// g_profiles is written from Tick (discovery, ApplySavedConfig) and iterated
		// with live Profile& references from RenderImGui on the render thread -- a
		// push_back from one side while the other holds a reference is a
		// use-after-free on reallocation. g_profilesMutex guards every access to the
		// container; callers of FindOrCreateProfile/EnsureBuiltInProfiles/
		// PersistKnownWeapons below must already hold it.
		std::mutex            g_profilesMutex;
		std::vector<Profile>  g_profiles;
		std::atomic<bool>     g_allInfiniteMagazine{ false };

		// Live readout, refreshed from Tick so the panel can show what the game actually
		// reports rather than what we hoped it would. Read from RenderImGui.
		std::atomic<float>    g_dbgMag       { -1.0f };
		std::atomic<float>    g_dbgMagMax    { -1.0f };
		std::atomic<float>    g_dbgReserve   { -1.0f };
		std::atomic<float>    g_dbgReserveMax{ -1.0f };

		// One-shot confirmation logging: records in ModLoader.log that both the
		// Tick-side fill and the RenderImGui-side draw have run this session,
		// so a silently-broken live-values pipeline is diagnosable from the
		// log alone rather than from a report of "nothing happens".
		std::atomic<bool> g_loggedTickFill{ false };
		std::atomic<bool> g_loggedRender{ false };

		// The equipped weapon's real base stats, read from its data asset CDO
		// (LastEquippedWeaponData) -- the inherited Au-layer fields on
		// UCrWeaponAttributeSet (WeaponDamage, FireRate, ...) that a previous build
		// showed here are never written by the game at all (confirmed permanently
		// zero), a dead parallel layer, not a bug in how they were read.
		// -1 = not available yet, matching
		// g_dbgMag above. g_dbgWeaponBaseMagazine is the CAPTURED ORIGINAL (via
		// g_composedMagazine), not a live read, since Magazine Size writes through
		// this same field -- a live read while active would show our own offset.
		std::atomic<float> g_dbgWeaponBaseDamage       { -1.0f };
		std::atomic<float> g_dbgWeaponRoundsPerMinute  { -1.0f };
		std::atomic<float> g_dbgWeaponBaseMagazine     { -1.0f };
		std::atomic<float> g_dbgWeaponBaseRange        { -1.0f };

		// Per-row expected/game live readout, indexed like
		// kAttrs/g_composed. `game` is CurrentValue read fresh at the start of this
		// tick, before we touch it; `expected` is what we intend it to be (the
		// composed result while active, or `game` itself while inactive). Compared
		// pre-write so a lasting mismatch actually means something. `base` is
		// BaseValue (never written by us); `buffed` is the game's own aggregate
		// with attachments folded in but before our composition -- ComposedAttribute
		// ::GetGame() while active, `game` itself while inactive. `buffed` != `base`
		// is what the "+mods" tag reports. Refreshed from Tick, read from RenderImGui.
		std::atomic<float> g_dbgAttrGame[kAttrCount];
		std::atomic<float> g_dbgAttrExpected[kAttrCount];
		std::atomic<float> g_dbgAttrBase[kAttrCount];
		std::atomic<float> g_dbgAttrBuffed[kAttrCount];

		// Grenade row readouts, parallel to g_dbgAttrGame/g_dbgAttrExpected
		// above but indexed by GrenadeRowIndex instead of AttrIndex -- these rows
		// aren't part of the per-gun kAttrs table (Charge Cost lives on a
		// different CDO type; Charge Capacity lives on a different attribute set),
		// so they get their own small parallel arrays instead of a slot in that
		// one. `unmod` is the same "captured original while active, live read
		// while not" value g_dbgWeaponBaseMagazine already computes for Magazine
		// Size, feeding RenderLiveValue's always-on "Unmodified" tooltip line.
		// Only ever populated while a grenade profile is the one equipped -- see
		// Tick()'s grenade block.
		std::atomic<float> g_dbgGrenadeGame[kGrenadeRowCount];
		std::atomic<float> g_dbgGrenadeExpected[kGrenadeRowCount];
		std::atomic<float> g_dbgGrenadeUnmod[kGrenadeRowCount];

		// Reserve ammo is an inventory item, not an attribute, so an empty inventory
		// blocks reload whatever is written to attributes. Tops up only on an observed
		// decrease: a stale read can miss a top-up, never repeat one.
		std::atomic<bool>  g_autoRestock     { false };
		std::atomic<bool>  g_restockHidden   { true };    // prefer the hidden inventory: no visible slot
		std::atomic<float> g_restockMags     { 3.0f };    // size of the one-off seed, in magazines
		std::atomic<int>   g_restockFailures { 0 };       // consecutive adds that did not raise the count
		std::atomic<int>   g_restockTotal    { 0 };       // rounds replaced this session, for the readout

		// Edge-detection state. Reset whenever the ammo type changes, because a count of
		// pistol rounds says nothing about rifle rounds.
		std::atomic<SDK::UAuItemDataBase*> g_restockItem  { nullptr };
		std::atomic<int>                   g_restockLast  { -1 };
		std::atomic<bool>                  g_restockSeeded{ false };

		constexpr int   kRestockMaxFailures     = 3;
		constexpr int   kRestockSessionCap      = 20000;

		using AddNewItemFn = SDK::TArray<SDK::FAuAddedItem>*(__fastcall*)(
			SDK::UAuItemsComponent* self, SDK::TArray<SDK::FAuAddedItem>* result,
			const SDK::UAuItemDataBase* newItem, uint32_t amount);

		AddNewItemFn ResolveAddNewItem()
		{
			static AddNewItemFn fn = nullptr;
			static bool tried = false;
			if (tried) return fn;
			tried = true;

			const uintptr_t address = AOB::Resolved().AddNewItem;
			if (!address)
			{
				LOG_WARN("Weapons: auto-restock unavailable -- AddNewItem pattern unresolved.");
				return nullptr;
			}
			fn = reinterpret_cast<AddNewItemFn>(address);
			return fn;
		}

		// The ammo item is the CDO of the class the weapon declares it needs.
		SDK::UAuItemDataBase* ResolveAmmoItem(SDK::UCrWeaponItemDataBase* weaponData)
		{
			if (!weaponData) return nullptr;

			SDK::UClass* itemClass = weaponData->RequiredItem;
			if (!itemClass || !itemClass->ClassDefaultObject) return nullptr;

			SDK::UClass* base = SDK::UAuItemDataBase::StaticClass();
			if (!base || !itemClass->ClassDefaultObject->IsA(base)) return nullptr;

			return static_cast<SDK::UAuItemDataBase*>(itemClass->ClassDefaultObject);
		}

		void RestockEquippedAmmo(SDK::ACrCharacterPlayerBase* character,
		                         SDK::UCrWeaponComponent* ws, float maxMag)
		{
			if (g_restockFailures.load() >= kRestockMaxFailures) return;
			if (g_restockTotal.load()    >= kRestockSessionCap)  return;

			SDK::UAuItemDataBase* ammo = ResolveAmmoItem(ws->LastEquippedWeaponData);
			if (!ammo) return;   // tools and anything with no declared ammo type

			AddNewItemFn addNewItem = ResolveAddNewItem();
			if (!addNewItem) return;

			// The hidden inventory is tried first: if reloads can draw from it, the
			// reserve is real but costs no visible slot.
			const bool restockHidden = g_restockHidden.load();
			SDK::UCrInventoryComponent* target = restockHidden
				? character->HiddenInventoryComponent
				: character->InventoryComponent;
			if (!target) target = character->InventoryComponent;
			if (!target) return;

			const int have = character->GetItemCount(ammo);

			// Switching weapons switches ammo type. Re-baseline rather than comparing
			// rifle rounds against a pistol count.
			bool seeded = g_restockSeeded.load();
			int  last   = g_restockLast.load();
			if (ammo != g_restockItem.load())
			{
				g_restockItem.store(ammo);
				last = have;
				g_restockLast.store(last);
				seeded = false;
				g_restockSeeded.store(false);
			}

			int amount = 0;

			if (!seeded)
			{
				// One-off seed, so switching this on with empty pockets gives you
				// something to reload from. After this, only consumption drives it.
				const int want = static_cast<int>(maxMag * g_restockMags.load() + 0.5f);
				amount = (have < want) ? (want - have) : 0;
				g_restockSeeded.store(true);
			}
			else if (have < last)
			{
				// THE EVENT: ammo actually left the inventory. Replace exactly that
				// much -- never a target, never a guess.
				amount = last - have;
			}

			if (amount <= 0)
			{
				g_restockLast.store(have);   // count rose (picked up, crafted) -- just follow it
				return;
			}

			SDK::TArray<SDK::FAuAddedItem> added{};
			addNewItem(reinterpret_cast<SDK::UAuItemsComponent*>(target), &added,
				ammo, static_cast<uint32_t>(amount));

			int gained = 0;
			for (int32_t i = 0; i < added.Num(); ++i)
				gained += added[i].Amount;

			// Backstop for what the delta rule cannot see: if adds are not landing at
			// all, stop rather than trying harder. A feature that quietly does nothing
			// beats one that fills every slot you own.
			if (gained <= 0)
			{
				if (g_restockFailures.fetch_add(1) + 1 >= kRestockMaxFailures)
				{
					g_autoRestock.store(false);
					SessionConfig::Set("playerWeapons.autoRestock", false);
					LOG_WARN("Weapons: auto-restock disabled itself -- %d adds of '%s' returned "
						"nothing (target %s inventory).",
						kRestockMaxFailures, ammo->GetName().c_str(),
						restockHidden ? "hidden" : "visible");
				}
				g_restockLast.store(have);
				return;
			}

			g_restockFailures.store(0);
			g_restockTotal.fetch_add(gained);

			// Re-read rather than assuming have+gained. If the count lags a frame the
			// worst case is a missed top-up next frame, not a repeated one.
			g_restockLast.store(character->GetItemCount(ammo));
		}

		// Written from Tick (weapon detection); read from RenderImGui for tab focus.
		std::atomic<int> g_activeProfile{ -1 };   // index into g_profiles, -1 = nothing equipped
		bool             g_configApplied = false; // set once ApplySavedConfig has run; game-thread only

		// One slot per weapon attribute, not per profile: character->WeaponAttributes
		// is a single set shared across whatever weapon is equipped (see the weapon
		// attachment audit), so switching weapons must not reset the composed state.
		// Game-thread only.
		BetterCheats::ComposedAttribute g_composed[kAttrCount];
		SDK::UCrWeaponAttributeSet*     g_composedOwner = nullptr;

		// Magazine Size composes separately from the other nine rows: the clip-size
		// getter reads LastEquippedWeaponData->BaseMagazine.Value directly (a plain
		// FScalableFloat.Value on the weapon TYPE's data asset CDO), not the GAS
		// MaxMagAmmoModOffset this row used to write. A magazine size can't
		// sensibly go to zero or negative, or run away unbounded if something
		// stacks badly, so it gets its own sanity clamp instead of the row's
		// own -100..999 slider range (that range bounds the OFFSET the slider
		// picks, not the result).
		constexpr float kMagazineFloor   = 1.0f;
		constexpr float kMagazineCeiling = 9999.0f;
		BetterCheats::ComposedAttribute g_composedMagazine;
		ObjectRef<SDK::UCrWeaponItemDataBase> g_magazineOwner;

		// Charge Cost: same per-weapon-TYPE-CDO shape as Magazine Size above, just
		// a plain float member instead of FScalableFloat.Value.
		BetterCheats::ComposedAttribute    g_composedGrenadeCost;
		ObjectRef<SDK::UCrGrenadeWeaponItemDataBase> g_grenadeCostOwner;

		// The last grenade the grenade rows applied to, so they keep following it
		// once the grenade leaves the player's hand (see Tick()).
		ObjectRef<SDK::UCrGrenadeWeaponItemDataBase> g_lastGrenadeData;

		// Grenade Damage row: scales BaseDamage.Value on the grenade TYPE's data
		// asset (the explosion reads it -- see the GrenadeDamageProbe log lines).
		// Same per-type-CDO shape as Charge Cost above.
		BetterCheats::ComposedAttribute g_composedGrenadeBaseDamage;
		ObjectRef<SDK::UCrGrenadeWeaponItemDataBase> g_grenadeBaseDamageOwner;
		constexpr float kGrenadeBaseDamageFinalMax = 1000000.0f;
		constexpr const char* kGrenadeBaseDamageKey = "playerWeapons.compose.grenadeBaseDamage";

		// Charge Capacity: a GAS attribute on character->GrenadeChargeAttributes, ONE
		// shared instance regardless of grenade type -- same "shared instance,
		// respawn-only Forget" shape as g_composed[]/g_composedOwner above, not the
		// per-type owner-swap-on-Release shape Magazine/Charge Cost need.
		BetterCheats::ComposedAttribute    g_composedGrenadeMax;
		SDK::UCrGrenadeChargeAttributeSet* g_grenadeAttrOwner = nullptr;

		// Forgets every composed slot the moment the attribute set instance changes
		// (respawn, world change) -- before anything below tries to Apply/Release
		// against what may already be gone. Also drops the magazine and grenade-
		// cost slots: unlike `weapons`, their owners are per-weapon-TYPE data
		// assets, not the character, but a respawn is exactly the kind of "can't
		// trust what we think we own any more" event that should forget them too
		// rather than try to Release() against a data asset we can no longer be
		// sure is still valid.
		void ForgetComposedIfOwnerChanged(SDK::UCrWeaponAttributeSet* current)
		{
			if (current == g_composedOwner) return;
			for (int a = 0; a < kAttrCount; ++a)
				g_composed[a].Forget();
			g_composedOwner = current;

			g_composedMagazine.Forget();
			g_magazineOwner.Reset();

			g_composedGrenadeCost.Forget();
			g_grenadeCostOwner.Reset();

			g_composedGrenadeBaseDamage.Forget();
			g_grenadeBaseDamageOwner.Reset();

			g_composedGrenadeMax.Forget();
			g_grenadeAttrOwner = nullptr;
		}

		// Unlike `weapons` (one shared instance regardless of what's equipped),
		// LastEquippedWeaponData is a different CDO per weapon TYPE -- switching
		// weapons must restore the PREVIOUS one's magazine before adopting the new
		// one's, or the old weapon stays offset forever (ComposedAttribute's own
		// owner-change handling only ever recaptures on the new owner, it never
		// writes back to an owner it's leaving).
		//
		// The previous owner is held across ticks and can be unloaded in the
		// meantime, so it's an ObjectRef: gone means nothing to hand back.
		template <class Owner, class ValueOf>
		void ReleaseComposedIfOwnerChanged(BetterCheats::ComposedAttribute& composed, ObjectRef<Owner>& ownerRef,
			Owner* current, ValueOf valueOf, const char* key)
		{
			Owner* previous = ownerRef.Get();
			if (current == previous) return;
			if (previous)
			{
				composed.Release(previous, valueOf(previous));
				BetterCheats::ClearComposeState(key);
			}
			else
			{
				composed.Forget();
			}
			ownerRef.Set(current);
		}

		void ReleaseMagazineIfOwnerChanged(SDK::UCrWeaponItemDataBase* current)
		{
			ReleaseComposedIfOwnerChanged(g_composedMagazine, g_magazineOwner, current,
				[](SDK::UCrWeaponItemDataBase* owner) -> float& { return owner->BaseMagazine.Value; },
				"playerWeapons.compose.magazine");
		}

		// Same reasoning as ReleaseMagazineIfOwnerChanged: Charge Cost lives on the
		// grenade TYPE's own CDO, not the character, so switching away from a
		// grenade (to a gun, a tool, or a different grenade type) must restore the
		// one being left before adopting the next.
		void ReleaseGrenadeCostIfOwnerChanged(SDK::UCrGrenadeWeaponItemDataBase* current)
		{
			ReleaseComposedIfOwnerChanged(g_composedGrenadeCost, g_grenadeCostOwner, current,
				[](SDK::UCrGrenadeWeaponItemDataBase* owner) -> float& { return owner->GrenadeThrowCostOfGrenadeCharge; },
				"playerWeapons.compose.grenadeCost");
		}

		// Same per-grenade-TYPE shape for the Damage row above.
		void ReleaseGrenadeBaseDamageIfOwnerChanged(SDK::UCrGrenadeWeaponItemDataBase* current)
		{
			ReleaseComposedIfOwnerChanged(g_composedGrenadeBaseDamage, g_grenadeBaseDamageOwner, current,
				[](SDK::UCrGrenadeWeaponItemDataBase* owner) -> float& { return owner->BaseDamage.Value; },
				kGrenadeBaseDamageKey);
		}

		// Hands the stock BaseDamage back. The grenade data asset is shared by every
		// holder of that grenade type and outlives the world, so a scaled value left
		// in it would carry into the next session. Game thread only.
		void ReleaseGrenadeBaseDamageNow()
		{
			if (SDK::UCrGrenadeWeaponItemDataBase* owner = g_grenadeBaseDamageOwner.Get())
			{
				g_composedGrenadeBaseDamage.Release(owner, owner->BaseDamage.Value);
				BetterCheats::ClearComposeState(kGrenadeBaseDamageKey);
			}
			else
			{
				g_composedGrenadeBaseDamage.Forget();
			}
		}

		constexpr const char* kChimeraMainWorldName = "ChimeraMain";

		void OnBeforeWorldEndPlay(SDK::UWorld* /*world*/, const char* worldName)
		{
			if (!worldName || std::strcmp(worldName, kChimeraMainWorldName) != 0)
				return;

			// Off the game thread there is nothing safe to write; SessionConfig keeps
			// the pair RestoreIfStale needs to undo the leftover on the next activation.
			if (!BetterCheats::IsGameThread())
			{
				g_composedGrenadeBaseDamage.Forget();
				return;
			}

			try { ReleaseGrenadeBaseDamageNow(); }
			catch (...) {}
		}

		// Charge Capacity is ONE instance across every grenade type (like
		// WeaponAttributes does for guns), so a weapon SWITCH must Release, not
		// Forget -- handled in Tick() via `active`. Forget only belongs
		// here, on a genuine instance change (respawn, world change).
		void ForgetGrenadeAttrsIfOwnerChanged(SDK::UCrGrenadeChargeAttributeSet* current)
		{
			if (current == g_grenadeAttrOwner) return;
			g_composedGrenadeMax.Forget();
			g_grenadeAttrOwner = current;
		}

		bool IsActive(int attr, float value)
		{
			return BetterCheats::DiffersFromDefault(value, kAttrs[attr].defaultValue);
		}

		// ComposeStep/ApplyComposedRow: the capture/write/restore/persist dance
		// every composed row in this file shares -- a BetterCheats:: helper in
		// attribute_compose.h, found here via unqualified enclosing-namespace
		// lookup, same as ComposedAttribute itself.

		std::string ToLower(const std::string& in)
		{
			std::string out = in;
			for (char& c : out)
				if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
			return out;
		}

		// The mining and building tools are equipped through the same UCrWeaponComponent,
		// so they surface here too -- but they belong to the Tools and Building panels,
		// and half this attribute set (recoil, spread, sway, ADS) is meaningless on a
		// drill. Keep them out of this panel entirely.
		bool IsWeaponAsset(const std::string& raw)
		{
			const std::string lowered = ToLower(raw);
			return lowered.find("tool") == std::string::npos;
		}

		// The game's weapon roster, so every tab is present from the start and a weapon
		// can be tuned without first going and equipping it. Discovery still runs, so
		// anything not listed here still gets its own tab.
		const char* kBuiltInWeapons[] = {
			"Default__I_PistolDataItem_C",
			"Default__I_RifleDataItem_C",
			"Default__I_ShotgunDataItem_C",
			"Default__I_MachineGunDataItem_C",
		};
		constexpr int kBuiltInCount = static_cast<int>(sizeof(kBuiltInWeapons) / sizeof(kBuiltInWeapons[0]));

		// "Default__I_RifleDataItem_C" -> "Rifle";  "...MachineGunDataItem_C" -> "Machine Gun"
		std::string Prettify(const std::string& raw)
		{
			std::string s = raw;
			auto stripPrefix = [&](const char* p) {
				const size_t n = std::string(p).size();
				if (s.size() > n && s.compare(0, n, p) == 0) s.erase(0, n);
			};
			auto stripSuffix = [&](const char* p) {
				const size_t n = std::string(p).size();
				if (s.size() > n && s.compare(s.size() - n, n, p) == 0) s.erase(s.size() - n);
			};
			stripPrefix("Default__");
			stripPrefix("I_");
			stripPrefix("BP_");
			stripSuffix("_C");
			stripSuffix("DataItem");
			stripSuffix("Data");
			stripSuffix("Item");

			// split CamelCase into words
			std::string out;
			for (size_t i = 0; i < s.size(); ++i)
			{
				if (i > 0 && s[i] >= 'A' && s[i] <= 'Z' && !(s[i-1] >= 'A' && s[i-1] <= 'Z'))
					out += ' ';
				out += s[i];
			}
			return out.empty() ? raw : out;
		}

		std::string Sanitize(const std::string& raw)
		{
			std::string out;
			for (char c : ToLower(raw))
				if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
			return out.empty() ? "unknown" : out;
		}

		std::string ConfigKey(const Profile& p, const char* attrKey, const char* leaf)
		{
			return std::string("playerWeapons.w.") + p.key + "." + attrKey + "." + leaf;
		}

		void ResetProfileValues(Profile& p)
		{
			for (int a = 0; a < kAttrCount; ++a)
				p.values[a] = kAttrs[a].defaultValue;
			for (int g = 0; g < kGrenadeRowCount; ++g)
				p.grenadeValues[g] = kGrenadeRows[g].defaultValue;
		}

		void LoadProfileFromConfig(Profile& p)
		{
			if (!SessionConfig::IsLoaded())
				return;
			for (int a = 0; a < kAttrCount; ++a)
				p.values[a] = SessionConfig::Get(ConfigKey(p, kAttrs[a].key, "value"), p.values[a]);
			p.oneHitKill       = SessionConfig::Get(ConfigKey(p, "oneHitKill", "enabled"), false);
			p.infiniteMagazine = SessionConfig::Get(ConfigKey(p, "infiniteMagazine", "enabled"), false);

			for (int g = 0; g < kGrenadeRowCount; ++g)
				p.grenadeValues[g] = SessionConfig::Get(ConfigKey(p, kGrenadeRows[g].key, "value"), p.grenadeValues[g]);
			p.infiniteCharges = SessionConfig::Get(ConfigKey(p, "infiniteCharges", "enabled"), false);
		}

		// Caller must hold g_profilesMutex.
		void PersistKnownWeapons()
		{
			if (!SessionConfig::IsLoaded())
				return;
			nlohmann::json arr = nlohmann::json::array();
			for (const Profile& p : g_profiles)
				arr.push_back({ {"key", p.key}, {"display", p.display}, {"raw", p.raw}, {"isGrenade", p.isGrenade} });
			SessionConfig::Set("playerWeapons.known", arr);
		}

		int FindOrCreateProfile(const std::string& rawName, bool isGrenade);

		// Make sure every known weapon has a tab, whether or not it has been equipped.
		// Caller must hold g_profilesMutex.
		void EnsureBuiltInProfiles()
		{
			static bool s_done = false;
			if (s_done) return;
			s_done = true;
			for (int i = 0; i < kBuiltInCount; ++i)
				FindOrCreateProfile(kBuiltInWeapons[i], false);
		}

		// Caller must hold g_profilesMutex.
		int FindOrCreateProfile(const std::string& rawName, bool isGrenade)
		{
			const std::string key = Sanitize(rawName);
			for (size_t i = 0; i < g_profiles.size(); ++i)
				if (g_profiles[i].key == key)
					return static_cast<int>(i);

			Profile p;
			p.key       = key;
			p.raw       = rawName;
			p.display   = Prettify(rawName);
			p.isGrenade = isGrenade;
			ResetProfileValues(p);
			g_profiles.push_back(p);
			LoadProfileFromConfig(g_profiles.back());
			PersistKnownWeapons();

			LOG_INFO("Weapons: discovered weapon '%s' -> tab '%s'%s", rawName.c_str(), p.display.c_str(),
				isGrenade ? " (grenade)" : "");
			return static_cast<int>(g_profiles.size()) - 1;
		}

		// Resolves this weapon TYPE's base stats from its own CDO, independent of
		// whether it's currently equipped -- profile.raw IS the CDO's object name
		// already, so this needs only FindFirstObjectByName, the same mechanism
		// ResolveCdoField (cdo_lookup.h) uses. Cached forever once found (base stats
		// don't change at runtime); re-resolved, at the same retry cadence, if the
		// CDO's package is ever unloaded out from under it. A grenade profile's
		// `raw` is a UCrGrenadeWeaponItemDataBase CDO, which upcasts to
		// UCrWeaponItemDataBase* safely (see the class comment above
		// kGrenadeGlobalCount) -- called uniformly for every profile, no
		// isGrenade branch needed.
		//
		// Game thread only (ObjectWalker touches GObjects). Called from Tick()
		// for every profile in g_profiles, not just the active one -- cheap: once
		// resolved, ObjectRef::Get() is a slot check, not a scan.
		void ResolveWeaponBaseFields(Profile& profile, float deltaSeconds)
		{
			if (profile.baseCdo.Get())
				return;   // resolved and still alive -- nothing to refresh

			if (!CdoRetryDue(profile.baseRetryCooldown, deltaSeconds))
				return;

			void* object = FindObjectByName(profile.raw.c_str());
			if (!object)
			{
				if (!profile.baseLoggedMiss)
				{
					LOG_WARN("Weapons: base CDO '%s' not found (yet) for tab '%s' -- showing no base stats until it resolves.",
						profile.raw.c_str(), profile.display.c_str());
					profile.baseLoggedMiss = true;
				}
				return;
			}

			auto* cdo = static_cast<SDK::UCrWeaponItemDataBase*>(object);
			profile.baseCdo.Set(cdo);
			profile.baseDamage          = cdo->BaseDamage.Value;
			profile.baseRoundsPerMinute = cdo->RoundsPerMinute.Value;
			profile.baseMagazine        = cdo->BaseMagazine.Value;
			profile.baseRange           = cdo->BaseRange.Value;
			profile.basePierce          = cdo->PossibleEnemiesHitPerTrace;
			profile.baseReloadTime      = cdo->ReloadTime.Value;
			profile.baseSpread          = cdo->Spread.Value;
			profile.baseAdsSpeed        = cdo->AimCameraSpeed.Value;
			profile.baseResolved        = true;
			LOG_INFO("Weapons: resolved base stats for '%s' (tab '%s').", profile.raw.c_str(), profile.display.c_str());
			if (profile.isGrenade)
				LOG_INFO("GrenadeDamageProbe: '%s' BaseDamage.Value = %.4f", profile.raw.c_str(), profile.baseDamage);
		}

		// Whether row `a` has a single real stat this plugin can convert its
		// multiplier into -- false for Recoil/Sway, which the game splits into
		// separate horizontal/vertical CDO fields with no unified value to
		// multiply (see weapon-tabs-research.md); those two stay shown as a
		// plain "x1.50" multiplier, same as before this pass.
		constexpr bool kAttrHasRealBase[kAttrCount] = {
			true,   // Damage
			true,   // Fire Rate
			true,   // Reload Speed
			true,   // Magazine Size (already real -- Add mode onto a real field)
			true,   // Damage Falloff (base effective range before falloff)
			false,  // Recoil
			true,   // Spread
			false,  // Sway
			true,   // ADS Speed
			true,   // Enemies Hit/Shot (already real -- Absolute mode, a literal count)
		};

		// This weapon's real per-stat base value, resolved once from its CDO
		// (see ResolveWeaponBaseFields) -- 0 if that hasn't happened yet, or if
		// this row has no such field (kAttrHasRealBase false).
		//
		// Fire Rate: RoundsPerMinute.Value is a per-shot interval in seconds
		// despite its name, not a rounds-per-minute count -- a rifle reads
		// ~0.13 there, i.e. 60 / 0.13 =~ 461 RPM. The engine source isn't
		// available (the SDK dump has signatures, not bodies), so this reading
		// is inferred from observed values.
		//
		// Reload Speed: ReloadTime.Value is shown multiplied by the slider
		// (real = base x effective multiplier), like every other row here. It
		// is not confirmed whether the game applies ReloadSpeedModMultiplier
		// to the duration directly or inversely ("higher is faster" on a
		// duration would want the inverse).
		float RealBaseForAttr(const Profile& profile, int a)
		{
			if (!profile.baseResolved) return 0.0f;
			switch (a)
			{
			case kAttrDamage:   return profile.baseDamage;
			case kAttrFireRate: return (profile.baseRoundsPerMinute > 0.0f) ? 60.0f / profile.baseRoundsPerMinute : 0.0f;
			case kAttrReload:   return profile.baseReloadTime;
			case kAttrMagazine: return profile.baseMagazine;
			case kAttrFalloff:  return profile.baseRange;
			case kAttrSpread:   return profile.baseSpread;
			case kAttrADS:      return profile.baseAdsSpeed;
			case kAttrPierce:   return static_cast<float>(profile.basePierce);
			default:            return 0.0f;   // Recoil, Sway
			}
		}

		// LastEquippedWeaponData (used below to pick the active profile) only becomes a
		// grenade once one has actually been thrown/charged, so a grenade that has only
		// ever been equipped never gets a tab. GetGrenadeItemData reports whatever grenade
		// is currently equipped regardless of charge state -- game-thread only, like every
		// other UObject/ProcessEvent call here.
		SDK::UCrGrenadeWeaponItemDataBase* GetEquippedGrenadeItemData(SDK::UCrWeaponComponent* weaponSystem)
		{
			if (!weaponSystem) return nullptr;

			static SDK::UFunction* func = nullptr;
			if (!func)
				func = SDK::UCrWeaponComponent::StaticClass()->GetFunction("CrWeaponComponent", "GetGrenadeItemData");
			if (!func)
			{
				LOG_WARN("Weapons: could not resolve UCrWeaponComponent::GetGrenadeItemData.");
				return nullptr;
			}

			SDK::Params::CrWeaponComponent_GetGrenadeItemData parms{};
			const auto flags = func->FunctionFlags;
			func->FunctionFlags |= 0x400;
			weaponSystem->ProcessEvent(func, &parms);
			func->FunctionFlags = flags;

			return parms.ReturnValue ? parms.OutGrenadeData : nullptr;
		}

	}

	void Tick(float deltaSeconds)
	{
		BetterCheats::RecordGameThread();

		// ApplySavedConfig() is driven by save load, which a hot-reloaded plugin has
		// already missed. plugin.cpp does call OnExperienceLoadComplete() on reload, but
		// from PluginInit's thread -- and SessionConfig::Reload() resolves the save name
		// through UCrSaveSubsystem, which needs the game thread. It returns false there,
		// so the config silently never loads and NOTHING persists. Retry from the engine
		// tick, which is the game thread.
		if (!g_configApplied && SessionConfig::IsLoaded())
			ApplySavedConfig();

		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			EnsureBuiltInProfiles();

			// Every profile, not just the active one -- see ResolveWeaponBaseFields.
			// Cheap once resolved; still cheap unresolved, since it's cooldown-gated
			// the same way the grenade-globals resolver above is.
			for (Profile& p : g_profiles)
				ResolveWeaponBaseFields(p, deltaSeconds);
		}

		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		if (!character || !character->WeaponSystem)
			return;

		// Tab presence for whatever grenade is currently equipped, independent of the
		// active-profile selection below -- see GetEquippedGrenadeItemData.
		SDK::UCrGrenadeWeaponItemDataBase* slotGrenade = GetEquippedGrenadeItemData(character->WeaponSystem);
		if (slotGrenade)
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			FindOrCreateProfile(slotGrenade->GetName(), true);
		}

		if (SDK::UCrWeaponItemDataBase* data = character->WeaponSystem->LastEquippedWeaponData)
		{
			const std::string raw = data->GetName();

			// Holding a tool leaves no active weapon profile, so nothing is applied.
			if (IsWeaponAsset(raw))
			{
				const bool isGrenade = data->IsA(SDK::UCrGrenadeWeaponItemDataBase::StaticClass());
				std::lock_guard<std::mutex> lock(g_profilesMutex);
				g_activeProfile.store(FindOrCreateProfile(raw, isGrenade));
			}
			else
			{
				g_activeProfile.store(-1);
			}
		}

		// Copy the active profile out under the lock instead of keeping a reference
		// into g_profiles across the SDK calls below -- a newly discovered weapon can
		// push_back and reallocate the vector from another Tick call. This also
		// used to `return` here whenever no weapon profile was active (holding a
		// tool, nothing equipped), which silently killed the live-values readout
		// below entirely -- unlike Movement, whose
		// equivalent update has no such gate and always runs. Fall back to an
		// all-default profile instead: IsActive() reads false for every row, so
		// the compose loop below Releases everything (correct -- nothing should be
		// overridden with no weapon out) while still updating every debug atomic.
		Profile profile;
		bool haveActiveProfile = false;
		bool anyInfiniteCharges = false;
		int  activeIndex = -1;
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			for (const Profile& p : g_profiles)
				if (p.isGrenade && p.infiniteCharges)
					anyInfiniteCharges = true;
			const int active = g_activeProfile.load();
			if (active >= 0 && active < static_cast<int>(g_profiles.size()))
			{
				profile = g_profiles[active];
				haveActiveProfile = true;
				activeIndex = active;
			}
		}
		if (!haveActiveProfile)
			ResetProfileValues(profile);   // oneHitKill/infiniteMagazine already default false

		// This tick's mods delta per row (unmodified - base, same multiplier
		// space g_dbgAttrBuffed/g_dbgAttrBase capture) -- written back into
		// g_profiles[activeIndex].cachedModsMult below, once the SDK calls that
		// fill it are done, so an unequipped tab can still show a best-effort
		// estimate of this weapon's real numbers (see RealBaseForAttr/the
		// estimate branch in RenderImGui) instead of nothing at all. Only
		// Multiply-mode rows carry a meaningful mods concept here.
		float modsMultThisTick[kAttrCount] = {};

		try
		{
			SDK::UCrWeaponAttributeSet* weapons = character->WeaponAttributes;
			ForgetComposedIfOwnerChanged(weapons);

			if (weapons)
			{
				for (int a = 0; a < kAttrCount; ++a)
				{
					if (a == kAttrMagazine)
						continue;   // composes onto the weapon data asset instead -- see below

					SDK::FGameplayAttributeData& attr = weapons->*kAttrs[a].member;
					const std::string key = std::string("playerWeapons.compose.") + kAttrs[a].key;

					// One Hit Kill: Absolute wins outright (ignores baseValue); Release()
					// hands Damage back to the game's own aggregate once this turns off.
					const bool oneHitKill = a == kAttrDamage && profile.oneHitKill;
					const bool active     = oneHitKill || IsActive(a, profile.values[a]);
					const ComposeStep step = oneHitKill
						? ApplyComposedRow(g_composed[a], weapons, attr.CurrentValue, key, active,
							kOneHitKillDamage, BetterCheats::ComposedAttribute::Mode::Absolute,
							kOneHitKillDamage, kOneHitKillDamage, &attr.BaseValue)
						: ApplyComposedRow(g_composed[a], weapons, attr.CurrentValue, key, active,
							profile.values[a], kAttrModes[a], kAttrs[a].minValue, kAttrs[a].maxValue,
							&attr.BaseValue);
					const float gameBefore = step.game;
					// On the very first activation frame there is no previous write to
					// compare against yet -- expected is trivially "game" until next tick.
					const float expected = step.expected;
					// "Buffed" (base + attachments, ours excluded): GetGame() reflects
					// what Apply() just composed from -- valid once active. Inactive rows
					// never had that captured, but CurrentValue is already untouched by
					// us, so it already IS base + attachments.
					const float buffed = active ? g_composed[a].GetGame() : gameBefore;
					g_dbgAttrGame[a].store(gameBefore);
					g_dbgAttrExpected[a].store(expected);
					g_dbgAttrBase[a].store(attr.BaseValue);
					g_dbgAttrBuffed[a].store(buffed);

					if (kAttrModes[a] == BetterCheats::ComposedAttribute::Mode::Multiply)
						modsMultThisTick[a] = buffed - attr.BaseValue;
				}

				if (!g_loggedTickFill.exchange(true))
					LOG_INFO("Weapons: live values first filled by Tick (weapon '%s').", profile.display.c_str());

				// Write back this weapon's own mods snapshot now, while its index
				// is still valid (g_profiles only ever grows, never shrinks or
				// reorders, so activeIndex captured above still names the same
				// weapon) -- see the estimate branch in RenderImGui.
				if (haveActiveProfile)
				{
					std::lock_guard<std::mutex> lock(g_profilesMutex);
					if (activeIndex >= 0 && activeIndex < static_cast<int>(g_profiles.size()))
						for (int a = 0; a < kAttrCount; ++a)
							if (kAttrModes[a] == BetterCheats::ComposedAttribute::Mode::Multiply)
								g_profiles[activeIndex].cachedModsMult[a] = modsMultThisTick[a];
				}
			}

			// Magazine Size: composes (Add) onto LastEquippedWeaponData->BaseMagazine
			// .Value, the weapon TYPE's own data asset field the clip-size getter
			// actually reads -- not a GAS attribute, so it's outside the
			// `weapons`-gated loop above and keyed by the data asset, not the character.
			SDK::UCrWeaponItemDataBase* equippedData = character->WeaponSystem->LastEquippedWeaponData;
			ReleaseMagazineIfOwnerChanged(equippedData);

			if (equippedData)
			{
				const bool active = IsActive(kAttrMagazine, profile.values[kAttrMagazine]);
				const ComposeStep step = ApplyComposedRow(g_composedMagazine, equippedData,
					equippedData->BaseMagazine.Value, "playerWeapons.compose.magazine",
					active, profile.values[kAttrMagazine], BetterCheats::ComposedAttribute::Mode::Add,
					kMagazineFloor, kMagazineCeiling);

				g_dbgAttrGame[kAttrMagazine].store(step.game);
				g_dbgAttrExpected[kAttrMagazine].store(step.expected);

				// Weapon base stats -- real CDO fields the game itself uses, never
				// written by us except BaseMagazine, so show the ORIGINAL we captured
				// rather than a live read that would show our own offset while active.
				g_dbgWeaponBaseDamage.store(equippedData->BaseDamage.Value);
				g_dbgWeaponRoundsPerMinute.store(equippedData->RoundsPerMinute.Value);
				g_dbgWeaponBaseRange.store(equippedData->BaseRange.Value);
				g_dbgWeaponBaseMagazine.store(active ? g_composedMagazine.GetGame() : step.game);
			}

			// Grenade rows. Charge Cost lives on the equipped grenade
			// TYPE's own data asset (UCrGrenadeWeaponItemDataBase extends
			// UCrWeaponItemDataBase, so LastEquippedWeaponData covers it too) --
			// same per-type-CDO shape as BaseMagazine above. Charge Capacity is a GAS
			// attribute on character->GrenadeChargeAttributes, ONE shared instance
			// regardless of grenade type -- same "shared instance, active profile
			// only" convention as WeaponAttributes above. Both, and Infinite
			// Charges below, apply only while a grenade profile is the one
			// currently equipped, matching every other per-weapon row in this
			// file: switching to a gun or tool releases them exactly like
			// switching weapons releases Damage/Fire Rate/etc.
			//
			// Spending the last charge takes the grenade out of the player's hand, so
			// "equipped" alone would drop every grenade row at 0 charges -- exactly
			// when Charge Capacity, Charge Cost and Infinite Charges are needed. When no
			// grenade is in hand the rows follow the grenade the weapon component
			// reports, or failing that the last one seen.
			SDK::UCrGrenadeWeaponItemDataBase* grenadeData =
				(equippedData && equippedData->IsA(SDK::UCrGrenadeWeaponItemDataBase::StaticClass()))
					? static_cast<SDK::UCrGrenadeWeaponItemDataBase*>(equippedData)
					: slotGrenade;
			if (!grenadeData)
				grenadeData = g_lastGrenadeData.Get();
			Profile grenadeProfile;
			ResetProfileValues(grenadeProfile);
			if (grenadeData)
			{
				g_lastGrenadeData.Set(grenadeData);
				if (profile.isGrenade)
					grenadeProfile = profile;
				else
				{
					// By name every tick: g_profiles is rebuilt on a config load, so a
					// cached index could point at another weapon.
					std::lock_guard<std::mutex> lock(g_profilesMutex);
					const int index = FindOrCreateProfile(grenadeData->GetName(), true);
					if (index >= 0 && index < static_cast<int>(g_profiles.size()))
						grenadeProfile = g_profiles[index];
				}
			}
			ReleaseGrenadeCostIfOwnerChanged(grenadeData);
			ReleaseGrenadeBaseDamageIfOwnerChanged(grenadeData);

			if (grenadeData)
			{
				// Final clamp is not the slider range: it bounds the written BaseDamage,
				// which has its own real-world magnitude.
				const float damageScale = grenadeProfile.grenadeValues[kGrenadeDamage];
				const bool  damageActive = BetterCheats::DiffersFromDefault(damageScale, kGrenadeRows[kGrenadeDamage].defaultValue);
				const ComposeStep damageStep = ApplyComposedRow(g_composedGrenadeBaseDamage, grenadeData,
					grenadeData->BaseDamage.Value, kGrenadeBaseDamageKey,
					damageActive, damageScale,
					kGrenadeModes[kGrenadeDamage], 0.0f, kGrenadeBaseDamageFinalMax);

				g_dbgGrenadeGame[kGrenadeDamage].store(damageStep.game);
				g_dbgGrenadeExpected[kGrenadeDamage].store(damageStep.expected);
				g_dbgGrenadeUnmod[kGrenadeDamage].store(damageActive ? g_composedGrenadeBaseDamage.GetGame() : damageStep.game);

				const bool active = BetterCheats::DiffersFromDefault(
					grenadeProfile.grenadeValues[kGrenadeCost], kGrenadeRows[kGrenadeCost].defaultValue);
				const ComposeStep step = ApplyComposedRow(g_composedGrenadeCost, grenadeData,
					grenadeData->GrenadeThrowCostOfGrenadeCharge, "playerWeapons.compose.grenadeCost",
					active, grenadeProfile.grenadeValues[kGrenadeCost], kGrenadeModes[kGrenadeCost],
					kGrenadeRows[kGrenadeCost].minValue, kGrenadeRows[kGrenadeCost].maxValue);

				g_dbgGrenadeGame[kGrenadeCost].store(step.game);
				g_dbgGrenadeExpected[kGrenadeCost].store(step.expected);
				g_dbgGrenadeUnmod[kGrenadeCost].store(active ? g_composedGrenadeCost.GetGame() : step.game);
			}

			SDK::UCrGrenadeChargeAttributeSet* grenadeAttrs = character->GrenadeChargeAttributes;
			ForgetGrenadeAttrsIfOwnerChanged(grenadeAttrs);

			// Before the top-up below, so an Infinite Charges refill can't hide the drop.
			ProbeGrenadeThrow(grenadeAttrs, grenadeData != nullptr, deltaSeconds,
				grenadeData ? grenadeProfile.grenadeValues[kGrenadeDamage] : 1.0f,
				grenadeData ? grenadeData->BaseDamage.Value : 0.0f);

			if (grenadeAttrs)
			{
				const GrenadeRowDef& def = kGrenadeRows[kGrenadeMax];
				float& value = (grenadeAttrs->*def.member).CurrentValue;
				const bool active = grenadeData && BetterCheats::DiffersFromDefault(grenadeProfile.grenadeValues[kGrenadeMax], def.defaultValue);
				const std::string key = std::string("playerWeapons.compose.") + def.key;

				const ComposeStep step = ApplyComposedRow(g_composedGrenadeMax, grenadeAttrs, value, key,
					active, grenadeProfile.grenadeValues[kGrenadeMax], kGrenadeModes[kGrenadeMax], def.minValue, def.maxValue);

				g_dbgGrenadeGame[kGrenadeMax].store(step.game);
				g_dbgGrenadeExpected[kGrenadeMax].store(step.expected);
				g_dbgGrenadeUnmod[kGrenadeMax].store(active ? g_composedGrenadeMax.GetGame() : step.game);

				// Infinite Charges: tops CurrentGrenadeCharge up to Max when it
				// drops, exactly like Infinite magazine tops SetEquippedWeaponCurrentAmmo
				// below -- never an unconditional write every tick (which would
				// fight a regen-style LEM/buff), only a top-up on the edge where
				// it's short of whatever Max is currently honouring (the game's
				// own, or our Charge Capacity override just above).
				//
				// Not gated on a grenade being in hand: spending the last charge
				// takes the grenade out of the player's hand, and the charge
				// set is shared by every grenade type, so the toggle on any
				// grenade tab keeps it full. The game's cost effect works on
				// the base value, so that is topped up with the current value
				// or the next recalculation would drop it back.
				if (anyInfiniteCharges)
				{
					const float maxCharge = grenadeAttrs->MaxGrenadeCharge.CurrentValue;
					SDK::FGameplayAttributeData& charge = grenadeAttrs->CurrentGrenadeCharge;
					if (maxCharge > 0.0f && (charge.CurrentValue < maxCharge || charge.BaseValue < maxCharge))
					{
						charge.CurrentValue = maxCharge;
						charge.BaseValue    = maxCharge;
					}
				}
			}

			// Fuse / Blast Radius / Throw Force: global, not gated by whether a
			// grenade is currently equipped -- see the GrenadeGlobalDef comment
			// above for why.
			{
				IPluginHooks* hooks = GetHooks();
				IPluginObjectProperties* props = hooks ? hooks->ObjectProperties : nullptr;

				for (int g = 0; g < kGrenadeGlobalCount; ++g)
				{
					const GrenadeGlobalDef& def = kGrenadeGlobals[g];
					CdoFieldHandle& h = g_grenadeGlobalHandle[g];
					SDK::UObject* object = props ? ResolveCdoField(h, def.className, def.propertyName, deltaSeconds, "Weapons") : nullptr;
					if (!object)
						continue;

					double raw = 0.0;
					if (!props->GetFloatProperty(object, h.property, &raw))
						continue;

					float valueF = static_cast<float>(raw);
					const float sliderValue = g_grenadeGlobalValue[g].load();
					const bool  active = BetterCheats::DiffersFromDefault(sliderValue, def.defaultValue);
					const std::string key = std::string("playerWeapons.compose.") + def.key;

					const ComposeStep step = ApplyComposedRow(g_composedGrenadeGlobal[g], object, valueF, key,
						active, sliderValue, BetterCheats::ComposedAttribute::Mode::Multiply,
						kGrenadeGlobalFinalMin, kGrenadeGlobalFinalMax);

					g_dbgGrenadeGlobalGame[g].store(step.game);
					g_dbgGrenadeGlobalExpected[g].store(step.expected);

					if (raw != static_cast<double>(valueF))
						props->SetFloatProperty(object, h.property, static_cast<double>(valueF));
				}
			}


			// Ammo (the reserve pool) is Net/RepNotify and reverts on the next replication
			// tick if written directly; the unreplicated Cr multiplier is what sticks.
			// Holding the magazine at full is what survives: it never empties, so it never
			// reloads, so the reserve is never drawn from.
			SDK::UCrWeaponComponent* ws = character->WeaponSystem;

			const float maxMag = ws->GetEquippedWeaponMaxMagazineAmmo();
			const float curMag = ws->GetEquippedWeaponCurrentAmmo();
			g_dbgMag.store(curMag);
			g_dbgMagMax.store(maxMag);
			g_dbgReserve.store(ws->GetEquippedWeaponAmmoInInventory());
			g_dbgReserveMax.store(ws->GetEquippedWeaponMaxAmmo());

			if (g_allInfiniteMagazine.load() || profile.infiniteMagazine)
			{
				if (maxMag > 0.0f && curMag < maxMag)
					ws->SetEquippedWeaponCurrentAmmo(maxMag);
			}

			// Auto-restock. Called every frame, but it only WRITES on the edge where
			// the ammo count has actually dropped -- see the note above.
			if (g_autoRestock.load() && maxMag > 0.0f)
				RestockEquippedAmmo(character, ws, maxMag);

		}
		catch (...) {}
	}

	void Initialize()
	{
		if (IPluginSelf* self = GetSelf())
			self->hooks->World->RegisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);
	}

	void Shutdown()
	{
		if (IPluginSelf* self = GetSelf())
			self->hooks->World->UnregisterOnBeforeWorldEndPlay(&OnBeforeWorldEndPlay);

		// The loader's own RELOAD button runs PluginShutdown from its D3D Present
		// hook, not the game thread Tick() recorded -- GetLocalCharacter() and
		// every UObject touch below intermittently crash there (see player_lookup.h).
		// Forget instead of Release: SessionConfig still has whatever RestoreIfStale
		// needs to undo a leftover write on the next activation, off-thread or not.
		if (!BetterCheats::IsGameThread())
		{
			for (int a = 0; a < kAttrCount; ++a)
				g_composed[a].Forget();
			g_composedMagazine.Forget();
			g_composedGrenadeCost.Forget();
			g_composedGrenadeBaseDamage.Forget();
			g_composedGrenadeMax.Forget();
			for (int g = 0; g < kGrenadeGlobalCount; ++g)
				g_composedGrenadeGlobal[g].Forget();
			return;
		}

		// Same character the composed state was captured against -- safe to hand
		// CurrentValue back. A different or null character means the attribute set
		// this state refers to is already gone; ForgetComposedIfOwnerChanged would
		// just no-op the writes below anyway, but skip the SDK call entirely.
		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		SDK::UCrWeaponAttributeSet* weapons = character ? character->WeaponAttributes : nullptr;
		SDK::UCrGrenadeChargeAttributeSet* grenadeAttrs = character ? character->GrenadeChargeAttributes : nullptr;

		try
		{
			if (weapons && weapons == g_composedOwner)
			{
				for (int a = 0; a < kAttrCount; ++a)
				{
					if (a == kAttrMagazine) continue;
					g_composed[a].Release(weapons, (weapons->*kAttrs[a].member).CurrentValue);
					BetterCheats::ClearComposeState(std::string("playerWeapons.compose.") + kAttrs[a].key);
				}
			}
			else
			{
				for (int a = 0; a < kAttrCount; ++a)
					g_composed[a].Forget();
			}

			if (SDK::UCrWeaponItemDataBase* owner = g_magazineOwner.Get())
			{
				g_composedMagazine.Release(owner, owner->BaseMagazine.Value);
				BetterCheats::ClearComposeState("playerWeapons.compose.magazine");
			}
			else
			{
				g_composedMagazine.Forget();
			}

			if (SDK::UCrGrenadeWeaponItemDataBase* owner = g_grenadeCostOwner.Get())
			{
				g_composedGrenadeCost.Release(owner, owner->GrenadeThrowCostOfGrenadeCharge);
				BetterCheats::ClearComposeState("playerWeapons.compose.grenadeCost");
			}
			else
			{
				g_composedGrenadeCost.Forget();
			}

			ReleaseGrenadeBaseDamageNow();

			if (grenadeAttrs && grenadeAttrs == g_grenadeAttrOwner)
			{
				g_composedGrenadeMax.Release(grenadeAttrs, grenadeAttrs->MaxGrenadeCharge.CurrentValue);
				BetterCheats::ClearComposeState("playerWeapons.compose.grenadeMax");
			}
			else
			{
				g_composedGrenadeMax.Forget();
			}

			// Fuse/Blast Radius/Throw Force: global CDOs, not tied to the local
			// character at all, so no owner-swap check -- only whether the CDO
			// is still loaded.
			IPluginHooks* hooks = GetHooks();
			IPluginObjectProperties* props = hooks ? hooks->ObjectProperties : nullptr;
			for (int g = 0; g < kGrenadeGlobalCount; ++g)
			{
				CdoFieldHandle& h = g_grenadeGlobalHandle[g];
				SDK::UObject* object = h.ok ? h.object.Get() : nullptr;
				if (props && object)
				{
					double raw = 0.0;
					if (props->GetFloatProperty(object, h.property, &raw))
					{
						float valueF = static_cast<float>(raw);
						g_composedGrenadeGlobal[g].Release(object, valueF);
						if (raw != static_cast<double>(valueF))
							props->SetFloatProperty(object, h.property, static_cast<double>(valueF));
					}
					BetterCheats::ClearComposeState(std::string("playerWeapons.compose.") + kGrenadeGlobals[g].key);
				}
				else
				{
					g_composedGrenadeGlobal[g].Forget();
				}
			}
		}
		catch (...) {}
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		size_t profileCount = 0;
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);

			// Restore previously discovered weapons so their tabs exist before the
			// player re-equips them.
			g_profiles.clear();
			const nlohmann::json known = SessionConfig::Get("playerWeapons.known", nlohmann::json::array());
			if (known.is_array())
			{
				bool dropped = false;
				for (const auto& e : known)
				{
					Profile p;
					p.key       = e.value("key", std::string());
					p.display   = e.value("display", std::string());
					p.raw       = e.value("raw", std::string());
					p.isGrenade = e.value("isGrenade", false);
					if (p.key.empty()) continue;

					// Drop tools recorded by an earlier build before they were filtered out.
					if (!p.raw.empty() && !IsWeaponAsset(p.raw)) { dropped = true; continue; }

					if (p.display.empty()) p.display = p.key;
					ResetProfileValues(p);
					g_profiles.push_back(p);
					LoadProfileFromConfig(g_profiles.back());
				}
				if (dropped)
					PersistKnownWeapons();
			}

			// ApplySavedConfig() rebuilds the list from config, which may predate the
			// built-in roster -- re-seed so every weapon still has a tab.
			for (int i = 0; i < kBuiltInCount; ++i)
				FindOrCreateProfile(kBuiltInWeapons[i], false);

			profileCount = g_profiles.size();
		}

		g_allInfiniteMagazine.store(SessionConfig::Get("playerWeapons.allInfiniteMagazine", false));
		g_autoRestock.store(SessionConfig::Get("playerWeapons.autoRestock", false));
		g_restockHidden.store(SessionConfig::Get("playerWeapons.restockHidden", true));
		g_restockMags.store(SessionConfig::Get("playerWeapons.restockMags", 3.0f));

		for (int g = 0; g < kGrenadeGlobalCount; ++g)
			g_grenadeGlobalValue[g].store(SessionConfig::Get(
				std::string("playerWeapons.") + kGrenadeGlobals[g].key + ".value", kGrenadeGlobals[g].defaultValue));

		g_configApplied = true;

		LOG_INFO("Weapons: applied saved config for session '%s' (%zu known weapons).",
			SessionConfig::GetSessionName().c_str(), profileCount);
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		if (!g_loggedRender.exchange(true))
			LOG_INFO("Weapons: live values readout rendering (%d rows).", kAttrCount);

		imgui->TextDisabled("Live values show the weapon in your hand; other tabs show an estimate.");
		imgui->TextDisabled("Multipliers apply to the weapon's base stats. Attachments still add on top.");

		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			EnsureBuiltInProfiles();
		}

		imgui->SeparatorText("Ammo");

		bool allInfiniteMagazine = g_allInfiniteMagazine.load();
		if (imgui->Checkbox("Infinite clip - ALL weapons", &allInfiniteMagazine))
		{
			g_allInfiniteMagazine.store(allInfiniteMagazine);
			SessionConfig::Set("playerWeapons.allInfiniteMagazine", allInfiniteMagazine);
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Holds the clip at full. It never empties, so it never reloads -- and\n"
			                  "because it never reloads, your reserve is never drawn from either.\n\n"
			                  "Per-weapon equivalents are on each weapon's tab below.");

		bool autoRestock = g_autoRestock.load();
		if (imgui->Checkbox("Keep ammo stocked", &autoRestock))
		{
			g_autoRestock.store(autoRestock);
			SessionConfig::Set("playerWeapons.autoRestock", autoRestock);
			g_restockFailures.store(0);
			g_restockItem.store(nullptr);   // re-baseline and re-seed on next tick
			g_restockLast.store(-1);
			g_restockSeeded.store(false);
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Tops the equipped weapon's own ammo back up so you never run dry.\n"
			                  "The clip drains and you reload normally -- there is just always\n"
			                  "something to reload from.\n\n"
			                  "It follows whatever you are holding, using the ammo type the weapon\n"
			                  "itself declares, and only ever adds the shortfall.");

		if (autoRestock)
		{
			imgui->Indent(imgui->GetFrameHeight());

			bool restockHidden = g_restockHidden.load();
			if (imgui->Checkbox("Keep it out of my inventory", &restockHidden))
			{
				g_restockHidden.store(restockHidden);
				SessionConfig::Set("playerWeapons.restockHidden", restockHidden);
			}
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Stocks the hidden inventory instead of your visible one, so it costs\n"
				                  "no slot. If reloads cannot draw from there, untick this and it will\n"
				                  "use your normal inventory instead.");

			float availX = 0.0f, availY = 0.0f;
			imgui->GetContentRegionAvail(&availX, &availY);
			float textW = 0.0f, textH = 0.0f;
			imgui->CalcTextSize("-88.88", &textW, &textH, false, -1.0f);
			const float frameH = imgui->GetFrameHeight();

			imgui->SetNextItemWidth(textW + (frameH * 2.0f) + (frameH * 0.9f));
			float restockMags = g_restockMags.load();
			if (imgui->InputFloat("Clips to keep in reserve", &restockMags, 1.0f, 5.0f, "%.0f"))
			{
				if (restockMags < 1.0f)   restockMags = 1.0f;
				if (restockMags > 100.0f) restockMags = 100.0f;
				g_restockMags.store(restockMags);
				SessionConfig::Set("playerWeapons.restockMags", restockMags);
			}

			imgui->Unindent(imgui->GetFrameHeight());
		}

		const float dbgMagMax = g_dbgMagMax.load();
		if (dbgMagMax >= 0.0f)
		{
			char line[220];
			snprintf(line, sizeof(line), "  clip %.0f / %.0f      reserve %.0f / %.0f%s",
				g_dbgMag.load(), dbgMagMax, g_dbgReserve.load(), g_dbgReserveMax.load(),
				g_restockTotal.load() > 0 ? "      [restocking]" : "");
			imgui->TextDisabled(line);
		}

		// The "weapon base: dmg / rounds/min / mag / range" line used to live
		// here, sourced from globals Tick() only ever filled for the equipped
		// weapon -- so every tab showed whatever was in the player's hand. It
		// now lives inside each tab below, sourced from that profile's own
		// cached CDO fields (see ResolveWeaponBaseFields), which are correct
		// whether or not that weapon is the one currently equipped.

		imgui->Spacing();
		imgui->SeparatorText("Per-Weapon");
		imgui->TextDisabled("A value differing from the default is applied. Reset a row to turn it off.");
		imgui->Spacing();

		// Widest attribute label decides where the live-values readout starts, so
		// every row's readout lines up regardless of that row's own label length.
		const float labelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, kAttrCount,
			[](int a) { return kAttrs[a].label; });

		bool profilesEmpty = false;
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			profilesEmpty = g_profiles.empty();
		}
		if (profilesEmpty)
		{
			imgui->TextDisabled("No weapons seen yet - equip one and its tab will appear here.");
			return;
		}

		if (!imgui->BeginTabBar("##weapon_tabs", 0))
			return;

		static int s_lastFocused = -1;
		const int activeProfile = g_activeProfile.load();
		const bool focusChanged = (activeProfile != s_lastFocused);
		s_lastFocused = activeProfile;

		// Tabs are discovered in equip order, which is arbitrary. Present them in the
		// order the game itself progresses through them, with tools trailing.
		static const char* kTabOrder[] = { "pistol", "rifle", "shotgun", "machine" };
		constexpr int kTabOrderCount = static_cast<int>(sizeof(kTabOrder) / sizeof(kTabOrder[0]));

		// g_profiles is only ever touched under this lock, held for the whole tab loop
		// below because every Profile& taken from it stays live throughout.
		{
		std::lock_guard<std::mutex> profilesLock(g_profilesMutex);

		std::vector<int> order(g_profiles.size());
		for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
		std::sort(order.begin(), order.end(), [&](int a, int b)
		{
			auto rank = [](const std::string& raw) {
				const std::string lowered = ToLower(raw);
				for (int i = 0; i < kTabOrderCount; ++i)
					if (lowered.find(kTabOrder[i]) != std::string::npos) return i;
				return 100;   // anything unlisted (tools) sorts last
			};
			const int ra = rank(g_profiles[a].raw);
			const int rb = rank(g_profiles[b].raw);
			if (ra != rb) return ra < rb;
			return g_profiles[a].display < g_profiles[b].display;
		});

		for (int oi = 0; oi < static_cast<int>(order.size()); ++oi)
		{
			const int p = order[oi];
			Profile& profile = g_profiles[p];
			const bool isEquipped = (p == activeProfile);

			// "###key" keeps the ImGui ID stable while the visible label changes.
			char label[96];
			snprintf(label, sizeof(label), "%s%s###%s",
				profile.display.c_str(), isEquipped ? " *" : "", profile.key.c_str());

			const int tabFlags = (focusChanged && isEquipped) ? (1 << 1) : 0; // SetSelected
			if (!imgui->BeginTabItem(label, nullptr, tabFlags))
				continue;

			imgui->PushIDInt(p);

			if (profile.isGrenade)
			{
				// Grenade-specific layout replaces the standard weapon one entirely --
				// none of Damage/Fire Rate/Recoil/Spread/etc. mean anything for a
				// thrown charge, and neither do the gun presets above (they index
				// into kAttrs by attribute, all meaningless here).
				imgui->TextDisabled("Damage and charge cost are per grenade type. Fuse, blast radius and\n"
				                    "throw force are below.");

				// A preset writes only into THIS grenade profile's rows, same as the
				// weapon presets above do for kAttrs.
				imgui->TextDisabled("Presets:");
				for (int i = 0; i < kGrenadePresetCount; ++i)
				{
					const GrenadePreset& preset = kGrenadePresets[i];

					imgui->SameLine(0.0f, -1.0f);

					if (imgui->SmallButton(preset.label))
					{
						// A preset is a starting point, not a mode: clear the profile's
						// grenade rows first so leftovers from a previous preset don't
						// silently survive underneath.
						for (int g = 0; g < kGrenadeRowCount; ++g)
						{
							profile.grenadeValues[g] = kGrenadeRows[g].defaultValue;
							SessionConfig::Set(ConfigKey(profile, kGrenadeRows[g].key, "value"), kGrenadeRows[g].defaultValue);
						}
						for (const GrenadePresetVal& v : preset.vals)
						{
							if (v.row < 0) break;
							profile.grenadeValues[v.row] = v.value;
							SessionConfig::Set(ConfigKey(profile, kGrenadeRows[v.row].key, "value"), v.value);
						}
						LOG_INFO("Weapons: applied grenade preset '%s' to '%s'", preset.label, profile.display.c_str());
					}
					if (imgui->IsItemHovered())
					{
						char tip[384];
						snprintf(tip, sizeof(tip), "%s%s%s",
							preset.tooltip,
							preset.credit ? "\n\n" : "",
							preset.credit ? preset.credit : "");
						imgui->SetTooltip(tip);
					}
				}

				// Saved presets sit directly under the built-ins and above every
				// control, same position in every group.
				imgui->Spacing();
				{
					constexpr int kGrenadePresetFieldCount = kGrenadeRowCount + 1; // + infiniteCharges
					BetterCheats::PresetStore::Field presetFields[kGrenadePresetFieldCount];
					// PresetStore group ids must never contain ':' themselves --
					// the store's own file format uses "[group:name]", splitting
					// on the FIRST ':' when it re-parses the file (see
					// preset_store.h). "Weapon:" + key put a second colon inside
					// the group id, which only broke once the file got re-read
					// from disk (a restart, or the loader's RELOAD button) --
					// same-session save/load/rename never re-parses, so it looked
					// fine until then. "Weapon." keeps every weapon/grenade type
					// in its own group without colliding with the format's own
					// delimiter.
					const std::string presetGroup = std::string("Weapon.") + profile.key;

					auto getLive = [&profile](BetterCheats::PresetStore::Field* out)
					{
						for (int g = 0; g < kGrenadeRowCount; ++g)
							out[g] = { kGrenadeRows[g].key, profile.grenadeValues[g] };
						out[kGrenadeRowCount] = { "infiniteCharges", profile.infiniteCharges ? 1.0f : 0.0f };
					};
					auto applyFields = [&profile](const BetterCheats::PresetStore::Field* f, int count)
					{
						for (int g = 0; g < kGrenadeRowCount && g < count; ++g)
						{
							profile.grenadeValues[g] = f[g].value;
							SessionConfig::Set(ConfigKey(profile, kGrenadeRows[g].key, "value"), f[g].value);
						}
						if (count > kGrenadeRowCount)
						{
							profile.infiniteCharges = f[kGrenadeRowCount].value != 0.0f;
							SessionConfig::Set(ConfigKey(profile, "infiniteCharges", "enabled"), profile.infiniteCharges);
						}
					};
					// Only the built-ins above should be guarded against; a saved
					// preset can't take one of their names.
					auto isBuiltin = [](const char* name)
					{
						for (int i = 0; i < kGrenadePresetCount; ++i)
							if (strcmp(kGrenadePresets[i].label, name) == 0)
								return true;
						return false;
					};
					// "<built-in> Custom" only when every row the preset touches
					// matches AND every other row is still at its own default --
					// same rule the weapon presets above use.
					auto computeSuggest = [&profile](char* out, int cap)
					{
						for (int i = 0; i < kGrenadePresetCount; ++i)
						{
							const GrenadePreset& preset = kGrenadePresets[i];

							bool touched[kGrenadeRowCount] = {};
							bool matches = true;
							for (const GrenadePresetVal& v : preset.vals)
							{
								if (v.row < 0) break;
								if (std::fabs(profile.grenadeValues[v.row] - v.value) > BetterCheats::kActiveEpsilon)
								{
									matches = false;
									break;
								}
								touched[v.row] = true;
							}
							if (matches)
							{
								for (int g = 0; g < kGrenadeRowCount && matches; ++g)
									if (!touched[g] && BetterCheats::DiffersFromDefault(profile.grenadeValues[g], kGrenadeRows[g].defaultValue))
										matches = false;
							}
							if (matches && profile.infiniteCharges)
								matches = false;   // presets never set this -- a match must leave it off too

							if (matches)
							{
								snprintf(out, cap, "%s Custom", preset.label);
								return;
							}
						}
						snprintf(out, cap, "Custom");
					};

					BetterCheats::UI::RenderSavedPresetsRow(imgui, "grenade_saved_presets", presetGroup.c_str(),
						presetFields, kGrenadePresetFieldCount,
						getLive, applyFields, isBuiltin, computeSuggest, profile.presetRow);
				}
				imgui->Spacing();

				if (imgui->Checkbox("Infinite charges", &profile.infiniteCharges))
					SessionConfig::Set(ConfigKey(profile, "infiniteCharges", "enabled"), profile.infiniteCharges);
				if (imgui->IsItemHovered())
					imgui->SetTooltip("Refills your charge bar to its max the moment it drops,\n"
					                  "using whatever the game (or your own Charge Capacity\n"
					                  "override below) currently honours as the max. Same top-up\n"
					                  "'Infinite magazine' uses for ammo, not a permanent override.");

				imgui->Spacing();

				if (imgui->BeginTable("##grenade_attr_table", 3, kWeaponsTableFlags))
				{
					const float grenadeLabelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, kGrenadeRowCount,
						[](int g) { return kGrenadeRows[g].label; });

					imgui->TableSetupColumn("Attribute", BetterCheats::UI::kColumnWidthFixed,
						BetterCheats::UI::GetReadoutColumnWidth(imgui, grenadeLabelReserve));
					imgui->TableSetupColumn("Value",     0, 0.54f);
					imgui->TableSetupColumn("",          0, 0.10f);

					for (int g = 0; g < kGrenadeRowCount; ++g)
					{
						const GrenadeRowDef& def   = kGrenadeRows[g];
						float&                value = profile.grenadeValues[g];
						const bool active = BetterCheats::DiffersFromDefault(value, def.defaultValue);

						imgui->PushIDInt(g);
						imgui->TableNextRow(0, 0.0f);

						char changeDesc[24];
						BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc), kGrenadeModes[g], value);

						BetterCheats::UI::RowSpec spec;
						spec.label         = def.label;
						spec.tooltip       = def.tooltip;
						spec.value         = &value;
						spec.minValue      = def.minValue;
						spec.maxValue      = def.maxValue;
						spec.step          = def.step;
						spec.format        = def.format;
						spec.resetValue    = def.defaultValue;
						spec.active        = active;
						spec.showLive      = true;
						spec.expected      = g_dbgGrenadeExpected[g].load();
						spec.game          = g_dbgGrenadeGame[g].load();
						spec.composing     = active;
						spec.hasBase       = false;   // no BaseValue split for these -- see cost/capacity comments in Tick()
						spec.unmodified    = g_dbgGrenadeUnmod[g].load();
						spec.changeDesc    = changeDesc;
						spec.labelReserve  = grenadeLabelReserve;

						if (BetterCheats::UI::BuildRow(imgui, spec).changed)
							SessionConfig::Set(ConfigKey(profile, def.key, "value"), value);

						imgui->PopID();
					}

					imgui->EndTable();
				}

				imgui->Spacing();
				if (imgui->SmallButton("Reset this weapon to stock"))
				{
					profile.infiniteCharges = false;
					SessionConfig::Set(ConfigKey(profile, "infiniteCharges", "enabled"), false);
					for (int g = 0; g < kGrenadeRowCount; ++g)
					{
						profile.grenadeValues[g] = kGrenadeRows[g].defaultValue;
						SessionConfig::Set(ConfigKey(profile, kGrenadeRows[g].key, "value"), kGrenadeRows[g].defaultValue);
					}
				}
				imgui->SameLine(0.0f, -1.0f);
				imgui->TextDisabled(profile.raw.c_str());

				// Fuse / Blast Radius / Throw Force: global, not per-weapon-type --
				// shown (and editable) identically on every grenade tab, so "Reset
				// this weapon to stock" above deliberately leaves these alone; each
				// row resets itself with its own reset button.
				imgui->Spacing();
				imgui->SeparatorText("Blast (applies to every grenade type)");

				if (imgui->BeginTable("##grenade_global_table", 3, kWeaponsTableFlags))
				{
					const float globalLabelReserve = BetterCheats::UI::PrescanLabelWidth(imgui, kGrenadeGlobalCount,
						[](int g) { return kGrenadeGlobals[g].label; });

					imgui->TableSetupColumn("Attribute", BetterCheats::UI::kColumnWidthFixed,
						BetterCheats::UI::GetReadoutColumnWidth(imgui, globalLabelReserve));
					imgui->TableSetupColumn("Value",     0, 0.54f);
					imgui->TableSetupColumn("",          0, 0.10f);

					for (int g = 0; g < kGrenadeGlobalCount; ++g)
					{
						const GrenadeGlobalDef& def = kGrenadeGlobals[g];
						const bool resolved = g_grenadeGlobalHandle[g].ok;

						float value = g_grenadeGlobalValue[g].load();
						const bool active = resolved && BetterCheats::DiffersFromDefault(value, def.defaultValue);

						imgui->PushIDInt(1000 + g);   // offset clear of the kGrenadeRowCount PushIDInt(g) block above
						imgui->TableNextRow(0, 0.0f);

						char changeDesc[24];
						BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc),
							BetterCheats::ComposedAttribute::Mode::Multiply, value);

						BetterCheats::UI::RowSpec spec;
						spec.label         = def.label;
						spec.tooltip       = resolved ? def.tooltip
							: "Not resolved yet -- the grenade projectile/throw ability class hasn't\n"
							  "loaded in this session. Usually resolves within a few seconds of\n"
							  "your first grenade throw; check ModLoader.log if it never does.";
						spec.value         = &value;
						spec.minValue      = def.minValue;
						spec.maxValue      = def.maxValue;
						spec.step          = def.step;
						spec.format        = def.format;
						spec.resetValue    = def.defaultValue;
						spec.active        = active;
						spec.disableSlider = !resolved;
						spec.showLive      = resolved;
						spec.expected      = g_dbgGrenadeGlobalExpected[g].load();
						spec.game          = g_dbgGrenadeGlobalGame[g].load();
						spec.composing     = active;
						spec.hasBase       = false;
						spec.unmodified    = active ? g_composedGrenadeGlobal[g].GetGame() : g_dbgGrenadeGlobalGame[g].load();
						spec.changeDesc    = changeDesc;
						spec.labelReserve  = globalLabelReserve;

						if (BetterCheats::UI::BuildRow(imgui, spec).changed)
						{
							g_grenadeGlobalValue[g].store(value);
							SessionConfig::Set(std::string("playerWeapons.") + def.key + ".value", value);
						}

						imgui->PopID();
					}

					imgui->EndTable();
				}
			}
			else
			{
			// This weapon type's own base stats -- from its CDO (resolved once,
			// see ResolveWeaponBaseFields), so they read correctly on every tab
			// whether or not this is the weapon currently in hand. Magazine is
			// the one exception: while EQUIPPED and our own override is active,
			// show the live composed value instead of the captured original, same
			// as the readout on the Magazine Size row below. "rounds/min" is the
			// real rate (60 / RoundsPerMinute.Value) -- that CDO field is
			// actually a per-shot interval in seconds despite its name (see
			// RealBaseForAttr), so showing it raw here used to mislabel a
			// number like 0.13 as "rounds/min".
			if (profile.baseResolved)
			{
				char dmg[32], rpm[32], mag[32], range[32];
				BetterCheats::UI::FormatLiveValue(dmg, sizeof(dmg), profile.baseDamage);
				const float realBaseRpm = (profile.baseRoundsPerMinute > 0.0f) ? 60.0f / profile.baseRoundsPerMinute : 0.0f;
				BetterCheats::UI::FormatLiveValue(rpm, sizeof(rpm), realBaseRpm);
				const bool magazineLive = isEquipped && IsActive(kAttrMagazine, profile.values[kAttrMagazine]);
				BetterCheats::UI::FormatLiveValue(mag, sizeof(mag),
					magazineLive ? g_dbgWeaponBaseMagazine.load() : profile.baseMagazine);
				BetterCheats::UI::FormatLiveValue(range, sizeof(range), profile.baseRange);
				char line[220];
				snprintf(line, sizeof(line),
					"  weapon base: dmg %s  rounds/min %s  mag %s  range %s",
					dmg, rpm, mag, range);
				imgui->TextDisabled(line);
			}
			else
			{
				imgui->TextDisabled("  weapon base: not resolved yet.");
			}
			imgui->Spacing();

			// A preset writes only into THIS weapon's profile, so every weapon carries its
			// own independently. `match` gates presets that only make sense on one weapon.
			const std::string loweredRaw = ToLower(profile.raw);
			imgui->TextDisabled("Presets:");
			bool anyPreset = false;
			for (int i = 0; i < kPresetCount; ++i)
			{
				const Preset& preset = kPresets[i];
				if (preset.match && *preset.match && loweredRaw.find(preset.match) == std::string::npos)
					continue;

				imgui->SameLine(0.0f, -1.0f);
				anyPreset = true;

				if (imgui->SmallButton(preset.label))
				{
					// A preset is a starting point, not a mode: clear the weapon first so
					// leftovers from a previous preset don't silently survive underneath.
					for (int a = 0; a < kAttrCount; ++a)
					{
						profile.values[a] = kAttrs[a].defaultValue;
						SessionConfig::Set(ConfigKey(profile, kAttrs[a].key, "value"), kAttrs[a].defaultValue);
					}
					for (const PresetVal& v : preset.vals)
					{
						if (v.attr < 0) break;
						profile.values[v.attr] = v.value;
						SessionConfig::Set(ConfigKey(profile, kAttrs[v.attr].key, "value"), v.value);
					}
					LOG_INFO("Weapons: applied preset '%s' to '%s'", preset.label, profile.display.c_str());
				}
				if (imgui->IsItemHovered())
				{
					char tip[384];
					snprintf(tip, sizeof(tip), "%s\n\nApplies to %s only.%s%s",
						preset.tooltip, profile.display.c_str(),
						preset.credit ? "\n\n" : "",
						preset.credit ? preset.credit : "");
					imgui->SetTooltip(tip);
				}
			}
			if (!anyPreset) { imgui->SameLine(0.0f, -1.0f); imgui->TextDisabled("(none for this weapon)"); }

			// Saved presets sit directly under the built-ins and above every
			// control, same position in every group.
			imgui->Spacing();
			{
				constexpr int kWeaponPresetFieldCount = kAttrCount + 2; // + oneHitKill + infiniteMagazine
				BetterCheats::PresetStore::Field presetFields[kWeaponPresetFieldCount];
				// See the grenade tab's identical comment above: group ids must
				// never contain ':' (the store's own "[group:name]" delimiter),
				// so this is "Weapon." not "Weapon:".
				const std::string presetGroup = std::string("Weapon.") + profile.key;

				auto getLive = [&profile](BetterCheats::PresetStore::Field* out)
				{
					for (int a = 0; a < kAttrCount; ++a)
						out[a] = { kAttrs[a].key, profile.values[a] };
					out[kAttrCount]     = { "oneHitKill",       profile.oneHitKill       ? 1.0f : 0.0f };
					out[kAttrCount + 1] = { "infiniteMagazine", profile.infiniteMagazine ? 1.0f : 0.0f };
				};
				auto applyFields = [&profile](const BetterCheats::PresetStore::Field* f, int count)
				{
					for (int a = 0; a < kAttrCount && a < count; ++a)
					{
						profile.values[a] = f[a].value;
						SessionConfig::Set(ConfigKey(profile, kAttrs[a].key, "value"), f[a].value);
					}
					if (count > kAttrCount)
					{
						profile.oneHitKill = f[kAttrCount].value != 0.0f;
						SessionConfig::Set(ConfigKey(profile, "oneHitKill", "enabled"), profile.oneHitKill);
					}
					if (count > kAttrCount + 1)
					{
						profile.infiniteMagazine = f[kAttrCount + 1].value != 0.0f;
						SessionConfig::Set(ConfigKey(profile, "infiniteMagazine", "enabled"), profile.infiniteMagazine);
					}
				};
				// Only the built-ins that actually apply to THIS weapon (same
				// `match` filtering the preset buttons above already use) --
				// a name free on other weapons but taken here should still
				// be refused, and vice versa.
				auto isBuiltin = [&loweredRaw](const char* name)
				{
					for (int i = 0; i < kPresetCount; ++i)
					{
						const Preset& preset = kPresets[i];
						if (preset.match && *preset.match && loweredRaw.find(preset.match) == std::string::npos)
							continue;
						if (strcmp(preset.label, name) == 0)
							return true;
					}
					return false;
				};
				// "<built-in> Custom" only when every applicable-preset field
				// matches AND every other row is still at its own default --
				// a hand-tweaked mix that happens to overlap one preset's
				// values isn't that preset.
				auto computeSuggest = [&profile, &loweredRaw](char* out, int cap)
				{
					for (int i = 0; i < kPresetCount; ++i)
					{
						const Preset& preset = kPresets[i];
						if (preset.match && *preset.match && loweredRaw.find(preset.match) == std::string::npos)
							continue;

						bool touched[kAttrCount] = {};
						bool matches = true;
						for (const PresetVal& v : preset.vals)
						{
							if (v.attr < 0) break;
							if (std::fabs(profile.values[v.attr] - v.value) > BetterCheats::kActiveEpsilon)
							{
								matches = false;
								break;
							}
							touched[v.attr] = true;
						}
						if (matches)
						{
							for (int a = 0; a < kAttrCount && matches; ++a)
								if (!touched[a] && BetterCheats::DiffersFromDefault(profile.values[a], kAttrs[a].defaultValue))
									matches = false;
						}
						if (matches && (profile.oneHitKill || profile.infiniteMagazine))
							matches = false;   // presets never set these -- a match must leave them off too

						if (matches)
						{
							snprintf(out, cap, "%s Custom", preset.label);
							return;
						}
					}
					snprintf(out, cap, "Custom");
				};

				BetterCheats::UI::RenderSavedPresetsRow(imgui, "weapon_saved_presets", presetGroup.c_str(),
					presetFields, kWeaponPresetFieldCount,
					getLive, applyFields, isBuiltin, computeSuggest, profile.presetRow);
			}
			imgui->Spacing();

			// ---- per-weapon toggles --------------------------------------------
			if (imgui->Checkbox("One Hit Kill", &profile.oneHitKill))
				SessionConfig::Set(ConfigKey(profile, "oneHitKill", "enabled"), profile.oneHitKill);
			if (imgui->IsItemHovered())
				imgui->SetTooltip("Overrides Damage with a flat 10000x while enabled. Turning it off\n"
				                  "hands Damage back to the game (attachment bonuses included).");

			imgui->SameLine(0.0f, -1.0f);
			if (imgui->Checkbox("Infinite magazine", &profile.infiniteMagazine))
				SessionConfig::Set(ConfigKey(profile, "infiniteMagazine", "enabled"), profile.infiniteMagazine);

			imgui->Spacing();

			// ---- attributes ----------------------------------------------------
			if (imgui->BeginTable("##weapon_attr_table", 3, kWeaponsTableFlags))
			{
				// Fixed, not stretched: must fit the label AND the live-values
				// readout next to it (see GetReadoutColumnWidth) or the readout
				// clips against the slider column. The other two stay stretched --
				// the slider is capped anyway (GetSliderWidthCap), so they have
				// slack to give up.
				imgui->TableSetupColumn("Attribute", BetterCheats::UI::kColumnWidthFixed,
					BetterCheats::UI::GetReadoutColumnWidth(imgui, labelReserve));
				imgui->TableSetupColumn("Value",     0, 0.54f);
				imgui->TableSetupColumn("",          0, 0.10f);

				for (int a = 0; a < kAttrCount; ++a)
				{
					const AttrDef& def   = kAttrs[a];
					float&         value = profile.values[a];

					const bool ownedByOneHitKill = (a == kAttrDamage && profile.oneHitKill);
					const bool rowActive         = IsActive(a, value) && !ownedByOneHitKill;

					imgui->PushIDInt(a);
					imgui->TableNextRow(0, 0.0f);

					char changeDesc[24];
					BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc),
						ownedByOneHitKill ? BetterCheats::ComposedAttribute::Mode::Absolute : kAttrModes[a],
						ownedByOneHitKill ? kOneHitKillDamage : value);

					BetterCheats::UI::RowSpec spec;
					spec.label         = def.label;
					spec.tooltip       = def.tooltip;
					spec.value         = &value;
					spec.minValue      = def.minValue;
					spec.maxValue      = def.maxValue;
					spec.step          = def.step;
					spec.format        = def.format;
					spec.resetValue    = def.defaultValue;
					spec.active        = rowActive;
					spec.disableSlider = ownedByOneHitKill;
					// Always on now -- live while this weapon is equipped, an estimate
					// otherwise (see the two branches below). No more "Show live
					// values" toggle to gate it.
					spec.showLive      = true;
					spec.estimate      = !isEquipped;
					spec.composing     = rowActive || ownedByOneHitKill;
					// Magazine composes onto the weapon data asset now, not a GAS
					// attribute -- no BaseValue split, so no tag; "Unmodified" is the
					// captured original (g_dbgWeaponBaseMagazine, same number the base-
					// stats block above shows).
					spec.hasBase       = (a != kAttrMagazine);

					if (a == kAttrMagazine)
					{
						// Already a real number (Add mode onto the weapon data asset's
						// own BaseMagazine field) -- no multiplier layer to convert.
						// Estimate = the CDO's own base plus our own offset; no live
						// mods signal exists for this field either way.
						const float baseMag = profile.baseMagazine;
						spec.expected   = isEquipped ? g_dbgAttrExpected[a].load() : (baseMag + (rowActive ? value : 0.0f));
						spec.game       = isEquipped ? g_dbgAttrGame[a].load()     : spec.expected;
						spec.unmodified = isEquipped ? g_dbgWeaponBaseMagazine.load() : baseMag;
						spec.base       = 0.0f;
						spec.hasOurs    = false;
					}
					else if (a == kAttrPierce)
					{
						// UCrWeaponAttributeSet::PossibleEnemiesHitPerTrace (what
						// g_dbgAttrBase/Buffed read) is a dead Au-layer GAS field --
						// BaseValue never gets written by the game at all (same
						// permanently-zero pattern as every other row's inherited
						// base, see the file comment above), so it never carried this
						// weapon's real pierce count. The true base lives on the
						// weapon TYPE's own CDO instead (profile.basePierce, see
						// ResolveWeaponBaseFields) -- used for BOTH base and
						// unmodified here, not just the tooltip's Base line: nothing
						// in this plugin has a verified live source for a REAL
						// attachment-driven pierce bonus, so comparing this base
						// against the still-dead GAS aggregate would tag every
						// weapon with a nonzero base as "+mods" for no real reason.
						// Already a real number (Absolute mode, a literal count).
						const float baseP = static_cast<float>(profile.basePierce);
						spec.base       = baseP;
						spec.unmodified = baseP;
						spec.expected   = isEquipped ? g_dbgAttrExpected[a].load() : (rowActive ? value : baseP);
						spec.game       = isEquipped ? g_dbgAttrGame[a].load()     : spec.expected;
						spec.hasOurs    = false;
					}
					else
					{
						// Every other row composes a *ModMultiplier GAS attribute
						// (Multiply mode), whose own BaseValue is a dead Au-layer
						// field always sitting at its neutral 1.0 (see the file's
						// Au-layer comment) -- true whether or not this weapon is
						// the one in hand. While unequipped, the estimate reuses
						// the exact "final = game + base*(k-1)" shape Tick()'s live
						// compose already uses, just fed by the mods this weapon
						// last showed (profile.cachedModsMult) instead of a fresh
						// GAS read -- 0 (no mods) the first time it's ever equipped.
						//
						// Damage/Fire Rate/Reload/Falloff/Spread/ADS have a real per-
						// stat base on the weapon's own CDO -- show the actual damage/
						// rate/etc. (real = base x effective multiplier) instead of the
						// bare multiplier the slider edits. Recoil/Sway have no single
						// real field to convert (kAttrHasRealBase false) and stay "x".
						BetterCheats::UI::MultiplyReadout readout;
						readout.live       = isEquipped;
						readout.base       = g_dbgAttrBase[a].load();
						readout.unmodified = g_dbgAttrBuffed[a].load();
						readout.game       = g_dbgAttrGame[a].load();
						readout.cachedMods = profile.cachedModsMult[a];
						readout.amount     = value;
						readout.active     = rowActive;
						readout.realBase   = kAttrHasRealBase[a] ? RealBaseForAttr(profile, a) : 0.0f;
						BetterCheats::UI::SetMultiplyReadout(spec, readout);

						// One Hit Kill forces Damage to a flat Absolute value that has
						// nothing to do with the multiplier above (Tick() applies it
						// the same way) -- set AFTER the real-unit scaling, not scaled
						// itself.
						if (ownedByOneHitKill)
						{
							spec.hasOurs  = false;
							spec.ours     = 0.0f;
							spec.expected = isEquipped ? g_dbgAttrExpected[a].load() : kOneHitKillDamage;
							spec.game     = isEquipped ? g_dbgAttrGame[a].load()     : kOneHitKillDamage;
						}
					}

					spec.changeDesc    = changeDesc;
					spec.tagWord       = "mods";
					spec.baseLabel     = "Base (no attachments)";
					spec.labelReserve  = labelReserve;

					if (BetterCheats::UI::BuildRow(imgui, spec).changed)
						SessionConfig::Set(ConfigKey(profile, def.key, "value"), value);

					imgui->PopID();
				}

				imgui->EndTable();
			}

			// Fixed, always-reserved spot below every row -- text only while
			// estimating, so switching tabs never shifts anything below this
			// point (a note that used to sit ABOVE the rows, conditionally,
			// was exactly what made tabs jump when switching between an
			// equipped and unequipped weapon).
			imgui->TextDisabled(isEquipped ? "" : "Estimated. Live values show once this weapon is equipped.");

			imgui->Spacing();
			if (imgui->SmallButton("Reset this weapon to stock"))
			{
				profile.oneHitKill       = false;
				profile.infiniteMagazine = false;
				SessionConfig::Set(ConfigKey(profile, "oneHitKill", "enabled"), false);
				SessionConfig::Set(ConfigKey(profile, "infiniteMagazine", "enabled"), false);
				for (int a = 0; a < kAttrCount; ++a)
				{
					profile.values[a] = kAttrs[a].defaultValue;
					SessionConfig::Set(ConfigKey(profile, kAttrs[a].key, "value"), kAttrs[a].defaultValue);
				}
			}
			imgui->SameLine(0.0f, -1.0f);
			imgui->TextDisabled(profile.raw.c_str());
			} // else (!profile.isGrenade)

			imgui->PopID();
			imgui->EndTabItem();
		}

		} // profilesLock

		imgui->EndTabBar();

		imgui->Spacing();
		imgui->Separator();
		imgui->TextDisabled("Presets modelled on the pak mods that Update 2 broke -");
		imgui->TextDisabled("thanks to axbhub (Better Weapons) and SwiftstepsKR (piercing weapons).");
	}
}
