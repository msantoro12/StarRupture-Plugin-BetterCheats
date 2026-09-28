#pragma once

#include "plugin_interface.h"
#include "Chimera_classes.hpp"
#include "AuItems_classes.hpp"
#include "AssetRegistry_classes.hpp"

// Shared item-type asset-registry plumbing. Both the Item Spawner
// (player_items.cpp) and the Inventory panel's stack-size controls
// (player_inventory.cpp) need "every UAuItemBlueprint asset type,
// resolved down to its UAuItemDataBase CDO" -- the resolution path (a
// ProcessEvent call against IAssetRegistry, then a force-load + generated-
// class chase mirroring the game's own UCrUW_CheatItemsTab::
// DebugGatherAllItems) is fragile and version-specific enough that it should
// exist exactly once rather than as two independent copies that could drift.
//
// Game-thread only, same constraint as every other UObject/ProcessEvent call
// in this codebase -- callers are responsible for getting here via
// PostToGameThread (see player_items.cpp/player_inventory.cpp for the
// request/adopt pattern each panel wraps this in).
namespace BetterCheats::ItemRegistry
{
	// /Script/AuItems.AuItemBlueprint, the class path both panels query.
	SDK::FTopLevelAssetPath ItemBlueprintClassPath();

	// UAssetRegistryHelpers::GetAssetRegistry via ProcessEvent. Null if the
	// interface or its UFUNCTION couldn't be resolved.
	SDK::IAssetRegistry* GetAssetRegistry();

	// IAssetRegistry::GetAssetsByClass via ProcessEvent (bSearchSubClasses
	// always true). Returns false on failure to resolve or invoke the
	// UFUNCTION; outAssets is left untouched in that case.
	bool GetAssetsByClass(SDK::IAssetRegistry* registry, const SDK::FTopLevelAssetPath& classPath,
		SDK::TArray<SDK::FAssetData>& outAssets);

	// Force-loads the Blueprint's package and resolves it down to its
	// generated class's CDO -- package -> generated "<AssetName>_C" class ->
	// ClassDefaultObject. Returns null (without throwing) for anything that
	// doesn't resolve; pass verbose=true to log why at each step. No class
	// check here -- ResolveItemFromBlueprintAsset (below) adds the
	// UAuItemDataBase check; a caller after a different Blueprint-actor base
	// (e.g. the gatherable-actor scan in player_inventory.cpp, keyed on
	// ACrGatherableBaseActor instead of AuItemBlueprint) does its own IsA
	// check on the result. Also the right call to re-resolve a CDO fresh
	// right before a write: the one cached at scan time can be garbage-
	// collected if its Blueprint class/package gets unloaded in the meantime.
	SDK::UObject* ResolveBlueprintCDO(const SDK::FAssetData& assetData, bool verbose);

	// ResolveBlueprintCDO() plus the UAuItemDataBase check both existing
	// callers (Item Spawner, Inventory's stack-size scan) actually want.
	SDK::UAuItemDataBase* ResolveItemFromBlueprintAsset(const SDK::FAssetData& assetData, bool verbose);
}
