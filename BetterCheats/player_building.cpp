#include "player_building.h"
#include "plugin_helpers.h"
#include "aob_patterns.h"
#include "aob_resolver.h"
#include "session_config.h"
#include "ui_widgets.h"

#include "AuActorPlacement_classes.hpp"
#include "Chimera_classes.hpp"
#include "Engine_classes.hpp"
#include "MassActors_classes.hpp"
#include "MassEntity_classes.hpp"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace BetterCheats::Panels::Building
{
	namespace
	{
		// -------------------------------------------------------------------------
		// No Build Cost
		// Hook UCrBuildingComponent::GetResourceConditionResult to always return
		// Valid (1) when active, bypassing the inventory check entirely.
		// -------------------------------------------------------------------------

		using GetResourceConditionResultFn = int64_t(__fastcall*)(void* self);

		GetResourceConditionResultFn g_originalGetResourceConditionResult = nullptr;
		HookHandle                   g_hookGetResourceConditionResult      = nullptr;
		bool                         g_noBuildCost                         = false;

		int64_t __fastcall Detour_GetResourceConditionResult(void* self)
		{
			if (g_noBuildCost)
				return 1; // EAuAPlacementConditionResult::Valid

			return g_originalGetResourceConditionResult(self);
		}

		// -------------------------------------------------------------------------
		// No Stability Check
		// ACrAPHelperActorCustom::CheckStability is the native function that actually
		// decides whether a placement passes the stability graph — its bool return is
		// the real gate. (The data-asset flags bCheckStability /
		// RequirePlatformConnecting are never read on this path, which is why patching
		// those did nothing.) We let the original run — so the HUD stability bar still
		// reflects the real computed value — and just force the return to true while
		// the cheat is active.
		//
		// ACrAPHelperDynamicPillar::CheckStability is the equivalent gate for the
		// multi-point/zoop path (chained foundations), which does not go through the
		// Custom overload — it needs its own hook or zoop placements stay blocked.
		// -------------------------------------------------------------------------

		using CheckStabilityFn = bool(__fastcall*)(void* self, const void* placementData);

		CheckStabilityFn g_originalCheckStabilityCustom        = nullptr;
		CheckStabilityFn g_originalCheckStabilityDynamicPillar = nullptr;
		HookHandle       g_hookCheckStabilityCustom            = nullptr;
		HookHandle       g_hookCheckStabilityDynamicPillar     = nullptr;
		bool             g_noStabilityCheck                    = false;

		bool __fastcall Detour_CheckStabilityCustom(void* self, const void* placementData)
		{
			bool result = g_originalCheckStabilityCustom(self, placementData);
			return g_noStabilityCheck ? true : result;
		}

		bool __fastcall Detour_CheckStabilityDynamicPillar(void* self, const void* placementData)
		{
			bool result = g_originalCheckStabilityDynamicPillar(self, placementData);
			return g_noStabilityCheck ? true : result;
		}

		// -------------------------------------------------------------------------
		// Unlock All Buildings
		// ACrTechnologyKeeper::bUnlockAllBuildings (0x03A8) short-circuits
		// IsBuildingAvailable to return true unconditionally. Setting the flag alone
		// is not enough — CheckAvailableBuildings must be called to rebuild
		// AvailableBuildings and broadcast the menu refresh delegate.
		// When bUnlockAllBuildings is true the function ignores Corporation/Reputation,
		// so we pass nullptr/0 safely in both directions.
		// This flag may be serialised into the save file — the UI warns accordingly.
		// -------------------------------------------------------------------------

		constexpr ptrdiff_t kBUnlockAllBuildingsOffset = 0x03A8;

		bool g_unlockAllBuildings     = false;
		bool g_prevUnlockAllBuildings = false;

		using CheckAvailableBuildingsFn = void(__fastcall*)(void* self, void* corporation, int64_t reputation);
		CheckAvailableBuildingsFn g_checkAvailableBuildings = nullptr;

		// -------------------------------------------------------------------------
		// Unlock All Recipes
		// Hook ACrCraftingRecipeOwner::IsRecipeUnlocked to always return true.
		// bUnlockAllRecipes on ACrTechnologyKeeper has no xrefs — the actual gate
		// is this function, called by the crafting UI per recipe.
		// -------------------------------------------------------------------------

		using IsRecipeUnlockedFn = bool(__fastcall*)(void* self, void* recipe);

		IsRecipeUnlockedFn g_originalIsRecipeUnlocked = nullptr;
		HookHandle         g_hookIsRecipeUnlocked      = nullptr;
		bool               g_unlockAllRecipes          = false;

		bool __fastcall Detour_IsRecipeUnlocked(void* self, void* recipe)
		{
			if (g_unlockAllRecipes)
				return true;

			return g_originalIsRecipeUnlocked(self, recipe);
		}

		// -------------------------------------------------------------------------
		// Habitat window pieces
		// -------------------------------------------------------------------------

		bool TryGetBuildingId(SDK::UObject* object, SDK::ECrBuildingID& out)
		{
			SDK::UClass* buildingClass = SDK::ACrBuildingActorBase::StaticClass();
			if (!object || !buildingClass || !object->IsA(buildingClass))
				return false;

			out = static_cast<SDK::ACrBuildingActorBase*>(object)->GetBuildingID();
			return true;
		}

		bool IsWindowId(SDK::ECrBuildingID id)
		{
			return id == SDK::ECrBuildingID::ViewportLeft  || id == SDK::ECrBuildingID::ViewportMiddle
			    || id == SDK::ECrBuildingID::ViewportRight || id == SDK::ECrBuildingID::ViewportSingle;
		}

		bool IsWindow(SDK::UObject* object)
		{
			SDK::ECrBuildingID id{};
			return TryGetBuildingId(object, id) && IsWindowId(id);
		}

		bool IsHabitat(SDK::UObject* object)
		{
			SDK::ECrBuildingID id{};
			return TryGetBuildingId(object, id)
			    && (id == SDK::ECrBuildingID::HabitatBig || id == SDK::ECrBuildingID::HabitatSmall);
		}

		// The machines a habitat wall comes with are the _Variant buildings.
		bool IsHabitatMachine(SDK::UObject* object)
		{
			SDK::ECrBuildingID id{};
			if (!TryGetBuildingId(object, id))
				return false;

			switch (id)
			{
			case SDK::ECrBuildingID::Assembler_Variant:
			case SDK::ECrBuildingID::Crafter_Variant:
			case SDK::ECrBuildingID::CrafterTier2_Variant:
			case SDK::ECrBuildingID::DefenseCannon_Variant:
			case SDK::ECrBuildingID::Exporter_Variant:
			case SDK::ECrBuildingID::ExporterTier2_Variant:
			case SDK::ECrBuildingID::FactoryTier2_Variant:
			case SDK::ECrBuildingID::FactoryVariant:
			case SDK::ECrBuildingID::Forge_Variant:
			case SDK::ECrBuildingID::Furnace_Variant:
			case SDK::ECrBuildingID::FurnaceTier2_Variant:
			case SDK::ECrBuildingID::Hammer_variant:
			case SDK::ECrBuildingID::MilitaryAssembler_Variant:
			case SDK::ECrBuildingID::MilitaryCrafter_Variant:
			case SDK::ECrBuildingID::PackageReceiver_Variant:
			case SDK::ECrBuildingID::PackageSender_Variant:
			case SDK::ECrBuildingID::Pressurizer_variant:
			case SDK::ECrBuildingID::Refinery_variant:
			case SDK::ECrBuildingID::ResourceRedistributor_Variant:
			case SDK::ECrBuildingID::Smelter_variant:
			case SDK::ECrBuildingID::SmelterTier2_Variant:
			case SDK::ECrBuildingID::Storage_Variant:
			case SDK::ECrBuildingID::StorageDepot_Variant:
			case SDK::ECrBuildingID::Synthetizer_Variant:
			case SDK::ECrBuildingID::SynthetizerTier2_Variant:
			case SDK::ECrBuildingID::TurretTier2_Variant:
			case SDK::ECrBuildingID::UniversalStorage_Variant:
				return true;
			default:
				return false;
			}
		}

		// -------------------------------------------------------------------------
		// Build Windows Anywhere
		// A window snaps to a habitat wall slot only when
		// UCrBuildingStabilitySubsystem::IsSocketFree says nothing is registered on
		// that socket, and the machine a habitat wall comes with is registered on
		// its slot. While a window is being placed, a slot whose occupants are all
		// such machines counts as free. Every other piece, and every other
		// occupant, still gets the game's answer.
		//
		// GetSnappedSocket is IsSocketFree's only caller and the one place that
		// knows which piece is being placed, so it marks the window for the
		// IsSocketFree calls it makes.
		//
		// The socket map stores each occupant's ID and removal takes out only the
		// departing ID, so the window and the machine each keep their own entry and
		// either can be deconstructed first.
		// -------------------------------------------------------------------------

		using GetSnappedSocketFn = SDK::AActor*(__fastcall*)(void* self, const SDK::UAuActorPlacementData* placementData,
			void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, float* a9, void* a10);
		using IsSocketFreeFn = bool(__fastcall*)(SDK::UObject* self, SDK::UObject* building, const SDK::UStaticMeshSocket* socket);

		using WeakObjectPtrAssignFn = void(__fastcall*)(SDK::FWeakObjectPtr* self, void* const* objectPtrAddr);
		using GetEntityHandleFromActorFn = SDK::FMassEntityHandle*(__fastcall*)(
			SDK::UMassActorSubsystem* self, SDK::FMassEntityHandle* result, SDK::FWeakObjectPtr actorKey);
		using ConstructPersistentEntityIdFn = void(__fastcall*)(
			SDK::FCrMassPersistentEntityID* self, SDK::FMassEntityHandle handle, const SDK::UObject* worldContext);
		using IsValidEntityHandleFn = bool(__fastcall*)(SDK::FCrMassPersistentEntityID* self, const SDK::UObject* worldContext);
		using GetActorFromHandleFn = SDK::AActor*(__fastcall*)(
			SDK::UMassActorSubsystem* self, SDK::FMassEntityHandle handle, int32_t access);

		using CustomConnectionMap = decltype(SDK::FCrBuildingStabilitySubsystemState::CustomConnectionData);

		constexpr uint32_t kNoPersistentId    = 0xFFFFFFFF;
		constexpr int32_t  kActorOnlyWhenAlive = 0;

		GetSnappedSocketFn            g_originalGetSnappedSocket = nullptr;
		IsSocketFreeFn                g_originalIsSocketFree     = nullptr;
		HookHandle                    g_hookGetSnappedSocket     = nullptr;
		HookHandle                    g_hookIsSocketFree         = nullptr;
		WeakObjectPtrAssignFn         g_buildActorKey            = nullptr;
		GetEntityHandleFromActorFn    g_getEntityHandle          = nullptr;
		ConstructPersistentEntityIdFn g_constructPersistentId    = nullptr;
		IsValidEntityHandleFn         g_isValidEntityHandle      = nullptr;
		GetActorFromHandleFn          g_getActorFromHandle       = nullptr;
		std::atomic<bool>             g_buildWindowsAnywhere{ false };

		thread_local bool t_placingWindow = false;

		bool OccupiedOnlyByHabitatMachines(SDK::UObject* stability, SDK::UObject* building, const SDK::UStaticMeshSocket* socket)
		{
			try
			{
				if (!socket || !IsHabitat(building))
					return false;

				SDK::UClass* actorSubsystemClass = SDK::UMassActorSubsystem::StaticClass();
				SDK::UWorldSubsystem* subsystem = SDK::USubsystemBlueprintLibrary::GetWorldSubsystem(
					stability, SDK::TSubclassOf<SDK::UWorldSubsystem>(actorSubsystemClass));
				if (!subsystem || !subsystem->IsA(actorSubsystemClass))
					return false;

				auto* actorSubsystem = static_cast<SDK::UMassActorSubsystem*>(subsystem);
				if (!*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(actorSubsystem) + AOB::kMassActorManagerOffset))
					return false;

				SDK::FWeakObjectPtr actorKey{};
				void* actorPtr = building;
				g_buildActorKey(&actorKey, &actorPtr);

				SDK::FMassEntityHandle handle{};
				g_getEntityHandle(actorSubsystem, &handle, actorKey);
				if (handle.Index == 0)
					return false;

				SDK::FCrMassPersistentEntityID buildingId{};
				g_constructPersistentId(&buildingId, handle, stability);
				if (buildingId.ID == kNoPersistentId)
					return false;

				auto& connections = *reinterpret_cast<CustomConnectionMap*>(
					reinterpret_cast<uint8_t*>(stability) + AOB::kStabilityCustomConnectionsOffset);

				auto entry = connections.Find(buildingId,
					[](const SDK::FCrMassPersistentEntityID& a, const SDK::FCrMassPersistentEntityID& b) { return a.ID == b.ID; });
				if (entry == end(connections))
					return false;

				int occupants = 0;
				for (auto& slot : entry->Value().SocketConnections)
				{
					if (!(slot.Key() == socket->SocketName))
						continue;

					for (const SDK::FCrMassPersistentEntityID& occupant : slot.Value().Values)
					{
						// IsValidEntityHandle refreshes CachedHandle, so it works on a copy.
						SDK::FCrMassPersistentEntityID resolved = occupant;
						if (!g_isValidEntityHandle(&resolved, stability))
							return false;

						if (!IsHabitatMachine(g_getActorFromHandle(actorSubsystem, resolved.CachedHandle, kActorOnlyWhenAlive)))
							return false;

						++occupants;
					}
				}
				return occupants > 0;
			}
			catch (...)
			{
				return false;
			}
		}

		SDK::AActor* __fastcall Detour_GetSnappedSocket(void* self, const SDK::UAuActorPlacementData* placementData,
			void* a3, void* a4, void* a5, void* a6, void* a7, void* a8, float* a9, void* a10)
		{
			const bool outer = t_placingWindow;
			t_placingWindow = g_buildWindowsAnywhere.load() && placementData && IsWindowId(placementData->BuildingID);

			SDK::AActor* result = g_originalGetSnappedSocket(self, placementData, a3, a4, a5, a6, a7, a8, a9, a10);

			t_placingWindow = outer;
			return result;
		}

		bool __fastcall Detour_IsSocketFree(SDK::UObject* self, SDK::UObject* building, const SDK::UStaticMeshSocket* socket)
		{
			const bool isFree = g_originalIsSocketFree(self, building, socket);
			if (isFree || !t_placingWindow)
				return isFree;

			return OccupiedOnlyByHabitatMachines(self, building, socket);
		}

		// -------------------------------------------------------------------------
		// Deconstruct Windows During Waves
		// ACrPlayerControllerBase::FindDeconstructibleTarget refuses any building
		// whose temperature is above 0, and a wave heats the habitat's windows. When
		// it finds nothing, it runs once more with that one jump turned into a
		// no-op, and the answer is kept only if it is a window, so every other
		// heated building stays protected. The function reads only its inputs and
		// writes nothing, so running it twice changes nothing else.
		// -------------------------------------------------------------------------

		using FindDeconstructibleTargetFn = SDK::AActor*(__fastcall*)(SDK::ACrPlayerControllerBase* self, const void* traceData);

		FindDeconstructibleTargetFn g_originalFindDeconstructibleTarget = nullptr;
		HookHandle                  g_hookFindDeconstructibleTarget     = nullptr;
		uint8_t*                    g_heatGate                          = nullptr;
		uint8_t                     g_heatGateBytes[2]                  = {};
		std::atomic<bool>           g_deconstructWindowsDuringWaves{ false };

		// IPluginMemoryUtils::Patch logs every write, and this flips the jump on
		// every call, so it writes the two bytes itself.
		bool WriteHeatGate(const uint8_t (&bytes)[2])
		{
			DWORD oldProtect = 0;
			if (!VirtualProtect(g_heatGate, sizeof(bytes), PAGE_EXECUTE_READWRITE, &oldProtect))
				return false;

			memcpy(g_heatGate, bytes, sizeof(bytes));
			VirtualProtect(g_heatGate, sizeof(bytes), oldProtect, &oldProtect);
			FlushInstructionCache(GetCurrentProcess(), g_heatGate, sizeof(bytes));
			return true;
		}

		class HeatGateBypass
		{
		public:
			HeatGateBypass()
			{
				static constexpr uint8_t kNops[2] = { 0x90, 0x90 };
				m_open = WriteHeatGate(kNops);
			}
			~HeatGateBypass()
			{
				if (m_open)
					WriteHeatGate(g_heatGateBytes);
			}
			HeatGateBypass(const HeatGateBypass&) = delete;
			HeatGateBypass& operator=(const HeatGateBypass&) = delete;

			explicit operator bool() const { return m_open; }

		private:
			bool m_open = false;
		};

		SDK::AActor* __fastcall Detour_FindDeconstructibleTarget(SDK::ACrPlayerControllerBase* self, const void* traceData)
		{
			SDK::AActor* target = g_originalFindDeconstructibleTarget(self, traceData);
			if (target || !g_deconstructWindowsDuringWaves.load())
				return target;

			{
				HeatGateBypass bypass;
				if (!bypass)
					return nullptr;
				target = g_originalFindDeconstructibleTarget(self, traceData);
			}

			try
			{
				return IsWindow(target) ? target : nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		// -------------------------------------------------------------------------
		// World accessors — game-thread only
		// -------------------------------------------------------------------------

		SDK::ACrPlayerControllerBase* GetLocalController()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
			if (!pc) return nullptr;

			// See player_skills.cpp's GetLocalController — the controller isn't a
			// Chimera one for the whole life of the world.
			SDK::UClass* controllerClass = SDK::ACrPlayerControllerBase::StaticClass();
			if (!controllerClass || !pc->IsA(controllerClass)) return nullptr;

			return static_cast<SDK::ACrPlayerControllerBase*>(pc);
		}

		SDK::ACrTechnologyKeeper* GetTechnologyKeeper()
		{
			SDK::UWorld* world = nullptr;
			try { world = SDK::UWorld::GetWorld(); }
			catch (...) { return nullptr; }
			if (!world) return nullptr;

			SDK::AGameStateBase* gs = SDK::UGameplayStatics::GetGameState(world);
			if (!gs) return nullptr;

			return static_cast<SDK::ACrGameStateBase*>(gs)->TechnologyKeeper;
		}
	}

	void Initialize()
	{
		IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;

		if (!hooks)
		{
			LOG_WARN("Building: hook utils unavailable, no-build-cost hook skipped");
			return;
		}

		const AOB::ResolvedAddresses& aob = AOB::Resolved();

		uintptr_t addr = aob.GetResourceConditionResult;
		if (!addr)
		{
			LOG_WARN("Building: GetResourceConditionResult unresolved");
		}
		else
		{
			g_hookGetResourceConditionResult = hooks->Install(
				addr,
				reinterpret_cast<void*>(&Detour_GetResourceConditionResult),
				reinterpret_cast<void**>(&g_originalGetResourceConditionResult));

			if (!g_hookGetResourceConditionResult)
			{
				LOG_WARN("Building: failed to install GetResourceConditionResult hook");
			}
			else
			{
				LOG_INFO("Building: GetResourceConditionResult hook installed");
			}
		}

		uintptr_t checkAddr = aob.CheckAvailableBuildings;
		if (!checkAddr)
		{
			LOG_WARN("Building: CheckAvailableBuildings unresolved — unlock all buildings will not refresh the menu");
		}
		else
		{
			g_checkAvailableBuildings = reinterpret_cast<CheckAvailableBuildingsFn>(checkAddr);
			LOG_INFO("Building: CheckAvailableBuildings resolved");
		}

		uintptr_t recipeAddr = aob.IsRecipeUnlocked;
		if (!recipeAddr)
		{
			LOG_WARN("Building: IsRecipeUnlocked unresolved");
		}
		else
		{
			g_hookIsRecipeUnlocked = hooks->Install(
				recipeAddr,
				reinterpret_cast<void*>(&Detour_IsRecipeUnlocked),
				reinterpret_cast<void**>(&g_originalIsRecipeUnlocked));

			if (!g_hookIsRecipeUnlocked)
				LOG_WARN("Building: failed to install IsRecipeUnlocked hook");
			else
				LOG_INFO("Building: IsRecipeUnlocked hook installed");
		}

		uintptr_t stabilityCustomAddr = aob.CheckStability_Custom;
		if (!stabilityCustomAddr)
		{
			LOG_WARN("Building: CheckStability_Custom unresolved");
		}
		else
		{
			g_hookCheckStabilityCustom = hooks->Install(
				stabilityCustomAddr,
				reinterpret_cast<void*>(&Detour_CheckStabilityCustom),
				reinterpret_cast<void**>(&g_originalCheckStabilityCustom));

			if (!g_hookCheckStabilityCustom)
				LOG_WARN("Building: failed to install CheckStability_Custom hook");
			else
				LOG_INFO("Building: CheckStability_Custom hook installed");
		}

		uintptr_t stabilityPillarAddr = aob.CheckStability_DynamicPillar;
		if (!stabilityPillarAddr)
		{
			LOG_WARN("Building: CheckStability_DynamicPillar unresolved — zoop placements will still be stability-checked");
		}
		else
		{
			g_hookCheckStabilityDynamicPillar = hooks->Install(
				stabilityPillarAddr,
				reinterpret_cast<void*>(&Detour_CheckStabilityDynamicPillar),
				reinterpret_cast<void**>(&g_originalCheckStabilityDynamicPillar));

			if (!g_hookCheckStabilityDynamicPillar)
				LOG_WARN("Building: failed to install CheckStability_DynamicPillar hook");
			else
				LOG_INFO("Building: CheckStability_DynamicPillar hook installed");
		}

		// aob_resolver.cpp has already logged the line for a group that missed.
		if (aob.GetSnappedSocket && aob.FWeakObjectPtr_AssignFObjectPtr && aob.UMassActorSubsystem_GetEntityHandleFromActor)
		{
			g_buildActorKey         = reinterpret_cast<WeakObjectPtrAssignFn>(aob.FWeakObjectPtr_AssignFObjectPtr);
			g_getEntityHandle       = reinterpret_cast<GetEntityHandleFromActorFn>(aob.UMassActorSubsystem_GetEntityHandleFromActor);
			g_constructPersistentId = reinterpret_cast<ConstructPersistentEntityIdFn>(aob.PersistentEntityID_Construct);
			g_isValidEntityHandle   = reinterpret_cast<IsValidEntityHandleFn>(aob.PersistentEntityID_IsValidEntityHandle);
			g_getActorFromHandle    = reinterpret_cast<GetActorFromHandleFn>(aob.MassActorSubsystem_GetActorFromHandle);

			g_hookIsSocketFree = hooks->Install(
				aob.IsSocketFree,
				reinterpret_cast<void*>(&Detour_IsSocketFree),
				reinterpret_cast<void**>(&g_originalIsSocketFree));
			if (g_hookIsSocketFree)
			{
				g_hookGetSnappedSocket = hooks->Install(
					aob.GetSnappedSocket,
					reinterpret_cast<void*>(&Detour_GetSnappedSocket),
					reinterpret_cast<void**>(&g_originalGetSnappedSocket));
				if (!g_hookGetSnappedSocket)
				{
					hooks->Remove(g_hookIsSocketFree);
					g_hookIsSocketFree     = nullptr;
					g_originalIsSocketFree = nullptr;
				}
			}

			if (g_hookGetSnappedSocket)
				LOG_INFO("Building: window placement hooks installed");
			else
				LOG_WARN("Building: failed to install window placement hooks - Build Windows Anywhere stays off");
		}

		if (aob.FindDeconstructibleTarget)
		{
			g_heatGate = reinterpret_cast<uint8_t*>(aob.FindDeconstructibleTarget_HeatGate);
			memcpy(g_heatGateBytes, g_heatGate, sizeof(g_heatGateBytes));

			g_hookFindDeconstructibleTarget = hooks->Install(
				aob.FindDeconstructibleTarget,
				reinterpret_cast<void*>(&Detour_FindDeconstructibleTarget),
				reinterpret_cast<void**>(&g_originalFindDeconstructibleTarget));
			if (!g_hookFindDeconstructibleTarget)
				LOG_WARN("Building: failed to install FindDeconstructibleTarget hook - Deconstruct Windows During Waves stays off");
			else
				LOG_INFO("Building: FindDeconstructibleTarget hook installed");
		}
	}

	void Shutdown()
	{
		IPluginHookUtils* hooks = GetHooks() ? GetHooks()->Hooks : nullptr;
		if (hooks && g_hookGetResourceConditionResult)
		{
			hooks->Remove(g_hookGetResourceConditionResult);
			g_hookGetResourceConditionResult      = nullptr;
			g_originalGetResourceConditionResult  = nullptr;
		}

		if (hooks && g_hookIsRecipeUnlocked)
		{
			hooks->Remove(g_hookIsRecipeUnlocked);
			g_hookIsRecipeUnlocked      = nullptr;
			g_originalIsRecipeUnlocked  = nullptr;
		}
		g_unlockAllRecipes = false;

		if (hooks && g_hookCheckStabilityCustom)
		{
			hooks->Remove(g_hookCheckStabilityCustom);
			g_hookCheckStabilityCustom     = nullptr;
			g_originalCheckStabilityCustom = nullptr;
		}

		if (hooks && g_hookCheckStabilityDynamicPillar)
		{
			hooks->Remove(g_hookCheckStabilityDynamicPillar);
			g_hookCheckStabilityDynamicPillar     = nullptr;
			g_originalCheckStabilityDynamicPillar = nullptr;
		}
		g_noStabilityCheck = false;

		if (hooks && g_hookGetSnappedSocket)
		{
			hooks->Remove(g_hookGetSnappedSocket);
			hooks->Remove(g_hookIsSocketFree);
			g_hookGetSnappedSocket     = nullptr;
			g_hookIsSocketFree         = nullptr;
			g_originalGetSnappedSocket = nullptr;
			g_originalIsSocketFree     = nullptr;
		}
		g_buildWindowsAnywhere = false;

		if (hooks && g_hookFindDeconstructibleTarget)
		{
			hooks->Remove(g_hookFindDeconstructibleTarget);
			g_hookFindDeconstructibleTarget     = nullptr;
			g_originalFindDeconstructibleTarget = nullptr;
		}
		g_deconstructWindowsDuringWaves = false;

		if (g_unlockAllBuildings)
		{
			g_unlockAllBuildings     = false;
			g_prevUnlockAllBuildings = false;
			try
			{
				if (SDK::ACrTechnologyKeeper* keeper = GetTechnologyKeeper())
				{
					*reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(keeper) + kBUnlockAllBuildingsOffset) = false;
					if (g_checkAvailableBuildings)
						g_checkAvailableBuildings(keeper, nullptr, 0);
				}
			}
			catch (...) {}
		}
		g_prevUnlockAllBuildings  = false;
		g_checkAvailableBuildings = nullptr;
	}

	void Tick(float /*deltaSeconds*/)
	{
		// No Stability Check is purely hook-driven (see Detour_CheckStabilityCustom /
		// Detour_CheckStabilityDynamicPillar above) — nothing to do here per-tick.

		// Unlock all buildings — only act on change so we don't spam CheckAvailableBuildings.
		// Writing the flag alone is insufficient; the function must run to rebuild
		// AvailableBuildings and fire the delegate that refreshes the build menu.
		if (g_unlockAllBuildings != g_prevUnlockAllBuildings)
		{
			g_prevUnlockAllBuildings = g_unlockAllBuildings;
			LOG_INFO("Building: unlock all buildings -> %s", g_unlockAllBuildings ? "enabled" : "disabled");
			try
			{
				SDK::ACrTechnologyKeeper* keeper = GetTechnologyKeeper();
				LOG_DEBUG("Building: TechnologyKeeper = 0x%llx", reinterpret_cast<unsigned long long>(keeper));
				if (keeper)
				{
					*reinterpret_cast<bool*>(reinterpret_cast<uint8_t*>(keeper) + kBUnlockAllBuildingsOffset) = g_unlockAllBuildings;
					if (g_checkAvailableBuildings)
					{
						LOG_DEBUG("Building: calling CheckAvailableBuildings (keeper=0x%llx, bUnlockAll=%d)",
							reinterpret_cast<unsigned long long>(keeper), g_unlockAllBuildings);
						g_checkAvailableBuildings(keeper, nullptr, 0);
						LOG_DEBUG("Building: CheckAvailableBuildings returned");
					}
					else
					{
						LOG_WARN("Building: CheckAvailableBuildings not resolved — menu will not refresh");
					}
				}
			}
			catch (...) { LOG_WARN("Building: exception in unlock all buildings toggle"); }
		}

	}

	void ApplySavedConfig()
	{
		if (!SessionConfig::IsLoaded())
			return;

		g_noBuildCost        = SessionConfig::Get("playerBuilding.noBuildCost", false);
		g_noStabilityCheck   = SessionConfig::Get("playerBuilding.noStabilityCheck", false);
		g_unlockAllBuildings = SessionConfig::Get("playerBuilding.unlockAllBuildings", false);
		g_unlockAllRecipes   = SessionConfig::Get("playerBuilding.unlockAllRecipes", false);
		g_buildWindowsAnywhere          = SessionConfig::Get("playerBuilding.buildWindowsAnywhere", false);
		g_deconstructWindowsDuringWaves = SessionConfig::Get("playerBuilding.deconstructWindowsDuringWaves", false);

		LOG_INFO("Building: applied saved config for session '%s'.", SessionConfig::GetSessionName().c_str());
	}

	void RenderImGui(IModLoaderImGui* imgui)
	{
		// ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp
		constexpr int kTableFlags    = (1 << 6) | (1 << 9) | (3 << 13);

		// No built-in presets here, so saved presets sit at the very top --
		// same "above every control" position every group uses.
		{
			static BetterCheats::UI::SavedPresetRowState s_presetRow;
			constexpr int kFieldCount = 6;
			BetterCheats::PresetStore::Field fields[kFieldCount];

			auto getLive = [](BetterCheats::PresetStore::Field* out)
			{
				out[0] = { "noBuildCost",         g_noBuildCost         ? 1.0f : 0.0f };
				out[1] = { "noStabilityCheck",    g_noStabilityCheck    ? 1.0f : 0.0f };
				out[2] = { "unlockAllBuildings",  g_unlockAllBuildings  ? 1.0f : 0.0f };
				out[3] = { "unlockAllRecipes",    g_unlockAllRecipes    ? 1.0f : 0.0f };
				out[4] = { "buildWindowsAnywhere",          g_buildWindowsAnywhere.load()          ? 1.0f : 0.0f };
				out[5] = { "deconstructWindowsDuringWaves", g_deconstructWindowsDuringWaves.load() ? 1.0f : 0.0f };
			};
			auto applyFields = [](const BetterCheats::PresetStore::Field* f, int count)
			{
				if (count > 0) { g_noBuildCost        = f[0].value != 0.0f; SessionConfig::Set("playerBuilding.noBuildCost", g_noBuildCost); }
				if (count > 1) { g_noStabilityCheck   = f[1].value != 0.0f; SessionConfig::Set("playerBuilding.noStabilityCheck", g_noStabilityCheck); }
				if (count > 2) { g_unlockAllBuildings = f[2].value != 0.0f; SessionConfig::Set("playerBuilding.unlockAllBuildings", g_unlockAllBuildings); }
				if (count > 3) { g_unlockAllRecipes   = f[3].value != 0.0f; SessionConfig::Set("playerBuilding.unlockAllRecipes", g_unlockAllRecipes); }
				if (count > 4)
				{
					const bool v = f[4].value != 0.0f;
					g_buildWindowsAnywhere = v;
					SessionConfig::Set("playerBuilding.buildWindowsAnywhere", v);
				}
				if (count > 5)
				{
					const bool v = f[5].value != 0.0f;
					g_deconstructWindowsDuringWaves = v;
					SessionConfig::Set("playerBuilding.deconstructWindowsDuringWaves", v);
				}
			};
			auto isBuiltin      = [](const char*) { return false; };
			auto computeSuggest = [](char* out, int cap) { snprintf(out, cap, "Custom"); };

			BetterCheats::UI::RenderSavedPresetsRow(imgui, "building_saved_presets", "Building",
				fields, kFieldCount, getLive, applyFields, isBuiltin, computeSuggest, s_presetRow);
		}
		imgui->Spacing();

		imgui->SeparatorText("Placement");

		if (imgui->BeginTable("##building_placement_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option",  0, 0.85f);
			imgui->TableSetupColumn("Enabled", 0, 0.15f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("No Build Cost");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##no_build_cost", &g_noBuildCost))
				SessionConfig::Set("playerBuilding.noBuildCost", g_noBuildCost);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("No Stability Check");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##no_stability_check", &g_noStabilityCheck))
				SessionConfig::Set("playerBuilding.noStabilityCheck", g_noStabilityCheck);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Build Windows Anywhere");
			imgui->TableSetColumnIndex(1);
			bool buildWindowsAnywhere = g_buildWindowsAnywhere.load();
			if (imgui->Checkbox("##build_windows_anywhere", &buildWindowsAnywhere))
			{
				g_buildWindowsAnywhere = buildWindowsAnywhere;
				SessionConfig::Set("playerBuilding.buildWindowsAnywhere", buildWindowsAnywhere);
			}
			imgui->SetItemTooltip("Lets a habitat window share a wall slot with the machine built into that wall.");

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Deconstruct Windows During Waves");
			imgui->TableSetColumnIndex(1);
			bool deconstructWindows = g_deconstructWindowsDuringWaves.load();
			if (imgui->Checkbox("##deconstruct_windows_waves", &deconstructWindows))
			{
				g_deconstructWindowsDuringWaves = deconstructWindows;
				SessionConfig::Set("playerBuilding.deconstructWindowsDuringWaves", deconstructWindows);
			}
			imgui->SetItemTooltip("Lets habitat windows be deconstructed while a wave has heated them. Other heated buildings stay protected.");

			imgui->EndTable();
		}

		imgui->Spacing();
		imgui->SeparatorText("Research");
		imgui->TextWrapped(
			"Warning: While attention has been put into this to try to not make these changes permanent in your save file, it may still happen.");
		imgui->Spacing();

		if (imgui->BeginTable("##building_research_table", 2, kTableFlags))
		{
			imgui->TableSetupColumn("Option",  0, 0.85f);
			imgui->TableSetupColumn("Enabled", 0, 0.15f);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Unlock All Buildings");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##unlock_buildings", &g_unlockAllBuildings))
				SessionConfig::Set("playerBuilding.unlockAllBuildings", g_unlockAllBuildings);

			imgui->TableNextRow(0, 0.0f);
			imgui->TableSetColumnIndex(0);
			imgui->Text("Unlock All Recipes");
			imgui->TableSetColumnIndex(1);
			if (imgui->Checkbox("##unlock_recipes", &g_unlockAllRecipes))
				SessionConfig::Set("playerBuilding.unlockAllRecipes", g_unlockAllRecipes);

			imgui->EndTable();
		}
	}
}
