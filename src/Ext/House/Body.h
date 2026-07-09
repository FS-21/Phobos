#pragma once
#include <HouseClass.h>

#include <Utilities/Container.h>
#include <Utilities/TemplateDef.h>

#include <Ext/Building/Body.h>

#include <map>

#include "New/Type/TechTreeTypeClass.h"

class HouseExt
{
public:
	using base_type = HouseClass;

	static constexpr DWORD Canary = 0x11111111;
	static constexpr size_t ExtPointerOffset = 0x16098;
	static constexpr bool ShouldConsiderInvalidatePointer = true;

	class ExtData final : public Extension<HouseClass>
	{
	public:
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

		// standalone? no need and not a good idea
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

		TechTreeTypeClass* PrimaryTechTreeType;
		TechTreeTypeClass* SecondaryTechTreeType;

		/**
		 *  If we are currently expanding our base towards a resourceful location,
		 *  this records the cell that we are expanding towards.
		 */
		CellStruct NextExpansionPointLocation { 0, 0 };

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
		bool ShouldBuildRefinery { false };
		int ExpansionPlacementFailures;
		int LastFactoryRecycleFrame;
		int NextExpansionSearchFrame;
		int NextBlacklistClearFrame;

		/**
		 *  Set when the AI has built its first barracks during the game.
		 *  Used to figure out whether the AI should reset its TeamDelay
		 *  timer when it has built a barracks.
		 */
		bool HasBuiltFirstBarracks { false };

		/**
		 *  Records when the AI last checked for excess refineries.
		 */
		int LastExcessRefineryCheckFrame { 0 };
		int LastObsoleteRefineryCheckFrame { 0 };
		int LastServiceDepotPlacementFailedFrame { 0 };

		/**
		 *  Records when the AI last checked for sleeping harvesters.
		 */
		int LastSleepingHarvesterCheckFrame { 0 };
		int LastPrimaryFactoryCheckFrame { 0 };

		/**
		 *  Defines whether the AI has already performed a final "desperate vehicle charge".
		 *  If it has been done, then there is no need to do it again.
		 */
		bool HasPerformedVehicleCharge { false };

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

		/**
		 *  Records the dynamic build counts calculated for each building type
		 *  including base AIBuildCounts and probabilistic AIExtraCounts.
		 */
		std::map<BuildingTypeClass*, int> AICachedBuildCounts;
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

		ExtData(HouseClass* OwnerObject) : Extension<HouseClass>(OwnerObject)
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
			, PrimaryTechTreeType(nullptr)
			, SecondaryTechTreeType(nullptr)
			, LastAttackedBuildingCoords { 0, 0 }
			, LastAttackerCoords { 0, 0 }
			, FrontlineThreatCoords { 0, 0 }
			, FrontlineThreatBuildingCoords { 0, 0 }
			, FrontlineThreatActiveFrames { 0 }
			, FrontlineThreatNeedsDefenses { 0 }
			, ExpansionPlacementFailures { 0 }
			, LastFactoryRecycleFrame { 0 }
			, NextExpansionSearchFrame { 0 }
			, NextBlacklistClearFrame { 0 }
			, LastObsoleteRefineryCheckFrame { 0 }
			, LastServiceDepotPlacementFailedFrame { 0 }
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

		virtual ~ExtData() = default;

		virtual void LoadFromINIFile(CCINIClass* pINI) override;
		//virtual void Initialize() override;
		virtual void InvalidatePointer(void* ptr, bool bRemoved) override;

		void UpdateVehicleProduction();

		virtual void LoadFromStream(PhobosStreamReader& Stm) override;
		virtual void SaveToStream(PhobosStreamWriter& Stm) override;

	private:
		template <typename T>
		void Serialize(T& Stm);
		bool UpdateHarvesterProduction();
	};

	class ExtContainer final : public Container<HouseExt>
	{
	public:
		ExtContainer();
		~ExtContainer();

		virtual bool InvalidateExtDataIgnorable(void* const ptr) const override
		{
			auto const abs = static_cast<AbstractClass*>(ptr)->WhatAmI();

			switch (abs)
			{
			case AbstractType::Building:
				return false;
			}

			return true;
		}
	};

	static ExtContainer ExtMap;

	static bool LoadGlobals(PhobosStreamReader& Stm);
	static bool SaveGlobals(PhobosStreamWriter& Stm);

	static void InitializeBaseDefenses();
	static bool BaseDefensesInitialized;

	static int ActiveHarvesterCount(HouseClass* pThis);
	static int TotalHarvesterCount(HouseClass* pThis);
	static HouseClass* GetHouseKind(OwnerHouseKind kind, bool allowRandom, HouseClass* pDefault, HouseClass* pInvoker = nullptr, HouseClass* pVictim = nullptr);
	static CellClass* GetEnemyBaseGatherCell(HouseClass* pTargetHouse, HouseClass* pCurrentHouse, CoordStruct defaultCurrentCoords, SpeedType speedTypeZone, int extraDistance = 0);
	static void GetAIChronoshiftSupers(HouseClass* pThis, SuperClass*& pSuperCSphere, SuperClass*& pSuperCWarp);

	static void ForceOnlyTargetHouseEnemy(HouseClass* pThis, int mode = -1);
	static void SetSkirmishHouseName(HouseClass* pHouse);

	static bool AdvAI_House_Search_For_Next_Expansion_Point(HouseClass* pHouse);
	static void AdvAI_Add_Failed_Expansion_Point(HouseClass* pHouse, CellStruct coords);
	static bool AdvAI_Is_Failed_Expansion_Point(HouseClass* pHouse, CellStruct coords);
	static bool AdvAI_Can_Build_Building(HouseClass* pHouse, BuildingTypeClass* pBuildingType, bool checkPrereqs, bool isTechTree = false);
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

	static CanBuildResult BuildLimitGroupCheck(const HouseClass* pThis, const TechnoTypeClass* pItem, bool buildLimitOnly, bool includeQueued);
	static bool ReachedBuildLimit(const HouseClass* pHouse, const TechnoTypeClass* pType, bool ignoreQueued);

	static void CalculatePowerSurplus(HouseClass* pThis);

	static std::vector<BuildingTypeClass*> BaseDefenses;
};
