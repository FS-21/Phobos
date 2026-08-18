#pragma once
#include <HouseClass.h>

#include <Ext/HouseType/Body.h>

#include <Utilities/Container.h>
#include <Utilities/Detach.h>
#include <Utilities/TemplateDef.h>
#include <Ext/Building/Body.h>

#include <map>
#include <string>
#include <array>
#include <vector>

#include "New/Type/TechTreeTypeClass.h"

class HouseExt final : public AbstractExt, public Detach::Listener<BuildingClass>
{
public:
	using base_type = HouseClass;

	// deprecated: the pre-rework nested data class is now the extension class itself
	using ExtData [[deprecated("use the extension class itself instead")]] = HouseExt;

	static constexpr DWORD Canary = 0x11111111;

public:
	// typed owner accessor
	HouseClass* OwnerObject() const
	{
		return static_cast<HouseClass*>(this->GetAttachedObject());
	}

	std::vector<BuildingClass*> PowerPlantEnhancers;
	std::vector<BuildingClass*> OwnedLimboDeliveredBuildings;
	std::vector<TechnoClass*> OwnedCountedHarvesters;
	bool ForceOnlyTargetHouseEnemy;
	int ForceOnlyTargetHouseEnemyMode;

	CounterClass LimboAircraft;  // Currently owned aircraft in limbo
	CounterClass LimboBuildings; // Currently owned buildings in limbo
	CounterClass LimboInfantry;  // Currently owned infantry in limbo
	CounterClass LimboVehicles;  // Currently owned vehicles in limbo

	BuildingClass* Factory_BuildingType;
	BuildingClass* Factory_InfantryType;
	BuildingClass* Factory_VehicleType;
	BuildingClass* Factory_NavyType;
	BuildingClass* Factory_AircraftType;

	CDTimerClass CombatAlertTimer;
	CDTimerClass AISuperWeaponDelayTimer;
	CDTimerClass AIFireSaleDelayTimer;

	//Read from INI
	Nullable<bool> RepairBaseNodes[3];

	// FactoryPlants with Allow/DisallowTypes set.
	std::vector<BuildingClass*> RestrictedFactoryPlants;

	int LastBuiltNavalVehicleType;
	int ProducingNavalUnitTypeIndex;

	// Factories that exist but don't count towards multiple factory bonus.
	int NumAirpads_NonMFB;
	int NumBarracks_NonMFB;
	int NumWarFactories_NonMFB;
	int NumConYards_NonMFB;
	int NumShipyards_NonMFB;

	std::map<int, std::vector<int>> SuspendedEMPulseSWs;

	struct SWExt
	{
		int ShotCount;
	};
	std::vector<SWExt> SuperExts;

	int ForceEnemyIndex;
	int TeamDelay;
	bool FreeRadar;
	bool ForceRadar;

	bool PlayerAutoRepair;

	std::array<int, 3> BeaconsPlacedOrder;

	TechTreeTypeClass* PrimaryTechTreeType;
	TechTreeTypeClass* SecondaryTechTreeType;

	/**
	 *  If we are currently expanding our base towards a resourceful location,
	 *  this records the cell that we are expanding towards.
	 */
	CellStruct NextExpansionPointLocation;
	CellStruct CombatCrawlingTarget;
	CellStruct ResourceCrawlingTarget;
	int ConsecutiveCombatBuilds;
	bool ResourceShouldBuildRefinery;

	struct BlockedExpansionPoint
	{
		CellStruct Coords { 0, 0 };
		int ExpiryFrame { 0 };
		int FailureCount { 0 };
	};

	/**
	 *  Locations that we should never expand towards.
	 *  Basically, locations that are unreachable.
	 */
	BlockedExpansionPoint PermanentlyBlockedExpansionPointLocations[20] {};

	/**
	 *  Records whether the AI has reached its expansion point.
	 *  If yes, the AI should build a refinery.
	 */
	bool ShouldBuildRefinery;
	bool ShouldPlaceDefenseAtBlockedEdge;
	int ExpansionPlacementFailures;
	int LastFactoryRecycleFrame;
	int NextExpansionSearchFrame;
	int NextBlacklistClearFrame;

