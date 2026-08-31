#pragma once
#include <Ext/Techno/Body.h>
#include <Ext/BuildingType/Body.h>
#include <BuildingClass.h>

class BuildingExt final : public TechnoExt, public Detach::Listener<BuildingClass>
{
public:
	using base_type = BuildingClass;

	// deprecated: the pre-rework nested data class is now the extension class itself
	using ExtData [[deprecated("use the extension class itself instead")]] = BuildingExt;

	static constexpr DWORD Canary = 0x87654321;

	bool DeployedTechno;
	bool IsCreatedFromMapFile;
	int LimboID;
	int GrindingWeapon_LastFiredFrame;
	int GrindingWeapon_AccumulatedCredits;
	BuildingClass* CurrentAirFactory;
	int AccumulatedIncome;
	std::optional<int> CurrentLaserWeaponIndex;
	int PoweredUpToLevel; // Distinct from UpgradeLevel, and set to highest PowersUpToLevel out of applied upgrades regardless of how many are currently applied to this building.
	SuperClass* CurrentEMPulseSW;
	bool IsFiringNow;
	int TurretAnimIdleFrame;
	int TurretAnimFiringFrame;
	int TurretAnimRateTick;
	int ConstructionStartFacing;

	/**
	*  If this building was built by the AI for it to reach an expansion
	*  point, this records the expansion point that the building helped reach.
	*/
	CellStruct AssignedExpansionPoint;

	BuildingExt(BuildingClass* OwnerObject) : TechnoExt(OwnerObject)
		, DeployedTechno { false }
		, IsCreatedFromMapFile { false }
		, LimboID { -1 }
		, GrindingWeapon_LastFiredFrame { 0 }
		, GrindingWeapon_AccumulatedCredits { 0 }
		, CurrentAirFactory { nullptr }
		, AccumulatedIncome { 0 }
		, CurrentLaserWeaponIndex {}
		, PoweredUpToLevel { 0 }
		, CurrentEMPulseSW {}
		, IsFiringNow { false }
		, TurretAnimIdleFrame { 0 }
		, TurretAnimFiringFrame { -1 }
		, TurretAnimRateTick { 0 }
		, ConstructionStartFacing { -1 }
		, AssignedExpansionPoint {}
	{ }

	// typed owner accessor (shadows the TechnoClass one from the base)
	BuildingClass* OwnerObject() const
	{
		return static_cast<BuildingClass*>(this->TechnoExt::OwnerObject());
	}

	// a building's type extension is always the BuildingTypeExt leaf
	BuildingTypeExt* GetTypeExtData() const
	{
		return static_cast<BuildingTypeExt*>(this->TypeExtData);
	}

	void DisplayIncomeString();
	void ApplyPoweredKillSpawns();
	bool HasSuperWeapon(int index) const;
	bool HandleInfiltrate(HouseClass* pInfiltratorHouse, int moneybefore);
	void UpdatePrimaryFactoryAI();

	static BuildingClass* OurBuildings[1000];
	static size_t OurBuildingCount;

	struct CachedAdjacencyAnchor
	{
		const BuildingClass* pBuilding;
		CellStruct Origin;
		int Width;
		int Height;
		bool IsConYard;
		bool IsSimpleBox;
	};
	static CachedAdjacencyAnchor AdjacencyAnchors[1000];
	static size_t AdjacencyAnchorCount;
	static CellStruct AttackCell;
	static CellStruct AttackSourceCell;

	virtual ~BuildingExt() = default;

	// virtual void LoadFromINIFile(CCINIClass* pINI) override;

	virtual void OnDetach(BuildingClass* pTarget, bool removed) override
	{
		if (removed)
			AnnounceInvalidPointer(this->CurrentAirFactory, pTarget);
	}

	virtual void LoadFromStream(PhobosStreamReader& Stm) override;
	virtual void SaveToStream(PhobosStreamWriter& Stm) override;

private:
	template <typename T>
	void Serialize(T& Stm);

public:
	class ExtContainer final : public Container<BuildingExt>
	{
	public:
		ExtContainer();
		~ExtContainer();
	};

	static ExtContainer ExtMap;

	static BuildingExt* Fetch(const BuildingClass* pThis)
	{
		return AbstractExt::Fetch<BuildingExt>(pThis);
	}

	static BuildingExt* TryFetch(const BuildingClass* pThis)
	{
		return AbstractExt::TryFetch<BuildingExt>(pThis);
	}

	static bool LoadGlobals(PhobosStreamReader& Stm);
	static bool SaveGlobals(PhobosStreamWriter& Stm);

	static bool OverlapsAnyBuilding(CellStruct cell, BuildingTypeClass* pBuildingType, BuildingClass* pIgnore = nullptr);
	static bool OverlapsBridge(CellStruct cell, BuildingTypeClass* pBuildingType);

