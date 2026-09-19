#include "player_weapons.h"
#include "plugin_helpers.h"
#include "session_config.h"
#include "aob_resolver.h"
#include "ui_widgets.h"
#include "attribute_compose.h"

#include "Chimera_classes.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
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
		constexpr float kActiveEpsilon = 0.0001f;

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
			  1.0f, 0.10f,  25.0f, 0.05f, "%.2fx", "Multiplies outgoing weapon damage, attachment bonuses included." },
			{ "Fire Rate",          "fireRate", &SDK::UCrWeaponAttributeSet::FireRateModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Multiplies rounds per second, attachment bonuses included." },
			{ "Reload Speed",       "reload",   &SDK::UCrWeaponAttributeSet::ReloadSpeedModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Higher is faster. Multiplies on top of any attachment bonus." },
			{ "Magazine Size",      "magazine", &SDK::UCrWeaponAttributeSet::MaxMagAmmoModOffset,
			  0.0f, -100.0f, 999.0f, 1.0f, "%.0f", "Flat OFFSET added to this weapon's magazine, on top of any\nattachment's own offset. Negative shrinks it." },
			{ "Damage Falloff",     "falloff",  &SDK::UCrWeaponAttributeSet::DamageFallOffModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Effective range before damage drops off. Multiplies on top of\nany attachment bonus." },
			{ "Recoil",             "recoil",   &SDK::UCrWeaponAttributeSet::RecoilModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 removes recoil entirely. Multiplies on top of any attachment bonus." },
			{ "Spread",             "spread",   &SDK::UCrWeaponAttributeSet::SpreadModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 is perfect accuracy. Multiplies on top of any attachment bonus." },
			{ "Sway",               "sway",     &SDK::UCrWeaponAttributeSet::SwayModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 removes weapon sway. Multiplies on top of any attachment bonus." },
			{ "ADS Speed",          "ads",      &SDK::UCrWeaponAttributeSet::ADS_TransitionSpeedModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Aim-down-sights transition speed. Multiplies on top of any\nattachment bonus." },
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

		// "Show live values" toggle -- default on so a fresh install proves the
		// compose fix works without the owner having to find the setting.
		std::atomic<bool> g_showLiveValues{ true };

		// One-shot proof-of-life logging (gss.13): confirms in ModLoader.log that
		// both the Tick-side fill and the RenderImGui-side draw actually ran this
		// session, instead of guessing from a report of "nothing happens".
		std::atomic<bool> g_loggedTickFill{ false };
		std::atomic<bool> g_loggedRender{ false };

		// Au-layer resolved stats on the equipped weapon (UAuWeaponAttributeSet,
		// inherited by UCrWeaponAttributeSet), refreshed from Tick. -1 = not
		// available yet, matching g_dbgMag above.
		std::atomic<float> g_dbgWeaponDamage    { -1.0f };
		std::atomic<float> g_dbgWeaponFireRate  { -1.0f };
		std::atomic<float> g_dbgWeaponMaxMagazine{ -1.0f };   // NOT what Magazine Size writes -- see g_dbgMagOffset*
		std::atomic<float> g_dbgWeaponAccuracy  { -1.0f };
		std::atomic<float> g_dbgWeaponStability { -1.0f };
		std::atomic<float> g_dbgWeaponRange     { -1.0f };

		// MaxMagAmmoModOffset -- what the Magazine Size row actually writes.
		std::atomic<float> g_dbgMagOffsetCurrent{ 0.0f };
		std::atomic<float> g_dbgMagOffsetBase   { 0.0f };

		// Per-row "expected = game" / "expected != game" readout, indexed like
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

		// Forgets every composed slot the moment the attribute set instance changes
		// (respawn, world change) -- before anything below tries to Apply/Release
		// against what may already be gone.
		void ForgetComposedIfOwnerChanged(SDK::UCrWeaponAttributeSet* current)
		{
			if (current == g_composedOwner) return;
			for (int a = 0; a < kAttrCount; ++a)
				g_composed[a].Forget();
			g_composedOwner = current;
		}

		bool IsActive(int attr, float value)
		{
			return std::fabs(value - kAttrs[attr].defaultValue) > kActiveEpsilon;
		}

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
		}

		void LoadProfileFromConfig(Profile& p)
		{
			if (!SessionConfig::IsLoaded())
				return;
			for (int a = 0; a < kAttrCount; ++a)
				p.values[a] = SessionConfig::Get(ConfigKey(p, kAttrs[a].key, "value"), p.values[a]);
			p.oneHitKill       = SessionConfig::Get(ConfigKey(p, "oneHitKill", "enabled"), false);
			p.infiniteMagazine = SessionConfig::Get(ConfigKey(p, "infiniteMagazine", "enabled"), false);
		}

		// Caller must hold g_profilesMutex.
		void PersistKnownWeapons()
		{
			if (!SessionConfig::IsLoaded())
				return;
			nlohmann::json arr = nlohmann::json::array();
			for (const Profile& p : g_profiles)
				arr.push_back({ {"key", p.key}, {"display", p.display}, {"raw", p.raw} });
			SessionConfig::Set("playerWeapons.known", arr);
		}

		int FindOrCreateProfile(const std::string& rawName);

		// Make sure every known weapon has a tab, whether or not it has been equipped.
		// Caller must hold g_profilesMutex.
		void EnsureBuiltInProfiles()
		{
			static bool s_done = false;
			if (s_done) return;
			s_done = true;
			for (int i = 0; i < kBuiltInCount; ++i)
				FindOrCreateProfile(kBuiltInWeapons[i]);
		}

		// Caller must hold g_profilesMutex.
		int FindOrCreateProfile(const std::string& rawName)
		{
			const std::string key = Sanitize(rawName);
			for (size_t i = 0; i < g_profiles.size(); ++i)
				if (g_profiles[i].key == key)
					return static_cast<int>(i);

			Profile p;
			p.key     = key;
			p.raw     = rawName;
			p.display = Prettify(rawName);
			ResetProfileValues(p);
			g_profiles.push_back(p);
			LoadProfileFromConfig(g_profiles.back());
			PersistKnownWeapons();

			LOG_INFO("Weapons: discovered weapon '%s' -> tab '%s'", rawName.c_str(), p.display.c_str());
			return static_cast<int>(g_profiles.size()) - 1;
		}


		// See player_attributes.cpp's GetLocalCharacter -- the pawn isn't a Chimera
		// character until it's actually possessed, and UWorld::GetWorld() can throw.
		SDK::ACrCharacterPlayerBase* GetLocalCharacter()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc || !pc->Pawn) return nullptr;

			SDK::UClass* characterClass = SDK::ACrCharacterPlayerBase::StaticClass();
			if (!characterClass || !pc->Pawn->IsA(characterClass)) return nullptr;

			return static_cast<SDK::ACrCharacterPlayerBase*>(pc->Pawn);
		}
	}

	void Tick(float deltaSeconds)
	{
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
		}

		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		if (!character || !character->WeaponSystem)
			return;

		if (SDK::UCrWeaponItemDataBase* data = character->WeaponSystem->LastEquippedWeaponData)
		{
			const std::string raw = data->GetName();

			// Holding a tool leaves no active weapon profile, so nothing is applied.
			if (IsWeaponAsset(raw))
			{
				std::lock_guard<std::mutex> lock(g_profilesMutex);
				g_activeProfile.store(FindOrCreateProfile(raw));
			}
			else
			{
				g_activeProfile.store(-1);
			}
		}

		// Copy the active profile out under the lock instead of keeping a reference
		// into g_profiles across the SDK calls below -- a newly discovered weapon can
		// push_back and reallocate the vector from another Tick call. ROOT CAUSE of
		// the gss.12 "dead readout" bug: this used to `return` here whenever no
		// weapon profile was active (holding a tool, nothing equipped), which
		// skipped the live-values readout below entirely -- unlike Movement, whose
		// equivalent update has no such gate and always runs. Fall back to an
		// all-default profile instead: IsActive() reads false for every row, so
		// the compose loop below Releases everything (correct -- nothing should be
		// overridden with no weapon out) while still updating every debug atomic.
		Profile profile;
		bool haveActiveProfile = false;
		{
			std::lock_guard<std::mutex> lock(g_profilesMutex);
			const int active = g_activeProfile.load();
			if (active >= 0 && active < static_cast<int>(g_profiles.size()))
			{
				profile = g_profiles[active];
				haveActiveProfile = true;
			}
		}
		if (!haveActiveProfile)
			ResetProfileValues(profile);   // oneHitKill/infiniteMagazine already default false

		try
		{
			SDK::UCrWeaponAttributeSet* weapons = character->WeaponAttributes;
			ForgetComposedIfOwnerChanged(weapons);

			if (weapons)
			{
				for (int a = 0; a < kAttrCount; ++a)
				{
					SDK::FGameplayAttributeData& attr = weapons->*kAttrs[a].member;

					// Captured before we touch anything: "game" for the readout, and
					// (when this row was already active going into this tick) the
					// basis for "expected" -- what our last write should still read
					// back as if nothing has overridden it since.
					const float gameBefore    = attr.CurrentValue;
					const bool  wasActive     = g_composed[a].IsActive();
					const float previousWrite = g_composed[a].GetWritten();

					bool active = true;
					if (a == kAttrDamage && profile.oneHitKill)
					{
						// Absolute wins outright; Release() hands Damage back to the
						// game's own aggregate once this turns off.
						g_composed[a].Apply(weapons, attr, kOneHitKillDamage,
							BetterCheats::ComposedAttribute::Mode::Absolute,
							kOneHitKillDamage, kOneHitKillDamage);
					}
					else if (IsActive(a, profile.values[a]))
					{
						g_composed[a].Apply(weapons, attr, profile.values[a], kAttrModes[a],
							kAttrs[a].minValue, kAttrs[a].maxValue);
					}
					else
					{
						active = false;
						g_composed[a].Release(weapons, attr);
					}

					// On the very first activation frame there is no previous write to
					// compare against yet -- expected is trivially "game" until next tick.
					const float expected = active ? (wasActive ? previousWrite : gameBefore) : gameBefore;
					// "Buffed" (base + attachments, ours excluded): GetGame() reflects
					// what Apply() just composed from -- valid once active. Inactive rows
					// never had that captured, but CurrentValue is already untouched by
					// us, so it already IS base + attachments.
					const float buffed = active ? g_composed[a].GetGame() : gameBefore;
					g_dbgAttrGame[a].store(gameBefore);
					g_dbgAttrExpected[a].store(expected);
					g_dbgAttrBase[a].store(attr.BaseValue);
					g_dbgAttrBuffed[a].store(buffed);
				}

				if (!g_loggedTickFill.exchange(true))
					LOG_INFO("Weapons: live values first filled by Tick (weapon '%s').", profile.display.c_str());

				// Live-values readout: the Au-layer resolved stats (WeaponDamage etc.)
				// are inherited members on the same UCrWeaponAttributeSet, and the raw
				// MaxMagAmmoModOffset base/current -- see g_dbgWeaponMaxMagazine's
				// comment for why this differs from Magazine Size's effect on the clip.
				g_dbgWeaponDamage.store(weapons->WeaponDamage.CurrentValue);
				g_dbgWeaponFireRate.store(weapons->FireRate.CurrentValue);
				g_dbgWeaponMaxMagazine.store(weapons->MaxMagazine.CurrentValue);
				g_dbgWeaponAccuracy.store(weapons->Accuracy.CurrentValue);
				g_dbgWeaponStability.store(weapons->Stability.CurrentValue);
				g_dbgWeaponRange.store(weapons->Range.CurrentValue);
				g_dbgMagOffsetCurrent.store(weapons->MaxMagAmmoModOffset.CurrentValue);
				g_dbgMagOffsetBase.store(weapons->MaxMagAmmoModOffset.BaseValue);
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

	void Shutdown()
	{
		// Same character the composed state was captured against -- safe to hand
		// CurrentValue back. A different or null character means the attribute set
		// this state refers to is already gone; ForgetComposedIfOwnerChanged would
		// just no-op the writes below anyway, but skip the SDK call entirely.
		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		SDK::UCrWeaponAttributeSet* weapons = character ? character->WeaponAttributes : nullptr;

		try
		{
			if (weapons && weapons == g_composedOwner)
			{
				for (int a = 0; a < kAttrCount; ++a)
					g_composed[a].Release(weapons, weapons->*kAttrs[a].member);
			}
			else
			{
				for (int a = 0; a < kAttrCount; ++a)
					g_composed[a].Forget();
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
					p.key     = e.value("key", std::string());
					p.display = e.value("display", std::string());
					p.raw     = e.value("raw", std::string());
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
				FindOrCreateProfile(kBuiltInWeapons[i]);

			profileCount = g_profiles.size();
		}

		g_allInfiniteMagazine.store(SessionConfig::Get("playerWeapons.allInfiniteMagazine", false));
		g_autoRestock.store(SessionConfig::Get("playerWeapons.autoRestock", false));
		g_restockHidden.store(SessionConfig::Get("playerWeapons.restockHidden", true));
		g_restockMags.store(SessionConfig::Get("playerWeapons.restockMags", 3.0f));
		g_showLiveValues.store(SessionConfig::Get("playerWeapons.showLiveValues", true));

		g_configApplied = true;

		LOG_INFO("Weapons: applied saved config for session '%s' (%zu known weapons).",
			SessionConfig::GetSessionName().c_str(), profileCount);
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		if (!g_loggedRender.exchange(true))
			LOG_INFO("Weapons: live values readout rendering (%d rows).", kAttrCount);

		// First control in the tab, unconditional -- gss.12 buried this below a
		// disclaimer line where it was easy to miss entirely.
		bool showLiveValues = g_showLiveValues.load();
		if (imgui->Checkbox("Show live values", &showLiveValues))
		{
			g_showLiveValues.store(showLiveValues);
			SessionConfig::Set("playerWeapons.showLiveValues", showLiveValues);
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Shows the game's own numbers next to each slider, so a change is\n"
			                  "obvious instead of a guess.");

		imgui->TextDisabled("Multipliers apply on top of the weapon's base stats and any attachments.");

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

		// Live values: the three numbers that settle the Magazine Size question --
		// does GetEquippedWeaponMaxMagazineAmmo() (the clip size above) track the
		// offset we write, or the untouched Au-layer MaxMagazine? Whichever one
		// moves with the slider is the one that's actually live.
		if (g_showLiveValues.load() && dbgMagMax >= 0.0f)
		{
			char dmg[32], rate[32], mag[32], acc[32], stab[32], range[32];
			BetterCheats::UI::FormatLiveValue(dmg,   sizeof(dmg),   g_dbgWeaponDamage.load());
			BetterCheats::UI::FormatLiveValue(rate,  sizeof(rate),  g_dbgWeaponFireRate.load());
			BetterCheats::UI::FormatLiveValue(mag,   sizeof(mag),   g_dbgWeaponMaxMagazine.load());
			BetterCheats::UI::FormatLiveValue(acc,   sizeof(acc),   g_dbgWeaponAccuracy.load());
			BetterCheats::UI::FormatLiveValue(stab,  sizeof(stab),  g_dbgWeaponStability.load());
			BetterCheats::UI::FormatLiveValue(range, sizeof(range), g_dbgWeaponRange.load());
			char line1[220];
			snprintf(line1, sizeof(line1),
				"  live (Au-layer): dmg %s  rate %s  mag %s  acc %s  stab %s  range %s",
				dmg, rate, mag, acc, stab, range);
			imgui->TextDisabled(line1);

			char offCur[32], offBase[32], getter[32];
			BetterCheats::UI::FormatLiveValue(offCur,  sizeof(offCur),  g_dbgMagOffsetCurrent.load());
			BetterCheats::UI::FormatLiveValue(offBase, sizeof(offBase), g_dbgMagOffsetBase.load());
			BetterCheats::UI::FormatLiveValue(getter,  sizeof(getter),  dbgMagMax);
			char line2[220];
			snprintf(line2, sizeof(line2),
				"  magazine offset: current %s / base %s    clip getter returned %s",
				offCur, offBase, getter);
			imgui->TextDisabled(line2);
		}

		imgui->Spacing();
		imgui->SeparatorText("Per-Weapon");
		imgui->TextDisabled("A value differing from the default is applied. Reset a row to turn it off.");
		imgui->Spacing();

		// Widest attribute label decides where the live-values readout starts, so
		// every row's readout lines up regardless of that row's own label length.
		float labelReserve = 0.0f;
		for (int a = 0; a < kAttrCount; ++a)
		{
			float w = 0.0f, h = 0.0f;
			imgui->CalcTextSize(kAttrs[a].label, &w, &h, false, -1.0f);
			if (w > labelReserve) labelReserve = w;
		}
		labelReserve += imgui->GetFrameHeight();

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
				imgui->TableSetupColumn("Attribute", 0, 0.36f);
				imgui->TableSetupColumn("Value",     0, 0.54f);
				imgui->TableSetupColumn("",          0, 0.10f);

				for (int a = 0; a < kAttrCount; ++a)
				{
					const AttrDef& def   = kAttrs[a];
					float&         value = profile.values[a];

					const bool ownedByOneHitKill = (a == kAttrDamage && profile.oneHitKill);
					const bool active            = IsActive(a, value) && !ownedByOneHitKill;

					imgui->PushIDInt(a);
					imgui->TableNextRow(0, 0.0f);

					imgui->TableSetColumnIndex(0);
					if (active) imgui->Text(def.label);
					else        imgui->TextDisabled(def.label);
					if (def.tooltip && imgui->IsItemHovered())
						imgui->SetTooltip(def.tooltip);

					if (g_showLiveValues.load())
					{
						const float expected = g_dbgAttrExpected[a].load();
						const float game     = g_dbgAttrGame[a].load();
						imgui->SameLine(labelReserve, 0.0f);
						BetterCheats::UI::RenderLiveValue(imgui, expected, game);

						char changeDesc[24];
						BetterCheats::UI::FormatChangeDesc(changeDesc, sizeof(changeDesc),
							ownedByOneHitKill ? BetterCheats::ComposedAttribute::Mode::Absolute : kAttrModes[a],
							ownedByOneHitKill ? kOneHitKillDamage : value);
						imgui->SameLine(0.0f, 8.0f);
						BetterCheats::UI::RenderBuffTag(imgui, "mods", "With attachments",
							g_dbgAttrBase[a].load(), g_dbgAttrBuffed[a].load(), changeDesc, expected, game);
					}

					// Slider for feel, typed box on the right for precision. Zero spacing
					// between them so they read as one joined control.
					imgui->TableSetColumnIndex(1);
					imgui->BeginDisabled(ownedByOneHitKill);

					float availX = 0.0f, availY = 0.0f;
					imgui->GetContentRegionAvail(&availX, &availY);

					// InputFloat draws [text][-][+]. Measure the widest value this row can
					// actually show rather than guessing a pixel count -- the loader's
					// FontScale is user-configurable (this user runs 1.50), so anything
					// hardcoded clips at some scale.
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
						SessionConfig::Set(ConfigKey(profile, def.key, "value"), value);
					}
					imgui->EndDisabled();

					imgui->TableSetColumnIndex(2);
					if (BetterCheats::UI::ResetButton(imgui, "##reset"))
					{
						value = def.defaultValue;
						SessionConfig::Set(ConfigKey(profile, def.key, "value"), value);
					}
					if (imgui->IsItemHovered())
						imgui->SetTooltip("Reset to the game default (turns this row off).");

					imgui->PopID();
				}

				imgui->EndTable();
			}

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