	/**
	 *  Set when the AI has built its first barracks during the game.
	 *  Used to figure out whether the AI should reset its TeamDelay
	 *  timer when it has built a barracks.
	 */
	bool HasBuiltFirstBarracks;

	/**
	 *  Records when the AI last checked for excess refineries.
	 */
	int LastExcessRefineryCheckFrame;
	int LastObsoleteRefineryCheckFrame;
	int LastServiceDepotPlacementFailedFrame;

	/**
	 *  Records when the AI last checked for sleeping harvesters.
	 */
	int LastSleepingHarvesterCheckFrame;
	int LastPrimaryFactoryCheckFrame;

	/**
	 *  Defines whether the AI has already performed a final "desperate vehicle charge".
	 *  If it has been done, then there is no need to do it again.
	 */
	bool HasPerformedVehicleCharge;

	/**
	 *  Records a value whether the current structure build choice
	 *  was made under threat of getting rushed early in the game.
	 */
	bool IsUnderStartRushThreat;

	/**
	 *  Records cooldown frames for building types that failed to be placed.
	 *  AI will not attempt to build these building types until the frame has passed.
	 */
	std::map<BuildingTypeClass*, int> PlacementFailedCooldowns;
	std::map<BuildingTypeClass*, int> PlacementConsecutiveFailures;
	std::map<std::string, int> GroupConsecutiveFailures;
	std::map<std::string, int> GroupPlacementCooldowns;
	std::map<BuildingTypeClass*, int> FeasibilityFailedCooldowns;

	/**
	 *  Records the dynamic build counts calculated for each building type
	 *  including base AIBuildCounts and probabilistic AIExtraCounts.
	 */
	std::map<BuildingTypeClass*, int> AICachedBuildCounts;
	TechnoTypeClass* LastAttackerType;
	int LastAttackedFrame;
	CellStruct LastAttackedBuildingCoords;
	CellStruct LastAttackerCoords;
	CellStruct FrontlineThreatCoords;
	int FrontlineThreatActiveFrames;
	int FrontlineThreatNeedsDefenses;
	CellStruct FrontlineThreatBuildingCoords;

	struct UnsafePlacementZone
	{
		CellStruct Coords;
		int ExpiryFrame;
	};
	std::vector<UnsafePlacementZone> UnsafePlacementZones;

	std::vector<CellStruct> UnclaimedTiberiumZones;
	CellStruct NextRefineryPlacementLocation;
	int LastUnclaimedTiberiumCheckFrame;

	std::vector<CellStruct> CachedReachableResourceFields;
	int NextReachableResourceScanFrame;

	std::vector<CellStruct> CachedResourceCandidates;
	int CachedResourceCandidatesExpiryFrame;

	struct CellStructComparator
	{
		bool operator()(const CellStruct& a, const CellStruct& b) const
		{
			if (a.X != b.X)
				return a.X < b.X;
			return a.Y < b.Y;
		}
	};
	std::map<CellStruct, int, CellStructComparator> ExpansionNodeFailureCounts;

	std::vector<CellStruct> CachedResourcePath;
	CellStruct CachedResourcePathTarget;
	CellStruct CachedResourcePathStart;

	std::vector<CellStruct> CachedCombatPath;
	CellStruct CachedCombatPathTarget;
	CellStruct CachedCombatPathStart;

	std::vector<CellStruct> CachedExpansionPath;
	CellStruct CachedExpansionPathTarget;
	CellStruct CachedExpansionPathStart;
	CellStruct GetCrawlingWaypoint(CellStruct targetCell);

	HouseClass* TargetAlliedFallbackHouse;