	static void StoreTiberium(BuildingClass* pThis, float amount, int idxTiberiumType, int idxStorageTiberiumType);

	static int CountOccupiedDocks(BuildingClass* pBuilding);
	static bool HasFreeDocks(BuildingClass* pBuilding);
	static bool CanGrindTechno(BuildingClass* pBuilding, TechnoClass* pTechno);
	static bool DoGrindingExtras(BuildingClass* pBuilding, TechnoClass* pTechno, int refund);
	static bool CanUndeployOnSell(BuildingClass* pThis);
	static void KickOutStuckUnits(BuildingClass* pThis);
	static const std::vector<CellStruct> GetFoundationCells(BuildingClass* pThis, CellStruct baseCoords, bool includeOccupyHeight = false);
	static WeaponStruct* GetLaserWeapon(BuildingClass* pThis);
	static void __stdcall UpdateFactoryQueues(BuildingClass* pThis);
	static void __fastcall KickOutClone(std::pair<TechnoTypeClass*, HouseClass*>& info, void*, BuildingClass* pFactory);
	static int GetTurretFrame(BuildingClass* pThis);
	static bool BuildingOnline(BuildingClass* pThis);

	static HouseClass* Find_Closest_Opponent(const HouseClass* pHouse);
	static int Get_Distance_To_Primary_Enemy(CellStruct cell, HouseClass* pHouse);
	static void Mark_Expansion_As_Done(HouseClass* pHouse);
	static void PopulateAdjacencyAnchors(HouseClass* pOwner, BuildingTypeClass* pBuildingType, bool includeAllies = true);
	static int Try_Place(BuildingClass* pBuilding, CellStruct cell);
	static RectangleStruct Get_Base_Rect(HouseClass* pHouse, int adjacency, int width, int height, BuildingTypeClass* pBuildingType = nullptr, bool includeAllies = true);
	static bool Should_Evaluate_Cell_For_Placement(CellStruct cell, BuildingClass* pBuilding, int adjacencyBonus);
	static bool Should_Evaluate_Cell_For_Placement(CellStruct cell, BuildingTypeClass* pBuildingType, HouseClass* pOwner, int adjacencyBonus);
	static bool OverlapsTiberiumTreeZone(CellStruct cell, BuildingTypeClass* pType);
	static int inline Modify_Rating_By_Terrain_Passability(CellStruct cell, BuildingClass* pBuilding, int originalValue);
	static CellStruct Find_Best_Building_Placement_Cell(RectangleStruct baseArea, BuildingClass* pBuilding, int (*valueGenerator)(CellStruct, BuildingClass*), int adjacencyBonus = 0);
	static int inline Modify_Rating_By_Allied_Building_Proximity(CellStruct cell, BuildingClass* pBuilding, int originalValue);
	static int Refinery_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static CellStruct Get_Best_Refinery_Placement_Position(BuildingClass* pBuilding);
	static RectangleStruct GetRefinerySearchRect(HouseClass* pHouse, BuildingTypeClass* pRefineryType);
	static int Near_Base_Center_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding);
	static int Near_Base_Center_Defense_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding);
	static int Near_Enemy_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding);
	static int Near_Refinery_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding);
	static int Near_ConYard_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding);
	static int Far_From_Enemy_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding);
	static CellStruct Get_Best_SuperWeapon_Building_Placement_Position(BuildingClass* pBuilding);
	static int Towards_Expansion_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static CellStruct Get_Best_Expansion_Placement_Position_Helper(HouseClass* pOwner, BuildingTypeClass* pBuildingType, BuildingClass* pBuilding);
	static CellStruct Get_Best_Expansion_Placement_Position(BuildingClass* pBuilding);
	static int Barracks_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static int NavalYard_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static int WarFactory_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static int Helipad_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static CellStruct Get_Best_Factory_Placement_Position(BuildingClass* pBuilding);
	static int Near_AttackCell_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static int Directional_Defense_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static CellStruct Get_Best_Defense_Placement_Position(BuildingClass* pBuilding);

	static bool AdvAI_Is_Support_Placement_Feasible(HouseClass* pHouse, BuildingTypeClass* pBuildingType);
	static CellStruct Get_Best_Support_Placement_Position(BuildingClass* pBuilding);
	static CellStruct Get_Best_Sensor_Placement_Position(BuildingClass* pBuilding);
	static int Near_WarFactory_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding);
	static int ServiceDepot_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding);
	static CellStruct Get_Best_ServiceDepot_Placement_Position(BuildingClass* pBuilding);
	static CellStruct Get_Best_Silo_Placement_Position(BuildingClass* pBuilding);
	static CellStruct Get_Best_Placement_Position(BuildingClass* pBuilding);
	static int Exit_Object_Custom_Position(BuildingClass* pBuilding);
};
