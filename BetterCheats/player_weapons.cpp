#include "player_weapons.h"
#include "plugin_helpers.h"
#include "session_config.h"
#include "aob_resolver.h"
#include "ui_widgets.h"

#include "Chimera_classes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace BetterCheats::Panels::Weapons
{
	namespace
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kWeaponsTableFlags = (1 << 6) | (1 << 9) | (3 << 13);

		// ImGuiCol_ index (same convention as player_attributes.cpp's Col_* constants)
		constexpr int Col_Text = 0;

		// Damage multiplier used by "One Hit Kill". Mirrors the flat 10000 the mining
		// laser hook uses in player_tools.cpp.
		constexpr float kOneHitKillDamage = 10000.0f;

		// A value is applied only when it differs from the game default, so there is no
		// separate enable toggle to keep in sync -- resetting a row IS disabling it.
		constexpr float kActiveEpsilon = 0.0001f;

		// ---------------------------------------------------------------------
		// Attribute table -- FGameplayAttributeData on UCrWeaponAttributeSet, reached
		// via ACrCharacterPlayerBase::WeaponAttributes. The set belongs to the CHARACTER
		// and applies to whatever is equipped, so per-weapon profiles work by writing
		// the equipped weapon's profile every tick.
		// ---------------------------------------------------------------------
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
			  1.0f, 0.10f,  25.0f, 0.05f, "%.2fx", "Multiplies outgoing weapon damage." },
			{ "Fire Rate",          "fireRate", &SDK::UCrWeaponAttributeSet::FireRateModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Multiplies rounds per second." },
			{ "Reload Speed",       "reload",   &SDK::UCrWeaponAttributeSet::ReloadSpeedModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Higher is faster." },
			{ "Magazine Size",      "magazine", &SDK::UCrWeaponAttributeSet::MaxMagAmmoModOffset,
			  0.0f, -100.0f, 999.0f, 1.0f, "%.0f", "Flat OFFSET added to this weapon's magazine. Negative shrinks it." },
			{ "Damage Falloff",     "falloff",  &SDK::UCrWeaponAttributeSet::DamageFallOffModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Effective range before damage drops off." },
			{ "Recoil",             "recoil",   &SDK::UCrWeaponAttributeSet::RecoilModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 removes recoil entirely." },
			{ "Spread",             "spread",   &SDK::UCrWeaponAttributeSet::SpreadModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 is perfect accuracy." },
			{ "Sway",               "sway",     &SDK::UCrWeaponAttributeSet::SwayModMultiplier,
			  1.0f, 0.00f,   3.0f, 0.05f, "%.2fx", "0 removes weapon sway." },
			{ "ADS Speed",          "ads",      &SDK::UCrWeaponAttributeSet::ADS_TransitionSpeedModMultiplier,
			  1.0f, 0.10f,   5.0f, 0.05f, "%.2fx", "Aim-down-sights transition speed." },
			{ "Enemies Hit / Shot", "pierce",   &SDK::UCrWeaponAttributeSet::PossibleEnemiesHitPerTrace,
			  1.0f, 1.00f,  10.0f, 1.0f,  "%.0f",  "Projectile piercing -- how many enemies one shot passes through." },
		};

		// ---------------------------------------------------------------------
		// Presets. `match` is a lowercase substring tested against the weapon's asset
		// name; "" offers the preset on every weapon. Entries end at attr < 0.
		// ---------------------------------------------------------------------
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

		// ---------------------------------------------------------------------
		// Profiles are DISCOVERED, not hardcoded. Weapon data assets live in the paks
		// (no I_*DataItem_C classes exist in the SDK dump), so the only truthful source
		// of the roster is what the player actually equips.
		// ---------------------------------------------------------------------
		struct Profile
		{
			std::string key;        // sanitized, used as the config path segment
			std::string display;    // prettified for the tab label
			std::string raw;        // exact asset name, shown for diagnosis
			float       values[kAttrCount];
			bool        oneHitKill       = false;
			bool        infiniteMagazine = false;
		};

		std::vector<Profile> g_profiles;
		bool        g_allInfiniteMagazine = false;

		// Live readout, refreshed from Tick so the panel can show what the game actually
		// reports rather than what we hoped it would.
		float       g_dbgMag           = -1.0f;
		float       g_dbgMagMax        = -1.0f;
		float       g_dbgReserve       = -1.0f;
		float       g_dbgReserveMax    = -1.0f;
		float       g_dbgAttrAmmoBase  = -1.0f;
		float       g_dbgAttrAmmoCur   = -1.0f;

		float       g_dbgTakeAmmo      = -1.0f;
		float       g_dbgTakeMagAmmo   = -1.0f;
		float       g_dbgReloadAmmo    = -1.0f;

		// ---------------------------------------------------------------------
		// Auto-restock.
		//
		// The reserve pool IS inventory items -- proven in game: with an empty
		// inventory the weapon would not fire or reload no matter which gameplay
		// attribute was written, because the reload gate checks real items before any
		// cost is applied. So the only way to have reloading work forever is to keep
		// the weapon's own ammo item topped up.
		//
		// Which item that is comes from the weapon itself: UAuWeaponItemDataBase::
		// RequiredItem, so this follows whatever you are holding without a hardcoded
		// table of ammo types.
		//
		// EVENT-DRIVEN, NOT TIMED. The count is read every frame, but a write only ever
		// happens on the EDGE where the count has actually gone DOWN, and it adds back
		// exactly the amount that went missing. Nothing is topped up "towards a target"
		// on a clock.
		//
		// That distinction is what makes a repeat of the flood structurally impossible
		// rather than merely unlikely. The original filled ~25 slots with 999 rounds
		// because it added every tick and judged success with a getter that did not
		// reflect items added the same frame -- so it never saw itself succeed and kept
		// going. Under the delta rule a stale read can only make us MISS a top-up (count
		// looks unchanged, so nothing is added); it can never make us repeat one, because
		// a repeat needs the count to fall twice.
		//
		// Backstops kept for the cases the delta rule cannot see: a convergence guard
		// that switches the feature off if adds stop landing, and a session cap.
		// ---------------------------------------------------------------------
		bool  g_autoRestock      = false;
		bool  g_restockHidden    = true;    // prefer the hidden inventory: no visible slot
		float g_restockMags      = 3.0f;    // size of the one-off seed, in magazines
		int   g_restockFailures  = 0;       // consecutive adds that did not raise the count
		int   g_restockTotal     = 0;       // rounds replaced this session, for the readout

		// Edge-detection state. Reset whenever the ammo type changes, because a count of
		// pistol rounds says nothing about rifle rounds.
		SDK::UAuItemDataBase* g_restockItem  = nullptr;
		int                   g_restockLast  = -1;
		bool                  g_restockSeeded = false;

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
			if (g_restockFailures >= kRestockMaxFailures) return;
			if (g_restockTotal    >= kRestockSessionCap)  return;

			SDK::UAuItemDataBase* ammo = ResolveAmmoItem(ws->LastEquippedWeaponData);
			if (!ammo) return;   // tools and anything with no declared ammo type

			AddNewItemFn addNewItem = ResolveAddNewItem();
			if (!addNewItem) return;

			// The hidden inventory is tried first: if reloads can draw from it, the
			// reserve is real but costs no visible slot.
			SDK::UCrInventoryComponent* target = g_restockHidden
				? character->HiddenInventoryComponent
				: character->InventoryComponent;
			if (!target) target = character->InventoryComponent;
			if (!target) return;

			const int have = character->GetItemCount(ammo);

			// Switching weapons switches ammo type. Re-baseline rather than comparing
			// rifle rounds against a pistol count.
			if (ammo != g_restockItem)
			{
				g_restockItem   = ammo;
				g_restockLast   = have;
				g_restockSeeded = false;
			}

			int amount = 0;

			if (!g_restockSeeded)
			{
				// One-off seed, so switching this on with empty pockets gives you
				// something to reload from. After this, only consumption drives it.
				const int want = static_cast<int>(maxMag * g_restockMags + 0.5f);
				amount = (have < want) ? (want - have) : 0;
				g_restockSeeded = true;
			}
			else if (have < g_restockLast)
			{
				// THE EVENT: ammo actually left the inventory. Replace exactly that
				// much -- never a target, never a guess.
				amount = g_restockLast - have;
			}

			if (amount <= 0)
			{
				g_restockLast = have;   // count rose (picked up, crafted) -- just follow it
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
				if (++g_restockFailures >= kRestockMaxFailures)
				{
					g_autoRestock = false;
					SessionConfig::Set("playerWeapons.autoRestock", false);
					LOG_WARN("Weapons: auto-restock disabled itself -- %d adds of '%s' returned "
						"nothing (target %s inventory).",
						kRestockMaxFailures, ammo->GetName().c_str(),
						g_restockHidden ? "hidden" : "visible");
				}
				g_restockLast = have;
				return;
			}

			g_restockFailures = 0;
			g_restockTotal   += gained;

			// Re-read rather than assuming have+gained. If the count lags a frame the
			// worst case is a missed top-up next frame, not a repeated one.
			g_restockLast = character->GetItemCount(ammo);
		}
		int         g_activeProfile       = -1;   // index into g_profiles, -1 = nothing equipped
		int         g_viewedProfile       = -1;
		bool        g_configApplied       = false;   // set once ApplySavedConfig has run
		std::string g_detectedWeapon      = "(none)";

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

		// ---------------------------------------------------------------------
		// Infinite ammo -- why it is done by pinning the magazine.
		//
		// Three mechanisms exist. Only one of them holds:
		//
		//   1. Pin the magazine to its maximum (SetEquippedWeaponCurrentAmmo). WORKS.
		//      The magazine never empties, so the weapon never reloads, so the reserve
		//      pool is never drawn from -- you can carry no ammo at all. This is what
		//      the panel ships.
		//
		//   2. Pin the reserve pool (UAuWeaponAttributeSet::Ammo, reachable because
		//      UCrWeaponAttributeSet derives from it). DOES NOT WORK. That attribute is
		//      flagged Net/RepNotify -- server-authoritative -- so a client-side write
		//      is overwritten on the next replication tick. Worth knowing: every Cr
		//      multiplier this panel writes successfully carries NO Net flag, and that
		//      difference is the whole reason those stick and this one does not. The
		//      float return type of GetEquippedWeaponAmmoInInventory is a red herring;
		//      it reads a replicated attribute, not an inventory item.
		//
		//   3. Zero the per-shot cost (UAuWeaponItemDataBase::AmmoCost). Works, but it
		//      writes to a CDO shared by every instance of that weapon, and from the
		//      player's side behaves identically to (1). Not worth the shared-state risk.
		// ---------------------------------------------------------------------

		// The game's weapon roster, so every tab is present from the start and a weapon
		// can be tuned without first going and equipping it. These are the asset names
		// the game actually reports (confirmed from UCrWeaponComponent at runtime);
		// discovery still runs, so anything not listed here still gets its own tab.
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
		void EnsureBuiltInProfiles()
		{
			static bool s_done = false;
			if (s_done) return;
			s_done = true;
			for (int i = 0; i < kBuiltInCount; ++i)
				FindOrCreateProfile(kBuiltInWeapons[i]);
		}

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

		void WriteAttribute(SDK::FGameplayAttributeData& attribute, float value)
		{
			attribute.BaseValue    = value;
			attribute.CurrentValue = value;
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

		EnsureBuiltInProfiles();

		SDK::ACrCharacterPlayerBase* character = GetLocalCharacter();
		if (!character || !character->WeaponSystem)
			return;

		if (SDK::UCrWeaponItemDataBase* data = character->WeaponSystem->LastEquippedWeaponData)
		{
			const std::string raw = data->GetName();
			if (raw != g_detectedWeapon)
				g_detectedWeapon = raw;

			// Holding a tool leaves no active weapon profile, so nothing is applied.
			g_activeProfile = IsWeaponAsset(raw) ? FindOrCreateProfile(raw) : -1;
		}

		if (g_activeProfile < 0 || g_activeProfile >= static_cast<int>(g_profiles.size()))
			return;

		const Profile& profile = g_profiles[g_activeProfile];

		try
		{
			if (SDK::UCrWeaponAttributeSet* weapons = character->WeaponAttributes)
			{
				for (int a = 0; a < kAttrCount; ++a)
				{
					if (a == kAttrDamage && profile.oneHitKill) continue;
					if (!IsActive(a, profile.values[a]))         continue;
					WriteAttribute(weapons->*kAttrs[a].member, profile.values[a]);
				}

				if (profile.oneHitKill)
					WriteAttribute(weapons->*kAttrs[kAttrDamage].member, kOneHitKillDamage);

				g_dbgAttrAmmoBase = weapons->Ammo.BaseValue;
				g_dbgAttrAmmoCur  = weapons->Ammo.CurrentValue;
				g_dbgTakeAmmo     = weapons->TakeAmmo.CurrentValue;
				g_dbgTakeMagAmmo  = weapons->TakeMagazineAmmo.CurrentValue;
				g_dbgReloadAmmo   = weapons->ReloadAmmo.CurrentValue;
			}

			// Infinite ammo.
			//
			// Holding the magazine at full is the ONLY mechanism here that survives.
			// Because the magazine never empties, the weapon never reloads, and because
			// it never reloads the reserve pool is never drawn from -- so you need carry
			// no ammo at all. That is the whole feature.
			//
			// Two other routes were tried and are deliberately not used:
			//   * UAuWeaponAttributeSet::Ammo (the reserve pool) is flagged Net/RepNotify
			//     -- server-authoritative. A client write is stomped on the next
			//     replication tick. The Cr multipliers this panel writes are NOT
			//     replicated, which is exactly why those stick and that one does not.
			//   * Zeroing UAuWeaponItemDataBase::AmmoCost works, but it writes to a CDO
			//     shared by every instance of the weapon and ends up behaving the same as
			//     this from the player's side. Not worth the shared-state risk.
			SDK::UCrWeaponComponent* ws = character->WeaponSystem;

			const float maxMag = ws->GetEquippedWeaponMaxMagazineAmmo();
			g_dbgMag        = ws->GetEquippedWeaponCurrentAmmo();
			g_dbgMagMax     = maxMag;
			g_dbgReserve    = ws->GetEquippedWeaponAmmoInInventory();
			g_dbgReserveMax = ws->GetEquippedWeaponMaxAmmo();

			if (g_allInfiniteMagazine || profile.infiniteMagazine)
			{
				if (maxMag > 0.0f && g_dbgMag < maxMag)
					ws->SetEquippedWeaponCurrentAmmo(maxMag);
			}

			// Auto-restock. Called every frame, but it only WRITES on the edge where
			// the ammo count has actually dropped -- see the note above.
			if (g_autoRestock && maxMag > 0.0f)
				RestockEquippedAmmo(character, ws, maxMag);

		}
		catch (...) {}
	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		// Restore previously discovered weapons so their tabs exist before the player
		// re-equips them.
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

		g_allInfiniteMagazine = SessionConfig::Get("playerWeapons.allInfiniteMagazine", false);
		g_autoRestock         = SessionConfig::Get("playerWeapons.autoRestock", false);
		g_restockHidden       = SessionConfig::Get("playerWeapons.restockHidden", true);
		g_restockMags         = SessionConfig::Get("playerWeapons.restockMags", 3.0f);

		g_configApplied = true;

		// ApplySavedConfig() rebuilds the list from config, which may predate the
		// built-in roster -- re-seed so every weapon still has a tab.
		for (int i = 0; i < kBuiltInCount; ++i)
			FindOrCreateProfile(kBuiltInWeapons[i]);

		LOG_INFO("Weapons: applied saved config for session '%s' (%zu known weapons).",
			SessionConfig::GetSessionName().c_str(), g_profiles.size());
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		EnsureBuiltInProfiles();

		imgui->SeparatorText("Ammo");

		if (imgui->Checkbox("Infinite clip - ALL weapons", &g_allInfiniteMagazine))
			SessionConfig::Set("playerWeapons.allInfiniteMagazine", g_allInfiniteMagazine);
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Holds the clip at full. It never empties, so it never reloads -- and\n"
			                  "because it never reloads, your reserve is never drawn from either.\n\n"
			                  "Per-weapon equivalents are on each weapon's tab below.");

		if (imgui->Checkbox("Keep ammo stocked", &g_autoRestock))
		{
			SessionConfig::Set("playerWeapons.autoRestock", g_autoRestock);
			g_restockFailures = 0;
			g_restockItem     = nullptr;   // re-baseline and re-seed on next tick
			g_restockLast     = -1;
			g_restockSeeded   = false;
		}
		if (imgui->IsItemHovered())
			imgui->SetTooltip("Tops the equipped weapon's own ammo back up so you never run dry.\n"
			                  "The clip drains and you reload normally -- there is just always\n"
			                  "something to reload from.\n\n"
			                  "It follows whatever you are holding, using the ammo type the weapon\n"
			                  "itself declares, and only ever adds the shortfall.");

		if (g_autoRestock)
		{
			imgui->Indent(imgui->GetFrameHeight());

			if (imgui->Checkbox("Keep it out of my inventory", &g_restockHidden))
				SessionConfig::Set("playerWeapons.restockHidden", g_restockHidden);
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
			if (imgui->InputFloat("Clips to keep in reserve", &g_restockMags, 1.0f, 5.0f, "%.0f"))
			{
				if (g_restockMags < 1.0f)   g_restockMags = 1.0f;
				if (g_restockMags > 100.0f) g_restockMags = 100.0f;
				SessionConfig::Set("playerWeapons.restockMags", g_restockMags);
			}

			imgui->Unindent(imgui->GetFrameHeight());
		}

		if (g_dbgMagMax >= 0.0f)
		{
			char line[220];
			snprintf(line, sizeof(line), "  clip %.0f / %.0f      reserve %.0f / %.0f%s",
				g_dbgMag, g_dbgMagMax, g_dbgReserve, g_dbgReserveMax,
				g_restockTotal > 0 ? "      [restocking]" : "");
			imgui->TextDisabled(line);
		}

		imgui->Spacing();
		imgui->SeparatorText("Per-Weapon");
		imgui->TextDisabled("A value differing from the default is applied. Reset a row to turn it off.");
		imgui->Spacing();

		if (g_profiles.empty())
		{
			imgui->TextDisabled("No weapons seen yet - equip one and its tab will appear here.");
			return;
		}

		if (!imgui->BeginTabBar("##weapon_tabs", 0))
			return;

		static int s_lastFocused = -1;
		const bool focusChanged = (g_activeProfile != s_lastFocused);
		s_lastFocused = g_activeProfile;

		// Tabs are discovered in equip order, which is arbitrary. Present them in the
		// order the game itself progresses through them, with tools trailing.
		static const char* kTabOrder[] = { "pistol", "rifle", "shotgun", "machine" };
		constexpr int kTabOrderCount = static_cast<int>(sizeof(kTabOrder) / sizeof(kTabOrder[0]));

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
			const bool isEquipped = (p == g_activeProfile);

			// "###key" keeps the ImGui ID stable while the visible label changes.
			char label[96];
			snprintf(label, sizeof(label), "%s%s###%s",
				profile.display.c_str(), isEquipped ? " *" : "", profile.key.c_str());

			const int tabFlags = (focusChanged && isEquipped) ? (1 << 1) : 0; // SetSelected
			if (!imgui->BeginTabItem(label, nullptr, tabFlags))
				continue;

			g_viewedProfile = p;
			imgui->PushIDInt(p);


			// ---- presets -------------------------------------------------------
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
				imgui->SetTooltip("Overrides Damage with a flat 10000x while enabled.");

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

		imgui->EndTabBar();

		imgui->Spacing();
		imgui->Separator();
		imgui->TextDisabled("Presets modelled on the pak mods that Update 2 broke -");
		imgui->TextDisabled("thanks to axbhub (Better Weapons) and SwiftstepsKR (piercing weapons).");
	}
}