	HouseExt(HouseClass* OwnerObject) : AbstractExt(OwnerObject)
		, TargetAlliedFallbackHouse { nullptr }
		, PowerPlantEnhancers {}
		, OwnedLimboDeliveredBuildings {}
		, OwnedCountedHarvesters {}
		, LimboAircraft {}
		, LimboBuildings {}
		, LimboInfantry {}
		, LimboVehicles {}
		, Factory_BuildingType { nullptr }
		, Factory_InfantryType { nullptr }
		, Factory_VehicleType { nullptr }
		, Factory_NavyType { nullptr }
		, Factory_AircraftType { nullptr }
		, AISuperWeaponDelayTimer {}
		, RepairBaseNodes { }
		, RestrictedFactoryPlants {}
		, LastBuiltNavalVehicleType { -1 }
		, ProducingNavalUnitTypeIndex { -1 }
		, CombatAlertTimer {}
		, NumAirpads_NonMFB { 0 }
		, NumBarracks_NonMFB { 0 }
		, NumWarFactories_NonMFB { 0 }
		, NumConYards_NonMFB { 0 }
		, NumShipyards_NonMFB { 0 }
		, AIFireSaleDelayTimer {}
		, SuspendedEMPulseSWs {}
		, SuperExts(SuperWeaponTypeClass::Array.Count)
		, ForceEnemyIndex(-1)
		, ForceOnlyTargetHouseEnemy { false }
		, ForceOnlyTargetHouseEnemyMode { -1 }
		, TeamDelay(-1)
		, FreeRadar(false)
		, ForceRadar(false)
		, PlayerAutoRepair(true)
		, BeaconsPlacedOrder { 0, 0, 0 }
		, PrimaryTechTreeType { nullptr }
		, SecondaryTechTreeType { nullptr }
		, NextExpansionPointLocation { 0, 0 }
		, CombatCrawlingTarget { 0, 0 }
		, ResourceCrawlingTarget { 0, 0 }
		, ConsecutiveCombatBuilds { 0 }
		, ResourceShouldBuildRefinery { false }
		, PermanentlyBlockedExpansionPointLocations {}
		, ShouldBuildRefinery { false }
		, ShouldPlaceDefenseAtBlockedEdge { false }
		, ExpansionPlacementFailures { 0 }
		, LastFactoryRecycleFrame { 0 }
		, NextExpansionSearchFrame { 0 }
		, NextBlacklistClearFrame { 0 }
		, HasBuiltFirstBarracks { false }
		, LastExcessRefineryCheckFrame { 0 }
		, LastObsoleteRefineryCheckFrame { 0 }
		, LastServiceDepotPlacementFailedFrame { 0 }
		, LastSleepingHarvesterCheckFrame { 0 }
		, LastPrimaryFactoryCheckFrame { 0 }
		, HasPerformedVehicleCharge { false }
		, IsUnderStartRushThreat { false }
		, PlacementFailedCooldowns {}
		, PlacementConsecutiveFailures {}
		, GroupConsecutiveFailures {}
		, GroupPlacementCooldowns {}
		, FeasibilityFailedCooldowns {}
		, AICachedBuildCounts {}
		, LastAttackerType { nullptr }
		, LastAttackedFrame { 0 }
		, LastAttackedBuildingCoords { 0, 0 }
		, LastAttackerCoords { 0, 0 }
		, FrontlineThreatCoords { 0, 0 }
		, FrontlineThreatActiveFrames { 0 }
		, FrontlineThreatNeedsDefenses { 0 }
		, FrontlineThreatBuildingCoords { 0, 0 }
		, UnsafePlacementZones {}
		, UnclaimedTiberiumZones {}
		, NextRefineryPlacementLocation { 0, 0 }
		, LastUnclaimedTiberiumCheckFrame { 0 }
		, CachedReachableResourceFields {}
		, NextReachableResourceScanFrame { 0 }
		, CachedResourceCandidates {}
		, CachedResourceCandidatesExpiryFrame { 0 }
		, ExpansionNodeFailureCounts {}
		, CachedResourcePath {}
		, CachedResourcePathTarget { 0, 0 }
		, CachedResourcePathStart { 0, 0 }
		, CachedCombatPath {}
		, CachedCombatPathTarget { 0, 0 }
		, CachedCombatPathStart { 0, 0 }
		, CachedExpansionPath {}
		, CachedExpansionPathTarget { 0, 0 }
		, CachedExpansionPathStart { 0, 0 }
	{ }

	bool OwnsLimboDeliveredBuilding(BuildingClass* pBuilding) const;
	void AddToLimboTracking(TechnoTypeClass* pTechnoType);
	void RemoveFromLimboTracking(TechnoTypeClass* pTechnoType);
	int CountOwnedPresentAndLimboed(TechnoTypeClass* pTechnoType) const;
	void UpdateNonMFBFactoryCounts(AbstractType rtti, bool remove, bool isNaval);
	int GetFactoryCountWithoutNonMFB(AbstractType rtti, bool isNaval) const;
	float GetRestrictedFactoryPlantMult(TechnoTypeClass* pTechnoType) const;

	int GetForceEnemyIndex();
	void SetForceEnemyIndex(int EnemyIndex);

	virtual ~HouseExt() = default;

	virtual void LoadFromINIFile(CCINIClass* pINI) override;
	//virtual void Initialize() override;
	virtual void OnDetach(BuildingClass* pTarget, bool removed) override;

	void UpdateVehicleProduction();

	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);
	bool UpdateHarvesterProduction();

public:
	class ExtContainer final : public Container<HouseExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static HouseExt* Fetch(const HouseClass* pThis)
	{
		return AbstractExt::Fetch<HouseExt>(pThis);
	}

	static HouseExt* TryFetch(const HouseClass* pThis)
	{
		return AbstractExt::TryFetch<HouseExt>(pThis);
	}

	static bool LoadGlobals(PhobosStreamReader& Stm);
	static bool SaveGlobals(PhobosStreamWriter& Stm);

	static void InitializeBaseDefenses();
	static bool BaseDefensesInitialized;

	static int ActiveHarvesterCount(HouseClass* pThis);
	static int TotalHarvesterCount(HouseClass* pThis);
	static HouseClass* GetHouseKind(OwnerHouseKind kind, bool allowRandom, HouseClass* pDefault, HouseClass* pInvoker = nullptr, HouseClass* pVictim = nullptr);
	static CellClass* GetEnemyBaseGatherCell(HouseClass* pTargetHouse, HouseClass* pCurrentHouse, CoordStruct defaultCurrentCoords, SpeedType speedTypeZone, int extraDistance = 0);
	static void GetAIChronoshiftSupers(HouseClass* pThis, SuperClass*& pSuperCSphere, SuperClass*& pSuperCWarp);

	static void SetForceOnlyTargetHouseEnemy(HouseClass* pThis, int mode = -1);
	static void SetSkirmishHouseName(HouseClass* pHouse);

	static bool AdvAI_House_Search_For_Next_Expansion_Point(HouseClass* pHouse);
	static std::vector<CellStruct> AdvAI_Get_Reachable_Resource_Fields(HouseClass* pHouse);
	static int AdvAI_GetMobileRefineryTargetCount(HouseClass* pHouse);
	static bool AdvAI_IsMobileRefineryHouse(HouseClass* pHouse);
	static bool AdvAI_CanBuildAnyStaticRefinery(HouseClass* pHouse);
	static void AdvAI_Add_Failed_Expansion_Point(HouseClass* pHouse, CellStruct coords);
	static bool AdvAI_Is_Failed_Expansion_Point(HouseClass* pHouse, CellStruct coords);
	static bool AdvAI_Has_Failed_Placement_Three_Times(HouseClass* pHouse, CellStruct coords);
	static bool AdvAI_Can_Build_Building(HouseClass* pHouse, BuildingTypeClass* pBuildingType, bool checkPrereqs, bool isTechTree = false);
	static BuildingTypeClass* AdvAI_Find_Next_Buildable_Prerequisite(HouseClass* pHouse, BuildingTypeClass* pTargetType, BuildingTypeClass* pRootGoalType, std::set<BuildingTypeClass*>& visited, bool& outIsSubPrereq);
	static bool AdvAI_Is_Recently_Attacked(HouseClass* pHouse);
	static bool AdvAI_Is_Under_Start_Rush_Threat(HouseClass* pHouse, int enemyAircraftValue);
	static int AdvAI_Calculate_Enemy_Aircraft_Value(HouseClass* pHouse);
	static const BuildingTypeClass* AdvAI_Evaluate_Get_Best_Building(HouseClass* pHouse);
	static const BuildingTypeClass* AdvAI_BuildAtLeastNOfSideAndMInTotal(HouseClass* pHouse, TechTreeTypeClass* techTree, TechTreeTypeClass::BuildType buildType, int sideBuildingsWanted, int totalBuildingsWanted, int extraCount = 0);
	static const BuildingTypeClass* AdvAI_Get_Building_To_Build(HouseClass* pHouse);
	static void AdvAI_Raise_Money(HouseClass* pHouse);
	static void AdvAI_Economy_Upkeep(HouseClass* pHouse);
	static void AdvAI_Awaken_Sleeping_Harvesters(HouseClass* pHouse);
	static void AdvAI_Sell_Extra_ConYards(HouseClass* pHouse);
	static void Vinifera_HouseClass_AI_Building(HouseClass* pHouse);
	static void AdvAI_HouseClass_Expert_AI(HouseClass* pHouse);
	static void AdvAI_Update_Primary_Factories(HouseClass* pHouse);
	static void AdvAI_Recycle_Furthest_Factory(HouseClass* pHouse, AbstractType factoryType, bool isNaval, size_t optimalCount, CellStruct targetCell);
	static void AdvAI_Recycle_Obsolete_Refineries(HouseClass* pHouse);
	static void AdvAI_Update_Unclaimed_Tiberium_Zones(HouseClass* pHouse);

	static int FindGenericPrerequisite(const char* id);
	static bool HasGenericPrerequisite(int idx, HouseClass* pHouse);
	static bool PrerequisitesMet(HouseClass* pHouse, TechnoTypeClass* pItem, bool skipSecretLabChecks = false);

	static bool IsDisabledFromShell(
	HouseClass const* pHouse, BuildingTypeClass const* pItem);

	static size_t FindOwnedIndex(
	HouseClass const* pHouse, int idxParentCountry,
	Iterator<TechnoTypeClass const*> items, size_t start = 0);

	static size_t FindBuildableIndex(
		HouseClass const* pHouse, int idxParentCountry,
		Iterator<TechnoTypeClass const*> items, size_t start = 0);

	template <typename T>
	static T* FindOwned(
		HouseClass const* const pHouse, int const idxParent,
		Iterator<T*> const items, size_t const start = 0)
	{
		auto const index = FindOwnedIndex(pHouse, idxParent, items, start);
		return index < items.size() ? items[index] : nullptr;
	}

	template <typename T>
	static T* FindBuildable(
		HouseClass const* const pHouse, int const idxParent,
		Iterator<T*> const items, size_t const start = 0)
	{
		auto const index = FindBuildableIndex(pHouse, idxParent, items, start);
		return index < items.size() ? items[index] : nullptr;
	}

	static std::vector<int> AIProduction_CreationFrames;
	static std::vector<int> AIProduction_Values;
	static std::vector<int> AIProduction_BestChoices;
	static std::vector<int> AIProduction_BestChoicesNaval;

	static CanBuildResult BuildLimitGroupUpgradeCheck(const HouseClass* pThis, const TechnoTypeClass* pItem, bool buildLimitOnly, bool includeQueued);
	static bool ReachedBuildLimit(const HouseClass* pHouse, const TechnoTypeClass* pType, bool ignoreQueued);

	static void CalculatePowerSurplus(HouseClass* pThis);

	static std::vector<BuildingTypeClass*> BaseDefenses;
};
