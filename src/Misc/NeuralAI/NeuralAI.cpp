#include "NeuralAI.h"

#include <AircraftClass.h>
#include <AircraftTypeClass.h>
#include <BeaconManagerClass.h>
#include <BuildingClass.h>
#include <BuildingTypeClass.h>
#include <CCINIClass.h>
#include <CellClass.h>
#include <ColorScheme.h>
#include <FactoryClass.h>
#include <FootClass.h>
#include <GameModeOptionsClass.h>
#include <HouseClass.h>
#include <InfantryClass.h>
#include <InfantryTypeClass.h>
#include <MapClass.h>
#include <MessageListClass.h>
#include <OverlayClass.h>
#include <OverlayTypeClass.h>
#include <RulesClass.h>
#include <ScenarioClass.h>
#include <SessionClass.h>
#include <SuperClass.h>
#include <TechnoClass.h>
#include <TemporalClass.h>
#include <TerrainClass.h>
#include <TerrainTypeClass.h>
#include <UnitClass.h>
#include <UnitTypeClass.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/House/Body.h>
#include <Ext/Rules/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/Techno/Body.h>
#include <Ext/TechnoType/Body.h>
#include <Ext/Unit/Body.h>
#include <Ext/TerrainType/Body.h>
#include <Ext/WarheadType/Body.h>
#include <Utilities/Debug.h>
#include <Utilities/GeneralUtils.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

bool NeuralAI::HeadlessMode = false;
int NeuralAI::TrainSlotIndex = 0;
int NeuralAI::MatchEndWinnerHouse = -1;
bool NeuralAI::MatchEndStalemate = false;

HANDLE NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;

HANDLE NeuralAI::BaseMappingHandle = NULL;

const uint8_t* NeuralAI::BaseMappedView = nullptr;

size_t NeuralAI::BaseMappedSize = 0;

HANDLE NeuralAI::TelemetryShmHandle = NULL;

AITrainerSharedTelemetry* NeuralAI::TelemetryView = nullptr;

namespace
{
	constexpr uint32_t TelemetryJsonShmCapacity = 65536;

	HANDLE TelemetryJsonShmHandle = NULL;

	uint8_t* TelemetryJsonView = nullptr;
} // namespace

AIBackpackFile NeuralAI::Backpack = {};

bool NeuralAI::BackpackLoaded = false;

AIBackpackFile NeuralAI::OpponentBackpack = {};

bool NeuralAI::OpponentLoaded = false;
bool NeuralAI::AllowBackpackInference = false;

uint64_t NeuralAI::CurrentRulesSignature = 0;

uint64_t NeuralAI::CurrentBaseSignature = 0;

int NeuralAI::LastCombatFrame = 0;
int NeuralAI::LastTotalKillsAndCaptures = 0;
DWORD NeuralAI::LastFPSCheckTick = 0;
int NeuralAI::FramesSinceLastFPSCheck = 0;
int NeuralAI::CurrentMeasuredFPS = 0;

namespace
{
	constexpr uint64_t FnvOffsetBasis = 14695981039346656037ULL;

	constexpr uint64_t FnvPrime = 1099511628211ULL;

	inline uint64_t HashBytesFNV1a(const void* data, size_t len, uint64_t hash = FnvOffsetBasis)
	{
		const uint8_t* pBytes = static_cast<const uint8_t*>(data);

		for (size_t i = 0; i < len; ++i)
		{
			hash ^= static_cast<uint64_t>(pBytes[i]);

			hash *= FnvPrime;
		}

		return hash;
	}

	template <typename T> inline uint64_t HashValueFNV1a(const T& val, uint64_t hash)
	{
		return HashBytesFNV1a(&val, sizeof(T), hash);
	}

	inline int8_t ClampInt8(int val)
	{
		if (val < -127)
			return -127;

		if (val > 127)
			return 127;

		return static_cast<int8_t>(val);
	}

	inline int16_t ClampInt16(int val)
	{
		if (val < -32000)
			return -32000;

		if (val > 32000)
			return 32000;

		return static_cast<int16_t>(val);
	}

	// Per-house vision map built from the sight of the AI and its allies. Replaces
	// CellClass::IsShrouded, which reflects the local player's shroud and would
	// desync multiplayer and leak the human's exploration to the AI.
	constexpr int VisionMapSide = 512;

	std::vector<uint8_t> VisionMap;

	int VisionMapFrame = -1;
	int VisionMapOwner = -1;

	void RebuildVisionMap(HouseClass* pHouse)
	{
		VisionMap.assign(static_cast<size_t>(VisionMapSide) * VisionMapSide, 0);

		VisionMapFrame = Unsorted::CurrentFrame;

		VisionMapOwner = pHouse->ArrayIndex;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
				continue;

			if (pTechno->Owner != pHouse && !pHouse->IsAlliedWith(pTechno->Owner))
				continue;

			auto const pType = pTechno->GetTechnoType();

			if (!pType)
				continue;

			const CellStruct coords = pTechno->GetMapCoords();
			const int radius = std::clamp(static_cast<int>(pType->Sight), 1, 24);

			for (int dy = -radius; dy <= radius; ++dy)
			{
				const int y = coords.Y + dy;

				if (y < 0 || y >= VisionMapSide)
					continue;

				for (int dx = -radius; dx <= radius; ++dx)
				{
					const int x = coords.X + dx;

					if (x < 0 || x >= VisionMapSide || dx * dx + dy * dy > radius * radius)
						continue;

					VisionMap[static_cast<size_t>(y) * VisionMapSide + x] = 1;
				}
			}
		}
	}

	bool IsCellVisibleToAI(HouseClass* pHouse, CellClass* pCell)
	{
		if (!pHouse || !pCell)
			return false;

		if (VisionMapFrame != Unsorted::CurrentFrame || VisionMapOwner != pHouse->ArrayIndex)
			RebuildVisionMap(pHouse);

		const CellStruct coords = pCell->MapCoords;

		if (coords.X < 0 || coords.X >= VisionMapSide || coords.Y < 0 || coords.Y >= VisionMapSide)
			return false;

		return VisionMap[static_cast<size_t>(coords.Y) * VisionMapSide + coords.X] != 0;
	}

	bool IsTunnelNetworkBuilding(const BuildingTypeClass* pBldType)
	{
		if (!pBldType)
			return false;

		static std::vector<int8_t> TunnelTypeCache;

		static int LastCachedCount = -1;

		if (BuildingTypeClass::Array.Count != LastCachedCount)
		{
			TunnelTypeCache.assign(static_cast<size_t>(std::max(0, BuildingTypeClass::Array.Count)) + 16, -1);

			LastCachedCount = BuildingTypeClass::Array.Count;
		}

		const int idx = pBldType->ArrayIndex;

		if (idx >= 0)
		{
			if (static_cast<size_t>(idx) >= TunnelTypeCache.size())
				TunnelTypeCache.resize(static_cast<size_t>(idx) + 32, -1);

			if (TunnelTypeCache[idx] != -1)
				return TunnelTypeCache[idx] != 0;
		}

		bool isTunnel = false;

		if (CCINIClass::INI_Rules && pBldType->ID && pBldType->ID[0] != '\0')
		{
			char tunnelBuf[64] = {};

			if (CCINIClass::INI_Rules->ReadString(pBldType->ID, "Tunnel", "", tunnelBuf, sizeof(tunnelBuf)) > 0
				&& !INIClass::IsBlank(tunnelBuf))
			{
				isTunnel = true;
			}
		}

		if (!isTunnel)
		{
			if (pBldType->Passengers > 0 && (pBldType->InfantryAbsorb || pBldType->UnitAbsorb)
				&& pBldType->PowerBonus == 0 && !pBldType->Bunker && !pBldType->CanBeOccupied && !pBldType->Grinding
				&& !pBldType->Hospital && !pBldType->Armory)
			{
				isTunnel = true;
			}
			else if (pBldType->ID && std::strstr(pBldType->ID, "TUNNEL") != nullptr && pBldType->Passengers > 0)
			{
				isTunnel = true;
			}
		}

		if (idx >= 0)
			TunnelTypeCache[idx] = isTunnel ? 1 : 0;

		return isTunnel;
	}

	bool HasNoManualUnload(const TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		if (auto const pTypeExt = TechnoTypeExt::TryFetch(const_cast<TechnoTypeClass*>(pType)))
		{
			if (pTypeExt->NoManualUnload.Get(false))
				return true;
		}

		static std::map<const TechnoTypeClass*, int8_t> NoManualUnloadCache;
		auto const it = NoManualUnloadCache.find(pType);
		if (it != NoManualUnloadCache.end())
			return it->second != 0;

		bool val = false;
		if (CCINIClass::INI_Rules && pType->ID && pType->ID[0] != '\0')
			val = CCINIClass::INI_Rules->ReadBool(pType->ID, "NoManualUnload", false);

		NoManualUnloadCache[pType] = val ? 1 : 0;
		return val;
	}

	bool HasNoManualEnter(const TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		static std::map<const TechnoTypeClass*, int8_t> NoManualEnterCache;
		auto const it = NoManualEnterCache.find(pType);
		if (it != NoManualEnterCache.end())
			return it->second != 0;

		bool val = false;
		if (CCINIClass::INI_Rules && pType->ID && pType->ID[0] != '\0')
			val = CCINIClass::INI_Rules->ReadBool(pType->ID, "NoManualEnter", false);

		if (!val && HasNoManualUnload(pType) && static_cast<int>(pType->SizeLimit) >= 50)
			val = true;

		NoManualEnterCache[pType] = val ? 1 : 0;
		return val;
	}

	bool IsImmobileUnitTypeOnLand(const TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		if (auto const pTypeExt = TechnoTypeExt::TryFetch(const_cast<TechnoTypeClass*>(pType)))
		{
			if (pTypeExt->NoManualMove.Get())
				return true;
		}

		return (pType->Speed <= 0) || (!pType->Naval && pType->SpeedType == SpeedType::Float);
	}

	TechnoTypeClass* GetConvertDeployTargetType(const TechnoTypeClass* pType)
	{
		if (!pType)
			return nullptr;

		if (auto const pTypeExt = TechnoTypeExt::TryFetch(const_cast<TechnoTypeClass*>(pType)))
		{
			if (auto const pCD = pTypeExt->Convert_Deploy.Get())
				return pCD;

			if (auto const pCUD = pTypeExt->Convert_UniversalDeploy.Get())
				return pCUD;
		}

		return nullptr;
	}

	// Matches Phobos CanEnterNow (Hooks.Transport.cpp:255-280):
	// Enforces individual unit Size <= Transport->SizeLimit AND total capacity
	// (respecting Passengers.BySize).
	bool CanPassengerFitInTransport(const TechnoClass* pTransport, const TechnoTypeClass* pPassengerType,
		int pendingCount = 0, int pendingSize = 0, int reservedSize = 0)
	{
		if (!pTransport || !pPassengerType)
			return false;

		const auto pTransportType = pTransport->GetTechnoType();
		if (!pTransportType || pTransportType->Passengers <= 0)
			return false;

		const int passengerSize = std::max(1, static_cast<int>(pPassengerType->Size));
		if (passengerSize > static_cast<int>(pTransportType->SizeLimit))
			return false;

		const auto pTransportTypeExt = TechnoTypeExt::Fetch(pTransportType);
		const bool bySize = pTransportTypeExt ? pTransportTypeExt->Passengers_BySize.Get() : true;
		const int maxAllowed = std::max(0, pTransportType->Passengers - reservedSize);
		if ((pTransport->Passengers.NumPassengers + pendingCount + 1) > pTransportType->Passengers)
			return false;

		if (bySize)
			return (pTransport->Passengers.GetTotalSize() + pendingSize + passengerSize) <= maxAllowed;

		return (pTransport->Passengers.NumPassengers + pendingCount + 1) <= maxAllowed;
	}

	bool IsTunnelDeliverySuperWeapon(const SuperWeaponTypeClass* pSWType, HouseClass* pOwner)
	{
		if (!pSWType)
			return false;

		if (pSWType->ID
			&& (std::strstr(pSWType->ID, "Tunnel") != nullptr || std::strstr(pSWType->ID, "TUNNEL") != nullptr))
		{
			return true;
		}

		if (pOwner)
		{
			for (auto const pBld : pOwner->Buildings)
			{
				if (pBld && !pBld->InLimbo && pBld->IsAlive && pBld->Type && IsTunnelNetworkBuilding(pBld->Type))
				{
					if (pBld->Type->SuperWeapon == pSWType->ArrayIndex
						|| pBld->Type->SuperWeapon2 == pSWType->ArrayIndex)
					{
						return true;
					}
				}
			}
		}

		return false;
	}

	// Protects Ore/Tiberium Spawner Trees (SpawnsTiberium) from being crushed or
	// blocked by AI building placement
	bool OverlapsTiberiumTreeZone(CellStruct cell, BuildingTypeClass* pType)
	{
		if (!pType || pType->ResourceDestination)
			return false;

		struct CachedTiberiumTree
		{
			short X;
			short Y;
			short Range;
		};

		static int s_CachedTreesFrame = -1;

		static std::vector<CachedTiberiumTree> s_CachedTrees;

		if (s_CachedTreesFrame != Unsorted::CurrentFrame)
		{
			s_CachedTreesFrame = Unsorted::CurrentFrame;

			s_CachedTrees.clear();

			for (auto const pTerrain : TerrainClass::Array)
			{
				if (pTerrain && pTerrain->IsAlive && !pTerrain->InLimbo && pTerrain->Type
					&& pTerrain->Type->SpawnsTiberium)
				{
					auto const pTerrainTypeExt = TerrainTypeExt::Fetch(pTerrain->Type);

					const short range = static_cast<short>(
						pTerrainTypeExt ? std::max(1, pTerrainTypeExt->SpawnsTiberium_Range.Get()) : 1);

					const CellStruct treeCell = pTerrain->GetMapCoords();

					s_CachedTrees.push_back({ treeCell.X, treeCell.Y, range });
				}
			}
		}

		if (s_CachedTrees.empty())
			return false;

		const int foundationW = pType->GetFoundationWidth();
		const int foundationH = pType->GetFoundationHeight(false);
		const int minX = cell.X;
		const int maxX = cell.X + foundationW - 1;
		const int minY = cell.Y;
		const int maxY = cell.Y + foundationH - 1;

		for (const auto& tree : s_CachedTrees)
		{
			if (minX <= tree.X + tree.Range && maxX >= tree.X - tree.Range && minY <= tree.Y + tree.Range
				&& maxY >= tree.Y - tree.Range)
			{
				return true;
			}
		}

		return false;
	}

	bool OverlapsBridge(CellStruct cell, BuildingTypeClass* pType)
	{
		if (!pType)
			return false;

		const int foundationW = pType->GetFoundationWidth();
		const int foundationH = pType->GetFoundationHeight(false);

		for (int dy = 0; dy < foundationH; ++dy)
		{
			for (int dx = 0; dx < foundationW; ++dx)
			{
				const CellStruct checkCell { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };

				if (!MapClass::Instance.CoordinatesLegal(checkCell))
					continue;

				if (auto const pCell = MapClass::Instance.TryGetCellAt(checkCell))
				{
					if (pCell->ContainsBridge())
						return true;
				}
			}
		}

		return false;
	}

	int GetSupportRadius(const BuildingTypeClass* pType)
	{
		if (!pType)
			return 0;

		int minRadius = 999;

		if (pType->CloakGenerator && pType->CloakRadiusInCells > 0 && pType->CloakRadiusInCells < minRadius)
			minRadius = pType->CloakRadiusInCells;

		auto const pBldExt = BuildingTypeExt::Fetch(pType);
		const bool isGapGen = (pBldExt && pBldExt->GapGenerator) || pType->GapGenerator;

		const int gapRadius
			= (pBldExt && pBldExt->GapRadiusInCells != 0) ? pBldExt->GapRadiusInCells.Get() : pType->GapRadiusInCells;

		const int superGapRadius = (pBldExt && pBldExt->SuperGapRadiusInCells != 0)
			? pBldExt->SuperGapRadiusInCells.Get()
			: pType->SuperGapRadiusInCells;

		if (isGapGen)
		{
			if (gapRadius > 0 && gapRadius < minRadius)
				minRadius = gapRadius;

			if (superGapRadius > 0 && superGapRadius < minRadius)
				minRadius = superGapRadius;
		}

		if (pType->SensorArray && pType->SensorsSight > 0 && pType->SensorsSight < minRadius)
			minRadius = pType->SensorsSight;

		if (auto const pTechExt = TechnoTypeExt::Fetch(pType))
		{
			if (pTechExt->InhibitorRange.isset() && pTechExt->InhibitorRange.Get() > 0
				&& pTechExt->InhibitorRange.Get() < minRadius)
			{
				minRadius = pTechExt->InhibitorRange.Get();
			}

			if (pTechExt->RadarJamRadius.Get() > 0 && pTechExt->RadarJamRadius.Get() < minRadius)
				minRadius = pTechExt->RadarJamRadius.Get();
		}

		return minRadius == 999 ? 0 : minRadius;
	}

	// Calculates real power drain (including toggled-off structures) and power grid
	// vulnerability metrics
	int GetRealPowerDrainAndGridMetrics(HouseClass* pHouse, bool& outHasUnpoweredBuildings,
		int& outPoweredDefAndSWCount, int& outMaxSinglePlantBonus, bool& outHasAdvancedPowerPlants)
	{
		outHasUnpoweredBuildings = false;

		outPoweredDefAndSWCount = 0;

		outMaxSinglePlantBonus = 0;

		outHasAdvancedPowerPlants = false;

		if (!pHouse)
			return 0;

		int drain = pHouse->PowerDrain;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			auto const pOT = pBld->Type;

			if (pOT->TogglePower && !pBld->StuffEnabled)
			{
				drain += pOT->PowerDrain;

				outHasUnpoweredBuildings = true;
			}

			if (pOT->PowerBonus > pOT->PowerDrain && !pOT->IsBaseDefense)
			{
				if (pOT->PowerBonus > outMaxSinglePlantBonus)
					outMaxSinglePlantBonus = pOT->PowerBonus;

				if (pOT->PowerBonus >= 300 || pOT->Cost >= 1000)
					outHasAdvancedPowerPlants = true;
			}

			if ((pOT->IsBaseDefense || pOT->SuperWeapon != -1) && (pOT->Powered || pOT->PowerDrain > 0))
				outPoweredDefAndSWCount++;
		}

		return drain;
	}

	// Checks if a distant static refinery (> 20 cells from ConYard) has exhausted
	// all ore within 27 cells and trees within 22 cells
	bool IsRefineryExhausted(BuildingClass* pBld, CellStruct baseCenter)
	{
		if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
			return false;

		if ((!pBld->Type->Refinery && !pBld->Type->ResourceDestination) || pBld->Type->ResourceGatherer
			|| pBld->Type->Enslaves != nullptr || pBld->Type->UndeploysInto != nullptr)
		{
			return false;
		}

		if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling
			|| pBld->CurrentMission == Mission::Construction || pBld->QueuedMission == Mission::Construction)
		{
			return false;
		}

		const CellStruct refCoords = pBld->GetMapCoords();

		if (baseCenter != CellStruct::Empty && refCoords.DistanceFrom(baseCenter) < 20.0)
			return false;

		for (int dy = -27; dy <= 27; ++dy)
		{
			for (int dx = -27; dx <= 27; ++dx)
			{
				if (dx * dx + dy * dy > 729)
					continue;

				const CellStruct scanCell { static_cast<short>(refCoords.X + dx),
					static_cast<short>(refCoords.Y + dy) };

				if (!MapClass::Instance.CoordinatesLegal(scanCell))
					continue;

				if (auto const pCell = MapClass::Instance.TryGetCellAt(scanCell))
				{
					if (pCell->GetContainedTiberiumValue() > 0)
						return false;

					if (dx * dx + dy * dy <= 484)
					{
						if (auto const pTerrain = pCell->GetTerrain(false))
						{
							if (pTerrain->IsAlive && pTerrain->Type && pTerrain->Type->SpawnsTiberium)
								return false;
						}
					}
				}
			}
		}

		return true;
	}

	void RecycleObsoleteRefineries(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->ConYards.Count <= 0)
			return;

		int staticRefineryCount = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type
				&& (pBld->Type->Refinery || pBld->Type->ResourceDestination) && !pBld->Type->ResourceGatherer)
			{
				staticRefineryCount++;
			}
		}

		// Never sell our last 2 refineries proactively
		if (staticRefineryCount <= 2)
			return;

		const CellStruct baseCenter = pHouse->ConYards.GetItem(0)->GetMapCoords();

		for (auto const pBld : pHouse->Buildings)
		{
			if (IsRefineryExhausted(pBld, baseCenter) && !pBld->HasAnyLink())
			{
				Debug::Log("[NeuralAI] House %d recycling exhausted distant refinery %s "
						"at (%d,%d).\n",
					pHouse->ArrayIndex, pBld->Type->ID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y);

				pBld->Sell(1);

				break;
			}
		}
	}

	// Emergency cash recovery (AdvAI_Raise_Money): ONLY when the AI has 0
	// refineries, 0 harvesters, and < 1000 credits
	void RaiseEmergencyMoneyForRefinery(HouseClass* pHouse, int ownedRefineries, int ownedEconomyUnits, int money)
	{
		if (!pHouse || pHouse->ConYards.Count <= 0 || ownedRefineries > 0 || ownedEconomyUnits > 0 || money >= 1000)
			return;

		BuildingClass* pBestToSell = nullptr;
		int bestPriority = 999;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->ToProtect
				|| pBld->Type->ConstructionYard)
			{
				continue;
			}

			if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling
				|| pBld->CurrentMission == Mission::Construction)
			{
				continue;
			}

			auto const pOT = pBld->Type;
			int prio = 999;

			if (pOT->Storage > 0 && !pOT->Refinery)
				prio = 1; // Silo in total bankruptcy
			else if (pOT->UnitRepair)
				prio = 2; // Service Depot
			else if (pOT->SuperWeapon != -1 && !pOT->IsBaseDefense)
				prio = 3; // Superweapon
			else if (pOT->SpySat || pOT->Radar)
				prio = 4; // Radar
			else if (pOT->IsBaseDefense)
				prio = 5; // Defense

			if (prio < bestPriority)
			{
				bestPriority = prio;

				pBestToSell = pBld;
			}
		}

		if (pBestToSell)
		{
			Debug::Log("[NeuralAI] House %d in total economic emergency (0 Refineries, "
					"0 Harvesters, %d credits): selling %s to fund recovery!\n",
				pHouse->ArrayIndex, money, pBestToSell->Type->ID);

			pBestToSell->Sell(1);
		}
	}

	enum class NeuralThreatCategory : int
	{
		None = 0,
		Infantry = 1,
		Vehicle = 2,
		Air = 3
	};

	struct NeuralAttackedBuildingRecord
	{
		CellStruct BuildingCoords { CellStruct::Empty };
		CellStruct AttackerCoords { CellStruct::Empty };

		TechnoTypeClass* AttackerType { nullptr };

		NeuralThreatCategory Threat { NeuralThreatCategory::None };

		int AttackFrame { 0 };
	};

	struct NeuralUnsafeZone
	{
		CellStruct Coords { CellStruct::Empty };
		int ExpirationFrame { 0 };
	};

	struct NeuralHouseMacroState
	{
		CellStruct PendingClearanceCell { CellStruct::Empty };

		BuildingTypeClass* PendingClearanceType { nullptr };

		int PendingClearanceFoundationWidth { 0 };
		int PendingClearanceFoundationHeight { 0 };

		Foundation PendingClearanceFoundation { Foundation::_1x1 };

		int ClearanceAttempts { 0 };
		int NextClearanceCheckFrame { 0 };
		int LastClearanceFailedFrame { -1000 };
		int ConsecutivePlacementFailures { 0 };
		bool IsBuildingRecoveryRefinery { false };
		int NextHarvesterRecoveryFrame { 0 };
		int TargetRecoveryRefineryZone { -1 };
		CellStruct FrontlineThreatCoords { CellStruct::Empty };
		CellStruct FrontlineThreatBuildingCoords { CellStruct::Empty };
		int FrontlineThreatActiveFrames { 0 };
		int FrontlineThreatNeedsDefenses { 0 };
		std::map<unsigned int, int> TemporaryRecoveryRefineryPlacedFrame {};
		std::map<int, int> BuildingPlacementCooldownUntilFrame {};

		std::vector<NeuralAttackedBuildingRecord> AttackedBuildingsLIFO {};
		std::vector<NeuralUnsafeZone> UnsafePlacementZones {};

		void ClearClearanceReservation()
		{
			this->PendingClearanceCell = CellStruct::Empty;

			this->PendingClearanceType = nullptr;

			this->PendingClearanceFoundationWidth = 0;

			this->PendingClearanceFoundationHeight = 0;

			this->PendingClearanceFoundation = Foundation::_1x1;

			this->ClearanceAttempts = 0;

			this->NextClearanceCheckFrame = 0;
		}

		void Reset()
		{
			this->ClearClearanceReservation();

			this->LastClearanceFailedFrame = -1000;
			this->ConsecutivePlacementFailures = 0;
			this->IsBuildingRecoveryRefinery = false;
			this->NextHarvesterRecoveryFrame = 0;
			this->TargetRecoveryRefineryZone = -1;
			this->FrontlineThreatCoords = CellStruct::Empty;
			this->FrontlineThreatBuildingCoords = CellStruct::Empty;
			this->FrontlineThreatActiveFrames = 0;
			this->FrontlineThreatNeedsDefenses = 0;
			this->TemporaryRecoveryRefineryPlacedFrame.clear();
			this->BuildingPlacementCooldownUntilFrame.clear();
			this->AttackedBuildingsLIFO.clear();
			this->UnsafePlacementZones.clear();
		}
	};

	std::array<NeuralHouseMacroState, 32> MacroStates {};

	NeuralHouseMacroState& GetMacroState(const HouseClass* pHouse)
	{
		const int idx = pHouse ? std::clamp(pHouse->ArrayIndex, 0, 31) : 0;

		return MacroStates[idx];
	}

	bool AreZonesConnected(CellStruct fromCell, CellStruct toCell, MovementZone movementZone)
	{
		if (!MapClass::Instance.CoordinatesLegal(fromCell) || !MapClass::Instance.CoordinatesLegal(toCell))
			return false;

		const int z1 = MapClass::Instance.GetMovementZoneType(fromCell, movementZone, false);
		const int z2 = MapClass::Instance.GetMovementZoneType(toCell, movementZone, false);

		return z1 != -1 && z1 == z2;
	}

	void GetDefenseCapabilities(BuildingTypeClass* pBld, bool& outAntiInf, bool& outAntiArmor, bool& outAntiAir,
		bool& outPureAA, bool& outUnarmedBunker)
	{
		outAntiInf = false;
		outAntiArmor = false;
		outAntiAir = false;
		outPureAA = false;
		outUnarmedBunker = false;

		if (!pBld)
			return;

		const auto pWS0 = pBld->GetWeapon(0);
		const auto pW0 = pWS0 ? pWS0->WeaponType : nullptr;
		const auto pWS1 = pBld->GetWeapon(1);
		const auto pW1 = pWS1 ? pWS1->WeaponType : nullptr;

		outUnarmedBunker = (pBld->CanBeOccupied || pBld->Bunker) && (pW0 == nullptr || pW0->Damage <= 0)
			&& (pW1 == nullptr || pW1->Damage <= 0);

		const bool w0AA = pW0 && pW0->Projectile && pW0->Projectile->AA;
		const bool w0AG = pW0 && (!pW0->Projectile || pW0->Projectile->AG);
		const bool w1AA = pW1 && pW1->Projectile && pW1->Projectile->AA;
		const bool w1AG = pW1 && (!pW1->Projectile || pW1->Projectile->AG);

		outAntiAir = (pBld->AntiAirValue > 0) || w0AA || w1AA;
		outPureAA = (w0AA || w1AA) && !w0AG && !w1AG && (pBld->AntiInfantryValue <= 0 && pBld->AntiArmorValue <= 0);

		if (pBld->AntiInfantryValue > 0 || pBld->AntiArmorValue > 0)
		{
			outAntiInf = (pBld->AntiInfantryValue > 0);
			outAntiArmor = (pBld->AntiArmorValue > 0);
		}
		else if (!outPureAA && !outUnarmedBunker && (w0AG || w1AG))
		{
			const auto pGndW = w0AG ? pW0 : pW1;
			const bool highTierOrHeavy = (pGndW && (pGndW->Damage >= 65 || pGndW->Range >= 6 * 256))
				|| pBld->Cost >= 800 || pBld->TechLevel >= 4;

			outAntiArmor = highTierOrHeavy;
			outAntiInf = !highTierOrHeavy || (pGndW && pGndW->Damage >= 65);
		}
	}

	void RecordUnsafePlacementZone(HouseClass* pHouse, CellStruct coords, int durationFrames = 2700)
	{
		if (!pHouse || coords == CellStruct::Empty)
			return;

		auto& state = GetMacroState(pHouse);
		const int expFrame = Unsorted::CurrentFrame + durationFrames;

		for (auto& z : state.UnsafePlacementZones)
		{
			if (z.Coords.DistanceFromSquared(coords) <= 36.0)
			{
				z.Coords = coords;
				z.ExpirationFrame = std::max(z.ExpirationFrame, expFrame);
				return;
			}
		}

		if (state.UnsafePlacementZones.size() >= 16)
			state.UnsafePlacementZones.erase(state.UnsafePlacementZones.begin());

		state.UnsafePlacementZones.push_back({ coords, expFrame });
	}

	void PruneAttackedBuildingsLIFO(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& state = GetMacroState(pHouse);

		if (!state.UnsafePlacementZones.empty())
		{
			state.UnsafePlacementZones.erase(
				std::remove_if(state.UnsafePlacementZones.begin(), state.UnsafePlacementZones.end(),
					[&](const NeuralUnsafeZone& z)
					{
						if (z.Coords == CellStruct::Empty || Unsorted::CurrentFrame >= z.ExpirationFrame)
							return true;

						// Once a ground defense turret secures this zone (within 7 cells), lift the unsafe penalty!
						for (auto const pDef : pHouse->Buildings)
						{
							if (!pDef || !pDef->IsAlive || pDef->InLimbo || !pDef->Type || !pDef->Type->IsBaseDefense
								|| IsTunnelNetworkBuilding(pDef->Type))
							{
								continue;
							}

							if (pDef->GetMapCoords().DistanceFromSquared(z.Coords) <= 49.0)
								return true;
						}

						return false;
					}),
				state.UnsafePlacementZones.end());
		}

		if (state.FrontlineThreatNeedsDefenses > 0 && Unsorted::CurrentFrame > state.FrontlineThreatActiveFrames)
		{
			state.FrontlineThreatNeedsDefenses = 0;
			state.FrontlineThreatCoords = CellStruct::Empty;
			state.FrontlineThreatBuildingCoords = CellStruct::Empty;
		}

		if (state.AttackedBuildingsLIFO.empty())
			return;

		state.AttackedBuildingsLIFO.erase(
			std::remove_if(state.AttackedBuildingsLIFO.begin(), state.AttackedBuildingsLIFO.end(),
				[&](const NeuralAttackedBuildingRecord& rec)
				{
					if (Unsorted::CurrentFrame - rec.AttackFrame > 1800)
						return true;

					auto const pCell = MapClass::Instance.TryGetCellAt(rec.BuildingCoords);

					if (!pCell)
						return true;

					auto const pBld = pCell->GetBuilding();

					if (!pBld || !pBld->IsAlive || pBld->InLimbo
						|| (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner)))
					{
						return true;
					}

					// AdvAI LIFO pruning: remove the record once a base defense with the
					// matching counter capability exists within 8.0 cells of the building
					for (auto const pDef : pHouse->Buildings)
					{
						if (!pDef || !pDef->IsAlive || pDef->InLimbo || !pDef->Type || !pDef->Type->IsBaseDefense
							|| IsTunnelNetworkBuilding(pDef->Type))
						{
							continue;
						}

						if (pDef->GetMapCoords().DistanceFromSquared(rec.BuildingCoords) > 64.0)
							continue;

						bool antiInf = false;
						bool antiArmor = false;
						bool antiAir = false;
						bool pureAA = false;
						bool unarmedBunker = false;
						GetDefenseCapabilities(pDef->Type, antiInf, antiArmor, antiAir, pureAA, unarmedBunker);

						if (unarmedBunker && pDef->Occupants.Count == 0 && pDef->BunkerLinkedItem == nullptr)
							continue;

						if (rec.Threat == NeuralThreatCategory::Air && antiAir)
							return true;

						if (rec.Threat == NeuralThreatCategory::Vehicle && (antiArmor || (!pureAA && unarmedBunker)))
							return true;

						if (rec.Threat == NeuralThreatCategory::Infantry && (antiInf || (!pureAA && unarmedBunker)))
							return true;

						if (rec.Threat == NeuralThreatCategory::None && !pureAA)
							return true;
					}

					return false;
				}),
			state.AttackedBuildingsLIFO.end());
	}

	NeuralThreatCategory GetActiveThreatCategory(HouseClass* pHouse, CellStruct threatCenter)
	{
		if (!pHouse || threatCenter == CellStruct::Empty)
			return NeuralThreatCategory::None;

		int infantryCount = 0;
		int vehicleCount = 0;
		int airCount = 0;
		constexpr double radiusSq = 225.0; // 15 cells

		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
				continue;

			HouseClass* const pOwner = pFoot->Owner;

			if (!pOwner || pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral()
				|| pOwner->IsObserver())
			{
				continue;
			}

			if (pFoot->GetMapCoords().DistanceFromSquared(threatCenter) > radiusSq)
				continue;

			auto const pTType = pFoot->GetTechnoType();

			if (pFoot->IsInAir() || pFoot->WhatAmI() == AbstractType::Aircraft || (pTType && pTType->JumpJet))
				airCount++;
			else if (pFoot->WhatAmI() == AbstractType::Infantry)
				infantryCount++;
			else if (pFoot->WhatAmI() == AbstractType::Unit)
				vehicleCount++;
		}

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame)
			{
				continue;
			}

			HouseClass* const pOwner = pBld->Owner;

			if (!pOwner || pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral()
				|| pOwner->IsObserver())
			{
				continue;
			}

			if (pBld->GetMapCoords().DistanceFromSquared(threatCenter) <= radiusSq)
				vehicleCount++;
		}

		if (infantryCount == 0 && vehicleCount == 0 && airCount == 0)
			return NeuralThreatCategory::None;

		if (airCount >= vehicleCount && airCount >= infantryCount)
			return NeuralThreatCategory::Air;

		if (vehicleCount >= infantryCount)
			return NeuralThreatCategory::Vehicle;

		return NeuralThreatCategory::Infantry;
	}

	int CalculateEnemyAircraftValue(HouseClass* pHouse)
	{
		if (!pHouse)
			return 0;

		int totalValue = 0;

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || !pAir->IsAlive || pAir->InLimbo || !pAir->Owner)
				continue;

			if (pAir->Owner == pHouse || pHouse->IsAlliedWith(pAir->Owner) || pAir->Owner->IsNeutral()
				|| pAir->Owner->IsObserver())
			{
				continue;
			}

			totalValue += 10;
		}

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Owner || !pUnit->Type)
				continue;

			if (pUnit->Owner == pHouse || pHouse->IsAlliedWith(pUnit->Owner) || pUnit->Owner->IsNeutral()
				|| pUnit->Owner->IsObserver())
			{
				continue;
			}

			const auto pWS = pUnit->Type->GetWeapon(0);
			const bool isSpawner
				= (pWS && pWS->WeaponType && pWS->WeaponType->Spawner) || pUnit->Type->Spawns != nullptr;

			if (pUnit->Type->ConsideredAircraft || pUnit->Type->JumpJet
				|| pUnit->Type->MovementZone == MovementZone::Fly || isSpawner)
			{
				totalValue += 5;
			}
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || !pInf->IsAlive || pInf->InLimbo || !pInf->Owner || !pInf->Type)
				continue;

			if (pInf->Owner == pHouse || pHouse->IsAlliedWith(pInf->Owner) || pInf->Owner->IsNeutral()
				|| pInf->Owner->IsObserver())
			{
				continue;
			}

			if (pInf->Type->ConsideredAircraft || pInf->Type->JumpJet || pInf->Type->MovementZone == MovementZone::Fly)
				totalValue += 4;
		}

		return totalValue;
	}

	// Updates PrimaryFactory for Infantry, Vehicles, and Naval units to the factory
	// closest & zone-connected to the frontline target
	void UpdatePrimaryFactories(HouseClass* pHouse, CellStruct targetCoords)
	{
		if (!pHouse || targetCoords == CellStruct::Empty)
			return;

		auto updateCategory = [&](AbstractType type, bool isNaval, CellStruct categoryGoal)
		{
			const CellStruct goalCoords = (categoryGoal != CellStruct::Empty) ? categoryGoal : targetCoords;
			if (goalCoords == CellStruct::Empty)
				return;

			BuildingClass* pBestFactory = nullptr;
			double bestDistanceSq = 1e18;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
					continue;

				if (pBld->Type->Factory != type || pBld->Type->Naval != isNaval)
					continue;

				if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
					continue;

				if (type == AbstractType::AircraftType && !BuildingExt::HasFreeDocks(pBld))
					continue;

				if (!isNaval && type != AbstractType::AircraftType
					&& !AreZonesConnected(pBld->GetMapCoords(), goalCoords, MovementZone::Normal))
				{
					continue;
				}

				const double distSq = pBld->GetMapCoords().DistanceFromSquared(goalCoords);

				if (distSq < bestDistanceSq)
				{
					bestDistanceSq = distSq;

					pBestFactory = pBld;
				}
			}

			if (!pBestFactory)
			{
				for (auto const pBld : pHouse->Buildings)
				{
					if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
						continue;

					if (pBld->Type->Factory != type || pBld->Type->Naval != isNaval)
						continue;

					const double distSq = pBld->GetMapCoords().DistanceFromSquared(goalCoords);

					if (distSq < bestDistanceSq)
					{
						bestDistanceSq = distSq;

						pBestFactory = pBld;
					}
				}
			}

			if (!pBestFactory || pBestFactory->IsPrimaryFactory)
				return;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Factory == type && pBld->Type->Naval == isNaval)
					pBld->IsPrimaryFactory = (pBld == pBestFactory);
			}

			if (auto const pExt = HouseExt::Fetch(pHouse))
			{
				if (type == AbstractType::UnitType)
				{
					if (isNaval)
						pExt->Factory_NavyType = pBestFactory;
					else
						pExt->Factory_VehicleType = pBestFactory;
				}
				else if (type == AbstractType::InfantryType)
				{
					pExt->Factory_InfantryType = pBestFactory;
				}
				else if (type == AbstractType::AircraftType)
				{
					pExt->Factory_AircraftType = pBestFactory;
				}
				else if (type == AbstractType::BuildingType)
				{
					pExt->Factory_BuildingType = pBestFactory;
				}
			}

			FactoryClass* pActiveFactory = pHouse->GetPrimaryFactory(type, isNaval, BuildCat::DontCare);

			if (!pActiveFactory)
			{
				for (auto const pBld : pHouse->Buildings)
				{
					if (pBld && pBld->IsAlive && pBld->Type && pBld->Type->Factory == type
						&& pBld->Type->Naval == isNaval && pBld->Factory != nullptr)
					{
						pActiveFactory = pBld->Factory;

						break;
					}
				}
			}

			if (pActiveFactory)
			{
				for (auto const pBld : pHouse->Buildings)
				{
					if (pBld && pBld->Type && pBld->Type->Factory == type && pBld->Type->Naval == isNaval)
					{
						pBld->IsPrimaryFactory = (pBld == pBestFactory);

						if (pBld == pBestFactory)
							pBld->Factory = pActiveFactory;
						else if (pBld->Factory == pActiveFactory)
							pBld->Factory = nullptr;
					}
				}

				pHouse->SetPrimaryFactory(pActiveFactory, type, isNaval, BuildCat::DontCare);
			}
		};

		auto const pExt = HouseExt::Fetch(pHouse);
		const CellStruct homeCenter
			= pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
		PruneAttackedBuildingsLIFO(pHouse);
		auto& macroState = GetMacroState(pHouse);
		CellStruct defenseGoal = CellStruct::Empty;
		if (!macroState.AttackedBuildingsLIFO.empty()
			&& (Unsorted::CurrentFrame - macroState.AttackedBuildingsLIFO.back().AttackFrame) <= 270)
		{
			defenseGoal = macroState.AttackedBuildingsLIFO.back().BuildingCoords;
		}

		CellStruct vehGoal = (defenseGoal != CellStruct::Empty) ? defenseGoal : targetCoords;
		const UnitTypeClass* pCurUnitType = nullptr;
		if (FactoryClass* pVF = pHouse->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare))
		{
			if (pVF->Object)
			{
				pCurUnitType
					= abstract_cast<UnitClass*>(pVF->Object) ? abstract_cast<UnitClass*>(pVF->Object)->Type : nullptr;
			}
		}

		if (!pCurUnitType && pExt && pExt->AIBrain_NextUnitTarget >= 0
			&& pExt->AIBrain_NextUnitTarget < UnitTypeClass::Array.Count)
		{
			pCurUnitType = UnitTypeClass::Array.GetItem(pExt->AIBrain_NextUnitTarget);
		}

		if (!pCurUnitType && pHouse->ProducingUnitTypeIndex >= 0
			&& pHouse->ProducingUnitTypeIndex < UnitTypeClass::Array.Count)
		{
			pCurUnitType = UnitTypeClass::Array.GetItem(pHouse->ProducingUnitTypeIndex);
		}

		if (pCurUnitType)
		{
			if (pCurUnitType->Harvester || pCurUnitType->ResourceGatherer)
			{
				CellStruct bestRefCell = CellStruct::Empty;
				int minNearbyHarvs = 9999;
				for (auto const pRef : pHouse->Buildings)
				{
					if (!pRef || !pRef->IsAlive || pRef->InLimbo || !pRef->Type || !pRef->Type->Refinery)
						continue;

					const CellStruct rc = pRef->GetMapCoords();
					int nearHarvs = 0;
					for (auto const pU : UnitClass::Array)
					{
						if (pU && pU->Owner == pHouse && pU->IsAlive && !pU->InLimbo && pU->Type
							&& (pU->Type->Harvester || pU->Type->ResourceGatherer))
						{
							if (pU->GetMapCoords().DistanceFrom(rc) <= 18.0)
								nearHarvs++;
						}
					}

					if (nearHarvs < minNearbyHarvs)
					{
						minNearbyHarvs = nearHarvs;
						bestRefCell = rc;
					}
				}

				if (bestRefCell != CellStruct::Empty)
					vehGoal = bestRefCell;
			}
			else if (defenseGoal == CellStruct::Empty && homeCenter != CellStruct::Empty
				&& pCurUnitType->DeploysInto != nullptr
				&& (pCurUnitType->DeploysInto->ConstructionYard || pCurUnitType->DeploysInto->BaseNormal
					|| pCurUnitType->DeploysInto->Refinery))
			{
				CellStruct bestExpOre = CellStruct::Empty;
				int bestOreDist = 999999;
				for (int dy = -48; dy <= 48; dy += 4)
				{
					for (int dx = -48; dx <= 48; dx += 4)
					{
						const CellStruct tc { static_cast<short>(homeCenter.X + dx),
							static_cast<short>(homeCenter.Y + dy) };
						auto const pC = MapClass::Instance.TryGetCellAt(tc);
						if (!pC || pC->GetContainedTiberiumValue() <= 0)
							continue;

						bool nearHub = false;
						for (auto const pOwnB : pHouse->Buildings)
						{
							if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->Type
								&& (pOwnB->Type->Refinery || pOwnB->Type->ConstructionYard || pOwnB->Type->BaseNormal))
							{
								const CellStruct bc = pOwnB->GetMapCoords();
								if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 14)
								{
									nearHub = true;
									break;
								}
							}
						}

						if (nearHub)
							continue;

						const int d = std::abs(dx) + std::abs(dy);
						if (d < bestOreDist)
						{
							bestOreDist = d;
							bestExpOre = tc;
						}
					}
				}

				if (bestExpOre != CellStruct::Empty)
					vehGoal = bestExpOre;
			}
		}

		CellStruct infGoal = (defenseGoal != CellStruct::Empty) ? defenseGoal : targetCoords;
		const InfantryTypeClass* pCurInfType = nullptr;
		if (FactoryClass* pIF = pHouse->GetPrimaryFactory(AbstractType::InfantryType, false, BuildCat::DontCare))
		{
			if (pIF->Object)
			{
				pCurInfType = abstract_cast<InfantryClass*>(pIF->Object)
					? abstract_cast<InfantryClass*>(pIF->Object)->Type
					: nullptr;
			}
		}

		if (!pCurInfType && pExt && pExt->AIBrain_NextInfantryTarget >= 0
			&& pExt->AIBrain_NextInfantryTarget < InfantryTypeClass::Array.Count)
		{
			pCurInfType = InfantryTypeClass::Array.GetItem(pExt->AIBrain_NextInfantryTarget);
		}

		if (!pCurInfType && pHouse->ProducingInfantryTypeIndex >= 0
			&& pHouse->ProducingInfantryTypeIndex < InfantryTypeClass::Array.Count)
		{
			pCurInfType = InfantryTypeClass::Array.GetItem(pHouse->ProducingInfantryTypeIndex);
		}

		if (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->InfantryAbsorb
					&& (pBld->Type->PowerBonus > 0 || pBld->Type->ExtraPowerBonus > 0)
					&& pBld->Passengers.NumPassengers < pBld->Type->Passengers)
				{
					infGoal = pBld->GetMapCoords();
					break;
				}
			}
		}

		if (pCurInfType && pCurInfType->Engineer && homeCenter != CellStruct::Empty)
		{
			CellStruct bestCapCell = CellStruct::Empty;
			int bestCapScore = 999999;
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner)
					continue;

				if (!pBld->Type->Capturable || !pBld->Owner->IsNeutral())
					continue;

				const CellStruct bc = pBld->GetMapCoords();
				const int dist = std::abs(bc.X - homeCenter.X) + std::abs(bc.Y - homeCenter.Y);
				const int effScore = (pBld->Type->ProduceCashAmount > 0) ? (dist / 2) : dist;
				if (effScore < bestCapScore)
				{
					bestCapScore = effScore;
					bestCapCell = bc;
				}
			}

			if (bestCapCell != CellStruct::Empty)
				infGoal = bestCapCell;
		}

		updateCategory(AbstractType::InfantryType, false, infGoal);

		updateCategory(AbstractType::UnitType, false, vehGoal);

		updateCategory(AbstractType::UnitType, true, targetCoords);
		updateCategory(AbstractType::AircraftType, false, targetCoords);
	}
} // namespace

void NeuralAI::ParseCommandLineArg(const char* pArg)
{
	if (!pArg)
		return;

	if (_stricmp(pArg, "-TRAIN_HEADLESS") == 0 || _stricmp(pArg, "-TrainHeadless") == 0)
	{
		NeuralAI::HeadlessMode = true;

		Debug::Log("[NeuralAI] Headless Turbo Training Mode enabled (-TRAIN_HEADLESS).\n");
	}
	else if (_strnicmp(pArg, "-TrainSlot=", 11) == 0)
	{
		NeuralAI::TrainSlotIndex = std::clamp(std::atoi(pArg + 11), 0, 63);

		Debug::Log("[NeuralAI] Training Slot Index set to %d.\n", NeuralAI::TrainSlotIndex);
	}
}

void NeuralAI::Initialize()
{
	CreateDirectoryA("AI", NULL);

	NeuralAI::EnsureBaseModelLoaded();

	if (NeuralAI::HeadlessMode)
		NeuralAI::InitTelemetrySharedMemory();
}

void NeuralAI::Shutdown()
{
	if (TelemetryJsonView)
	{
		UnmapViewOfFile(TelemetryJsonView);

		TelemetryJsonView = nullptr;
	}

	if (TelemetryJsonShmHandle)
	{
		CloseHandle(TelemetryJsonShmHandle);

		TelemetryJsonShmHandle = NULL;
	}

	if (NeuralAI::TelemetryView)
	{
		UnmapViewOfFile(NeuralAI::TelemetryView);

		NeuralAI::TelemetryView = nullptr;
	}

	if (NeuralAI::TelemetryShmHandle)
	{
		CloseHandle(NeuralAI::TelemetryShmHandle);

		NeuralAI::TelemetryShmHandle = NULL;
	}

	if (NeuralAI::BaseMappedView)
	{
		UnmapViewOfFile(NeuralAI::BaseMappedView);

		NeuralAI::BaseMappedView = nullptr;
	}

	if (NeuralAI::BaseMappingHandle)
	{
		CloseHandle(NeuralAI::BaseMappingHandle);

		NeuralAI::BaseMappingHandle = NULL;
	}

	if (NeuralAI::BaseFileHandle != INVALID_HANDLE_VALUE)
	{
		CloseHandle(NeuralAI::BaseFileHandle);

		NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;
	}
}

bool NeuralAI::IsEnabledForHouse(const HouseClass* pHouse)
{
	if (!pHouse || pHouse->IsControlledByHuman() || pHouse->IsNeutral() || pHouse->IsObserver() || pHouse->Defeated)
		return false;

	if (pHouse->Type && pHouse->Type->MultiplayPassive)
		return false;

	if (auto const pRulesExt = RulesExt::Global())
	{
		if (!pRulesExt->AIBrain)
			return false;
	}

	return true;
}

bool NeuralAI::IsCampaignRestricted(const HouseClass* pHouse)
{
	if (!SessionClass::IsCampaign())
		return false;

	if (!pHouse)
		return true;

	if (pHouse->AutoBaseBuilding)
		return false;

	if (auto const pHouseExt = HouseExt::TryFetch(pHouse))
	{
		if (pHouseExt->AIBrain_Campaign)
			return false;
	}

	return true;
}

uint64_t NeuralAI::ComputeRulesSignature()
{
	uint64_t hash = FnvOffsetBasis;

	auto const pRules = RulesClass::Instance;

	if (!pRules)
		return hash;

	hash = HashValueFNV1a(pRules->HarvestersPerRefinery[0], hash);

	hash = HashValueFNV1a(pRules->AISlaveMinerNumber[0], hash);

	hash = HashValueFNV1a(pRules->PowerSurplus, hash);

	for (auto const pBld : BuildingTypeClass::Array)
	{
		if (!pBld)
			continue;

		hash = HashValueFNV1a(pBld->Cost, hash);

		hash = HashValueFNV1a(pBld->Strength, hash);

		hash = HashValueFNV1a(pBld->TechLevel, hash);

		hash = HashValueFNV1a(pBld->PowerBonus, hash);

		hash = HashValueFNV1a(pBld->PowerDrain, hash);

		hash = HashValueFNV1a(pBld->GetOwners(), hash);
	}

	for (auto const pUnit : UnitTypeClass::Array)
	{
		if (!pUnit)
			continue;

		hash = HashValueFNV1a(pUnit->Cost, hash);

		hash = HashValueFNV1a(pUnit->Strength, hash);

		hash = HashValueFNV1a(pUnit->Speed, hash);

		hash = HashValueFNV1a(pUnit->TechLevel, hash);

		hash = HashValueFNV1a(pUnit->GetOwners(), hash);
	}

	for (auto const pInf : InfantryTypeClass::Array)
	{
		if (!pInf)
			continue;

		hash = HashValueFNV1a(pInf->Cost, hash);

		hash = HashValueFNV1a(pInf->Strength, hash);

		hash = HashValueFNV1a(pInf->TechLevel, hash);

		hash = HashValueFNV1a(pInf->GetOwners(), hash);
	}

	for (auto const pAir : AircraftTypeClass::Array)
	{
		if (!pAir)
			continue;

		hash = HashValueFNV1a(pAir->Cost, hash);

		hash = HashValueFNV1a(pAir->Strength, hash);

		hash = HashValueFNV1a(pAir->TechLevel, hash);

		hash = HashValueFNV1a(pAir->GetOwners(), hash);
	}

	return hash;
}

void NeuralAI::EnsureBaseModelLoaded()
{
	if (NeuralAI::BaseMappedView)
		return;

	CreateDirectoryA("AI", NULL);

	CreateDirectoryA("Resources\\AI", NULL);

	const char* basePath = (GetFileAttributesA("Resources\\AI\\ai_base.bin") != INVALID_FILE_ATTRIBUTES)
		? "Resources\\AI\\ai_base.bin"
		: "AI\\ai_base.bin";

	DWORD attrs = GetFileAttributesA(basePath);

	if (attrs == INVALID_FILE_ATTRIBUTES)
	{
		// Bootstrap a deterministic compact INT8 MoE base brain if none exists yet.
		// Phobos_AI_Trainer can expand or replace this with the full 250 MB league
		// brain.
		constexpr uint32_t expertBlockSize = NeuralAI::InputVectorSize * NeuralAI::OutputVectorSize;

		constexpr uint32_t totalBootstrapSize = sizeof(AIBaseHeader) + (expertBlockSize * NeuralAI::NumSpecialists);

		std::vector<uint8_t> buffer(totalBootstrapSize, 0);

		auto const pHdr = reinterpret_cast<AIBaseHeader*>(buffer.data());

		std::memcpy(pHdr->Magic, "PHOBOSAI", 8);

		pHdr->SchemaVersion = 1;

		pHdr->TotalSizeBytes = totalBootstrapSize;

		pHdr->NumExperts = NeuralAI::NumSpecialists;

		pHdr->NumArchetypes = NeuralAI::NumArchetypes;

		pHdr->InputDim = 192;

		pHdr->ReservedInputDim = 64;

		pHdr->HiddenDim1 = 256;

		pHdr->HiddenDim2 = 128;

		pHdr->OutputDim = 64;

		pHdr->ReservedOutputDim = 64;

		for (int i = 0; i < 256; ++i)
		{
			const int x = i - 128;
			const double norm = static_cast<double>(x) / 32.0;
			const double sig = 1.0 / (1.0 + std::exp(-norm));
			const double tnh = std::tanh(norm);

			pHdr->SigmoidLUT[i] = ClampInt8(static_cast<int>(sig * 127.0));

			pHdr->TanhLUT[i] = ClampInt8(static_cast<int>(tnh * 127.0));
		}

		uint32_t offset = sizeof(AIBaseHeader);

		uint64_t seed = 0x50484F424F534149ULL;

		for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
		{
			if (e < 8)
				pHdr->ExpertWeightsOffset[e] = offset;
			else
				pHdr->ExtraExpertWeightsOffset[e - 8] = offset;

			int8_t* w = reinterpret_cast<int8_t*>(buffer.data() + offset);

			for (uint32_t idx = 0; idx < expertBlockSize; ++idx)
			{
				seed ^= seed >> 12;

				seed ^= seed << 25;

				seed ^= seed >> 27;

				const int32_t val = static_cast<int32_t>((seed * 0x2545F4914F6CDD1DULL) >> 58) - 32;

				w[idx] = ClampInt8(val);
			}

			offset += expertBlockSize;
		}

		pHdr->BaseModelHash
			= HashBytesFNV1a(buffer.data() + sizeof(AIBaseHeader), totalBootstrapSize - sizeof(AIBaseHeader));

		HANDLE hOut
			= CreateFileA(basePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hOut != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;

			WriteFile(hOut, buffer.data(), totalBootstrapSize, &written, NULL);

			CloseHandle(hOut);
		}
	}

	NeuralAI::BaseFileHandle = CreateFileA(
		basePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (NeuralAI::BaseFileHandle == INVALID_HANDLE_VALUE)
		return;

	LARGE_INTEGER fileSize = {};

	if (!GetFileSizeEx(NeuralAI::BaseFileHandle, &fileSize)
		|| fileSize.QuadPart < static_cast<LONGLONG>(sizeof(AIBaseHeader)))
	{
		CloseHandle(NeuralAI::BaseFileHandle);

		NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;

		return;
	}

	NeuralAI::BaseMappedSize = static_cast<size_t>(fileSize.QuadPart);

	NeuralAI::BaseMappingHandle
		= CreateFileMappingA(NeuralAI::BaseFileHandle, NULL, PAGE_READONLY, 0, 0, "Local\\PhobosAIBaseSharedMapping");

	if (!NeuralAI::BaseMappingHandle)
		NeuralAI::BaseMappingHandle = CreateFileMappingA(NeuralAI::BaseFileHandle, NULL, PAGE_READONLY, 0, 0, NULL);

	if (NeuralAI::BaseMappingHandle)
	{
		NeuralAI::BaseMappedView
			= static_cast<const uint8_t*>(MapViewOfFile(NeuralAI::BaseMappingHandle, FILE_MAP_READ, 0, 0, 0));
	}
}

bool NeuralAI::LoadBackpackFile(const char* pPath, AIBackpackFile& outBackpack, bool requireSignature)
{
	HANDLE hFile = CreateFileA(
		pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	AIBackpackFile loaded = {};

	DWORD bytesRead = 0;

	const bool valid = ReadFile(hFile, &loaded, sizeof(AIBackpackFile), &bytesRead, NULL)
		&& bytesRead == sizeof(AIBackpackFile) && std::memcmp(loaded.Magic, "PHOBOSBP", 8) == 0
		&& loaded.SchemaVersion == 3;

	CloseHandle(hFile);

	if (valid)
	{
		if (requireSignature && loaded.BaseSignature != NeuralAI::CurrentBaseSignature)
			loaded.BaseSignature = NeuralAI::CurrentBaseSignature;

		outBackpack = loaded;
	}

	return valid;
}

const AIBackpackFile& NeuralAI::PolicyFor(const HouseClass* pHouse)
{
	if (NeuralAI::HeadlessMode && NeuralAI::OpponentLoaded)
	{
		auto const pExt = HouseExt::TryFetch(pHouse);

		if (pExt && pExt->AIBrain_PolicySlot == 1)
			return NeuralAI::OpponentBackpack;
	}

	return NeuralAI::Backpack;
}

void NeuralAI::LoadOrResetBackpack()
{
	NeuralAI::CurrentRulesSignature = NeuralAI::ComputeRulesSignature();

	uint64_t baseModelHash = 0;

	if (NeuralAI::BaseMappedView && NeuralAI::BaseMappedSize >= sizeof(AIBaseHeader))
	{
		auto const pHdr = reinterpret_cast<const AIBaseHeader*>(NeuralAI::BaseMappedView);

		baseModelHash = pHdr->BaseModelHash;
	}

	NeuralAI::CurrentBaseSignature = HashValueFNV1a(baseModelHash, NeuralAI::CurrentRulesSignature);

	// Campaign and standard Multiplayer use only the shared deterministic base
	// brain.
	if (SessionClass::IsCampaign())
	{
		NeuralAI::AllowBackpackInference = false;

		return;
	}

	if (SessionClass::IsMultiplayer() && !NeuralAI::HeadlessMode)
	{
		int humanCount = 0;
		for (auto const pH : HouseClass::Array)
		{
			if (pH && (pH->IsControlledByHuman() || pH->IsObserver()))
				humanCount++;
		}

		if (humanCount > 1)
		{
			NeuralAI::AllowBackpackInference = false;
			return;
		}
	}

	// In headless training the trainer owns both policy files: they are loaded
	// verbatim and never written back
	if (NeuralAI::HeadlessMode)
	{
		char policyPath[MAX_PATH];

		std::memset(&NeuralAI::Backpack, 0, sizeof(AIBackpackFile));

		std::memcpy(NeuralAI::Backpack.Magic, "PHOBOSBP", 8);

		NeuralAI::Backpack.SchemaVersion = 3;

		NeuralAI::Backpack.BaseSignature = NeuralAI::CurrentBaseSignature;

		NeuralAI::Backpack.EstimatedELO = 1200;

		std::snprintf(
			policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_slot_%d.dat", NeuralAI::TrainSlotIndex);

		const bool challengerLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::Backpack, false);

		std::memcpy(&NeuralAI::OpponentBackpack, &NeuralAI::Backpack, sizeof(AIBackpackFile));

		std::snprintf(
			policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_opp_slot_%d.dat", NeuralAI::TrainSlotIndex);

		NeuralAI::OpponentLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::OpponentBackpack, false);

		NeuralAI::AllowBackpackInference = true;

		NeuralAI::BackpackLoaded = true;

		Debug::Log("[NeuralAI] Headless policies: challenger=%s opponent=%s\n", challengerLoaded ? "loaded" : "default",
			NeuralAI::OpponentLoaded ? "loaded" : "shared");

		return;
	}

	NeuralAI::AllowBackpackInference = true;

	const char* bpPath = (GetFileAttributesA("Resources\\AI\\ai_backpack.dat") != INVALID_FILE_ATTRIBUTES)
		? "Resources\\AI\\ai_backpack.dat"
		: "AI\\ai_backpack.dat";

	const bool valid = NeuralAI::LoadBackpackFile(bpPath, NeuralAI::Backpack, true);

	if (!valid)
	{
		std::memset(&NeuralAI::Backpack, 0, sizeof(AIBackpackFile));

		std::memcpy(NeuralAI::Backpack.Magic, "PHOBOSBP", 8);

		NeuralAI::Backpack.SchemaVersion = 3;

		NeuralAI::Backpack.BaseSignature = NeuralAI::CurrentBaseSignature;

		NeuralAI::Backpack.EstimatedELO = 1200;

		NeuralAI::Backpack.BackpackHash
			= HashBytesFNV1a(&NeuralAI::Backpack.LoRA_A[0], sizeof(NeuralAI::Backpack.LoRA_A));

		NeuralAI::SaveBackpack();

		Debug::Log("[NeuralAI] Backpack initialized/reset for BaseSignature %llX.\n", NeuralAI::CurrentBaseSignature);
	}

	NeuralAI::BackpackLoaded = true;
}

void NeuralAI::SaveBackpack()
{
	if (SessionClass::IsCampaign() || !NeuralAI::AllowBackpackInference)
		return;

	CreateDirectoryA("AI", NULL);

	CreateDirectoryA("Resources\\AI", NULL);

	NeuralAI::Backpack.BackpackHash = HashBytesFNV1a(
		&NeuralAI::Backpack.LoRA_A[0], sizeof(NeuralAI::Backpack.LoRA_A) + sizeof(NeuralAI::Backpack.LoRA_B));

	char bpPath[MAX_PATH];

	if (NeuralAI::HeadlessMode && NeuralAI::TrainSlotIndex > 0)
		std::snprintf(bpPath, sizeof(bpPath), "Resources\\AI\\ai_backpack_slot_%d.dat", NeuralAI::TrainSlotIndex);
	else
		std::snprintf(bpPath, sizeof(bpPath), "Resources\\AI\\ai_backpack.dat");

	HANDLE hFile
		= CreateFileA(bpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile != INVALID_HANDLE_VALUE)
	{
		DWORD written = 0;

		WriteFile(hFile, &NeuralAI::Backpack, sizeof(AIBackpackFile), &written, NULL);

		CloseHandle(hFile);
	}

	if (!NeuralAI::HeadlessMode || NeuralAI::TrainSlotIndex == 0)
	{
		HANDLE hFileAlt = CreateFileA(
			"AI\\ai_backpack.dat", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hFileAlt != INVALID_HANDLE_VALUE)
		{
			DWORD written = 0;

			WriteFile(hFileAlt, &NeuralAI::Backpack, sizeof(AIBackpackFile), &written, NULL);

			CloseHandle(hFileAlt);
		}
	}
}

void NeuralAI::InitTelemetrySharedMemory()
{
	if (!TelemetryJsonView)
	{
		char jsonShmName[128];

		std::snprintf(jsonShmName, sizeof(jsonShmName), "Local\\PhobosAITrainerJsonShm_%d", NeuralAI::TrainSlotIndex);

		TelemetryJsonShmHandle
			= CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, TelemetryJsonShmCapacity, jsonShmName);

		if (TelemetryJsonShmHandle)
		{
			TelemetryJsonView = static_cast<uint8_t*>(
				MapViewOfFile(TelemetryJsonShmHandle, FILE_MAP_ALL_ACCESS, 0, 0, TelemetryJsonShmCapacity));

			if (TelemetryJsonView)
				std::memset(TelemetryJsonView, 0, 16);
		}
	}

	if (NeuralAI::TelemetryView)
		return;

	char shmName[128];

	std::snprintf(shmName, sizeof(shmName), "Local\\PhobosAITrainerShm_%d", NeuralAI::TrainSlotIndex);

	NeuralAI::TelemetryShmHandle
		= CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(AITrainerSharedTelemetry), shmName);

	if (NeuralAI::TelemetryShmHandle)
	{
		NeuralAI::TelemetryView = static_cast<AITrainerSharedTelemetry*>(
			MapViewOfFile(NeuralAI::TelemetryShmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AITrainerSharedTelemetry)));

		if (NeuralAI::TelemetryView)
		{
			std::memset(NeuralAI::TelemetryView, 0, sizeof(AITrainerSharedTelemetry));

			std::memcpy(NeuralAI::TelemetryView->Magic, "PHOBOSTM", 8);

			NeuralAI::TelemetryView->SchemaVersion = 1;

			NeuralAI::TelemetryView->SlotIndex = static_cast<uint32_t>(NeuralAI::TrainSlotIndex);

			NeuralAI::TelemetryView->ProcessId = GetCurrentProcessId();
		}
	}
}

void NeuralAI::OnScenarioStart()
{
	NeuralAI::Initialize();

	NeuralAI::LoadOrResetBackpack();

	for (auto& ms : MacroStates)
		ms.Reset();

	NeuralAI::LastCombatFrame = 0;

	NeuralAI::LastTotalKillsAndCaptures = 0;

	NeuralAI::LastFPSCheckTick = GetTickCount();

	NeuralAI::FramesSinceLastFPSCheck = 0;

	static const wchar_t* const ArchetypeNamesW[8] = { L"Balanced Grandmaster", L"SpecOps & Infiltration",
		L"Tech & Fortress Turtle", L"Blitzkrieg Rush", L"TechBoom & Heavy Armor", L"Harasser & Guerrilla Raider",
		L"Air & Naval Dominator", L"Siege, Urban & Coalition" };

	int activeAIHouses = 0;
	HouseClass* pFirstAI = nullptr;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!NeuralAI::IsEnabledForHouse(pHouse))
			continue;

		activeAIHouses++;

		auto const pExt = HouseExt::Fetch(pHouse);

		if (!NeuralAI::IsCampaignRestricted(pHouse))
		{
			pHouse->Production = true;

			pHouse->AIState_1E4 = 0;
		}

		if (!NeuralAI::HeadlessMode)
		{
			pExt->AIBrain_PolicySlot = 0;
		}
		else if (!pFirstAI)
		{
			pFirstAI = pHouse;

			pExt->AIBrain_PolicySlot = 0;
		}
		else if (pHouse->IsAlliedWith(pFirstAI))
		{
			pExt->AIBrain_PolicySlot = 0;
		}
		else
		{
			bool hasAlliedSlot0 = false;
			bool hasAlliedSlot1 = false;

			for (auto const pPrev : HouseClass::Array)
			{
				if (!pPrev || pPrev == pHouse || !NeuralAI::IsEnabledForHouse(pPrev))
					continue;

				if (pPrev->ArrayIndex >= pHouse->ArrayIndex)
					break;

				if (pHouse->IsAlliedWith(pPrev))
				{
					const int prevSlot = HouseExt::Fetch(pPrev)->AIBrain_PolicySlot;

					if (prevSlot == 0)
						hasAlliedSlot0 = true;
					else
						hasAlliedSlot1 = true;
				}
			}

			if (hasAlliedSlot0)
				pExt->AIBrain_PolicySlot = 0;
			else if (hasAlliedSlot1)
				pExt->AIBrain_PolicySlot = 1;
			else
				pExt->AIBrain_PolicySlot = (activeAIHouses - 1) % 2;
		}

		// Select Strategic Archetype (0..4) weighted by evolved OutputBias[48..52]
		// + learned faction memory + stochastic exploration
		int bestArch = ScenarioClass::Instance->Random.RandomRanged(0, NeuralAI::NumArchetypes - 1);

		if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
		{
			const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);

			int bestArchScore = -999999;

			for (int a = 0; a < NeuralAI::NumArchetypes; ++a)
			{
				int memBonus = 0;
				for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
				{
					const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + a * 10 + e) % 512);
					if (policy.Entries[eIdx].VisitCount > 0)
						memBonus += static_cast<int>(policy.Entries[eIdx].ArchetypeScoreDelta[a]) * 3;
				}
				memBonus = std::clamp(memBonus, -1200, 1200);

				const int exploreJitter = ScenarioClass::Instance->Random.RandomRanged(-90, 90);
				const int archBiasSlot = (a < 5) ? (48 + a) : (86 + (a - 5));
				const int aScore = static_cast<int>(policy.OutputBias[archBiasSlot]) + memBonus + exploreJitter;

				if (aScore > bestArchScore)
				{
					bestArchScore = aScore;

					bestArch = a;
				}
			}
		}

		pExt->AIBrain_Archetype = bestArch;

		pExt->AIBrain_ActiveExpert = 0;

		pExt->AIBrain_LastInferenceFrame = -1000;

		pExt->AIBrain_NextBuildingTarget = -1;

		pExt->AIBrain_NextUnitTarget = -1;

		pExt->AIBrain_NextNavalTarget = -1;

		pExt->AIBrain_NextInfantryTarget = -1;

		pExt->AIBrain_NextAircraftTarget = -1;

		pExt->AIBrain_PlannedBuildCell = CellStruct::Empty;

		pExt->AIBrain_ScoutTargetCell = CellStruct::Empty;

		pExt->AIBrain_AttackTargetCell = CellStruct::Empty;

		pExt->AIBrain_InfiltratedEnemyMask = 0;

		pExt->AIBrain_ScoutedEnemyMask = 0;

		pExt->AIBrain_EstimatedEnemyThreat.fill(0);

		const char* countryId = (pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AIHouse";

		const int archIdx = std::clamp(pExt->AIBrain_Archetype, 0, NeuralAI::NumArchetypes - 1);

		Debug::Log("[NeuralAI] Activated for House #%d (%s, Diff=%d) -> "
				"Archetype=%d (%ls)\n",
			pHouse->ArrayIndex, countryId, pHouse->GetAIDifficultyIndex(), archIdx, ArchetypeNamesW[archIdx]);

		if (!NeuralAI::HeadlessMode)
		{
			wchar_t msgBuf[256];

			std::swprintf(msgBuf, 256, L"[Neural AI] House #%d (%hs) -> Style: %ls", pHouse->ArrayIndex, countryId,
				ArchetypeNamesW[archIdx]);

			MessageListClass::Instance.PrintMessage(msgBuf, RulesClass::Instance->MessageDelay,
				HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : 0, true);
		}
	}

	if (activeAIHouses > 0)
	{
		Debug::Log("[NeuralAI] ScenarioStart: %d AI Houses controlled by NeuralAI "
				"(aimd.ini bypassed) | BaseSignature=%016llX | ELO=%d\n",
			activeAIHouses, NeuralAI::CurrentBaseSignature, NeuralAI::Backpack.EstimatedELO);

		if (!NeuralAI::HeadlessMode)
		{
			wchar_t bannerBuf[256];

			std::swprintf(bannerBuf, 256,
				L"[Phobos Neural AI] ACTIVE (%d AI Houses | Brain Sig:%08X | ELO:%d "

				L"| aimd.ini bypassed)",
				activeAIHouses, static_cast<uint32_t>(NeuralAI::CurrentBaseSignature & 0xFFFFFFFFULL),
				NeuralAI::Backpack.EstimatedELO);

			MessageListClass::Instance.PrintMessage(bannerBuf, RulesClass::Instance->MessageDelay,
				HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : 0, true);
		}
	}

	if (NeuralAI::HeadlessMode)
	{
		reinterpret_cast<void(__fastcall*)()>(0x407020)();

		if (Game::hWnd && !IsIconic(Game::hWnd))
		{
			ReleaseCapture();

			ShowWindow(Game::hWnd, SW_SHOWMINNOACTIVE);
		}

		NeuralAI::UpdateTelemetrySnapshot(false, -1, false);
	}
	else if (auto const pCur = HouseClass::CurrentPlayer)
	{
		const bool isObs = HouseClass::IsCurrentPlayerObserver() || Game::ObserverMode || pCur->IsInitiallyObserver()
			|| (pCur->Type && !_stricmp(pCur->Type->ID, "Observer"));
		if (isObs)
		{
			pCur->MakeObserver();
			pCur->Visionary = 1;
			MapClass::Instance.Reveal(pCur);
		}
	}
}

void NeuralAI::OnScenarioEnd(bool playerWon)
{
	if (SessionClass::IsCampaign())
		return;

	int bestHouseIdx = -1;
	int bestScore = -1;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver())
			continue;

		const int score = (pHouse->TotalKilledUnits * 200) + (pHouse->TotalKilledBuildings * 450)
			+ (pHouse->OwnedBuildings * 300) + (pHouse->OwnedUnits * 200);

		if (pHouse->IsWinner || (!pHouse->Defeated && score > bestScore))
		{
			bestScore = score;

			bestHouseIdx = pHouse->ArrayIndex;
		}
	}

	if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded && !NeuralAI::HeadlessMode)
	{
		NeuralAI::Backpack.GamesPlayed++;

		for (auto const pHouse : HouseClass::Array)
		{
			if (!NeuralAI::IsEnabledForHouse(pHouse))
				continue;

			auto const pExt = HouseExt::Fetch(pHouse);
			const bool won = (pHouse->ArrayIndex == bestHouseIdx) || pHouse->IsWinner;

			if (won)
			{
				NeuralAI::Backpack.WinsCount++;

				NeuralAI::Backpack.EstimatedELO = std::min(3500, NeuralAI::Backpack.EstimatedELO + 8);
			}
			else
			{
				NeuralAI::Backpack.EstimatedELO = std::max(800, NeuralAI::Backpack.EstimatedELO - 4);
			}

			const int arch = std::clamp(pExt->AIBrain_Archetype, 0, 7);
			const int exp = std::clamp(pExt->AIBrain_ActiveExpert, 0, NeuralAI::NumSpecialists - 1);

			const int fastBonus
				= (!NeuralAI::MatchEndStalemate && won && Unsorted::CurrentFrame > 0 && Unsorted::CurrentFrame <= 18000)
				? 4
				: 0;

			const int delta = NeuralAI::MatchEndStalemate ? (won ? -4 : -8) : (won ? (12 + fastBonus) : -6);

			uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + arch * 10 + exp) % 512);

			if (NeuralAI::Backpack.NumEntries < 512)
				NeuralAI::Backpack.NumEntries = std::max(NeuralAI::Backpack.NumEntries, entryIdx + 1);

			auto& entry = NeuralAI::Backpack.Entries[entryIdx];

			entry.StateClusterHash = (static_cast<uint32_t>(pHouse->SideIndex) << 16) | static_cast<uint32_t>(arch << 8)
				| static_cast<uint32_t>(exp);

			entry.ArchetypeScoreDelta[arch] = ClampInt16(entry.ArchetypeScoreDelta[arch] + delta);

			if (exp < 8)
				entry.ExpertScoreDelta[exp] = ClampInt16(entry.ExpertScoreDelta[exp] + delta);
			else
				entry.BuildCategoryDelta[14 + (exp - 8)] = ClampInt16(entry.BuildCategoryDelta[14 + (exp - 8)] + delta);

			entry.VisitCount++;

			// Update LoRA rank-16 adapter weights deterministically
			const int loraIdx = (arch * 64 + exp * 8) % (256 * 16);

			NeuralAI::Backpack.LoRA_A[loraIdx] = ClampInt8(NeuralAI::Backpack.LoRA_A[loraIdx] + (won ? 2 : -1));
		}

		NeuralAI::SaveBackpack();
	}

	if (NeuralAI::HeadlessMode)
	{
		NeuralAI::UpdateTelemetrySnapshot(true,
			NeuralAI::MatchEndWinnerHouse >= 0 ? NeuralAI::MatchEndWinnerHouse : bestHouseIdx,
			NeuralAI::MatchEndStalemate);
	}
}

void NeuralAI::OnAutoBaseBuildingTriggered(HouseClass* pHouse, bool enabled)
{
	if (!pHouse)
		return;

	pHouse->AutoBaseBuilding = enabled;

	if (enabled)
	{
		if (auto const pExt = HouseExt::TryFetch(pHouse))
			pExt->AIBrain_Campaign = true;
	}
}

void NeuralAI::OnBuildingDamaged(BuildingClass* pBuilding, TechnoClass* pAttacker, HouseClass* pAttackerHouse)
{
	if (!pBuilding || !pBuilding->Owner || !NeuralAI::IsEnabledForHouse(pBuilding->Owner))
		return;

	auto& state = GetMacroState(pBuilding->Owner);

	const CellStruct bldCoords = pBuilding->GetMapCoords();
	CellStruct attackerCoords = CellStruct::Empty;

	TechnoTypeClass* pAttackerType = nullptr;

	NeuralThreatCategory threat = NeuralThreatCategory::None;

	if (pAttacker)
	{
		attackerCoords = pAttacker->GetMapCoords();

		pAttackerType = pAttacker->GetTechnoType();

		if (pAttacker->IsInAir() || pAttacker->WhatAmI() == AbstractType::Aircraft
			|| (pAttackerType && (pAttackerType->JumpJet || pAttackerType->ConsideredAircraft)))
		{
			threat = NeuralThreatCategory::Air;
		}
		else if (pAttacker->WhatAmI() == AbstractType::Infantry)
		{
			threat = NeuralThreatCategory::Infantry;
		}
		else
		{
			threat = NeuralThreatCategory::Vehicle;
		}
	}
	else if (pAttackerHouse && pAttackerHouse->ConYards.Count > 0)
	{
		attackerCoords = pAttackerHouse->ConYards.GetItem(0)->GetMapCoords();
	}

	if (pBuilding->Type && !pBuilding->Type->IsBaseDefense && !pBuilding->Type->ConstructionYard
		&& pBuilding->GetHealthPercentage() < 0.35)
	{
		RecordUnsafePlacementZone(pBuilding->Owner, bldCoords, 2700);
	}

	auto it = std::find_if(state.AttackedBuildingsLIFO.begin(), state.AttackedBuildingsLIFO.end(),
		[&](const NeuralAttackedBuildingRecord& r) { return r.BuildingCoords == bldCoords; });

	if (it != state.AttackedBuildingsLIFO.end())
	{
		NeuralAttackedBuildingRecord rec = *it;

		rec.AttackerCoords = attackerCoords;

		rec.AttackerType = pAttackerType;

		if (threat != NeuralThreatCategory::None)
			rec.Threat = threat;

		rec.AttackFrame = Unsorted::CurrentFrame;

		state.AttackedBuildingsLIFO.erase(it);

		state.AttackedBuildingsLIFO.push_back(rec);
	}
	else
	{
		NeuralAttackedBuildingRecord rec;

		rec.BuildingCoords = bldCoords;

		rec.AttackerCoords = attackerCoords;

		rec.AttackerType = pAttackerType;

		rec.Threat = threat;

		rec.AttackFrame = Unsorted::CurrentFrame;

		state.AttackedBuildingsLIFO.push_back(rec);
	}

	if (state.AttackedBuildingsLIFO.size() > 20)
		state.AttackedBuildingsLIFO.erase(state.AttackedBuildingsLIFO.begin());
}

void NeuralAI::OnHeadlessFrameTick()
{
	if (!NeuralAI::HeadlessMode)
		return;

	NeuralAI::FramesSinceLastFPSCheck++;

	const DWORD now = GetTickCount();

	if (now - NeuralAI::LastFPSCheckTick >= 1000)
	{
		NeuralAI::CurrentMeasuredFPS
			= (NeuralAI::FramesSinceLastFPSCheck * 1000) / std::max<DWORD>(1, now - NeuralAI::LastFPSCheckTick);

		NeuralAI::FramesSinceLastFPSCheck = 0;

		NeuralAI::LastFPSCheckTick = now;
	}

	const int frame = Unsorted::CurrentFrame;

	if (frame % 60 != 0)
		return;

	int totalKillsAndCaptures = 0;
	int aliveCombatHouses = 0;
	int lastAliveHouseIdx = -1;

	std::vector<HouseClass*> aliveCombatList;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver())
			continue;

		totalKillsAndCaptures
			+= pHouse->TotalKilledUnits + pHouse->TotalKilledBuildings + pHouse->StolenBuildingsCredits;

		if (!pHouse->Defeated && (pHouse->OwnedBuildings > 0 || pHouse->OwnedUnits > 0 || pHouse->OwnedInfantry > 0))
		{
			aliveCombatHouses++;

			lastAliveHouseIdx = pHouse->ArrayIndex;

			aliveCombatList.push_back(pHouse);
		}
	}

	bool hasHostilePairAlive = false;

	for (size_t i = 0; i < aliveCombatList.size() && !hasHostilePairAlive; ++i)
	{
		for (size_t j = i + 1; j < aliveCombatList.size(); ++j)
		{
			if (!aliveCombatList[i]->IsAlliedWith(aliveCombatList[j])
				|| !aliveCombatList[j]->IsAlliedWith(aliveCombatList[i]))
			{
				hasHostilePairAlive = true;

				break;
			}
		}
	}

	if (totalKillsAndCaptures > NeuralAI::LastTotalKillsAndCaptures)
	{
		NeuralAI::LastTotalKillsAndCaptures = totalKillsAndCaptures;

		NeuralAI::LastCombatFrame = frame;
	}

	const bool onlyOneSideLeft = (frame > 300 && (aliveCombatHouses <= 1 || !hasHostilePairAlive));
	const bool activeRecentCombat = ((frame - NeuralAI::LastCombatFrame) < 2700);
	const int baseMaxFrames = (aliveCombatHouses >= 3) ? 162000 : 135000;
	const int maxMatchFrames = activeRecentCombat ? (baseMaxFrames + 27000) : baseMaxFrames;
	const int noCombatLimit = (aliveCombatHouses >= 3) ? 13500 : 10800;

	const bool stalemate = !onlyOneSideLeft
		&& ((frame > 18000 && (frame - NeuralAI::LastCombatFrame) >= noCombatLimit) || (frame >= maxMatchFrames));

	if (stalemate || onlyOneSideLeft)
	{
		int bestHouseIdx = lastAliveHouseIdx;
		int bestScore = -1;

		for (auto const pHouse : HouseClass::Array)
		{
			if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || pHouse->Defeated)
				continue;

			const int score = (pHouse->TotalKilledUnits * 200) + (pHouse->TotalKilledBuildings * 450)
				+ (pHouse->OwnedBuildings * 300) + (pHouse->OwnedUnits * 200);

			if (score > bestScore)
			{
				bestScore = score;

				bestHouseIdx = pHouse->ArrayIndex;
			}
		}

		NeuralAI::MatchEndWinnerHouse = bestHouseIdx;

		NeuralAI::MatchEndStalemate = stalemate;

		NeuralAI::UpdateTelemetrySnapshot(true, bestHouseIdx, stalemate);

		NeuralAI::OnScenarioEnd(true);

		Game::IsActive = false;

		return;
	}

	if (frame % 60 == 0)
		NeuralAI::UpdateTelemetrySnapshot(false, -1, false);
}

void NeuralAI::UpdateTelemetrySnapshot(bool matchEnded, int winnerHouseIdx, bool stalemate)
{
	AITrainerSharedTelemetry localSnap = {};

	AITrainerSharedTelemetry* dst = NeuralAI::TelemetryView ? NeuralAI::TelemetryView : &localSnap;

	std::memcpy(dst->Magic, "PHOBOSTM", 8);

	dst->SchemaVersion = 1;

	dst->SlotIndex = static_cast<uint32_t>(NeuralAI::TrainSlotIndex);

	dst->ProcessId = GetCurrentProcessId();

	dst->CurrentFrame = static_cast<uint32_t>(Unsorted::CurrentFrame);

	dst->FramesPerSecond = static_cast<uint32_t>(NeuralAI::CurrentMeasuredFPS);

	dst->MatchFinished = matchEnded ? 1u : 0u;

	dst->WinnerHouseIndex = winnerHouseIdx;

	dst->StalemateTriggered = stalemate ? 1u : 0u;

	dst->RulesSignature = NeuralAI::CurrentRulesSignature;

	uint32_t hCount = 0;

	for (auto const pHouse : HouseClass::Array)
	{
		if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || hCount >= 8)
			continue;

		auto const pExt = HouseExt::Fetch(pHouse);

		auto& hs = dst->Houses[hCount++];

		hs.ArrayIndex = pHouse->ArrayIndex;

		hs.SideIndex = pHouse->SideIndex;

		hs.CountryIndex = pHouse->Type ? pHouse->Type->ArrayIndex : -1;

		hs.Archetype = pExt->AIBrain_Archetype;

		hs.ActiveExpert = pExt->AIBrain_ActiveExpert;

		hs.Credits = pHouse->Available_Money();

		hs.KilledUnits = pHouse->TotalKilledUnits;

		hs.KilledBuildings = pHouse->TotalKilledBuildings;

		hs.OwnedUnits = pHouse->OwnedUnits;

		hs.OwnedBuildings = pHouse->OwnedBuildings;

		hs.OwnedInfantry = pHouse->OwnedInfantry;

		hs.OwnedAircraft = pHouse->OwnedAircraft;

		hs.NetWorthScore
			= (hs.KilledUnits * 200) + (hs.KilledBuildings * 450) + (hs.OwnedBuildings * 300) + (hs.OwnedUnits * 200);

		hs.IsWinner = (pHouse->ArrayIndex == winnerHouseIdx || pHouse->IsWinner) ? 1 : 0;

		hs.IsDefeated = pHouse->Defeated ? 1 : 0;

		hs.PolicySlot = static_cast<uint8_t>(pExt->AIBrain_PolicySlot);

		hs.ColorR = pHouse->Color.R;

		hs.ColorG = pHouse->Color.G;

		hs.ColorB = pHouse->Color.B;
	}

	dst->NumHouses = hCount;

	std::memset(dst->RadarCells, 0, sizeof(dst->RadarCells));

	int minMapX = 99999;
	int maxMapX = -99999;
	int minMapY = 99999;
	int maxMapY = -99999;

	auto trackBounds = [&](const CellStruct& c)
	{
		if (c.X > 0 && c.Y > 0)
		{
			if (c.X < minMapX)
				minMapX = c.X;

			if (c.X > maxMapX)
				maxMapX = c.X;

			if (c.Y < minMapY)
				minMapY = c.Y;

			if (c.Y > maxMapY)
				maxMapY = c.Y;
		}
	};

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame
			|| pBld->Owner->IsNeutral())
		{
			continue;
		}

		if (pBld->Type->CanBeOccupied && !pBld->Type->BaseNormal && !pBld->Type->ConstructionYard
			&& pBld->Type->TechLevel < 0)
		{
			continue;
		}

		const CellStruct cell = pBld->GetMapCoords();

		if (cell.X <= 1 && cell.Y <= 1)
			continue;

		trackBounds(cell);

		const int rx = std::clamp(static_cast<int>(cell.X) / 2, 0, 127);
		const int ry = std::clamp(static_cast<int>(cell.Y) / 2, 0, 127);

		dst->RadarCells[ry * 128 + rx] = static_cast<uint8_t>((pBld->Owner->ArrayIndex & 0x07) + 1 + 0x80);
	}

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Owner || pUnit->Owner->IsNeutral())
			continue;

		const CellStruct cell = pUnit->GetMapCoords();

		trackBounds(cell);

		const int rx = std::clamp(static_cast<int>(cell.X) / 2, 0, 127);
		const int ry = std::clamp(static_cast<int>(cell.Y) / 2, 0, 127);

		dst->RadarCells[ry * 128 + rx] = static_cast<uint8_t>((pUnit->Owner->ArrayIndex & 0x07) + 1);
	}

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || pInf->Owner->IsNeutral())
			continue;

		trackBounds(pInf->GetMapCoords());
	}

	if (minMapX > maxMapX)
	{
		minMapX = 0;

		maxMapX = 256;

		minMapY = 0;

		maxMapY = 256;
	}
	else
	{
		minMapX = std::max(0, minMapX - 10);

		minMapY = std::max(0, minMapY - 10);

		maxMapX += 10;

		maxMapY += 10;
	}

	const int spanX = std::max(32, maxMapX - minMapX);
	const int spanY = std::max(32, maxMapY - minMapY);
	const int mapW = MapClass::Instance.MapRect.Width;
	const int localX = MapClass::Instance.VisibleRect.X;
	const int localY = MapClass::Instance.VisibleRect.Y;
	const int localW = MapClass::Instance.VisibleRect.Width;
	const int localH = MapClass::Instance.VisibleRect.Height;
	const bool hasIsoBounds = (mapW > 0 && localW > 0 && localH > 0);

	auto projectToRadar = [&](const CellStruct& c, float& outRx, float& outRy)
	{
		if (hasIsoBounds)
		{
			const float isoX
				= static_cast<float>((static_cast<int>(c.X) - static_cast<int>(c.Y) + mapW - 1) - (2 * localX));

			const float isoY
				= static_cast<float>((static_cast<int>(c.X) + static_cast<int>(c.Y) - mapW - 1) - (2 * localY));

			outRx = std::clamp(3.0f + (isoX * 121.0f) / std::max(1.0f, 2.0f * localW), 2.0f, 125.0f);

			outRy = std::clamp(3.0f + (isoY * 121.0f) / std::max(1.0f, 2.0f * localH), 2.0f, 125.0f);
		}
		else
		{
			outRx = std::clamp(6.0f + ((c.X - minMapX) * 115.0f) / spanX, 4.0f, 123.0f);

			outRy = std::clamp(6.0f + ((c.Y - minMapY) * 115.0f) / spanY, 4.0f, 123.0f);
		}
	};

	if (matchEnded || (Unsorted::CurrentFrame % 60 == 0))
	{
		std::string jsonBuf;

		jsonBuf.reserve(32768);

		char lineBuf[2048];

		int lineLen = std::snprintf(lineBuf, sizeof(lineBuf),
			"{\n  \"slot\": %u,\n  \"pid\": %u,\n  \"frame\": %u,\n  \"fps\": "
			"%u,\n  \"finished\": %s,\n  \"winnerHouse\": %d,\n  \"stalemate\": "
			"%s,\n  \"elo\": %d,\n  \"gamesPlayed\": %u,\n  "
			"\"rulesSignature\": \"%016llX\",\n  \"baseSignature\": \"%016llX\",\n "
			" \"houses\": [\n",
			dst->SlotIndex, dst->ProcessId, dst->CurrentFrame, dst->FramesPerSecond, matchEnded ? "true" : "false",
			dst->WinnerHouseIndex, stalemate ? "true" : "false", NeuralAI::Backpack.EstimatedELO,
			NeuralAI::Backpack.GamesPlayed, static_cast<unsigned long long>(NeuralAI::CurrentRulesSignature),
			static_cast<unsigned long long>(NeuralAI::CurrentBaseSignature));

		if (lineLen > 0)
			jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

		for (uint32_t i = 0; i < dst->NumHouses; ++i)
		{
			const auto& h = dst->Houses[i];

			auto const pOwner = (h.ArrayIndex >= 0 && h.ArrayIndex < HouseClass::Array.Count)
				? HouseClass::Array.GetItem(h.ArrayIndex)
				: nullptr;

			int defenses = 0;

			std::map<const BuildingTypeClass*, int> bldComp;

			if (pOwner)
			{
				for (auto const pOwned : pOwner->Buildings)
				{
					if (pOwned && pOwned->Type && !pOwned->InLimbo && pOwned->IsAlive && !pOwned->Type->InvisibleInGame)
					{
						if (pOwned->Type->CanBeOccupied && !pOwned->Type->BaseNormal && !pOwned->Type->ConstructionYard
							&& pOwned->Type->TechLevel < 0)
						{
							continue;
						}

						const bool isDeployedVehicle = pOwned->Type->ResourceGatherer
							|| (pOwned->Type->UndeploysInto != nullptr && !pOwned->Type->ConstructionYard
								&& !pOwned->Type->BaseNormal && !pOwned->Type->Refinery);
						if (isDeployedVehicle)
							continue;

						if (pOwned->Type->IsBaseDefense)
							defenses++;

						bldComp[pOwned->Type]++;
					}
				}
			}

			std::vector<std::pair<int, const BuildingTypeClass*>> rankedBlds;

			for (auto const& bEntry : bldComp)
				rankedBlds.emplace_back(bEntry.second, bEntry.first);

			std::sort(
				rankedBlds.begin(), rankedBlds.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

			std::string bldText;

			for (size_t rb = 0; rb < rankedBlds.size() && rb < 8; ++rb)
			{
				if (rb > 0)
					bldText += ";";

				bldText += rankedBlds[rb].second->ID;

				bldText += ":" + std::to_string(rankedBlds[rb].first);
			}

			std::map<const TechnoTypeClass*, int> composition;

			if (pOwner)
			{
				for (auto const pOwnedUnit : UnitClass::Array)
				{
					if (pOwnedUnit && pOwnedUnit->Owner == pOwner && pOwnedUnit->Type && !pOwnedUnit->InLimbo
						&& pOwnedUnit->IsAlive)
					{
						composition[pOwnedUnit->Type]++;
					}
				}

				for (auto const pOwnedInf : InfantryClass::Array)
				{
					if (pOwnedInf && pOwnedInf->Owner == pOwner && pOwnedInf->Type && !pOwnedInf->InLimbo
						&& pOwnedInf->IsAlive)
					{
						composition[pOwnedInf->Type]++;
					}
				}

				for (auto const pOwnedAir : AircraftClass::Array)
				{
					if (pOwnedAir && pOwnedAir->Owner == pOwner && pOwnedAir->Type && !pOwnedAir->InLimbo
						&& pOwnedAir->IsAlive)
					{
						composition[pOwnedAir->Type]++;
					}
				}
			}

			std::vector<std::pair<int, const TechnoTypeClass*>> ranked;

			for (auto const& entry : composition)
				ranked.emplace_back(entry.second, entry.first);

			std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

			std::string compositionText;

			for (size_t r = 0; r < ranked.size() && r < 10; ++r)
			{
				if (r > 0)
					compositionText += ";";

				compositionText += ranked[r].second->ID;

				compositionText += ":" + std::to_string(ranked[r].first);
			}

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf),
				"    {\"idx\": %d, \"side\": %d, \"country\": %d, \"archetype\": %d, "
				"\"expert\": %d, \"credits\": %d, \"killedUnits\": %d, "
				"\"killedBlds\": %d, \"ownedUnits\": %d, \"ownedBlds\": "
				"%d, \"score\": %d, \"winner\": %u, \"defeated\": %u, \"policy\": "
				"%u, \"conYards\": %d, \"harvesters\": %d, \"refineries\": %d, "
				"\"warFactories\": %d, \"barracks\": %d, "
				"\"defenses\": %d, \"blds\": \"%s\", \"units\": \"%s\"}%s\n",
				h.ArrayIndex, h.SideIndex, h.CountryIndex, h.Archetype, h.ActiveExpert, h.Credits, h.KilledUnits,
				h.KilledBuildings, h.OwnedUnits, h.OwnedBuildings, h.NetWorthScore, h.IsWinner, h.IsDefeated,
				h.PolicySlot, pOwner ? pOwner->NumConYards : 0, pOwner ? pOwner->CountResourceGatherers : 0,
				pOwner ? pOwner->CountResourceDestinations : 0, pOwner ? pOwner->NumWarFactories : 0,
				pOwner ? pOwner->NumBarracks : 0, defenses, bldText.c_str(), compositionText.c_str(),
				(i + 1 < dst->NumHouses) ? "," : "");

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));
		}

		jsonBuf.append("  ],\n  \"radar\": [");

		auto sanitizeJsonStr = [](const char* src, char* out, size_t outCap)
		{
			if (!out || outCap == 0)
				return;

			size_t w = 0;
			if (src)
			{
				for (size_t i = 0; src[i] != '\0' && w + 1 < outCap; ++i)
				{
					const unsigned char ch = static_cast<unsigned char>(src[i]);
					if (ch >= 32 && ch != '"' && ch != '\\')
						out[w++] = static_cast<char>(ch);
				}
			}

			out[w] = '\0';
		};

		bool firstPt = true;
		int ptCount = 0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame
				|| pBld->Owner->IsNeutral() || pBld->Owner->IsObserver())
			{
				continue;
			}

			if (pBld->Type->CanBeOccupied && !pBld->Type->BaseNormal && !pBld->Type->ConstructionYard
				&& pBld->Type->TechLevel < 0)
			{
				continue;
			}

			if (ptCount >= 320 || jsonBuf.size() > 56000)
				break;

			const CellStruct c = pBld->GetMapCoords();

			if (c.X <= 1 && c.Y <= 1)
				continue;

			float rx = 0.0f;
			float ry = 0.0f;

			projectToRadar(c, rx, ry);

			const bool isDeployedVehicleBld = pBld->Type->ResourceGatherer
				|| (pBld->Type->UndeploysInto != nullptr && !pBld->Type->ConstructionYard && !pBld->Type->BaseNormal
					&& !pBld->Type->Refinery);
			// ptKind: 0 = unit/deployed vehicle, 1 = structure, 2 = defense structure
			// (IsBaseDefense=yes)
			const int ptKind = isDeployedVehicleBld ? 0 : (pBld->Type->IsBaseDefense ? 2 : 1);

			char idClean[32];
			char nameClean[64];
			sanitizeJsonStr(pBld->Type->ID, idClean, sizeof(idClean));
			sanitizeJsonStr(pBld->Type->Name, nameClean, sizeof(nameClean));

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,%d,\"%s\",\"%s\"]", firstPt ? "" : ",",
				rx, ry, pBld->Owner->ArrayIndex, ptKind, idClean, nameClean);

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

			firstPt = false;

			ptCount++;
		}

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Owner || pUnit->Owner->IsNeutral()
				|| pUnit->Owner->IsObserver())
			{
				continue;
			}

			if (ptCount >= 460 || jsonBuf.size() > 58000)
				break;

			const CellStruct c = pUnit->GetMapCoords();
			float rx = 0.0f;
			float ry = 0.0f;

			projectToRadar(c, rx, ry);

			char idClean[32];
			char nameClean[64];
			sanitizeJsonStr(pUnit->Type ? pUnit->Type->ID : "", idClean, sizeof(idClean));
			sanitizeJsonStr(pUnit->Type ? pUnit->Type->Name : "", nameClean, sizeof(nameClean));

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,0,\"%s\",\"%s\"]", firstPt ? "" : ",",
				rx, ry, pUnit->Owner->ArrayIndex, idClean, nameClean);

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

			firstPt = false;

			ptCount++;
		}

		int infStep = 0;

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || pInf->Owner->IsNeutral()
				|| pInf->Owner->IsObserver())
			{
				continue;
			}

			if ((infStep++ % 2) != 0)
				continue;

			if (ptCount >= 520 || jsonBuf.size() > 60000)
				break;

			const CellStruct c = pInf->GetMapCoords();
			float rx = 0.0f;
			float ry = 0.0f;

			projectToRadar(c, rx, ry);

			char idClean[32];
			char nameClean[64];
			sanitizeJsonStr(pInf->Type ? pInf->Type->ID : "", idClean, sizeof(idClean));
			sanitizeJsonStr(pInf->Type ? pInf->Type->Name : "", nameClean, sizeof(nameClean));

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,0,\"%s\",\"%s\"]", firstPt ? "" : ",",
				rx, ry, pInf->Owner->ArrayIndex, idClean, nameClean);

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

			firstPt = false;

			ptCount++;
		}

		jsonBuf.append("]\n}\n");

		if (TelemetryJsonView)
		{
			const uint32_t maxPayload = TelemetryJsonShmCapacity - 16;

			const uint32_t copyLen = static_cast<uint32_t>(std::min<size_t>(jsonBuf.size(), maxPayload));

			volatile uint32_t* pHdr = reinterpret_cast<volatile uint32_t*>(TelemetryJsonView);

			const uint32_t nextOdd = (pHdr[0] | 1u) + 2u;

			pHdr[0] = nextOdd;

			MemoryBarrier();

			pHdr[1] = copyLen;

			std::memcpy(TelemetryJsonView + 8, jsonBuf.data(), copyLen);

			TelemetryJsonView[8 + copyLen] = '\0';

			MemoryBarrier();

			pHdr[0] = nextOdd + 1u;
		}
		else
		{
			char jsonPath[MAX_PATH];

			std::snprintf(jsonPath, sizeof(jsonPath), "AI\\train_telemetry_%d.json", NeuralAI::TrainSlotIndex);

			FILE* fp = nullptr;

			if (fopen_s(&fp, jsonPath, "wb") == 0 && fp)
			{
				std::fwrite(jsonBuf.data(), 1, jsonBuf.size(), fp);

				std::fclose(fp);
			}
		}
	}
}

void NeuralAI::ExtractFogFilteredFeatures(HouseClass* pHouse, std::array<int8_t, InputVectorSize>& features)
{
	features.fill(0);

	auto const pExt = HouseExt::Fetch(pHouse);

	// 0..31: Own Economy, Power & Infrastructure
	const int credits = pHouse->Available_Money();

	features[0] = ClampInt8(credits / 250);

	features[1] = ClampInt8(pHouse->CountResourceGatherers * 16);

	features[2] = ClampInt8(pHouse->CountResourceDestinations * 20);

	features[3] = ClampInt8((pHouse->PowerOutput - pHouse->PowerDrain) / 10);

	features[4] = pHouse->HasLowPower() ? -100 : 60;

	features[5] = ClampInt8(pHouse->NumConYards * 32);

	features[6] = ClampInt8(pHouse->NumBarracks * 24);

	features[7] = ClampInt8(pHouse->NumWarFactories * 24);

	features[8] = ClampInt8(pHouse->NumShipyards * 24);

	features[9] = ClampInt8(pHouse->NumAirpads * 24);

	features[10] = ClampInt8(pHouse->NumOrePurifiers * 40);

	features[11] = ClampInt8(pHouse->UnitRepairStations.Count * 40);

	features[12] = ClampInt8(pHouse->CloningVats.Count * 40);

	features[13] = pHouse->SpySatActive ? 100 : 0;

	features[14] = pHouse->IsTiberiumShort ? -80 : 40;

	features[15] = ClampInt8(Unsorted::CurrentFrame / 450);

	// 32..63: Own Force Composition
	features[32] = ClampInt8(pHouse->OwnedInfantry * 4);

	features[33] = ClampInt8(pHouse->OwnedUnits * 6);

	features[34] = ClampInt8(pHouse->OwnedNavy * 8);

	features[35] = ClampInt8(pHouse->OwnedAircraft * 10);

	features[36] = ClampInt8(pHouse->OwnedBuildings * 4);

	features[37] = ClampInt8(pHouse->SideIndex * 32);

	features[38] = ClampInt8(pExt->AIBrain_Archetype * 16);

	// 64..95: Strict Fog-of-War Filtered Enemy Intelligence
	// We ONLY inspect enemy objects that are on unshrouded cells, or houses
	// infiltrated by our Spies.
	int visibleEnemyInfantry = 0;
	int visibleEnemyTanks = 0;
	int visibleEnemyAir = 0;
	int visibleEnemyNavy = 0;
	int visibleEnemyDefenses = 0;
	int visibleEnemyBuildings = 0;
	int visibleEnemiesOnBridge = 0;
	int destroyedBridgesCount = 0;
	int ownParasitedOrControlled = 0;
	int ownEliteUnits = 0;
	int visibleEnemyAirShips = 0;
	int visibleEnemyStealthUnits = 0;
	int visibleEnemyGarrisons = 0;
	int visibleEnemyElites = 0;
	int visibleEnemyArtillery = 0;
	int visibleEnemyMindControllers = 0;
	int visibleEnemyHarvesters = 0;
	int neutralCapturableTechs = 0;
	int emptyUrbanOccupiables = 0;
	int damagedUrbanGarrisons = 0;
	int ownHeavyAndSiegeCount = 0;
	int ownDisguiseAndDetectorUnits = 0;
	int enemySlaveMinersAndOwnOverpowerCoils = 0;
	int ownRadDeployersAndGapStealthUnits = 0;
	int parasiteWarfareScore = 0;
	int grinderAndMindCapturedUnits = 0;
	int empUnitsAndEMPedEnemies = 0;
	int tankBunkersAndAmmoReloadUnits = 0;
	int sniperPoisonChaosAndTiberiumHealers = 0;
	int submarinesAndAntiSubDestroyers = 0;
	int dyingLoadedTransportsCount = 0;
	int ownTemporaledOrIvanBombedCount = 0;
	int occupiedTankBunkersAndDrainedBlds = 0;
	int amphibiousFleetAndBeachheadScore = 0;
	int woundedFrontlineAndFocusTargets = 0;
	int enemyExposedPowerAndFactories = 0;
	int foreignConYardAndCaptureScore = 0;
	int camouflageAmbushAndGroveScore = 0;
	int ownDamagedBuildings = 0;
	int ownDamagedVehicles = 0;
	int ownHarvestersUnderAttack = 0;
	const DWORD ownCountryBit = (pHouse->Type ? (1u << pHouse->Type->ArrayIndex2) : 0);

	const BuildingTypeClass* pPrimaryConYardType
		= (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)) ? pHouse->ConYards.GetItem(0)->Type : nullptr;
	CellStruct earlyHomeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		earlyHomeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();

	const int earlyHomeZone = (earlyHomeCoords != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(earlyHomeCoords, MovementZone::Normal, false)
		: -1;
	int islandCapturableTechs = 0;

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant
			|| pBld->Type->InvisibleInGame)
		{
			continue;
		}

		if (pBld->Type->BridgeRepairHut && MapClass::Instance.IsLinkedBridgeDestroyed(pBld->GetMapCoords()))
			destroyedBridgesCount++;

		if (pBld->Owner && pBld->Owner->IsNeutral() && pBld->Type->Capturable)
		{
			neutralCapturableTechs++;
			if (earlyHomeZone >= 0)
			{
				const int bz
					= MapClass::Instance.GetMovementZoneType(pBld->GetMapCoords(), MovementZone::Normal, false);
				if (bz >= 0 && bz != earlyHomeZone)
				{
					islandCapturableTechs
						+= ((pBld->Type->ProduceCashAmount > 0 || pBld->Type->ProduceCashStartup > 0) ? 2 : 1);
				}
			}
		}

		if (pBld->Type->CanBeOccupied)
		{
			if (pBld->Occupants.Count == 0)
			{
				emptyUrbanOccupiables++;

				if (pBld->Type->Repairable && pBld->GetHealthPercentage() < 0.72)
					damagedUrbanGarrisons++;
			}
			else if (pBld->Occupants.GetItem(0) && pHouse->IsAlliedWith(pBld->Occupants.GetItem(0)->Owner)
				&& pBld->Type->Repairable && pBld->GetHealthPercentage() < 0.75)
			{
				damagedUrbanGarrisons++;
			}
		}

		if (pBld->Owner == pHouse)
		{
			if (pBld->GetHealthPercentage() < 0.75)
				ownDamagedBuildings++;

			if (pBld->Type->Overpowerable && (!pBld->IsOverpowered || pHouse->HasLowPower()))
				enemySlaveMinersAndOwnOverpowerCoils++;

			if (pBld->Type->Grinding)
				grinderAndMindCapturedUnits += 2;

			if (pBld->Type->Bunker)
			{
				if (pBld->BunkerLinkedItem == nullptr)
					tankBunkersAndAmmoReloadUnits += 2;
				else
					occupiedTankBunkersAndDrainedBlds++;
			}

			if (pBld->DrainingMe != nullptr)
				occupiedTankBunkersAndDrainedBlds += 3;

			if (pBld->TemporalTargetingMe != nullptr || pBld->AttachedBomb != nullptr)
				ownTemporaledOrIvanBombedCount += 3;

			if ((pBld->Type->ConstructionYard || pBld->Type->Factory != AbstractType::None)
				&& ((pPrimaryConYardType && pBld->Type->ConstructionYard && pBld->Type != pPrimaryConYardType)
					|| (ownCountryBit != 0 && !pBld->Type->InOwners(ownCountryBit))))
			{
				foreignConYardAndCaptureScore += 3;
			}

			if (IsTunnelNetworkBuilding(pBld->Type))
			{
				amphibiousFleetAndBeachheadScore += 3;

				foreignConYardAndCaptureScore += 2;

				camouflageAmbushAndGroveScore += 3;
			}
		}
	}

	int ownReadySupers = 0;
	int enemyImminentSuperUrgency = 0;

	for (auto const pOtherHouse : HouseClass::Array)
	{
		if (!pOtherHouse || pOtherHouse->IsNeutral() || pOtherHouse->IsObserver() || pOtherHouse->Defeated)
			continue;

		const bool isEnemyH = (pOtherHouse != pHouse && !pHouse->IsAlliedWith(pOtherHouse));

		for (auto const pSuper : pOtherHouse->Supers)
		{
			if (!pSuper || !pSuper->Type || !pSuper->Type->DisableableFromShell)
				continue;

			if (pOtherHouse == pHouse && pSuper->IsReady)
			{
				ownReadySupers++;
			}
			else if (isEnemyH && pSuper->IsPresent)
			{
				const int left = pSuper->RechargeTimer.GetTimeLeft();

				if (pSuper->IsReady || (left > 0 && left <= 900))
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 110);
				else if (left > 0 && left <= 2700)
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 65);
				else
					enemyImminentSuperUrgency = std::max(enemyImminentSuperUrgency, 30);
			}
		}
	}

	pExt->AIBrain_EstimatedEnemyThreat.fill(0);

	CellStruct homeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		homeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();

	const int homeGroundZone = (homeCoords != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false)
		: -1;

	int islandEnemyBuildings = 0;

	for (auto const pTechno : TechnoClass::Array)
	{
		if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
			continue;

		auto const pOwner = pTechno->Owner;

		if (pOwner == pHouse)
		{
			if (pTechno->Veterancy.IsElite())
				ownEliteUnits++;

			if (pTechno->MindControlledBy != nullptr)
				ownParasitedOrControlled++;

			if (auto const pFoot = abstract_cast<FootClass*>(pTechno))
			{
				if (pFoot->ParasiteEatingMe != nullptr)
					ownParasitedOrControlled++;
			}

			if (auto const pOwnU = abstract_cast<UnitClass*>(pTechno))
			{
				if (pOwnU->Type)
				{
					if ((pOwnU->Type->Harvester || pOwnU->Type->ResourceGatherer)
						&& pOwnU->GetHealthPercentage() < 0.85)
					{
						ownHarvestersUnderAttack++;
					}
					else if (pOwnU->Type->Cost >= 700 && pOwnU->GetHealthPercentage() < 0.75)
					{
						ownDamagedVehicles++;
					}

					if (!pOwnU->Type->Harvester && !pOwnU->Type->ResourceGatherer)
					{
						const auto pOWS = pOwnU->Type->GetWeapon(0);
						const auto pOW = pOWS ? pOWS->WeaponType : nullptr;

						if (pOwnU->Type->Cost >= 1000 || (pOW && (pOW->Range >= (8 * 256) || pOW->Spawner)))
							ownHeavyAndSiegeCount++;
					}

					if (pOwnU->Type->GapGenerator)
						ownRadDeployersAndGapStealthUnits++;

					if (pOwnU->Type->Passengers >= 4)
						amphibiousFleetAndBeachheadScore++;

					if (pOwnU->Type->DeploysInto && pOwnU->Type->DeploysInto->ConstructionYard && pPrimaryConYardType
						&& pOwnU->Type->DeploysInto != pPrimaryConYardType)
					{
						foreignConYardAndCaptureScore += 4;
					}

					if (!pOwnU->Type->Naval && !pOwnU->Type->Harvester && !pOwnU->Type->ResourceGatherer
						&& (pOwnU->Type->CanDisguise || pOwnU->Type->PermaDisguise || pOwnU->Type->Cloakable))
					{
						camouflageAmbushAndGroveScore += 3;
					}
				}
			}

			if (pTechno->MindControlledBy != nullptr)
				grinderAndMindCapturedUnits += 2;

			if (pTechno->TemporalTargetingMe != nullptr || pTechno->AttachedBomb != nullptr)
				ownTemporaledOrIvanBombedCount += 3;

			if (pTechno->Passengers.NumPassengers > 0 && pTechno->GetHealthPercentage() < 0.35)
				dyingLoadedTransportsCount += 3;

			if (pTechno->Target != nullptr && pTechno->GetHealthPercentage() < 0.45)
				woundedFrontlineAndFocusTargets++;

			if (auto const pOwnTType = pTechno->GetTechnoType())
			{
				if (pOwnTType->CanDisguise || pOwnTType->DetectDisguise)
					ownDisguiseAndDetectorUnits++;

				if (pOwnTType->ManualReload && pOwnTType->Ammo > 0 && pTechno->Ammo == 0)
					tankBunkersAndAmmoReloadUnits += 2;

				if (pOwnTType->TiberiumHeal && pTechno->GetHealthPercentage() < 0.65)
					sniperPoisonChaosAndTiberiumHealers += 2;

				if (pOwnTType->Naval && (pOwnTType->Cloakable || pOwnTType->Sensors))
					submarinesAndAntiSubDestroyers += 2;

				const auto pOwnWS = pOwnTType->GetWeapon(0);
				const auto pOwnW = pOwnWS ? pOwnWS->WeaponType : nullptr;

				if (pOwnW && pOwnW->Warhead)
				{
					if (pOwnW->Warhead->Parasite || pOwnW->Warhead->Sonic || pOwnW->Damage < 0)
						parasiteWarfareScore++;

					if (pOwnW->Warhead->EMEffect)
						empUnitsAndEMPedEnemies += 2;

					if (pOwnW->Warhead->Poison || pOwnW->Warhead->Psychedelic)
						sniperPoisonChaosAndTiberiumHealers += 2;
				}

				if (auto const pOwnIType = abstract_cast<InfantryTypeClass*>(pOwnTType))
				{
					if (pOwnIType->Deployer && pOwnIType->ImmuneToRadiation)
						ownRadDeployersAndGapStealthUnits++;
				}
			}

			continue;
		}

		if (pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver())
			continue;

		const CellStruct cellCoords = pTechno->GetMapCoords();
		auto const pCell = MapClass::Instance.TryGetCellAt(cellCoords);

		if (!pCell)
			continue;

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);
		const bool spyInfiltrated = (pExt->AIBrain_InfiltratedEnemyMask & (1 << ownerIdx)) != 0;

		const bool cellVisibleToAI = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell) || spyInfiltrated
			|| (pOwner->WhoLastHurtMe == pHouse->ArrayIndex);

		if (!cellVisibleToAI)
		{
			if (auto const pHiddenTType = pTechno->GetTechnoType())
			{
				const bool isHiddenThreat = (pTechno->CloakState != CloakState::Uncloaked || pTechno->Disguised
					|| pHiddenTType->Cloakable || pHiddenTType->CanDisguise || pHiddenTType->PermaDisguise);

				if (isHiddenThreat
					&& (pOwner->OwnedBuildings == 0
						|| (Unsorted::CurrentFrame > 5400
							&& (Unsorted::CurrentFrame - NeuralAI::LastCombatFrame) >= 3600)))
				{
					visibleEnemyStealthUnits += 2;
				}
			}

			continue;
		}

		pExt->AIBrain_ScoutedEnemyMask |= (1 << ownerIdx);

		if (pTechno->OnBridge && pCell->ContainsBridge())
			visibleEnemiesOnBridge++;

		if (pTechno->Veterancy.IsElite())
			visibleEnemyElites++;

		if (auto const pTType = pTechno->GetTechnoType())
		{
			if (pTType->Cloakable || pTechno->CloakState != CloakState::Uncloaked || pTechno->Disguised
				|| pTType->CanDisguise || pTType->PermaDisguise)
			{
				visibleEnemyStealthUnits++;
			}

			const auto pWS = pTType->GetWeapon(0);
			const auto pW = pWS ? pWS->WeaponType : nullptr;

			if (pW && pW->Range >= (8 * 256) && pTechno->WhatAmI() != AbstractType::Building)
				visibleEnemyArtillery++;

			if (pW && pW->Warhead && pW->Warhead->MindControl)
				visibleEnemyMindControllers++;

			if (pTechno->Disguised)
				ownDisguiseAndDetectorUnits += 2;

			if (pTType->Enslaves != nullptr)
				enemySlaveMinersAndOwnOverpowerCoils += 2;

			if (pTechno->IsUnderEMP())
				empUnitsAndEMPedEnemies += 2;

			if (auto const pEIType = abstract_cast<InfantryTypeClass*>(pTType))
			{
				if (pEIType->Engineer || pEIType->Infiltrate || pEIType->C4 || pEIType->Ivan || pEIType->Deployer)
					sniperPoisonChaosAndTiberiumHealers++;
			}

			if (auto const pETUnit = abstract_cast<UnitTypeClass*>(pTType))
			{
				if (pETUnit->Harvester || pETUnit->ResourceGatherer)
					visibleEnemyHarvesters++;
			}
		}

		switch (pTechno->WhatAmI())
		{
		case AbstractType::Infantry:
			visibleEnemyInfantry++;

			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 10;

			break;

		case AbstractType::Unit:
			if (pTechno->GetTechnoType()->Naval)
				visibleEnemyNavy++;
			else
				visibleEnemyTanks++;

			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 25;

			break;

		case AbstractType::Aircraft:
			visibleEnemyAir++;

			if (pTechno->GetTechnoType() && pTechno->GetTechnoType()->Strength >= 600)
				visibleEnemyAirShips++; // Kirov Airship / Floating Disc / Heavy Bomber

			// alert!
			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 30;

			break;

		case AbstractType::Building:
		{
			auto const pBld = static_cast<BuildingClass*>(pTechno);
			auto const pBldType = pBld->Type;
			if (pBldType && (pBldType->Immune || pBldType->Insignificant || pBldType->InvisibleInGame))
				break;

			visibleEnemyBuildings++;

			if (pBld->Occupants.Count > 0)
				visibleEnemyGarrisons++;

			if (pBldType && pBldType->IsBaseDefense)
				visibleEnemyDefenses++;

			if (pBldType
				&& (pBldType->PowerBonus > 0 || pBldType->SuperWeapon != -1 || pBldType->Factory != AbstractType::None
					|| pBldType->ConstructionYard))
			{
				enemyExposedPowerAndFactories++;
			}

			if (pBldType && pBldType->Capturable
				&& (pBldType->ConstructionYard || pBldType->Factory != AbstractType::None || pBldType->TechLevel >= 6))
			{
				foreignConYardAndCaptureScore += (pBldType->ConstructionYard ? 3 : 1);
			}

			if (homeGroundZone >= 0)
			{
				const int enemyZone = MapClass::Instance.GetMovementZoneType(cellCoords, MovementZone::Normal, false);

				if (enemyZone >= 0 && enemyZone != homeGroundZone)
					islandEnemyBuildings++;
			}

			pExt->AIBrain_EstimatedEnemyThreat[ownerIdx] += 20;

			break;
		}

		default:
			break;
		}
	}

	features[64] = ClampInt8(visibleEnemyInfantry * 4);

	features[65] = ClampInt8(visibleEnemyTanks * 6);

	features[66] = ClampInt8(visibleEnemyAir * 12);

	features[67] = ClampInt8(visibleEnemyNavy * 10);

	features[68] = ClampInt8(visibleEnemyDefenses * 8);

	features[69] = ClampInt8(visibleEnemyBuildings * 4);

	features[70] = ClampInt8(visibleEnemiesOnBridge * 35);

	features[71] = ClampInt8(visibleEnemyAirShips * 45);

	features[72] = ClampInt8(visibleEnemyStealthUnits * 30);

	features[73] = ClampInt8(visibleEnemyGarrisons * 25);

	features[74] = ClampInt8(visibleEnemyElites * 25);

	features[75] = ClampInt8(enemyImminentSuperUrgency);

	features[76] = ClampInt8(visibleEnemyArtillery * 25);

	features[77] = ClampInt8(visibleEnemyMindControllers * 35);

	features[78] = ClampInt8(visibleEnemyHarvesters * 25);

	features[79] = ClampInt8(damagedUrbanGarrisons * 30);

	features[80] = ClampInt8(ownHeavyAndSiegeCount * 15);

	features[81] = ClampInt8(ownDisguiseAndDetectorUnits * 20);

	features[82] = ClampInt8(enemySlaveMinersAndOwnOverpowerCoils * 25);

	features[83] = ClampInt8(ownRadDeployersAndGapStealthUnits * 25);

	features[84] = ClampInt8((parasiteWarfareScore + ownParasitedOrControlled * 2) * 20);

	features[85] = ClampInt8(grinderAndMindCapturedUnits * 25);

	features[86] = ClampInt8(empUnitsAndEMPedEnemies * 25);

	features[87] = ClampInt8(tankBunkersAndAmmoReloadUnits * 25);

	features[88] = ClampInt8(sniperPoisonChaosAndTiberiumHealers * 20);

	features[89] = ClampInt8((submarinesAndAntiSubDestroyers + visibleEnemyNavy) * 18);

	features[90] = ClampInt8(dyingLoadedTransportsCount * 35);

	features[91] = ClampInt8(ownTemporaledOrIvanBombedCount * 35);

	features[92] = ClampInt8(occupiedTankBunkersAndDrainedBlds * 30);

	features[93] = ClampInt8((amphibiousFleetAndBeachheadScore * 2 + islandEnemyBuildings * 3 + visibleEnemyNavy) * 15);

	features[94] = ClampInt8((woundedFrontlineAndFocusTargets + visibleEnemyTanks) * 12);

	features[95] = ClampInt8((enemyExposedPowerAndFactories + visibleEnemyArtillery * 2) * 18);

	features[18] = ClampInt8(destroyedBridgesCount * 30);

	features[22] = ClampInt8(ownParasitedOrControlled * 40);

	features[23] = ClampInt8(ownEliteUnits * 20);

	features[24] = ClampInt8((pHouse->OwnedAircraft - (pHouse->NumAirpads * 4)) * 20);

	features[25] = ClampInt8(neutralCapturableTechs * 25);

	features[26] = ClampInt8(emptyUrbanOccupiables * 8);

	features[28] = ClampInt8(ownReadySupers * 60);

	features[29] = ClampInt8(ownDamagedBuildings * 25);

	features[30] = ClampInt8(ownDamagedVehicles * 25);

	features[31] = ClampInt8(ownHarvestersUnderAttack * 60);

	features[16] = GameModeOptionsClass::Instance.MCVRedeploy
		? ClampInt8((pHouse->NumConYards * 25) + (features[20] > 0 ? 40 : 15))
		: 0;

	features[17] = ClampInt8(foreignConYardAndCaptureScore * 20);

	// 96..127: Map Spatial Congestion & Allied Beacons
	int alliedBeaconActive = 0;

	for (auto const pAlly : HouseClass::Array)
	{
		if (!pAlly || pAlly == pHouse || !pHouse->IsAlliedWith(pAlly))
			continue;

		const int allyIdx = pAlly->ArrayIndex;

		if (allyIdx >= 0 && allyIdx < 8)
		{
			for (int b = 0; b < 3; ++b)
			{
				if (auto const pBeacon = BeaconManagerClass::Instance.Beacons[allyIdx][b])
				{
					alliedBeaconActive = 100;

					pExt->AIBrain_AttackTargetCell = CellClass::Coord2Cell(pBeacon->Coord);

					break;
				}
			}
		}
	}

	features[96] = ClampInt8(alliedBeaconActive);

	features[97] = ClampInt8(pHouse->Occupiables.Count * 16);

	// Scan local vs. uncolonized distant Ore/Tiberium patches across all owned
	// ConYards for Expert 6 (Multi-Outpost Expansion)
	CellStruct baseCenter = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	if (pHouse->ConYards.Count > 0)
	{
		baseCenter =

			pHouse->ConYards.GetItem(pHouse->ConYards.Count - 1)->GetMapCoords();
	}

	int localOreCells = 0;
	int expansionOreCells = 0;
	int islandExpansionOreCells = 0;
	int visibleCratesNearby = 0;
	int nearbyTreeGroveCells = 0;
	int uncolonizedIslandLandCells = 0;
	if (baseCenter != CellStruct::Empty)
	{
		for (int dy = -144; dy <= 144; dy += (std::abs(dy) <= 48 ? 3 : (std::abs(dy) <= 96 ? 6 : 8)))
		{
			for (int dx = -144; dx <= 144; dx += (std::abs(dx) <= 48 ? 3 : (std::abs(dx) <= 96 ? 6 : 8)))
			{
				const CellStruct tc { static_cast<short>(baseCenter.X + dx), static_cast<short>(baseCenter.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(tc);
				if (!pCell)
					continue;

				const bool withinNearScan = (std::abs(dx) <= 48 && std::abs(dy) <= 48);
				if (withinNearScan && pCell->GetTerrain(false) != nullptr)
					nearbyTreeGroveCells++;

				if (withinNearScan && pCell->OverlayTypeIndex >= 0
					&& pCell->OverlayTypeIndex < OverlayTypeClass::Array.Count && IsCellVisibleToAI(pHouse, pCell))
				{
					if (auto const pOvl = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
					{
						if (pOvl->Crate)
							visibleCratesNearby++;
					}
				}

				if (pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock)
					continue;

				const int cellZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
				int minSameZoneYardDist = 9999;
				bool hasConYardInZone = false;
				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->Insignificant
						|| pOwnB->Type->InvisibleInGame)
					{
						continue;
					}

					if (!pOwnB->Type->ConstructionYard && !pOwnB->Type->Refinery && !pOwnB->Type->ResourceDestination
						&& !pOwnB->Type->BaseNormal)
					{
						continue;
					}

					const CellStruct yc = pOwnB->GetMapCoords();
					const int yz = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, false);
					if (cellZone >= 0 && yz != cellZone)
						continue;

					if (pOwnB->Type->ConstructionYard)
						hasConYardInZone = true;

					const int d = std::max(std::abs(tc.X - yc.X), std::abs(tc.Y - yc.Y));
					if (d < minSameZoneYardDist)
						minSameZoneYardDist = d;
				}

				if (homeGroundZone >= 0 && cellZone >= 0 && cellZone != homeGroundZone && !hasConYardInZone
					&& minSameZoneYardDist > 24 && pCell->GetBuilding() == nullptr)
				{
					uncolonizedIslandLandCells++;
				}

				if (pCell->GetContainedTiberiumValue() <= 0)
					continue;

				if (minSameZoneYardDist <= 14)
				{
					localOreCells++;
				}
				else if (minSameZoneYardDist > 14)
				{
					expansionOreCells++;
					if (homeGroundZone >= 0 && cellZone >= 0 && cellZone != homeGroundZone && !hasConYardInZone)
						islandExpansionOreCells++;
				}
			}
		}
	}

	const bool lowMoneyAndScarceLocalOre = (pHouse->Available_Money() < 1600 && localOreCells <= 8
		&& (expansionOreCells > 0 || islandExpansionOreCells > 0 || islandCapturableTechs > 0));
	const bool homeIslandCongestedOrDepleted = (uncolonizedIslandLandCells > 12
		&& (localOreCells <= 6 || pHouse->Buildings.Count >= 14 || pHouse->Available_Money() >= 3500
			|| lowMoneyAndScarceLocalOre));
	features[19] = ClampInt8(localOreCells * 4);
	features[20] = ClampInt8(
		expansionOreCells * 3 + (homeIslandCongestedOrDepleted ? 28 : 0) + (lowMoneyAndScarceLocalOre ? 24 : 0));
	features[21] = ClampInt8((islandEnemyBuildings > 0 ? 75 : 0)
		+ ((islandExpansionOreCells > 1 || islandCapturableTechs > 0 || homeIslandCongestedOrDepleted) ? 50 : 0));

	features[27] = ClampInt8(visibleCratesNearby * 45);

	features[98] = ClampInt8(
		camouflageAmbushAndGroveScore * 16 + std::min(40, nearbyTreeGroveCells * 2) + visibleEnemyHarvesters * 12);

	if (features[21] > 0 && features[67] < 40)
		features[67] = 40; // Ensure Shipyard is prioritized whenever an enemy or

	// Ore expansion is on a disconnected island!
	// 128..191: Dynamic Rules & GameModeOptions Context
	features[128] = GameModeOptionsClass::Instance.SWAllowed ? 80 : -80;

	features[129] = GameModeOptionsClass::Instance.Crates ? 60 : -60;

	features[130] = GameModeOptionsClass::Instance.ShortGame ? 80 : 0;

	features[131] = GameModeOptionsClass::Instance.BuildOffAlly ? 60 : 0;

	features[132] = GameModeOptionsClass::Instance.MCVRedeploy ? 60 : -60;

	const bool bridgesDestroyable = RulesClass::Instance && RulesClass::Instance->BridgeStrength < 1000000;

	features[134] = bridgesDestroyable ? 80 : -80;

	const bool multiEngineerActive = RulesClass::Instance && RulesClass::Instance->EngineerCaptureLevel < 0.99;

	features[135] = multiEngineerActive ? 80 : -80;
}

void NeuralAI::RunDeterministicMoE(HouseClass* pHouse, const std::array<int8_t, InputVectorSize>& features,
	std::array<int16_t, OutputVectorSize>& outScores)
{
	outScores.fill(0);

	auto const pExt = HouseExt::Fetch(pHouse);

	const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);

	// Select active specialist expert (0..9) combining situational priors +
	// evolved Expert Routing Bias (Slots 40..47 & 84..85) + learned Faction Memory
	std::array<int, 10> expScores = { 100, 100, 100, 80, 100, 160, 110, 110, 95, 95 };

	if (pHouse->HasLowPower() || pHouse->CountResourceGatherers == 0 || pHouse->CountResourceDestinations == 0
		|| pHouse->NumConYards == 0)
	{
		expScores[0] += 950; // Expert 0: Economy, Silos & Crisis Micro
	}

	if (pHouse->NumBarracks == 0 || pHouse->NumWarFactories == 0
		|| (pHouse->Available_Money() > 2500 && pHouse->OwnedBuildings < 8))
	{
		expScores[1] += 700; // Expert 1: Spatial Tetris Base Architect
	}

	if (features[66] > 24 || features[65] > 36)
		expScores[2] += 420; // Expert 2: Dynamic Counter-Strategy

	if (HouseClass::Array.Count > 4 && (Unsorted::CurrentFrame % 240 < 60))
		expScores[3] += 380; // Expert 3: Multi-Player Opportunism & Diplomacy

	if (pExt->AIBrain_Archetype == 1 || (Unsorted::CurrentFrame % 300 < 60 && pHouse->OwnedUnits >= 4))
		expScores[4] += 360; // Expert 4: Special Operations & Trickster

	if (pHouse->OwnedUnits >= 3 || pHouse->OwnedInfantry >= 5)
		expScores[5] += 300; // Expert 5: Tactical Squad Micro, Kiting & Beacons

	const bool desperateForExpansionResources
		= (features[19] < 28 && (features[20] > 6 || features[21] > 0 || features[25] > 0));
	if (desperateForExpansionResources
		|| ((pHouse->CountResourceDestinations < 3 || features[19] < 28) && features[20] > 10
			&& pHouse->Available_Money() >= 1200))
	{
		expScores[6] += desperateForExpansionResources ? 720 : 480; // Expert 6: Base-Crawling & Ore Outpost Expansion
	}

	if ((features[68] > 16 || pExt->AIBrain_Archetype == 2 || pExt->AIBrain_Archetype == 4)
		&& pHouse->OwnedBuildings >= 7)
	{
		expScores[7] += 440; // Expert 7: Forward Defense Crawling & Chokepoint Siege
	}

	if (pHouse->NumAirpads > 0 || pHouse->OwnedAircraft >= 2 || features[66] > 20 || features[24] > 0
		|| pExt->AIBrain_Archetype == 6)
	{
		expScores[8] += 450; // Expert 8: Airforce & Sky Dominance
	}

	if (features[26] > 0 || features[96] > 0 || features[97] > 0 || features[79] > 0 || pExt->AIBrain_Archetype == 7)
	{
		expScores[9] += 430; // Expert 9: Urban Garrison & Allied Coalition Warfare
	}

	const int curArch = std::clamp(pExt->AIBrain_Archetype, 0, NeuralAI::NumArchetypes - 1);
	int activeExpert = 0;
	int bestExpScore = -999999;

	for (int e = 0; e < NeuralAI::NumSpecialists; ++e)
	{
		int s = expScores[e];

		if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
		{
			s += static_cast<int>(policy.OutputBias[(e < 8) ? (40 + e) : (84 + (e - 8))]);

			const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + curArch * 10 + e) % 512);

			if (policy.Entries[eIdx].VisitCount > 0)
			{
				s += static_cast<int>(
					(e < 8)
						? policy.Entries[eIdx].ExpertScoreDelta[e]
						: policy.Entries[eIdx].BuildCategoryDelta[14 + (e - 8)]) * 6;
			}
		}

		if (s > bestExpScore)
		{
			bestExpScore = s;

			activeExpert = e;
		}
	}

	// Easy difficulty disables Spec-Ops Trickster and advanced tactical kiting
	const int diffIdx = pHouse->GetAIDifficultyIndex();

	if (diffIdx == 2 && (activeExpert == 4 || activeExpert == 5 || activeExpert == 7))
		activeExpert = 2;

	pExt->AIBrain_ActiveExpert = activeExpert;

	// Run INT8 Fixed-Point Matrix-Vector Inference on the mapped Base Model
	if (NeuralAI::BaseMappedView && NeuralAI::BaseMappedSize >= sizeof(AIBaseHeader))
	{
		auto const pHdr = reinterpret_cast<const AIBaseHeader*>(NeuralAI::BaseMappedView);

		uint32_t offset = (activeExpert < 8)
			? pHdr->ExpertWeightsOffset[activeExpert]
			: pHdr->ExtraExpertWeightsOffset[activeExpert - 8];

		if (offset == 0)
			offset = pHdr->ExpertWeightsOffset[activeExpert % 8];

		if (offset == 0)
			offset = pHdr->ExpertWeightsOffset[activeExpert % 6];

		const size_t requiredBytes
			= static_cast<size_t>(offset) + (NeuralAI::InputVectorSize * NeuralAI::OutputVectorSize);

		if (offset >= sizeof(AIBaseHeader) && requiredBytes <= NeuralAI::BaseMappedSize)
		{
			const int8_t* weights = reinterpret_cast<const int8_t*>(NeuralAI::BaseMappedView + offset);

			for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)
			{
				int32_t acc = 0;

				const int8_t* row = weights + (o * NeuralAI::InputVectorSize);

				for (int i = 0; i < NeuralAI::InputVectorSize; ++i)
				{
					acc +=

						static_cast<int32_t>(features[i]) * static_cast<int32_t>(row[i]);
				}

				const int lutIdx = std::clamp((acc >> 10) + 128, 0, 255);

				outScores[o] = static_cast<int16_t>(pHdr->TanhLUT[lutIdx]) * 8;
			}
		}
	}

	// Apply Signed Backpack LoRA delta & Learned Faction Strategy Memory in
	// Skirmish / Headless Training
	if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)
	{
		std::array<int32_t, 16> rankAct = {};

		for (int r = 0; r < 16; ++r)
		{
			int32_t acc = 0;

			for (int i = 0; i < NeuralAI::InputVectorSize; ++i)
			{
				acc += static_cast<int32_t>(features[i]) *

					static_cast<int32_t>(policy.LoRA_A[r * NeuralAI::InputVectorSize + i]);
			}

			rankAct[r] = acc >> 8;
		}

		for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)
		{
			int32_t delta = 0;

			for (int r = 0; r < 16; ++r)
			{
				delta +=

					rankAct[r] * static_cast<int32_t>(policy.LoRA_B[r * NeuralAI::OutputVectorSize + o]);
			}

			outScores[o] = ClampInt16(outScores[o] + (delta >> 7) + policy.OutputBias[o]);
		}

		// Recall learned strategy outcome deltas from Backpack Entries for this
		// Faction + Archetype + Expert
		const uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 80 + curArch * 10 + activeExpert) % 512);

		const auto& memEntry = policy.Entries[entryIdx];

		if (memEntry.VisitCount > 0)
		{
			const int16_t expDelta = (activeExpert < 8)
				? memEntry.ExpertScoreDelta[activeExpert]
				: memEntry.BuildCategoryDelta[14 + (activeExpert - 8)];
			const int16_t learnedBoost = ClampInt16(memEntry.ArchetypeScoreDelta[curArch] * 4 + expDelta * 4);

			for (int o = 16; o < 36; ++o)
				outScores[o] = ClampInt16(outScores[o] + learnedBoost);

			// If this Archetype has a negative win/loss record in the Backpack for
			// this faction, pivot to the highest-scoring alternative Archetype
			// (weighted by Slots 48..52 & 86..88)
			if (memEntry.ArchetypeScoreDelta[curArch] < -18 && (Unsorted::CurrentFrame % 900 < 30))
			{
				int bestAltArch = (curArch + 1) % NeuralAI::NumArchetypes;
				int bestAltScore = -999999;

				for (int a = 0; a < NeuralAI::NumArchetypes; ++a)
				{
					if (a == curArch)
						continue;

					const int altScore = static_cast<int>(outScores[(a < 5) ? (48 + a) : (86 + (a - 5))])
						+ static_cast<int>(memEntry.ArchetypeScoreDelta[a]) * 6;

					if (altScore > bestAltScore)
					{
						bestAltScore = altScore;

						bestAltArch = a;
					}
				}

				pExt->AIBrain_Archetype = bestAltArch;
			}
		}
	}

	// Mid-Match Adaptive Strategy Pivot: If our current strategy is stalled (e.g.
	// high frame count, low army vs strong enemy), pivot to the strongest
	// alternative Archetype (weighted by Slots 48..52) to surprise the opponent!
	if (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame % 2700 == 0) && pHouse->OwnedUnits < 4
		&& features[65] > 20)
	{
		int bestPivotArch = (pExt->AIBrain_Archetype + 1) % NeuralAI::NumArchetypes;
		int bestPivotScore = -999999;

		for (int a = 0; a < NeuralAI::NumArchetypes; ++a)
		{
			if (a == pExt->AIBrain_Archetype)
				continue;

			const int pScore = static_cast<int>(outScores[(a < 5) ? (48 + a) : (86 + (a - 5))]);

			if (pScore > bestPivotScore)
			{
				bestPivotScore = pScore;

				bestPivotArch = a;
			}
		}

		pExt->AIBrain_Archetype = bestPivotArch;
	}

	// Controlled Deterministic Exploration: Inject a small synchronized variation
	// across unit/infantry/aircraft action slots so the AI experiments with
	// alternative unit compositions and tech branches rather than repeating a
	// rigid script
	const int exploreSeed = ((Unsorted::CurrentFrame / 450) * 31 + pHouse->ArrayIndex * 17 + pHouse->SideIndex * 13);

	for (int o = 16; o <= 33; ++o)
	{
		const int exploreBonus = (((exploreSeed + o * 19) % 11) - 5) * 28; // -140 .. +140 exploration window

		outScores[o] = ClampInt16(outScores[o] + exploreBonus);
	}

	// Apply League Archetype & Expert structural priors so the AI executes crisp
	// RTS build orders immediately while integrating all 64 Output Slots:
	const int arch = pExt->AIBrain_Archetype;

	outScores[0] = ClampInt16(outScores[0]
		+ (pHouse->HasLowPower() ? 2200 : ((pHouse->PowerOutput - pHouse->PowerDrain < 60) ? 1050 : -350)));

	outScores[1] = ClampInt16(outScores[1]
		+ ((pHouse->CountResourceDestinations == 0)
				? 2100
				: ((pHouse->CountResourceDestinations < 2)
						? 1150
						: ((pHouse->CountResourceDestinations < 3 && pHouse->Available_Money() > 2200) ? 650
																										: -150))));

	outScores[2] = ClampInt16(outScores[2] + ((pHouse->NumBarracks == 0) ? 1750 : -200));

	outScores[3] = ClampInt16(outScores[3]
		+ ((pHouse->NumWarFactories == 0)
				? 1950
				: ((pHouse->Available_Money() > 3200 && pHouse->NumWarFactories < 2) ? 850 : -100)));

	outScores[4] = ClampInt16(outScores[4] + ((pHouse->NumWarFactories > 0) ? ((arch == 4) ? 1450 : 1150) : 100));

	outScores[5] = ClampInt16(outScores[5] + ((features[65] > 18 || arch == 2) ? 420 : 120));

	outScores[6] = ClampInt16(outScores[6] + ((features[66] > 12) ? 750 : 80));

	outScores[7] = ClampInt16(
		outScores[7] + (((features[67] > 10 || features[21] > 0) && pHouse->NumShipyards == 0) ? 1550 : -200));

	outScores[8] = ClampInt16(
		outScores[8]
		+ ((pHouse->NumWarFactories > 0 && pHouse->NumAirpads == 0)
			? ((arch == 6 || activeExpert == 8) ? 1100 : 550)
			: -150));

	const bool needsDepotForMCVExpansion = (pHouse->NumWarFactories > 0 && pHouse->CountResourceDestinations >= 1
		&& pHouse->UnitRepairStations.Count == 0
		&& (features[19] < 28 || features[20] > 12 || features[21] > 0 || pHouse->Buildings.Count >= 20));
	outScores[9] = ClampInt16(outScores[9]
		+ ((pHouse->NumWarFactories > 0 && pHouse->CountResourceDestinations >= 1
			&& pHouse->UnitRepairStations.Count == 0)
				? (needsDepotForMCVExpansion ? 1650 : 1050)
				: -250));

	outScores[10] = ClampInt16(outScores[10]
		+ ((GameModeOptionsClass::Instance.SWAllowed && pHouse->Available_Money() > (arch == 4 ? 3200 : 4500))
			? (arch == 4 ? 1150 : 850)
			: -600));

	outScores[11] = ClampInt16(outScores[11] + ((pHouse->IsTiberiumShort || features[14] < 0) ? 1450 : -300));

	// Macro Scaling & Tech/Defense/Naval Bias Slots (12..15): 12: Dual Factory
	// Production Scaling (2nd War Factory / 2nd Barracks) 13: Early Tech Center &
	// Battle Lab Rush 14: Base Defense Perimeter Density 15: Naval Supremacy &
	// Amphibious Commitment
	outScores[12] = ClampInt16(outScores[12] + ((pHouse->Available_Money() > 2600) ? 240 : -80));

	outScores[13] = ClampInt16(outScores[13] + ((arch == 2 || arch == 4) ? 280 : 60));

	outScores[14] = ClampInt16(outScores[14] + ((arch == 2 || activeExpert == 7) ? 260 : 40));

	outScores[15] = ClampInt16(outScores[15] + ((features[67] > 10 || features[21] > 0) ? 350 : -120));

	outScores[2] = ClampInt16(outScores[2] + std::clamp(static_cast<int>(outScores[12]) / 3, -150, 300));

	outScores[3] = ClampInt16(outScores[3] + std::clamp(static_cast<int>(outScores[12]) / 2, -200, 450));

	outScores[4] = ClampInt16(outScores[4] + std::clamp(static_cast<int>(outScores[13]) / 2, -250, 500));

	outScores[5] = ClampInt16(outScores[5] + std::clamp(static_cast<int>(outScores[14]) / 2, -250, 450));

	outScores[6] = ClampInt16(outScores[6] + std::clamp(static_cast<int>(outScores[14]) / 3, -200, 350));

	outScores[7] = ClampInt16(outScores[7] + std::clamp(static_cast<int>(outScores[15]) / 2, -250, 500));

	outScores[10] = ClampInt16(outScores[10] + std::clamp(static_cast<int>(outScores[55]) / 2, -300, 500));

	outScores[16] = ClampInt16(outScores[16] + ((arch == 3) ? 600 : 350));

	outScores[17] = ClampInt16(outScores[17] + ((features[65] > 12) ? 750 : 400));

	outScores[18] = ClampInt16(outScores[18]
		+ ((arch == 1 || pHouse->ToCapture >= 0 || (features[18] > 0 && pHouse->OwnedUnits >= 5)) ? 650 : 250));

	outScores[19] = ClampInt16(outScores[19] + ((arch == 1 && pExt->AIBrain_InfiltratedEnemyMask == 0) ? 500 : 100));

	outScores[20] = ClampInt16(outScores[20] + ((features[66] > 12) ? 700 : 300));

	outScores[23] = ClampInt16(outScores[23] + ((arch == 2 || activeExpert == 7) ? 260 : 80));

	// Tactical & Spatial Strategy Output Slots (34..39) driven by Neural MoE +
	// LoRA + Evolved OutputBias + StarCraft/AdvAI Depleting-Spawn Urgency:
	const bool localOreDepleting
		= (features[19] < 28 || (Unsorted::CurrentFrame > 2700 && pHouse->CountResourceDestinations < 3));
	outScores[34]
		= ClampInt16(outScores[34] + ((activeExpert == 6 || localOreDepleting || features[20] > 12) ? 420 : 90));

	outScores[35]
		= ClampInt16(outScores[35] + ((activeExpert == 6 || localOreDepleting || features[20] > 10) ? 440 : 60));

	outScores[36] = ClampInt16(outScores[36] + ((arch == 1 || pHouse->ToCapture >= 0) ? 300 : 80));

	outScores[37] = ClampInt16(outScores[37] + ((arch == 3) ? 260 : ((arch == 2) ? -220 : 0)));

	outScores[38] = ClampInt16(outScores[38] + ((activeExpert == 7 || arch == 2) ? 280 : 40));

	outScores[39]
		= ClampInt16(outScores[39] + ((activeExpert == 0 || activeExpert == 6) ? 220 : ((arch == 3) ? -220 : 0)));

	// Feed evolved tactical/spatial slots into production & expansion priorities
	outScores[1] = ClampInt16(outScores[1]
		+ std::clamp(static_cast<int>(outScores[34]) / 3 + static_cast<int>(outScores[39]) / 2, -300, 550));

	outScores[3] = ClampInt16(outScores[3] + std::clamp(static_cast<int>(outScores[34]) / 3, -200, 400));

	outScores[5] = ClampInt16(outScores[5] + std::clamp(static_cast<int>(outScores[38]) / 2, -250, 500));

	outScores[6] = ClampInt16(outScores[6] + std::clamp(static_cast<int>(outScores[38]) / 3, -200, 350));

	outScores[18] = ClampInt16(outScores[18] + std::clamp(static_cast<int>(outScores[36]), -300, 750));

	// Slot 21: Repair Destroyed Bridge via Engineer + BridgeRepairHut (only when
	// ready to push offensively)
	if (features[18] > 0)
	{
		if (pHouse->OwnedUnits * 6 >= features[65] && pHouse->OwnedUnits >= 4)
			outScores[21] = ClampInt16(outScores[21] + 1200); // We have ground superiority: repair bridge
		else
			outScores[21] = ClampInt16(outScores[21] - 1500); // Outnumbered: keep broken bridge as a moat!
	}
	else
	{
		outScores[21] = -2000;
	}

	// Slot 22: Demolish Intact Bridge (Ambush enemy column on bridge, or
	// scorched-earth defense when outnumbered)
	if (features[134] > 0) // Bridges are destroyable (Immune Bridges GameOption is OFF)
	{
		if (features[70] > 0)
			outScores[22] = ClampInt16(outScores[22] + 1900);
		else if (features[65] > (pHouse->OwnedUnits * 6 + 18))
			outScores[22] = ClampInt16(outScores[22] + 1100);
		else
			outScores[22] = ClampInt16(outScores[22] - 1000);
	}
	else
	{
		outScores[22] = -3000; // Immune Bridges ON: never waste ammo shooting bridges
	}

	outScores[24] = ClampInt16(outScores[24] + 800 + ((arch == 3 || arch == 4) ? 320 : 0)
		- std::clamp(static_cast<int>(outScores[39]) / 2, -450, 350));

	outScores[25] = ClampInt16(outScores[25] + ((features[64] > 20) ? 700 : 300));

	outScores[26] = ClampInt16(outScores[26] + ((features[66] > 12) ? 950 : 250));

	outScores[27] = ClampInt16(outScores[27] + ((features[68] > 16 || arch == 4 || activeExpert == 7) ? 950 : 350));

	outScores[28] = ClampInt16(outScores[28]
		+ ((features[21] > 0) ? 1450 : ((arch == 1 && pHouse->OwnedInfantry >= 3) ? 650 : 150))
		+ std::clamp(static_cast<int>(outScores[53]) / 2, -250, 450));

	outScores[29] = ClampInt16(outScores[29] + 600 + std::clamp(static_cast<int>(outScores[15]) / 2, -200, 450));

	// Slot 30: Multi-Outpost Expansion MCV, Multi-ConYard Production Speed Boost
	// & Nomadic MCV (No hard cap on NumConYards: the neural network weighs
	// whether to build more MCVs for expansion, nomadic hopping, or
	// MultipleFactory building speed vs being moderate based on economy & biases)
	const bool hasUncolonizedIslandExpansion = (features[21] == 50 || features[21] >= 100);
	const int maxConYardsForScore
		= (features[21] > 0) ? (hasUncolonizedIslandExpansion ? 4 : 2) : ((features[20] > 12) ? 3 : 2);
	const bool homeCongestedOrIslandTarget = (hasUncolonizedIslandExpansion
		|| (features[20] > 10
			&& (pHouse->Buildings.Count >= 20 || GetMacroState(pHouse).ConsecutivePlacementFailures > 0)));
	const bool urgentResourceOrIslandExpansion
		= (localOreDepleting && (features[20] > 6 || homeCongestedOrIslandTarget))
		|| (hasUncolonizedIslandExpansion && pHouse->NumConYards < 3);
	const int depleteDiscount = (features[19] < 20 && (features[20] > 6 || homeCongestedOrIslandTarget))
		? 850
		: ((features[19] < 28 || homeCongestedOrIslandTarget) ? 500 : 0);
	const int mcvReserveThreshold = std::clamp(1800 + (pHouse->NumConYards * (features[21] > 0 ? 550 : 850))
			- depleteDiscount - (static_cast<int>(outScores[35]) * 2) - (static_cast<int>(outScores[81]) * 2),
		1100, 4800);
	if (pHouse->NumConYards >= 1 && pHouse->NumConYards < maxConYardsForScore && pHouse->NumWarFactories > 0
		&& (urgentResourceOrIslandExpansion || pHouse->Available_Money() > mcvReserveThreshold)
		&& (features[20] > 6 || localOreDepleting || homeCongestedOrIslandTarget || outScores[35] > 80
			|| outScores[81] > 80 || outScores[30] > 160)
		&& (features[19] < 32 || homeCongestedOrIslandTarget || pHouse->Available_Money() > mcvReserveThreshold + 1000
			|| outScores[30] > 220 || outScores[81] > 120))
	{
		const int perYardPenalty = (features[21] > 0) ? 280 : 380;
		outScores[30] = ClampInt16(outScores[30] + 1050 + (localOreDepleting ? 420 : 0)
			+ (homeCongestedOrIslandTarget ? 400 : 0) + (urgentResourceOrIslandExpansion ? 350 : 0)
			+ std::clamp(static_cast<int>(outScores[35]) + (static_cast<int>(outScores[81]) / 2), -400, 800)
			- ((pHouse->NumConYards - 1) * perYardPenalty));
	}
	else
	{
		outScores[30] = ClampInt16(outScores[30] - 1200);
	}

	// Slot 31: War Factory Harvester Boom (Dynamic Harvester-to-Refinery ratio
	// scaled by Ore/Gem abundance on map)
	const int refCount = std::max(1, pHouse->CountResourceDestinations);
	const bool richResources = (features[19] >= 32 || (features[19] + features[20]) >= 56);
	const bool scarceResources = (features[19] < 12 && features[20] < 18);
	const int desiredHarvesters = scarceResources ? refCount : std::min(10, refCount * (richResources ? 3 : 2));

	if (pHouse->CountResourceGatherers == 0)
		outScores[31] = ClampInt16(outScores[31] + 2600);
	else if (pHouse->CountResourceGatherers < desiredHarvesters)
		outScores[31] = ClampInt16(outScores[31] + (richResources ? 1650 : 1400));
	else
		outScores[31] = ClampInt16(outScores[31] - 1200);

	// Expert 6 & Expert 7 spatial specialization boosts
	if (activeExpert == 6)
	{
		outScores[1] = ClampInt16(outScores[1] + 650);

		outScores[3] = ClampInt16(outScores[3] + 350);

		outScores[31] = ClampInt16(outScores[31] + 300);
	}
	else if (activeExpert == 7)
	{
		outScores[5] = ClampInt16(outScores[5] + 600);

		outScores[6] = ClampInt16(outScores[6] + 450);

		outScores[27] = ClampInt16(outScores[27] + 400);
	}

	outScores[32] = ClampInt16(outScores[32] + ((arch == 6 || activeExpert == 8) ? 750 : 400));

	outScores[33] = ClampInt16(outScores[33] + ((arch == 6 || activeExpert == 8) ? 800 : 450));

	// New Real Tactical & Economic Hands (Slots 56..60):
	// Slot 56: Eco-Raid / Enemy Harvester Hunting & Own Harvester Defense
	outScores[56] = ClampInt16(outScores[56] + (features[78] * 8) + (features[31] * 10)
		+ ((arch == 1 || arch == 3 || activeExpert == 4 || activeExpert == 5) ? 280 : 60));

	// Slot 57: Siege, Urban Garrison Repair/Assault & Combat Deploy/Undeploy
	outScores[57] = ClampInt16(outScores[57] + (features[73] * 10) + (features[79] * 9) + (features[68] * 6)
		+ (features[76] * 6) + ((arch == 4 || activeExpert == 7) ? 320 : 80));

	outScores[27] = ClampInt16(outScores[27] + std::clamp(static_cast<int>(outScores[57]) / 3, -200, 450));

	if (features[79] > 0)
	{
		outScores[18] = ClampInt16(outScores[18] + 320); // Train Engineer to repair damaged urban strongholds!

		outScores[23] = ClampInt16(outScores[23] + 280); // Train Occupier infantry to hold repaired urban buildings!
	}

	// Slot 58: Tactical Kiting, Mind-Control Breaker, Tank Crush & Anti-SW
	// Scatter
	outScores[58] = ClampInt16(outScores[58] + (features[75] * 8) + (features[76] * 7) + (features[77] * 9)
		+ (features[22] * 8) + ((activeExpert == 5) ? 340 : 100));

	// Slot 59: Smart Wrench Building Repair & Map Crate Pickup
	outScores[59] = ClampInt16(
		outScores[59] + (features[27] * 12) + (features[29] * 9) + ((pHouse->Available_Money() > 1400) ? 240 : -150));

	// Slot 60: AA & Sensor Escort, LowPower Defense Toggle & Spawner Standoff
	outScores[60] = ClampInt16(outScores[60] + (features[80] * 10) + (features[66] * 8) + (features[72] * 9)
		+ (pHouse->HasLowPower() ? 360 : 80));

	outScores[26] = ClampInt16(outScores[26] + std::clamp(static_cast<int>(outScores[60]) / 3, -150, 380));

	// Slot 61: Dynamic Spy/Mirage Disguise (DisguiseAs) & Counter-Spy Detection
	// (DetectDisguise)
	outScores[61] = ClampInt16(
		outScores[61] + (features[81] * 10) + (features[72] * 8) + ((arch == 1 || activeExpert == 4) ? 320 : 90));

	// Slot 62: Slave Miner Liberation (Kill SlaveOwner -> Free Slaves) & Tesla
	// Coil Overpowering
	outScores[62]
		= ClampInt16(outScores[62] + (features[82] * 11) + (features[78] * 6) + (pHouse->HasLowPower() ? 280 : 90));

	// Slot 63: Desolator Rad-Field Chokepoint Deploy & Mobile Gap/Stealth Shroud
	// Escort
	outScores[63] = ClampInt16(outScores[63] + (features[83] * 10) + (features[64] * 7) + (features[80] * 6)
		+ ((activeExpert == 5 || activeExpert == 7) ? 260 : 80));

	// Slot 64: Parasite Warfare (Terror Drone/Squid), De-Parasiting
	// (Sonic/RemoveParasite/Repair) & Smart OpenTopped
	outScores[64] = ClampInt16(outScores[64] + (features[84] * 10) + (features[22] * 9)
		+ ((activeExpert == 4 || activeExpert == 5) ? 280 : 90));

	// Slot 65: Grinder Mind-Control Recycling & Emergency Salvage
	outScores[65] = ClampInt16(outScores[65] + (features[85] * 11) + (features[77] * 6)
		+ ((activeExpert == 0 || activeExpert == 4) ? 260 : 80));

	// Slot 66: EMP Pulse Targeting (Warhead->EMEffect) & EMP-Disabled Finisher
	// (IsUnderEMP)
	outScores[66] = ClampInt16(outScores[66] + (features[86] * 11) + (features[65] * 6)
		+ ((activeExpert == 2 || activeExpert == 5) ? 280 : 90));

	// Slot 67: Tank Bunker Garrisoning (Bunker/Bunkerable) & Manual Ammo Reload
	// (ManualReload)
	outScores[67] = ClampInt16(outScores[67] + (features[87] * 10) + (features[68] * 6)
		+ ((activeExpert == 1 || activeExpert == 7) ? 260 : 80));

	// Slot 68: Sniper/Poison Specialist Hunter, Chaos Gas (Psychedelic) &
	// TiberiumHeal Regeneration
	outScores[68] = ClampInt16(outScores[68] + (features[88] * 10) + (features[64] * 6)
		+ ((activeExpert == 2 || activeExpert == 4) ? 280 : 90));

	// Slot 69: Submarine Cloak Ambush & Anti-Sub Destroyer Escort (Naval
	// Cloakable & Sensors)
	outScores[69] = ClampInt16(
		outScores[69] + (features[89] * 10) + (features[67] * 8) + ((arch == 4 || activeExpert == 2) ? 260 : 80));

	// Slot 70: Dying Transport / Battle Fortress Emergency Passenger Unload
	outScores[70] = ClampInt16(outScores[70] + (features[90] * 12) + (features[30] * 6) + 140);

	// Slot 71: Chrono Legionnaire Temporal Erasure Breaker (TemporalTargetingMe
	// -> Kill Owner)
	outScores[71] = ClampInt16(outScores[71] + (features[91] * 12) + 160);

	// Slot 72: Crazy Ivan AttachedBomb Disarm or Kamikaze Charge into Enemy Ranks
	outScores[72] = ClampInt16(outScores[72] + (features[91] * 11) + (features[64] * 5) + 140);

	// Slot 73: Dynamic Tank Bunker Unload (UnloadBunker on Low HP or Offensive
	// Wave)
	outScores[73] = ClampInt16(outScores[73] + (features[92] * 10) + (features[87] * 6) + 120);

	// Slot 74: Anti-Drain Floating Disc Interceptor (DrainingMe -> Priority #1 AA
	// Target)
	outScores[74] = ClampInt16(outScores[74] + (features[92] * 12) + (features[71] * 8) + 150);

	// Slot 75: Naval War Multi-Transport D-Day Convoy, Shuttle Ferry & Island MCV
	// Beachhead Factory
	const int base75
		= (features[21] > 0) ? std::max(static_cast<int>(outScores[75]), 150) : static_cast<int>(outScores[75]);
	outScores[75] = ClampInt16(
		base75 + (features[93] * 11) + (features[21] * 10) + ((activeExpert == 4 || activeExpert == 6) ? 320 : 110));

	if (features[21] > 0 && pHouse->NumConYards >= 1
		&& (pHouse->Available_Money() > 1400 || localOreDepleting || pHouse->Buildings.Count >= 20))
	{
		outScores[30] = ClampInt16(std::max(static_cast<int>(outScores[30]), 250) + 680);
		outScores[28] = ClampInt16(std::max(static_cast<int>(outScores[28]), 200) + 650);
	}

	// Slot 76: Squad Focus-Fire on Wounded In-Range Enemy (Lanchester
	// Target-Fire)
	outScores[76]
		= ClampInt16(outScores[76] + (features[94] * 10) + (features[65] * 6) + ((activeExpert == 5) ? 300 : 120));

	// Slot 77: Wounded Frontline Tank Pull-Back Micro (Step Behind Healthy Tanks)
	outScores[77]
		= ClampInt16(outScores[77] + (features[94] * 9) + (features[30] * 8) + ((activeExpert == 5) ? 280 : 110));

	// Slot 78: High-Ground Cliff Advantage Positioning (CellClass::Level)
	outScores[78]
		= ClampInt16(outScores[78] + (features[80] * 8) + (features[68] * 6) + ((activeExpert == 7) ? 280 : 100));

	// Slot 79: Surgical Enemy Power, Superweapon & Factory Snipe inside Enemy
	// Base
	outScores[79] = ClampInt16(outScores[79] + (features[95] * 11) + (features[75] * 8)
		+ ((activeExpert == 4 || activeExpert == 5) ? 300 : 120));

	// Slot 80: Fast Flanker & Jumpjet Counter-Battery vs Enemy Artillery
	outScores[80] = ClampInt16(outScores[80] + (features[76] * 12) + (features[95] * 7)
		+ ((arch == 3 || activeExpert == 2 || activeExpert == 5) ? 300 : 110));

	// Slot 81: Nomadic MCV Hop, Outpost Drop & Forward Siege Turret-Push
	// (MCVRedeploy)
	outScores[81] = ClampInt16(outScores[81] + (features[16] * 10) + (features[20] * 7) + (features[68] * 5)
		+ ((activeExpert == 6 || activeExpert == 7) ? 320 : 100));

	// Slot 82: Enemy ConYard Heist, Stolen MCV Safe Extraction & Cross-Faction
	// Tech Tree
	outScores[82] = ClampInt16(outScores[82] + (features[17] * 11) + (features[95] * 6)
		+ ((arch == 1 || activeExpert == 1 || activeExpert == 4) ? 320 : 120));

	if (features[17] >= 40)
	{
		outScores[18] = ClampInt16(outScores[18] + 450); // Train Engineers to capture Enemy ConYard/Tech!

		outScores[4] = ClampInt16(outScores[4] + 400); // Climb newly captured Foreign Tech Tree!
	}

	// Slot 83: Mirage Tank & Camouflage Tree-Grove Ambush, Retreat When Spotted &
	// Ambush Zone Rotation
	outScores[83] = ClampInt16(outScores[83] + (features[98] * 9) + (features[78] * 6)
		+ ((arch == 1 || activeExpert == 4 || activeExpert == 5) ? 300 : 110));

	// Expert 8 (Airforce & Sky Dominance), Expert 9 (Urban Garrison & Allied
	// Coalition), and Archetypes 5..7 structural boosts & active routing/bias
	// slots (84..88)
	outScores[84] = ClampInt16(outScores[84] + (features[66] * 6) + (features[24] * 8)
		+ ((activeExpert == 8 || arch == 6) ? 340 : 90));
	outScores[85] = ClampInt16(outScores[85] + (features[26] * 8) + (features[96] * 6) + (features[97] * 7)
		+ ((activeExpert == 9 || arch == 7) ? 340 : 90));
	outScores[86] = ClampInt16(outScores[86] + (features[78] * 7) + (features[98] * 6) + ((arch == 5) ? 260 : 40));
	outScores[87] = ClampInt16(outScores[87] + (features[67] * 7) + (features[21] * 7) + ((arch == 6) ? 260 : 40));
	outScores[88] = ClampInt16(outScores[88] + (features[68] * 6) + (features[26] * 6) + ((arch == 7) ? 260 : 40));

	if (activeExpert == 8 || arch == 6)
	{
		outScores[8] = ClampInt16(outScores[8] + 620);
		outScores[32] = ClampInt16(outScores[32] + 550);
		outScores[33] = ClampInt16(outScores[33] + 600);
		outScores[74] = ClampInt16(outScores[74] + 240);

		if (arch == 6 && (features[67] > 10 || features[21] > 0))
		{
			outScores[7] = ClampInt16(outScores[7] + 550);
			outScores[29] = ClampInt16(outScores[29] + 450);
		}
	}

	if (activeExpert == 9 || arch == 7)
	{
		outScores[16] = ClampInt16(outScores[16] + 320);
		outScores[18] = ClampInt16(outScores[18] + 280);
		outScores[23] = ClampInt16(outScores[23] + 450);
		outScores[27] = ClampInt16(outScores[27] + 480);
		outScores[57] = ClampInt16(outScores[57] + 360);
	}

	if (arch == 5)
	{
		outScores[25] = ClampInt16(outScores[25] + 480);
		outScores[37] = ClampInt16(outScores[37] + 320);
		outScores[56] = ClampInt16(outScores[56] + 450);
		outScores[80] = ClampInt16(outScores[80] + 360);
		outScores[83] = ClampInt16(outScores[83] + 340);
	}

	// Slots 89..127 (39 slots) are FREE RESERVE SLOTS kept at 0 until assigned
}

namespace
{
	bool IsAmphibiousOrAirTransport(const TechnoTypeClass* pType)
	{
		if (!pType || pType->Passengers < 4)
			return false;

		return pType->SpeedType == SpeedType::Hover || pType->SpeedType == SpeedType::Amphibious
			|| pType->SpeedType == SpeedType::FloatBeach || pType->SpeedType == SpeedType::Winged
			|| pType->MovementZone == MovementZone::Amphibious
			|| pType->MovementZone == MovementZone::AmphibiousDestroyer
			|| pType->MovementZone == MovementZone::AmphibiousCrusher || pType->MovementZone == MovementZone::Fly
			|| pType->MovementZone == MovementZone::Subterrannean || pType->JumpJet || pType->ConsideredAircraft;
	}

	// Finds a safe coastal/flank cell on a target island (matching
	// targetIslandZone) away from enemy turrets & units
	// Validates Carryall pickup against rulesmd.ini tags (Carryall.Allowed,
	// Carryall.SizeLimit, SizeLimit, Organic, Naval, NonVehicle) and engine
	// Action::Capture
	bool CanCarryallLiftUnit(AircraftClass* pCarryall, UnitClass* pUnit)
	{
		if (!pCarryall || !pUnit || pCarryall->InLimbo || !pCarryall->IsAlive || pUnit->InLimbo || !pUnit->IsAlive)
			return false;

		auto const pCarryType = pCarryall->Type;
		auto const pUnitType = pUnit->Type;
		if (!pCarryType || !pUnitType || !pCarryType->Carryall || pCarryType->Passengers <= 0)
			return false;

		if (pCarryall->Passengers.NumPassengers >= pCarryType->Passengers || pUnit->IsOnCarryall
			|| pUnit->IsIronCurtained())
		{
			return false;
		}

		if (pUnitType->Organic || pUnitType->Naval || pUnitType->NonVehicle)
			return false;

		if (pCarryType->SizeLimit > 0
			&& static_cast<int>(pUnitType->TechnoTypeClass::Size) > static_cast<int>(pCarryType->SizeLimit))
		{
			return false;
		}

		if (auto const pCarryTypeExt = TechnoTypeExt::Fetch(pCarryType))
		{
			if (pCarryTypeExt->Carryall_SizeLimit.isset()
				&& static_cast<int>(pUnitType->TechnoTypeClass::Size) > pCarryTypeExt->Carryall_SizeLimit.Get())
			{
				return false;
			}
		}

		if (auto const pUnitTypeExt = TechnoTypeExt::Fetch(pUnitType))
		{
			if (!pUnitTypeExt->Carryall_Allowed.Get())
				return false;
		}

		return !pUnit->Transporter;
	}

	bool CanHouseFerryTypeAcrossWater(HouseClass* pHouse, const TechnoTypeClass* pFootType)
	{
		if (!pHouse || !pFootType)
			return false;

		if (pFootType->MovementZone == MovementZone::Amphibious
			|| pFootType->MovementZone == MovementZone::AmphibiousDestroyer
			|| pFootType->MovementZone == MovementZone::AmphibiousCrusher
			|| pFootType->MovementZone == MovementZone::Fly || pFootType->MovementZone == MovementZone::Subterrannean
			|| pFootType->MovementZone == MovementZone::Water || pFootType->SpeedType == SpeedType::Hover
			|| pFootType->SpeedType == SpeedType::Winged || pFootType->JumpJet || pFootType->Teleporter
			|| pFootType->Naval)
		{
			return true;
		}

		const int passSize = std::max(1, static_cast<int>(pFootType->Size));
		auto const pAsUnitType = abstract_cast<const UnitTypeClass*>(pFootType);
		const bool isLiftableVehicle
			= (pAsUnitType != nullptr && !pAsUnitType->Organic && !pAsUnitType->Naval && !pAsUnitType->NonVehicle
				&& (!TechnoTypeExt::Fetch(pFootType) || TechnoTypeExt::Fetch(pFootType)->Carryall_Allowed.Get()));

		auto CanCarryallTypeLift = [&](const TechnoTypeClass* pCarryType, bool isCarryallFlag) -> bool
		{
			if (!isCarryallFlag || !isLiftableVehicle || !pCarryType || pCarryType->Passengers <= 0)
				return false;

			if (pCarryType->SizeLimit > 0 && passSize > static_cast<int>(pCarryType->SizeLimit))
				return false;

			if (auto const pCarryExt = TechnoTypeExt::Fetch(pCarryType))
			{
				if (pCarryExt->Carryall_SizeLimit.isset() && passSize > pCarryExt->Carryall_SizeLimit.Get())
					return false;
			}

			return true;
		};

		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
				continue;

			auto const pUT = pU->Type;
			if (IsAmphibiousOrAirTransport(pUT) && static_cast<int>(pUT->SizeLimit) >= passSize
				&& pUT->Passengers >= passSize)
			{
				return true;
			}

			if (TechnoTypeExt::Fetch(pUT) && CanCarryallTypeLift(pUT, TechnoTypeExt::Fetch(pUT)->Carryall))
				return true;
		}

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || pAir->Owner != pHouse || pAir->InLimbo || !pAir->IsAlive || !pAir->Type)
				continue;

			auto const pAT = pAir->Type;
			if (!pAT->Carryall && pAT->Passengers >= passSize && static_cast<int>(pAT->SizeLimit) >= passSize)
				return true;

			if (CanCarryallTypeLift(pAT, pAT->Carryall))
				return true;
		}

		const unsigned int ownHouseBit
			= (pHouse->Type && pHouse->Type->ArrayIndex >= 0 && pHouse->Type->ArrayIndex < 32)
			? (1u << pHouse->Type->ArrayIndex)
			: 0u;
		for (auto const pUT : UnitTypeClass::Array)
		{
			if (!pUT || pUT->TechLevel < 0 || pUT->TechLevel > 10 || pUT->TechLevel > pHouse->TechLevel)
				continue;

			const bool fitsTrans = IsAmphibiousOrAirTransport(pUT) && static_cast<int>(pUT->SizeLimit) >= passSize
				&& pUT->Passengers >= passSize;
			const bool fitsCarry
				= TechnoTypeExt::Fetch(pUT) && CanCarryallTypeLift(pUT, TechnoTypeExt::Fetch(pUT)->Carryall);
			if (!fitsTrans && !fitsCarry)
				continue;

			const bool hasFactoryNow = pUT->Naval ? (pHouse->NumShipyards > 0) : (pHouse->NumWarFactories > 0);
			if (hasFactoryNow && HouseExt::PrerequisitesMet(pHouse, pUT))
				return true;

			if (ownHouseBit != 0u && pUT->InOwners(ownHouseBit) && (hasFactoryNow || pHouse->NumConYards > 0))
				return true;
		}

		for (auto const pAT : AircraftTypeClass::Array)
		{
			if (!pAT || pAT->TechLevel < 0 || pAT->TechLevel > 10 || pAT->TechLevel > pHouse->TechLevel)
				continue;

			if (pHouse->NumAirpads == 0 && pHouse->NumWarFactories == 0)
				continue;

			const bool fitsTrans
				= !pAT->Carryall && pAT->Passengers >= passSize && static_cast<int>(pAT->SizeLimit) >= passSize;
			const bool fitsCarry = CanCarryallTypeLift(pAT, pAT->Carryall);
			if ((fitsTrans || fitsCarry) && HouseExt::PrerequisitesMet(pHouse, pAT))
				return true;
		}

		return false;
	}

	void OrderFootToEnter(FootClass* pFoot, TechnoClass* pEnterTarget)
	{
		if (!pFoot || !pEnterTarget || pFoot->InLimbo || !pFoot->IsAlive || pEnterTarget->InLimbo
			|| !pEnterTarget->IsAlive)
		{
			return;
		}

		if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
		{
			if (pFoot->ContainsLink(pEnterTarget) || pFoot->ArchiveTarget == pEnterTarget
				|| pFoot->Destination == pEnterTarget)
			{
				return;
			}

			// When FootClass::Mission_Enter is driving a large vehicle (e.g. Slave
			// Miner / MCV) onto the boarding cell next to pEnterTarget, the YR engine
			// sets pFoot->Destination to that adjacent CellClass*. Do NOT reset its
			// navigation list or spam SetDestination!
			if (pFoot->Destination != nullptr)
			{
				const CellStruct dc = CellClass::Coord2Cell(pFoot->Destination->GetCoords());
				if (dc.DistanceFrom(pEnterTarget->GetMapCoords()) <= 3.5)
					return;
			}
		}

		// Match EventClass::Execute (MEGA_MISSION, 0x004C73B9-0x004C747C) exact call
		// order: UnitClass::Assign_Destination (0x00741C54) and
		// InfantryClass::Assign_Destination (0x0051AC2A) ONLY establish radio contact
		// (RequestLink + RequestLoading) or ArchiveTarget when CurrentMission ==
		// Mission::Enter or QueuedMission == Mission::Enter BEFORE SetDestination!
		pFoot->ClearMegaMissionData();

		if (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
			|| pFoot->CurrentMission == Mission::None || pFoot->CurrentMission == Mission::Sleep)
		{
			pFoot->ForceMission(Mission::Enter);
		}
		else
		{
			pFoot->QueueMission(Mission::Enter, false);
		}

		pFoot->SetArchiveTarget(nullptr);
		pFoot->ClearNavigationList();
		pFoot->SetTarget(nullptr);
		pFoot->SetDestination(pEnterTarget, true);

		if (pFoot->ReadyToNextMission())
			pFoot->NextMission();
	}

	void OrderFootToAttackMove(FootClass* pFoot, AbstractClass* pDest)
	{
		if (!pFoot || !pDest || pFoot->InLimbo || !pFoot->IsAlive)
			return;

		pFoot->SetTarget(nullptr);
		pFoot->MegaMission = Mission::AttackMove;

		if (pDest->WhatAmI() == AbstractType::Cell)
		{
			pFoot->MegaDestination = pDest;
			pFoot->MegaTarget = nullptr;
		}
		else
		{
			pFoot->MegaDestination = MapClass::Instance.TryGetCellAt(CellClass::Coord2Cell(pDest->GetCoords()));
			pFoot->MegaTarget = pDest;
		}

		pFoot->HaveAttackMoveTarget = false;
		pFoot->SetDestination(pFoot->MegaDestination ? pFoot->MegaDestination : pDest, true);
		pFoot->QueueMission(Mission::Move, true);
	}

	bool HasActiveBoardingPassenger(TechnoClass* pTrans, int transZone)
	{
		if (!pTrans)
			return false;

		if (pTrans->HasAnyLink())
		{
			if (auto const pLinked = pTrans->GetNthLink(0))
			{
				if (pLinked->IsAlive && !pLinked->InLimbo && pLinked->Owner == pTrans->Owner
					&& (pLinked->CurrentMission == Mission::Enter || pLinked->QueuedMission == Mission::Enter))
				{
					return true;
				}
			}
		}

		const CellStruct tc = pTrans->GetMapCoords();

		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU == pTrans || pU->Owner != pTrans->Owner || pU->InLimbo || !pU->IsAlive)
				continue;

			if (pU->CurrentMission != Mission::Enter && pU->QueuedMission != Mission::Enter)
				continue;

			if (pU->ContainsLink(pTrans) || pU->ArchiveTarget == pTrans || pU->Destination == pTrans)
				return true;

			if (pU->Destination != nullptr && pU->GetMapCoords().DistanceFrom(tc) <= 6.0
				&& CellClass::Coord2Cell(pU->Destination->GetCoords()).DistanceFrom(tc) <= 3.0)
			{
				return true;
			}
		}

		for (auto const pI : InfantryClass::Array)
		{
			if (!pI || pI->Owner != pTrans->Owner || pI->InLimbo || !pI->IsAlive)
				continue;

			if (pI->CurrentMission != Mission::Enter && pI->QueuedMission != Mission::Enter)
				continue;

			if (pI->ContainsLink(pTrans) || pI->ArchiveTarget == pTrans || pI->Destination == pTrans)
				return true;

			if (pI->Destination != nullptr && pI->GetMapCoords().DistanceFrom(tc) <= 6.0
				&& CellClass::Coord2Cell(pI->Destination->GetCoords()).DistanceFrom(tc) <= 3.0)
			{
				return true;
			}
		}

		return false;
	}

	bool ClearStaleEnterOrUnreachableCrossIslandMission(FootClass* pFoot, bool canCrossWaterDirectly)
	{
		if (!pFoot || pFoot->InLimbo || !pFoot->IsAlive)
			return false;

		const auto pFootType = pFoot->GetTechnoType();

		if (!pFootType)
			return false;

		const CellStruct footCoords = pFoot->GetMapCoords();
		const int fz = MapClass::Instance.GetMovementZoneType(footCoords, MovementZone::Normal, false);

		if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
		{
			TechnoClass* pEnterT = pFoot->HasAnyLink()
				? pFoot->GetNthLink(0)
				: abstract_cast<TechnoClass*>(pFoot->ArchiveTarget ? pFoot->ArchiveTarget : pFoot->Destination);
			bool validEnter = false;

			if (pEnterT && pEnterT->IsAlive && !pEnterT->InLimbo)
			{
				const auto pEnterType = pEnterT->GetTechnoType();
				const int ez
					= MapClass::Instance.GetMovementZoneType(pEnterT->GetMapCoords(), MovementZone::Normal, false);

				if (pEnterT->WhatAmI() == AbstractType::Building)
				{
					if (auto const pEnterBld = static_cast<BuildingClass*>(pEnterT))
					{
						const bool sameLandmass = (canCrossWaterDirectly || fz < 0 || ez < 0 || ez == fz);

						if (pEnterBld->Type && pEnterBld->Type->Bunker)
						{
							validEnter = sameLandmass
								&& (pEnterBld->BunkerLinkedItem == nullptr || pEnterBld->BunkerLinkedItem == pFoot);
						}
						else if (pEnterBld->Type && pEnterBld->Type->CanBeOccupied)
						{
							const int maxOcc = std::clamp(pEnterBld->Type->MaxNumberOccupants, 1, 10);
							validEnter = sameLandmass && pEnterBld->Occupants.Count < maxOcc;
						}
						else
						{
							validEnter = sameLandmass;
						}
					}
				}
				else if (pEnterType && pEnterType->Passengers > 0)
				{
					const bool hasRoom = CanPassengerFitInTransport(pEnterT, pFootType);
					const auto pECell = pEnterT->GetCell();
					const bool reachableShore = canCrossWaterDirectly || (fz >= 0 && ez == fz)
						|| pFoot->ContainsLink(pEnterT)
						|| (pECell && pECell->LandType == LandType::Water && pFoot->DistanceFrom(pEnterT) <= 1280);
					validEnter = hasRoom && reachableShore;
				}
			}

			if (validEnter)
				return false;

			if (pFoot->HasAnyLink())
				pFoot->SendToFirstLink(RadioCommand::NotifyUnlink);

			pFoot->ClearMegaMissionData();
			pFoot->SetArchiveTarget(nullptr);
			pFoot->SetDestination(nullptr, true);
			pFoot->ClearNavigationList();
			pFoot->ForceMission(Mission::Guard);

			return true;
		}

		if (!canCrossWaterDirectly && fz >= 0
			&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::AttackMove
				|| pFoot->MegaMission == Mission::AttackMove || pFoot->CurrentMission == Mission::Hunt
				|| pFoot->CurrentMission == Mission::Attack))
		{
			if (pFoot->Target == nullptr
				&& (pFoot->CurrentMission == Mission::Hunt || pFoot->CurrentMission == Mission::AttackMove
					|| pFoot->MegaMission == Mission::AttackMove))
			{
				bool reachedDest = (pFoot->Destination == nullptr || pFoot->DistanceFrom(pFoot->Destination) <= 384);

				if (!reachedDest && pFoot->Owner)
				{
					bool anyEnemyInZone = false;

					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
							|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
						{
							continue;
						}

						if (pEB->Owner == pFoot->Owner || pFoot->Owner->IsAlliedWith(pEB->Owner)
							|| pEB->Owner->IsNeutral() || pEB->Owner->IsObserver() || pEB->Owner->Defeated)
						{
							continue;
						}

						if (MapClass::Instance.GetMovementZoneType(pEB->GetMapCoords(), MovementZone::Normal, false)
							== fz)
						{
							anyEnemyInZone = true;
							break;
						}
					}

					if (!anyEnemyInZone)
					{
						for (auto const pEU : UnitClass::Array)
						{
							if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Naval)
								continue;

							if (pEU->Owner == pFoot->Owner || pFoot->Owner->IsAlliedWith(pEU->Owner)
								|| pEU->Owner->IsNeutral() || pEU->Owner->IsObserver() || pEU->Owner->Defeated)
							{
								continue;
							}

							if (MapClass::Instance.GetMovementZoneType(pEU->GetMapCoords(), MovementZone::Normal, false)
								== fz)
							{
								anyEnemyInZone = true;
								break;
							}
						}
					}

					if (!anyEnemyInZone)
						reachedDest = true;
				}

				if (reachedDest)
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}

			if (pFoot->Destination != nullptr && pFoot->Target == nullptr)
			{
				const CellStruct dc = CellClass::Coord2Cell(pFoot->Destination->GetCoords());
				const int dz = MapClass::Instance.GetMovementZoneType(dc, MovementZone::Normal, false);
				auto const pDCell = MapClass::Instance.TryGetCellAt(dc);

				if ((dz >= 0 && dz != fz) || (pDCell && pDCell->LandType == LandType::Water))
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}

			if (pFoot->Target != nullptr)
			{
				const CellStruct tc = CellClass::Coord2Cell(pFoot->Target->GetCoords());
				const int tz = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
				int wpnRange = 1280;

				if (pFootType->GetWeapon(0) && pFootType->GetWeapon(0)->WeaponType)
					wpnRange = std::max(wpnRange, pFootType->GetWeapon(0)->WeaponType->Range);

				if (tz >= 0 && tz != fz && pFoot->DistanceFrom(pFoot->Target) > (wpnRange + 512))
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetTarget(nullptr);
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}
		}

		return false;
	}

	bool IsTrueBaseStructureTarget(const BuildingTypeClass* pTargetBld)
	{
		if (!pTargetBld)
			return false;

		return (pTargetBld->ConstructionYard || pTargetBld->Refinery || pTargetBld->ResourceGatherer
			|| pTargetBld->ResourceDestination || pTargetBld->Factory != AbstractType::None
			|| pTargetBld->PowerBonus > pTargetBld->PowerDrain || pTargetBld->BaseNormal || pTargetBld->IsBaseDefense
			|| pTargetBld->Radar || pTargetBld->SpySat || pTargetBld->UnitRepair || pTargetBld->SuperWeapon != -1
			|| pTargetBld->Storage > 0 || pTargetBld->Grinding || pTargetBld->Cloning || pTargetBld->OrePurifier
			|| pTargetBld->FactoryPlant || pTargetBld->SecretLab || pTargetBld->Hospital || pTargetBld->Armory
			|| (pTargetBld->TechLevel >= 0 && pTargetBld->TechLevel <= 10));
	}

	CellClass* FindSafeIslandLandingCell(
		HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone, bool preferOre)
	{
		CellClass* pBestLanding = nullptr;
		int bestScore = -999999;
		const int radii[2] = { 36, 68 };
		const int steps[2] = { 2, 3 };
		for (int pass = 0; pass < 2 && !pBestLanding; ++pass)
		{
			const int maxR = radii[pass];
			const int step = steps[pass];
			for (int dy = -maxR; dy <= maxR; dy += step)
			{
				for (int dx = -maxR; dx <= maxR; dx += step)
				{
					const CellStruct cand { static_cast<short>(islandRefCell.X + dx),
						static_cast<short>(islandRefCell.Y + dy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(cand);
					if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock
						|| pCell->GetBuilding())
					{
						continue;
					}

					if (targetIslandZone >= 0)
					{
						const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
						if (cz != targetIslandZone)
							continue;
					}

					if (!pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false)
						|| !pCell->IsClearToMove(
							SpeedType::Hover, true, true, -1, MovementZone::AmphibiousDestroyer, -1, false))
					{
						continue;
					}

					bool nearCoast = (pCell->LandType == LandType::Beach);
					if (!nearCoast)
					{
						const CellStruct neighbors[8] = { { static_cast<short>(cand.X + 1), cand.Y },
							{ static_cast<short>(cand.X - 1), cand.Y }, { cand.X, static_cast<short>(cand.Y + 1) },
							{ cand.X, static_cast<short>(cand.Y - 1) }, { static_cast<short>(cand.X + 2), cand.Y },
							{ static_cast<short>(cand.X - 2), cand.Y }, { cand.X, static_cast<short>(cand.Y + 2) },
							{ cand.X, static_cast<short>(cand.Y - 2) } };
						for (const auto& nc : neighbors)
						{
							if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
							{
								if ((pNC->LandType == LandType::Water || pNC->LandType == LandType::Beach)
									&& std::abs(static_cast<int>(pCell->Level) - static_cast<int>(pNC->Level)) <= 1)
								{
									nearCoast = true;
									break;
								}
							}
						}
					}

					if (!nearCoast)
					{
						const CellStruct ring3[4] = { { static_cast<short>(cand.X + 3), cand.Y },
							{ static_cast<short>(cand.X - 3), cand.Y }, { cand.X, static_cast<short>(cand.Y + 3) },
							{ cand.X, static_cast<short>(cand.Y - 3) } };
						for (const auto& nc3 : ring3)
						{
							if (auto const pNC3 = MapClass::Instance.TryGetCellAt(nc3))
							{
								if ((pNC3->LandType == LandType::Water || pNC3->LandType == LandType::Beach)
									&& std::abs(static_cast<int>(pCell->Level) - static_cast<int>(pNC3->Level)) <= 1)
								{
									nearCoast = true;
									break;
								}
							}
						}
					}

					if (!nearCoast)
						continue;

					int score = 900 - (std::abs(dx) + std::abs(dy)) * 3;
					if (pCell->FirstObject != nullptr)
					{
						score -= 550; // Prefer an unoccupied beach cell so multiple
									// transports/retries spread out!
					}

					bool allImmediateNeighborsDryLand = (pCell->LandType != LandType::Beach);
					int emptyDryNeighbors = 0;
					for (int ndy = -1; ndy <= 1; ++ndy)
					{
						for (int ndx = -1; ndx <= 1; ++ndx)
						{
							if (ndx == 0 && ndy == 0)
								continue;

							const CellStruct adj { static_cast<short>(cand.X + ndx), static_cast<short>(cand.Y + ndy) };
							auto const pAdj = MapClass::Instance.TryGetCellAt(adj);
							if (!pAdj || pAdj->LandType == LandType::Water || pAdj->GetBuilding() != nullptr
								|| !pAdj->IsClearToMove(
									SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
							{
								allImmediateNeighborsDryLand = false;
							}
							else if (pAdj->FirstObject == nullptr)
							{
								emptyDryNeighbors++;
							}
						}
					}

					if (allImmediateNeighborsDryLand)
						score += 350;

					score += emptyDryNeighbors * 55;
					if (emptyDryNeighbors < 2)
					{
						score -= 700; // Heavily penalize congested/blocked exit cells so
									// transports shift to a clear beach zone!
					}

					if (preferOre && pCell->GetContainedTiberiumValue() > 0)
						score += 180;

					for (auto const pBld : BuildingClass::Array)
					{
						if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type
							|| pBld->Type->Insignificant || pBld->Type->InvisibleInGame)
						{
							continue;
						}

						if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral())
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const int dist = std::max(std::abs(cand.X - bc.X), std::abs(cand.Y - bc.Y));
						if (dist <= 11)
						{
							if (pBld->Type->IsBaseDefense)
								score -= (12 - dist) * 140;
							else if (dist <= 5)
								score -= 120;
						}
					}

					for (auto const pEnemyU : UnitClass::Array)
					{
						if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner)
							continue;

						if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner)
							|| pEnemyU->Owner->IsNeutral())
						{
							continue;
						}

						const CellStruct uc = pEnemyU->GetMapCoords();
						const int dist = std::max(std::abs(cand.X - uc.X), std::abs(cand.Y - uc.Y));
						if (dist <= 8)
							score -= 220;
					}

					if (score > bestScore)
					{
						bestScore = score;
						pBestLanding = pCell;
					}
				}
			}
		}

		return pBestLanding ? pBestLanding : MapClass::Instance.TryGetCellAt(islandRefCell);
	}
} // namespace

void NeuralAI::ExecuteTacticalAndSpecOpsMicro(HouseClass* pHouse, const std::array<int16_t, OutputVectorSize>& scores)
{
	auto const FindDeployableFoundationNearCell
		= [](HouseClass* pH, BuildingTypeClass* pBldType, const CellStruct& originCell, int requiredZone,
			UnitClass* pDeployerUnit = nullptr) -> CellStruct
	{
		if (!pH || !pBldType || originCell == CellStruct::Empty)
			return CellStruct::Empty;

		const int w = pBldType->GetFoundationWidth();
		const int h = pBldType->GetFoundationHeight(false);
		const CellStruct deployOffset = (w > 2 || h > 2) ? CellStruct { -1, -1 } : CellStruct { 0, 0 };
		const CellStruct curUnitCell = pDeployerUnit ? pDeployerUnit->GetMapCoords() : CellStruct::Empty;
		for (int r = 0; r <= 28; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;

					const CellStruct cand { static_cast<short>(originCell.X + dx),
						static_cast<short>(originCell.Y + dy) };
					const CellStruct topLeftCell = cand + deployOffset;
					if (!MapClass::Instance.CoordinatesLegal(cand) || !MapClass::Instance.CoordinatesLegal(topLeftCell))
						continue;

					if (requiredZone >= 0)
					{
						const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
						if (cz != requiredZone)
							continue;
					}

					if (pDeployerUnit && pDeployerUnit->Type && cand != curUnitCell)
					{
						auto const pStandCell = MapClass::Instance.TryGetCellAt(cand);
						if (!pStandCell
							|| !pStandCell->IsClearToMove(pDeployerUnit->Type->SpeedType, false, false, -1,
								pDeployerUnit->Type->MovementZone, -1, false))
						{
							continue;
						}
					}

					if (!NeuralAI::IsCellClearedOfExitCorridors(pH, pBldType, topLeftCell))
						continue;

					UnitExt::Deployer = pDeployerUnit;
					const bool canCreate = pBldType->CanCreateHere(topLeftCell, pH);
					bool canExist = canCreate;
					if (canExist)
					{
						for (auto pFoundCell = pBldType->GetFoundationData(true);
							*pFoundCell != CellStruct { 0x7FFF, 0x7FFF }; ++pFoundCell)
						{
							const CellStruct fc = topLeftCell + *pFoundCell;
							auto const pFC = MapClass::Instance.TryGetCellAt(fc);
							if (!pFC || !pFC->CanThisExistHere(pBldType->SpeedType, pBldType, pH))
							{
								canExist = false;
								break;
							}

							for (ObjectClass* pObj = pFC->FirstObject; pObj; pObj = pObj->NextObject)
							{
								if (pObj == pDeployerUnit)
									continue;

								if (auto const pOtherU = abstract_cast<UnitClass*>(pObj))
								{
									if (pOtherU->IsAlive && !pOtherU->InLimbo && pOtherU->Type
										&& (pOtherU->Type->DeploysInto != nullptr
											|| IsAmphibiousOrAirTransport(pOtherU->Type)))
									{
										canExist = false;
										break;
									}
								}
							}

							if (!canExist)
								break;
						}
					}

					UnitExt::Deployer = nullptr;
					if (canExist)
						return cand;
				}
			}
		}

		return CellStruct::Empty;
	};

	auto const CanDeployerUnpackHere = [](HouseClass* pH, BuildingTypeClass* pBldType, UnitClass* pDeployerUnit) -> bool
	{
		if (!pH || !pBldType || !pDeployerUnit)
			return false;

		const int w = pBldType->GetFoundationWidth();
		const int h = pBldType->GetFoundationHeight(false);
		const CellStruct deployOffset = (w > 2 || h > 2) ? CellStruct { -1, -1 } : CellStruct { 0, 0 };
		const CellStruct topLeftCell = pDeployerUnit->GetMapCoords() + deployOffset;
		if (!NeuralAI::IsCellClearedOfExitCorridors(pH, pBldType, topLeftCell))
			return false;

		bool occupiedByOtherDeployerOrTrans = false;
		for (auto pFoundCell = pBldType->GetFoundationData(true); *pFoundCell != CellStruct { 0x7FFF, 0x7FFF };
			++pFoundCell)
		{
			const CellStruct fc = topLeftCell + *pFoundCell;
			auto const pFC = MapClass::Instance.TryGetCellAt(fc);
			if (!pFC)
				return false;

			for (ObjectClass* pObj = pFC->FirstObject; pObj; pObj = pObj->NextObject)
			{
				if (pObj == pDeployerUnit)
					continue;

				if (auto const pFoot = abstract_cast<FootClass*>(pObj))
				{
					if (pFoot->Owner == pH && pFoot->IsAlive && !pFoot->InLimbo)
					{
						auto const pOtherU = abstract_cast<UnitClass*>(pFoot);
						if (pOtherU && pOtherU->Type
							&& (pOtherU->Type->DeploysInto != nullptr || IsAmphibiousOrAirTransport(pOtherU->Type)))
						{
							occupiedByOtherDeployerOrTrans = true;
							if (pOtherU->Type->DeploysInto != nullptr)
							{
								if (pOtherU->UniqueID > pDeployerUnit->UniqueID
									&& pOtherU->CurrentMission != Mission::Move)
								{
									pOtherU->Scatter(CoordStruct::Empty, true, false);
								}
							}
							else if (pOtherU->CurrentMission != Mission::Move)
							{
								pOtherU->Scatter(CoordStruct::Empty, true, false);
							}
						}
						else if (pFoot->CurrentMission != Mission::Move)
						{
							if (!pOtherU || !pOtherU->Type
								|| (!pOtherU->Type->Harvester && !pOtherU->Type->ResourceGatherer))
							{
								pFoot->Scatter(CoordStruct::Empty, true, false);
							}
						}
					}
				}
			}
		}

		if (occupiedByOtherDeployerOrTrans)
			return false;

		return UnitExt::CanDeployIntoBuilding(pDeployerUnit);
	};

	auto const OrderDeployerUnpack = [](UnitClass* pDeployerUnit) -> void
	{
		if (!pDeployerUnit)
			return;

		// UnitClass::TryToDeploy (0x7393DA) strictly aborts if this->Destination !=
		// nullptr!
		const bool hadDest = (pDeployerUnit->Destination != nullptr);
		pDeployerUnit->SetDestination(nullptr, true);
		pDeployerUnit->ClearNavigationList();
		pDeployerUnit->SetTarget(nullptr);
		if (pDeployerUnit->CurrentMission != Mission::Unload)
		{
			pDeployerUnit->QueueMission(Mission::Unload, true);
		}
		else if (hadDest && !pDeployerUnit->IsDeploying)
		{
			pDeployerUnit->MissionStatus = 0;
		}
	};

	auto const FindBestDeployStandCell
		= [&](HouseClass* pH, BuildingTypeClass* pBldType, UnitClass* pDeployerUnit, int requiredZone) -> CellStruct
	{
		if (!pH || !pBldType || !pDeployerUnit)
			return CellStruct::Empty;

		const CellStruct tetrisTopLeft = NeuralAI::FindSpatialTetrisBuildCell(pH, pBldType);
		if (tetrisTopLeft != CellStruct::Empty)
		{
			const CellStruct standOffset
				= (pBldType->GetFoundationWidth() > 2 || pBldType->GetFoundationHeight(false) > 2)
				? CellStruct { 1, 1 }
				: CellStruct { 0, 0 };
			const CellStruct nearTetris = FindDeployableFoundationNearCell(
				pH, pBldType, tetrisTopLeft + standOffset, requiredZone, pDeployerUnit);
			if (nearTetris != CellStruct::Empty)
				return nearTetris;
		}

		return FindDeployableFoundationNearCell(
			pH, pBldType, pDeployerUnit->GetMapCoords(), requiredZone, pDeployerUnit);
	};

	auto const pExt = HouseExt::Fetch(pHouse);
	CellStruct homeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		homeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();

	const int homeGroundZone = (homeCoords != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false)
		: -1;

	// Universal Cross-Island Reachability & Transport Dispatcher:
	// Any ground-only unit or infantry whose action target is on a disconnected
	// landmass (footZone != targetZone) is automatically routed via an
	// Amphibious, Hover, Air, or Carryall Transport to the target island's
	// landing beach, and once unloaded on the target island (footZone ==
	// targetZone), executes its native mission.
	static struct PassengerIslandGoalEntry
	{
		DWORD UniqueID;
		CellStruct TargetCoords;
		int Frame;
	} s_PassengerIslandGoals[64] {};

	auto RecordPassengerIslandGoal = [](DWORD uid, const CellStruct& targetCoords)
	{
		int oldestIdx = 0;
		int oldestFrame = INT_MAX;
		for (int i = 0; i < 64; ++i)
		{
			if (s_PassengerIslandGoals[i].UniqueID == uid || s_PassengerIslandGoals[i].UniqueID == 0)
			{
				s_PassengerIslandGoals[i] = { uid, targetCoords, Unsorted::CurrentFrame };
				return;
			}

			if (s_PassengerIslandGoals[i].Frame < oldestFrame)
			{
				oldestFrame = s_PassengerIslandGoals[i].Frame;
				oldestIdx = i;
			}
		}

		s_PassengerIslandGoals[oldestIdx] = { uid, targetCoords, Unsorted::CurrentFrame };
	};

	auto LookupPassengerIslandGoal = [](DWORD uid) -> CellStruct
	{
		for (int i = 0; i < 64; ++i)
		{
			if (s_PassengerIslandGoals[i].UniqueID == uid
				&& (Unsorted::CurrentFrame - s_PassengerIslandGoals[i].Frame) <= 9000)
			{
				return s_PassengerIslandGoals[i].TargetCoords;
			}
		}

		return CellStruct::Empty;
	};

	auto ClearPassengerIslandGoal = [](DWORD uid)
	{
		for (int i = 0; i < 64; ++i)
		{
			if (s_PassengerIslandGoals[i].UniqueID == uid)
			{
				s_PassengerIslandGoals[i] = { 0, CellStruct::Empty, 0 };
				return;
			}
		}
	};

	auto countPendingBoardersOn = [&](TechnoClass* pT, FootClass* pExcludeFoot, int& outCount, int& outSize)
	{
		outCount = 0;
		outSize = 0;
		for (auto const pCheckU : UnitClass::Array)
		{
			if (!pCheckU || pCheckU->Owner != pHouse || pCheckU->InLimbo || !pCheckU->IsAlive || !pCheckU->Type
				|| pCheckU == pT || pCheckU == pExcludeFoot)
			{
				continue;
			}

			if ((pCheckU->CurrentMission == Mission::Enter || pCheckU->QueuedMission == Mission::Enter)
				&& (pCheckU->ContainsLink(pT) || pCheckU->ArchiveTarget == pT || pCheckU->Destination == pT))
			{
				outCount++;
				outSize += std::max(1, static_cast<int>(pCheckU->Type->TechnoTypeClass::Size));
			}
		}

		for (auto const pCheckI : InfantryClass::Array)
		{
			if (!pCheckI || pCheckI->Owner != pHouse || pCheckI->InLimbo || !pCheckI->IsAlive || !pCheckI->Type
				|| pCheckI == pExcludeFoot)
			{
				continue;
			}

			if ((pCheckI->CurrentMission == Mission::Enter || pCheckI->QueuedMission == Mission::Enter)
				&& (pCheckI->ContainsLink(pT) || pCheckI->ArchiveTarget == pT || pCheckI->Destination == pT))
			{
				outCount++;
				outSize += std::max(1, static_cast<int>(pCheckI->Type->TechnoTypeClass::Size));
			}
		}
	};

	auto CanFootCrossWaterDirectly = [](const FootClass* pF) -> bool
	{
		if (!pF)
			return false;

		auto const pT = pF->GetTechnoType();
		if (!pT)
			return false;

		return (pT->MovementZone == MovementZone::Amphibious || pT->MovementZone == MovementZone::AmphibiousDestroyer
			|| pT->MovementZone == MovementZone::AmphibiousCrusher || pT->MovementZone == MovementZone::Fly
			|| pT->MovementZone == MovementZone::Subterrannean || pT->MovementZone == MovementZone::Water
			|| pT->SpeedType == SpeedType::Hover || pT->SpeedType == SpeedType::Winged || pT->JumpJet || pT->Teleporter
			|| pT->Naval);
	};

	struct PendingCrossIslandOrder
	{
		FootClass* Passenger;
		int PickupZone;
		CellClass* TargetLandingCell;
		int TargetZone;
		int PassengerSize;
	};
	std::vector<PendingCrossIslandOrder> pendingCrossIslandOrders;

	auto UnstickCrossIslandFoot = [&](FootClass* pFoot) -> bool
	{
		if (!pFoot || CanFootCrossWaterDirectly(pFoot))
			return false;

		if (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
			|| pFoot->CurrentMission == Mission::Harvest || pFoot->CurrentMission == Mission::Unload)
		{
			return false;
		}

		AbstractClass* pGoal = pFoot->Destination ? pFoot->Destination : pFoot->Target;
		if (!pGoal)
			return false;

		if (auto const pDestFoot = abstract_cast<FootClass*>(pFoot->Destination))
		{
			if (pDestFoot->Owner == pHouse && pDestFoot->GetTechnoType() && pDestFoot->GetTechnoType()->Passengers > 0)
			{
				const int fz
					= MapClass::Instance.GetMovementZoneType(pFoot->GetMapCoords(), MovementZone::Normal, false);
				const int tz
					= MapClass::Instance.GetMovementZoneType(pDestFoot->GetMapCoords(), MovementZone::Normal, false);
				if (fz == tz)
					return false;
			}
		}

		const int fz = MapClass::Instance.GetMovementZoneType(pFoot->GetMapCoords(), MovementZone::Normal, false);
		const CellStruct gc = CellClass::Coord2Cell(pGoal->GetCoords());
		const int gz = MapClass::Instance.GetMovementZoneType(gc, MovementZone::Normal, false);
		if (fz >= 0 && gz >= 0 && fz != gz)
		{
			if (pFoot->Target && pFoot->CurrentMission == Mission::Attack && pFoot->GetTechnoType())
			{
				const auto pW
					= pFoot->GetTechnoType()->GetWeapon(0) ? pFoot->GetTechnoType()->GetWeapon(0)->WeaponType : nullptr;
				if (pW && pFoot->DistanceFrom(pFoot->Target) <= pW->Range)
					return false;
			}

			pFoot->SetDestination(nullptr, true);
			pFoot->SetTarget(nullptr);
			pFoot->QueueMission(Mission::Guard, true);
			return true;
		}

		return false;
	};

	auto DispatchFootToTargetOrTransport
		= [&](FootClass* pFoot, AbstractClass* pTarget, Mission desiredMission, bool useSetTarget = false) -> bool
	{
		if (!pFoot || !pTarget)
			return false;

		auto const pFootType = pFoot->GetTechnoType();
		if (!pFootType)
			return false;

		const CellStruct footCoords = pFoot->GetMapCoords();
		const int footZone = MapClass::Instance.GetMovementZoneType(footCoords, MovementZone::Normal, false);
		const CellStruct targetCoords = CellClass::Coord2Cell(pTarget->GetCoords());
		const int targetZone = MapClass::Instance.GetMovementZoneType(targetCoords, MovementZone::Normal, false);
		bool canShootFromHere = false;
		if (useSetTarget || desiredMission == Mission::Attack)
		{
			const auto pWpn = pFootType->GetWeapon(0) ? pFootType->GetWeapon(0)->WeaponType : nullptr;
			if (pWpn && pFoot->DistanceFrom(pTarget) <= pWpn->Range)
				canShootFromHere = true;
		}

		if (footZone < 0 || targetZone < 0 || footZone == targetZone || CanFootCrossWaterDirectly(pFoot)
			|| canShootFromHere)
		{
			if (useSetTarget || desiredMission == Mission::Attack)
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetTarget(pTarget);
				pFoot->QueueMission(desiredMission, true);
			}
			else if (desiredMission == Mission::Enter && abstract_cast<TechnoClass*>(pTarget))
			{
				OrderFootToEnter(pFoot, abstract_cast<TechnoClass*>(pTarget));
			}
			else if (desiredMission == Mission::AttackMove)
			{
				OrderFootToAttackMove(pFoot, pTarget);
			}
			else
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetDestination(pTarget, true);
				pFoot->QueueMission(desiredMission, true);
			}

			return true;
		}

		if (!CanHouseFerryTypeAcrossWater(pHouse, pFootType))
			return false;

		RecordPassengerIslandGoal(pFoot->UniqueID, targetCoords);
		const int passSize = std::max(1, static_cast<int>(pFootType->Size));
		if (CellClass* pTargetLanding = FindSafeIslandLandingCell(pHouse, targetCoords, targetZone, false))
			pendingCrossIslandOrders.push_back({ pFoot, footZone, pTargetLanding, targetZone, passSize });

		FootClass* pSameIslandTrans = nullptr;
		int bestTransDist = 999999;
		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type
				|| !IsAmphibiousOrAirTransport(pU->Type))
			{
				continue;
			}

			int pendCnt = 0, pendSz = 0;
			countPendingBoardersOn(pU, pFoot, pendCnt, pendSz);
			if (!CanPassengerFitInTransport(pU, pFootType, pendCnt, pendSz))
				continue;

			auto const pTC = pU->GetCell();
			const int tz = MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false);
			if (tz == footZone && pTC && pTC->LandType != LandType::Water)
			{
				const int d = pFoot->DistanceFrom(pU);
				if (d < bestTransDist)
				{
					bestTransDist = d;
					pSameIslandTrans = pU;
				}
			}
		}

		if (!pSameIslandTrans)
		{
			for (auto const pAirT : AircraftClass::Array)
			{
				if (!pAirT || pAirT->Owner != pHouse || pAirT->InLimbo || !pAirT->IsAlive || !pAirT->Type)
					continue;

				if (!pAirT->Type->Carryall && pAirT->Type->Passengers >= 1 && !pAirT->IsInAir()
					&& pAirT->GetHeight() == 0)
				{
					int pendCnt = 0, pendSz = 0;
					countPendingBoardersOn(pAirT, pFoot, pendCnt, pendSz);
					if (!CanPassengerFitInTransport(pAirT, pFootType, pendCnt, pendSz))
						continue;

					const int tz
						= MapClass::Instance.GetMovementZoneType(pAirT->GetMapCoords(), MovementZone::Normal, false);
					if (tz == footZone)
					{
						pSameIslandTrans = pAirT;
						break;
					}
				}
			}
		}

		if (pSameIslandTrans)
		{
			if (pSameIslandTrans->WhatAmI() == AbstractType::Unit
				&& (pSameIslandTrans->CurrentMission == Mission::Move
					|| (pSameIslandTrans->Destination != nullptr
						&& !HasActiveBoardingPassenger(pSameIslandTrans, footZone))))
			{
				pSameIslandTrans->SetDestination(nullptr, true);
				pSameIslandTrans->ClearNavigationList();
				pSameIslandTrans->ForceMission(Mission::Guard);
			}

			OrderFootToEnter(pFoot, pSameIslandTrans);
			return true;
		}

		if (auto const pPickupBeach = FindSafeIslandLandingCell(pHouse, footCoords, footZone, false))
		{
			pFoot->SetTarget(nullptr);
			if (footCoords.DistanceFrom(pPickupBeach->MapCoords) > 2.5)
			{
				pFoot->SetDestination(pPickupBeach, true);
				pFoot->QueueMission(Mission::Move, true);
			}
			else if (pFoot->CurrentMission != Mission::Guard)
			{
				pFoot->SetDestination(nullptr, true);
				pFoot->QueueMission(Mission::Guard, true);
			}

			return true;
		}

		return false;
	};

	// Find any idle Amphibious/Hover Transport owned by this house that still has
	// passenger room
	FootClass* pLoadingTransport = nullptr;
	int loadingTransMaxPass = 0;
	int loadingTransSizeLimit = 0;
	for (auto const pAirT : AircraftClass::Array)
	{
		if (!pAirT || pAirT->Owner != pHouse || pAirT->InLimbo || !pAirT->IsAlive || !pAirT->Type)
			continue;

		if (!pAirT->Type->Carryall && pAirT->Type->Passengers >= 3 && !pAirT->IsInAir() && pAirT->GetHeight() == 0
			&& pAirT->Passengers.NumPassengers < pAirT->Type->Passengers)
		{
			pLoadingTransport = pAirT;
			loadingTransMaxPass = pAirT->Type->Passengers;
			loadingTransSizeLimit = static_cast<int>(pAirT->Type->SizeLimit);
			break;
		}
	}

	for (auto const pU : UnitClass::Array)
	{
		if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
			continue;

		if (IsAmphibiousOrAirTransport(pU->Type) && pU->Passengers.NumPassengers < pU->Type->Passengers)
		{
			if (!pLoadingTransport || static_cast<int>(pU->Type->SizeLimit) > loadingTransSizeLimit)
			{
				pLoadingTransport = pU;
				loadingTransMaxPass = pU->Type->Passengers;
				loadingTransSizeLimit = static_cast<int>(pU->Type->SizeLimit);
			}

			if (loadingTransSizeLimit >= 6)

				break;
		}
	}

	// Multi-Player Opportunism (Expert 3 Routing Slot 43): Balance focusing
	// high-threat aggressor vs finishing off the most vulnerable enemy house
	const int focusBias = static_cast<int>(scores[43]);

	// Discover uncolonized island Resource (Ore/Gems), buildable Island Land
	// (when home island is full), and Strategic Neutral Tech Building targets
	CellClass* pIslandResourceGoalCell = nullptr;
	int resourceIslandZone = -1;
	CellClass* pIslandTechGoalCell = nullptr;
	int techIslandZone = -1;
	int conYardsOnHomeIsland = 0;
	int refineriesOnHomeIsland = 0;
	bool hasUncolonizedOreOnHomeIsland = false;
	UnitClass* pExtraHomeSlaveMiner = nullptr;
	if (homeGroundZone >= 0 && homeCoords != CellStruct::Empty)
	{
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->Insignificant
				|| pOwnB->Type->InvisibleInGame)
			{
				continue;
			}

			const int bz = MapClass::Instance.GetMovementZoneType(pOwnB->GetMapCoords(), MovementZone::Normal, false);
			if (bz == homeGroundZone)
			{
				if (pOwnB->Type->ConstructionYard)
					conYardsOnHomeIsland++;

				if (pOwnB->Type->Refinery || pOwnB->Type->ResourceGatherer || pOwnB->Type->ResourceDestination)
					refineriesOnHomeIsland++;
			}
		}

		for (auto const pOwnU : UnitClass::Array)
		{
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type)
				continue;

			if (pOwnU->Type->ResourceGatherer && pOwnU->Type->DeploysInto != nullptr)
			{
				const int uz
					= MapClass::Instance.GetMovementZoneType(pOwnU->GetMapCoords(), MovementZone::Normal, false);
				if (uz == homeGroundZone)
				{
					refineriesOnHomeIsland++;
					if (refineriesOnHomeIsland >= 3)
					{
						if (!pExtraHomeSlaveMiner || pOwnU->CurrentMission == Mission::Enter
							|| pOwnU->QueuedMission == Mission::Enter)
						{
							pExtraHomeSlaveMiner = pOwnU;
						}
					}
				}
			}
		}

		const bool homeIslandCongested = (pHouse->Buildings.Count >= 14 || pHouse->Available_Money() >= 3000
			|| GetMacroState(pHouse).ConsecutivePlacementFailures > 0);
		std::vector<int> enemyBaseZones;
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
				|| pEB->Type->InvisibleInGame)
			{
				continue;
			}

			if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
				continue;

			if (pEB->Type->ConstructionYard || pEB->Type->Factory != AbstractType::None || pEB->Type->Refinery
				|| pEB->Type->IsBaseDefense)
			{
				const int ebz
					= MapClass::Instance.GetMovementZoneType(pEB->GetMapCoords(), MovementZone::Normal, false);
				if (ebz >= 0 && std::find(enemyBaseZones.begin(), enemyBaseZones.end(), ebz) == enemyBaseZones.end())
					enemyBaseZones.push_back(ebz);
			}
		}

		int bestResScore = -999999;
		CellClass* pFallbackIslandLandCell = nullptr;
		int fallbackIslandLandZone = -1;
		int bestLandScore = -999999;
		for (int dy = -216; dy <= 216; dy += 4)
		{
			for (int dx = -216; dx <= 216; dx += 4)
			{
				const CellStruct cand { static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(cand);
				if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock)
					continue;

				const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
				if (cz < 0)
					continue;

				const int tibVal = pCell->GetContainedTiberiumValue();
				if (cz == homeGroundZone)
				{
					if (tibVal > 0 && !hasUncolonizedOreOnHomeIsland)
					{
						bool coveredByHomeHub = false;
						for (auto const pBld : pHouse->Buildings)
						{
							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant
								|| pBld->Type->InvisibleInGame)
							{
								continue;
							}

							if (pBld->Type->Refinery || pBld->Type->ConstructionYard || pBld->Type->ResourceDestination)
							{
								const CellStruct bc = pBld->GetMapCoords();
								if (std::max(std::abs(cand.X - bc.X), std::abs(cand.Y - bc.Y)) < 14)
								{
									coveredByHomeHub = true;
									break;
								}
							}
						}

						if (!coveredByHomeHub)
							hasUncolonizedOreOnHomeIsland = true;
					}

					continue;
				}

				// Do not mistake an enemy-occupied base island for a peaceful neutral
				// resource island during archipelago colonization!
				if (std::find(enemyBaseZones.begin(), enemyBaseZones.end(), cz) != enemyBaseZones.end())
					continue;

				bool alreadyColonized = false;
				bool hasOrphanedHubWithoutConYardOrWarFac = false;
				int conYardsInCz = 0;
				int warFacsInCz = 0;
				int hubsInCz = 0;
				int minStepStoneDist = std::abs(dx) + std::abs(dy);
				for (auto const pBld : pHouse->Buildings)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant
						|| pBld->Type->InvisibleInGame)
					{
						continue;
					}

					if (pBld->Type->Refinery || pBld->Type->ConstructionYard || pBld->Type->ResourceDestination
						|| pBld->Type->BaseNormal || pBld->Type->Factory != AbstractType::None)
					{
						const CellStruct bc = pBld->GetMapCoords();
						const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
						if (bz == cz)
						{
							hubsInCz++;
							if (pBld->Type->ConstructionYard)
								conYardsInCz++;

							if (pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval)
								warFacsInCz++;

							continue;
						}

						const int dHub = std::abs(cand.X - bc.X) + std::abs(cand.Y - bc.Y);
						if (dHub < minStepStoneDist)
							minStepStoneDist = dHub;
					}
				}

				if (conYardsInCz >= 1 || warFacsInCz >= 1)
					alreadyColonized = true;
				else if (hubsInCz >= 1)
					hasOrphanedHubWithoutConYardOrWarFac = true;

				if (!alreadyColonized)
				{
					for (auto const pOwnMcv : UnitClass::Array)
					{
						if (!pOwnMcv || pOwnMcv->Owner != pHouse || pOwnMcv->InLimbo || !pOwnMcv->IsAlive
							|| !pOwnMcv->Type || !pOwnMcv->Type->DeploysInto)
						{
							continue;
						}

						if (pOwnMcv->Type->DeploysInto->ConstructionYard || pOwnMcv->Type->DeploysInto->Refinery
							|| pOwnMcv->Type->DeploysInto->BaseNormal)
						{
							if (MapClass::Instance.GetMovementZoneType(
									pOwnMcv->GetMapCoords(), MovementZone::Normal, false)
								== cz)
							{
								alreadyColonized = true;
								break;
							}
						}
					}
				}

				if (alreadyColonized)
					continue;

				if (tibVal > 0)
				{
					const int sc = std::min(tibVal, 600) - minStepStoneDist * 2
						- (hasOrphanedHubWithoutConYardOrWarFac ? 250 : 0);
					if (sc > bestResScore)
					{
						bestResScore = sc;
						resourceIslandZone = cz;
						pIslandResourceGoalCell = pCell;
					}
				}
				else if ((homeIslandCongested || !hasUncolonizedOreOnHomeIsland) && pCell->GetBuilding() == nullptr
					&& pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
				{
					const int lsc = 300 - minStepStoneDist * 2;
					if (lsc > bestLandScore)
					{
						bestLandScore = lsc;
						fallbackIslandLandZone = cz;
						pFallbackIslandLandCell = pCell;
					}
				}
			}
		}

		if (!pIslandResourceGoalCell && pFallbackIslandLandCell)
		{
			pIslandResourceGoalCell = pFallbackIslandLandCell;
			resourceIslandZone = fallbackIslandLandZone;
		}

		if (pIslandResourceGoalCell)
		{
			if (auto const pSafe
				= FindSafeIslandLandingCell(pHouse, pIslandResourceGoalCell->MapCoords, resourceIslandZone, true))
			{
				pIslandResourceGoalCell = pSafe;
			}
		}

		// Orphaned Island Refinery Recovery: If any island has an owned Refinery
		// with 0 Harvesters in its movement zone, either switch that island's War
		// Factory to PrimaryFactory (if one exists on that island) OR ferry a spare
		// Harvester from another island!
		if (Unsorted::CurrentFrame % 90 < 20)
		{
			for (auto const pRefB : pHouse->Buildings)
			{
				if (!pRefB || pRefB->InLimbo || !pRefB->IsAlive || !pRefB->Type)
					continue;

				if (!pRefB->Type->Refinery && !pRefB->Type->ResourceDestination)
					continue;

				const int rz
					= MapClass::Instance.GetMovementZoneType(pRefB->GetMapCoords(), MovementZone::Normal, false);
				if (rz < 0)
					continue;

				int harvsInRz = 0;
				for (auto const pHU : UnitClass::Array)
				{
					if (!pHU || pHU->Owner != pHouse || pHU->InLimbo || !pHU->IsAlive || !pHU->Type)
						continue;

					if ((pHU->Type->Harvester || pHU->Type->ResourceGatherer)
						&& MapClass::Instance.GetMovementZoneType(pHU->GetMapCoords(), MovementZone::Normal, false)
							== rz)
					{
						harvsInRz++;
					}
				}

				if (harvsInRz == 0)
				{
					BuildingClass* pWarFacInRz = nullptr;
					for (auto const pWB : pHouse->Buildings)
					{
						if (pWB && !pWB->InLimbo && pWB->IsAlive && pWB->Type
							&& pWB->Type->Factory == AbstractType::UnitType && !pWB->Type->Naval
							&& MapClass::Instance.GetMovementZoneType(pWB->GetMapCoords(), MovementZone::Normal, false)
								== rz)
						{
							pWarFacInRz = pWB;
							break;
						}
					}

					if (pWarFacInRz)
					{
						if (!pWarFacInRz->IsPrimaryFactory)
							pWarFacInRz->TogglePrimaryFactory();
					}
					else if (auto const pRefCell = pRefB->GetCell())
					{
						for (auto const pSpareH : UnitClass::Array)
						{
							if (!pSpareH || pSpareH->Owner != pHouse || pSpareH->InLimbo || !pSpareH->IsAlive
								|| !pSpareH->Type || !pSpareH->Type->Harvester)
							{
								continue;
							}

							const int uz = MapClass::Instance.GetMovementZoneType(
								pSpareH->GetMapCoords(), MovementZone::Normal, false);
							if (uz < 0 || uz == rz)
								continue;

							int harvsInUz = 0;
							for (auto const pOtherH : UnitClass::Array)
							{
								if (pOtherH && pOtherH->Owner == pHouse && !pOtherH->InLimbo && pOtherH->IsAlive
									&& pOtherH->Type && pOtherH->Type->Harvester
									&& MapClass::Instance.GetMovementZoneType(
										pOtherH->GetMapCoords(), MovementZone::Normal, false)
										== uz)
								{
									harvsInUz++;
								}
							}

							if (harvsInUz >= 2 && CanHouseFerryTypeAcrossWater(pHouse, pSpareH->Type))
							{
								DispatchFootToTargetOrTransport(pSpareH, pRefCell, Mission::Move, false);
								break;
							}
						}
					}

					break;
				}
			}
		}

		// Enemy Island Offensive Beachhead & Reconquest Discovery: Once all neutral
		// islands in the archipelago are colonized (!pIslandResourceGoalCell),
		// allow MCVs & Forward Deployers (Outposts, Mobile War Factories/Barracks,
		// Nanocores) to reconquer the weakest/least-defended enemy island!
		if (!pIslandResourceGoalCell)
		{
			int bestEnemyReconqScore = -999999;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
					|| pEB->Type->InvisibleInGame || pEB->Type->Naval)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
					continue;

				const CellStruct ebc = pEB->GetMapCoords();
				const int ebz = MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, false);
				if (ebz < 0 || ebz == homeGroundZone)
					continue;

				bool weHaveBeachheadThere = false;
				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
						continue;

					if (pOwnB->Type->ConstructionYard || pOwnB->Type->BaseNormal
						|| (pOwnB->Type->Factory != AbstractType::None && !pOwnB->Type->Naval))
					{
						if (MapClass::Instance.GetMovementZoneType(pOwnB->GetMapCoords(), MovementZone::Normal, false)
							== ebz)
						{
							weHaveBeachheadThere = true;
							break;
						}
					}
				}

				if (!weHaveBeachheadThere)
				{
					int enemyTurretsInZone = 0;
					int enemyBldsInZone = 0;
					for (auto const pCheckEB : BuildingClass::Array)
					{
						if (!pCheckEB || pCheckEB->InLimbo || !pCheckEB->IsAlive || !pCheckEB->Owner || !pCheckEB->Type
							|| pCheckEB->Type->Insignificant)
						{
							continue;
						}

						if (pCheckEB->Owner == pHouse || pHouse->IsAlliedWith(pCheckEB->Owner)
							|| pCheckEB->Owner->IsNeutral())
						{
							continue;
						}

						if (MapClass::Instance.GetMovementZoneType(
								pCheckEB->GetMapCoords(), MovementZone::Normal, false)
							== ebz)
						{
							enemyBldsInZone++;
							if (pCheckEB->Type->IsBaseDefense)
								enemyTurretsInZone++;
						}
					}

					const int reconqScore = 1000 - (enemyTurretsInZone * 180) - (enemyBldsInZone * 30);
					if (reconqScore > bestEnemyReconqScore)
					{
						if (auto const pEnemyFlankBeach = FindSafeIslandLandingCell(pHouse, ebc, ebz, false))
						{
							bestEnemyReconqScore = reconqScore;
							pIslandResourceGoalCell = pEnemyFlankBeach;
							resourceIslandZone = ebz;
						}
					}
				}
			}
		}

		int bestTechDist = 999999;
		for (auto const pCivBld : BuildingClass::Array)
		{
			if (!pCivBld || pCivBld->InLimbo || !pCivBld->IsAlive || !pCivBld->Type || !pCivBld->Type->Capturable)
				continue;

			if (!pCivBld->Owner || (!pCivBld->Owner->IsNeutral() && pHouse->IsAlliedWith(pCivBld->Owner)))
				continue;

			const CellStruct bc = pCivBld->GetMapCoords();
			const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
			if (bz < 0 || bz == homeGroundZone)
				continue;

			bool engAlreadyOnIsland = false;
			for (auto const pOwnInf : InfantryClass::Array)
			{
				if (pOwnInf && pOwnInf->Owner == pHouse && !pOwnInf->InLimbo && pOwnInf->IsAlive && pOwnInf->Type
					&& pOwnInf->Type->Engineer)
				{
					if (MapClass::Instance.GetMovementZoneType(pOwnInf->GetMapCoords(), MovementZone::Normal, false)
						== bz)
					{
						engAlreadyOnIsland = true;
						break;
					}
				}
			}

			if (engAlreadyOnIsland)
				continue;

			const int rawDist = std::abs(bc.X - homeCoords.X) + std::abs(bc.Y - homeCoords.Y);
			const bool isCashDerrick = (pCivBld->Type->ProduceCashAmount > 0 || pCivBld->Type->ProduceCashStartup > 0);
			const int d = isCashDerrick ? (rawDist / 2) : rawDist;
			if (d < bestTechDist)
			{
				bestTechDist = d;
				techIslandZone = bz;
				pIslandTechGoalCell = FindSafeIslandLandingCell(pHouse, bc, bz, false);
			}
		}
	}

	int bestEnemyIdx = -1;
	int bestEnemyPriority = -999999;

	for (auto const pOther : HouseClass::Array)
	{
		if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver()
			|| pOther->Defeated)
		{
			continue;
		}

		const int idx = std::clamp(pOther->ArrayIndex, 0, 7);
		int priority = pExt->AIBrain_EstimatedEnemyThreat[idx];

		if (pHouse->WhoLastHurtMe == pOther->ArrayIndex)
			priority += 250;

		if (focusBias > 40 && pOther->OwnedBuildings <= 4)
			priority += std::clamp(focusBias / 2, 40, 260);

		if (priority > bestEnemyPriority)
		{
			bestEnemyPriority = priority;

			bestEnemyIdx = pOther->ArrayIndex;
		}
	}

	if (bestEnemyIdx >= 0)
		pHouse->EnemyHouseIndex = bestEnemyIdx;

	// Tactical Squad Micro: Retreat damaged expensive/Elite/Parasited vehicles to
	// Service Depot, Chrono-Miner Warp escape, Beacons & Allied Base Rescue!
	BuildingClass* pRepairDepot
		= pHouse->UnitRepairStations.Count > 0 ? pHouse->UnitRepairStations.GetItem(0) : nullptr;

	BuildingClass* pGrinder = pHouse->Grinders.Count > 0 ? pHouse->Grinders.GetItem(0) : nullptr;

	CellClass* pBeaconCell = (pExt->AIBrain_AttackTargetCell != CellStruct::Empty)
		? MapClass::Instance.TryGetCellAt(pExt->AIBrain_AttackTargetCell)
		: nullptr;

	// If no manual Beacon is active, check if an ALLIED player's base is under
	// attack (< 85% HP) and rush to defend our Ally!
	if (!pBeaconCell)
	{
		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type)
				continue;

			if (pBld->Owner != pHouse && pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral())
			{
				if (pBld->HasBeenAttacked && pBld->GetHealthPercentage() < 0.85)
				{
					pBeaconCell = pBld->GetCell();

					break;
				}
			}
		}
	}

	// Real Tactical Sensors for Slots 56..60:
	// - Slot 56: Own Harvester under attack & Visible Enemy Harvester to raid
	// - Slot 57: Visible Enemy Garrisoned Building / Base Defense & Urban
	// Strongholds to clear/repair
	// - Slot 58: Enemy MindController holding a unit & Imminent Enemy Superweapon
	// - Slot 59: Visible Map Crate to collect
	// - Slot 60: Leading Heavy/Siege Tank for AA & Sensor Escort
	UnitClass* pAttackedOwnHarvester = nullptr;
	UnitClass* attackedOwnHarvesters[6] {};
	int attackedOwnHarvCount = 0;
	UnitClass* pVisibleEnemyHarvester = nullptr;
	UnitClass* visibleEnemyHarvesters[6] {};
	int visibleEnemyHarvCount = 0;
	UnitClass* pEnemySlaveMiner = nullptr;
	UnitClass* pLeadHeavyTank = nullptr;
	UnitClass* pLeadWarship = nullptr;
	UnitClass* pParasitedOwnUnit = nullptr;
	UnitClass* pBestParasiteVictim = nullptr;
	UnitClass* pEnemyChaosTarget = nullptr;
	InfantryClass* pSampleEnemyInfantry = nullptr;
	InfantryClass* pEnemySpecialistInf = nullptr;
	TechnoClass* pDetectedEnemyInfiltrator = nullptr;
	TechnoClass* detectedEnemyInfiltrators[8] {};
	int detectedInfiltratorCount = 0;
	TechnoClass* pHiddenOrStealthEnemy = nullptr;
	UnitClass* pEnemySubOrShip = nullptr;

	const bool combatStalledForHunt
		= (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame - NeuralAI::LastCombatFrame) >= 3600);

	TechnoClass* pEnemyMindController = nullptr;
	TechnoClass* pEnemyEMPedTarget = nullptr;
	TechnoClass* pEnemyNonEMPedHeavy = nullptr;
	TechnoClass* pEnemyTemporalAttacker = nullptr;
	TechnoClass* pEnemyDrainer = nullptr;
	TechnoClass* pEnemyArtilleryUnit = nullptr;
	UnitClass* pWoundedEnemyVehicle = nullptr;
	BuildingClass* pSurgicalEnemyBld = nullptr;
	BuildingClass* surgicalEnemyBlds[8] {};
	int surgicalEnemyBldCount = 0;
	BuildingClass* pPrioritySiegeBld = nullptr;
	BuildingClass* pEnemyGarrisonToClear = nullptr;
	BuildingClass* enemyGarrisonsToClear[6] {};
	int enemyGarrisonCount = 0;
	BuildingClass* pDamagedUrbanToRepair = nullptr;
	BuildingClass* pGrinderBld = nullptr;
	BuildingClass* pEmptyTankBunker = nullptr;
	BuildingClass* pUnitReloadBld = nullptr;
	BuildingClass* pCapturableEnemyConYard = nullptr;
	CellClass* pVisibleCrateCell = nullptr;
	bool enemySuperImminent = false;
	bool ownEngineerNearEnemyConYard = false;

	for (auto const pOtherH : HouseClass::Array)
	{
		if (!pOtherH || pOtherH == pHouse || pHouse->IsAlliedWith(pOtherH) || pOtherH->IsNeutral()
			|| pOtherH->IsObserver() || pOtherH->Defeated)
		{
			continue;
		}

		for (auto const pSup : pOtherH->Supers)
		{
			if (!pSup || !pSup->Type || !pSup->Type->DisableableFromShell)
				continue;

			if (pSup->IsReady || (pSup->RechargeTimer.GetTimeLeft() > 0 && pSup->RechargeTimer.GetTimeLeft() <= 600))
			{
				enemySuperImminent = true;

				break;
			}
		}

		if (enemySuperImminent)
			break;
	}

	for (auto const pCheckU : UnitClass::Array)
	{
		if (!pCheckU || pCheckU->InLimbo || !pCheckU->IsAlive || !pCheckU->Owner || !pCheckU->Type)
			continue;

		if (pCheckU->MindControlledBy && pCheckU->MindControlledBy->Owner && pCheckU->MindControlledBy->Owner != pHouse
			&& !pHouse->IsAlliedWith(pCheckU->MindControlledBy->Owner) && pCheckU->MindControlledBy->GetCell()
			&& IsCellVisibleToAI(pHouse, pCheckU->MindControlledBy->GetCell()))
		{
			pEnemyMindController = pCheckU->MindControlledBy;
		}

		if (pCheckU->Owner == pHouse)
		{
			if (pCheckU->ParasiteEatingMe != nullptr && !pParasitedOwnUnit)
				pParasitedOwnUnit = pCheckU;

			if (pCheckU->TemporalTargetingMe && pCheckU->TemporalTargetingMe->Owner && !pEnemyTemporalAttacker)
				pEnemyTemporalAttacker = pCheckU->TemporalTargetingMe->Owner;

			if (pCheckU->Type->Naval && pCheckU->Type->Cost >= 1000 && !pLeadWarship)
				pLeadWarship = pCheckU;

			if ((pCheckU->Type->Harvester || pCheckU->Type->ResourceGatherer)
				&& (pCheckU->HasBeenAttacked || pCheckU->GetHealthPercentage() < 0.90))
			{
				if (!pAttackedOwnHarvester)
					pAttackedOwnHarvester = pCheckU;

				if (attackedOwnHarvCount < 6)
					attackedOwnHarvesters[attackedOwnHarvCount++] = pCheckU;
			}
			else if (!pLeadHeavyTank && !pCheckU->Type->Naval && !pCheckU->Type->Harvester
				&& !pCheckU->Type->ResourceGatherer)
			{
				const auto pCWS = pCheckU->Type->GetWeapon(0);
				const auto pCW = pCWS ? pCWS->WeaponType : nullptr;

				if (pCheckU->Type->Cost >= 1000 || (pCW && (pCW->Range >= (8 * 256) || pCW->Spawner)))
					pLeadHeavyTank = pCheckU;
			}
		}
		else if (!pHouse->IsAlliedWith(pCheckU->Owner) && !pCheckU->Owner->IsNeutral())
		{
			if (pCheckU->GetCell() && IsCellVisibleToAI(pHouse, pCheckU->GetCell()))
			{
				if (pCheckU->Type->Harvester || pCheckU->Type->ResourceGatherer)
				{
					if (!pVisibleEnemyHarvester)
						pVisibleEnemyHarvester = pCheckU;

					if (visibleEnemyHarvCount < 6)
						visibleEnemyHarvesters[visibleEnemyHarvCount++] = pCheckU;
				}

				if (!pEnemySlaveMiner && pCheckU->Type->Enslaves != nullptr)
					pEnemySlaveMiner = pCheckU;

				if (!pBestParasiteVictim && pCheckU->Type->Parasiteable && !pCheckU->Type->Organic
					&& pCheckU->ParasiteEatingMe == nullptr && pCheckU->Type->Cost >= 700)
				{
					pBestParasiteVictim = pCheckU;
				}

				if (pCheckU->IsUnderEMP() && !pEnemyEMPedTarget)
				{
					pEnemyEMPedTarget = pCheckU;
				}
				else if (!pCheckU->IsUnderEMP() && pCheckU->Type->Cost >= 800 && !pCheckU->Type->Organic
					&& !pEnemyNonEMPedHeavy)
				{
					pEnemyNonEMPedHeavy = pCheckU;
				}

				if (!pEnemyChaosTarget && !pCheckU->Berzerk && !pCheckU->Type->ImmuneToPsionics
					&& pCheckU->Type->Cost >= 600)
				{
					pEnemyChaosTarget = pCheckU;
				}

				if (!pWoundedEnemyVehicle && pCheckU->GetHealthPercentage() < 0.55 && pCheckU->Type->Cost >= 500)
					pWoundedEnemyVehicle = pCheckU;

				const auto pEWS = pCheckU->Type->GetWeapon(0);
				const auto pEW = pEWS ? pEWS->WeaponType : nullptr;

				if (!pEnemyArtilleryUnit && pEW && (pEW->Range >= (8 * 256) || pEW->Spawner) && !pCheckU->Type->Naval)
					pEnemyArtilleryUnit = pCheckU;

				if (pCheckU->Disguised && homeCoords != CellStruct::Empty
					&& pCheckU->GetMapCoords().DistanceFrom(homeCoords) <= 22.0)
				{
					if (!pDetectedEnemyInfiltrator)
						pDetectedEnemyInfiltrator = pCheckU;

					if (detectedInfiltratorCount < 8)
						detectedEnemyInfiltrators[detectedInfiltratorCount++] = pCheckU;
				}

				if (pCheckU->Type->Naval
					&& (!pEnemySubOrShip || pCheckU->Type->Cloakable || pCheckU->CloakState != CloakState::Uncloaked))
				{
					pEnemySubOrShip = pCheckU;
				}

				if (!pCheckU->Type->Naval && !pHiddenOrStealthEnemy
					&& (pCheckU->CloakState != CloakState::Uncloaked || pCheckU->Disguised || pCheckU->Type->Cloakable
						|| pCheckU->Type->CanDisguise || pCheckU->Type->PermaDisguise))
				{
					pHiddenOrStealthEnemy = pCheckU;
				}
			}
			else if (combatStalledForHunt || pCheckU->Owner->OwnedBuildings == 0)
			{
				if (pCheckU->Type->Naval
					&& (!pEnemySubOrShip || pCheckU->Type->Cloakable || pCheckU->CloakState != CloakState::Uncloaked))
				{
					pEnemySubOrShip = pCheckU;
				}
				else if (!pCheckU->Type->Naval && !pHiddenOrStealthEnemy
					&& (pCheckU->CloakState != CloakState::Uncloaked || pCheckU->Disguised || pCheckU->Type->Cloakable
						|| pCheckU->Type->CanDisguise || pCheckU->Type->PermaDisguise
						|| pCheckU->Owner->OwnedBuildings == 0))
				{
					pHiddenOrStealthEnemy = pCheckU;
				}
			}
		}
	}

	for (auto const pCheckInf : InfantryClass::Array)
	{
		if (!pCheckInf || pCheckInf->InLimbo || !pCheckInf->IsAlive || !pCheckInf->Owner || !pCheckInf->Type)
			continue;

		if (pCheckInf->Owner == pHouse || pHouse->IsAlliedWith(pCheckInf->Owner) || pCheckInf->Owner->IsNeutral())
			continue;

		if (!pCheckInf->GetCell())
			continue;

		if (!IsCellVisibleToAI(pHouse, pCheckInf->GetCell()))
		{
			if ((combatStalledForHunt || pCheckInf->Owner->OwnedBuildings == 0) && !pHiddenOrStealthEnemy
				&& (pCheckInf->CloakState != CloakState::Uncloaked || pCheckInf->Disguised || pCheckInf->Type->Cloakable
					|| pCheckInf->Type->CanDisguise || pCheckInf->Type->PermaDisguise
					|| pCheckInf->Owner->OwnedBuildings == 0))
			{
				pHiddenOrStealthEnemy = pCheckInf;
			}

			continue;
		}

		if (!pSampleEnemyInfantry && !pCheckInf->Type->Doggie && !pCheckInf->Type->Slaved)
			pSampleEnemyInfantry = pCheckInf;

		if (!pEnemySpecialistInf
			&& (pCheckInf->Type->Engineer || pCheckInf->Type->Infiltrate || pCheckInf->Type->C4 || pCheckInf->Type->Ivan
				|| pCheckInf->Type->Deployer || pCheckInf->Type->VehicleThief || pCheckInf->Type->Thief))
		{
			pEnemySpecialistInf = pCheckInf;
		}

		if (homeCoords != CellStruct::Empty
			&& (pCheckInf->Disguised || pCheckInf->Type->Infiltrate || pCheckInf->Type->Engineer
				|| pCheckInf->Type->Thief || pCheckInf->Type->VehicleThief || pCheckInf->Type->C4
				|| pCheckInf->Type->Ivan)
			&& pCheckInf->GetMapCoords().DistanceFrom(homeCoords) <= 22.0)
		{
			if (!pDetectedEnemyInfiltrator)
				pDetectedEnemyInfiltrator = pCheckInf;

			if (detectedInfiltratorCount < 8)
				detectedEnemyInfiltrators[detectedInfiltratorCount++] = pCheckInf;
		}
	}

	struct IncomingAirFlank
	{
		FootClass* Target { nullptr };
		int AssignedInterceptors { 0 };
	};

	IncomingAirFlank baseAirFlanks[8] {};
	int baseAirFlankCount = 0;

	if (homeCoords != CellStruct::Empty)
	{
		for (auto const pTech : TechnoClass::Array)
		{
			if (!pTech || pTech->InLimbo || !pTech->IsAlive || !pTech->Owner)
				continue;

			if (pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral())
				continue;

			auto const pAirFoot = abstract_cast<FootClass*>(pTech);

			if (!pAirFoot)
				continue;

			const bool isAirborneThreat = (pAirFoot->WhatAmI() == AbstractType::Aircraft || pAirFoot->IsInAir()
				|| pAirFoot->GetHeight() > 20);

			if (!isAirborneThreat || !pAirFoot->GetCell() || !IsCellVisibleToAI(pHouse, pAirFoot->GetCell()))
				continue;

			const CellStruct ac = pAirFoot->GetMapCoords();
			bool threatensBase = (ac.DistanceFrom(homeCoords) <= 22.0);

			if (!threatensBase)
			{
				for (auto const pOwnB : pHouse->Buildings)
				{
					if (pOwnB && !pOwnB->InLimbo && pOwnB->IsAlive
						&& ac.DistanceFrom(pOwnB->GetMapCoords()) <= 15.0)
					{
						threatensBase = true;
						break;
					}
				}
			}

			if (threatensBase && baseAirFlankCount < 8)
			{
				int interceptors = 0;

				for (auto const pOwnU : UnitClass::Array)
				{
					if (pOwnU && pOwnU->Owner == pHouse && !pOwnU->InLimbo && pOwnU->IsAlive
						&& pOwnU->Target == pAirFoot)
					{
						interceptors++;
					}
				}

				for (auto const pOwnI : InfantryClass::Array)
				{
					if (pOwnI && pOwnI->Owner == pHouse && !pOwnI->InLimbo && pOwnI->IsAlive
						&& pOwnI->Target == pAirFoot)
					{
						interceptors++;
					}
				}

				baseAirFlanks[baseAirFlankCount++] = { pAirFoot, interceptors };
			}
		}
	}

	int bestSiegeScore = -1;

	for (auto const pEBld : BuildingClass::Array)
	{
		if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Type || pEBld->Type->Immune
			|| pEBld->Type->Insignificant || pEBld->Type->InvisibleInGame)
		{
			continue;
		}

		if (pEBld->Owner == pHouse)
		{
			if (pEBld->TemporalTargetingMe && pEBld->TemporalTargetingMe->Owner && !pEnemyTemporalAttacker)
				pEnemyTemporalAttacker = pEBld->TemporalTargetingMe->Owner;

			if (pEBld->DrainingMe && !pEnemyDrainer)
				pEnemyDrainer = pEBld->DrainingMe;

			if (!pGrinderBld && pEBld->Type->Grinding)
				pGrinderBld = pEBld;

			if (!pEmptyTankBunker && pEBld->Type->Bunker && pEBld->BunkerLinkedItem == nullptr)
				pEmptyTankBunker = pEBld;

			if (!pUnitReloadBld && (pEBld->Type->UnitReload || pEBld->Type->UnitRepair || pEBld->Type->Helipad))
				pUnitReloadBld = pEBld;

		}

		// Expert 9: If an occupied urban building or bunker is about to collapse (< 22% HP)
		// under enemy fire, evacuate our garrisoned infantry so they survive!
		if (pEBld->Type->CanBeOccupied && pEBld->Occupants.Count > 0 && pEBld->HasBeenAttacked
			&& pEBld->GetHealthPercentage() < 0.22 && pEBld->Occupants.GetItem(0)
			&& pEBld->Occupants.GetItem(0)->Owner == pHouse)
		{
			pEBld->KickOutUnit(nullptr, CellStruct::Empty);
		}

		if (pEBld->Type->CanBeOccupied && pEBld->Type->Repairable && pEBld->GetHealthPercentage() < 0.72)
		{
			const bool isFriendlyOrEmpty = (pEBld->Occupants.Count == 0)
				|| (pEBld->Occupants.GetItem(0) && pHouse->IsAlliedWith(pEBld->Occupants.GetItem(0)->Owner));

			if (isFriendlyOrEmpty && !pDamagedUrbanToRepair)
				pDamagedUrbanToRepair = pEBld;
		}

		if (!pEBld->Owner || pEBld->Owner == pHouse || pHouse->IsAlliedWith(pEBld->Owner) || pEBld->Owner->IsNeutral())
			continue;

		if (!pEBld->GetCell() || !IsCellVisibleToAI(pHouse, pEBld->GetCell()))
			continue;

		if (pEBld->Type->CanBeOccupied && pEBld->Occupants.Count > 0)
		{
			if (!pEnemyGarrisonToClear)
				pEnemyGarrisonToClear = pEBld;

			if (enemyGarrisonCount < 6)
				enemyGarrisonsToClear[enemyGarrisonCount++] = pEBld;
		}

		if (pEBld->Type->Capturable && pEBld->Type->ConstructionYard && !pCapturableEnemyConYard)
			pCapturableEnemyConYard = pEBld;

		if (scores[79] > 80
			&& (pEBld->Type->SuperWeapon != -1 || pEBld->Type->PowerBonus > 0
				|| pEBld->Type->Factory != AbstractType::None || pEBld->Type->ConstructionYard))
		{
			if (!pSurgicalEnemyBld || pEBld->Type->SuperWeapon != -1
				|| (pEBld->Type->PowerBonus > 0 && pSurgicalEnemyBld->Type->SuperWeapon == -1))
			{
				pSurgicalEnemyBld = pEBld;
			}

			if (surgicalEnemyBldCount < 8)
				surgicalEnemyBlds[surgicalEnemyBldCount++] = pEBld;
		}

		if (scores[57] > 150)
		{
			int sScore = 0;

			if (pEBld->Occupants.Count > 0)
				sScore = 100 + pEBld->Occupants.Count * 15;
			else if (pEBld->Type->IsBaseDefense)
				sScore = 85;

			if (sScore > bestSiegeScore)
			{
				bestSiegeScore = sScore;

				pPrioritySiegeBld = pEBld;
			}
		}
	}

	if (pCapturableEnemyConYard && scores[82] > 60)
	{
		for (auto const pEng : InfantryClass::Array)
		{
			if (pEng && pEng->Owner == pHouse && !pEng->InLimbo && pEng->IsAlive && pEng->Type && pEng->Type->Engineer
				&& pEng->DistanceFrom(pCapturableEnemyConYard) <= 2560)
			{
				ownEngineerNearEnemyConYard = true;

				break;
			}
		}
	}

	// Slot 82 (Stolen ConYard / MCV Human Shield Escort, Anti-Recapture & Island
	// Extraction): Detect if we own a newly captured ConYard or stolen/escaping
	// MCV outside our safe home base (> 16 cells from homeCoords), and identify
	// the most dangerous enemy threat near it (prioritizing enemy Engineers
	// trying to recapture it, then enemy tanks/infantry firing at it) so nearby
	// friendly units & infantry act as Human Shields / Bodyguards!
	TechnoClass* pStolenConYardOrMCV = nullptr;
	TechnoClass* pThreatToStolenAsset = nullptr;
	UnitClass* pStrandedStolenMCVOnIsland = nullptr;

	if (scores[82] > 60 && homeCoords != CellStruct::Empty)
	{
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || !pOwnB->Type->ConstructionYard)
				continue;

			if (pHouse->NumConYards >= 2 && pOwnB != pHouse->ConYards.GetItem(0)
				&& pOwnB->GetMapCoords().DistanceFrom(homeCoords) > 16.0)
			{
				pStolenConYardOrMCV = pOwnB;

				break;
			}
		}

		for (auto const pOwnU : UnitClass::Array)
		{
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type
				|| !pOwnU->Type->DeploysInto || !pOwnU->Type->DeploysInto->ConstructionYard)
			{
				continue;
			}

			const CellStruct mc = pOwnU->GetMapCoords();

			const bool isForeignMCV
				= (pHouse->CountOwnedNow(pOwnU->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1);

			if (mc.DistanceFrom(homeCoords) > 14.0 && (isForeignMCV || pOwnU->HasBeenAttacked))
			{
				pStolenConYardOrMCV = pOwnU;

				const int mz = MapClass::Instance.GetMovementZoneType(mc, MovementZone::Normal, false);

				if (homeGroundZone >= 0 && mz >= 0 && mz != homeGroundZone && isForeignMCV)
					pStrandedStolenMCVOnIsland = pOwnU;

				break;
			}
		}

		if (pStolenConYardOrMCV)
		{
			int bestThreatScore = -1;

			for (auto const pET : TechnoClass::Array)
			{
				if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner || pET->WhatAmI() == AbstractType::Building)
					continue;

				if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
					continue;

				const int dist = pStolenConYardOrMCV->DistanceFrom(pET);

				if (dist > 3584)
					continue;

				int tScore = 4000 - dist;

				if (auto const pEI = abstract_cast<InfantryClass*>(pET))
				{
					if (pEI->Type && pEI->Type->Engineer)
						tScore += 15000;
					else if (pEI->Type && (pEI->Type->C4 || pEI->Type->Ivan))
						tScore += 9000;
				}

				if (pET->Target == pStolenConYardOrMCV)
					tScore += 5000;

				if (tScore > bestThreatScore)
				{
					bestThreatScore = tScore;

					pThreatToStolenAsset = pET;
				}
			}
		}
	}

	if (scores[59] > 80 && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 90 < 25))
	{
		for (int dy = -36; dy <= 36 && !pVisibleCrateCell; dy += 2)
		{
			for (int dx = -36; dx <= 36; dx += 2)
			{
				const CellStruct tc { static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy) };
				auto const pC = MapClass::Instance.TryGetCellAt(tc);

				if (!pC || pC->OverlayTypeIndex < 0 || pC->OverlayTypeIndex >= OverlayTypeClass::Array.Count
					|| !IsCellVisibleToAI(pHouse, pC))
				{
					continue;
				}

				if (auto const pOvl = OverlayTypeClass::Array.GetItem(pC->OverlayTypeIndex))
				{
					if (pOvl->Crate)
					{
						pVisibleCrateCell = pC;

						break;
					}
				}
			}
		}
	}

	// Strict Objective & Combat Non-Interruption Guards:
	// Never abort the target or attack mission of a unit/infantry that is already
	// attacking an enemy objective or actively marching toward an enemy target to
	// pull it into a critical-mass assembly, staging rally, or transport!
	auto IsFootActivelyAttackingOrMovingToEnemy = [pHouse](const FootClass* pFoot) -> bool
	{
		if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
			return false;

		if (pFoot->Target != nullptr)
		{
			if (auto const pTargetTech = abstract_cast<TechnoClass*>(pFoot->Target))
			{
				if (pTargetTech->IsAlive && !pTargetTech->InLimbo)
					return true;
			}
			else if (pFoot->Target->WhatAmI() == AbstractType::Cell)
			{
				return true;
			}
		}

		if (pFoot->CurrentMission == Mission::Attack || pFoot->QueuedMission == Mission::Attack
			|| pFoot->SuspendedMission == Mission::Attack || pFoot->CurrentMission == Mission::Hunt
			|| pFoot->QueuedMission == Mission::Hunt)
		{
			return true;
		}

		if (pFoot->HaveAttackMoveTarget || pFoot->IsFiring || pFoot->HasBeenAttacked)
			return true;

		AbstractClass* pDest = pFoot->Destination
			? pFoot->Destination
			: (pFoot->MegaDestination ? pFoot->MegaDestination : pFoot->MegaTarget);
		if (pDest != nullptr
			&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::Area_Guard
				|| pFoot->CurrentMission == Mission::AttackMove || pFoot->MegaMission == Mission::AttackMove))
		{
			if (auto const pDestTech = abstract_cast<TechnoClass*>(pDest))
			{
				if (pDestTech->IsAlive && !pDestTech->InLimbo && pDestTech->Owner && pDestTech->Owner != pHouse
					&& !pHouse->IsAlliedWith(pDestTech->Owner) && !pDestTech->Owner->IsNeutral())
				{
					return true;
				}
			}

			const CellStruct fc = pFoot->GetMapCoords();
			const CellStruct dc = CellClass::Coord2Cell(pDest->GetCoords());
			if (fc.DistanceFrom(dc) > 2.0)
			{
				for (auto const pEB : BuildingClass::Array)
				{
					if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
						|| pEB->Type->InvisibleInGame)
					{
						continue;
					}

					if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
						|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
					{
						continue;
					}

					if (dc.DistanceFrom(pEB->GetMapCoords()) <= 15.0)
						return true;
				}

				for (auto const pEU : UnitClass::Array)
				{
					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type)
						continue;

					if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral()
						|| pEU->Owner->IsObserver() || pEU->Owner->Defeated)
					{
						continue;
					}

					if (dc.DistanceFrom(pEU->GetMapCoords()) <= 12.0)
						return true;
				}
			}
		}

		return false;
	};

	auto IsFootNearEnemyFrontline = [pHouse](const FootClass* pFoot, int footZone) -> bool
	{
		if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
			return false;

		const CellStruct fc = pFoot->GetMapCoords();
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
				|| pEB->Type->InvisibleInGame)
			{
				continue;
			}

			if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
				|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
			{
				continue;
			}

			const CellStruct ebc = pEB->GetMapCoords();
			if (footZone >= 0 && !pEB->Type->Naval
				&& MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, false) != footZone)
			{
				continue;
			}

			if (fc.DistanceFrom(ebc) <= 18.5)
				return true;
		}

		for (auto const pEU : UnitClass::Array)
		{
			if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type)
				continue;

			if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral()
				|| pEU->Owner->IsObserver() || pEU->Owner->Defeated)
			{
				continue;
			}

			const CellStruct euc = pEU->GetMapCoords();
			if (footZone >= 0 && !pEU->Type->Naval
				&& MapClass::Instance.GetMovementZoneType(euc, MovementZone::Normal, false) != footZone)
			{
				continue;
			}

			if (fc.DistanceFrom(euc) <= 13.0)
				return true;
		}

		return false;
	};

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive)
			continue;

		auto const pType = pUnit->Type;

		if (!pType)
			continue;

		if (pUnit->HasAnyLink())
		{
			if (auto const pLinkedBld = abstract_cast<BuildingClass*>(pUnit->GetNthLink(0)))
			{
				if (pLinkedBld->Type
					&& (pLinkedBld->Type->WeaponsFactory || pLinkedBld->Type->Factory == AbstractType::UnitType))
				{
					if (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr)
					{
						continue; // Never interrupt a vehicle's short exit move while still
								// linked to its War Factory / Shipyard bib!
					}

					pUnit->Scatter(CoordStruct::Empty, true, false);
					continue;
				}
			}
		}

		// Evade radiation clouds (Desolator / Nuke fallout) immediately so units
		// don't melt in place
		if (auto const pUnitCell = pUnit->GetCell())
		{
			if (pUnitCell->GetRadLevel() > 25 && pUnit->CurrentMission != Mission::Move)
				pUnit->Scatter(CoordStruct::Empty, true, false);
		}

		// Slot 58 (Anti-SW Scatter): When an enemy Superweapon is ready/imminent,
		// scatter tightly clumped combat vehicles so a single Nuke/Lightning Storm
		// doesn't wipe the whole army!
		if (enemySuperImminent && scores[58] > 140 && !pType->Harvester && !pType->ResourceGatherer
			&& pUnit->CurrentMission != Mission::Move && (Unsorted::CurrentFrame % 90 < 15))
		{
			const CellStruct uc = pUnit->GetMapCoords();
			int clumpedNeighbors = 0;

			for (auto const pOtherU : UnitClass::Array)
			{
				if (!pOtherU || pOtherU == pUnit || pOtherU->Owner != pHouse || pOtherU->InLimbo || !pOtherU->IsAlive)
					continue;

				const CellStruct oc = pOtherU->GetMapCoords();

				if (std::abs(uc.X - oc.X) <= 1 && std::abs(uc.Y - oc.Y) <= 1)
					clumpedNeighbors++;
			}

			if (clumpedNeighbors >= 3)
			{
				pUnit->Scatter(CoordStruct::Empty, true, false);

				continue;
			}
		}

		// Slot 70 (Dying Transport / OpenTopped Emergency Passenger Unload):
		// If a transport or OpenTopped vehicle carrying passengers on land is about
		// to explode (< 24% HP), immediately Unload so the squad inside survives!
		// Strictly respect NoManualUnload / NoManualEnter (dummy sub-weapon
		// passenger hacks in any mod).
		if (pUnit->Passengers.NumPassengers > 0 && !pType->Naval && !HasNoManualUnload(pType)
			&& !HasNoManualEnter(pType) && pUnit->GetHealthPercentage() < 0.24 && scores[70] > 60)
		{
			if (auto const pUC = pUnit->GetCell())
			{
				if (pUC->LandType != LandType::Water && pUnit->CurrentMission != Mission::Unload)
				{
					pUnit->QueueMission(Mission::Unload, true);

					continue;
				}
			}
		}

		// Slot 72 (Crazy Ivan AttachedBomb Kamikaze Charge): If our vehicle has an
		// attached Ivan bomb and no Repair Depot is nearby, charge straight into
		// the nearest enemy so the bomb explodes in the enemy's ranks instead of
		// ours!
		if (pUnit->AttachedBomb != nullptr && !pRepairDepot && scores[72] > 60)
		{
			TechnoClass* pKamikazeTarget = pSurgicalEnemyBld ? static_cast<TechnoClass*>(pSurgicalEnemyBld)
															: static_cast<TechnoClass*>(pWoundedEnemyVehicle);

			if (pKamikazeTarget && pUnit->Target != pKamikazeTarget)
			{
				pUnit->SetTarget(pKamikazeTarget);

				pUnit->QueueMission(Mission::Attack, true);

				continue;
			}
		}

		// If infected by a Terror Drone / Giant Squid (ParasiteEatingMe), rush
		// immediately to Service Depot to rip it out!
		if (pRepairDepot && pUnit->ParasiteEatingMe != nullptr)
		{
			OrderFootToEnter(pUnit, pRepairDepot);

			continue;
		}

		const int unitZoneForEco
			= MapClass::Instance.GetMovementZoneType(pUnit->GetMapCoords(), MovementZone::Normal, false);
		// Only treat pExtraHomeSlaveMiner as a transport passenger while it is
		// still on homeGroundZone waiting to sail to resourceIslandZone! Once a
		// Slave Miner lands on another island (unitZoneForEco != homeGroundZone),
		// it must work normally via Mission::Harvest and NEVER be forced to deploy
		// on a barren beach (which causes YAREFN to loop in Mission::Selling)!
		const bool isIslandColonizerSlaveMiner = (pType->ResourceGatherer && pType->DeploysInto != nullptr
			&& pUnit == pExtraHomeSlaveMiner && homeGroundZone >= 0 && unitZoneForEco == homeGroundZone
			&& CanHouseFerryTypeAcrossWater(pHouse, pType) && pIslandResourceGoalCell != nullptr
			&& resourceIslandZone != homeGroundZone);
		if ((pType->Harvester || pType->ResourceGatherer) && !isIslandColonizerSlaveMiner)
		{
			const bool canHarvCrossWater = (pType->MovementZone == MovementZone::Amphibious
				|| pType->MovementZone == MovementZone::AmphibiousDestroyer
				|| pType->MovementZone == MovementZone::AmphibiousCrusher || pType->MovementZone == MovementZone::Fly
				|| pType->JumpJet || pType->Teleporter);
			ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, canHarvCrossWater);
			if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter)
				continue;

			// If a Slave Miner (ResourceGatherer && DeploysInto != nullptr) just
			// unloaded on an island beach far from ore, guide it to the nearest ore
			// patch on the SAME landmass and switch to Mission::Harvest so the YR
			// engine natively deploys YAREFN right on the ore patch!
			if (pType->ResourceGatherer && pType->DeploysInto != nullptr && !pType->Teleporter)
			{
				const CellStruct uc = pUnit->GetMapCoords();
				bool hasNearOre = false;
				CellClass* pBestSameZoneOre = nullptr;
				int bestOreDist = 999999;
				for (int dy = -48; dy <= 48; dy += 2)
				{
					for (int dx = -48; dx <= 48; dx += 2)
					{
						const CellStruct tc { static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy) };
						auto const pC = MapClass::Instance.TryGetCellAt(tc);
						if (!pC || pC->GetContainedTiberiumValue() <= 0)
							continue;

						if (unitZoneForEco >= 0
							&& MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false)
								!= unitZoneForEco)
						{
							continue;
						}

						const int d = std::max(std::abs(dx), std::abs(dy));
						if (d <= 6)
						{
							hasNearOre = true;
							break;
						}

						if (d < bestOreDist)
						{
							bestOreDist = d;
							pBestSameZoneOre = pC;
						}
					}

					if (hasNearOre)
						break;
				}

				if (!hasNearOre && pBestSameZoneOre
					&& (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
						|| pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None))
				{
					pUnit->SetDestination(pBestSameZoneOre, true);
					pUnit->QueueMission(Mission::Move, true);
					continue;
				}

				if (hasNearOre
					&& (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
						|| pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None
						|| (pUnit->CurrentMission == Mission::Move && bestOreDist <= 4)))
				{
					pUnit->QueueMission(Mission::Harvest, true);
					continue;
				}
			}

			// IMPORTANT: Never call SetDestination() on Harvesters or Chrono Miners
			// (Teleporter=yes)! In Yuri's Revenge, calling SetDestination() on a
			// ChronoLocomotionClass unit triggers an immediate Chrono-shift and
			// resets UnitClass::Mission_Harvest's internal state machine, causing
			// Allied Chrono Miners to loop-teleport endlessly to the Refinery
			// entrance cell. Vanilla Mission_Harvest natively handles ore/gem search
			// and Chrono return.
			if (pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None
				|| ((pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
					&& !pUnit->HasAnyLink()))
			{
				pUnit->QueueMission(Mission::Harvest, false);
			}

			continue;
		}

		// Yuri Grinder Economy: Recycle heavily damaged Mind-Controlled enemy
		// vehicles into the Grinder for instant cash!
		if (pGrinder && pUnit->MindControlledBy != nullptr && pUnit->GetHealthPercentage() < 0.45
			&& pUnit->CurrentMission != Mission::Enter)
		{
			OrderFootToEnter(pUnit, pGrinder);

			continue;
		}

		// Auto-deploy MCV if we have no ConYards (ensuring a safe, corridor-clear
		// 4x4 foundation when rebuilt from a War Factory or retreating after
		// emergency undeploy), or march any additional Expansion MCV to an Ore
		// field
		if (pType->DeploysInto && pType->DeploysInto->ConstructionYard)
		{
			// When a ConYard packs up via UndeployBuildingToVehicle, the newly spawned MCV
			// starts in Mission::Guard with its migration goal stored in ArchiveTarget.
			// Promote ArchiveTarget to an active Move destination on frame 1 so it never
			// immediately unpacks on the exact same cell!
			if (pUnit->ArchiveTarget != nullptr && pUnit->CurrentMission != Mission::Move
				&& pUnit->CurrentMission != Mission::Enter && pUnit->QueuedMission != Mission::Move
				&& pUnit->QueuedMission != Mission::Enter && pUnit->DistanceFrom(pUnit->ArchiveTarget) > 2048)
			{
				AbstractClass* const pArchDest = pUnit->ArchiveTarget;
				pUnit->SetArchiveTarget(nullptr);
				pUnit->SetDestination(pArchDest, true);
				pUnit->ForceMission(Mission::Move);
			}

			const CellStruct mcvCell = pUnit->GetMapCoords();
			const bool isForeignTechMCV = (pHouse->CountOwnedNow(pType->DeploysInto) == 0 && pHouse->NumConYards >= 1);
			bool enemyNearMCV = false;

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
					|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
					continue;

				const CellStruct ec = pEB->GetMapCoords();
				const int maxBldThreatDist = pEB->Type->IsBaseDefense ? 13 : 11;

				if (std::max(std::abs(mcvCell.X - ec.X), std::abs(mcvCell.Y - ec.Y)) <= maxBldThreatDist)
				{
					enemyNearMCV = true;
					break;
				}
			}

			if (!enemyNearMCV)
			{
				for (auto const pET : TechnoClass::Array)
				{
					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner
						|| pET->WhatAmI() == AbstractType::Building)
					{
						continue;
					}

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
						continue;

					auto const pETType = pET->GetTechnoType();
					auto const pETUnitType = abstract_cast<UnitTypeClass*>(pETType);

					if (!pETType || (pETUnitType && pETUnitType->Harvester) || pETType->ResourceGatherer
						|| IsAmphibiousOrAirTransport(pETType))
					{
						continue;
					}

					auto const pEWpn = pET->GetWeapon(0) ? pET->GetWeapon(0)->WeaponType : nullptr;

					if (!pEWpn
						|| (pETType->Naval && (pEWpn->Range <= 2048 || (pEWpn->Projectile && !pEWpn->Projectile->AG))))
					{
						continue;
					}

					const CellStruct etc = pET->GetMapCoords();

					if (std::max(std::abs(mcvCell.X - etc.X), std::abs(mcvCell.Y - etc.Y)) <= 12)
					{
						enemyNearMCV = true;
						break;
					}
				}
			}

			const int mcvZone = MapClass::Instance.GetMovementZoneType(mcvCell, MovementZone::Normal, false);
			int minAnchorDist = 9999;
			int conYardsInMcvZone = 0;
			int hubsAndFactoriesInMcvZone = 0;
			CellStruct newestYard = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : mcvCell;

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
					|| pOwnBld->Type->InvisibleInGame)
				{
					continue;
				}

				if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery && !pOwnBld->Type->BaseNormal
					&& pOwnBld->Type->Factory == AbstractType::None)
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();
				const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);

				if (pOwnBld->Type->ConstructionYard)
				{
					newestYard = bc;

					if (bz == mcvZone)
						conYardsInMcvZone++;
				}

				if (mcvZone >= 0 && bz != mcvZone)
					continue;

				if (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination
					|| pOwnBld->Type->Factory != AbstractType::None || pOwnBld->Type->BaseNormal)
				{
					hubsAndFactoriesInMcvZone++;
				}

				const int d = std::max(std::abs(mcvCell.X - bc.X), std::abs(mcvCell.Y - bc.Y));

				if (d < minAnchorDist)
					minAnchorDist = d;
			}

			const bool canFerryThisMCV = CanHouseFerryTypeAcrossWater(pHouse, pType);
			bool ownsLiveMcvTransportNow = false;

			for (auto const pT : UnitClass::Array)
			{
				if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->IsAlive && pT->Type
					&& IsAmphibiousOrAirTransport(pT->Type)
					&& static_cast<int>(pT->Type->SizeLimit) >= static_cast<int>(pType->TechnoTypeClass::Size)
					&& pT->Type->Passengers >= static_cast<int>(pType->TechnoTypeClass::Size))
				{
					ownsLiveMcvTransportNow = true;
					break;
				}
			}

			const bool isMovingToDestEarly
				= ((pUnit->CurrentMission == Mission::Move || pUnit->QueuedMission == Mission::Move)
					&& pUnit->Destination != nullptr && pUnit->DistanceFrom(pUnit->Destination) > 1280)
				|| pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter;

			// Keep a nomadic single MCV undeployed while it is actively migrating to a distant
			// resource expansion or boarding/sailing on a live transport to a resource island
			const bool isNomadicSingleMCVHop = (pHouse->NumConYards == 0 && hubsAndFactoriesInMcvZone >= 3
				&& pHouse->NumWarFactories >= 1 && !enemyNearMCV
				&& ((ownsLiveMcvTransportNow && pIslandResourceGoalCell != nullptr && mcvZone != resourceIslandZone)
					|| isMovingToDestEarly));

			if (pHouse->NumConYards == 0 && !isNomadicSingleMCVHop)
			{
				auto const pYardType = pType->DeploysInto;
				if (!enemyNearMCV && CanDeployerUnpackHere(pHouse, pYardType, pUnit))
				{
					OrderDeployerUnpack(pUnit);
					continue;
				}

				const bool reachedOrNoDest = (pUnit->CurrentMission != Mission::Move || pUnit->Destination == nullptr
					|| CellClass::Coord2Cell(pUnit->Destination->GetCoords()) == mcvCell);
				if (reachedOrNoDest || (Unsorted::CurrentFrame % 45 < 15))
				{
					const CellStruct safeYardStandCell = FindBestDeployStandCell(pHouse, pYardType, pUnit, mcvZone);
					if (safeYardStandCell == mcvCell && !enemyNearMCV
						&& CanDeployerUnpackHere(pHouse, pYardType, pUnit))
					{
						OrderDeployerUnpack(pUnit);
					}
					else if (safeYardStandCell != CellStruct::Empty)
					{
						if (auto const pSafeDest = MapClass::Instance.TryGetCellAt(safeYardStandCell))
						{
							pUnit->SetDestination(pSafeDest, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}
					else if (!enemyNearMCV)
					{
						pUnit->Scatter(CoordStruct::Empty, true, false);
					}
				}

				continue;
			}

			// Slot 82 (Stolen / Foreign MCV Safe Extraction, Island Transport
			// Evacuation & Home Base Deployment): If the stolen MCV is on a
			// disconnected island (mcvZone != homeGroundZone), board a nearby
			// Amphibious Transport (or move to the safest pickup beach while a
			// transport sails over to rescue it) to ferry it back to our safe Home
			// Island! If on the same landmass and inside a hostile enemy zone
			// (enemyNearMCV) OR outside our home base (> 16 cells from homeCoords),
			// evacuate it by land back to our safe home base! Once the stolen
			// foreign-faction MCV reaches our safe home base (<= 16 cells from
			// homeCoords and !enemyNearMCV), deploy it safely inside our base
			// perimeter so we can build its foreign tech tree!
			const bool landedOnNewIsland = (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone
				&& conYardsInMcvZone == 0 && hubsAndFactoriesInMcvZone < 2);
			if (scores[82] > 60 && homeCoords != CellStruct::Empty && !landedOnNewIsland
				&& (enemyNearMCV || isForeignTechMCV))
			{
				const bool onDisconnectedEnemyIsland
					= (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone);

				if (onDisconnectedEnemyIsland && isForeignTechMCV && CanHouseFerryTypeAcrossWater(pHouse, pType))
				{
					UnitClass* pRescueTrans = nullptr;
					int bestRescueDist = 999999;

					for (auto const pT : UnitClass::Array)
					{
						if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
							|| !IsAmphibiousOrAirTransport(pT->Type))
						{
							continue;
						}

						if (pType->TechnoTypeClass::Size > pT->Type->SizeLimit
							|| (pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size))
								> pT->Type->Passengers)
						{
							continue;
						}

						const int d = pUnit->DistanceFrom(pT);

						if (d < bestRescueDist)
						{
							bestRescueDist = d;

							pRescueTrans = pT;
						}
					}

					if (pRescueTrans && bestRescueDist <= 3584)
					{
						if (pRescueTrans->CurrentMission == Mission::Move
							|| (pRescueTrans->Destination != nullptr
								&& !HasActiveBoardingPassenger(pRescueTrans, mcvZone)))
						{
							pRescueTrans->SetDestination(nullptr, true);
							pRescueTrans->ClearNavigationList();
							pRescueTrans->ForceMission(Mission::Guard);
						}

						OrderFootToEnter(pUnit, pRescueTrans);

						continue;
					}

					if (pUnit->CurrentMission != Mission::Move || (Unsorted::CurrentFrame % 60 < 15))
					{
						if (auto const pPickupBeach = FindSafeIslandLandingCell(pHouse, mcvCell, mcvZone, false))
						{
							pUnit->SetDestination(pPickupBeach, true);

							pUnit->QueueMission(Mission::Move, true);
						}
					}

					continue;
				}

				const double distToHome = mcvCell.DistanceFrom(homeCoords);

				if (enemyNearMCV || (isForeignTechMCV && distToHome > 16.0))
				{
					if (pUnit->CurrentMission != Mission::Move || (Unsorted::CurrentFrame % 60 < 15))
					{
						const CellStruct safeHomeCell
							= NeuralAI::FindSpatialTetrisBuildCell(pHouse, pType->DeploysInto);

						const CellStruct goalCoords = (safeHomeCell != CellStruct::Empty) ? safeHomeCell : homeCoords;

						if (auto const pHomeDest = MapClass::Instance.TryGetCellAt(goalCoords))
						{
							pUnit->SetDestination(pHomeDest, true);

							pUnit->QueueMission(Mission::Move, true);
						}
					}

					continue;
				}

				if (isForeignTechMCV && !enemyNearMCV
					&& (distToHome <= 16.0
						|| (onDisconnectedEnemyIsland && !CanHouseFerryTypeAcrossWater(pHouse, pType))))
				{
					if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
					{
						OrderDeployerUnpack(pUnit);
					}
					else
					{
						const CellStruct clearCell
							= FindBestDeployStandCell(pHouse, pType->DeploysInto, pUnit, mcvZone);
						if (auto const pClear = MapClass::Instance.TryGetCellAt(clearCell))
						{
							pUnit->SetDestination(pClear, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}

					continue;
				}
			}

			const bool isMovingToDest = (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr
				&& pUnit->DistanceFrom(pUnit->Destination) > 768);
			const bool isSecondaryIsland = (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone);
			const bool isArchipelagoOrNavalMap = (pIslandResourceGoalCell != nullptr || pIslandTechGoalCell != nullptr
				|| pHouse->NumShipyards > 0 || pLoadingTransport != nullptr || isSecondaryIsland);
			const bool alreadyHasConYardOnThisIsland = (!isForeignTechMCV
				&& ((isSecondaryIsland && conYardsInMcvZone >= 1) || (isArchipelagoOrNavalMap && conYardsInMcvZone >= 1)
					|| conYardsInMcvZone >= 2));
			const bool currentIslandAlreadyDeveloped = (conYardsInMcvZone >= 1
				|| (hubsAndFactoriesInMcvZone >= 2
					&& (mcvZone != homeGroundZone || pHouse->NumConYards >= 1 || !hasUncolonizedOreOnHomeIsland
						|| pHouse->OwnedBuildings >= 6)));
			const bool mustColonizeOtherIsland = (canFerryThisMCV && mcvZone >= 0
				&& (currentIslandAlreadyDeveloped || alreadyHasConYardOnThisIsland) && !isForeignTechMCV
				&& ((pIslandResourceGoalCell != nullptr && mcvZone != resourceIslandZone)
					|| alreadyHasConYardOnThisIsland));
			const bool wantsMultiConYardSpeed = (pHouse->NumConYards >= 1 && !mustColonizeOtherIsland
				&& !alreadyHasConYardOnThisIsland && !isArchipelagoOrNavalMap
				&& ((scores[30] > 650 && scores[35] <= 60 && scores[81] <= 90)
					|| (!canFerryThisMCV && !hasUncolonizedOreOnHomeIsland)));
			const int requiredAnchorDist = landedOnNewIsland ? 0 : (wantsMultiConYardSpeed ? 8 : 16);

			if (mustColonizeOtherIsland)
			{
				const bool isSelfAmphibOrAir = (pType->MovementZone == MovementZone::Amphibious
					|| pType->MovementZone == MovementZone::AmphibiousDestroyer
					|| pType->MovementZone == MovementZone::AmphibiousCrusher
					|| pType->MovementZone == MovementZone::Fly || pType->JumpJet);
				if (isSelfAmphibOrAir && pIslandResourceGoalCell)
				{
					if (!isMovingToDest || (Unsorted::CurrentFrame % 90 < 25))
					{
						pUnit->SetDestination(pIslandResourceGoalCell, true);
						pUnit->QueueMission(Mission::Move, true);
					}
				}
				else
				{
					ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, false);
					if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter)
					{
						// Already actively boarding a valid transport: do NOT overwrite or
						// spam orders!
					}
					else
					{
						UnitClass* pBeachedMcvTrans = nullptr;
						int bestTransDist = 999999;
						for (auto const pT : UnitClass::Array)
						{
							if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
								|| !IsAmphibiousOrAirTransport(pT->Type))
							{
								continue;
							}

							if (static_cast<int>(pType->TechnoTypeClass::Size) > static_cast<int>(pT->Type->SizeLimit)
								|| (pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size))
									> pT->Type->Passengers)
							{
								continue;
							}

							auto const pTCell = pT->GetCell();
							const int tz = MapClass::Instance.GetMovementZoneType(
								pT->GetMapCoords(), MovementZone::Normal, false);
							if (tz == mcvZone && pTCell && pTCell->LandType != LandType::Water)
							{
								const int d = pUnit->DistanceFrom(pT);
								if (d < bestTransDist)
								{
									bestTransDist = d;
									pBeachedMcvTrans = pT;
								}
							}
						}

						if (pBeachedMcvTrans)
						{
							if (!HasActiveBoardingPassenger(pBeachedMcvTrans, mcvZone)
								&& (pBeachedMcvTrans->CurrentMission == Mission::Move
									|| pBeachedMcvTrans->Destination != nullptr))
							{
								pBeachedMcvTrans->SetDestination(nullptr, true);
								pBeachedMcvTrans->ClearNavigationList();
								pBeachedMcvTrans->ForceMission(Mission::Guard);
							}

							OrderFootToEnter(pUnit, pBeachedMcvTrans);
						}
						else if (!isMovingToDest)
						{
							if (!NeuralAI::IsCellClearedOfExitCorridors(pHouse, pType->DeploysInto, mcvCell))
							{
								pUnit->Scatter(CoordStruct::Empty, true, false);
							}
							else if (auto const pIslandPickupBeach
								= FindSafeIslandLandingCell(pHouse, mcvCell, mcvZone, false))
							{
								if (mcvCell.DistanceFrom(pIslandPickupBeach->MapCoords) > 4.0)
								{
									pUnit->SetDestination(pIslandPickupBeach, true);
									pUnit->QueueMission(Mission::Move, true);
								}
							}
						}
					}
				}
			}
			else if (landedOnNewIsland || (!isMovingToDest && minAnchorDist >= requiredAnchorDist && !enemyNearMCV))
			{
				if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
				{
					OrderDeployerUnpack(pUnit);
				}
				else
				{
					const CellStruct deployCell
						= FindDeployableFoundationNearCell(pHouse, pType->DeploysInto, mcvCell, mcvZone, pUnit);
					if (deployCell != CellStruct::Empty)
					{
						if (!isMovingToDest || (Unsorted::CurrentFrame % 45 < 15))
						{
							if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
							{
								pUnit->SetDestination(pClear, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
					}
					else
					{
						pUnit->Scatter(CoordStruct::Empty, true, false);
					}
				}
			}
			else if (!isMovingToDest
				&& (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
					|| pUnit->CurrentMission == Mission::Move))
			{
				CellClass* pExpansionDest = nullptr;
				// Slot 81 (Forward Siege Outpost / Prudent Standoff Distance from
				// Enemy Base): Find the nearest enemy structure on the same landmass
				// (mcvZone) and compute a safe standoff position
				// (~16 cells outside the enemy perimeter, >= 14 cells from our existing
				// hubs, and >= 12 cells from enemy turrets) so the MCV can deploy a
				// forward Outpost (War Factory, Barracks, Refinery, or
				// Prism/Obelisk/Grand Cannon turret push)!
				if (scores[81] > 75 && (pHouse->CountResourceDestinations >= 1 || pHouse->OwnedBuildings >= 5))
				{
					BuildingClass* pSameZoneEnemyBld = nullptr;
					int bestEnemyDist = 99999;
					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
							|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
						{
							continue;
						}

						if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
							continue;

						const CellStruct ec = pEB->GetMapCoords();
						if (mcvZone >= 0
							&& MapClass::Instance.GetMovementZoneType(ec, MovementZone::Normal, false) != mcvZone)
						{
							continue;
						}

						const int dE = std::max(std::abs(mcvCell.X - ec.X), std::abs(mcvCell.Y - ec.Y));
						if (dE < bestEnemyDist)
						{
							bestEnemyDist = dE;
							pSameZoneEnemyBld = pEB;
						}
					}

					if (pSameZoneEnemyBld && bestEnemyDist >= 20)
					{
						const CellStruct ec = pSameZoneEnemyBld->GetMapCoords();
						const int dxE = mcvCell.X - ec.X;
						const int dyE = mcvCell.Y - ec.Y;
						const int distE = std::max(1, std::max(std::abs(dxE), std::abs(dyE)));
						for (int standoffDist = 16; standoffDist <= 22 && !pExpansionDest; standoffDist += 2)
						{
							const CellStruct siegeCenter { static_cast<short>(ec.X + (dxE * standoffDist) / distE),
								static_cast<short>(ec.Y + (dyE * standoffDist) / distE) };
							const CellStruct deployStand = FindDeployableFoundationNearCell(
								pHouse, pType->DeploysInto, siegeCenter, mcvZone, pUnit);
							const CellStruct candStage = (deployStand != CellStruct::Empty) ? deployStand : siegeCenter;
							auto const pSC = MapClass::Instance.TryGetCellAt(candStage);
							if (!pSC
								|| !pSC->IsClearToMove(
									pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							{
								continue;
							}

							if (mcvZone >= 0
								&& MapClass::Instance.GetMovementZoneType(candStage, MovementZone::Normal, false)
									!= mcvZone)
							{
								continue;
							}

							bool nearOwnHubOrEnemyTurret = false;
							for (auto const pOwnBld : pHouse->Buildings)
							{
								if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
									|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
								{
									continue;
								}

								if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery
									&& !pOwnBld->Type->ResourceDestination && !pOwnBld->Type->BaseNormal
									&& pOwnBld->Type->Factory == AbstractType::None)
								{
									continue;
								}

								const CellStruct bc = pOwnBld->GetMapCoords();
								if (std::max(std::abs(candStage.X - bc.X), std::abs(candStage.Y - bc.Y)) < 14)
								{
									nearOwnHubOrEnemyTurret = true;
									break;
								}
							}

							for (auto const pEB : BuildingClass::Array)
							{
								if (nearOwnHubOrEnemyTurret)
									break;

								if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
									|| pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner)
									|| pEB->Owner->IsNeutral())
								{
									continue;
								}

								const CellStruct ebc = pEB->GetMapCoords();
								if (std::max(std::abs(candStage.X - ebc.X), std::abs(candStage.Y - ebc.Y)) < 12)
								{
									nearOwnHubOrEnemyTurret = true;
									break;
								}
							}

							if (!nearOwnHubOrEnemyTurret)
								pExpansionDest = pSC;
						}
					}
				}

				// Distant Uncolonized Ore / Difficult Map Zone Expansion (scan up to
				// [-128, 128] across the landmass):
				int bestExpDist = 99999;
				for (int dy = -128; dy <= 128 && !pExpansionDest; dy += (std::abs(dy) <= 56 ? 3 : 4))
				{
					for (int dx = -128; dx <= 128; dx += (std::abs(dx) <= 56 ? 3 : 4))
					{
						const CellStruct tc { static_cast<short>(mcvCell.X + dx), static_cast<short>(mcvCell.Y + dy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(tc);
						if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
							continue;

						const int candZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
						if (mcvZone >= 0 && candZone != mcvZone)
							continue;

						bool tooCloseToExistingHub = false;
						for (auto const pOwnBld : pHouse->Buildings)
						{
							if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
								|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
							{
								continue;
							}

							if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery
								&& !pOwnBld->Type->ResourceDestination && !pOwnBld->Type->BaseNormal
								&& pOwnBld->Type->Factory == AbstractType::None)
							{
								continue;
							}

							const CellStruct bc = pOwnBld->GetMapCoords();
							if (MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false) != candZone)
								continue;

							if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 14)
							{
								tooCloseToExistingHub = true;
								break;
							}
						}

						if (tooCloseToExistingHub)
							continue;

						const int distFromMCV = std::abs(tc.X - mcvCell.X) + std::abs(tc.Y - mcvCell.Y);
						if (distFromMCV < bestExpDist)
						{
							bestExpDist = distFromMCV;
							pExpansionDest = pCell;
						}
					}
				}

				if (pExpansionDest)
				{
					pUnit->SetDestination(pExpansionDest, true);
					pUnit->QueueMission(Mission::Move, true);
				}
				else if (!enemyNearMCV && !alreadyHasConYardOnThisIsland)
				{
					if (CanDeployerUnpackHere(pHouse, pType->DeploysInto, pUnit))
					{
						OrderDeployerUnpack(pUnit);
					}
					else
					{
						const CellStruct localYardCell
							= FindBestDeployStandCell(pHouse, pType->DeploysInto, pUnit, mcvZone);
						if (localYardCell != CellStruct::Empty)
						{
							if (auto const pLocalDest = MapClass::Instance.TryGetCellAt(localYardCell))
							{
								pUnit->SetDestination(pLocalDest, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
						else
						{
							pUnit->Scatter(CoordStruct::Empty, true, false);
						}
					}
				}
			}

			continue;
		}

		// Tactical Combat Deployment (Tick Tank, Nod Artillery, Juggernaut, Siege
		// Chopper) & Universal Deployable Key Structures across all mods (Mobile
		// Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret Cores,
		// CABAL Nanocores)!
		if ((pType->DeploysInto && !pType->DeploysInto->ConstructionYard) || pType->DeployFire)
		{
			const bool hasPrimaryWeapon
				= (pType->GetWeapon(0) != nullptr && pType->GetWeapon(0)->WeaponType != nullptr);

			auto const pTargetBld = pType->DeploysInto;

			const bool isKeyStructureDeployer = (pTargetBld != nullptr && IsTrueBaseStructureTarget(pTargetBld)
				&& (!pType->Harvester && !pType->ResourceGatherer || isIslandColonizerSlaveMiner)
				&& (!pTargetBld->IsBaseDefense || !hasPrimaryWeapon));

			if (isKeyStructureDeployer)
			{
				const CellStruct nanoCell = pUnit->GetMapCoords();
				const int nanoZone = MapClass::Instance.GetMovementZoneType(nanoCell, MovementZone::Normal, false);
				const bool isNanoMoving = (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr
					&& pUnit->DistanceFrom(pUnit->Destination) > 768);
				const bool nanoReachedOrNoDest
					= (pUnit->CurrentMission != Mission::Move || pUnit->Destination == nullptr
						|| CellClass::Coord2Cell(pUnit->Destination->GetCoords()) == nanoCell);

				int ownedRefineries = 0;
				int ownedWarFactories = 0;
				int ownedBarracks = 0;
				int minDistFromBaseBld = 9999;
				int minDistFromRefOrYard = 9999;
				int hubsInNanoZone = 0;

				for (auto const pOwnBld : pHouse->Buildings)
				{
					if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
						|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
					{
						continue;
					}

					const CellStruct bc = pOwnBld->GetMapCoords();
					const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
					const int d = std::max(std::abs(nanoCell.X - bc.X), std::abs(nanoCell.Y - bc.Y));

					if (nanoZone < 0 || bz == nanoZone)
					{
						if (d < minDistFromBaseBld)
							minDistFromBaseBld = d;

						if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard
							|| pOwnBld->Type->ResourceDestination || pOwnBld->Type->Factory != AbstractType::None)
						{
							hubsInNanoZone++;
							if (d < minDistFromRefOrYard)
								minDistFromRefOrYard = d;
						}
					}

					if (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination)
						ownedRefineries++;
					else if (pOwnBld->Type->Factory == AbstractType::UnitType && !pOwnBld->Type->Naval)
						ownedWarFactories++;
					else if (pOwnBld->Type->Factory == AbstractType::InfantryType)
						ownedBarracks++;
				}

				const bool isDedicatedMobileOutpost
					= (pTargetBld->BaseNormal && !pTargetBld->Refinery && pTargetBld->Factory == AbstractType::None
						&& pTargetBld->PowerBonus <= pTargetBld->PowerDrain && pHouse->Buildings.Count >= 1);
				const bool canFerryDeployer = CanHouseFerryTypeAcrossWater(pHouse, pType);
				const bool isCrossIslandFactoryDeployer = (canFerryDeployer && pIslandResourceGoalCell != nullptr
					&& nanoZone != resourceIslandZone
					&& ((pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval && ownedWarFactories >= 1)
						|| (pTargetBld->Factory == AbstractType::InfantryType && ownedBarracks >= 1)));

				const bool isExpansionDeployer = isIslandColonizerSlaveMiner || isDedicatedMobileOutpost
					|| isCrossIslandFactoryDeployer
					|| ((pTargetBld->Refinery || pTargetBld->ResourceDestination) && ownedRefineries >= 1
						&& scores[30] > 450);

				if (isExpansionDeployer)
				{
					const bool mustColonizeIslandWithDeployer = (canFerryDeployer && nanoZone >= 0
						&& hubsInNanoZone >= 1 && pIslandResourceGoalCell != nullptr && nanoZone != resourceIslandZone);
					const bool landedOnNewIsland
						= (homeGroundZone >= 0 && nanoZone >= 0 && nanoZone != homeGroundZone && hubsInNanoZone == 0);

					if (mustColonizeIslandWithDeployer)
					{
						const bool isSelfAmphibOrAir = (pType->MovementZone == MovementZone::Amphibious
							|| pType->MovementZone == MovementZone::AmphibiousDestroyer
							|| pType->MovementZone == MovementZone::AmphibiousCrusher
							|| pType->MovementZone == MovementZone::Fly || pType->JumpJet);
						if (isSelfAmphibOrAir && pIslandResourceGoalCell)
						{
							if (!isNanoMoving || (Unsorted::CurrentFrame % 90 < 25))
							{
								pUnit->SetDestination(pIslandResourceGoalCell, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
						else
						{
							ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, false);
							if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter)
							{
								// Already actively boarding a valid transport: do NOT overwrite
								// or spam orders!
							}
							else
							{
								UnitClass* pBeachedTrans = nullptr;
								int bestTransDist = 999999;
								for (auto const pT : UnitClass::Array)
								{
									if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type
										|| !IsAmphibiousOrAirTransport(pT->Type))
									{
										continue;
									}

									if (static_cast<int>(pType->TechnoTypeClass::Size)
											> static_cast<int>(pT->Type->SizeLimit)
										|| (pT->Passengers.GetTotalSize()
											+ static_cast<int>(pType->TechnoTypeClass::Size))
											> pT->Type->Passengers)
									{
										continue;
									}

									auto const pTCell = pT->GetCell();
									const int tz = MapClass::Instance.GetMovementZoneType(
										pT->GetMapCoords(), MovementZone::Normal, false);
									if (tz == nanoZone && pTCell && pTCell->LandType != LandType::Water)
									{
										const int d = pUnit->DistanceFrom(pT);
										if (d < bestTransDist)
										{
											bestTransDist = d;
											pBeachedTrans = pT;
										}
									}
								}

								if (pBeachedTrans)
								{
									if (!HasActiveBoardingPassenger(pBeachedTrans, nanoZone)
										&& (pBeachedTrans->CurrentMission == Mission::Move
											|| pBeachedTrans->Destination != nullptr))
									{
										pBeachedTrans->SetDestination(nullptr, true);
										pBeachedTrans->ClearNavigationList();
										pBeachedTrans->ForceMission(Mission::Guard);
									}

									OrderFootToEnter(pUnit, pBeachedTrans);
								}
								else if (!isNanoMoving)
								{
									if (auto const pIslandPickupBeach
										= FindSafeIslandLandingCell(pHouse, nanoCell, nanoZone, false))
									{
										if (nanoCell.DistanceFrom(pIslandPickupBeach->MapCoords) > 4.0)
										{
											pUnit->SetDestination(pIslandPickupBeach, true);
											pUnit->QueueMission(Mission::Move, true);
										}
									}
								}
							}
						}
					}
					else if (isIslandColonizerSlaveMiner)
					{
						// Never force OrderDeployerUnpack on a Slave Miner; let
						// Mission::Harvest manage deployment on ore!
						if (pUnit->CurrentMission != Mission::Harvest && pUnit->CurrentMission != Mission::Enter
							&& pUnit->CurrentMission != Mission::Move)
						{
							pUnit->QueueMission(Mission::Harvest, false);
						}
					}
					else if (landedOnNewIsland || (!isNanoMoving && minDistFromRefOrYard >= 10))
					{
						if (CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
						{
							OrderDeployerUnpack(pUnit);
						}
						else
						{
							const CellStruct deployCell
								= FindDeployableFoundationNearCell(pHouse, pTargetBld, nanoCell, nanoZone, pUnit);
							if (deployCell != CellStruct::Empty)
							{
								if (nanoReachedOrNoDest || (Unsorted::CurrentFrame % 45 < 15))
								{
									if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
									{
										pUnit->SetDestination(pClear, true);
										pUnit->QueueMission(Mission::Move, true);
									}
								}
							}
							else if (isIslandColonizerSlaveMiner)
							{
								pUnit->QueueMission(Mission::Harvest, false);
							}
						}
					}
					else if (nanoReachedOrNoDest || (Unsorted::CurrentFrame % 60 < 15))
					{
						CellClass* pExpOreCell = nullptr;
						int bestOreDist = 99999;
						for (int dy = -56; dy <= 56; dy += 3)
						{
							for (int dx = -56; dx <= 56; dx += 3)
							{
								const CellStruct tc { static_cast<short>(nanoCell.X + dx),
									static_cast<short>(nanoCell.Y + dy) };
								auto const pC = MapClass::Instance.TryGetCellAt(tc);
								if (!pC || pC->GetContainedTiberiumValue() <= 0)
									continue;

								const int candZone
									= MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
								if (nanoZone >= 0 && candZone != nanoZone)
									continue;

								bool nearExistingRef = false;
								for (auto const pOwnBld : pHouse->Buildings)
								{
									if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
										|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
									{
										continue;
									}

									if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard
										|| pOwnBld->Type->ResourceDestination)
									{
										const CellStruct bc = pOwnBld->GetMapCoords();
										if (MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false)
											!= candZone)
										{
											continue;
										}

										if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 12)
										{
											nearExistingRef = true;
											break;
										}
									}
								}

								if (nearExistingRef)
									continue;

								const int distFromNano = std::abs(tc.X - nanoCell.X) + std::abs(tc.Y - nanoCell.Y);
								if (distFromNano < bestOreDist)
								{
									bestOreDist = distFromNano;
									pExpOreCell = pC;
								}
							}
						}

						if (pExpOreCell && bestOreDist > 6)
						{
							pUnit->SetDestination(pExpOreCell, true);
							pUnit->QueueMission(Mission::Move, true);
						}
						else if (CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
						{
							OrderDeployerUnpack(pUnit);
						}
						else
						{
							const CellStruct deployCell = FindBestDeployStandCell(pHouse, pTargetBld, pUnit, nanoZone);
							if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
							{
								pUnit->SetDestination(pClear, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
					}

					continue;
				}

				// Defense Turret Deployer (Mobile Turret Cores / ROBOTEGG_OBELISK2,
				// ROBOTEGG_CERBERUS2, etc.): Unpack on a valid foundation clear of
				// factory exit corridors, or move to a free perimeter foundation!
				if (pTargetBld->IsBaseDefense)
				{
					if (minDistFromBaseBld >= 2 && CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
					{
						OrderDeployerUnpack(pUnit);
						continue;
					}

					if (nanoReachedOrNoDest || (Unsorted::CurrentFrame % 45 < 15))
					{
						const CellStruct perimStandCell = FindBestDeployStandCell(pHouse, pTargetBld, pUnit, nanoZone);
						if (perimStandCell == nanoCell && CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
						{
							OrderDeployerUnpack(pUnit);
							continue;
						}

						if (auto const pPerim = MapClass::Instance.TryGetCellAt(perimStandCell))
						{
							pUnit->SetDestination(pPerim, true);
							pUnit->QueueMission(Mission::Move, true);
							continue;
						}
					}

					continue;
				}

				// Base Infrastructure Deployer (Power, Barracks, Radar, Tech, Repair
				// Bay, Shield/Gap Generator, or Refinery/Factory): Unpack immediately
				// if current cell has a valid foundation & is clear of factory exit
				// corridors; otherwise search for a free foundation and move there!
				if (CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
				{
					OrderDeployerUnpack(pUnit);
				}
				else if (nanoReachedOrNoDest || (Unsorted::CurrentFrame % 45 < 15))
				{
					const CellStruct clearStandCell = FindBestDeployStandCell(pHouse, pTargetBld, pUnit, nanoZone);
					if (auto const pClear = MapClass::Instance.TryGetCellAt(clearStandCell))
					{
						pUnit->SetDestination(pClear, true);
						pUnit->QueueMission(Mission::Move, true);
					}
				}

				continue;
			}
			else if (pUnit->Target != nullptr && pUnit->CurrentMission == Mission::Attack)
			{
				OrderDeployerUnpack(pUnit);

				continue;
			}
			else if (pTargetBld != nullptr && !IsTrueBaseStructureTarget(pTargetBld))
			{
				// Tactical Support Deployers (Mobile Stealth Generator SGEN/GGEN,
				// Mobile Sensor Array LPST/NLPST, Limpet Drone): Only deploy when near
				// our frontline lead tank or when an enemy unit is within 10 cells,
				// never right outside the War Factory!
				const bool nearLeadTank = (pLeadHeavyTank != nullptr && pUnit->DistanceFrom(pLeadHeavyTank) <= 1024);
				bool nearEnemyThreat = false;
				for (auto const pEU : UnitClass::Array)
				{
					if (pEU && !pEU->InLimbo && pEU->IsAlive && pEU->Owner && pEU->Owner != pHouse
						&& !pHouse->IsAlliedWith(pEU->Owner) && !pEU->Owner->IsNeutral()
						&& pUnit->DistanceFrom(pEU) <= 2560)
					{
						nearEnemyThreat = true;
						break;
					}
				}

				if ((nearLeadTank || nearEnemyThreat) && CanDeployerUnpackHere(pHouse, pTargetBld, pUnit))
				{
					OrderDeployerUnpack(pUnit);
					continue;
				}
			}
		}

		// Preserve Elite vehicles (< 60% HP) and expensive vehicles by retreating
		// to Service Depot (Slot 54 adjusts retreat HP & survival reflex)
		const double baseRetreat = std::clamp(0.35 + static_cast<double>(scores[54]) * 0.00025, 0.22, 0.55);
		const double repairThreshold = pUnit->Veterancy.IsElite() ? std::max(0.60, baseRetreat + 0.15) : baseRetreat;

		// Slot 64 (Immediate Service Depot De-Parasiting) & Slot 54 (Repair
		// Retreat): If a Terror Drone or Squid is eating our vehicle
		// (ParasiteEatingMe != nullptr), rush to Service Depot immediately even at
		// 99% HP so the repair arm kills the parasite!
		if (pRepairDepot
			&& ((pUnit->ParasiteEatingMe != nullptr && scores[64] > 60)
				|| ((pType->Cost >= 800 || pUnit->Veterancy.IsElite())
					&& pUnit->GetHealthPercentage() < repairThreshold)))
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				DispatchFootToTargetOrTransport(pUnit, pRepairDepot, Mission::Enter, false);
			}

			continue;
		}

		// Slot 65 (Grinder Mind-Control Recycling & Salvage): Send mind-controlled
		// vehicles we captured (MindControlledBy != nullptr) into our Grinder for
		// instant cash AND to immediately free our Yuri/Mastermind's control slot!
		if (pGrinderBld && !pType->Naval && scores[65] > 80
			&& (pUnit->MindControlledBy != nullptr
				|| (!pRepairDepot && pUnit->GetHealthPercentage() < 0.18 && pHouse->Available_Money() < 600)))
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				DispatchFootToTargetOrTransport(pUnit, pGrinderBld, Mission::Enter, false);
			}

			continue;
		}

		// Slot 67 (ManualReload Ammo Replenishment): Return to reload dock when
		// Ammo == 0
		if (pUnitReloadBld && pType->ManualReload && pType->Ammo > 0 && pUnit->Ammo == 0 && scores[67] > 60)
		{
			if (pUnit->CurrentMission != Mission::Enter)
			{
				DispatchFootToTargetOrTransport(pUnit, pUnitReloadBld, Mission::Enter, false);
			}

			continue;
		}

		// Slot 68 (TiberiumHeal Regeneration): Move wounded
		// CyborgReaper/Fiend/Visceroid units onto nearby Tiberium/Ore to heal for
		// free!
		if (pType->TiberiumHeal && pUnit->GetHealthPercentage() < 0.55 && scores[68] > 60
			&& pUnit->CurrentMission != Mission::Move)
		{
			if (pUnit->MoveToTiberium(10, false))
				continue;
		}

		UnstickCrossIslandFoot(pUnit);
		const bool isUnitIdle
			= (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
			&& !IsFootActivelyAttackingOrMovingToEnemy(pUnit);

		const auto pPrimaryWpn = pType->GetWeapon(0) ? pType->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Engineer ConYard Heist Cease-Fire): If our tank is firing at an
		// enemy ConYard that is already in red health (< 35% HP) while our own
		// Engineer is within 10 cells moving in to capture it, hold fire on the
		// ConYard so our Engineer can steal it alive!
		if (ownEngineerNearEnemyConYard && pCapturableEnemyConYard && pUnit->Target == pCapturableEnemyConYard
			&& pCapturableEnemyConYard->GetHealthPercentage() < 0.35 && scores[82] > 60)
		{
			pUnit->SetTarget(nullptr);

			pUnit->QueueMission(Mission::Guard, true);

			continue;
		}

		// Slot 71 (Chrono Legionnaire Temporal Breaker): Immediately kill any enemy
		// Chrono Legionnaire erasing our building or tank so it is restored at
		// 100%!
		if (pEnemyTemporalAttacker && pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[71] > 60
			&& pUnit->DistanceFrom(pEnemyTemporalAttacker) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyTemporalAttacker, Mission::Attack, true);

			continue;
		}

		const auto pSecondaryWpn = pType->GetWeapon(1) ? pType->GetWeapon(1)->WeaponType : nullptr;
		const bool hasMobileAAWeapon = (pPrimaryWpn && pPrimaryWpn->Projectile && pPrimaryWpn->Projectile->AA)
			|| (pSecondaryWpn && pSecondaryWpn->Projectile && pSecondaryWpn->Projectile->AA);

		// Slot 74 (Anti-Drain Floating Disc Interceptor & Multi-Flank Mobile AA Defense):
		// Mobile AA units immediately shoot down any enemy Floating Disc draining our buildings,
		// and distribute across multiple simultaneous incoming airborne flanks near our base!
		if (pEnemyDrainer && hasMobileAAWeapon && scores[74] > 60 && pUnit->DistanceFrom(pEnemyDrainer) <= 4608)
		{
			pUnit->SetTarget(pEnemyDrainer);

			pUnit->QueueMission(Mission::Attack, true);

			continue;
		}

		if (baseAirFlankCount > 0 && hasMobileAAWeapon && isUnitIdle)
		{
			int bestAirIdx = -1;
			int bestAirCost = 999999;

			for (int ai = 0; ai < baseAirFlankCount; ++ai)
			{
				auto const pAirT = baseAirFlanks[ai].Target;

				if (!pAirT || pAirT->InLimbo || !pAirT->IsAlive)
					continue;

				const int distAir = pUnit->DistanceFrom(pAirT);

				if (distAir > 5632)
					continue;

				const int balancedScore = distAir + (baseAirFlanks[ai].AssignedInterceptors * 1536);

				if (balancedScore < bestAirCost)
				{
					bestAirCost = balancedScore;
					bestAirIdx = ai;
				}
			}

			if (bestAirIdx >= 0)
			{
				auto const pBestAir = baseAirFlanks[bestAirIdx].Target;
				baseAirFlanks[bestAirIdx].AssignedInterceptors++;
				DispatchFootToTargetOrTransport(pUnit, pBestAir, Mission::Attack, true);
				continue;
			}
		}

		// Slot 77 (Wounded Frontline Tank Pull-Back Micro): When our tank is
		// focused below 38% HP in active combat and healthy allied tanks are right
		// beside it, pull back 4 cells behind our healthy frontline so the enemy
		// switches target!
		if (pUnit->Target != nullptr && !pType->Naval && pUnit->GetHealthPercentage() < 0.38 && scores[77] > 80
			&& homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 45 < 15))
		{
			int healthyAlliesBeside = 0;

			for (auto const pAllyT : UnitClass::Array)
			{
				if (pAllyT && pAllyT != pUnit && pAllyT->Owner == pHouse && !pAllyT->InLimbo && pAllyT->IsAlive
					&& pAllyT->GetHealthPercentage() > 0.70 && pUnit->DistanceFrom(pAllyT) <= 1280)
				{
					healthyAlliesBeside++;
				}
			}

			if (healthyAlliesBeside >= 1)
			{
				const CellStruct uc = pUnit->GetMapCoords();
				const int sx = (homeCoords.X > uc.X) ? 3 : ((homeCoords.X < uc.X) ? -3 : 0);
				const int sy = (homeCoords.Y > uc.Y) ? 3 : ((homeCoords.Y < uc.Y) ? -3 : 0);
				const CellStruct backCell { static_cast<short>(uc.X + sx), static_cast<short>(uc.Y + sy) };

				if (auto const pBC = MapClass::Instance.TryGetCellAt(backCell))
				{
					if (pBC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						pUnit->SetDestination(pBC, true);

						pUnit->QueueMission(Mission::Move, true);

						continue;
					}
				}
			}
		}

		// Slot 76 (Squad Focus-Fire on Wounded In-Range Enemy): Concentrate fire on
		// a wounded enemy vehicle already inside our exact weapon range!
		if (pWoundedEnemyVehicle && pUnit->CurrentMission == Mission::Attack && pUnit->Target != pWoundedEnemyVehicle
			&& pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[76] > 80
			&& pUnit->DistanceFrom(pWoundedEnemyVehicle) <= pPrimaryWpn->Range)
		{
			pUnit->SetTarget(pWoundedEnemyVehicle);

			continue;
		}

		// Slot 79 (Surgical Enemy Power, Superweapon & Factory Snipe): When inside
		// range of a critical enemy structure, prioritize the nearest one over walls/silos!
		if (surgicalEnemyBldCount > 0 && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[79] > 80)
		{
			const int maxSnipeDist = std::max(1280, static_cast<int>(pPrimaryWpn->Range) + 384);
			BuildingClass* pNearestSurgical = nullptr;
			int bestSurgDist = maxSnipeDist + 1;

			for (int si = 0; si < surgicalEnemyBldCount; ++si)
			{
				auto const pCandBld = surgicalEnemyBlds[si];

				if (!pCandBld || pCandBld->InLimbo || !pCandBld->IsAlive)
					continue;

				const int d = pUnit->DistanceFrom(pCandBld);

				if (d <= maxSnipeDist && d < bestSurgDist)
				{
					bestSurgDist = d;
					pNearestSurgical = pCandBld;
				}
			}

			if (pNearestSurgical)
			{
				DispatchFootToTargetOrTransport(pUnit, pNearestSurgical, Mission::Attack, true);
				continue;
			}
		}

		// Slot 80 (Fast Flanker & Jumpjet Counter-Battery vs Enemy Artillery):
		if (pEnemyArtilleryUnit && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0
			&& (pType->Speed >= 8 || pType->JumpJet) && scores[80] > 80
			&& pUnit->DistanceFrom(pEnemyArtilleryUnit) <= 4608)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyArtilleryUnit, Mission::Attack, true);

			continue;
		}

		// Slot 78 (High-Ground Cliff Advantage Positioning): Siege/long-range units
		// step onto nearby higher-elevation cells (CellClass::Level) when idle!
		if (isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256) && !pType->Naval && scores[78] > 90
			&& (Unsorted::CurrentFrame % 120 < 20))
		{
			if (auto const pCurC = pUnit->GetCell())
			{
				const CellStruct uc = pUnit->GetMapCoords();

				for (int dy = -4; dy <= 4; dy += 2)
				{
					bool movedHigh = false;

					for (int dx = -4; dx <= 4; dx += 2)
					{
						const CellStruct hc { static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy) };

						if (auto const pHC = MapClass::Instance.TryGetCellAt(hc))
						{
							if (pHC->Level > pCurC->Level
								&& pHC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							{
								pUnit->SetDestination(pHC, true);

								pUnit->QueueMission(Mission::Move, true);

								movedHigh = true;

								break;
							}
						}
					}

					if (movedHigh)
						break;
				}
			}
		}

		// Slot 58 (Mind-Control Breaker): Focus-fire enemy Yuri/Mastermind/Psychic
		// Tower with ImmuneToPsionics vehicles or long-range artillery to
		// immediately free our mind-controlled tank!
		if (pEnemyMindController && isUnitIdle
			&& (pType->ImmuneToPsionics || (pPrimaryWpn && pPrimaryWpn->Range >= (6 * 256)))
			&& pUnit->DistanceFrom(pEnemyMindController) <= 4608)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyMindController, Mission::Attack, true);

			continue;
		}

		// Slot 58 (Tank Infantry Crushing): Heavy Crusher=yes tanks run over nearby
		// Crushable=yes enemy infantry (<= 3 cells) instead of wasting slow cannon
		// shells!
		if (pType->Crusher && !pType->Naval && scores[58] > 100
			&& (isUnitIdle || pUnit->CurrentMission == Mission::Attack) && (Unsorted::CurrentFrame % 45 < 15))
		{
			CellClass* pCrushCell = nullptr;

			for (auto const pEInf : InfantryClass::Array)
			{
				if (!pEInf || pEInf->InLimbo || !pEInf->IsAlive || !pEInf->Owner || !pEInf->Type
					|| !pEInf->Type->Crushable || pEInf->Type->C4 || pEInf->Type->Ivan)
				{
					continue;
				}

				if (pEInf->Owner == pHouse || pHouse->IsAlliedWith(pEInf->Owner) || pEInf->Owner->IsNeutral())
					continue;

				if (pEInf->GetCell() && pEInf->GetCell()->GetBuilding() == nullptr && pUnit->DistanceFrom(pEInf) <= 768
					&& IsCellVisibleToAI(pHouse, pEInf->GetCell()))
				{
					pCrushCell = pEInf->GetCell();

					break;
				}
			}

			if (pCrushCell)
			{
				DispatchFootToTargetOrTransport(pUnit, pCrushCell, Mission::Move, false);

				continue;
			}
		}

		// Slot 58 (Artillery Kiting / Hit-and-Run): Keep long-range siege units
		// (Prism Tank, V3, Magnetron, Juggernaut) at standoff distance when enemy
		// short-range tanks close within 3.5 cells!
		if (pPrimaryWpn && pPrimaryWpn->Range >= (8 * 256) && scores[58] > 140 && homeCoords != CellStruct::Empty
			&& (Unsorted::CurrentFrame % 45 < 15))
		{
			bool enemyTooClose = false;

			for (auto const pCloseEnemy : UnitClass::Array)
			{
				if (!pCloseEnemy || pCloseEnemy->InLimbo || !pCloseEnemy->IsAlive || !pCloseEnemy->Owner)
					continue;

				if (pCloseEnemy->Owner == pHouse || pHouse->IsAlliedWith(pCloseEnemy->Owner)
					|| pCloseEnemy->Owner->IsNeutral())
				{
					continue;
				}

				if (pUnit->DistanceFrom(pCloseEnemy) <= 896)
				{
					enemyTooClose = true;

					break;
				}
			}

			if (enemyTooClose)
			{
				const CellStruct uc = pUnit->GetMapCoords();
				const int stepX = (homeCoords.X > uc.X) ? 3 : ((homeCoords.X < uc.X) ? -3 : 0);
				const int stepY = (homeCoords.Y > uc.Y) ? 3 : ((homeCoords.Y < uc.Y) ? -3 : 0);
				const CellStruct kiteCoords { static_cast<short>(uc.X + stepX), static_cast<short>(uc.Y + stepY) };

				if (auto const pKiteCell = MapClass::Instance.TryGetCellAt(kiteCoords))
				{
					if (pKiteCell->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						pUnit->SetDestination(pKiteCell, true);

						pUnit->QueueMission(Mission::Move, true);

						continue;
					}
				}
			}
		}

		// Slot 59 (Crate Pickup): Send nearest fast idle vehicle to collect any
		// visible map Crate!
		if (pVisibleCrateCell && isUnitIdle && pUnit->DistanceFrom(pVisibleCrateCell) <= 3584)
		{
			DispatchFootToTargetOrTransport(pUnit, pVisibleCrateCell, Mission::Move, false);

			pVisibleCrateCell = nullptr;

			continue;
		}

		// Slot 56 (Own Harvester Defense & Enemy Harvester Eco-Raid - Multi-Instance Nearest Match):
		if (attackedOwnHarvCount > 0 && isUnitIdle)
		{
			UnitClass* pNearestAttackedHarv = nullptr;
			int bestOwnHarvDist = 4609;

			for (int hi = 0; hi < attackedOwnHarvCount; ++hi)
			{
				auto const pCandH = attackedOwnHarvesters[hi];

				if (!pCandH || pCandH->InLimbo || !pCandH->IsAlive)
					continue;

				const int d = pUnit->DistanceFrom(pCandH);

				if (d <= 4608 && d < bestOwnHarvDist)
				{
					bestOwnHarvDist = d;
					pNearestAttackedHarv = pCandH;
				}
			}

			if (pNearestAttackedHarv)
			{
				if (auto const pHarvCell = pNearestAttackedHarv->GetCell())
				{
					DispatchFootToTargetOrTransport(pUnit, pHarvCell, Mission::AttackMove, false);

					continue;
				}
			}
		}

		if (visibleEnemyHarvCount > 0 && isUnitIdle && scores[56] > 220 && pType->Speed >= 6)
		{
			UnitClass* pNearestEnemyHarv = nullptr;
			int bestEnemyHarvDist = 4609;

			for (int hi = 0; hi < visibleEnemyHarvCount; ++hi)
			{
				auto const pCandEH = visibleEnemyHarvesters[hi];

				if (!pCandEH || pCandEH->InLimbo || !pCandEH->IsAlive)
					continue;

				const int d = pUnit->DistanceFrom(pCandEH);

				if (d <= 4608 && d < bestEnemyHarvDist)
				{
					bestEnemyHarvDist = d;
					pNearestEnemyHarv = pCandEH;
				}
			}

			if (pNearestEnemyHarv)
			{
				const int wpnRange = pPrimaryWpn ? static_cast<int>(pPrimaryWpn->Range) : 1280;

				if (bestEnemyHarvDist <= wpnRange + 384)
				{
					DispatchFootToTargetOrTransport(pUnit, pNearestEnemyHarv, Mission::Attack, true);
					continue;
				}
				else if (auto const pEnemyHarvCell = pNearestEnemyHarv->GetCell())
				{
					// Use AttackMove when approaching an enemy harvester so raiders fight
					// back against any enemy units/defenses blocking the path!
					DispatchFootToTargetOrTransport(pUnit, pEnemyHarvCell, Mission::AttackMove, false);
					continue;
				}
			}
		}

		// Slot 57 (Surgical Bunker/Garrison Penetrator & Siege Focus):
		// Units with PenetratesBunker=yes kill enemy occupants inside civilian
		// structures without demolishing the building so our infantry can take it!
		// Long-range siege vehicles prioritize enemy garrisons & base defenses!
		if (enemyGarrisonCount > 0 && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Warhead
			&& pPrimaryWpn->Warhead->PenetratesBunker)
		{
			const int maxGarrDist = std::max(1536, static_cast<int>(pPrimaryWpn->Range) + 512);
			BuildingClass* pNearestGarr = nullptr;
			int bestGarrDist = maxGarrDist + 1;

			for (int gi = 0; gi < enemyGarrisonCount; ++gi)
			{
				auto const pCandG = enemyGarrisonsToClear[gi];

				if (!pCandG || pCandG->InLimbo || !pCandG->IsAlive || pCandG->Occupants.Count <= 0)
					continue;

				const int d = pUnit->DistanceFrom(pCandG);

				if (d <= maxGarrDist && d < bestGarrDist)
				{
					bestGarrDist = d;
					pNearestGarr = pCandG;
				}
			}

			if (pNearestGarr)
			{
				DispatchFootToTargetOrTransport(pUnit, pNearestGarr, Mission::Attack, true);
				continue;
			}
		}

		if (pPrioritySiegeBld && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256)
			&& pUnit->DistanceFrom(pPrioritySiegeBld) <= static_cast<int>(pPrimaryWpn->Range) + 512)
		{
			DispatchFootToTargetOrTransport(pUnit, pPrioritySiegeBld, Mission::Attack, true);
			continue;
		}

		// Slot 82 (Human Shield & Anti-Recapture Bodyguard Screen for Stolen
		// ConYard / Escaping MCV): If we just captured an enemy ConYard or are
		// extracting its stolen MCV, any nearby friendly combat vehicle (<= 16
		// cells) immediately intercepts enemy Engineers/tanks threatening it, or
		// moves directly around the stolen ConYard/MCV as a protective human shield
		// to draw enemy fire away while it escapes!
		if (pStolenConYardOrMCV && pUnit != pStolenConYardOrMCV && !pType->Naval && scores[82] > 60
			&& pUnit->DistanceFrom(pStolenConYardOrMCV) <= 4096)
		{
			if (pThreatToStolenAsset && pPrimaryWpn && pPrimaryWpn->Damage > 0 && pUnit->Target != pThreatToStolenAsset)
			{
				DispatchFootToTargetOrTransport(pUnit, pThreatToStolenAsset, Mission::Attack, true);

				continue;
			}
			else if (isUnitIdle && pUnit->DistanceFrom(pStolenConYardOrMCV) > 640)
			{
				if (auto const pShieldCell = pStolenConYardOrMCV->GetCell())
				{
					DispatchFootToTargetOrTransport(pUnit, pShieldCell, Mission::Area_Guard, false);

					continue;
				}
			}
		}

		// Slot 60 (Spawner Standoff & AA/Sensor Combined-Arms Escort):
		// Keep Spawner=yes units (Aircraft Carrier, Dreadnought, V3) at standoff
		// distance (> 5 cells) from approaching enemies!
		if (pPrimaryWpn && pPrimaryWpn->Spawner && scores[60] > 100 && homeCoords != CellStruct::Empty
			&& (Unsorted::CurrentFrame % 45 < 15))
		{
			bool enemyClosingOnSpawner = false;

			for (auto const pET : TechnoClass::Array)
			{
				if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner)
					continue;

				if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
					continue;

				if (pUnit->DistanceFrom(pET) <= 1280)
				{
					enemyClosingOnSpawner = true;

					break;
				}
			}

			if (enemyClosingOnSpawner)
			{
				const CellStruct uc = pUnit->GetMapCoords();
				const int sx = (homeCoords.X > uc.X) ? 4 : ((homeCoords.X < uc.X) ? -4 : 0);
				const int sy = (homeCoords.Y > uc.Y) ? 4 : ((homeCoords.Y < uc.Y) ? -4 : 0);
				const CellStruct retreatCell { static_cast<short>(uc.X + sx), static_cast<short>(uc.Y + sy) };

				if (auto const pRC = MapClass::Instance.TryGetCellAt(retreatCell))
				{
					if (pRC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
					{
						pUnit->SetDestination(pRC, true);

						pUnit->QueueMission(Mission::Move, true);

						continue;
					}
				}
			}
		}

		// Escort our lead Heavy/Siege Tank with mobile AA, Sensor, Mobile Radar
		// Jammer, GapGenerator & Mobile Stealth Generator vehicles (Slots 60 & 63)!
		const bool isUnarmedSupportEscort
			= (pPrimaryWpn == nullptr && !pType->Harvester && !pType->ResourceGatherer && pType->Passengers == 0
				&& (pType->DeploysInto == nullptr || !IsTrueBaseStructureTarget(pType->DeploysInto)));
		if (pLeadHeavyTank && pUnit != pLeadHeavyTank && isUnitIdle && !pType->Naval
			&& (isUnarmedSupportEscort
				|| (((pPrimaryWpn && pPrimaryWpn->Projectile && pPrimaryWpn->Projectile->AA) || pType->Sensors)
					&& scores[60] > 120)
				|| (pType->GapGenerator && scores[63] > 80))
			&& (Unsorted::CurrentFrame % 90 < 20))
		{
			const int distToLead = pUnit->DistanceFrom(pLeadHeavyTank);

			if (distToLead > 768 && distToLead <= 4608)
			{
				if (auto const pLeadCell = pLeadHeavyTank->GetCell())
				{
					DispatchFootToTargetOrTransport(pUnit, pLeadCell, Mission::Area_Guard, false);

					continue;
				}
			}
			else if (pType->GapGenerator && pType->IsSimpleDeployer && !pUnit->GapSuperCharged && distToLead <= 768)
			{
				pUnit->QueueMission(Mission::Unload, true);

				continue;
			}
		}

		// Slot 61 (Mirage Tank Dynamic Disguise & Counter-Disguise Interception):
		if (pType->CanDisguise && !pType->PermaDisguise && !pUnit->Disguised && pSampleEnemyInfantry
			&& pUnit->CanDisguiseAs(pSampleEnemyInfantry) && scores[61] > 80)
		{
			pUnit->DisguiseAs(pSampleEnemyInfantry);
		}

		// Slot 83 (Mirage Tank & Camouflage Tree-Grove Ambush, Spotted Retreat &
		// Ambush Zone Rotation): Position Mirage Tanks (CanDisguise /
		// PermaDisguise) and ground Stealth Ambush tanks (Cloakable && !Naval)
		// inside tree groves (GetTerrain) along enemy Harvester/reinforcement
		// corridors and mimic the exact tree species of the grove! Ambush
		// unsuspecting critical enemy units (Harvesters, Engineers, Artillery,
		// wounded vehicles) that pass within weapon range without leaving the tree
		// grove. If spotted/attacked by the enemy (HasBeenAttacked, HP < 78%,
		// targeted by enemy tanks, or inside enemy DisguiseSensors), retreat
		// immediately back to our home base / Service Depot! After regrouping or
		// after an ambush epoch (~900 frames), rotate to a different tree grove
		// sector to keep harassing the enemy from new angles!
		const bool isCamoAmbushTank =

			!pType->Naval && !pType->Harvester && !pType->ResourceGatherer
			&& (pType->CanDisguise || pType->PermaDisguise || pType->Cloakable) && pPrimaryWpn
			&& pPrimaryWpn->Damage > 0;

		if (isCamoAmbushTank && scores[83] > 60 && homeCoords != CellStruct::Empty)
		{
			const CellStruct uc = pUnit->GetMapCoords();
			auto const pUC = pUnit->GetCell();
			int adjacentTrees = 0;

			TerrainClass* pNearTree = nullptr;

			for (int dy = -1; dy <= 1; ++dy)
			{
				for (int dx = -1; dx <= 1; ++dx)
				{
					const CellStruct nc { static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy) };

					if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
					{
						if (auto const pTer = pNC->GetTerrain(false))
						{
							adjacentTrees++;

							if (!pNearTree)
								pNearTree = pTer;
						}
					}
				}
			}

			// Mimic the exact tree species of the surrounding grove when stationary!
			if ((pType->CanDisguise || pType->PermaDisguise) && pNearTree && isUnitIdle
				&& (!pUnit->Disguised || (Unsorted::CurrentFrame % 300 == 0))
				&& pUnit->CanDisguiseAs(reinterpret_cast<AbstractClass*>(pNearTree)))
			{
				pUnit->DisguiseAs(reinterpret_cast<AbstractClass*>(pNearTree));
			}

			// Check if the enemy has spotted / compromised our ambush ("si les
			// pillan")
			bool ambushSpotted = (pUnit->GetHealthPercentage() < 0.78)
				|| (pUnit->HasBeenAttacked && pUnit->GetHealthPercentage() < 0.92);

			if (!ambushSpotted && pUC && pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < 24
				&& pUC->DisguiseSensors_InclHouse(static_cast<unsigned int>(pHouse->EnemyHouseIndex)))
			{
				ambushSpotted = true;
			}

			if (!ambushSpotted)
			{
				int closeHostileTanks = 0;

				for (auto const pEU : UnitClass::Array)
				{
					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Harvester
						|| pEU->Type->ResourceGatherer)
					{
						continue;
					}

					if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral())
						continue;

					if (pEU->Target == pUnit)
					{
						ambushSpotted = true;

						break;
					}

					if (pUnit->DistanceFrom(pEU) <= 1152 && ++closeHostileTanks >= 2)
					{
						ambushSpotted = true;

						break;
					}
				}
			}

			if (ambushSpotted && uc.DistanceFrom(homeCoords) > 10.0)
			{
				pUnit->SetTarget(nullptr);

				if (pRepairDepot && pUnit->GetHealthPercentage() < 0.85)
				{
					DispatchFootToTargetOrTransport(pUnit, pRepairDepot, Mission::Enter, false);
				}
				else if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					DispatchFootToTargetOrTransport(pUnit, pHomeCell, Mission::Move, false);
				}

				continue;
			}

			// If concealed in a tree grove away from base, snipe unsuspecting
			// critical enemies that walk directly into our weapon range!
			if (!ambushSpotted && adjacentTrees >= 1 && uc.DistanceFrom(homeCoords) > 10.0)
			{
				TechnoClass* pAmbushVictim = nullptr;

				if (pVisibleEnemyHarvester && pUnit->DistanceFrom(pVisibleEnemyHarvester) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pVisibleEnemyHarvester;
				}
				else if (pEnemySpecialistInf && pUnit->DistanceFrom(pEnemySpecialistInf) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pEnemySpecialistInf;
				}
				else if (pEnemyArtilleryUnit && pUnit->DistanceFrom(pEnemyArtilleryUnit) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pEnemyArtilleryUnit;
				}
				else if (pWoundedEnemyVehicle && pUnit->DistanceFrom(pWoundedEnemyVehicle) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pWoundedEnemyVehicle;
				}
				else if (pSampleEnemyInfantry && pUnit->DistanceFrom(pSampleEnemyInfantry) <= pPrimaryWpn->Range)
				{
					pAmbushVictim = pSampleEnemyInfantry;
				}

				if (pAmbushVictim)
				{
					pUnit->SetTarget(pAmbushVictim);

					pUnit->QueueMission(Mission::Attack, true);

					continue;
				}
			}

			// Move to a tree grove along enemy Harvester/transit routes (or rotate to
			// a new grove sector every ~900 frames so the enemy cannot predict us!)
			const bool shouldRotateGrove = (Unsorted::CurrentFrame % 900 < 20);

			if (isUnitIdle && pUnit->GetHealthPercentage() >= 0.85
				&& (adjacentTrees == 0 || uc.DistanceFrom(homeCoords) <= 11.0 || shouldRotateGrove)
				&& (Unsorted::CurrentFrame % 60 < 20))
			{
				CellStruct enemyFocus = homeCoords;

				if (pVisibleEnemyHarvester)
					enemyFocus = pVisibleEnemyHarvester->GetMapCoords();
				else if (pSurgicalEnemyBld)
					enemyFocus = pSurgicalEnemyBld->GetMapCoords();
				else if (pPrioritySiegeBld)
					enemyFocus = pPrioritySiegeBld->GetMapCoords();

				const CellStruct searchCenter { static_cast<short>((homeCoords.X + enemyFocus.X) / 2),
					static_cast<short>((homeCoords.Y + enemyFocus.Y) / 2) };

				const int desiredSector = ((Unsorted::CurrentFrame / 900) + pUnit->UniqueID) % 4;
				CellClass* pBestGroveCell = nullptr;
				int bestGroveScore = -99999;

				for (int dy = -28; dy <= 28; dy += 2)
				{
					for (int dx = -28; dx <= 28; dx += 2)
					{
						const CellStruct gc { static_cast<short>(searchCenter.X + dx),
							static_cast<short>(searchCenter.Y + dy) };

						auto const pGC = MapClass::Instance.TryGetCellAt(gc);

						if (!pGC || pGC->GetTerrain(false) != nullptr
							|| !pGC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
						{
							continue;
						}

						if (std::max(std::abs(gc.X - homeCoords.X), std::abs(gc.Y - homeCoords.Y)) <= 11
							|| std::max(std::abs(gc.X - uc.X), std::abs(gc.Y - uc.Y)) <= 6)
						{
							continue;
						}

						// Count adjacent trees & nearby Ore (Harvester transit route!)
						int groveTrees = 0;
						bool nearOreRoute = (pGC->GetContainedTiberiumValue() > 0);

						for (int ty = -1; ty <= 1; ++ty)
						{
							for (int tx = -1; tx <= 1; ++tx)
							{
								const CellStruct tc2 { static_cast<short>(gc.X + tx), static_cast<short>(gc.Y + ty) };

								if (auto const pTC2 = MapClass::Instance.TryGetCellAt(tc2))
								{
									if (pTC2->GetTerrain(false) != nullptr)
										groveTrees++;

									if (pTC2->GetContainedTiberiumValue() > 0)
										nearOreRoute = true;
								}
							}
						}

						if (groveTrees == 0)
							continue;

						// Ensure outside enemy base defense range (> 11 cells)
						bool nearEnemyTurret = false;

						for (auto const pEB : BuildingClass::Array)
						{
							if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
								|| !pEB->Type->IsBaseDefense)
							{
								continue;
							}

							if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
								continue;

							const CellStruct ec = pEB->GetMapCoords();

							if (std::max(std::abs(gc.X - ec.X), std::abs(gc.Y - ec.Y)) <= 11)
							{
								nearEnemyTurret = true;

								break;
							}
						}

						if (nearEnemyTurret)
							continue;

						const int cellSector = ((dx >= 0 ? 1 : 0) + (dy >= 0 ? 2 : 0));

						int gScore = groveTrees * 35 + (nearOreRoute ? 90 : 0) + (cellSector == desiredSector ? 120 : 0)
							- (std::abs(gc.X - enemyFocus.X) + std::abs(gc.Y - enemyFocus.Y));

						if (gScore > bestGroveScore)
						{
							bestGroveScore = gScore;

							pBestGroveCell = pGC;
						}
					}
				}

				if (pBestGroveCell)
				{
					DispatchFootToTargetOrTransport(pUnit, pBestGroveCell, Mission::Move, false);

					continue;
				}
			}
		}

		if (isUnitIdle && !pType->Naval && (pType->DetectDisguise || pType->Sensors))
		{
			if (pDetectedEnemyInfiltrator && pUnit->DistanceFrom(pDetectedEnemyInfiltrator) <= 3840)
			{
				DispatchFootToTargetOrTransport(pUnit, pDetectedEnemyInfiltrator, Mission::Attack, true);

				continue;
			}

			TechnoClass* pSweepTarget = pHiddenOrStealthEnemy ? pHiddenOrStealthEnemy : pDetectedEnemyInfiltrator;

			if (pSweepTarget)
			{
				if (auto const pSweepCell = pSweepTarget->GetCell())
				{
					DispatchFootToTargetOrTransport(pUnit, pSweepCell, Mission::AttackMove, false);

					continue;
				}
			}
		}

		// Slot 62 (Slave Miner Liberation): If our unit is shooting an enslaved
		// worker (Slaved=yes) whose SlaveOwner (Slave Miner) is alive nearby,
		// redirect fire to the SlaveOwner so all 5 slaves flip to our House!
		if (auto const pCurTargetInf = abstract_cast<InfantryClass*>(pUnit->Target))
		{
			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner
				&& pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && scores[62] > 60)
			{
				DispatchFootToTargetOrTransport(pUnit, pCurTargetInf->SlaveOwner, Mission::Attack, true);

				continue;
			}
		}

		if (pEnemySlaveMiner && isUnitIdle && !pType->Naval && scores[62] > 140
			&& pUnit->DistanceFrom(pEnemySlaveMiner) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemySlaveMiner, Mission::Attack, true);

			continue;
		}

		// Slot 64 (Active Ally De-Parasiting via Sonic, RemoveParasite, or Damage <
		// 0): Dolphins (Sonic=yes), Repair IFVs (Damage < 0), and units with Phobos
		// RemoveParasite=yes immediately free allied vehicles/ships being eaten by
		// a Giant Squid or Terror Drone!
		const auto pWH = pPrimaryWpn ? pPrimaryWpn->Warhead : nullptr;
		const auto pWHExt = pWH ? WarheadTypeExt::TryFetch(pWH) : nullptr;

		const bool canCureParasite = pPrimaryWpn
			&& (pPrimaryWpn->Damage < 0 || (pWH && pWH->Sonic) || (pWHExt && pWHExt->RemoveParasite.Get(false)));

		if (pParasitedOwnUnit && pParasitedOwnUnit != pUnit && isUnitIdle && canCureParasite && scores[64] > 60
			&& pUnit->DistanceFrom(pParasitedOwnUnit) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pParasitedOwnUnit, Mission::Attack, true);

			continue;
		}

		// Slot 64 (Offensive Parasite Targeting: Terror Drone, Giant Squid,
		// Cyberworm): Target expensive enemy tanks/harvesters/warships that do NOT
		// yet have a parasite!
		if (pWH && pWH->Parasite && isUnitIdle && pBestParasiteVictim && pUnit->ParasiteImUsing == nullptr
			&& scores[64] > 60 && pType->Naval == pBestParasiteVictim->Type->Naval
			&& pUnit->DistanceFrom(pBestParasiteVictim) <= 4608)
		{
			DispatchFootToTargetOrTransport(pUnit, pBestParasiteVictim, Mission::Attack, true);

			continue;
		}

		// Slot 66 (EMP Pulse Targeting & EMP-Paralyzed Target Finisher):
		if (pWH && pWH->EMEffect && isUnitIdle && pEnemyNonEMPedHeavy && scores[66] > 80
			&& pUnit->DistanceFrom(pEnemyNonEMPedHeavy) <= 4096)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyNonEMPedHeavy, Mission::Attack, true);

			continue;
		}
		else if (pEnemyEMPedTarget && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && (!pWH || !pWH->EMEffect)
			&& scores[66] > 100 && pUnit->DistanceFrom(pEnemyEMPedTarget) <= 3584)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyEMPedTarget, Mission::Attack, true);

			continue;
		}

		// Prioritize filling any owned empty Tank Bunker with compatible idle Bunkerable tanks
		if (isUnitIdle && pType->Bunkerable && !pType->Naval && !pType->Harvester && !pType->ResourceGatherer
			&& pUnit->BunkerLinkedItem == nullptr && pPrimaryWpn && pPrimaryWpn->Damage > 0)
		{
			const int unitZoneForBunker
				= MapClass::Instance.GetMovementZoneType(pUnit->GetMapCoords(), MovementZone::Normal, false);
			BuildingClass* pBestTankBunker = nullptr;
			int bestTankBunkerDist = 4096;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Bunker
					|| pBld->BunkerLinkedItem != nullptr)
				{
					continue;
				}

				const int bz
					= MapClass::Instance.GetMovementZoneType(pBld->GetMapCoords(), MovementZone::Normal, false);

				if (unitZoneForBunker >= 0 && bz >= 0 && bz != unitZoneForBunker)
					continue;

				bool anotherTankAlreadyEntering = false;

				for (auto const pOtherU : UnitClass::Array)
				{
					if (pOtherU && pOtherU != pUnit && pOtherU->Owner == pHouse && !pOtherU->InLimbo && pOtherU->IsAlive
						&& (pOtherU->CurrentMission == Mission::Enter || pOtherU->QueuedMission == Mission::Enter)
						&& (pOtherU->Destination == pBld || pOtherU->ArchiveTarget == pBld
							|| pOtherU->ContainsLink(pBld)))
					{
						anotherTankAlreadyEntering = true;
						break;
					}
				}

				if (anotherTankAlreadyEntering)
					continue;

				const int d = pUnit->DistanceFrom(pBld);

				if (d <= bestTankBunkerDist)
				{
					bestTankBunkerDist = d;
					pBestTankBunker = pBld;
				}
			}

			if (pBestTankBunker)
			{
				OrderFootToEnter(pUnit, pBestTankBunker);
				continue;
			}
		}

		// Slot 68 (Psychedelic Chaos Gas): Target non-berserk heavy enemy tanks!
		if (pWH && pWH->Psychedelic && isUnitIdle && pEnemyChaosTarget && scores[68] > 80
			&& pUnit->DistanceFrom(pEnemyChaosTarget) <= 3584)
		{
			DispatchFootToTargetOrTransport(pUnit, pEnemyChaosTarget, Mission::Attack, true);

			continue;
		}

		// Slot 69 (Submarine Cloak Ambush & Anti-Submarine Destroyer Escort):
		if (pType->Naval && scores[69] > 80)
		{
			if (pType->Cloakable && pUnit->GetHealthPercentage() < 0.45 && homeCoords != CellStruct::Empty
				&& pUnit->CurrentMission != Mission::Move)
			{
				if (auto const pHC = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					pUnit->SetDestination(pHC, true);

					pUnit->QueueMission(Mission::Move, true);

					continue;
				}
			}
			else if (isUnitIdle && pEnemySubOrShip && (pType->Sensors || (pPrimaryWpn && pPrimaryWpn->Damage > 0)))
			{
				if (auto const pSubCell = pEnemySubOrShip->GetCell())
				{
					if (pType->Sensors || IsCellVisibleToAI(pHouse, pSubCell))
					{
						OrderFootToAttackMove(pUnit, pSubCell);

						continue;
					}
				}
			}
			else if (pType->Sensors && pLeadWarship && pUnit != pLeadWarship && isUnitIdle
				&& (Unsorted::CurrentFrame % 90 < 20))
			{
				const int distW = pUnit->DistanceFrom(pLeadWarship);

				if (distW > 768 && distW <= 4608)
				{
					if (auto const pWC = pLeadWarship->GetCell())
					{
						pUnit->SetDestination(pWC, true);

						pUnit->QueueMission(Mission::Area_Guard, true);

						continue;
					}
				}
			}
		}

		// Field Repair Vehicles (Repair IFV / Mobile Mechanic / Support Drone where
		// Damage < 0): Heal damaged own OR allied vehicles in the field!
		if (pPrimaryWpn && pPrimaryWpn->Damage < 0 && isUnitIdle)
		{
			for (auto const pAllyU : UnitClass::Array)
			{
				if (!pAllyU || pAllyU == pUnit || !pAllyU->Owner || !pHouse->IsAlliedWith(pAllyU->Owner)
					|| pAllyU->InLimbo || !pAllyU->IsAlive || !pAllyU->Type || pAllyU->Type->Organic)
				{
					continue;
				}

				if ((pAllyU->ParasiteEatingMe != nullptr || pAllyU->GetHealthPercentage() < 0.85)
					&& pUnit->DistanceFrom(pAllyU) <= 3584)
				{
					DispatchFootToTargetOrTransport(pUnit, pAllyU, Mission::Attack, true);

					break;
				}
			}

			continue;
		}

		// Yuri Floating Disc (DrainWeapon=yes): Prioritize draining Enemy
		// Superweapons, Refineries (steal credits!), and Power Plants (blackout!)
		if (pPrimaryWpn && pPrimaryWpn->DrainWeapon && isUnitIdle && pUnit->DrainTarget == nullptr)
		{
			BuildingClass* pBestDrainBld = nullptr;
			int bestDrainScore = -1;

			for (auto const pEBld : BuildingClass::Array)
			{
				if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Owner || !pEBld->Type
					|| pEBld->DrainingMe != nullptr)
				{
					continue;
				}

				if (pEBld->Owner == pHouse || pHouse->IsAlliedWith(pEBld->Owner) || pEBld->Owner->IsNeutral())
					continue;

				if (!pEBld->GetCell() || !IsCellVisibleToAI(pHouse, pEBld->GetCell()))
					continue;

				int dScore = 0;

				if (pEBld->Type->SuperWeapon != -1)
					dScore = 100;
				else if (pEBld->Type->Refinery)
					dScore = 90;
				else if (pEBld->Type->PowerBonus > 0)
					dScore = 85;
				else if (pEBld->Type->IsBaseDefense && pEBld->Type->Powered)
					dScore = 75;

				if (dScore > bestDrainScore)
				{
					bestDrainScore = dScore;

					pBestDrainBld = pEBld;
				}
			}

			if (pBestDrainBld)
			{
				DispatchFootToTargetOrTransport(pUnit, pBestDrainBld, Mission::Attack, true);

				continue;
			}
		}

		// Generalized Deployable OpenTopped Bunker Vehicles across any mod
		// (IsSimpleDeployer with Convert.Deploy / Convert.UniversalDeploy ->
		// OpenTopped transport/bunker): Before deploying into an immobile
		// OpenTopped bunker, ensure we are >= 4 cells away from any
		// Factory/Refinery/ConYard exit so we never block production doors!
		if (isUnitIdle && !pType->Naval && pType->IsSimpleDeployer && !pType->OpenTopped
			&& (Unsorted::CurrentFrame % 90 < 25))
		{
			TechnoTypeClass* pConvTarget = GetConvertDeployTargetType(pType);
			if (pConvTarget && pConvTarget->OpenTopped && pConvTarget->Passengers > 0 && !HasNoManualEnter(pConvTarget)
				&& !HasNoManualUnload(pConvTarget))
			{
				const CellStruct uc = pUnit->GetMapCoords();
				bool tooCloseToBaseDoor = false;
				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
						continue;

					if (pOwnB->Type->Factory != AbstractType::None || pOwnB->Type->Refinery
						|| pOwnB->Type->ConstructionYard)
					{
						const CellStruct bc = pOwnB->GetMapCoords();
						const int bw = pOwnB->Type->GetFoundationWidth();
						const int bh = pOwnB->Type->GetFoundationHeight(false);
						if (uc.X >= bc.X - 3 && uc.X <= bc.X + bw + 2 && uc.Y >= bc.Y - 3 && uc.Y <= bc.Y + bh + 3)
						{
							tooCloseToBaseDoor = true;
							break;
						}
					}
				}

				if (tooCloseToBaseDoor)
				{
					pUnit->Scatter(CoordStruct::Empty, true, false);
					continue;
				}

				int nearbyFriendlyInf = 0;
				for (auto const pOwnI : InfantryClass::Array)
				{
					if (pOwnI && pOwnI->Owner == pHouse && !pOwnI->InLimbo && pOwnI->IsAlive && pOwnI->Type
						&& !pOwnI->Type->Engineer && !pOwnI->Type->Infiltrate && pOwnI->DistanceFrom(pUnit) <= 2816)
					{
						nearbyFriendlyInf++;
					}
				}

				if (nearbyFriendlyInf >= 2 || pUnit->Passengers.NumPassengers > 0)
				{
					pUnit->QueueMission(Mission::Unload, true);
					continue;
				}
			}
		}

		// Armed Vehicles (any unit with Size <= transport SizeLimit) boarding
		// OpenTopped transports (Slot 53):
		const int unitPassSize = std::max(1, static_cast<int>(pType->TechnoTypeClass::Size));
		if (isUnitIdle && !pType->Naval && !pType->Harvester && !pType->ResourceGatherer && !pType->OpenTopped
			&& !pType->Gunner && pType->Passengers == 0 && pType->DeploysInto == nullptr
			&& GetConvertDeployTargetType(pType) == nullptr && pPrimaryWpn && pPrimaryWpn->Damage > 0
			&& (Unsorted::CurrentFrame % 90 < 25))
		{
			bool unitBoardedOpenTopped = false;
			for (auto const pOTVeh : UnitClass::Array)
			{
				if (!pOTVeh || pOTVeh == pUnit || pOTVeh->Owner != pHouse || pOTVeh->InLimbo || !pOTVeh->IsAlive
					|| !pOTVeh->Type)
				{
					continue;
				}

				if (!pOTVeh->Type->OpenTopped || HasNoManualEnter(pOTVeh->Type) || HasNoManualUnload(pOTVeh->Type))
					continue;

				if (unitPassSize > static_cast<int>(pOTVeh->Type->SizeLimit))
					continue;

				int pendingCount = 0;
				int pendingSize = 0;
				for (auto const pOtherI : InfantryClass::Array)
				{
					if (pOtherI && pOtherI->Owner == pHouse && !pOtherI->InLimbo && pOtherI->IsAlive
						&& pOtherI->CurrentMission == Mission::Enter && pOtherI->Destination == pOTVeh)
					{
						pendingCount++;
						pendingSize += std::max(1, static_cast<int>(pOtherI->Type->TechnoTypeClass::Size));
					}
				}

				for (auto const pOtherU : UnitClass::Array)
				{
					if (pOtherU && pOtherU != pUnit && pOtherU->Owner == pHouse && !pOtherU->InLimbo && pOtherU->IsAlive
						&& pOtherU->CurrentMission == Mission::Enter && pOtherU->Destination == pOTVeh)
					{
						pendingCount++;
						pendingSize += std::max(1, static_cast<int>(pOtherU->Type->TechnoTypeClass::Size));
					}
				}

				if (CanPassengerFitInTransport(pOTVeh, pType, pendingCount, pendingSize)
					&& pUnit->DistanceFrom(pOTVeh) <= 2560)
				{
					DispatchFootToTargetOrTransport(pUnit, pOTVeh, Mission::Enter, false);
					unitBoardedOpenTopped = true;
					break;
				}
			}

			if (unitBoardedOpenTopped)
				continue;
		}

		// Tank Bunker Garrisoning (Bunkerable=yes into empty owned Tank Bunker):
		// Fortify base perimeter!
		if (pType->Bunkerable && isUnitIdle && pUnit->BunkerLinkedItem == nullptr && (Unsorted::CurrentFrame % 90 < 20))
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Bunker
					|| pBld->BunkerLinkedItem != nullptr)
				{
					continue;
				}

				if (pUnit->DistanceFrom(pBld) <= 3072)
				{
					DispatchFootToTargetOrTransport(pUnit, pBld, Mission::Enter, false);

					break;
				}
			}
		}

		// Respond to allied Beacon with idle combat units
		if (pBeaconCell && isUnitIdle)
		{
			DispatchFootToTargetOrTransport(pUnit, pBeaconCell, Mission::AttackMove, false);
		}
	}

	// Special Operations: Spies, Bridge Repair, Urban Garrisoning, Bio-Reactors,
	// Tesla Overpowering, Hospitals, Medics, Crazy Ivan & Bridge Demolition
	BuildingClass* pTargetBridgeHut = nullptr;
	BuildingClass* pBioReactor = nullptr;
	BuildingClass* pOverpowerCoil = nullptr;
	BuildingClass* pHospital = nullptr;

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
			continue;

		if (scores[21] > 400 && !pTargetBridgeHut && pBld->Type->BridgeRepairHut
			&& MapClass::Instance.IsLinkedBridgeDestroyed(pBld->GetMapCoords()))
		{
			pTargetBridgeHut = pBld;
		}

		if (pBld->Owner && pHouse->IsAlliedWith(pBld->Owner))
		{
			if (pBld->Owner == pHouse && pBld->Type->InfantryAbsorb
				&& (pBld->Type->PowerBonus > 0 || pBld->Type->ExtraPowerBonus > 0)
				&& !IsTunnelNetworkBuilding(pBld->Type) && pBld->Passengers.NumPassengers < pBld->Type->Passengers)
			{
				if (!pBioReactor || pBld->Passengers.NumPassengers < pBioReactor->Passengers.NumPassengers)
					pBioReactor = pBld;
			}

			if (!pOverpowerCoil && pBld->Type->Overpowerable && (!pBld->IsOverpowered || pBld->Owner->HasLowPower()))
				pOverpowerCoil = pBld; // Overpower own OR allied Tesla Coils!

			if (!pHospital && pBld->Type->Hospital)
				pHospital = pBld; // Use own OR allied Hospital!
		}
	}

	int infAlreadyHeadingToBioReactor = 0;
	if (pBioReactor)
	{
		for (auto const pCheckInf : InfantryClass::Array)
		{
			if (pCheckInf && pCheckInf->Owner == pHouse && !pCheckInf->InLimbo && pCheckInf->IsAlive
				&& pCheckInf->Destination == pBioReactor)
			{
				infAlreadyHeadingToBioReactor++;
			}
		}

		if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor >= pBioReactor->Type->Passengers)
			pBioReactor = nullptr;
	}

	const bool bioReactorNeedsPower
		= pBioReactor && (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350);
	if (false)
	{
		{
		}
	}

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type)
			continue;

		// Evade radiation puddles (Desolator / Nuke fallout) immediately if not
		// radiation-immune; if we ARE a radiation-immune Deployer (Desolator,
		// Slot 63), tactically deploy to irradiate nearby enemy squads/slaves and
		// undeploy once the zone is cleared!
		if (auto const pInfCell = pInf->GetCell())
		{
			const int cellRad = pInfCell->GetRadLevel();

			if (cellRad > 10 && !pInf->Type->ImmuneToRadiation && pInf->CurrentMission != Mission::Move)
			{
				pInf->Scatter(CoordStruct::Empty, true, false);

				continue;
			}
			else if (pInf->Type->Deployer && pInf->Type->ImmuneToRadiation && scores[63] > 80
				&& (Unsorted::CurrentFrame % 45 < 15))
			{
				int enemiesInRadRange = 0;

				for (auto const pET : TechnoClass::Array)
				{
					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner || pET->WhatAmI() == AbstractType::Building
						|| pET->WhatAmI() == AbstractType::Aircraft)
					{
						continue;
					}

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
						continue;

					if (pInf->DistanceFrom(pET) <= 1536)
						enemiesInRadRange++;
				}

				if (!pInf->IsDeployed() && enemiesInRadRange >= 2 && cellRad < 180)
				{
					pInf->QueueMission(Mission::Unload, true);

					continue;
				}
				else if (pInf->IsDeployed() && enemiesInRadRange == 0 && cellRad >= 100)
				{
					pInf->QueueMission(Mission::Unload, true);

					continue;
				}
			}
		}

		UnstickCrossIslandFoot(pInf);
		const bool isIdle = (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard)
			&& !IsFootActivelyAttackingOrMovingToEnemy(pInf);

		const auto pInfWpn = pInf->Type->GetWeapon(0) ? pInf->Type->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Infantry Human Shield & Anti-Engineer Recapture Screen for
		// Stolen ConYard / Escaping MCV): Nearby friendly combat infantry (<= 14
		// cells) immediately gun down enemy Engineers trying to recapture our
		// stolen ConYard, or form a human shield perimeter around the stolen
		// ConYard/MCV to absorb enemy fire during extraction!
		if (pStolenConYardOrMCV && !pInf->Type->Engineer && !pInf->Type->Infiltrate && scores[82] > 60
			&& pInf->DistanceFrom(pStolenConYardOrMCV) <= 3584)
		{
			if (pThreatToStolenAsset && pInfWpn && pInfWpn->Damage > 0 && pInf->Target != pThreatToStolenAsset)
			{
				DispatchFootToTargetOrTransport(pInf, pThreatToStolenAsset, Mission::Attack, true);

				continue;
			}
			else if (isIdle && pInf->DistanceFrom(pStolenConYardOrMCV) > 512)
			{
				if (auto const pShieldCell = pStolenConYardOrMCV->GetCell())
				{
					DispatchFootToTargetOrTransport(pInf, pShieldCell, Mission::Area_Guard, false);

					continue;
				}
			}
		}

		// Slot 57 (Combat Infantry Deploy): Deploy GI / Guardian GI / Desolator
		// (Deployer=yes) when actively engaging an enemy in combat!
		if (pInf->Type->Deployer && pInf->Target != nullptr && pInf->CurrentMission == Mission::Attack
			&& scores[57] > 100 && (Unsorted::CurrentFrame % 60 < 20))
		{
			pInf->QueueMission(Mission::Unload, true);

			continue;
		}

		// Slot 58 (Mind-Control Breaker): Snipers / Virus / ImmuneToPsionics
		// infantry eliminate enemy Yuri/Mastermind controllers!
		if (pEnemyMindController && isIdle && (pInf->Type->ImmuneToPsionics || (pInfWpn && pInfWpn->Range >= (6 * 256)))
			&& pInf->DistanceFrom(pEnemyMindController) <= 4096)
		{
			DispatchFootToTargetOrTransport(pInf, pEnemyMindController, Mission::Attack, true);

			continue;
		}

		// Slot 59 (Crate Pickup): Send nearest idle infantry to pick up a visible
		// map Crate!
		if (pVisibleCrateCell && isIdle && !pInf->Type->Engineer && pInf->DistanceFrom(pVisibleCrateCell) <= 3072)
		{
			DispatchFootToTargetOrTransport(pInf, pVisibleCrateCell, Mission::Move, false);

			pVisibleCrateCell = nullptr;

			continue;
		}

		// Slot 65 (Grinder Mind-Control Recycling for Infantry): Send
		// mind-controlled infantry into our Grinder for instant credits and to free
		// Yuri's control slot!
		if (pInf->MindControlledBy != nullptr)
		{
			if (pBioReactor && pInf->Destination != pBioReactor
				&& (bioReactorNeedsPower || !pGrinderBld
					|| pInf->DistanceFrom(pBioReactor) <= pInf->DistanceFrom(pGrinderBld) + 1536))
			{
				DispatchFootToTargetOrTransport(pInf, pBioReactor, Mission::Enter, false);
				infAlreadyHeadingToBioReactor++;
				if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor
					>= pBioReactor->Type->Passengers)
				{
					pBioReactor = nullptr;
				}

				continue;
			}
		}

		if (pGrinderBld && pInf->MindControlledBy != nullptr && scores[65] > 80)
		{
			DispatchFootToTargetOrTransport(pInf, pGrinderBld, Mission::Enter, false);

			continue;
		}

		// Slot 68 (TiberiumHeal Infantry Regeneration & Sniper/Poison Specialist
		// Hunter):
		if (pInf->Type->TiberiumHeal && pInf->GetHealthPercentage() < 0.55 && scores[68] > 60
			&& pInf->CurrentMission != Mission::Move)
		{
			if (pInf->MoveToTiberium(8, false))
				continue;
		}

		if (pEnemySpecialistInf && isIdle && pInfWpn && pInfWpn->Damage > 0 && !pInf->Type->Engineer
			&& ((pInfWpn->Warhead && pInfWpn->Warhead->Poison) || pInfWpn->Range >= (6 * 256)) && scores[68] > 80
			&& pInf->DistanceFrom(pEnemySpecialistInf) <= 4096)
		{
			DispatchFootToTargetOrTransport(pInf, pEnemySpecialistInf, Mission::Attack, true);

			continue;
		}

		// Retreat wounded valuable/Elite infantry (< 55% HP) to Hospital if owned!
		if (pHospital && !pInf->Type->Engineer && (pInf->Type->Cost >= 300 || pInf->Veterancy.IsElite())
			&& pInf->GetHealthPercentage() < 0.55)
		{
			if (pInf->CurrentMission != Mission::Enter && pInf->DistanceFrom(pHospital) <= 4096)
			{
				DispatchFootToTargetOrTransport(pInf, pHospital, Mission::Enter, false);

				continue;
			}
		}

		// Combat Medics / Field Mechanics (Damage < 0): Heal wounded own OR allied
		// infantry nearby!
		if (pInfWpn && pInfWpn->Damage < 0 && isIdle)
		{
			for (auto const pAllyInf : InfantryClass::Array)
			{
				if (!pAllyInf || pAllyInf == pInf || !pAllyInf->Owner || !pHouse->IsAlliedWith(pAllyInf->Owner)
					|| pAllyInf->InLimbo || !pAllyInf->IsAlive)
				{
					continue;
				}

				if (pAllyInf->GetHealthPercentage() < 0.85 && pInf->DistanceFrom(pAllyInf) <= 2560)
				{
					DispatchFootToTargetOrTransport(pInf, pAllyInf, Mission::Attack, true);

					break;
				}
			}

			continue;
		}

		// Chrono Legionnaire (Temporal=yes) & Crazy Ivan (Ivan=yes): Prioritize
		// high-cost enemy units/buildings!
		if (isIdle && ((pInfWpn && pInfWpn->Warhead && pInfWpn->Warhead->Temporal) || pInf->Type->Ivan))
		{
			TechnoClass* pBestSpecTarget = nullptr;
			int bestSpecCost = 800;

			for (auto const pEnemyU : UnitClass::Array)
			{
				if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
					continue;

				if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
					continue;

				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell())
					&& pEnemyU->Type->Cost > bestSpecCost && pInf->DistanceFrom(pEnemyU) <= 4608)
				{
					bestSpecCost = pEnemyU->Type->Cost;

					pBestSpecTarget = pEnemyU;
				}
			}

			if (pBestSpecTarget)
			{
				DispatchFootToTargetOrTransport(pInf, pBestSpecTarget, Mission::Attack, true);

				continue;
			}
		}

		// Send idle Engineer to disarm Ivan Bombs on our own OR allied
		// buildings/tanks, repair destroyed bridges, or capture Neutral Tech
		// Buildings (Oil Derricks, Tech Outposts, Airports) and damaged enemy
		// buildings (respecting Multi-Engineer)
		if (pInf->Type->Engineer && isIdle)
		{
			// Disarm any attached Crazy Ivan Bomb on our own OR allied high-value
			// buildings!
			bool disarmedBomb = false;

			for (auto const pAllyBld : BuildingClass::Array)
			{
				if (pAllyBld && !pAllyBld->InLimbo && pAllyBld->IsAlive && pAllyBld->Owner
					&& pHouse->IsAlliedWith(pAllyBld->Owner) && pAllyBld->AttachedBomb != nullptr
					&& pInf->DistanceFrom(pAllyBld) <= 3584)
				{
					DispatchFootToTargetOrTransport(pInf, pAllyBld, Mission::Attack, true);

					disarmedBomb = true;

					break;
				}
			}

			if (disarmedBomb)
				continue;

			if (pTargetBridgeHut)
			{
				DispatchFootToTargetOrTransport(pInf, pTargetBridgeHut, Mission::Enter, false);

				pTargetBridgeHut = nullptr; // One engineer per hut is enough

				continue;
			}

			// Repair heavily damaged friendly-occupied or empty urban garrisonable
			// buildings (CanBeOccupied && Repairable) to 100% full health so our
			// infantry hold an unbreakable urban stronghold!
			if (pDamagedUrbanToRepair && pInf->DistanceFrom(pDamagedUrbanToRepair) <= 4096)
			{
				DispatchFootToTargetOrTransport(pInf, pDamagedUrbanToRepair, Mission::Enter, false);

				pDamagedUrbanToRepair = nullptr;

				continue;
			}

			const double capLevel = RulesClass::Instance ? RulesClass::Instance->EngineerCaptureLevel : 1.0;
			const int engCaptureBias = NeuralAI::PolicyFor(pHouse).OutputBias[36];
			BuildingClass* pBestCapBld = nullptr;
			int bestCapDist = 999999;

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner)
					continue;

				if (!pBld->Type->Capturable || pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner))
					continue;

				auto const pCell = pBld->GetCell();

				if (!pCell)
					continue;

				const bool isNeutralTech = pBld->Owner->IsNeutral();

				// Neutral map tech structures (Oil Derricks, Hospitals, Outposts,
				// Secret Labs) are known map landmarks; Enemy buildings require
				// line-of-sight or active Engineer raid bias.
				if (!isNeutralTech && !IsCellVisibleToAI(pHouse, pCell))
					continue;

				const bool isCashTechBld = (pBld->Type->ProduceCashAmount > 0 || pBld->Type->ProduceCashStartup > 0);

				const bool isHighValueEnemyBld = !isNeutralTech
					&& (isCashTechBld || pBld->Type->ConstructionYard || pBld->Type->Refinery
						|| pBld->Type->Factory != AbstractType::None || pBld->Type->SuperWeapon != -1
						|| pBld->Type->PowerBonus > 0);

				const bool canCaptureNow = isNeutralTech || (pBld->GetHealthPercentage() <= capLevel + 0.05)
					|| (engCaptureBias > 60 && isHighValueEnemyBld)
					|| (scores[82] > 60 && pBld->Type->ConstructionYard);

				if (!canCaptureNow)
					continue;

				// Coordinate multiple Engineers: do not send a 2nd Engineer to a
				// neutral tech building already targeted by another friendly Engineer!
				if (isNeutralTech)
				{
					bool alreadyTargetedByOtherEng = false;
					for (auto const pOtherInf : InfantryClass::Array)
					{
						if (pOtherInf && pOtherInf != pInf && pOtherInf->Owner == pHouse && !pOtherInf->InLimbo
							&& pOtherInf->IsAlive && pOtherInf->Type && pOtherInf->Type->Engineer
							&& pOtherInf->Destination == pBld)
						{
							alreadyTargetedByOtherEng = true;
							break;
						}
					}

					if (alreadyTargetedByOtherEng)
						continue;
				}

				const int dist = pInf->DistanceFrom(pBld);

				// Strongly prioritize Cash-Producing Oil Derricks (ProduceCashAmount >
				// and BaseNormal expansion hubs!
				const bool isStrategicTechHub = isCashTechBld || BuildingTypeExt::IsAIBaseNormal(pBld->Type);
				const int effectiveDist
					= (!isNeutralTech && (pBld->Type->ConstructionYard || isCashTechBld) && scores[82] > 60)
					? (dist / 3)
					: (isNeutralTech ? (isCashTechBld ? (dist / 3) : (isStrategicTechHub ? (dist * 2 / 5) : (dist / 2)))
									: dist);

				if (effectiveDist < bestCapDist)
				{
					bestCapDist = effectiveDist;

					pBestCapBld = pBld;
				}
			}

			if (pBestCapBld)
			{
				DispatchFootToTargetOrTransport(pInf, pBestCapBld, Mission::Enter, false);
			}

			continue;
		}

		// Slot 61 (Dynamic Spy Disguise via DisguiseAs): Ensure our Spy disguises
		// specifically as an enemy infantryman rather than a friendly/default unit!
		if (pInf->Type->CanDisguise && pSampleEnemyInfantry && scores[61] > 60
			&& (!pInf->Disguised || !pInf->DisguisedAsHouse || pHouse->IsAlliedWith(pInf->DisguisedAsHouse))
			&& pInf->CanDisguiseAs(pSampleEnemyInfantry))
		{
			pInf->DisguiseAs(pSampleEnemyInfantry);
		}

		// Slot 62 (Slave Miner Liberation for Infantry): Redirect fire from Slaved
		// workers onto their SlaveOwner so killing the miner frees the slaves!
		if (auto const pCurTargetInf = abstract_cast<InfantryClass*>(pInf->Target))
		{
			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner
				&& pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && scores[62] > 60)
			{
				DispatchFootToTargetOrTransport(pInf, pCurTargetInf->SlaveOwner, Mission::Attack, true);

				continue;
			}
		}

		// Spy Full-Spectrum Sabotage, Multi-Spy Role Coordination & Pre-Attack Power Plant Staging:
		// 1) Multiple Spies coordinate so each targets a distinct building/role:
		//    - Un-infiltrated War Factory / Barracks so newly built units spawn Veteran!
		//    - Enemy Radar when our AI lacks SpySat / radar vision on that enemy!
		//    - Enemy Refinery / Silos when we need credits and the enemy has funds!
		//    - Lightly guarded Enemy Power Plants to black out powered defenses!
		//    - Enemy Superweapons & Battle Labs (Secret Tech)!
		// 2) Pre-Attack Power Plant Blackout Staging: When targeting an enemy Power Plant
		//    while our army is assembling (not yet attacking that enemy house), wait disguised
		//    5..10 cells away if safe from detectors, and immediately infiltrate as soon as
		//    our attack wave launches/approaches (or if an enemy detector gets close)!
		if (pInf->Type->Infiltrate)
		{
			BuildingClass* pStagedPowerBld = abstract_cast<BuildingClass*>(
				pInf->Destination ? pInf->Destination : pInf->ArchiveTarget);

			if (!pStagedPowerBld || !pStagedPowerBld->IsAlive || pStagedPowerBld->InLimbo || !pStagedPowerBld->Owner
				|| pHouse->IsAlliedWith(pStagedPowerBld->Owner) || pStagedPowerBld->Owner->IsNeutral()
				|| !pStagedPowerBld->Type || pStagedPowerBld->Type->PowerBonus <= pStagedPowerBld->Type->PowerDrain)
			{
				pStagedPowerBld = nullptr;

				if (isIdle)
				{
					for (auto const pCandPwr : BuildingClass::Array)
					{
						if (!pCandPwr || pCandPwr->InLimbo || !pCandPwr->IsAlive || !pCandPwr->Owner || !pCandPwr->Type)
							continue;

						if (pHouse->IsAlliedWith(pCandPwr->Owner) || pCandPwr->Owner->IsNeutral())
							continue;

						if (pCandPwr->Type->Spyable && pCandPwr->Type->PowerBonus > pCandPwr->Type->PowerDrain
							&& pInf->DistanceFrom(pCandPwr) <= 2816)
						{
							pStagedPowerBld = pCandPwr;
							break;
						}
					}
				}
			}

			if (pStagedPowerBld && pInf->Disguised)
			{
				const int distToPwr = pInf->DistanceFrom(pStagedPowerBld);

				if (distToPwr >= 1024 && distToPwr <= 2816)
				{
					HouseClass* const pPwrOwner = pStagedPowerBld->Owner;
					bool detectorDangerNearSpy = pInf->HasBeenAttacked;

					if (!detectorDangerNearSpy)
					{
						for (auto const pDetI : InfantryClass::Array)
						{
							if (!pDetI || pDetI->InLimbo || !pDetI->IsAlive || !pDetI->Owner || !pDetI->Type)
								continue;

							if (pHouse->IsAlliedWith(pDetI->Owner) || pDetI->Owner->IsNeutral())
								continue;

							if ((pDetI->Type->Doggie || pDetI->Type->DetectDisguise || pDetI->Type->Sensors)
								&& pInf->DistanceFrom(pDetI) <= 1664)
							{
								detectorDangerNearSpy = true;
								break;
							}
						}
					}

					if (!detectorDangerNearSpy)
					{
						for (auto const pDetU : UnitClass::Array)
						{
							if (!pDetU || pDetU->InLimbo || !pDetU->IsAlive || !pDetU->Owner || !pDetU->Type)
								continue;

							if (pHouse->IsAlliedWith(pDetU->Owner) || pDetU->Owner->IsNeutral())
								continue;

							if (pDetU->Type->Sensors && pInf->DistanceFrom(pDetU) <= 1664)
							{
								detectorDangerNearSpy = true;
								break;
							}
						}
					}

					bool enemyHasPoweredDefenses = false;
					for (auto const pEB : pPwrOwner->Buildings)
					{
						if (pEB && !pEB->InLimbo && pEB->IsAlive && pEB->Type && pEB->Type->IsBaseDefense
							&& (pEB->Type->Powered || pEB->Type->PoweredSpecial))
						{
							enemyHasPoweredDefenses = true;
							break;
						}
					}

					int friendlyWaveApproachingEnemy = 0;
					const CellStruct pwrCoords = pStagedPowerBld->GetMapCoords();

					for (auto const pOwnU : UnitClass::Array)
					{
						if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type
							|| pOwnU->Type->Harvester || pOwnU->Type->ResourceGatherer)
						{
							continue;
						}

						if (pOwnU->GetMapCoords().DistanceFrom(pwrCoords) <= 26.0
							|| (IsFootActivelyAttackingOrMovingToEnemy(pOwnU)
								&& pOwnU->GetMapCoords().DistanceFrom(pwrCoords) <= 38.0))
						{
							friendlyWaveApproachingEnemy++;
							if (friendlyWaveApproachingEnemy >= 2)
								break;
						}
					}

					const bool blackoutAlreadyActive = (pPwrOwner->PowerBlackoutTimer.GetTimeLeft() > 150);
					const bool shouldWaitForWave = !detectorDangerNearSpy && enemyHasPoweredDefenses
						&& !blackoutAlreadyActive && friendlyWaveApproachingEnemy < 2
						&& (pHouse->OwnedUnits + pHouse->OwnedInfantry >= 4) && distToPwr >= 1280 && distToPwr <= 2560;

					if (shouldWaitForWave)
					{
						if (pInf->CurrentMission != Mission::Guard)
						{
							pInf->SetDestination(nullptr, true);
							pInf->QueueMission(Mission::Guard, true);
						}

						continue;
					}
					else if (isIdle && !blackoutAlreadyActive)
					{
						DispatchFootToTargetOrTransport(pInf, pStagedPowerBld, Mission::Enter, false);
						pExt->AIBrain_InfiltratedEnemyMask |= (1 << std::clamp(pPwrOwner->ArrayIndex, 0, 7));
						continue;
					}
				}
			}
		}

		if (pInf->Type->Infiltrate && isIdle)
		{
			BuildingClass* pBestSpyTarget = nullptr;
			int bestSpyScore = -9999;
			const int ourMoney = pHouse->Available_Money();

			for (auto const pEnemyBld : BuildingClass::Array)
			{
				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;

				if (!pEnemyBld->Type->Spyable || pHouse->IsAlliedWith(pEnemyBld->Owner)
					|| pEnemyBld->Owner->IsNeutral())
				{
					continue;
				}

				HouseClass* const pEnemyOwner = pEnemyBld->Owner;
				int spyScore = 0;

				if (pEnemyBld->Type->SuperWeapon != -1)
				{
					spyScore = enemySuperImminent ? 160 : 125;
				}
				else if (pEnemyBld->Type->Factory == AbstractType::UnitType && !pEnemyBld->Type->Naval)
				{
					// Unlock Veteran Vehicle production if our House hasn't infiltrated a War Factory yet!
					spyScore = !pHouse->WarFactoryInfiltrated ? 145 : 15;
				}
				else if (pEnemyBld->Type->Factory == AbstractType::InfantryType)
				{
					// Unlock Veteran Infantry production if our House hasn't infiltrated a Barracks yet!
					spyScore = !pHouse->BarracksInfiltrated ? 135 : 15;
				}
				else if (pEnemyBld->Type->Factory == AbstractType::AircraftType || pEnemyBld->Type->Helipad)
				{
					spyScore = 95;
				}
				else if (pEnemyBld->Type->Radar || pEnemyBld->Type->SpySat)
				{
					// Gain enemy Radar / map vision when our AI lacks SpySat and hasn't spied this enemy's radar!
					const bool alreadyHaveRadarVision
						= pHouse->SpySatActive || pEnemyOwner->RadarVisibleTo.Contains(pHouse->ArrayIndex);
					spyScore = !alreadyHaveRadarVision ? 130 : 35;
				}
				else if (pEnemyBld->Type->Refinery || pEnemyBld->Type->Storage > 0)
				{
					const int enemyMoney = pEnemyOwner->Available_Money();
					if (enemyMoney < 300)
						spyScore = 25;
					else if (ourMoney < 2500 && enemyMoney >= 800)
						spyScore = 150;
					else
						spyScore = 105;
				}
				else if (pEnemyBld->Type->PowerBonus > pEnemyBld->Type->PowerDrain)
				{
					if (pEnemyOwner->PowerBlackoutTimer.GetTimeLeft() > 150)
					{
						spyScore = 15;
					}
					else
					{
						bool ownerHasPoweredDef = false;
						for (auto const pEB : pEnemyOwner->Buildings)
						{
							if (pEB && !pEB->InLimbo && pEB->IsAlive && pEB->Type && pEB->Type->IsBaseDefense
								&& (pEB->Type->Powered || pEB->Type->PoweredSpecial))
							{
								ownerHasPoweredDef = true;
								break;
							}
						}

						spyScore = ownerHasPoweredDef ? 142 : 115;
					}
				}
				else if (pEnemyBld->Type->TechLevel >= 6)
				{
					const int eSide = pEnemyOwner->SideIndex;
					const bool needStolenTech = (eSide == 0 && !pHouse->Side0TechInfiltrated)
						|| (eSide == 1 && !pHouse->Side1TechInfiltrated)
						|| (eSide == 2 && !pHouse->Side2TechInfiltrated);
					spyScore = needStolenTech ? 128 : 55;
				}

				if (spyScore <= 0)
					continue;

				// De-duplicate across multiple friendly Spies so each Spy picks a distinct target/role!
				for (auto const pOtherSpy : InfantryClass::Array)
				{
					if (!pOtherSpy || pOtherSpy == pInf || pOtherSpy->Owner != pHouse || pOtherSpy->InLimbo
						|| !pOtherSpy->IsAlive || !pOtherSpy->Type || !pOtherSpy->Type->Infiltrate)
					{
						continue;
					}

					if (pOtherSpy->Destination == pEnemyBld || pOtherSpy->ArchiveTarget == pEnemyBld
						|| LookupPassengerIslandGoal(pOtherSpy->UniqueID) == pEnemyBld->GetMapCoords()
						|| (pOtherSpy->CurrentMission == Mission::Guard && pOtherSpy->DistanceFrom(pEnemyBld) <= 2816))
					{
						spyScore -= 500;
						break;
					}
				}

				// Prefer lightly guarded buildings: penalize nearby enemy dogs/detectors and defense turrets!
				int detectorPenalty = 0;
				for (auto const pDetI : InfantryClass::Array)
				{
					if (pDetI && !pDetI->InLimbo && pDetI->IsAlive && pDetI->Owner && !pHouse->IsAlliedWith(pDetI->Owner)
						&& !pDetI->Owner->IsNeutral() && pDetI->Type
						&& (pDetI->Type->Doggie || pDetI->Type->DetectDisguise || pDetI->Type->Sensors)
						&& pEnemyBld->DistanceFrom(pDetI) <= 2048)
					{
						detectorPenalty += 35;
					}
				}

				int turretPenalty = 0;
				for (auto const pDefB : pEnemyOwner->Buildings)
				{
					if (pDefB && !pDefB->InLimbo && pDefB->IsAlive && pDefB->Type && pDefB->Type->IsBaseDefense
						&& pEnemyBld->DistanceFrom(pDefB) <= 1792)
					{
						turretPenalty += 10;
					}
				}

				spyScore -= std::min(140, detectorPenalty + turretPenalty);
				spyScore -= (pInf->DistanceFrom(pEnemyBld) / 1024);

				if (spyScore > bestSpyScore)
				{
					bestSpyScore = spyScore;

					pBestSpyTarget = pEnemyBld;
				}
			}

			if (pBestSpyTarget)
			{
				DispatchFootToTargetOrTransport(pInf, pBestSpyTarget, Mission::Enter, false);

				pExt->AIBrain_InfiltratedEnemyMask |= (1 << std::clamp(pBestSpyTarget->Owner->ArrayIndex, 0, 7));
			}

			continue;
		}

		// Credit Thief (Thief=yes): Coordinate multiple Thieves to infiltrate distinct
		// enemy Refineries & Ore Silos (preferring solvent enemy houses)!
		if (pInf->Type->Thief && !pInf->Type->Infiltrate && isIdle)
		{
			BuildingClass* pBestThiefTarget = nullptr;
			int bestThiefScore = -999999;

			for (auto const pEnemyBld : BuildingClass::Array)
			{
				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;

				if (pHouse->IsAlliedWith(pEnemyBld->Owner) || pEnemyBld->Owner->IsNeutral())
					continue;

				if (pEnemyBld->Type->Refinery || pEnemyBld->Type->Storage > 0)
				{
					int thiefScore = 10000 - (pInf->DistanceFrom(pEnemyBld) / 16);

					if (pEnemyBld->Owner->Available_Money() >= 600)
						thiefScore += 3000;

					for (auto const pOtherThief : InfantryClass::Array)
					{
						if (pOtherThief && pOtherThief != pInf && pOtherThief->Owner == pHouse && !pOtherThief->InLimbo
							&& pOtherThief->IsAlive && pOtherThief->Type && pOtherThief->Type->Thief
							&& (pOtherThief->Destination == pEnemyBld || pOtherThief->ArchiveTarget == pEnemyBld))
						{
							thiefScore -= 8000;
							break;
						}
					}

					if (thiefScore > bestThiefScore)
					{
						bestThiefScore = thiefScore;

						pBestThiefTarget = pEnemyBld;
					}
				}
			}

			if (pBestThiefTarget)
			{
				DispatchFootToTargetOrTransport(pInf, pBestThiefTarget, Mission::Enter, false);

				continue;
			}
		}

		// Hijacker (VehicleThief=yes): Coordinate multiple Hijackers so each steals a
		// distinct expensive visible enemy tank (Apocalypse, Mammoth Mk.II, Avatar, Prism Tank)!
		if (pInf->Type->VehicleThief && isIdle)
		{
			UnitClass* pBestStealTank = nullptr;
			int bestTankScore = 700;

			for (auto const pEnemyU : UnitClass::Array)
			{
				if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
					continue;

				if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
					continue;

				if (pEnemyU->Type->Naval || pEnemyU->Type->Organic || pEnemyU->ParasiteEatingMe != nullptr)
					continue;

				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell())
					&& pInf->DistanceFrom(pEnemyU) <= 5120)
				{
					bool alreadyTargetedByOtherHijacker = false;

					for (auto const pOtherHij : InfantryClass::Array)
					{
						if (pOtherHij && pOtherHij != pInf && pOtherHij->Owner == pHouse && !pOtherHij->InLimbo
							&& pOtherHij->IsAlive && pOtherHij->Type && pOtherHij->Type->VehicleThief
							&& (pOtherHij->Destination == pEnemyU || pOtherHij->ArchiveTarget == pEnemyU))
						{
							alreadyTargetedByOtherHijacker = true;
							break;
						}
					}

					const int stealScore = pEnemyU->Type->Cost - (alreadyTargetedByOtherHijacker ? 5000 : 0);

					if (stealScore > bestTankScore)
					{
						bestTankScore = stealScore;

						pBestStealTank = pEnemyU;
					}
				}
			}

			if (pBestStealTank)
			{
				DispatchFootToTargetOrTransport(pInf, pBestStealTank, Mission::Enter, false);

				continue;
			}
		}

		// Slot 61 (Guard Dogs & Counter-Spy Detectors - Multi-Infiltrator Interception):
		// Distribute Dogs/Detectors across all detected enemy Spies, Engineers, Thieves,
		// Hijackers, or Commandos approaching our base!
		if ((pInf->Type->Doggie || pInf->Type->DetectDisguise || pInf->Type->Sensors) && isIdle)
		{
			if (detectedInfiltratorCount > 0)
			{
				TechnoClass* pBestInfiltrator = nullptr;
				int bestInfDistScore = 999999;

				for (int di = 0; di < detectedInfiltratorCount; ++di)
				{
					auto const pCandInf = detectedEnemyInfiltrators[di];

					if (!pCandInf || pCandInf->InLimbo || !pCandInf->IsAlive)
						continue;

					const int d = pInf->DistanceFrom(pCandInf);

					if (d > 4608)
						continue;

					int dogsAlreadyChasing = 0;

					for (auto const pOtherDog : InfantryClass::Array)
					{
						if (pOtherDog && pOtherDog != pInf && pOtherDog->Owner == pHouse && !pOtherDog->InLimbo
							&& pOtherDog->IsAlive && (pOtherDog->Target == pCandInf || pOtherDog->Destination == pCandInf))
						{
							dogsAlreadyChasing++;
						}
					}

					const int distScore = d + (dogsAlreadyChasing * 1536);

					if (distScore < bestInfDistScore)
					{
						bestInfDistScore = distScore;
						pBestInfiltrator = pCandInf;
					}
				}

				if (pBestInfiltrator)
				{
					DispatchFootToTargetOrTransport(pInf, pBestInfiltrator, Mission::Attack, true);

					continue;
				}
			}

			if (pHiddenOrStealthEnemy && (Unsorted::CurrentFrame % 90 < 25))
			{
				if (auto const pSweepCell = pHiddenOrStealthEnemy->GetCell())
				{
					DispatchFootToTargetOrTransport(pInf, pSweepCell, Mission::AttackMove, false);

					continue;
				}
			}

			// Expert 2: Patrol idle defensive infantry & dogs around fragile key buildings
			// (Superweapons, Power Plants, Radar/SpySat, Factories, ConYard)!
			if ((Unsorted::CurrentFrame + pInf->UniqueID * 13) % 180 < 20)
			{
				BuildingClass* pPatrolBld = nullptr;
				const int patrolSlot = ((Unsorted::CurrentFrame / 180) + pInf->UniqueID) % 7;
				int seenKeyBlds = 0;

				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
						continue;

					if (pOwnB->Type->SuperWeapon != -1 || pOwnB->Type->PowerBonus > pOwnB->Type->PowerDrain
						|| pOwnB->Type->SpySat || pOwnB->Type->Factory != AbstractType::None
						|| pOwnB->Type->ConstructionYard)
					{
						pPatrolBld = pOwnB;
						if (seenKeyBlds == patrolSlot)
							break;

						seenKeyBlds++;
					}
				}

				if (pPatrolBld && pInf->DistanceFrom(pPatrolBld) > 768 && pInf->DistanceFrom(pPatrolBld) <= 6144)
				{
					if (auto const pPatrolCell = pPatrolBld->GetCell())
					{
						OrderFootToAttackMove(pInf, pPatrolCell);

						continue;
					}
				}
			}

			if (pInf->Type->Doggie && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 120 < 20))
			{
				if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
				{
					if (pInf->DistanceFrom(pHomeCell) > 6144)
					{
						DispatchFootToTargetOrTransport(pInf, pHomeCell, Mission::Area_Guard, false);
					}
				}

				continue;
			}
		}

		// C4 Commandos (Tanya, Navy SEAL, Boris, Cyborg Commando): Coordinate multiple
		// Commandos to demolish distinct visible enemy Warships (if amphibious) or buildings!
		if (pInf->Type->C4 && isIdle)
		{
			TechnoClass* pBestC4Target = nullptr;
			int bestC4Dist = 5632;

			const bool canSwim = (pInf->Type->MovementZone == MovementZone::Amphibious
				|| pInf->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pInf->Type->SpeedType == SpeedType::Hover);

			auto IsTargetedByOtherC4 = [pInf, pHouse](const TechnoClass* pCand) -> bool
			{
				for (auto const pOtherC4 : InfantryClass::Array)
				{
					if (pOtherC4 && pOtherC4 != pInf && pOtherC4->Owner == pHouse && !pOtherC4->InLimbo
						&& pOtherC4->IsAlive && pOtherC4->Type && pOtherC4->Type->C4
						&& (pOtherC4->Target == pCand || pOtherC4->Destination == pCand))
					{
						return true;
					}
				}

				return false;
			};

			if (canSwim)
			{
				for (auto const pEnemyShip : UnitClass::Array)
				{
					if (!pEnemyShip || pEnemyShip->InLimbo || !pEnemyShip->IsAlive || !pEnemyShip->Owner
						|| !pEnemyShip->Type || !pEnemyShip->Type->Naval)
					{
						continue;
					}

					if (pEnemyShip->Owner == pHouse || pHouse->IsAlliedWith(pEnemyShip->Owner)
						|| pEnemyShip->Owner->IsNeutral())
					{
						continue;
					}

					if (pEnemyShip->GetCell() && IsCellVisibleToAI(pHouse, pEnemyShip->GetCell()))
					{
						const int d = pInf->DistanceFrom(pEnemyShip) + (IsTargetedByOtherC4(pEnemyShip) ? 2560 : 0);

						if (d < bestC4Dist)
						{
							bestC4Dist = d;

							pBestC4Target = pEnemyShip;
						}
					}
				}
			}

			if (!pBestC4Target && surgicalEnemyBldCount > 0)
			{
				for (int si = 0; si < surgicalEnemyBldCount; ++si)
				{
					auto const pCandBld = surgicalEnemyBlds[si];

					if (!pCandBld || pCandBld->InLimbo || !pCandBld->IsAlive || !pCandBld->Type->CanC4)
						continue;

					const int d = pInf->DistanceFrom(pCandBld) + (IsTargetedByOtherC4(pCandBld) ? 2560 : 0);

					if (d < bestC4Dist)
					{
						bestC4Dist = d;
						pBestC4Target = pCandBld;
					}
				}
			}

			if (!pBestC4Target && (pSurgicalEnemyBld || pPrioritySiegeBld))
				pBestC4Target = pSurgicalEnemyBld ? pSurgicalEnemyBld : pPrioritySiegeBld;

			if (pBestC4Target)
			{
				DispatchFootToTargetOrTransport(pInf, pBestC4Target, Mission::Attack, true);

				continue;
			}
		}

		// IFV (Gunner=yes), OpenTopped Transports/Bunkers (OpenTopped=yes) &
		// Subterranean/Ground APC Strike Team Boarding (Slot 53)! Generalized &
		// cross-faction agnostic: any armed infantry of any faction (native or
		// unlocked from a foreign tech tree) can board any owned OpenTopped or
		// Gunner vehicle, while strictly respecting NoManualEnter & NoManualUnload!
		const int apcBoardBias = static_cast<int>(scores[53]);
		const int apcBoardWindow = std::clamp(25 + (apcBoardBias / 18), 15, 55);
		const int apcBoardRange = std::clamp(2560 + apcBoardBias * 2, 1792, 4096);
		const auto pInfWpn1 = pInf->Type->GetWeapon(1) ? pInf->Type->GetWeapon(1)->WeaponType : nullptr;
		const auto pInfEliteWpn0 = pInf->Type->GetEliteWeapon(0) ? pInf->Type->GetEliteWeapon(0)->WeaponType : nullptr;
		const auto pInfEliteWpn1 = pInf->Type->GetEliteWeapon(1) ? pInf->Type->GetEliteWeapon(1)->WeaponType : nullptr;
		const WeaponTypeClass* pBestInfGun = nullptr;
		for (auto const pCandW : { pInfWpn, pInfWpn1, pInfEliteWpn0, pInfEliteWpn1 })
		{
			if (pCandW && pCandW->Damage > 0 && (!pBestInfGun || pCandW->Range > pBestInfGun->Range))
				pBestInfGun = pCandW;
		}

		// Immediately man any owned under-filled Battle Bunker (IsBaseDefense && CanBeOccupied)
		// with nearby compatible Occupier infantry before considering APC boarding or offensive waves
		if (isIdle && pInf->Type->Occupier && !pInf->Type->Engineer && !pInf->Type->Infiltrate)
		{
			const int infZoneForBunker
				= MapClass::Instance.GetMovementZoneType(pInf->GetMapCoords(), MovementZone::Normal, false);
			BuildingClass* pBestOwnedBunker = nullptr;
			int bestOwnedBunkerDist = 4608;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->IsBaseDefense
					|| !pBld->Type->CanBeOccupied)
				{
					continue;
				}

				const int maxOcc = std::clamp(pBld->Type->MaxNumberOccupants, 1, 10);

				if (!pBld->CanBeOccupiedBy(pInf) || pBld->Occupants.Count >= maxOcc || pBld->GetHealthPercentage() < 0.30)
					continue;

				const int bz
					= MapClass::Instance.GetMovementZoneType(pBld->GetMapCoords(), MovementZone::Normal, false);

				if (infZoneForBunker >= 0 && bz >= 0 && bz != infZoneForBunker)
					continue;

				int pendingInfEntering = 0;

				for (auto const pOtherI : InfantryClass::Array)
				{
					if (pOtherI && pOtherI != pInf && pOtherI->Owner == pHouse && !pOtherI->InLimbo && pOtherI->IsAlive
						&& (pOtherI->CurrentMission == Mission::Enter || pOtherI->QueuedMission == Mission::Enter)
						&& (pOtherI->Destination == pBld || pOtherI->ArchiveTarget == pBld))
					{
						pendingInfEntering++;
					}
				}

				if (pBld->Occupants.Count + pendingInfEntering >= maxOcc)
					continue;

				const int d = pInf->DistanceFrom(pBld);

				if (d <= bestOwnedBunkerDist)
				{
					bestOwnedBunkerDist = d;
					pBestOwnedBunker = pBld;
				}
			}

			if (pBestOwnedBunker)
			{
				OrderFootToEnter(pInf, pBestOwnedBunker);
				continue;
			}
		}

		if (isIdle && !pInf->Type->Infiltrate && (Unsorted::CurrentFrame % 90 < apcBoardWindow))
		{
			bool boardedCombatVehicle = false;
			for (auto const pCombVeh : UnitClass::Array)
			{
				if (!pCombVeh || pCombVeh->Owner != pHouse || pCombVeh->InLimbo || !pCombVeh->IsAlive
					|| !pCombVeh->Type)
				{
					continue;
				}

				if (HasNoManualEnter(pCombVeh->Type) || HasNoManualUnload(pCombVeh->Type))
					continue;

				const int infPassengerSize = std::max(1, static_cast<int>(pInf->Type->TechnoTypeClass::Size));
				if (infPassengerSize > static_cast<int>(pCombVeh->Type->SizeLimit))
					continue;

				int pendingBoardersCount = 0;
				int pendingBoardersSize = 0;
				for (auto const pOtherInf : InfantryClass::Array)
				{
					if (pOtherInf && pOtherInf != pInf && pOtherInf->Owner == pHouse && !pOtherInf->InLimbo
						&& pOtherInf->IsAlive && pOtherInf->CurrentMission == Mission::Enter
						&& pOtherInf->Destination == pCombVeh)
					{
						pendingBoardersCount++;
						pendingBoardersSize += std::max(1, static_cast<int>(pOtherInf->Type->TechnoTypeClass::Size));
					}
				}

				for (auto const pOtherU : UnitClass::Array)
				{
					if (pOtherU && pOtherU != pCombVeh && pOtherU->Owner == pHouse && !pOtherU->InLimbo
						&& pOtherU->IsAlive && pOtherU->CurrentMission == Mission::Enter
						&& pOtherU->Destination == pCombVeh)
					{
						pendingBoardersCount++;
						pendingBoardersSize += std::max(1, static_cast<int>(pOtherU->Type->TechnoTypeClass::Size));
					}
				}

				if (!CanPassengerFitInTransport(pCombVeh, pInf->Type, pendingBoardersCount, pendingBoardersSize))
					continue;

				const bool isUsefulOpenToppedGunner = !pInf->Type->Engineer && !pInf->Type->Doggie && !pInf->Type->Thief
					&& !pInf->Type->VehicleThief && !pInf->Type->Ivan && pBestInfGun != nullptr
					&& pBestInfGun->Damage > 0 && pBestInfGun->Range >= 384;

				const bool isUsefulIFVPassenger = !pInf->Type->Doggie && !pInf->Type->Thief && !pInf->Type->VehicleThief
					&& (pBestInfGun != nullptr || pInf->Type->IFVMode > 0)
					&& (!pInf->Type->Engineer || pParasitedOwnUnit != nullptr);

				const bool isGunnerIFV = (isUsefulIFVPassenger && pCombVeh->Type->Gunner
					&& (pCombVeh->Passengers.NumPassengers + pendingBoardersCount) == 0);
				const bool isOpenToppedFortress = (isUsefulOpenToppedGunner && pCombVeh->Type->OpenTopped
					&& (pCombVeh->Passengers.NumPassengers + pendingBoardersCount) < pCombVeh->Type->Passengers);
				const bool isSubterraneanOrGroundAPC
					= (!pCombVeh->Type->Naval && !pCombVeh->Type->Gunner && !pCombVeh->Type->OpenTopped
						&& pCombVeh->Type->Passengers >= 3 && !IsAmphibiousOrAirTransport(pCombVeh->Type)
						&& (pCombVeh->Passengers.NumPassengers + pendingBoardersCount) < pCombVeh->Type->Passengers
						&& (pCombVeh->Type->MovementZone == MovementZone::Subterrannean || pInf->Type->Engineer
							|| pInf->Type->C4 || apcBoardBias > 80));

				if ((isGunnerIFV || isOpenToppedFortress || isSubterraneanOrGroundAPC)
					&& pInf->DistanceFrom(pCombVeh) <= apcBoardRange)
				{
					DispatchFootToTargetOrTransport(pInf, pCombVeh, Mission::Enter, false);
					boardedCombatVehicle = true;
					break;
				}
			}

			if (boardedCombatVehicle)
				continue;
		}

		// Slot 62 (Soviet Tesla Trooper Overpowering): Charge owned/allied
		// Overpowerable Tesla Coils via Assaulter or IsElectricBolt weapon so they
		// stay powered during LowPower and gain bonus range/damage!
		const auto pSecWpn = pInf->Type->GetWeapon(1) ? pInf->Type->GetWeapon(1)->WeaponType : nullptr;
		const bool hasInfAAWeapon = (pInfWpn && pInfWpn->Projectile && pInfWpn->Projectile->AA)
			|| (pSecWpn && pSecWpn->Projectile && pSecWpn->Projectile->AA);

		// Multi-Flank Mobile AA Infantry Interception (Flak Troopers, Guardian GIs):
		if (baseAirFlankCount > 0 && hasInfAAWeapon && isIdle)
		{
			int bestInfAirIdx = -1;
			int bestInfAirCost = 999999;

			for (int ai = 0; ai < baseAirFlankCount; ++ai)
			{
				auto const pAirT = baseAirFlanks[ai].Target;

				if (!pAirT || pAirT->InLimbo || !pAirT->IsAlive)
					continue;

				const int distAir = pInf->DistanceFrom(pAirT);

				if (distAir > 4608)
					continue;

				const int balancedScore = distAir + (baseAirFlanks[ai].AssignedInterceptors * 1536);

				if (balancedScore < bestInfAirCost)
				{
					bestInfAirCost = balancedScore;
					bestInfAirIdx = ai;
				}
			}

			if (bestInfAirIdx >= 0)
			{
				auto const pBestAir = baseAirFlanks[bestInfAirIdx].Target;
				baseAirFlanks[bestInfAirIdx].AssignedInterceptors++;
				DispatchFootToTargetOrTransport(pInf, pBestAir, Mission::Attack, true);
				continue;
			}
		}

		if (pOverpowerCoil && isIdle
			&& (pInf->Type->Assaulter || (pInfWpn && pInfWpn->IsElectricBolt) || (pSecWpn && pSecWpn->IsElectricBolt))
			&& pInf->DistanceFrom(pOverpowerCoil) <= 3584)
		{
			DispatchFootToTargetOrTransport(pInf, pOverpowerCoil, Mission::Attack, true);

			pOverpowerCoil = nullptr;

			continue;
		}

		// Surgical Urban Garrison Clearing (Assaulter=yes or PenetratesBunker=yes):
		// Evict/kill enemy occupants inside the nearest civilian building without destroying
		// the structure so our Engineer can repair it and our infantry can occupy it!
		if (enemyGarrisonCount > 0 && isIdle
			&& (pInf->Type->Assaulter || (pInfWpn && pInfWpn->Warhead && pInfWpn->Warhead->PenetratesBunker)))
		{
			BuildingClass* pNearestGarr = nullptr;
			int bestInfGarrDist = 3841;

			for (int gi = 0; gi < enemyGarrisonCount; ++gi)
			{
				auto const pCandG = enemyGarrisonsToClear[gi];

				if (!pCandG || pCandG->InLimbo || !pCandG->IsAlive || pCandG->Occupants.Count <= 0)
					continue;

				const int d = pInf->DistanceFrom(pCandG);

				if (d <= 3840 && d < bestInfGarrDist)
				{
					bestInfGarrDist = d;
					pNearestGarr = pCandG;
				}
			}

			if (pNearestGarr)
			{
				if (pInf->Type->Assaulter)
				{
					DispatchFootToTargetOrTransport(pInf, pNearestGarr, Mission::Enter, false);
					continue;
				}
				else
				{
					DispatchFootToTargetOrTransport(pInf, pNearestGarr, Mission::Attack, true);
					continue;
				}
			}
		}

		// Yuri Bio-Reactor Power Surge: Enter Bio-Reactor with basic infantry
		// during LowPower!
		if (pBioReactor && bioReactorNeedsPower && isIdle && pInf->Destination != pBioReactor && !pInf->Type->Engineer
			&& !pInf->Type->Infiltrate && !pInf->Type->Doggie && pInf->Type->Cost <= 250
			&& pInf->DistanceFrom(pBioReactor) <= 4096)
		{
			DispatchFootToTargetOrTransport(pInf, pBioReactor, Mission::Enter, false);
			infAlreadyHeadingToBioReactor++;
			if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor >= pBioReactor->Type->Passengers)
				pBioReactor = nullptr;

			continue;
		}

		if (false && pBioReactor && isIdle && pInf->Type->Cost <= 250 && pInf->DistanceFrom(pBioReactor) <= 3072)
		{
			pInf->SetDestination(pBioReactor, true);

			pInf->QueueMission(Mission::Enter, true);

			pBioReactor = nullptr;

			continue;
		}

		// Urban Garrisoning (Slot 23): Occupy nearby empty Civilian Buildings /
		// Battle Bunkers with Occupier infantry up to their real
		// MaxNumberOccupants!
		const int garrisonBias = static_cast<int>(scores[23]);
		const int garrisonWindow = std::clamp(15 + (garrisonBias / 20), 6, 40);
		const int garrisonRange = std::clamp(2560 + garrisonBias * 2, 1536, 4096);

		if (pInf->Type->Occupier && isIdle && (Unsorted::CurrentFrame % 90 < garrisonWindow))
		{
			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->CanBeOccupied)
					continue;

				const int maxOcc = std::clamp(pBld->Type->MaxNumberOccupants, 1, 10);

				if (pBld->CanBeOccupiedBy(pInf) && pBld->Occupants.Count < maxOcc
					&& pBld->GetHealthPercentage() >= 0.30 && pInf->DistanceFrom(pBld) <= garrisonRange)
				{
					DispatchFootToTargetOrTransport(pInf, pBld, Mission::Enter, false);

					break;
				}
			}
		}
	}

	// Tactical Bridge Demolition (Ambush enemy units crossing a bridge when
	// Immune Bridges is OFF)
	if (scores[22] > 600 && RulesClass::Instance && RulesClass::Instance->BridgeStrength < 1000000)
	{
		CellClass* pTargetBridgeCell = nullptr;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner)
				continue;

			if (pTechno->Owner == pHouse || pHouse->IsAlliedWith(pTechno->Owner) || pTechno->Owner->IsNeutral())
				continue;

			if (pTechno->OnBridge)
			{
				auto const pCell = pTechno->GetCell();

				if (pCell && pCell->ContainsBridge() && IsCellVisibleToAI(pHouse, pCell))
				{
					pTargetBridgeCell = pCell;

					break;
				}
			}
		}

		if (pTargetBridgeCell)
		{
			int assignedDemolishers = 0;

			for (auto const pUnit : UnitClass::Array)
			{
				if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
					continue;

				if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer || pUnit->OnBridge)
					continue;

				if (IsFootActivelyAttackingOrMovingToEnemy(pUnit))
					continue;

				if (pUnit->DistanceFrom(pTargetBridgeCell) <= 3072) // Within 12 cells of the bridge
				{
					pUnit->SetTarget(pTargetBridgeCell);

					pUnit->QueueMission(Mission::Attack, true);

					if (++assignedDemolishers >= 4)
						break;
				}
			}
		}
	}

	// Autonomous Fog-of-War Scouting & Coordinated Multi-Wave Offensive Dispatch
	// (Replaces legacy aimd.ini TeamScripts)
	BuildingClass* pVisibleEnemyBld = nullptr;
	int bestVisEnemyBldScore = -999999;
	CellClass* pFallbackEnemyCell = nullptr;
	int bestFallbackEnemyScore = -999999;

	// First pass: find the closest enemy structure distance on our landmass
	// (outer perimeter reference) and count enemy base defenses so we peel the
	// enemy base from the outside in instead of driving suicidally through half
	// the enemy base to reach a deep interior ConYard/Superweapon!
	int minSameZoneEnemyBldDist = 999999;
	int minAnyEnemyBldDist = 999999;
	int totalEnemyBaseDefenses = 0;
	for (auto const pEnemyBld : BuildingClass::Array)
	{
		if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type
			|| pEnemyBld->Type->Immune || pEnemyBld->Type->Insignificant || pEnemyBld->Type->InvisibleInGame)
		{
			continue;
		}

		const CellStruct ebc = pEnemyBld->GetMapCoords();
		if (ebc.X <= 1 && ebc.Y <= 1)
			continue;

		auto const pOwner = pEnemyBld->Owner;
		if (pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver()
			|| pOwner->Defeated)
		{
			continue;
		}

		if (pEnemyBld->Type->IsBaseDefense)
			totalEnemyBaseDefenses++;

		const int distHome
			= (homeCoords != CellStruct::Empty) ? (std::abs(ebc.X - homeCoords.X) + std::abs(ebc.Y - homeCoords.Y)) : 0;
		if (distHome < minAnyEnemyBldDist)
			minAnyEnemyBldDist = distHome;

		const int ebz = MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, false);
		if (homeGroundZone >= 0 && ebz == homeGroundZone && distHome < minSameZoneEnemyBldDist)
			minSameZoneEnemyBldDist = distHome;
	}

	for (auto const pEnemyBld : BuildingClass::Array)
	{
		if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type
			|| pEnemyBld->Type->Immune || pEnemyBld->Type->Insignificant || pEnemyBld->Type->InvisibleInGame)
		{
			continue;
		}

		const CellStruct ebc = pEnemyBld->GetMapCoords();
		if (ebc.X <= 1 && ebc.Y <= 1)
			continue;

		auto const pOwner = pEnemyBld->Owner;
		if (pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver()
			|| pOwner->Defeated)
		{
			continue;
		}

		auto const pCell = pEnemyBld->GetCell();
		if (!pCell)
			continue;

		const int distHome
			= (homeCoords != CellStruct::Empty) ? (std::abs(ebc.X - homeCoords.X) + std::abs(ebc.Y - homeCoords.Y)) : 0;
		const int ebz = MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, false);
		const int refMinDist = (homeGroundZone >= 0 && ebz == homeGroundZone && minSameZoneEnemyBldDist < 999999)
			? minSameZoneEnemyBldDist
			: (minAnyEnemyBldDist < 999999 ? minAnyEnemyBldDist : distHome);
		const int depthIntoEnemyBase = std::max(0, distHome - refMinDist);

		// Perimeter-First Scoring: heavily penalize targets that sit deep behind
		// the enemy's outer perimeter (> 6 cells deeper than the closest enemy
		// structure) so our battalions chew through the closest enemy
		// structures/defenses on the outer ring first instead of driving through
		// half the enemy base!
		int bldCandScore = 1000 - (distHome * 8) - (depthIntoEnemyBase > 6 ? (depthIntoEnemyBase - 6) * 18 : 0);
		if (depthIntoEnemyBase <= 6)
			bldCandScore += 220; // Outer-perimeter target facing our approach!
		if (pOwner->ArrayIndex == pHouse->EnemyHouseIndex)
			bldCandScore += 220;

		if (homeGroundZone >= 0 && ebz == homeGroundZone)
			bldCandScore += 350;

		if (!pEnemyBld->Type->Naval && pCell->LandType != LandType::Water)
			bldCandScore += 180;

		if (pEnemyBld->Type->IsBaseDefense && depthIntoEnemyBase <= 8)
		{
			bldCandScore += 135; // Clear outer base defenses blocking our approach!
		}
		else if (pEnemyBld->Type->ConstructionYard || pEnemyBld->Type->Factory != AbstractType::None
			|| pEnemyBld->Type->SuperWeapon != -1)
		{
			bldCandScore += 110;
		}
		else if (pEnemyBld->Type->Refinery || pEnemyBld->Type->PowerBonus > 0)
		{
			bldCandScore += 90;
		}

		if (bldCandScore > bestFallbackEnemyScore)
		{
			bestFallbackEnemyScore = bldCandScore;
			pFallbackEnemyCell = pCell;
		}

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);
		const bool visible = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell)
			|| ((pExt->AIBrain_InfiltratedEnemyMask & (1 << ownerIdx)) != 0)
			|| (pOwner->WhoLastHurtMe == pHouse->ArrayIndex);

		if (visible && bldCandScore > bestVisEnemyBldScore)
		{
			bestVisEnemyBldScore = bldCandScore;
			pVisibleEnemyBld = pEnemyBld;
		}
	}

	const bool allEnemyBldsGone = (pFallbackEnemyCell == nullptr);

	if (!pVisibleEnemyBld && !pFallbackEnemyCell)
	{
		if (pHiddenOrStealthEnemy && pHiddenOrStealthEnemy->GetCell())
		{
			pFallbackEnemyCell = pHiddenOrStealthEnemy->GetCell();
		}
		else if (pEnemySubOrShip && pEnemySubOrShip->GetCell())
		{
			pFallbackEnemyCell = pEnemySubOrShip->GetCell();
		}
		else
		{
			for (auto const pRemT : TechnoClass::Array)
			{
				if (!pRemT || pRemT->InLimbo || !pRemT->IsAlive || !pRemT->Owner
					|| pRemT->WhatAmI() == AbstractType::Building)
				{
					continue;
				}

				auto const pRO = pRemT->Owner;

				if (pRO == pHouse || pHouse->IsAlliedWith(pRO) || pRO->IsNeutral() || pRO->IsObserver()
					|| pRO->Defeated)
				{
					continue;
				}

				if (auto const pRC = pRemT->GetCell())
				{
					pFallbackEnemyCell = pRC;

					if (pRO->ArrayIndex == pHouse->EnemyHouseIndex)
						break;
				}
			}
		}
	}

	const bool endgameRemnantHunt
		= (!pVisibleEnemyBld && pFallbackEnemyCell != nullptr && (allEnemyBldsGone || combatStalledForHunt));

	// Count idle combat units and infantry ready at base
	std::vector<UnitClass*> idleCombatUnits;

	std::vector<InfantryClass*> idleCombatInf;

	for (auto const pUnit : UnitClass::Array)
	{
		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
			continue;

		if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer
			|| (pUnit->Type->DeploysInto && pUnit->Type->DeploysInto->ConstructionYard))
		{
			continue;
		}

		if (pUnit->GetHealthPercentage() < 0.40)
			continue;

		if (!endgameRemnantHunt && (pUnit->Type->CanDisguise || pUnit->Type->PermaDisguise) && scores[83] > 60
			&& homeCoords != CellStruct::Empty && pUnit->GetMapCoords().DistanceFrom(homeCoords) > 11.0)
		{
			continue;
		}

		const bool canCrossWaterDirectly = (pUnit->Type->MovementZone == MovementZone::Amphibious
			|| pUnit->Type->MovementZone == MovementZone::AmphibiousDestroyer
			|| pUnit->Type->MovementZone == MovementZone::AmphibiousCrusher
			|| pUnit->Type->MovementZone == MovementZone::Fly
			|| pUnit->Type->MovementZone == MovementZone::Subterrannean || pUnit->Type->JumpJet
			|| pUnit->Type->Teleporter);
		ClearStaleEnterOrUnreachableCrossIslandMission(pUnit, canCrossWaterDirectly);

		if (IsImmobileUnitTypeOnLand(pUnit->Type))
		{
			continue; // Static deployed vehicle bunker (NoManualMove=yes / Speed=0):
					// stay in Area_Guard and let OpenTopped passengers fire!
		}

		if ((pUnit->Type->Gunner || pUnit->Type->OpenTopped) && !HasNoManualEnter(pUnit->Type)
			&& !HasNoManualUnload(pUnit->Type) && pUnit->Passengers.NumPassengers < pUnit->Type->Passengers
			&& !pUnit->HasBeenAttacked)
		{
			bool passengerWalkingToBoard = false;
			for (auto const pCheckI : InfantryClass::Array)
			{
				if (pCheckI && pCheckI->Owner == pHouse && !pCheckI->InLimbo && pCheckI->IsAlive
					&& pCheckI->CurrentMission == Mission::Enter && pCheckI->Destination == pUnit
					&& pCheckI->DistanceFrom(pUnit) <= 3840)
				{
					passengerWalkingToBoard = true;
					break;
				}
			}

			if (passengerWalkingToBoard)
			{
				continue; // Wait a few seconds for the nearby gunner/passenger to climb
						// inside before driving away!
			}
		}

		if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter
			|| pUnit->BunkerLinkedItem != nullptr || pUnit->HasAnyLink())
		{
			continue;
		}

		if (IsFootActivelyAttackingOrMovingToEnemy(pUnit))
			continue;

		if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
			|| pUnit->CurrentMission == Mission::AttackMove
			|| (pUnit->CurrentMission == Mission::Move && pUnit->MegaMission == Mission::AttackMove))
		{
			idleCombatUnits.push_back(pUnit);
		}
	}

	for (auto const pInf : InfantryClass::Array)
	{
		if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type)
			continue;

		if (pInf->Type->Engineer || pInf->Type->Infiltrate)
			continue;

		const bool canInfCrossWater = (pInf->Type->MovementZone == MovementZone::Amphibious
			|| pInf->Type->MovementZone == MovementZone::AmphibiousDestroyer
			|| pInf->Type->MovementZone == MovementZone::Fly || pInf->Type->JumpJet || pInf->Type->Teleporter);
		ClearStaleEnterOrUnreachableCrossIslandMission(pInf, canInfCrossWater);

		if (pInf->CurrentMission == Mission::Enter || pInf->QueuedMission == Mission::Enter || pInf->HasAnyLink())
		{
			continue;
		}

		if (IsFootActivelyAttackingOrMovingToEnemy(pInf))
			continue;

		if (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard
			|| pInf->CurrentMission == Mission::AttackMove
			|| (pInf->CurrentMission == Mission::Move && pInf->MegaMission == Mission::AttackMove))
		{
			idleCombatInf.push_back(pInf);
		}
	}

	// Determine the primary island zone where our idle ground army is
	// concentrated (could be homeGroundZone or an expansion island!)
	int primaryArmyZone = homeGroundZone;
	{
		struct ZoneArmyCount
		{
			int zone;
			int weight;
		};
		std::vector<ZoneArmyCount> zoneCounts;
		auto addZoneWeight = [&zoneCounts](int z, int w)
		{
			if (z < 0)
				return;

			for (auto& zc : zoneCounts)
			{
				if (zc.zone == z)
				{
					zc.weight += w;
					return;
				}
			}

			zoneCounts.push_back({ z, w });
		};
		for (auto const pU : idleCombatUnits)
		{
			if (!IsAmphibiousOrAirTransport(pU->Type))
			{
				addZoneWeight(
					MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false), 3);
			}
		}

		for (auto const pI : idleCombatInf)
		{
			addZoneWeight(MapClass::Instance.GetMovementZoneType(pI->GetMapCoords(), MovementZone::Normal, false), 1);
		}

		int bestW = 0;
		for (const auto& zc : zoneCounts)
		{
			if (zc.weight > bestW)
			{
				bestW = zc.weight;
				primaryArmyZone = zc.zone;
			}
		}
	}

	const int arch = pExt->AIBrain_Archetype;
	const int rushWaveBias = NeuralAI::PolicyFor(pHouse).OutputBias[37];
	const int reserveBias = -rushWaveBias;
	// Critical-Mass Team Scaling (Lanchester's Law):
	// In RA2/YR, larger battalions are exponentially more devastating against
	// fortified AI/human bases. Scale minimum battalion size up when the enemy
	// has base defenses or when our industrial capacity is mature!
	const int baseWave = (arch == 3 || arch == 5) ? 4 : ((arch == 2 || arch == 4 || arch == 7) ? 8 : 6);
	const int defenseMassBonus = std::clamp(totalEnemyBaseDefenses / 2, 0, 4) + ((pHouse->OwnedUnits >= 18) ? 2 : 0);
	const bool highMoneyMultiFactoryStream = (pHouse->Available_Money() >= 4000
		&& (pHouse->NumWarFactories >= 2 || pHouse->NumBarracks >= 2 || pHouse->NumAirpads >= 2));
	const int streamWaveDiscount = highMoneyMultiFactoryStream ? 2 : 0;
	const size_t minWaveUnits = endgameRemnantHunt
		? 1u
		: static_cast<size_t>(std::clamp(
			baseWave + defenseMassBonus - streamWaveDiscount - (rushWaveBias / 160),
			highMoneyMultiFactoryStream ? 2 : 3, 14));
	const int blindPushFrame = (rushWaveBias > 100 || highMoneyMultiFactoryStream) ? 1350 : 3200;

	// Multi-Purpose Map Unit Recall ("utilizarlas para diferentes propósitos por
	// el mapa y a la hora de la verdad reúne las que necesite en un lugar y
	// mandarlas contra objetivos enemigos"): While accumulating forces, units can
	// ambush in tree groves, patrol outposts, hunt crates, or guard harvesters.
	// When recalling those non-engaged map units brings our strike force to
	// critical mass (>= minWaveUnits), recruit them into idleCombatUnits so the
	// entire army rallies for the decisive offensive push!
	if (!endgameRemnantHunt && idleCombatUnits.size() < minWaveUnits)
	{
		std::vector<UnitClass*> recallableMapUnits;
		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
				continue;

			if (pUnit->Type->Naval || pUnit->Type->Harvester || pUnit->Type->ResourceGatherer
				|| IsAmphibiousOrAirTransport(pUnit->Type))
			{
				continue;
			}

			if ((pUnit->Type->DeploysInto && pUnit->Type->DeploysInto->ConstructionYard)
				|| IsImmobileUnitTypeOnLand(pUnit->Type))
			{
				continue;
			}

			if (pUnit->GetHealthPercentage() < 0.65 || pUnit->HasBeenAttacked || pUnit->Target != nullptr)
				continue;

			if (pUnit->CurrentMission == Mission::Enter || pUnit->QueuedMission == Mission::Enter
				|| (pUnit->CaptureManager && pUnit->CaptureManager->NumControlNodes() > 0))
			{
				continue;
			}

			const int recZone
				= MapClass::Instance.GetMovementZoneType(pUnit->GetMapCoords(), MovementZone::Normal, false);
			if (IsFootActivelyAttackingOrMovingToEnemy(pUnit) || IsFootNearEnemyFrontline(pUnit, recZone))
			{
				continue; // Never abort a unit that is already attacking an enemy
						// objective or at an active frontline!
			}

			const auto pW0 = pUnit->Type->GetWeapon(0) ? pUnit->Type->GetWeapon(0)->WeaponType : nullptr;
			if (!pW0 || pW0->Damage <= 0)
				continue;

			if (std::find(idleCombatUnits.begin(), idleCombatUnits.end(), pUnit) != idleCombatUnits.end())
				continue;

			if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard
				|| pUnit->CurrentMission == Mission::Move)
			{
				recallableMapUnits.push_back(pUnit);
			}
		}

		if (idleCombatUnits.size() + recallableMapUnits.size() >= minWaveUnits)
		{
			for (auto const pRecU : recallableMapUnits)
			{
				idleCombatUnits.push_back(pRecU);
			}
		}
	}

	// Helper: returns true ONLY if a sister transport is actively
	// beaching/unloading right next to pFoot (within 10 cells) and is alive and
	// NOT intercepted/attacked. If a transport was destroyed or intercepted at
	// sea, immediately ignore it!
	auto isWaitingForSisterTransportUnload = [pHouse](FootClass* pFoot, bool canCrossWaterDirectly) -> bool
	{
		if (!pFoot || canCrossWaterDirectly || pFoot->HasBeenAttacked || (Unsorted::CurrentFrame % 180 >= 110))
			return false;

		const CellStruct fc = pFoot->GetMapCoords();
		for (auto const pCheckT : UnitClass::Array)
		{
			if (!pCheckT || pCheckT->Owner != pHouse || pCheckT->InLimbo || !pCheckT->IsAlive || !pCheckT->Type
				|| !IsAmphibiousOrAirTransport(pCheckT->Type))
			{
				continue;
			}

			if (pCheckT->Passengers.NumPassengers == 0)
				continue;

			// If the sister transport was intercepted/attacked or is critically
			// damaged, ignore it so landed units march immediately!
			if (pCheckT->HasBeenAttacked || pCheckT->GetHealthPercentage() < 0.65)
				continue;

			if (fc.DistanceFrom(pCheckT->GetMapCoords()) <= 10.0)
				return true;
		}

		return false;
	};

	CellClass* pWaveTargetCell = pVisibleEnemyBld
		? pVisibleEnemyBld->GetCell()
		: ((endgameRemnantHunt || Unsorted::CurrentFrame > blindPushFrame) ? pFallbackEnemyCell : nullptr);

	// Expert 9 & Arch 7 (Allied Coalition Rescue & Joint Attack Wave):
	// If an allied house's critical building (ConYard, Factory, Superweapon) is
	// under enemy attack, coordinate our wave to defend our ally!
	if (pExt->AIBrain_ActiveExpert == 9 || pExt->AIBrain_Archetype == 7 || scores[85] > 120)
	{
		for (auto const pAllyB : BuildingClass::Array)
		{
			if (!pAllyB || pAllyB->InLimbo || !pAllyB->IsAlive || !pAllyB->Owner || pAllyB->Owner == pHouse
				|| !pHouse->IsAlliedWith(pAllyB->Owner) || pAllyB->Owner->IsNeutral() || !pAllyB->Type)
			{
				continue;
			}

			if ((pAllyB->Type->ConstructionYard || pAllyB->Type->Factory != AbstractType::None
					|| pAllyB->Type->SuperWeapon != -1)
				&& pAllyB->HasBeenAttacked && pAllyB->GetHealthPercentage() < 0.85)
			{
				if (auto const pRescueCell = pAllyB->GetCell())
				{
					pWaveTargetCell = pRescueCell;

					break;
				}
			}
		}
	}

	CellClass* pIslandGoalCell = pWaveTargetCell ? pWaveTargetCell : pFallbackEnemyCell;
	int targetIslandZone = -1;
	if (pIslandGoalCell)
	{
		targetIslandZone
			= MapClass::Instance.GetMovementZoneType(pIslandGoalCell->MapCoords, MovementZone::Normal, false);
	}

	CellClass* pSecondaryWaveTargetCell = nullptr;
	CellClass* pSecondaryIslandGoalCell = nullptr;
	int secondaryIslandZone = -1;

	// Smart Enemy Island Reconquest & Multi-Squadron Parallel Target Selection:
	// Pick BOTH a primary enemy island objective (pIslandGoalCell) AND a
	// secondary enemy island/flank objective (pSecondaryIslandGoalCell /
	// pSecondaryWaveTargetCell) so ground armies, transports, and naval squadrons
	// operate in parallel teams across multiple fronts!
	{
		CellClass* pBestEnemyIslandCell = nullptr;
		int bestEnemyIslandZone = -1;
		int bestEnemyIslandScore = -999999;
		CellClass* pSecondEnemyIslandCell = nullptr;
		int secondEnemyIslandZone = -1;
		int secondEnemyIslandScore = -999999;

		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
				|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame || pEB->Type->Naval)
			{
				continue;
			}

			auto const pEO = pEB->Owner;
			if (pEO == pHouse || pHouse->IsAlliedWith(pEO) || pEO->IsNeutral() || pEO->IsObserver() || pEO->Defeated)
				continue;

			auto const pECell = pEB->GetCell();
			if (!pECell || pECell->LandType == LandType::Water)
				continue;

			const int ebz = MapClass::Instance.GetMovementZoneType(pECell->MapCoords, MovementZone::Normal, false);
			if (ebz < 0)
				continue;

			if (!pSecondaryWaveTargetCell && pWaveTargetCell
				&& pECell->MapCoords.DistanceFrom(pWaveTargetCell->MapCoords) >= 10.0)
			{
				pSecondaryWaveTargetCell = pECell;
			}

			if (primaryArmyZone >= 0 && ebz == primaryArmyZone)
				continue;

			int enemyTurretsInZone = 0;
			int enemyBldsInZone = 0;
			for (auto const pCheckEB : BuildingClass::Array)
			{
				if (!pCheckEB || pCheckEB->InLimbo || !pCheckEB->IsAlive || !pCheckEB->Owner || !pCheckEB->Type
					|| pCheckEB->Type->Insignificant)
				{
					continue;
				}

				if (pCheckEB->Owner == pHouse || pHouse->IsAlliedWith(pCheckEB->Owner) || pCheckEB->Owner->IsNeutral())
					continue;

				if (MapClass::Instance.GetMovementZoneType(pCheckEB->GetMapCoords(), MovementZone::Normal, false)
					== ebz)
				{
					enemyBldsInZone++;
					if (pCheckEB->Type->IsBaseDefense)
						enemyTurretsInZone++;
				}
			}

			int ourUnitsInZone = 0;
			for (auto const pU : idleCombatUnits)
			{
				if (MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false) == ebz)
					ourUnitsInZone++;
			}

			int score = 2000 - (enemyTurretsInZone * 180) - (enemyBldsInZone * 25);
			if (ourUnitsInZone >= 1 && ourUnitsInZone <= 12)
				score += 350; // Reinforce active landing beachhead!
			if (homeGroundZone >= 0 && ebz != homeGroundZone)
				score += 200;

			if (pEO->ArrayIndex == pHouse->EnemyHouseIndex)
				score += 150;

			if (score > bestEnemyIslandScore)
			{
				if (pBestEnemyIslandCell
					&& (bestEnemyIslandZone != ebz
						|| pECell->MapCoords.DistanceFrom(pBestEnemyIslandCell->MapCoords) >= 14.0))
				{
					secondEnemyIslandScore = bestEnemyIslandScore;
					pSecondEnemyIslandCell = pBestEnemyIslandCell;
					secondEnemyIslandZone = bestEnemyIslandZone;
				}

				bestEnemyIslandScore = score;
				pBestEnemyIslandCell = pECell;
				bestEnemyIslandZone = ebz;
			}
			else if (score > secondEnemyIslandScore && pBestEnemyIslandCell
				&& (bestEnemyIslandZone != ebz
					|| pECell->MapCoords.DistanceFrom(pBestEnemyIslandCell->MapCoords) >= 14.0))
			{
				secondEnemyIslandScore = score;
				pSecondEnemyIslandCell = pECell;
				secondEnemyIslandZone = ebz;
			}
		}

		if (pBestEnemyIslandCell && bestEnemyIslandZone >= 0)
		{
			pIslandGoalCell = pBestEnemyIslandCell;
			targetIslandZone = bestEnemyIslandZone;
		}

		if (pSecondEnemyIslandCell && secondEnemyIslandZone >= 0)
		{
			pSecondaryIslandGoalCell = pSecondEnemyIslandCell;
			secondaryIslandZone = secondEnemyIslandZone;
		}
	}

	bool hasIdleGroundArmyOnOtherIsland = false;
	if (targetIslandZone >= 0)
	{
		for (auto const pU : idleCombatUnits)
		{
			if (IsAmphibiousOrAirTransport(pU->Type))
				continue;

			const int uz = MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false);
			if (uz >= 0 && uz != targetIslandZone)
			{
				hasIdleGroundArmyOnOtherIsland = true;
				break;
			}
		}

		if (!hasIdleGroundArmyOnOtherIsland)
		{
			for (auto const pI : idleCombatInf)
			{
				const int iz = MapClass::Instance.GetMovementZoneType(pI->GetMapCoords(), MovementZone::Normal, false);
				if (iz >= 0 && iz != targetIslandZone)
				{
					hasIdleGroundArmyOnOtherIsland = true;
					break;
				}
			}
		}
	}

	const bool enemyOnDisconnectedIsland = (targetIslandZone >= 0
		&& ((homeGroundZone >= 0 && targetIslandZone != homeGroundZone)
			|| (primaryArmyZone >= 0 && targetIslandZone != primaryArmyZone) || hasIdleGroundArmyOnOtherIsland));

	// If no enemy building is visible yet, send a fast scout unit/infantry only
	// if on the same landmass
	const int scoutPeriod = std::clamp(225 - (static_cast<int>(scores[37]) / 4), 105, 360);
	if (!pVisibleEnemyBld && pFallbackEnemyCell && !enemyOnDisconnectedIsland
		&& (Unsorted::CurrentFrame % scoutPeriod < 30))
	{
		const int fbZone
			= MapClass::Instance.GetMovementZoneType(pFallbackEnemyCell->MapCoords, MovementZone::Normal, false);
		if (!idleCombatUnits.empty())
		{
			auto const pScout = idleCombatUnits.front();
			const int sz = MapClass::Instance.GetMovementZoneType(pScout->GetMapCoords(), MovementZone::Normal, false);
			if (fbZone >= 0 && sz == fbZone && pFallbackEnemyCell->LandType != LandType::Water)
			{
				OrderFootToAttackMove(pScout, pFallbackEnemyCell);
			}
		}
		else if (!idleCombatInf.empty())
		{
			auto const pScoutInf = idleCombatInf.front();
			const int sz
				= MapClass::Instance.GetMovementZoneType(pScoutInf->GetMapCoords(), MovementZone::Normal, false);
			if (fbZone >= 0 && sz == fbZone && pFallbackEnemyCell->LandType != LandType::Water)
			{
				OrderFootToAttackMove(pScoutInf, pFallbackEnemyCell);
			}
		}
	}

	// Coordinated Multi-Squadron Offensive Strike Wave (Slot 37: Rush vs Mass
	// Timing) Splits forces into parallel Squadron Alpha (pWaveTargetCell) and
	// Squadron Bravo (pSecondaryWaveTargetCell)
	if (pWaveTargetCell
		&& (endgameRemnantHunt || idleCombatUnits.size() >= minWaveUnits
			|| (idleCombatUnits.size() + idleCombatInf.size() >= minWaveUnits + 4)))
	{
		const int waveTargetZone
			= MapClass::Instance.GetMovementZoneType(pWaveTargetCell->MapCoords, MovementZone::Normal, false);
		const bool waveTargetInWater = (pWaveTargetCell->LandType == LandType::Water);
		const int secWaveZone = pSecondaryWaveTargetCell
			? MapClass::Instance.GetMovementZoneType(pSecondaryWaveTargetCell->MapCoords, MovementZone::Normal, false)
			: -1;
		const size_t desiredUnitReserve
			= endgameRemnantHunt ? 0u : static_cast<size_t>(std::clamp(1 + (reserveBias / 180), 0, 2));
		const size_t reserveCount = idleCombatUnits.size() > (desiredUnitReserve + 1) ? desiredUnitReserve : 0;

		// Forward Staging / Rally Assembly ("reúne las que necesite en un lugar y
		// mandarlas contra objetivos enemigos"): If our battalion units on the
		// target landmass are scattered across the map (e.g., > 35% of them are >
		// 15 cells away from the forward staging point outside the enemy
		// perimeter), rally them at a safe Staging Cell (~15 cells from
		// pWaveTargetCell toward our army centroid) first so they hit the enemy
		// perimeter together as one devastating wall of armor instead of trickling!
		CellClass* pForwardStagingCell = nullptr;
		bool holdAtForwardStaging = false;
		if (!endgameRemnantHunt && waveTargetZone >= 0 && !waveTargetInWater)
		{
			int sameZoneCount = 0;
			int sumX = 0;
			int sumY = 0;
			for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
			{
				auto const pCheckU = idleCombatUnits[i];
				if (!pCheckU || pCheckU->Type->Naval || IsAmphibiousOrAirTransport(pCheckU->Type))
					continue;

				const CellStruct cuc = pCheckU->GetMapCoords();
				if (MapClass::Instance.GetMovementZoneType(cuc, MovementZone::Normal, false) == waveTargetZone)
				{
					if (IsFootActivelyAttackingOrMovingToEnemy(pCheckU)
						|| IsFootNearEnemyFrontline(pCheckU, waveTargetZone))
					{
						continue; // Do not include units already engaged at a frontline in
								// staging centroid/count!
					}

					sameZoneCount++;
					sumX += cuc.X;
					sumY += cuc.Y;
				}
			}

			if (sameZoneCount >= 4)
			{
				const CellStruct armyCenter { static_cast<short>(sumX / sameZoneCount),
					static_cast<short>(sumY / sameZoneCount) };
				const CellStruct tc = pWaveTargetCell->MapCoords;
				const int dx = armyCenter.X - tc.X;
				const int dy = armyCenter.Y - tc.Y;
				const int distToTarget = std::max(1, static_cast<int>(tc.DistanceFrom(armyCenter)));
				if (distToTarget >= 21)
				{
					// Prefer staging 20 to 30 cells outside the enemy perimeter (outside
					// long-range defenses), adapting dynamically if map/island geometry
					// is tighter.
					const int candidateStageDists[] = { 25, 23, 27, 21, 29, 19, 16 };
					for (int pass = 0; pass < 2 && !pForwardStagingCell; ++pass)
					{
						for (int desiredDist : candidateStageDists)
						{
							if (pForwardStagingCell)
								break;

							if (pass == 0 && desiredDist < 20)
								continue;

							const int stageDist = std::min(desiredDist, distToTarget - 5);
							if (stageDist < (pass == 0 ? 20 : 15))
								continue;

							const CellStruct rawStage { static_cast<short>(tc.X + (dx * stageDist) / distToTarget),
								static_cast<short>(tc.Y + (dy * stageDist) / distToTarget) };
							for (int r = 0; r <= 6 && !pForwardStagingCell; r += 2)
							{
								for (int sy = -r; sy <= r && !pForwardStagingCell; sy += 2)
								{
									for (int sx = -r; sx <= r; sx += 2)
									{
										const CellStruct sc { static_cast<short>(rawStage.X + sx),
											static_cast<short>(rawStage.Y + sy) };
										if (auto const pSC = MapClass::Instance.TryGetCellAt(sc))
										{
											if (pSC->LandType != LandType::Water
												&& MapClass::Instance.GetMovementZoneType(
													sc, MovementZone::Normal, false)
													== waveTargetZone
												&& pSC->IsClearToMove(
													SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
											{
												bool safeFromLongRangeDef = true;
												const double minSafeDefDist = (pass == 0) ? 20.0 : 15.0;
												if (sc.DistanceFrom(tc) < minSafeDefDist)
													safeFromLongRangeDef = false;

												for (int ebIdx = 0;
													safeFromLongRangeDef && ebIdx < BuildingClass::Array.Count; ++ebIdx)
												{
													auto const pEB = BuildingClass::Array.GetItem(ebIdx);
													if (!pEB || !pEB->IsAlive || pEB->InLimbo || !pEB->Owner
														|| pHouse->IsAlliedWith(pEB->Owner))
													{
														continue;
													}

													if (!pEB->Type
														|| (!pEB->Type->IsBaseDefense
															&& !pEB->GetWeapon(0)->WeaponType))
													{
														continue;
													}

													const CellStruct ebc = pEB->GetMapCoords();
													if (MapClass::Instance.GetMovementZoneType(
															ebc, MovementZone::Normal, false)
														!= waveTargetZone)
													{
														continue;
													}

													double reqClearance = minSafeDefDist;
													if (pass == 0 && pEB->Type->IsBaseDefense)
													{
														if (auto const pW = pEB->GetWeapon(0)->WeaponType)
														{
															reqClearance = std::max(
																reqClearance, static_cast<double>(pW->Range / 256 + 3));
														}
													}

													if (sc.DistanceFrom(ebc) < reqClearance)
														safeFromLongRangeDef = false;
												}

												if (safeFromLongRangeDef)
												{
													pForwardStagingCell = pSC;
													break;
												}
											}
										}
									}
								}
							}
						}
					}

					if (pForwardStagingCell)
					{
						int gatheredNearStage = 0;
						bool anyVanguardUnderFire = false;
						for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
						{
							auto const pCheckU = idleCombatUnits[i];
							if (!pCheckU || pCheckU->Type->Naval || IsAmphibiousOrAirTransport(pCheckU->Type))
								continue;

							const CellStruct cuc = pCheckU->GetMapCoords();
							if (MapClass::Instance.GetMovementZoneType(cuc, MovementZone::Normal, false)
								!= waveTargetZone)
							{
								continue;
							}

							if (IsFootActivelyAttackingOrMovingToEnemy(pCheckU)
								|| IsFootNearEnemyFrontline(pCheckU, waveTargetZone))
							{
								continue;
							}

							if (pCheckU->HasBeenAttacked || cuc.DistanceFrom(tc) <= 18.0)
								anyVanguardUnderFire = true;

							if (cuc.DistanceFrom(pForwardStagingCell->MapCoords) <= 14.0)
								gatheredNearStage++;
						}

						if (!anyVanguardUnderFire && gatheredNearStage * 100 < sameZoneCount * 65)
						{
							holdAtForwardStaging = true;
						}
					}
				}
			}
		}

		size_t dispatchedVeh = 0;
		for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
		{
			auto const pU = idleCombatUnits[i];
			const int uz = MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false);
			// Never abort a unit's current attack target/objective or pull a
			// frontline unit away from an active enemy front!
			if (IsFootActivelyAttackingOrMovingToEnemy(pU) || IsFootNearEnemyFrontline(pU, uz))
				continue;

			const bool canCrossWaterDirectly = (pU->Type->MovementZone == MovementZone::Amphibious
				|| pU->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pU->Type->MovementZone == MovementZone::AmphibiousCrusher
				|| pU->Type->MovementZone == MovementZone::Fly || pU->Type->MovementZone == MovementZone::Subterrannean
				|| pU->Type->JumpJet || pU->Type->Teleporter);
			// If this unit was ferried across islands by transport, check if an
			// unattacked sister transport of the battalion is still unloading right
			// next to us (<= 10 cells)
			if (isWaitingForSisterTransportUnload(pU, canCrossWaterDirectly))
			{
				continue; // Regroup battalion on beachhead while unattacked sister
						// transport finishes unloading!
			}

			CellClass* pUnitWaveGoal = pWaveTargetCell;
			int goalZone = waveTargetZone;
			const CellStruct preTransportGoal = LookupPassengerIslandGoal(pU->UniqueID);
			if (preTransportGoal != CellStruct::Empty)
			{
				const int ptz = MapClass::Instance.GetMovementZoneType(preTransportGoal, MovementZone::Normal, false);
				if (ptz >= 0 && uz == ptz)
				{
					if (pU->GetMapCoords().DistanceFrom(preTransportGoal) <= 5.0)
					{
						ClearPassengerIslandGoal(pU->UniqueID);
					}
					else if (CellClass* pPreCell = MapClass::Instance.TryGetCellAt(preTransportGoal))
					{
						pUnitWaveGoal = pPreCell;
						goalZone = ptz;
					}
				}
			}

			if (pUnitWaveGoal == pWaveTargetCell && pSecondaryWaveTargetCell
				&& idleCombatUnits.size() >= (minWaveUnits + 2) && (pU->UniqueID % 3 == 0)
				&& (canCrossWaterDirectly || (secWaveZone >= 0 && uz == secWaveZone)))
			{
				pUnitWaveGoal = pSecondaryWaveTargetCell;
				goalZone = secWaveZone;
			}

			const bool goalInWater = (pUnitWaveGoal->LandType == LandType::Water);
			if (!canCrossWaterDirectly && (goalInWater || goalZone < 0 || uz < 0 || uz != goalZone))
			{
				continue; // Keep ground-only vehicles in Guard state on their island so
						// they board Amphibious/Air Transports!
			}

			if (pU->Type->JumpJet && pU->Type->Strength >= 1000 && pU->DistanceFrom(pUnitWaveGoal) > 7680)
			{
				const CellStruct uCoords = pU->GetMapCoords();
				const CellStruct flankCoords { uCoords.X, pUnitWaveGoal->MapCoords.Y };
				if (auto const pFlankCell = MapClass::Instance.TryGetCellAt(flankCoords))
				{
					pU->SetDestination(pFlankCell, true);
					pU->QueueMission(Mission::Move, true);
					dispatchedVeh++;
					continue;
				}
			}

			const CellStruct uCellCoords = pU->GetMapCoords();
			const bool canUseForwardStaging = holdAtForwardStaging && pForwardStagingCell && uz == waveTargetZone
				&& pUnitWaveGoal == pWaveTargetCell
				&& (uCellCoords.DistanceFrom(pUnitWaveGoal->MapCoords)
					> pForwardStagingCell->MapCoords.DistanceFrom(pUnitWaveGoal->MapCoords) + 2.0);
			if (canUseForwardStaging)
			{
				AbstractClass* pCurDest = pU->Destination ? pU->Destination : pU->MegaDestination;
				const bool alreadyHeadingToStage = (pCurDest != nullptr && pU->CurrentMission != Mission::Guard
					&& CellClass::Coord2Cell(pCurDest->GetCoords()).DistanceFrom(pForwardStagingCell->MapCoords)
						<= 6.0);
				if (!alreadyHeadingToStage)
				{
					OrderFootToAttackMove(pU, pForwardStagingCell);
				}
			}
			else if ((pVisibleEnemyBld || endgameRemnantHunt) && pU->Type->GetWeapon(0)
				&& pU->Type->GetWeapon(0)->WeaponType)
			{
				const int wpnRange = pU->Type->GetWeapon(0)->WeaponType->Range;
				if (pVisibleEnemyBld && pUnitWaveGoal == pWaveTargetCell
					&& pU->DistanceFrom(pVisibleEnemyBld) <= std::max(1280, wpnRange + 384))
				{
					pU->ClearMegaMissionData();
					pU->SetTarget(pVisibleEnemyBld);
					pU->QueueMission(Mission::Attack, true);
				}
				else
				{
					OrderFootToAttackMove(pU, pUnitWaveGoal);
				}
			}
			else
			{
				pU->ClearMegaMissionData();
				pU->QueueMission(Mission::Hunt, true);
			}

			dispatchedVeh++;
		}

		const size_t desiredInfReserve = static_cast<size_t>(std::clamp(1 + (reserveBias / 160), 0, 3));
		const size_t infReserve = idleCombatInf.size() > (desiredInfReserve + 2) ? desiredInfReserve : 0;
		size_t dispatchedInf = 0;
		for (size_t i = infReserve; i < idleCombatInf.size(); ++i)
		{
			auto const pI = idleCombatInf[i];
			const int iz = MapClass::Instance.GetMovementZoneType(pI->GetMapCoords(), MovementZone::Normal, false);
			if (IsFootActivelyAttackingOrMovingToEnemy(pI) || IsFootNearEnemyFrontline(pI, iz))
				continue;

			const bool canInfCrossWater = (pI->Type->MovementZone == MovementZone::Amphibious
				|| pI->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pI->Type->MovementZone == MovementZone::Fly || pI->Type->JumpJet || pI->Type->Teleporter);
			CellClass* pInfWaveGoal = pWaveTargetCell;
			int infGoalZone = waveTargetZone;
			const CellStruct preTransportInfGoal = LookupPassengerIslandGoal(pI->UniqueID);
			if (preTransportInfGoal != CellStruct::Empty)
			{
				const int ptz
					= MapClass::Instance.GetMovementZoneType(preTransportInfGoal, MovementZone::Normal, false);
				if (ptz >= 0 && iz == ptz)
				{
					if (pI->GetMapCoords().DistanceFrom(preTransportInfGoal) <= 5.0)
					{
						ClearPassengerIslandGoal(pI->UniqueID);
					}
					else if (CellClass* pPreCell = MapClass::Instance.TryGetCellAt(preTransportInfGoal))
					{
						pInfWaveGoal = pPreCell;
						infGoalZone = ptz;
					}
				}
			}

			if (pInfWaveGoal == pWaveTargetCell && pSecondaryWaveTargetCell && idleCombatInf.size() >= 5
				&& (pI->UniqueID % 3 == 0) && (canInfCrossWater || (secWaveZone >= 0 && iz == secWaveZone)))
			{
				pInfWaveGoal = pSecondaryWaveTargetCell;
				infGoalZone = secWaveZone;
			}

			const bool infGoalInWater = (pInfWaveGoal->LandType == LandType::Water);
			if (!canInfCrossWater && (infGoalInWater || infGoalZone < 0 || iz < 0 || iz != infGoalZone))
			{
				continue; // Keep ground infantry ready on their island to board
						// transports!
			}

			const int infWpnRange = (pI->Type->GetWeapon(0) && pI->Type->GetWeapon(0)->WeaponType)
				? pI->Type->GetWeapon(0)->WeaponType->Range
				: 1024;
			if (pVisibleEnemyBld && pInfWaveGoal == pWaveTargetCell
				&& pI->DistanceFrom(pVisibleEnemyBld) <= std::max(1024, infWpnRange + 384))
			{
				pI->ClearMegaMissionData();
				pI->SetTarget(pVisibleEnemyBld);
				pI->QueueMission(Mission::Attack, true);
			}
			else
			{
				OrderFootToAttackMove(pI, pInfWaveGoal);
			}

			dispatchedInf++;
		}

		if (dispatchedVeh > 0 || dispatchedInf > 0)
		{
			Debug::Log("[NeuralAI] Frame %d | House #%d (%s) launched Offensive Wave "
					"(%zu Vehicles + %zu Infantry) -> Target Cell (%d,%d)\n",
				Unsorted::CurrentFrame, pHouse->ArrayIndex,
				(pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI", dispatchedVeh, dispatchedInf,
				pWaveTargetCell->MapCoords.X, pWaveTargetCell->MapCoords.Y);
		}
	}

	// Slot 75 (Parallel Multi-Group Ground Push & Full-Map Naval Coastal
	// Bombardment Saturation): Combat vehicles/infantry on ANY landmass that
	// shares a MovementZone with enemy structures split into two
	//    parallel assault groups (primary vs secondary target on that landmass)
	//    and push without hesitation!
	// Naval Warships (including long-range capital ships & missile subs whose
	// land-attack weapon is Primary, Secondary,
	//    or Spawner, e.g., Dreadnought, Aircraft Carrier, Boomer, Nod Missile
	//    Sub, Destroyer) scan the ENTIRE map for enemy Naval Yards and
	//    coastal/island structures within bombardment reach of water, distribute
	//    across up to 4 coastal targets in parallel, sail to offshore water
	//    firing cells, and continuously saturate the enemy coast!
	{
		auto findBestSameLandmassEnemyBld
			= [pHouse](const CellStruct& unitCoords, int unitZone, bool pickSecondaryGroup) -> BuildingClass*
		{
			// First find the closest enemy structure distance on this landmass so our
			// units peel the outer perimeter first instead of suicidally driving
			// through half the enemy base to reach an interior ConYard/Superweapon!
			int minDistOnLandmass = 999999;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
					|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
					|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
				{
					continue;
				}

				const CellStruct ebc = pEB->GetMapCoords();
				if (pEB->Type->Naval || unitZone < 0
					|| MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, false) != unitZone)
				{
					continue;
				}

				const int d = std::abs(ebc.X - unitCoords.X) + std::abs(ebc.Y - unitCoords.Y);
				if (d < minDistOnLandmass)
					minDistOnLandmass = d;
			}

			BuildingClass* pBestBld = nullptr;
			int bestScore = -999999;
			BuildingClass* pSecondBld = nullptr;
			int secondScore = -999999;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
					|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
					|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
				{
					continue;
				}

				const CellStruct ebc = pEB->GetMapCoords();
				if (pEB->Type->Naval || unitZone < 0
					|| MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, false) != unitZone)
				{
					continue;
				}

				const int distCells = std::abs(ebc.X - unitCoords.X) + std::abs(ebc.Y - unitCoords.Y);
				const int depthBeyondPerimeter = std::max(0, distCells - minDistOnLandmass);
				int prio = 60;
				if (pEB->Type->IsBaseDefense && distCells <= 14)
				{
					prio = 165;
				}
				else if (depthBeyondPerimeter <= 6)
				{
					// Among outer-perimeter structures within 6 cells of the front edge,
					// prioritize high-value targets!
					if (pEB->Type->ConstructionYard || pEB->Type->SuperWeapon != -1)
						prio = 125;
					else if (pEB->Type->Factory != AbstractType::None)
						prio = 115;
					else if (pEB->Type->Refinery || pEB->Type->PowerBonus > 0)
						prio = 95;
					else
						prio = 80;
				}

				const int sc
					= prio - (distCells * 12) - (depthBeyondPerimeter > 6 ? (depthBeyondPerimeter - 6) * 20 : 0);
				if (sc > bestScore)
				{
					if (pBestBld && pBestBld->GetMapCoords().DistanceFrom(ebc) >= 6.0)
					{
						secondScore = bestScore;
						pSecondBld = pBestBld;
					}

					bestScore = sc;
					pBestBld = pEB;
				}
				else if (sc > secondScore && pBestBld && pBestBld->GetMapCoords().DistanceFrom(ebc) >= 6.0)
				{
					secondScore = sc;
					pSecondBld = pEB;
				}
			}

			return (pickSecondaryGroup && pSecondBld) ? pSecondBld : pBestBld;
		};

		auto findCoastalNavalBombardmentTarget
			= [pHouse](const CellStruct& shipCoords, int maxStandOffCells, unsigned int shipUniqueID) -> BuildingClass*
		{
			struct NavalCand
			{
				BuildingClass* Bld;
				int Score;
			};
			NavalCand topCands[4]
				= { { nullptr, -999999 }, { nullptr, -999999 }, { nullptr, -999999 }, { nullptr, -999999 } };
			const int waterCheckRadius = std::clamp(maxStandOffCells, 6, 26);

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
					|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
					|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
				{
					continue;
				}

				const CellStruct ebc = pEB->GetMapCoords();
				if (ebc.X <= 1 && ebc.Y <= 1)
					continue;

				bool reachableFromWater = pEB->Type->Naval;
				if (!reachableFromWater)
				{
					for (int r = 3; r <= waterCheckRadius && !reachableFromWater; r += 3)
					{
						const CellStruct probes[8] = { { static_cast<short>(ebc.X + r), ebc.Y },
							{ static_cast<short>(ebc.X - r), ebc.Y }, { ebc.X, static_cast<short>(ebc.Y + r) },
							{ ebc.X, static_cast<short>(ebc.Y - r) },
							{ static_cast<short>(ebc.X + (r * 3) / 4), static_cast<short>(ebc.Y + (r * 3) / 4) },
							{ static_cast<short>(ebc.X - (r * 3) / 4), static_cast<short>(ebc.Y + (r * 3) / 4) },
							{ static_cast<short>(ebc.X + (r * 3) / 4), static_cast<short>(ebc.Y - (r * 3) / 4) },
							{ static_cast<short>(ebc.X - (r * 3) / 4), static_cast<short>(ebc.Y - (r * 3) / 4) } };
						for (const auto& pr : probes)
						{
							if (auto const pWC = MapClass::Instance.TryGetCellAt(pr))
							{
								if (pWC->LandType == LandType::Water)
								{
									reachableFromWater = true;
									break;
								}
							}
						}
					}
				}

				if (!reachableFromWater)
					continue;

				const int distCells = std::abs(ebc.X - shipCoords.X) + std::abs(ebc.Y - shipCoords.Y);
				int prio = 200;
				if (pEB->Type->Naval || pEB->Type->IsBaseDefense)
					prio = 360;
				else if (pEB->Type->ConstructionYard || pEB->Type->SuperWeapon != -1)
					prio = 330;
				else if (pEB->Type->Factory != AbstractType::None)
					prio = 300;
				else if (pEB->Type->Refinery || pEB->Type->PowerBonus > 0)
					prio = 260;

				if (pEB->Owner->ArrayIndex == pHouse->EnemyHouseIndex)
					prio += 60;

				const int sc = prio - (distCells * 2);

				for (int k = 0; k < 4; ++k)
				{
					if (sc > topCands[k].Score)
					{
						for (int m = 3; m > k; --m)
							topCands[m] = topCands[m - 1];
						topCands[k] = { pEB, sc };
						break;
					}
				}
			}

			int validCount = 0;
			for (int k = 0; k < 4; ++k)
			{
				if (topCands[k].Bld != nullptr)
					validCount++;
			}

			if (validCount == 0)
				return nullptr;

			return topCands[shipUniqueID % static_cast<unsigned int>(validCount)].Bld;
		};

		auto findOffshoreFiringWaterCell
			= [](UnitClass* pShip, const CellStruct& targetCoords, int desiredRangeCells) -> CellClass*
		{
			const CellStruct sc = pShip->GetMapCoords();
			const int dx = sc.X - targetCoords.X;
			const int dy = sc.Y - targetCoords.Y;
			const int dist = std::max(1, std::max(std::abs(dx), std::abs(dy)));
			const int lat = (static_cast<int>(pShip->UniqueID % 5) - 2) * 2;
			const int perpX = (-dy * lat) / dist;
			const int perpY = (dx * lat) / dist;
			const int maxR = std::clamp(desiredRangeCells, 5, 22);
			for (int r = std::max(4, maxR - 3); r <= maxR; ++r)
			{
				const CellStruct probe { static_cast<short>(targetCoords.X + (dx * r) / dist + perpX),
					static_cast<short>(targetCoords.Y + (dy * r) / dist + perpY) };
				if (auto const pWC = MapClass::Instance.TryGetCellAt(probe))
				{
					if (pWC->LandType == LandType::Water
						&& pWC->IsClearToMove(
							pShip->Type->SpeedType, true, true, -1, pShip->Type->MovementZone, -1, false))
					{
						return pWC;
					}
				}
			}

			CellClass* pBestWater = nullptr;
			int bestWaterDist = 999999;
			for (int wy = -maxR; wy <= maxR; wy += 2)
			{
				for (int wx = -maxR; wx <= maxR; wx += 2)
				{
					const int dTarget = std::max(std::abs(wx), std::abs(wy));
					if (dTarget < 3 || dTarget > maxR)
						continue;

					const CellStruct wc { static_cast<short>(targetCoords.X + wx),
						static_cast<short>(targetCoords.Y + wy) };
					if (auto const pWC = MapClass::Instance.TryGetCellAt(wc))
					{
						if (pWC->LandType == LandType::Water
							&& pWC->IsClearToMove(
								pShip->Type->SpeedType, true, true, -1, pShip->Type->MovementZone, -1, false))
						{
							const int dShip = std::abs(wc.X - sc.X) + std::abs(wc.Y - sc.Y);
							if (dShip < bestWaterDist)
							{
								bestWaterDist = dShip;
								pBestWater = pWC;
							}
						}
					}
				}
			}

			return pBestWater;
		};

		auto findNearestSameZoneEnemyUnit
			= [pHouse](const CellStruct& unitCoords, int unitZone, int maxDistCells) -> UnitClass*
		{
			UnitClass* pBestEU = nullptr;
			int bestDist = maxDistCells + 1;
			for (auto const pEU : UnitClass::Array)
			{
				if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Naval)
					continue;

				if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral()
					|| pEU->Owner->IsObserver() || pEU->Owner->Defeated)
				{
					continue;
				}

				const CellStruct euc = pEU->GetMapCoords();
				const int d = std::abs(euc.X - unitCoords.X) + std::abs(euc.Y - unitCoords.Y);
				if (d >= bestDist)
					continue;

				if (unitZone >= 0
					&& MapClass::Instance.GetMovementZoneType(euc, MovementZone::Normal, false) != unitZone)
				{
					continue;
				}

				if (pEU->GetCell() && IsCellVisibleToAI(pHouse, pEU->GetCell()))
				{
					bestDist = d;
					pBestEU = pEU;
				}
			}

			return pBestEU;
		};

		for (auto const pBeachU : idleCombatUnits)
		{
			if (!pBeachU || !pBeachU->Type || IsAmphibiousOrAirTransport(pBeachU->Type))
				continue;

			if (IsFootActivelyAttackingOrMovingToEnemy(pBeachU))
			{
				continue; // Already attacking or moving toward an enemy objective:
						// never abort its current target!
			}

			const auto pWpn0 = pBeachU->Type->GetWeapon(0) ? pBeachU->Type->GetWeapon(0)->WeaponType : nullptr;
			const auto pWpn1 = pBeachU->Type->GetWeapon(1) ? pBeachU->Type->GetWeapon(1)->WeaponType : nullptr;
			const bool isCarrierSpawner
				= (pBeachU->Type->Spawns != nullptr) || (pWpn0 && pWpn0->Spawner) || (pWpn1 && pWpn1->Spawner);
			if ((!pWpn0 || pWpn0->Damage <= 0) && (!pWpn1 || pWpn1->Damage <= 0) && !isCarrierSpawner)
				continue;

			const CellStruct uc = pBeachU->GetMapCoords();
			if (pBeachU->Type->Naval)
			{
				const bool wpn0HitsLand = pWpn0 && (pWpn0->Damage > 0 || pWpn0->Spawner)
					&& (!pWpn0->Projectile || pWpn0->Projectile->AG || pWpn0->Spawner || isCarrierSpawner);
				const bool wpn1HitsLand = pWpn1 && (pWpn1->Damage > 0 || pWpn1->Spawner)
					&& (!pWpn1->Projectile || pWpn1->Projectile->AG || pWpn1->Spawner);
				const WeaponTypeClass* pLandSiegeWpn = nullptr;
				if (wpn0HitsLand && wpn1HitsLand)
					pLandSiegeWpn = (pWpn1->Range > pWpn0->Range) ? pWpn1 : pWpn0;
				else if (wpn1HitsLand)
					pLandSiegeWpn = pWpn1;
				else if (wpn0HitsLand)
					pLandSiegeWpn = pWpn0;

				if (!pLandSiegeWpn)
				{
					// Anti-naval / anti-sub warship (Torpedo Sub, Dolphin, Squid): hunt
					// enemy ships or Naval Yards!
					if (pEnemySubOrShip)
					{
						pBeachU->SetTarget(pEnemySubOrShip);
						pBeachU->QueueMission(Mission::Attack, true);
					}

					continue;
				}

				const int siegeRangeLeptons = std::max(pLandSiegeWpn->Range, isCarrierSpawner ? (18 * 256) : 1536);
				const int siegeRangeCells = std::clamp(siegeRangeLeptons / 256, 6, 26);
				if (BuildingClass* pNavalTargetBld
					= findCoastalNavalBombardmentTarget(uc, siegeRangeCells, pBeachU->UniqueID))
				{
					const int distLeptons = pBeachU->DistanceFrom(pNavalTargetBld);
					if (distLeptons <= std::max(2560, siegeRangeLeptons))
					{
						pBeachU->ClearMegaMissionData();
						pBeachU->SetTarget(pNavalTargetBld);
						pBeachU->QueueMission(Mission::Attack, true);
					}
					else if (CellClass* pOffshoreFireCell = findOffshoreFiringWaterCell(
								pBeachU, pNavalTargetBld->GetMapCoords(), std::max(5, siegeRangeCells - 2)))
					{
						OrderFootToAttackMove(pBeachU, pOffshoreFireCell);
					}
					else if (auto const pNCell = pNavalTargetBld->GetCell())
					{
						OrderFootToAttackMove(pBeachU, pNCell);
					}
				}

				continue;
			}

			const int uz = MapClass::Instance.GetMovementZoneType(uc, MovementZone::Normal, false);
			const bool canBeachUCrossWater = (pBeachU->Type->MovementZone == MovementZone::Amphibious
				|| pBeachU->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pBeachU->Type->MovementZone == MovementZone::AmphibiousCrusher
				|| pBeachU->Type->MovementZone == MovementZone::Fly
				|| pBeachU->Type->MovementZone == MovementZone::Subterrannean || pBeachU->Type->JumpJet
				|| pBeachU->Type->Teleporter);
			if (isWaitingForSisterTransportUnload(pBeachU, canBeachUCrossWater))
			{
				continue; // Wait on beachhead while unattacked adjacent sister
						// transport finishes unloading!
			}

			const int maxWpnRangeLeptons = std::max(
				pWpn0 ? static_cast<int>(pWpn0->Range) : 1280, pWpn1 ? static_cast<int>(pWpn1->Range) : 1280);
			UnitClass* pCloseEnemyVeh = findNearestSameZoneEnemyUnit(uc, uz, 12);
			if (pCloseEnemyVeh)
			{
				if (pBeachU->DistanceFrom(pCloseEnemyVeh) <= maxWpnRangeLeptons + 384)
				{
					pBeachU->ClearMegaMissionData();
					pBeachU->SetTarget(pCloseEnemyVeh);
					pBeachU->QueueMission(Mission::Attack, true);
				}
				else if (auto const pCloseECell = pCloseEnemyVeh->GetCell())
				{
					OrderFootToAttackMove(pBeachU, pCloseECell);
				}

				continue;
			}

			// Resume pre-transport initial objective if this unit just landed on its
			// target island!
			const CellStruct preGoal = LookupPassengerIslandGoal(pBeachU->UniqueID);
			if (preGoal != CellStruct::Empty && uz >= 0
				&& MapClass::Instance.GetMovementZoneType(preGoal, MovementZone::Normal, false) == uz)
			{
				if (uc.DistanceFrom(preGoal) <= 5.0)
				{
					ClearPassengerIslandGoal(pBeachU->UniqueID);
				}
				else if (CellClass* pPreGoalCell = MapClass::Instance.TryGetCellAt(preGoal))
				{
					OrderFootToAttackMove(pBeachU, pPreGoalCell);
					continue;
				}
			}

			if (BuildingClass* pSameIslBld = findBestSameLandmassEnemyBld(uc, uz, (pBeachU->UniqueID & 1) != 0))
			{
				const double distToBld = uc.DistanceFrom(pSameIslBld->GetMapCoords());
				const bool onEnemyIsland = (homeGroundZone >= 0 && uz >= 0 && uz != homeGroundZone);
				// Do NOT trickle units 1-by-1 from our home base when
				// idleCombatUnits.size() < minWaveUnits! Only engage on home island
				// once critical mass (>= minWaveUnits) is assembled, or if the unit is
				// already at the enemy perimeter (<= 20 cells)!
				if (onEnemyIsland || endgameRemnantHunt || distToBld <= 20.0 || idleCombatUnits.size() >= minWaveUnits)
				{
					if (pBeachU->DistanceFrom(pSameIslBld) <= maxWpnRangeLeptons + 384 && pSameIslBld->GetCell()
						&& (pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pSameIslBld->GetCell())))
					{
						pBeachU->ClearMegaMissionData();
						pBeachU->SetTarget(pSameIslBld);
						pBeachU->QueueMission(Mission::Attack, true);
					}
					else if (auto const pSameIslCell = pSameIslBld->GetCell())
					{
						OrderFootToAttackMove(pBeachU, pSameIslCell);
					}
				}
			}
		}

		for (auto const pBeachI : idleCombatInf)
		{
			if (!pBeachI || !pBeachI->Type)
				continue;

			if (IsFootActivelyAttackingOrMovingToEnemy(pBeachI))
				continue;

			const CellStruct ic = pBeachI->GetMapCoords();
			const int iz = MapClass::Instance.GetMovementZoneType(ic, MovementZone::Normal, false);
			const bool canBeachICrossWater = (pBeachI->Type->MovementZone == MovementZone::Amphibious
				|| pBeachI->Type->MovementZone == MovementZone::AmphibiousDestroyer
				|| pBeachI->Type->MovementZone == MovementZone::Fly || pBeachI->Type->JumpJet
				|| pBeachI->Type->Teleporter);
			if (isWaitingForSisterTransportUnload(pBeachI, canBeachICrossWater))
				continue;

			const CellStruct preInfGoal = LookupPassengerIslandGoal(pBeachI->UniqueID);
			if (preInfGoal != CellStruct::Empty && iz >= 0
				&& MapClass::Instance.GetMovementZoneType(preInfGoal, MovementZone::Normal, false) == iz)
			{
				if (ic.DistanceFrom(preInfGoal) <= 5.0)
				{
					ClearPassengerIslandGoal(pBeachI->UniqueID);
				}
				else if (CellClass* pPreInfCell = MapClass::Instance.TryGetCellAt(preInfGoal))
				{
					OrderFootToAttackMove(pBeachI, pPreInfCell);
					continue;
				}
			}

			if (BuildingClass* pSameIslBld = findBestSameLandmassEnemyBld(ic, iz, (pBeachI->UniqueID & 1) != 0))
			{
				const double distToBld = ic.DistanceFrom(pSameIslBld->GetMapCoords());
				const bool onEnemyIsland = (homeGroundZone >= 0 && iz >= 0 && iz != homeGroundZone);
				if (onEnemyIsland || endgameRemnantHunt || distToBld <= 20.0 || idleCombatUnits.size() >= minWaveUnits
					|| idleCombatInf.size() >= (minWaveUnits + 3))
				{
					const auto pInfW0 = pBeachI->Type->GetWeapon(0) ? pBeachI->Type->GetWeapon(0)->WeaponType : nullptr;
					const int infWpnRange = pInfW0 ? static_cast<int>(pInfW0->Range) : 1024;
					if (pBeachI->DistanceFrom(pSameIslBld) <= infWpnRange + 384 && pSameIslBld->GetCell()
						&& (pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pSameIslBld->GetCell())))
					{
						pBeachI->ClearMegaMissionData();
						pBeachI->SetTarget(pSameIslBld);
						pBeachI->QueueMission(Mission::Attack, true);
					}
					else if (auto const pSameIslCell = pSameIslBld->GetCell())
					{
						OrderFootToAttackMove(pBeachI, pSameIslCell);
					}
				}
			}
		}
	}

	// Count how many amphibious transports are currently loading on our home
	// island so loaded transports can synchronize and sail together as a D-Day
	// Convoy!
	UnitClass* pHomeExpansionDeployer = nullptr;
	if (homeGroundZone >= 0
		&& (pIslandResourceGoalCell != nullptr || !hasUncolonizedOreOnHomeIsland || pHouse->Buildings.Count >= 16))
	{
		for (auto const pOwnU : UnitClass::Array)
		{
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type
				|| !pOwnU->Type->DeploysInto)
			{
				continue;
			}

			const auto pUT = pOwnU->Type;
			const bool isSelfAmphibOrAir = (pUT->MovementZone == MovementZone::Amphibious
				|| pUT->MovementZone == MovementZone::AmphibiousDestroyer
				|| pUT->MovementZone == MovementZone::AmphibiousCrusher || pUT->MovementZone == MovementZone::Fly
				|| pUT->JumpJet);
			if (isSelfAmphibOrAir)
				continue;

			const int uz = MapClass::Instance.GetMovementZoneType(pOwnU->GetMapCoords(), MovementZone::Normal, false);
			if (uz < 0 || uz == resourceIslandZone)
				continue;

			int conYardsInUz = 0;
			int hubsInUz = 0;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->Insignificant
					|| pOwnB->Type->InvisibleInGame)
				{
					continue;
				}

				if (MapClass::Instance.GetMovementZoneType(pOwnB->GetMapCoords(), MovementZone::Normal, false) != uz)
					continue;

				if (pOwnB->Type->ConstructionYard)
					conYardsInUz++;

				if (pOwnB->Type->Refinery || pOwnB->Type->ResourceDestination || pOwnB->Type->BaseNormal
					|| pOwnB->Type->Factory != AbstractType::None)
				{
					hubsInUz++;
				}
			}

			const bool isExpMCV = (pUT->DeploysInto->ConstructionYard && (conYardsInUz >= 1 || hubsInUz >= 2));
			const bool isExpOutpostOrRef = (!pUT->DeploysInto->ConstructionYard && hubsInUz >= 1
				&& (pOwnU == pExtraHomeSlaveMiner
					|| (!pUT->Harvester && !pUT->ResourceGatherer
						&& (pUT->DeploysInto->Refinery || pUT->DeploysInto->ResourceDestination
							|| pUT->DeploysInto->BaseNormal || pUT->DeploysInto->Factory != AbstractType::None))));
			if (isExpMCV || isExpOutpostOrRef)
			{
				pHomeExpansionDeployer = pOwnU;
				if (isExpMCV)
					break;
			}
		}
	}

	InfantryClass* pHomeIslandEngineer = nullptr;
	if (homeGroundZone >= 0 && pIslandTechGoalCell != nullptr)
	{
		for (auto const pOwnInf : InfantryClass::Array)
		{
			if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type
				|| !pOwnInf->Type->Engineer)
			{
				continue;
			}

			const int iz = MapClass::Instance.GetMovementZoneType(pOwnInf->GetMapCoords(), MovementZone::Normal, false);
			if (iz == homeGroundZone)
			{
				pHomeIslandEngineer = pOwnInf;
				break;
			}
		}
	}

	int homeTransportsStillLoading = 0;
	int readyCombatConvoyTransports = 0;

	for (auto const pCheckT : UnitClass::Array)
	{
		if (!pCheckT || pCheckT->Owner != pHouse || pCheckT->InLimbo || !pCheckT->IsAlive || !pCheckT->Type
			|| !IsAmphibiousOrAirTransport(pCheckT->Type))
		{
			continue;
		}

		if (pCheckT->Type->MovementZone == MovementZone::Fly || pCheckT->Type->JumpJet)
			continue;

		bool checkCarryingMCV = false;
		for (FootClass* pP = pCheckT->Passengers.GetFirstPassenger(); pP != nullptr;
			pP = abstract_cast<FootClass*>(pP->NextObject))
		{
			if (auto const pPU = abstract_cast<UnitClass*>(pP))
			{
				if (pPU->Type && pPU->Type->DeploysInto != nullptr)
				{
					checkCarryingMCV = true;
					break;
				}
			}
		}

		if (checkCarryingMCV)
			continue;

		const CellStruct checkCoords = pCheckT->GetMapCoords();
		const int tz = MapClass::Instance.GetMovementZoneType(checkCoords, MovementZone::Normal, false);
		const bool isCheckReady = (pCheckT->Passengers.NumPassengers >= 3 || pCheckT->Passengers.GetTotalSize() >= 6);
		if (isCheckReady)
		{
			if (targetIslandZone < 0 || tz != targetIslandZone)
				readyCombatConvoyTransports++;

			continue;
		}

		// If a loading transport is intercepted/under attack, do NOT count it as a
		// loading convoy sister so other transports sail immediately!
		if (pCheckT->HasBeenAttacked || pCheckT->GetHealthPercentage() < 0.65)
			continue;

		const bool activelyBoarding = HasActiveBoardingPassenger(pCheckT, tz);
		const bool nearPickupIsland = (tz == homeGroundZone || tz == primaryArmyZone
			|| (homeCoords != CellStruct::Empty && checkCoords.DistanceFrom(homeCoords) <= 28.0));
		const bool hasTroopsForSister = activelyBoarding
			|| (pCheckT->Passengers.NumPassengers > 0 && (!idleCombatUnits.empty() || !idleCombatInf.empty()))
			|| (pCheckT->Passengers.NumPassengers == 0
				&& (idleCombatUnits.size() >= 2 || (idleCombatUnits.size() + idleCombatInf.size()) >= 4));
		if (nearPickupIsland && hasTroopsForSister)
			homeTransportsStillLoading++;
	}

	bool deployerTransportAssigned = false;

	for (auto const pTrans : UnitClass::Array)
	{
		if (!pTrans || pTrans->Owner != pHouse || pTrans->InLimbo || !pTrans->IsAlive || !pTrans->Type)
			continue;

		// Subterranean APC / Ground APC Strike Team: If carrying Engineers or >= 3
		// Infantry, burrow/drive to enemy base and Unload! (Never unload
		// NoManualUnload/NoManualEnter units!)
		if (!IsAmphibiousOrAirTransport(pTrans->Type) && pTrans->Type->Passengers >= 3 && !pTrans->Type->OpenTopped
			&& !pTrans->Type->Gunner && !HasNoManualUnload(pTrans->Type) && !HasNoManualEnter(pTrans->Type))
		{
			if (pTrans->Passengers.NumPassengers > 0 && pVisibleEnemyBld)
			{
				bool hasEngineerOrFullSquad = (pTrans->Passengers.NumPassengers >= 3);

				for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
					pPass = abstract_cast<FootClass*>(pPass->NextObject))
				{
					if (auto const pPassInf = abstract_cast<InfantryClass*>(pPass))
					{
						if (pPassInf->Type && (pPassInf->Type->Engineer || pPassInf->Type->C4))
						{
							hasEngineerOrFullSquad = true;

							break;
						}
					}
				}

				if (hasEngineerOrFullSquad)
				{
					if (pTrans->DistanceFrom(pVisibleEnemyBld) <= 640)
					{
						if (pTrans->CurrentMission != Mission::Unload)
							pTrans->QueueMission(Mission::Unload, true);
					}
					else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard)
					{
						if (auto const pEnemyCell = pVisibleEnemyBld->GetCell())
						{
							pTrans->SetDestination(pEnemyCell, true);

							pTrans->QueueMission(Mission::Move, true);
						}
					}
				}
			}

			continue;
		}

		if (auto const pTransTypeExt = TechnoTypeExt::Fetch(pTrans->Type))
		{
			if (pTransTypeExt->Carryall && pTrans->Passengers.NumPassengers == 0)
			{
				if (auto const pCarryExt = UnitExt::Fetch(pTrans))
				{
					if (pCarryExt->CarryallCargo && pCarryExt->CarryallCargo->IsAlive)
					{
						auto const pCarriedUnit = pCarryExt->CarryallCargo;
						CellClass* pDropCell = nullptr;
						const CellStruct customCoords = LookupPassengerIslandGoal(pCarriedUnit->UniqueID);
						if (customCoords != CellStruct::Empty)
						{
							const int cz
								= MapClass::Instance.GetMovementZoneType(customCoords, MovementZone::Normal, false);
							pDropCell = FindSafeIslandLandingCell(pHouse, customCoords, cz,
								pCarriedUnit->Type && pCarriedUnit->Type->DeploysInto != nullptr);
						}

						if (!pDropCell && pCarriedUnit->Type)
						{
							if (pRepairDepot && pCarriedUnit->GetHealthPercentage() < 0.45)
							{
								pDropCell = pRepairDepot->GetCell();
							}
							else if ((pCarriedUnit->Type->DeploysInto != nullptr || pCarriedUnit->Type->Harvester
										|| pCarriedUnit->Type->Weeder)
								&& pIslandResourceGoalCell)
							{
								pDropCell = pIslandResourceGoalCell;
							}
						}

						if (!pDropCell)
						{
							pDropCell = pIslandGoalCell
								? pIslandGoalCell
								: (pIslandTechGoalCell
										? pIslandTechGoalCell
										: (pIslandResourceGoalCell ? pIslandResourceGoalCell : pWaveTargetCell));
						}

						if (pDropCell && pCarryExt->CarryallState == JumpjetCarryallState::Carrying)
						{
							if (pTrans->GetMapCoords().DistanceFrom(pDropCell->MapCoords) <= 2.5
								&& pCarryExt->CarryallCanDropAt(pTrans->GetCell()))
							{
								pTrans->QueueMission(Mission::Unload, true);
							}
							else if (pTrans->CurrentMission == Mission::Guard
								|| pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 90 < 25))
							{
								pTrans->SetDestination(pDropCell, true);
								pTrans->QueueMission(Mission::Move, true);
							}
						}

						continue;
					}
					else if (pCarryExt->CarryallState == JumpjetCarryallState::Idle
						&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard)
						&& (Unsorted::CurrentFrame % 90 < 35))
					{
						bool pickedCarryallTarget = false;
						for (auto const& ord : pendingCrossIslandOrders)
						{
							if (auto const pOrdU = abstract_cast<UnitClass*>(ord.Passenger))
							{
								if (pOrdU->IsAlive && !pOrdU->InLimbo && pCarryExt->CarryallCanLift(pOrdU))
								{
									pTrans->SetDestination(pOrdU, true);
									pTrans->QueueMission(Mission::Move, true);
									pickedCarryallTarget = true;
									break;
								}
							}
						}

						for (auto const pU : UnitClass::Array)
						{
							if (pickedCarryallTarget)
								break;

							if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type
								|| !pCarryExt->CarryallCanLift(pU))
							{
								continue;
							}

							const int uZone = MapClass::Instance.GetMovementZoneType(
								pU->GetMapCoords(), MovementZone::Normal, false);
							const bool isCriticallyDamagedTank
								= (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
							const bool isLoadedStrikeAPC
								= (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 1
									&& (pWaveTargetCell || pIslandGoalCell || pIslandTechGoalCell));
							const bool isIslandExpansionMCV = (pU->Type->DeploysInto != nullptr
								&& pIslandResourceGoalCell && uZone >= 0 && uZone != resourceIslandZone
								&& (pU == pHomeExpansionDeployer || pHouse->NumConYards >= 1
									|| pHouse->CountOwnedNow(pU->Type) >= 2 || pHouse->OwnedBuildings >= 6));
							const bool isIslandHarvester
								= ((pU->Type->Harvester || pU->Type->Weeder) && pIslandResourceGoalCell
									&& uZone == homeGroundZone && pHouse->CountOwnedNow(pU->Type) >= 2);
							const bool isIslandCombatVehicle = (pIslandGoalCell != nullptr && uZone == homeGroundZone
								&& !IsAmphibiousOrAirTransport(pU->Type) && !pU->Type->Harvester && !pU->Type->Weeder
								&& pU->Type->DeploysInto == nullptr
								&& (pU->CurrentMission == Mission::Guard || pU->CurrentMission == Mission::Area_Guard)
								&& !IsFootActivelyAttackingOrMovingToEnemy(pU) && !IsFootNearEnemyFrontline(pU, uZone));
							if (isCriticallyDamagedTank || isLoadedStrikeAPC || isIslandExpansionMCV
								|| isIslandHarvester || isIslandCombatVehicle)
							{
								pTrans->SetDestination(pU, true);
								pTrans->QueueMission(Mission::Move, true);
								pickedCarryallTarget = true;
								break;
							}
						}

						if (pickedCarryallTarget || !IsAmphibiousOrAirTransport(pTrans->Type))
							continue;
					}
					else if (pCarryExt->CarryallState != JumpjetCarryallState::Idle)
					{
						continue;
					}
				}
			}
		}

		if (!IsAmphibiousOrAirTransport(pTrans->Type))
			continue;

		const int numPass = pTrans->Passengers.NumPassengers;
		const int totalPassSize = pTrans->Passengers.GetTotalSize();
		bool carryingMCV = false;
		bool carryingStolenForeignMCV = false;

		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
			pPass = abstract_cast<FootClass*>(pPass->NextObject))
		{
			if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
			{
				if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr)
				{
					carryingMCV = true; // MCV or Red Alert 3-style CABAL Expansion Nanocore!
					const unsigned int ownHouseBit
						= (pHouse->Type && pHouse->Type->ArrayIndex >= 0 && pHouse->Type->ArrayIndex < 32)
						? (1u << pHouse->Type->ArrayIndex)
						: 0u;
					if (pPassUnit->Type->DeploysInto->ConstructionYard
						&& pHouse->CountOwnedNow(pPassUnit->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1
						&& (ownHouseBit == 0u || !pPassUnit->Type->DeploysInto->InOwners(ownHouseBit))
						&& !HouseExt::PrerequisitesMet(pHouse, pPassUnit->Type))
					{
						carryingStolenForeignMCV = true;
					}

					break;
				}
			}
		}

		const bool isReadyToSail = carryingMCV || numPass >= 3 || totalPassSize >= 6;
		const CellStruct transCoords = pTrans->GetMapCoords();
		const int transZone = MapClass::Instance.GetMovementZoneType(transCoords, MovementZone::Normal, false);

		// Slot 82 (Island Extraction of Stolen Foreign MCV to Safe Home Island):
		// If this transport is carrying a stolen foreign-faction MCV from an
		// enemy island, sail straight back to our safe Home Island (homeCoords /
		// homeGroundZone) and unload it there so it can deploy safely!
		if (carryingStolenForeignMCV && homeCoords != CellStruct::Empty && scores[82] > 60)
		{
			if (CellClass* pHomeSafeLanding = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, true))
			{
				auto const pTransCell = pTrans->GetCell();

				const bool onHomeIslandLand = (homeGroundZone >= 0 && transZone == homeGroundZone && pTransCell
					&& pTransCell->LandType != LandType::Water);

				if (onHomeIslandLand && pTrans->DistanceFrom(pHomeSafeLanding) <= 1024)
				{
					if (pTrans->CurrentMission != Mission::Unload)
						pTrans->QueueMission(Mission::Unload, true);
				}
				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
					|| (Unsorted::CurrentFrame % 90 < 25))
				{
					pTrans->SetDestination(pHomeSafeLanding, true);

					pTrans->QueueMission(Mission::Move, true);
				}

				continue;
			}
		}

		// If we have a stolen foreign MCV stranded on an enemy island
		// (pStrandedStolenMCVOnIsland) and this transport is empty, dispatch it to
		// the island beach near the stolen MCV to pick it up!
		if (numPass == 0 && pStrandedStolenMCVOnIsland && scores[82] > 60)
		{
			const CellStruct mcvIslandCell = pStrandedStolenMCVOnIsland->GetMapCoords();

			const int mcvIslandZone
				= MapClass::Instance.GetMovementZoneType(mcvIslandCell, MovementZone::Normal, false);

			if (CellClass* pRescueBeach = FindSafeIslandLandingCell(pHouse, mcvIslandCell, mcvIslandZone, false))
			{
				if (pTrans->DistanceFrom(pRescueBeach) > 1024
					&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
						|| (Unsorted::CurrentFrame % 90 < 25)))
				{
					pTrans->SetDestination(pRescueBeach, true);

					pTrans->QueueMission(Mission::Move, true);
				}

				continue;
			}
		}

		bool carryingEngineer = false;
		bool carryingHarvester = false;
		CellClass* pCustomPassengerGoal = nullptr;
		int customPassengerZone = -1;
		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr;
			pPass = abstract_cast<FootClass*>(pPass->NextObject))
		{
			if (!pCustomPassengerGoal)
			{
				const CellStruct passGoalCoords = LookupPassengerIslandGoal(pPass->UniqueID);
				if (passGoalCoords != CellStruct::Empty)
				{
					const int pgz = MapClass::Instance.GetMovementZoneType(passGoalCoords, MovementZone::Normal, false);
					if (pgz >= 0)
					{
						if (CellClass* pSafePassLanding = FindSafeIslandLandingCell(pHouse, passGoalCoords, pgz, false))
						{
							pCustomPassengerGoal = pSafePassLanding;
							customPassengerZone = pgz;
						}
					}
				}
			}

			if (auto const pPassInf = abstract_cast<InfantryClass*>(pPass))
			{
				if (pPassInf->Type && pPassInf->Type->Engineer)
					carryingEngineer = true;
			}
			else if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
			{
				if (pPassUnit->Type && (pPassUnit->Type->Harvester || pPassUnit->Type->Weeder))
					carryingHarvester = true;
			}
		}

		CellClass* pActiveIslandGoal = pCustomPassengerGoal ? pCustomPassengerGoal : pIslandGoalCell;
		int activeIslandZone = pCustomPassengerGoal ? customPassengerZone : targetIslandZone;
		// Parallel Amphibious Task Groups: when multiple combat transports are
		// ready and a secondary enemy island/flank exists, assign odd-numbered
		// transports to strike the secondary objective in parallel!
		if (!pCustomPassengerGoal && !carryingMCV && !carryingHarvester && !carryingEngineer && pSecondaryIslandGoalCell
			&& secondaryIslandZone >= 0 && readyCombatConvoyTransports >= 2 && (pTrans->UniqueID % 2 == 1))
		{
			pActiveIslandGoal = pSecondaryIslandGoalCell;
			activeIslandZone = secondaryIslandZone;
		}

		if (!pCustomPassengerGoal && (carryingMCV || carryingHarvester) && pIslandResourceGoalCell)
		{
			pActiveIslandGoal = pIslandResourceGoalCell;
			activeIslandZone = resourceIslandZone;
		}
		else if (carryingEngineer && pIslandTechGoalCell)
		{
			pActiveIslandGoal = pIslandTechGoalCell;
			activeIslandZone = techIslandZone;
		}
		else if (!pActiveIslandGoal)
		{
			if (pIslandResourceGoalCell)
			{
				pActiveIslandGoal = pIslandResourceGoalCell;
				activeIslandZone = resourceIslandZone;
			}
			else if (pIslandTechGoalCell)
			{
				pActiveIslandGoal = pIslandTechGoalCell;
				activeIslandZone = techIslandZone;
			}
		}

		// Fast Continuous Amphibious Squadron Dispatch:
		// When we already have military dominance (OwnedUnits >= or >= 2
		// transports are ready, dispatch immediately instead of holding transports
		// back waiting for a slow 4-5 ship convoy!
		const int desiredConvoyTransports = (homeTransportsStillLoading > 0)
			? std::clamp(readyCombatConvoyTransports + homeTransportsStillLoading, 2, 6)
			: ((pHouse->OwnedUnits >= 15) ? 1 : 2);
		const bool isSoloPriorityCargo = carryingMCV || carryingHarvester || carryingStolenForeignMCV
			|| (carryingEngineer && pIslandTechGoalCell != nullptr);
		const bool waitForConvoySister = (!isSoloPriorityCargo && homeTransportsStillLoading > 0
			&& readyCombatConvoyTransports < desiredConvoyTransports
			&& (activeIslandZone < 0 || transZone != activeIslandZone) && !pTrans->HasBeenAttacked
			&& (Unsorted::CurrentFrame % 210 < 120));
		// If this transport already has passengers (numPass >= and is
		// intercepted/under attack during boarding, depart immediately with what we
		// have!
		const bool emergencyDepartUnderFire
			= (numPass >= 1 && (pTrans->HasBeenAttacked || pTrans->GetHealthPercentage() < 0.70));
		// CRITICAL: Once a transport with numPass >= 1 has left our home pickup
		// island (or is already on activeIslandZone / in Mission::Unload / in water
		// not waiting for convoy), it MUST finish unloading ALL remaining
		// passengers (even when numPass drops from 3 to instead of aborting back
		// to water with 1 unit still inside!
		auto hasEnemyPresenceInIslandZone = [&](int checkZone) -> bool
		{
			if (checkZone < 0)
				return false;

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
					|| pEB->Type->InvisibleInGame || pEB->Type->Naval)
				{
					continue;
				}

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
					|| pEB->Owner->IsObserver() || pEB->Owner->Defeated)
				{
					continue;
				}

				if (MapClass::Instance.GetMovementZoneType(pEB->GetMapCoords(), MovementZone::Normal, false)
					== checkZone)
				{
					return true;
				}
			}

			for (auto const pEU : UnitClass::Array)
			{
				if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Naval)
					continue;

				if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral()
					|| pEU->Owner->IsObserver() || pEU->Owner->Defeated)
				{
					continue;
				}

				if (MapClass::Instance.GetMovementZoneType(pEU->GetMapCoords(), MovementZone::Normal, false)
					== checkZone)
				{
					return true;
				}
			}

			return false;
		};

		auto isSafeFriendlyPickupIsland = [&](int checkZone) -> bool
		{
			if (checkZone < 0)
				return false;

			if ((activeIslandZone >= 0 && checkZone == activeIslandZone)
				|| (targetIslandZone >= 0 && checkZone == targetIslandZone && checkZone != homeGroundZone)
				|| (secondaryIslandZone >= 0 && checkZone == secondaryIslandZone && checkZone != homeGroundZone))
			{
				return false;
			}

			if (checkZone == homeGroundZone)
				return true;

			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
					continue;

				if (pOwnB->Type->ConstructionYard
					|| (pOwnB->Type->Factory != AbstractType::None && !pOwnB->Type->Naval))
				{
					if (MapClass::Instance.GetMovementZoneType(pOwnB->GetMapCoords(), MovementZone::Normal, false)
						== checkZone)
					{
						return true;
					}
				}
			}

			// Also allow picking up stranded friendly units from cleared/neutral islands
			// that have no enemy presence so they can rejoin the next offensive wave
			return !hasEnemyPresenceInIslandZone(checkZone);
		};

		auto hasMoreBoardableUnitsOnIsland = [&](int checkZone) -> bool
		{
			if (checkZone < 0 || !isSafeFriendlyPickupIsland(checkZone))
				return false;

			const int freeCap = pTrans->Type->Passengers - totalPassSize;

			if (freeCap <= 0 || numPass >= pTrans->Type->Passengers)
				return false;

			for (auto const pV : idleCombatUnits)
			{
				if (!pV || pV->InLimbo || !pV->IsAlive || IsAmphibiousOrAirTransport(pV->Type))
					continue;

				const int vz = MapClass::Instance.GetMovementZoneType(pV->GetMapCoords(), MovementZone::Normal, false);

				if (vz != checkZone)
					continue;

				const int vsz = std::max(1, static_cast<int>(pV->Type->TechnoTypeClass::Size));

				if (vsz <= static_cast<int>(pTrans->Type->SizeLimit) && vsz <= freeCap)
					return true;
			}

			for (auto const pI : idleCombatInf)
			{
				if (!pI || pI->InLimbo || !pI->IsAlive)
					continue;

				const int iz = MapClass::Instance.GetMovementZoneType(pI->GetMapCoords(), MovementZone::Normal, false);

				if (iz != checkZone)
					continue;

				const int isz = std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size));

				if (isz <= static_cast<int>(pTrans->Type->SizeLimit) && isz <= freeCap)
					return true;
			}

			return false;
		};

		auto const pCurTransCell = pTrans->GetCell();
		const bool onSafePickupShore
			= isSafeFriendlyPickupIsland(transZone) && pCurTransCell && pCurTransCell->LandType != LandType::Water;
		const bool activelyBoardingOnPickupIsland = (numPass < pTrans->Type->Passengers) && onSafePickupShore
			&& (HasActiveBoardingPassenger(pTrans, transZone)
				|| (transZone != homeGroundZone && hasMoreBoardableUnitsOnIsland(transZone)));
		const bool finishedPickupOnNonHomeIsland
			= (numPass >= 1) && onSafePickupShore && transZone != homeGroundZone && !activelyBoardingOnPickupIsland;
		const bool hasCargoInTransitOrUnloading = (numPass >= 1) && !activelyBoardingOnPickupIsland
			&& ((activeIslandZone >= 0 && transZone == activeIslandZone) || pTrans->CurrentMission == Mission::Unload
				|| finishedPickupOnNonHomeIsland
				|| (transZone >= 0 && transZone != homeGroundZone && !isSafeFriendlyPickupIsland(transZone))
				|| (pCurTransCell && pCurTransCell->LandType == LandType::Water && !waitForConvoySister
					&& (homeCoords == CellStruct::Empty || transCoords.DistanceFrom(homeCoords) > 16.0
						|| !HasActiveBoardingPassenger(pTrans, homeGroundZone))));
		const bool isReadyToSailEffective = isReadyToSail || emergencyDepartUnderFire || hasCargoInTransitOrUnloading
			|| carryingHarvester || (carryingEngineer && pIslandTechGoalCell != nullptr)
			|| (pCustomPassengerGoal != nullptr);
		if (isReadyToSailEffective && pActiveIslandGoal && waitForConvoySister && !hasCargoInTransitOrUnloading)
		{
			// Offshore Convoy Staging (supports 4-5 transport armadas!): If this
			// transport is already full and waiting for sister transport(s) to finish
			// boarding, pull 3..8 cells out into the water with a lateral spread so
			// we free the beach for all 4-5 transports!
			auto const pTransCell = pTrans->GetCell();
			if (pTransCell && pTransCell->LandType != LandType::Water)
			{
				const CellStruct goalC = pActiveIslandGoal->MapCoords;
				const int dxG = goalC.X - transCoords.X;
				const int dyG = goalC.Y - transCoords.Y;
				const int distG = std::max(1, std::max(std::abs(dxG), std::abs(dyG)));
				const int latOffset = (static_cast<int>(pTrans->UniqueID % 5) - 2) * 2;
				const int perpX = (-dyG * latOffset) / distG;
				const int perpY = (dxG * latOffset) / distG;
				CellClass* pOffshoreStage = nullptr;
				for (int step = 3; step <= 8 && !pOffshoreStage; ++step)
				{
					const CellStruct sc { static_cast<short>(transCoords.X + (dxG * step) / distG + perpX),
						static_cast<short>(transCoords.Y + (dyG * step) / distG + perpY) };
					if (auto const pSC = MapClass::Instance.TryGetCellAt(sc))
					{
						if (pSC->LandType == LandType::Water
							&& pSC->IsClearToMove(
								pTrans->Type->SpeedType, true, true, -1, pTrans->Type->MovementZone, -1, false))
						{
							pOffshoreStage = pSC;
						}
					}
				}

				if (pOffshoreStage && (pTrans->CurrentMission != Mission::Move || (Unsorted::CurrentFrame % 60 < 20)))
				{
					pTrans->SetDestination(pOffshoreStage, true);
					pTrans->QueueMission(Mission::Move, true);
				}
			}
			else if (pTrans->CurrentMission == Mission::Move)
			{
				pTrans->SetDestination(nullptr, true);
				pTrans->ClearNavigationList();
				pTrans->ForceMission(Mission::Guard);
			}
		}
		else if (isReadyToSailEffective && pActiveIslandGoal)
		{
			CellClass* pSafeLanding = FindSafeIslandLandingCell(
				pHouse, pActiveIslandGoal->MapCoords, activeIslandZone, carryingMCV || carryingHarvester);
			if (pSafeLanding)
			{
				auto const pTransCell = pTrans->GetCell();
				const double distToLanding = transCoords.DistanceFrom(pSafeLanding->MapCoords);
				const bool onTargetIslandLand = (activeIslandZone >= 0 && transZone == activeIslandZone && pTransCell
					&& pTransCell->LandType != LandType::Water && !pTransCell->GetBuilding());

				// Offshore Rendezvous before hitting the enemy beach:
				bool pauseOffshoreForTrailingSister = false;
				if (!isSoloPriorityCargo && !hasCargoInTransitOrUnloading && pTransCell
					&& pTransCell->LandType == LandType::Water && distToLanding >= 6.5 && distToLanding <= 15.0
					&& !pTrans->HasBeenAttacked && pTrans->GetHealthPercentage() >= 0.90
					&& (Unsorted::CurrentFrame % 420 < 310))
				{
					for (auto const pSisterT : UnitClass::Array)
					{
						if (!pSisterT || pSisterT == pTrans || pSisterT->Owner != pHouse || pSisterT->InLimbo
							|| !pSisterT->IsAlive || !pSisterT->Type || !IsAmphibiousOrAirTransport(pSisterT->Type))
						{
							continue;
						}

						if (pSisterT->HasBeenAttacked || pSisterT->GetHealthPercentage() < 0.75
							|| pSisterT->CurrentMission != Mission::Move)
						{
							continue;
						}

						if (pSisterT->Passengers.NumPassengers < 2 && pSisterT->Passengers.GetTotalSize() < 5)
							continue;

						const double sisterDist = pSisterT->GetMapCoords().DistanceFrom(pSafeLanding->MapCoords);
						if (sisterDist > distToLanding + 4.0 && sisterDist <= distToLanding + 28.0)
						{
							pauseOffshoreForTrailingSister = true;
							break;
						}
					}
				}

				if (onTargetIslandLand && distToLanding <= 14.5)
				{
					// Scatter any idle friendly units standing on the 8 adjacent cells
					// around pTrans so they never block the unload ramp, and count how
					// many adjacent dry land cells are actually empty right now!
					int emptyAdjUnloadCells = 0;
					for (int ndy = -1; ndy <= 1; ++ndy)
					{
						for (int ndx = -1; ndx <= 1; ++ndx)
						{
							if (ndx == 0 && ndy == 0)
								continue;

							const CellStruct adj { static_cast<short>(transCoords.X + ndx),
								static_cast<short>(transCoords.Y + ndy) };
							auto const pAdj = MapClass::Instance.TryGetCellAt(adj);
							if (!pAdj || pAdj->LandType == LandType::Water || pAdj->GetBuilding() != nullptr
								|| !pAdj->IsClearToMove(
									SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
							{
								continue;
							}

							if (pAdj->FirstObject == nullptr)
							{
								emptyAdjUnloadCells++;
							}
							else
							{
								for (ObjectClass* pObj = pAdj->FirstObject; pObj; pObj = pObj->NextObject)
								{
									if (auto const pFootAdj = abstract_cast<FootClass*>(pObj))
									{
										if (pFootAdj->Owner == pHouse && pFootAdj->IsAlive && !pFootAdj->InLimbo
											&& pFootAdj->CurrentMission != Mission::Move
											&& pFootAdj->CurrentMission != Mission::Attack)
										{
											pFootAdj->Scatter(CoordStruct::Empty, true, false);
										}
									}
								}
							}
						}
					}

					// If our current beach spot has < 2 empty exit cells (or if
					// Mission::Unload failed and reverted to Guard while congested),
					// relocate to a nearby unobstructed beach/land cell with >= 3 empty
					// dry neighbor cells so remaining passengers unload cleanly!
					if (emptyAdjUnloadCells < 2)
					{
						CellClass* pBetterUnloadCell = nullptr;
						int bestAltScore = -999999;
						for (int dy = -12; dy <= 12; ++dy)
						{
							for (int dx = -12; dx <= 12; ++dx)
							{
								if (std::abs(dx) <= 1 && std::abs(dy) <= 1)
									continue;

								const CellStruct altC { static_cast<short>(transCoords.X + dx),
									static_cast<short>(transCoords.Y + dy) };
								auto const pAlt = MapClass::Instance.TryGetCellAt(altC);
								if (!pAlt || pAlt->LandType == LandType::Water || pAlt->LandType == LandType::Rock
									|| pAlt->GetBuilding() || pAlt->FirstObject != nullptr)
								{
									continue;
								}

								if (MapClass::Instance.GetMovementZoneType(altC, MovementZone::Normal, false)
									!= activeIslandZone)
								{
									continue;
								}

								if (!pAlt->IsClearToMove(
										SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false))
								{
									continue;
								}

								int altEmptyNeighbors = 0;
								for (int ay = -1; ay <= 1; ++ay)
								{
									for (int ax = -1; ax <= 1; ++ax)
									{
										if (ax == 0 && ay == 0)
											continue;

										const CellStruct nC { static_cast<short>(altC.X + ax),
											static_cast<short>(altC.Y + ay) };
										auto const pN = MapClass::Instance.TryGetCellAt(nC);
										if (pN && pN->LandType != LandType::Water && !pN->GetBuilding()
											&& pN->FirstObject == nullptr
											&& pN->IsClearToMove(
												SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false))
										{
											altEmptyNeighbors++;
										}
									}
								}

								if (altEmptyNeighbors >= 3)
								{
									const int sc = altEmptyNeighbors * 80 - (std::abs(dx) + std::abs(dy)) * 10;
									if (sc > bestAltScore)
									{
										bestAltScore = sc;
										pBetterUnloadCell = pAlt;
									}
								}
							}
						}

						if (pBetterUnloadCell)
						{
							pTrans->SetDestination(pBetterUnloadCell, true);
							pTrans->QueueMission(Mission::Move, true);
							continue;
						}
					}

					pTrans->SetDestination(nullptr, true);
					pTrans->ClearNavigationList();
					if (pTrans->CurrentMission != Mission::Unload)
						pTrans->QueueMission(Mission::Unload, true);
				}
				else if (pauseOffshoreForTrailingSister)
				{
					if (pTrans->CurrentMission == Mission::Move)
					{
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->ForceMission(Mission::Guard);
					}
				}
				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
					|| (Unsorted::CurrentFrame % 60 < 20))
				{
					pTrans->SetDestination(pSafeLanding, true);
					pTrans->QueueMission(Mission::Move, true);
				}
			}
		}
		else if (!isReadyToSailEffective)
		{
			auto const pTransCell = pTrans->GetCell();
			const bool activelyLoadingLinkedUnit = HasActiveBoardingPassenger(pTrans, transZone);

			// Reserve at most ONE transport for pHomeExpansionDeployer, and if we
			// already own >= 2 ConYards while >= 5 combat vehicles are waiting to
			// invade an enemy island, let combat invasion waves take priority when no
			// second transport is free!
			const bool prioritizeCombatInvasionNow = (pHouse->NumConYards >= 2 && enemyOnDisconnectedIsland
				&& idleCombatUnits.size() >= 5 && (Unsorted::CurrentFrame % 240 < 150));
			const bool canCarryHomeDeployer = (!deployerTransportAssigned && !prioritizeCombatInvasionNow
				&& pHomeExpansionDeployer != nullptr && !carryingMCV
				&& static_cast<int>(pHomeExpansionDeployer->Type->TechnoTypeClass::Size)
					<= static_cast<int>(pTrans->Type->SizeLimit));
			const int reservedSizeForDeployer
				= canCarryHomeDeployer ? static_cast<int>(pHomeExpansionDeployer->Type->TechnoTypeClass::Size) : 0;

			if (canCarryHomeDeployer && numPass > 0
				&& (totalPassSize + reservedSizeForDeployer) > pTrans->Type->Passengers && pTransCell
				&& pTransCell->LandType != LandType::Water)
			{
				deployerTransportAssigned = true;
				pTrans->SetDestination(nullptr, true);
				pTrans->ClearNavigationList();
				if (pTrans->CurrentMission != Mission::Unload)
					pTrans->QueueMission(Mission::Unload, true);

				continue;
			}

			if (canCarryHomeDeployer && (totalPassSize + reservedSizeForDeployer) <= pTrans->Type->Passengers)
			{
				deployerTransportAssigned = true;
				const int deployerZone = MapClass::Instance.GetMovementZoneType(
					pHomeExpansionDeployer->GetMapCoords(), MovementZone::Normal, false);
				CellClass* pDeployerPickupBeach
					= FindSafeIslandLandingCell(pHouse, pHomeExpansionDeployer->GetMapCoords(), deployerZone, false);
				const bool onDeployerLand
					= (transZone == deployerZone && pTransCell && pTransCell->LandType != LandType::Water);
				if (!onDeployerLand && pDeployerPickupBeach)
				{
					if (!activelyLoadingLinkedUnit
						&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
							|| (Unsorted::CurrentFrame % 60 < 20)))
					{
						pTrans->SetDestination(pDeployerPickupBeach, true);
						pTrans->QueueMission(Mission::Move, true);
					}

					continue;
				}

				if (onDeployerLand)
				{
					if (!activelyLoadingLinkedUnit
						&& (pTrans->CurrentMission == Mission::Move || pTrans->Destination != nullptr))
					{
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->ForceMission(Mission::Guard);
					}

					if ((pHomeExpansionDeployer->CurrentMission != Mission::Enter
							&& pHomeExpansionDeployer->QueuedMission != Mission::Enter)
						|| pHomeExpansionDeployer->ContainsLink(pTrans)
						|| pHomeExpansionDeployer->ArchiveTarget == pTrans
						|| pHomeExpansionDeployer->Destination == pTrans)
					{
						OrderFootToEnter(pHomeExpansionDeployer, pTrans);
					}

					continue;
				}
			}
			else if (pHomeIslandEngineer != nullptr && !carryingEngineer
				&& CanPassengerFitInTransport(pTrans, pHomeIslandEngineer->Type))
			{
				const int engZone = MapClass::Instance.GetMovementZoneType(
					pHomeIslandEngineer->GetMapCoords(), MovementZone::Normal, false);
				CellClass* pEngPickupBeach
					= FindSafeIslandLandingCell(pHouse, pHomeIslandEngineer->GetMapCoords(), engZone, false);
				const bool onEngLand = (transZone == engZone && pTransCell && pTransCell->LandType != LandType::Water);
				if (!onEngLand && pEngPickupBeach)
				{
					if (!activelyLoadingLinkedUnit
						&& (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
							|| (Unsorted::CurrentFrame % 60 < 20)))
					{
						pTrans->SetDestination(pEngPickupBeach, true);
						pTrans->QueueMission(Mission::Move, true);
					}

					continue;
				}

				if (onEngLand)
				{
					if (pTrans->CurrentMission == Mission::Move
						|| (pTrans->Destination != nullptr && !activelyLoadingLinkedUnit))
					{
						pTrans->SetDestination(nullptr, true);
						pTrans->ClearNavigationList();
						pTrans->ForceMission(Mission::Guard);
					}

					OrderFootToEnter(pHomeIslandEngineer, pTrans);
				}
			}
			else if (!pendingCrossIslandOrders.empty())
			{
				int pendCntOnThisTrans = 0;
				int pendSzOnThisTrans = 0;
				countPendingBoardersOn(pTrans, nullptr, pendCntOnThisTrans, pendSzOnThisTrans);

				bool handledPendingOrder = false;
				for (auto const& ord : pendingCrossIslandOrders)
				{
					if (!ord.Passenger || !ord.Passenger->IsAlive || ord.Passenger->InLimbo)
						continue;

					if ((ord.Passenger->CurrentMission == Mission::Enter
							|| ord.Passenger->QueuedMission == Mission::Enter)
						&& !ord.Passenger->ContainsLink(pTrans) && ord.Passenger->ArchiveTarget != pTrans
						&& ord.Passenger->Destination != pTrans)
					{
						continue; // Already boarding another transport of the armada!
					}

					if (!CanPassengerFitInTransport(pTrans, ord.Passenger->GetTechnoType(), pendCntOnThisTrans,
							pendSzOnThisTrans, reservedSizeForDeployer))
					{
						continue;
					}

					CellClass* pOrdPickupBeach
						= FindSafeIslandLandingCell(pHouse, ord.Passenger->GetMapCoords(), ord.PickupZone, false);
					const bool onOrdLand
						= (transZone == ord.PickupZone && pTransCell && pTransCell->LandType != LandType::Water);
					if (!onOrdLand && pOrdPickupBeach)
					{
						if (!activelyLoadingLinkedUnit
							&& (pTrans->CurrentMission == Mission::Guard
								|| pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 60 < 20)))
						{
							pTrans->SetDestination(pOrdPickupBeach, true);
							pTrans->QueueMission(Mission::Move, true);
						}

						handledPendingOrder = true;
						break;
					}

					if (onOrdLand)
					{
						if (pTrans->CurrentMission == Mission::Move
							|| (pTrans->Destination != nullptr && !activelyLoadingLinkedUnit))
						{
							pTrans->SetDestination(nullptr, true);
							pTrans->ClearNavigationList();
							pTrans->ForceMission(Mission::Guard);
						}

						if (ord.Passenger->CurrentMission != Mission::Enter
							&& ord.Passenger->QueuedMission != Mission::Enter)
						{
							OrderFootToEnter(ord.Passenger, pTrans);
							pendCntOnThisTrans++;
							pendSzOnThisTrans += ord.PassengerSize;
						}

						handledPendingOrder = true;
					}
				}

				if (handledPendingOrder
					&& (pTrans->Passengers.NumPassengers + pendCntOnThisTrans) >= pTrans->Type->Passengers)
				{
					continue;
				}
			}

			// Safe Friendly Base & Stranded Cleared-Island Pickup + Empty Transport Retreat:
			int desiredPickupZone = -1;
			CellStruct desiredPickupAnchor = homeCoords;
			{
				struct IslandArmyCluster
				{
					int zone;
					int score;
					int minDistToTrans;
					CellStruct closestUnitCoords;
				};
				std::vector<IslandArmyCluster> clusters;
				auto addCandidateToCluster = [&](FootClass* pCand, int candSize, int weight)
				{
					if (!pCand || pCand->InLimbo || !pCand->IsAlive)
						return;

					if (candSize > static_cast<int>(pTrans->Type->SizeLimit))
						return;

					if ((pCand->CurrentMission == Mission::Enter || pCand->QueuedMission == Mission::Enter)
						&& !pCand->ContainsLink(pTrans) && pCand->ArchiveTarget != pTrans
						&& pCand->Destination != pTrans)
					{
						return; // Already boarding a different transport
					}

					const CellStruct cc = pCand->GetMapCoords();
					const int cz = MapClass::Instance.GetMovementZoneType(cc, MovementZone::Normal, false);
					if (!isSafeFriendlyPickupIsland(cz))
						return;

					const int d = std::abs(cc.X - transCoords.X) + std::abs(cc.Y - transCoords.Y);
					for (auto& cl : clusters)
					{
						if (cl.zone == cz)
						{
							cl.score += weight;
							if (d < cl.minDistToTrans)
							{
								cl.minDistToTrans = d;
								cl.closestUnitCoords = cc;
							}

							return;
						}
					}

					clusters.push_back({ cz, weight, d, cc });
				};

				for (auto const pV : idleCombatUnits)
				{
					if (!IsAmphibiousOrAirTransport(pV->Type))
						addCandidateToCluster(pV, static_cast<int>(pV->Type->TechnoTypeClass::Size), 35);
				}

				for (auto const pI : idleCombatInf)
				{
					addCandidateToCluster(pI, std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size)), 12);
				}

				std::vector<IslandArmyCluster> readyIslands;
				for (const auto& cl : clusters)
				{
					if (cl.zone == transZone)
					{
						desiredPickupZone = cl.zone;
						desiredPickupAnchor = cl.closestUnitCoords;
						break;
					}

					if (cl.score >= 35 || (cl.zone != homeGroundZone && cl.score >= 24))
						readyIslands.push_back(cl);
				}

				if (desiredPickupZone < 0 && !readyIslands.empty())
				{
					const auto& chosenCl = readyIslands[pTrans->UniqueID % readyIslands.size()];
					desiredPickupZone = chosenCl.zone;
					desiredPickupAnchor = chosenCl.closestUnitCoords;
				}

				if (desiredPickupZone < 0)
				{
					int bestClusterVal = -999999;
					for (const auto& cl : clusters)
					{
						int val = cl.score - (cl.minDistToTrans * 2);
						if (val > bestClusterVal)
						{
							bestClusterVal = val;
							desiredPickupZone = cl.zone;
							desiredPickupAnchor = cl.closestUnitCoords;
						}
					}
				}

				if (desiredPickupZone < 0)
				{
					desiredPickupZone = homeGroundZone;
					desiredPickupAnchor = homeCoords;
					// Prefer anchoring near our Naval Yard or War Factory on
					// homeGroundZone so returning empty transports retreat right to our
					// safe coastal shipyard/base perimeter!
					for (auto const pOwnB : pHouse->Buildings)
					{
						if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
							continue;

						if (pOwnB->Type->Naval && pOwnB->Type->Factory == AbstractType::UnitType)
						{
							desiredPickupAnchor = pOwnB->GetMapCoords();
							break;
						}
					}
				}
			}

			const bool notAtPickupShore = (pTransCell && pTransCell->LandType == LandType::Water)
				|| (desiredPickupZone >= 0 && transZone != desiredPickupZone)
				|| (!isSafeFriendlyPickupIsland(transZone));
			if (notAtPickupShore && desiredPickupAnchor != CellStruct::Empty)
			{
				if (activelyLoadingLinkedUnit && isSafeFriendlyPickupIsland(transZone))
				{
					continue; // Let native MoveTo_Beach_Near (vtable+0x480) finish
							// beaching & loading the linked unit!
				}

				CellClass* pPickupBeach
					= FindSafeIslandLandingCell(pHouse, desiredPickupAnchor, desiredPickupZone, false);
				if (!pPickupBeach && homeCoords != CellStruct::Empty)
					pPickupBeach = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false);

				if (pPickupBeach)
				{
					if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard
						|| pTrans->CurrentMission == Mission::Unload || (Unsorted::CurrentFrame % 60 < 20))
					{
						pTrans->SetDestination(pPickupBeach, true);
						pTrans->QueueMission(Mission::Move, true);
					}

					// If the transport is approaching the pickup beach (within 14 cells),
					// march up to 6 inland units on desiredPickupZone toward the beach!
					if (transCoords.DistanceFrom(pPickupBeach->MapCoords) <= 14.0)
					{
						int ralliedToBeach = 0;
						for (auto const pV : idleCombatUnits)
						{
							if (ralliedToBeach >= 6)
								break;

							if (IsAmphibiousOrAirTransport(pV->Type)
								|| pV->Type->TechnoTypeClass::Size > pTrans->Type->SizeLimit)
							{
								continue;
							}

							if (pV->CurrentMission == Mission::Enter || pV->QueuedMission == Mission::Enter)
								continue;

							const int uz = MapClass::Instance.GetMovementZoneType(
								pV->GetMapCoords(), MovementZone::Normal, false);
							if (IsFootActivelyAttackingOrMovingToEnemy(pV) || IsFootNearEnemyFrontline(pV, uz))
								continue;

							if (uz == desiredPickupZone
								&& pV->GetMapCoords().DistanceFrom(pPickupBeach->MapCoords) > 6.0)
							{
								pV->SetDestination(pPickupBeach, true);
								pV->QueueMission(Mission::Move, true);
								ralliedToBeach++;
							}
						}
					}
				}

				continue;
			}

			if ((pTrans->Type->MovementZone == MovementZone::Fly || pTrans->Type->JumpJet) && pTrans->GetHeight() > 0
				&& pTrans->Destination == nullptr && pTransCell && pTransCell->LandType != LandType::Water
				&& !pTransCell->GetBuilding())
			{
				if (pTrans->CurrentMission != Mission::Unload)
					pTrans->QueueMission(Mission::Unload, true);
			}

			// CRITICAL (UnitClass::Receive_Message @ 0x00737806):
			// A transport in Mission::Move (2) with Destination != nullptr or
			// Locomotor->Is_Moving() rejects RequestLoading with AnswerLeave (0x0A)!
			// Put the beached transport into Mission::Guard before ordering units to
			// board it (unless a linked unit is already actively boarding)!
			if (!activelyLoadingLinkedUnit
				&& (pTrans->CurrentMission == Mission::Move || pTrans->Destination != nullptr))
			{
				pTrans->SetDestination(nullptr, true);
				pTrans->ClearNavigationList();
				pTrans->ForceMission(Mission::Guard);
			}

			if (!pActiveIslandGoal && !enemyOnDisconnectedIsland && pIslandResourceGoalCell == nullptr
				&& pIslandTechGoalCell == nullptr && pendingCrossIslandOrders.empty())
			{
				continue;
			}

			int boardedNow = numPass;
			int sizeNow = totalPassSize;
			const int maxAllowedPassSize = std::max(0, pTrans->Type->Passengers - reservedSizeForDeployer);

			// Count friendly units/infantry on transZone already en route to board
			// pTrans so we don't queue the entire island onto 1 transport!
			for (auto const pCheckU : UnitClass::Array)
			{
				if (!pCheckU || pCheckU->Owner != pHouse || pCheckU->InLimbo || !pCheckU->IsAlive || !pCheckU->Type
					|| pCheckU == pTrans)
				{
					continue;
				}

				if ((pCheckU->CurrentMission == Mission::Enter || pCheckU->QueuedMission == Mission::Enter)
					&& (pCheckU->ContainsLink(pTrans) || pCheckU->ArchiveTarget == pTrans
						|| pCheckU->Destination == pTrans))
				{
					boardedNow++;
					sizeNow += static_cast<int>(pCheckU->Type->TechnoTypeClass::Size);
				}
			}

			for (auto const pCheckI : InfantryClass::Array)
			{
				if (!pCheckI || pCheckI->Owner != pHouse || pCheckI->InLimbo || !pCheckI->IsAlive || !pCheckI->Type)
					continue;

				if ((pCheckI->CurrentMission == Mission::Enter || pCheckI->QueuedMission == Mission::Enter)
					&& (pCheckI->ContainsLink(pTrans) || pCheckI->ArchiveTarget == pTrans
						|| pCheckI->Destination == pTrans))
				{
					boardedNow++;
					sizeNow += std::max(1, static_cast<int>(pCheckI->Type->TechnoTypeClass::Size));
				}
			}

			// Recruit the closest available vehicles on the same safe pickup island
			// (uz == transZone) at any distance on the island!
			while (boardedNow < pTrans->Type->Passengers && sizeNow < maxAllowedPassSize)
			{
				UnitClass* pBestV = nullptr;
				double bestDist = 999999.0;
				for (auto const pV : idleCombatUnits)
				{
					if (!pV || pV->InLimbo || !pV->IsAlive || !pV->Type || IsAmphibiousOrAirTransport(pV->Type))
						continue;

					if (pV->CurrentMission == Mission::Enter || pV->QueuedMission == Mission::Enter)
						continue;

					if (pV->CaptureManager && pV->CaptureManager->NumControlNodes() > 0)
						continue;

					const int vehSize = static_cast<int>(pV->Type->TechnoTypeClass::Size);
					if (vehSize > static_cast<int>(pTrans->Type->SizeLimit) || (sizeNow + vehSize) > maxAllowedPassSize)
						continue;

					const int uz
						= MapClass::Instance.GetMovementZoneType(pV->GetMapCoords(), MovementZone::Normal, false);
					if (transZone >= 0 && uz != transZone)
						continue;

					if (IsFootActivelyAttackingOrMovingToEnemy(pV) || IsFootNearEnemyFrontline(pV, uz))
						continue;

					const double d = pV->GetMapCoords().DistanceFrom(transCoords);
					if (d < bestDist)
					{
						bestDist = d;
						pBestV = pV;
					}
				}

				if (!pBestV)
					break;

				if (pActiveIslandGoal)
					RecordPassengerIslandGoal(pBestV->UniqueID, pActiveIslandGoal->MapCoords);

				OrderFootToEnter(pBestV, pTrans);
				boardedNow++;
				sizeNow += static_cast<int>(pBestV->Type->TechnoTypeClass::Size);
			}

			// Recruit the closest available infantry on the same safe pickup island
			// (iz == transZone) respecting SizeLimit & Size
			while (boardedNow < pTrans->Type->Passengers && sizeNow < maxAllowedPassSize
				&& static_cast<int>(pTrans->Type->SizeLimit) >= 1)
			{
				InfantryClass* pBestI = nullptr;
				double bestDist = 999999.0;
				for (auto const pI : idleCombatInf)
				{
					if (!pI || pI->InLimbo || !pI->IsAlive || !pI->Type)
						continue;

					if (pI->CurrentMission == Mission::Enter || pI->QueuedMission == Mission::Enter)
						continue;

					const int infSize = std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size));
					if (infSize > static_cast<int>(pTrans->Type->SizeLimit) || (sizeNow + infSize) > maxAllowedPassSize)
						continue;

					const int iz
						= MapClass::Instance.GetMovementZoneType(pI->GetMapCoords(), MovementZone::Normal, false);
					if (transZone >= 0 && iz != transZone)
						continue;

					if (IsFootActivelyAttackingOrMovingToEnemy(pI) || IsFootNearEnemyFrontline(pI, iz))
						continue;

					const double d = pI->GetMapCoords().DistanceFrom(transCoords);
					if (d < bestDist)
					{
						bestDist = d;
						pBestI = pI;
					}
				}

				if (!pBestI)
					break;

				if (pActiveIslandGoal)
					RecordPassengerIslandGoal(pBestI->UniqueID, pActiveIslandGoal->MapCoords);

				OrderFootToEnter(pBestI, pTrans);
				boardedNow++;
				sizeNow += std::max(1, static_cast<int>(pBestI->Type->TechnoTypeClass::Size));
			}
		}
	}

	// Dispatch loaded/idle combat Aircraft, Carryall «Matryoshka» Airlift &
	// Emergency Sell-Dodge / ConYard Undeploy
	for (auto const pAir : AircraftClass::Array)
	{
		if (!pAir || pAir->Owner != pHouse || pAir->InLimbo || !pAir->IsAlive || !pAir->Type)
			continue;

		const bool isAirIdle = (pAir->CurrentMission == Mission::Guard || pAir->CurrentMission == Mission::Area_Guard
			|| pAir->CurrentMission == Mission::Sleep);

		// Carryall «Matryoshka / Muñeca Rusa» Airlift (Slot 53): Pick up loaded
		// APCs, Expansion Nanocores/MCVs, or rescue critically damaged tanks!
		const int carryallWindow = std::clamp(20 + (static_cast<int>(scores[53]) / 18), 8, 45);

		if (!pAir->Type->Carryall && pAir->Type->Passengers > 0)
		{
			const int numAirPass = pAir->Passengers.NumPassengers;
			const int totalAirPassSize = pAir->Passengers.GetTotalSize();
			const CellStruct airCoords = pAir->GetMapCoords();
			const int airZone = MapClass::Instance.GetMovementZoneType(airCoords, MovementZone::Normal, false);
			bool airCarryingMCV = false;
			bool airCarryingHarvester = false;
			bool airCarryingEngineer = false;
			CellClass* pAirCustomPassGoal = nullptr;
			int airCustomPassZone = -1;
			for (FootClass* pPass = pAir->Passengers.GetFirstPassenger(); pPass != nullptr;
				pPass = abstract_cast<FootClass*>(pPass->NextObject))
			{
				if (!pAirCustomPassGoal)
				{
					const CellStruct passGoalCoords = LookupPassengerIslandGoal(pPass->UniqueID);
					if (passGoalCoords != CellStruct::Empty)
					{
						const int pgz
							= MapClass::Instance.GetMovementZoneType(passGoalCoords, MovementZone::Normal, false);
						if (pgz >= 0)
						{
							if (CellClass* pSafePassLanding
								= FindSafeIslandLandingCell(pHouse, passGoalCoords, pgz, false))
							{
								pAirCustomPassGoal = pSafePassLanding;
								airCustomPassZone = pgz;
							}
						}
					}
				}

				if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))
				{
					if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr)
						airCarryingMCV = true;
					else if (pPassUnit->Type && (pPassUnit->Type->Harvester || pPassUnit->Type->Weeder))
						airCarryingHarvester = true;
				}
				else if (auto const pPassInf = abstract_cast<InfantryClass*>(pPass))
				{
					if (pPassInf->Type && pPassInf->Type->Engineer)
						airCarryingEngineer = true;
				}
			}

			const bool airReadyToFly = airCarryingMCV || airCarryingHarvester
				|| (airCarryingEngineer && pIslandTechGoalCell != nullptr) || (pAirCustomPassGoal != nullptr)
				|| numAirPass >= 3 || totalAirPassSize >= 6 || numAirPass >= pAir->Type->Passengers;
			if (airReadyToFly)
			{
				CellClass* pAirGoal = pAirCustomPassGoal ? pAirCustomPassGoal : pIslandGoalCell;
				int airGoalZone = pAirCustomPassGoal ? airCustomPassZone : targetIslandZone;
				if ((airCarryingMCV || airCarryingHarvester) && pIslandResourceGoalCell)
				{
					pAirGoal = pIslandResourceGoalCell;
					airGoalZone = resourceIslandZone;
				}
				else if (airCarryingEngineer && pIslandTechGoalCell)
				{
					pAirGoal = pIslandTechGoalCell;
					airGoalZone = techIslandZone;
				}
				else if (!pAirGoal)
				{
					pAirGoal = pIslandResourceGoalCell ? pIslandResourceGoalCell
													: (pIslandTechGoalCell ? pIslandTechGoalCell : pWaveTargetCell);
					airGoalZone
						= pIslandResourceGoalCell ? resourceIslandZone : (pIslandTechGoalCell ? techIslandZone : -1);
				}

				if (pAirGoal)
				{
					CellClass* pSafeAirDrop = FindSafeIslandLandingCell(
						pHouse, pAirGoal->MapCoords, airGoalZone, airCarryingMCV || airCarryingHarvester);
					if (pSafeAirDrop)
					{
						if (airCoords.DistanceFrom(pSafeAirDrop->MapCoords) <= 4.0
							&& pSafeAirDrop->LandType != LandType::Water)
						{
							if (pAir->CurrentMission != Mission::Unload)
							{
								pAir->SetDestination(pSafeAirDrop, true);
								pAir->QueueMission(Mission::Unload, true);
							}
						}
						else if (isAirIdle || (Unsorted::CurrentFrame % 90 < 25))
						{
							pAir->SetDestination(pSafeAirDrop, true);
							pAir->QueueMission(Mission::Move, true);
						}
					}
				}
			}
			else if (numAirPass == 0 && primaryArmyZone >= 0 && airZone != primaryArmyZone
				&& homeCoords != CellStruct::Empty)
			{
				if (CellClass* pHomeAirBeach = FindSafeIslandLandingCell(pHouse, homeCoords, primaryArmyZone, false))
				{
					if (isAirIdle)
					{
						pAir->SetDestination(pHomeAirBeach, true);
						pAir->QueueMission(Mission::Move, true);
					}
				}
			}
			else if (!pAir->IsInAir() && pAir->GetHeight() == 0
				&& (!pendingCrossIslandOrders.empty() || enemyOnDisconnectedIsland || pIslandResourceGoalCell != nullptr
					|| pIslandTechGoalCell != nullptr || scores[28] > 600 || scores[75] > 100))
			{
				int boardedAir = numAirPass;
				int sizeAir = totalAirPassSize;
				for (auto const& ord : pendingCrossIslandOrders)
				{
					if (!ord.Passenger || !ord.Passenger->IsAlive || ord.Passenger->InLimbo)
						continue;

					if (boardedAir >= pAir->Type->Passengers || (sizeAir + ord.PassengerSize) > pAir->Type->Passengers)
						break;

					if (ord.PassengerSize <= static_cast<int>(pAir->Type->SizeLimit) && ord.PickupZone == airZone)
					{
						OrderFootToEnter(ord.Passenger, pAir);
						boardedAir++;
						sizeAir += ord.PassengerSize;
					}
				}

				for (auto const pV : idleCombatUnits)
				{
					const int vehSize = static_cast<int>(pV->Type->TechnoTypeClass::Size);
					if (boardedAir >= pAir->Type->Passengers || (sizeAir + vehSize) > pAir->Type->Passengers)
						break;

					if (pV->CurrentMission == Mission::Enter || pV->QueuedMission == Mission::Enter)
						continue;

					const int uz
						= MapClass::Instance.GetMovementZoneType(pV->GetMapCoords(), MovementZone::Normal, false);
					if (IsFootActivelyAttackingOrMovingToEnemy(pV) || IsFootNearEnemyFrontline(pV, uz))
						continue;

					if (!IsAmphibiousOrAirTransport(pV->Type) && vehSize <= static_cast<int>(pAir->Type->SizeLimit)
						&& uz == airZone && pV->GetMapCoords().DistanceFrom(airCoords) <= 28.0)
					{
						OrderFootToEnter(pV, pAir);
						boardedAir++;
						sizeAir += vehSize;
					}
				}

				for (auto const pI : idleCombatInf)
				{
					if (boardedAir >= pAir->Type->Passengers)
						break;

					if (pI->CurrentMission == Mission::Enter || pI->QueuedMission == Mission::Enter)
						continue;

					const int infSize = std::max(1, static_cast<int>(pI->Type->TechnoTypeClass::Size));
					if (infSize > static_cast<int>(pAir->Type->SizeLimit)
						|| (sizeAir + infSize) > pAir->Type->Passengers)
					{
						continue;
					}

					const int iz
						= MapClass::Instance.GetMovementZoneType(pI->GetMapCoords(), MovementZone::Normal, false);
					if (IsFootActivelyAttackingOrMovingToEnemy(pI) || IsFootNearEnemyFrontline(pI, iz))
						continue;

					if (iz == airZone && pI->GetMapCoords().DistanceFrom(airCoords) <= 28.0)
					{
						OrderFootToEnter(pI, pAir);
						boardedAir++;
						sizeAir += infSize;
					}
				}
			}

			continue;
		}

		if (pAir->Type->Carryall && pAir->Passengers.NumPassengers > 0)
		{
			FootClass* pCarriedFoot = pAir->Passengers.GetFirstPassenger();
			UnitClass* pCarriedUnit = abstract_cast<UnitClass*>(pCarriedFoot);
			CellClass* pDropCell = nullptr;
			if (pCarriedUnit && pCarriedUnit->Type)
			{
				if (pRepairDepot && pCarriedUnit->GetHealthPercentage() < 0.45)
				{
					pDropCell = pRepairDepot->GetCell();
				}
				else if ((pCarriedUnit->Type->DeploysInto != nullptr || pCarriedUnit->Type->Harvester
							|| pCarriedUnit->Type->Weeder)
					&& pIslandResourceGoalCell)
				{
					pDropCell = pIslandResourceGoalCell;
				}
			}

			if (!pDropCell)
			{
				pDropCell = pIslandGoalCell
					? pIslandGoalCell
					: (pIslandTechGoalCell ? pIslandTechGoalCell
										: (pIslandResourceGoalCell ? pIslandResourceGoalCell : pWaveTargetCell));
			}

			if (pDropCell)
			{
				if (pAir->DistanceFrom(pDropCell) <= 768 && pDropCell->LandType != LandType::Water)
				{
					if (pAir->CurrentMission != Mission::Unload)
					{
						pAir->SetDestination(pDropCell, true);
						pAir->QueueMission(Mission::Unload, true);
					}
				}
				else if (isAirIdle || (Unsorted::CurrentFrame % 90 < 25))
				{
					pAir->SetDestination(pDropCell, true);
					pAir->QueueMission(Mission::Move, true);
				}
			}

			continue;
		}

		if (pAir->Type->Carryall && isAirIdle && pAir->Passengers.NumPassengers == 0
			&& (Unsorted::CurrentFrame % 90 < carryallWindow))
		{
			for (auto const pU : UnitClass::Array)
			{
				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type
					|| !CanCarryallLiftUnit(pAir, pU))
				{
					continue;
				}

				const int uZone
					= MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false);
				const bool isCriticallyDamagedTank
					= (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
				const bool isLoadedStrikeAPC = (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 1
					&& (pWaveTargetCell || pIslandGoalCell || pIslandTechGoalCell));
				const bool isIslandExpansionMCV = (pU->Type->DeploysInto != nullptr && pIslandResourceGoalCell
					&& uZone >= 0 && uZone != resourceIslandZone
					&& (pHouse->NumConYards >= 1 || pHouse->CountOwnedNow(pU->Type) >= 2));
				const bool isIslandHarvester = ((pU->Type->Harvester || pU->Type->Weeder) && pIslandResourceGoalCell
					&& uZone == homeGroundZone && pHouse->CountOwnedNow(pU->Type) >= 2);
				if (isCriticallyDamagedTank || isLoadedStrikeAPC || isIslandExpansionMCV || isIslandHarvester)
				{
					pAir->SetTarget(pU);
					pAir->QueueMission(Mission::Capture, true);
					break;
				}
			}

			continue;
		}

		if (false && pAir->Type->Carryall && isAirIdle && pAir->Passengers.NumPassengers == 0
			&& (Unsorted::CurrentFrame % 90 < carryallWindow))
		{
			for (auto const pU : UnitClass::Array)
			{
				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || pU->Type->Naval)
					continue;

				const bool isCriticallyDamagedTank
					= (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);

				const bool isLoadedStrikeAPC
					= (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 2 && pWaveTargetCell);

				if (isCriticallyDamagedTank || isLoadedStrikeAPC)
				{
					pAir->SetTarget(pU);

					pAir->QueueMission(Mission::Capture, true);

					break;
				}
			}

			continue;
		}

		if (pAir->Ammo > 0 && isAirIdle)
		{
			if (pEnemyMindController)
			{
				pAir->SetTarget(pEnemyMindController);

				pAir->QueueMission(Mission::Attack, true);
			}
			else if (pVisibleEnemyHarvester && scores[56] > 200)
			{
				pAir->SetTarget(pVisibleEnemyHarvester);

				pAir->QueueMission(Mission::Attack, true);
			}
			else if (pVisibleEnemyBld)
			{
				pAir->SetTarget(pVisibleEnemyBld);

				pAir->QueueMission(Mission::Attack, true);
			}
		}
	}

	// Dying Garrison Evacuation, Re-Mobilizing Deployed Siege Units (Slot 57),
	// Smart Wrench Building Repair (Slot 59), Emergency Sell-Dodge & ConYard
	// Emergency Undeploy into MCV (Slot 54)!
	if (Unsorted::CurrentFrame % 30 < 10)
	{
		// Evacuate our garrisoned infantry from any civilian/bunker structure that
		// is about to collapse (< 18% HP) so the squad survives!
		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || pBld->Occupants.Count == 0)
				continue;

			if (pBld->Occupants.GetItem(0) && pBld->Occupants.GetItem(0)->Owner == pHouse
				&& pBld->GetHealthPercentage() < 0.18)
			{
				pBld->QueueMission(Mission::Unload, true);
			}
		}

		const double sellDodgeThresh = std::clamp(0.09 + static_cast<double>(scores[54]) * 0.0001, 0.05, 0.16);
		const double undeployThresh = std::clamp(0.14 + static_cast<double>(scores[54]) * 0.00012, 0.08, 0.22);
		const int currentCredits = pHouse->Available_Money();
		const bool isLowPower = pHouse->HasLowPower();
		bool hasPoweredDefense = false;

		if (isLowPower)
		{
			for (auto const pCheckBld : pHouse->Buildings)
			{
				if (pCheckBld && !pCheckBld->InLimbo && pCheckBld->IsAlive && pCheckBld->Type
					&& pCheckBld->Type->IsBaseDefense && pCheckBld->Type->Powered && pCheckBld->StuffEnabled)
				{
					hasPoweredDefense = true;

					break;
				}
			}
		}

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->IsIronCurtained())
				continue;

			// Slot 60 (Emergency Low-Power Defense Prioritization): If our base is in
			// LowPower and we own powered base defenses (Tesla Coils, Prism Towers,
			// Obelisks, SAMs), temporarily toggle offline non-defense high-drain
			// structures (Superweapons, Tech/Radar buildings) via GoOffline() so base
			// defenses stay active! Once power surplus is restored (>= 150), bring
			// offline structures back online via GoOnline().
			if (pOwnBld->Type->TogglePower && (Unsorted::CurrentFrame % 90 < 15))
			{
				if (isLowPower && hasPoweredDefense && scores[60] > 80 && pOwnBld->StuffEnabled
					&& !pOwnBld->Type->IsBaseDefense && pOwnBld->Type->PowerDrain >= 100
					&& (pOwnBld->Type->SuperWeapon != -1 || pOwnBld->Type->Factory == AbstractType::None))
				{
					pOwnBld->GoOffline();
				}
				else if (!isLowPower && !pOwnBld->StuffEnabled && (pHouse->PowerOutput - pHouse->PowerDrain >= 150))
				{
					pOwnBld->GoOnline();
				}
			}

			// Slot 75 (Island Beachhead Primary Factory Switch): Once we construct a
			// War Factory or Barracks on a disconnected island (e.g. from a landed
			// Beachhead MCV), set it as PrimaryFactory via TogglePrimaryFactory() so
			// new tanks and infantry spawn directly on the enemy's island!
			if (!pOwnBld->IsPrimaryFactory
				&& (pOwnBld->Type->Factory == AbstractType::UnitType
					|| pOwnBld->Type->Factory == AbstractType::InfantryType)
				&& !pOwnBld->Type->Naval && (Unsorted::CurrentFrame % 90 < 20))
			{
				const int bldZone
					= MapClass::Instance.GetMovementZoneType(pOwnBld->GetMapCoords(), MovementZone::Normal, false);
				bool factoryOnEnemyIsland = (enemyOnDisconnectedIsland && bldZone >= 0 && bldZone == targetIslandZone);
				if (!factoryOnEnemyIsland && bldZone >= 0 && homeGroundZone >= 0 && bldZone != homeGroundZone)
				{
					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
							|| pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
						{
							continue;
						}

						if (MapClass::Instance.GetMovementZoneType(pEB->GetMapCoords(), MovementZone::Normal, false)
							== bldZone)
						{
							factoryOnEnemyIsland = true;
							break;
						}
					}
				}

				if (factoryOnEnemyIsland)
				{
					bool alreadyHasPrimaryInZone = false;
					for (auto const pOtherB : pHouse->Buildings)
					{
						if (!pOtherB || pOtherB == pOwnBld || pOtherB->InLimbo || !pOtherB->IsAlive || !pOtherB->Type
							|| !pOtherB->IsPrimaryFactory)
						{
							continue;
						}

						if (pOtherB->Type->Factory == pOwnBld->Type->Factory
							&& pOtherB->Type->Naval == pOwnBld->Type->Naval
							&& MapClass::Instance.GetMovementZoneType(
								pOtherB->GetMapCoords(), MovementZone::Normal, false)
								== bldZone)
						{
							alreadyHasPrimaryInZone = true;
							break;
						}
					}

					if (!alreadyHasPrimaryInZone)
						pOwnBld->TogglePrimaryFactory();
				}
			}

			const double hpPct = pOwnBld->GetHealthPercentage();

			// Slot 73 (Dynamic Tank Bunker Unload): Eject the bunkered tank if the
			// Tank Bunker is about to collapse (< 22% HP) or if no enemies are near!
			if (pOwnBld->Type->Bunker && pOwnBld->BunkerLinkedItem != nullptr && scores[73] > 60)
			{
				if (hpPct < 0.22)
					pOwnBld->UnloadBunker();
			}

			// Helper to properly undeploy a BuildingClass into its UndeploysInto
			// vehicle (MCV, Tick Tank, Nod Artillery, Siege Chopper): In the Yuri's
			// Revenge engine, BuildingClass undeployment requires setting a non-null
			// ArchiveTarget cell and calling Sell(1) (whereas selling for cash
			// requires ArchiveTarget == nullptr).
			auto UndeployBuildingToVehicle = [](BuildingClass* pBld, const CellStruct& goalCoords)
			{
				if (!pBld || !pBld->Type || !pBld->Type->UndeploysInto)
					return;

				// Never manually force-undeploy a Slave Miner / ResourceGatherer
				// building via Sell(1); its native BuildingClass::Mission_Harvest state
				// machine handles undeployment when moving between ore patches!
				if (pBld->Type->ResourceGatherer || pBld->Type->Enslaves != nullptr)
					return;

				if (pBld->CurrentMission == Mission::Construction || pBld->CurrentMission == Mission::Selling
					|| pBld->QueuedMission == Mission::Construction || pBld->QueuedMission == Mission::Selling)
				{
					return;
				}

				const CellStruct bc = pBld->GetMapCoords();
				const int bZone = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
				CellClass* pDestCell = nullptr;
				if (goalCoords != CellStruct::Empty && goalCoords != bc)
				{
					if (auto const pGC = MapClass::Instance.TryGetCellAt(goalCoords))
					{
						if (pGC->LandType != LandType::Water
							&& (bZone < 0
								|| MapClass::Instance.GetMovementZoneType(goalCoords, MovementZone::Normal, false)
									== bZone))
						{
							pDestCell = pGC;
						}
					}
				}

				if (!pDestCell)
				{
					for (int dy = -3; dy <= 3 && !pDestCell; ++dy)
					{
						for (int dx = -3; dx <= 3 && !pDestCell; ++dx)
						{
							if (dx == 0 && dy == 0)
								continue;

							const CellStruct nc { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };
							if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
							{
								if (pNC->LandType != LandType::Water && pNC->GetBuilding() == nullptr
									&& (bZone < 0
										|| MapClass::Instance.GetMovementZoneType(nc, MovementZone::Normal, false)
											== bZone))
								{
									pDestCell = pNC;
								}
							}
						}
					}
				}

				if (!pDestCell)
					pDestCell = pBld->GetCell();

				if (pDestCell)
				{
					pBld->SetArchiveTarget(pDestCell);
					pBld->Sell(1);
				}
			};

			// Always activate wrench repair immediately on any damaged Construction Yard
			// so it never stays below undeployThresh or stalls tech/defense production
			if (pOwnBld->Type->ConstructionYard && pOwnBld->Type->ClickRepairable && hpPct < 0.92
				&& currentCredits >= 100 && !pOwnBld->IsBeingRepaired)
			{
				pOwnBld->SetRepairState(1);
			}

			bool handledConYardAction = false;

			if (hpPct < 0.16 && pOwnBld->HasBeenAttacked && pOwnBld->Type->ConstructionYard && pHouse->NumConYards == 1
				&& pOwnBld->Type->UndeploysInto != nullptr && GameModeOptionsClass::Instance.MCVRedeploy)
			{
				const CellStruct yc = pOwnBld->GetMapCoords();
				const int yz = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, false);
				bool closeAttackerNearCY = false;

				for (auto const pET : TechnoClass::Array)
				{
					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner
						|| pET->WhatAmI() == AbstractType::Building)
					{
						continue;
					}

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
						continue;

					if (pOwnBld->DistanceFrom(pET) <= 2048)
					{
						closeAttackerNearCY = true;
						break;
					}
				}

				CellStruct safeRetreatHub = CellStruct::Empty;

				if (closeAttackerNearCY)
				{
					for (auto const pFriendlyB : pHouse->Buildings)
					{
						if (!pFriendlyB || pFriendlyB == pOwnBld || pFriendlyB->InLimbo || !pFriendlyB->IsAlive
							|| !pFriendlyB->Type)
						{
							continue;
						}

						const CellStruct fc = pFriendlyB->GetMapCoords();

						if (yz >= 0 && MapClass::Instance.GetMovementZoneType(fc, MovementZone::Normal, false) != yz)
							continue;

						if (fc.DistanceFrom(yc) >= 16.0)
						{
							safeRetreatHub = fc;
							break;
						}
					}
				}

				// Only pack up our last ConYard if an enemy is right on top of it AND we own a
				// distant friendly hub (>= 16 cells away) to retreat to; otherwise stay deployed
				// with wrench repair active and keep building defenses and units!
				if (closeAttackerNearCY && safeRetreatHub != CellStruct::Empty)
				{
					UndeployBuildingToVehicle(pOwnBld, safeRetreatHub);
					handledConYardAction = true;
				}
			}
			else if (pOwnBld->Type->ConstructionYard && pHouse->NumConYards >= 2
				&& pOwnBld != pHouse->ConYards.GetItem(0) && scores[82] > 60 && homeCoords != CellStruct::Empty
				&& pOwnBld->GetMapCoords().DistanceFrom(homeCoords) > 18.0)
			{
				const CellStruct yc = pOwnBld->GetMapCoords();
				int closeHostileAttackers = 0;

				for (auto const pEB : BuildingClass::Array)
				{
					if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune)
						continue;

					if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
						continue;

					const CellStruct ec = pEB->GetMapCoords();
					const int dB = std::max(std::abs(yc.X - ec.X), std::abs(yc.Y - ec.Y));

					if (pEB->Type->IsBaseDefense && dB <= 11)
						closeHostileAttackers++;
				}

				for (auto const pET : TechnoClass::Array)
				{
					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner
						|| pET->WhatAmI() == AbstractType::Building)
					{
						continue;
					}

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
						continue;

					if (pOwnBld->DistanceFrom(pET) <= 2560)
						closeHostileAttackers++;
				}

				const bool hostileZone = (closeHostileAttackers >= 1)
					&& (pOwnBld->HasBeenAttacked || hpPct < 0.75 || closeHostileAttackers >= 2);

				if (hostileZone)
				{
					const bool canUndeployToMCV
						= (GameModeOptionsClass::Instance.MCVRedeploy && pOwnBld->Type->UndeploysInto != nullptr);
					const int yardZone = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, false);
					const bool onEnemyIsland = (homeGroundZone >= 0 && yardZone >= 0 && yardZone != homeGroundZone);
					bool hasAmphibTransportOrNavalYard = (pHouse->NumShipyards > 0);

					if (onEnemyIsland && !hasAmphibTransportOrNavalYard)
					{
						for (auto const pT : UnitClass::Array)
						{
							if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->IsAlive && pT->Type
								&& IsAmphibiousOrAirTransport(pT->Type))
							{
								hasAmphibTransportOrNavalYard = true;
								break;
							}
						}
					}

					const bool impossibleToExtractAlive
						= (!canUndeployToMCV && (hpPct < 0.65 || closeHostileAttackers >= 2))
						|| (hpPct < 0.32 && closeHostileAttackers >= 2)
						|| (onEnemyIsland && !hasAmphibTransportOrNavalYard
							&& (hpPct < 0.60 || closeHostileAttackers >= 1));

					if (impossibleToExtractAlive)
					{
						pOwnBld->SetArchiveTarget(nullptr);
						pOwnBld->Sell(1);
					}
					else if (canUndeployToMCV)
					{
						UndeployBuildingToVehicle(pOwnBld, homeCoords);
					}
					else if (pOwnBld->Type->ClickRepairable && hpPct < 0.90 && currentCredits > 600
						&& !pOwnBld->IsBeingRepaired)
					{
						pOwnBld->SetRepairState(1);
					}

					handledConYardAction = true;
				}
			}

			const bool conYardUnderRecentAttack = pOwnBld->HasBeenAttacked || hpPct < 0.90
				|| (pHouse->LATime > 0 && (Unsorted::CurrentFrame - pHouse->LATime) < 1200);

			if (!handledConYardAction && !conYardUnderRecentAttack && GameModeOptionsClass::Instance.MCVRedeploy
				&& pOwnBld->Type->ConstructionYard && pOwnBld->Type->UndeploysInto != nullptr
				&& (Unsorted::CurrentFrame % 240 < 20))
			{
				FactoryClass* pBldFac
					= pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare);
				FactoryClass* pDefFac = pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::Combat);
				const bool busyConstructingBld
					= ((pBldFac && pBldFac->Object != nullptr) || (pDefFac && pDefFac->Object != nullptr));
				const CellStruct yc = pOwnBld->GetMapCoords();
				const int yz = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, false);
				auto const pMcvType = pOwnBld->Type->UndeploysInto;
				bool ownsLiveMcvTransport = false;

				for (auto const pT : UnitClass::Array)
				{
					if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->IsAlive && pT->Type
						&& IsAmphibiousOrAirTransport(pT->Type)
						&& static_cast<int>(pT->Type->SizeLimit) >= static_cast<int>(pMcvType->TechnoTypeClass::Size)
						&& pT->Type->Passengers >= static_cast<int>(pMcvType->TechnoTypeClass::Size))
					{
						ownsLiveMcvTransport = true;
						break;
					}
				}

				const unsigned int yardHouseBit
					= (pHouse->Type && pHouse->Type->ArrayIndex >= 0 && pHouse->Type->ArrayIndex < 32)
					? (1u << pHouse->Type->ArrayIndex)
					: 0u;
				const bool canBuildAdditionalMCV = (pHouse->NumWarFactories >= 1 && pMcvType->TechLevel >= 0
					&& pMcvType->TechLevel <= pHouse->TechLevel && yardHouseBit != 0u
					&& pMcvType->InOwners(yardHouseBit) && HouseExt::PrerequisitesMet(pHouse, pMcvType));
				const bool canFerryNomadNow
					= (pIslandResourceGoalCell != nullptr && yz != resourceIslandZone && ownsLiveMcvTransport);

				bool hasUnharvestedOreNearThisYard = false;
				CellClass* pDistantUncolonizedLandOreCell = nullptr;
				int bestDistantOreDist = 999999;

				for (int dy = -64; dy <= 64; dy += 4)
				{
					for (int dx = -64; dx <= 64; dx += 4)
					{
						const CellStruct tc { static_cast<short>(yc.X + dx), static_cast<short>(yc.Y + dy) };
						auto const pC = MapClass::Instance.TryGetCellAt(tc);

						if (!pC)
							continue;

						if (MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false) != yz)
							continue;

						const auto pTerr = pC->GetTerrain(false);
						const bool hasOreOrTree = (pC->GetContainedTiberiumValue() > 0)
							|| (pTerr && pTerr->Type && pTerr->Type->SpawnsTiberium);

						if (!hasOreOrTree)
							continue;

						const int chebDist = std::max(std::abs(dx), std::abs(dy));

						if (chebDist <= 22)
						{
							hasUnharvestedOreNearThisYard = true;
							break;
						}
						else if (chebDist >= 26 && chebDist < bestDistantOreDist)
						{
							bool coveredByOwnedHub = false;

							for (auto const pCheckB : pHouse->Buildings)
							{
								if (!pCheckB || pCheckB->InLimbo || !pCheckB->IsAlive || !pCheckB->Type)
									continue;

								if (pCheckB->Type->Refinery || pCheckB->Type->ConstructionYard
									|| pCheckB->Type->ResourceDestination)
								{
									const CellStruct cbc = pCheckB->GetMapCoords();

									if (std::max(std::abs(tc.X - cbc.X), std::abs(tc.Y - cbc.Y)) <= 16)
									{
										coveredByOwnedHub = true;
										break;
									}
								}
							}

							if (!coveredByOwnedHub)
							{
								bestDistantOreDist = chebDist;
								pDistantUncolonizedLandOreCell = pC;
							}
						}
					}

					if (hasUnharvestedOreNearThisYard)
						break;
				}

				int structuresDroppedHere = 0;
				int defsDroppedHere = 0;
				bool hasRefineryHere = false;
				bool hasWarFacHere = false;

				for (auto const pNearB : pHouse->Buildings)
				{
					if (!pNearB || pNearB == pOwnBld || pNearB->InLimbo || !pNearB->IsAlive || !pNearB->Type)
						continue;

					const CellStruct nc = pNearB->GetMapCoords();

					if (std::max(std::abs(yc.X - nc.X), std::abs(yc.Y - nc.Y)) <= 16
						&& MapClass::Instance.GetMovementZoneType(nc, MovementZone::Normal, false) == yz)
					{
						if (pNearB->Type->Refinery || pNearB->Type->ResourceDestination || pNearB->Type->BaseNormal
							|| pNearB->Type->Factory != AbstractType::None || pNearB->Type->IsBaseDefense)
						{
							structuresDroppedHere++;
						}

						if (pNearB->Type->Refinery || pNearB->Type->ResourceDestination)
							hasRefineryHere = true;

						if (pNearB->Type->Factory == AbstractType::UnitType && !pNearB->Type->Naval)
							hasWarFacHere = true;

						if (pNearB->Type->IsBaseDefense && !IsTunnelNetworkBuilding(pNearB->Type))
							defsDroppedHere++;
					}
				}

				const bool hasRefineryOrFactoryHere = hasRefineryHere && hasWarFacHere;
				CellClass* pNomadicResourceGoalCell
					= canFerryNomadNow ? pIslandResourceGoalCell : pDistantUncolonizedLandOreCell;

				const bool isSecondaryConYardReadyToHop
					= (pHouse->NumConYards >= 2 && pOwnBld != pHouse->ConYards.GetItem(0)
						&& !hasUnharvestedOreNearThisYard && !canBuildAdditionalMCV && hasRefineryHere && hasWarFacHere
						&& defsDroppedHere >= 2 && structuresDroppedHere >= 5 && pNomadicResourceGoalCell != nullptr);

				const bool isSingleConYardDepletedHop = (pHouse->NumConYards == 1 && !canBuildAdditionalMCV
					&& !hasUnharvestedOreNearThisYard && pHouse->OwnedBuildings >= 10 && pHouse->NumWarFactories >= 1
					&& hasRefineryOrFactoryHere && structuresDroppedHere >= 4 && pNomadicResourceGoalCell != nullptr);

				if (!busyConstructingBld && pNomadicResourceGoalCell != nullptr
					&& (isSecondaryConYardReadyToHop || isSingleConYardDepletedHop))
				{
					UndeployBuildingToVehicle(pOwnBld, pNomadicResourceGoalCell->MapCoords);
					handledConYardAction = true;
				}
			}

			if (!handledConYardAction && hpPct < sellDodgeThresh && !pOwnBld->Type->ConstructionYard
				&& !pOwnBld->Type->ResourceGatherer && pOwnBld->Type->Enslaves == nullptr && pOwnBld->Type->Cost >= 500
				&& pOwnBld->CurrentMission != Mission::Selling)
			{
				// Classic C&C Sell-Dodge right before destruction!
				pOwnBld->SetArchiveTarget(nullptr);
				pOwnBld->Sell(1);
			}
			else if (!handledConYardAction && pOwnBld->Type->UndeploysInto != nullptr
				&& !pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery && !pOwnBld->Type->ResourceGatherer
				&& !pOwnBld->Type->ResourceDestination && pOwnBld->Type->Enslaves == nullptr
				&& !pOwnBld->Type->BaseNormal && !IsTrueBaseStructureTarget(pOwnBld->Type)
				&& (pOwnBld->Type->IsBaseDefense
					|| (pOwnBld->Type->GetWeapon(0) != nullptr && pOwnBld->Type->GetWeapon(0)->WeaponType != nullptr))
				&& pOwnBld->Target == nullptr && (Unsorted::CurrentFrame % 150 < 30))
			{
				// Slot 57 (Re-Mobilize Deployed Siege Unit): When a deployed Siege
				// Chopper, Tick Tank, or Nod Artillery has finished combat and no
				// enemies are within 12 cells, pack back into a mobile vehicle so it
				// advances with the army!
				bool enemyInRange = false;
				for (auto const pETech : TechnoClass::Array)
				{
					if (!pETech || pETech->InLimbo || !pETech->IsAlive || !pETech->Owner)
						continue;

					if (pETech->Owner == pHouse || pHouse->IsAlliedWith(pETech->Owner) || pETech->Owner->IsNeutral())
						continue;

					if (pOwnBld->DistanceFrom(pETech) <= 3072)
					{
						enemyInRange = true;
						break;
					}
				}

				if (!enemyInRange)
				{
					const CellStruct rallyGoal = pWaveTargetCell ? pWaveTargetCell->MapCoords : homeCoords;
					UndeployBuildingToVehicle(pOwnBld, rallyGoal);
				}
			}
			else if (pOwnBld->Type->ClickRepairable)
			{
				// Slot 59 (Smart Wrench Building Repair): Activate engine wrench repair
				// on damaged key structures when credits are healthy, and pause repair
				// if credits drop critically low!
				if (hpPct < 0.82 && hpPct >= sellDodgeThresh && scores[59] > 80 && currentCredits > 1200
					&& !pOwnBld->IsBeingRepaired)
				{
					pOwnBld->SetRepairState(1);
				}
				else if (pOwnBld->IsBeingRepaired && currentCredits < 450)
				{
					pOwnBld->SetRepairState(0);
				}
			}
		}
	}

	// CABAL / Faction Research & Tech-Upgrade Unlocks (PreUpgr_SW* /
	// LimboUpgrade_SW* where DisableableFromShell=no, excluding Tunnel Delivery
	// SWs)
	if (homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 120 < 25))
	{
		for (auto const pSuper : pHouse->Supers)
		{
			if (!pSuper || !pSuper->IsReady || !pSuper->Type)
				continue;

			if (_stricmp(pSuper->Type->ID, "DONT_USE_THIS_SW") == 0)
				continue;

			auto const pSWExt = SWTypeExt::Fetch(pSuper->Type);
			const bool isLimboResearchUpgrade = pSWExt
				&& (!pSWExt->LimboDelivery_Types.empty() || !pSWExt->LimboKill_IDs.empty()
					|| !pSWExt->Convert_Pairs.empty());

			if (!pSuper->Type->DisableableFromShell && isLimboResearchUpgrade
				&& pSuper->Type->Type != SuperWeaponType::ChronoWarp
				&& !IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
			{
				pSuper->Launch(homeCoords, false);
				pSuper->Reset();
				break;
			}
		}
	}

	// CABAL / Ares / Antares Subterranean Tunnel Network (ROBOTTUNNEL +
	// CABALTunnelSpecial -> ROBOTCTUNNEL -> ROBOTTUNNEL): Creative Tunnel Exit
	// Placement via Tunnel Delivery SW on unprotected enemy flanks, behind
	// defenses, on islands, or near neutral Techs Two-Way Subterranean Portal
	// Micro: load Infantry, Cyborgs, Engineers & Size<=2 Vehicles at Home Tunnel
	// -> eject at Forward Tunnel!
	if (homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 45 < 20))
	{
		BuildingClass* pHomeTunnel = nullptr;
		BuildingClass* pForwardTunnel = nullptr;
		int minHomeTunnelDist = 999999;
		int bestForwardTunnelScore = -999999;
		int ownTunnelCount = 0;

		if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))
		{
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
					|| !IsTunnelNetworkBuilding(pOwnBld->Type))
				{
					continue;
				}

				ownTunnelCount++;

				const int dHome = pOwnBld->DistanceFrom(pHomeCell);

				if (dHome < minHomeTunnelDist)
				{
					minHomeTunnelDist = dHome;

					pHomeTunnel = pOwnBld;
				}
			}

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld == pHomeTunnel || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
					|| !IsTunnelNetworkBuilding(pOwnBld->Type))
				{
					continue;
				}

				const int dHome = pOwnBld->DistanceFrom(pHomeCell);
				const int dTarget = pWaveTargetCell ? pOwnBld->DistanceFrom(pWaveTargetCell) : 0;
				const int fwdScore = pWaveTargetCell ? (dHome - dTarget * 2) : dHome;

				if (fwdScore > bestForwardTunnelScore)
				{
					bestForwardTunnelScore = fwdScore;

					pForwardTunnel = pOwnBld;
				}
			}
		}

		// Step 1: If we own 1 Tunnel (or need a 2nd Tunnel endpoint) and our Tunnel
		// Delivery SW (e.g. CABALTunnelSpecial) is ready, dig the remote tunnel
		// endpoint at an unprotected enemy flank, island beachhead, or neutral Tech
		// Building!
		if (ownTunnelCount == 1 && pHomeTunnel)
		{
			for (auto const pSuper : pHouse->Supers)
			{
				if (!pSuper || !pSuper->IsReady || !pSuper->Type || !IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
					continue;

				CellStruct bestTunnelCell = CellStruct::Empty;
				int bestTunnelCellScore = -999999;

				// If our single surviving tunnel is far from home (> 18 cells), our
				// Home Tunnel was destroyed -> restore Home Tunnel first!
				if (minHomeTunnelDist > 4608)
				{
					if (CellClass* pHomeRestore = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false))
						bestTunnelCell = pHomeRestore->MapCoords;
				}
				else
				{
					// Evaluate visible enemy buildings and neutral capturable Tech
					// Buildings to find an unprotected flank/infiltration spot
					for (auto const pBld : BuildingClass::Array)
					{
						if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type
							|| pBld->Type->Immune)
						{
							continue;
						}

						const bool isNeutralTech = (pBld->Owner->IsNeutral() && pBld->Type->Capturable);

						const bool isEnemyBld = (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner)
							&& !pBld->Owner->IsNeutral() && !pBld->Owner->IsObserver() && !pBld->Owner->Defeated);

						if (!isNeutralTech && !isEnemyBld)
							continue;

						auto const pBldCell = pBld->GetCell();

						if (!pBldCell || !IsCellVisibleToAI(pHouse, pBldCell))
							continue;

						const CellStruct bc = pBld->GetMapCoords();
						const int distFromHomeCells = std::abs(bc.X - homeCoords.X) + std::abs(bc.Y - homeCoords.Y);

						if (distFromHomeCells < 20)
							continue;

						const int ringMin = isNeutralTech ? 3 : 6;
						const int ringMax = isNeutralTech ? 6 : 12;

						for (int dy = -ringMax; dy <= ringMax; dy += 3)
						{
							for (int dx = -ringMax; dx <= ringMax; dx += 3)
							{
								const int cheb = std::max(std::abs(dx), std::abs(dy));

								if (cheb < ringMin || cheb > ringMax)
									continue;

								const CellStruct tc { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };
								auto const pCand = MapClass::Instance.TryGetCellAt(tc);

								if (!pCand || !IsCellVisibleToAI(pHouse, pCand))
									continue;

								if ((pCand->LandType != LandType::Clear && pCand->LandType != LandType::Road)
									|| pCand->ContainsBridge() || pCand->GetBuilding() != nullptr
									|| pCand->FirstObject != nullptr)
								{
									continue;
								}

								// Count nearby enemy defensive turrets within 9 cells of this
								// flank candidate
								int nearbyEnemyTurrets = 0;

								for (auto const pOtherBld : BuildingClass::Array)
								{
									if (!pOtherBld || pOtherBld->InLimbo || !pOtherBld->IsAlive || !pOtherBld->Owner
										|| !pOtherBld->Type)
									{
										continue;
									}

									if (pOtherBld->Owner == pHouse || pHouse->IsAlliedWith(pOtherBld->Owner)
										|| pOtherBld->Owner->IsNeutral())
									{
										continue;
									}

									if (pOtherBld->Type->IsBaseDefense && pOtherBld->Type->GetWeapon(0) != nullptr)
									{
										const CellStruct oc = pOtherBld->GetMapCoords();

										if (std::abs(tc.X - oc.X) <= 9 && std::abs(tc.Y - oc.Y) <= 9)
											nearbyEnemyTurrets++;
									}
								}

								int candScore = 400 - nearbyEnemyTurrets * 260;

								if (nearbyEnemyTurrets == 0)
								{
									candScore +=

										450; // Completely unprotected flank / rear blindspot!
								}

								if (isNeutralTech)
								{
									candScore += 380 + (scores[36] > 60 ? 200 : 0);
								}
								else
								{
									if (pBld->Type->ConstructionYard || pBld->Type->SuperWeapon != -1)
									{
										candScore += 420;
									}
									else if (pBld->Type->Factory != AbstractType::None || pBld->Type->PowerBonus > 0
										|| pBld->Type->Refinery)
									{
										candScore += 280;
									}
								}

								const int candZone
									= MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);

								if (homeGroundZone >= 0 && candZone >= 0 && candZone != homeGroundZone)
									candScore += 480; // Disconnected island portal!

								if (candScore > bestTunnelCellScore)
								{
									bestTunnelCellScore = candScore;

									bestTunnelCell = tc;
								}
							}
						}
					}

					// Fallback: if no visible enemy building yet but pWaveTargetCell is
					// far and visible, place near frontline
					if (bestTunnelCell == CellStruct::Empty && pWaveTargetCell)
					{
						const CellStruct wc = pWaveTargetCell->MapCoords;

						if (std::abs(wc.X - homeCoords.X) + std::abs(wc.Y - homeCoords.Y) >= 22)
						{
							if (CellClass* pFrontCell = FindSafeIslandLandingCell(pHouse, wc, targetIslandZone, false))
							{
								if (IsCellVisibleToAI(pHouse, pFrontCell))
									bestTunnelCell = pFrontCell->MapCoords;
							}
						}
					}
				}

				if (bestTunnelCell != CellStruct::Empty)
				{
					pSuper->Launch(bestTunnelCell, false);
					pSuper->Reset();

					break;
				}
			}
		}

		// Step 2: Active Two-Way Subterranean Tunnel Portal (pHomeTunnel <->
		// pForwardTunnel)
		if (ownTunnelCount >= 2 && pHomeTunnel && pForwardTunnel && pHomeTunnel != pForwardTunnel
			&& pHomeTunnel->DistanceFrom(pForwardTunnel) >= 2560)
		{
			// Keep all non-forward Tunnel mouths in Guard so passengers only unload
			// at pForwardTunnel!
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld == pForwardTunnel || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
					|| !IsTunnelNetworkBuilding(pOwnBld->Type))
				{
					continue;
				}

				if (pOwnBld->CurrentMission == Mission::Unload)
					pOwnBld->QueueMission(Mission::Guard, false);
			}

			if (pForwardTunnel->CurrentMission != Mission::Unload)
				pForwardTunnel->QueueMission(Mission::Unload, true);

			// Check if there is a high-value capturable building near pForwardTunnel
			// (within 22 cells = 5632 leptons)
			BuildingClass* pFlankCaptureTarget = nullptr;
			BuildingClass* pFlankAttackTarget = nullptr;
			int bestCapDist = 5632;
			int bestAtkDist = 6144;

			for (auto const pBld : BuildingClass::Array)
			{
				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Immune)
					continue;

				if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsObserver()
					|| pBld->Owner->Defeated)
				{
					continue;
				}

				const int dFwd = pBld->DistanceFrom(pForwardTunnel);

				if (dFwd <= bestCapDist && pBld->Type->Capturable
					&& (pBld->Owner->IsNeutral() || pBld->Type->ConstructionYard
						|| pBld->Type->Factory != AbstractType::None || pBld->Type->Refinery
						|| pBld->Type->SuperWeapon != -1 || pBld->Type->TechLevel >= 5))
				{
					bestCapDist = dFwd;

					pFlankCaptureTarget = pBld;
				}

				if (!pBld->Owner->IsNeutral() && dFwd <= bestAtkDist)
				{
					bestAtkDist = dFwd;

					pFlankAttackTarget = pBld;
				}
			}

			// Feed Engineers, Cyborgs, Infantry & Size<=SizeLimit Vehicles near
			// pHomeTunnel into the subterranean portal!
			int sentIntoTunnel = 0;
			const int maxBatchEnter = std::clamp(static_cast<int>(pHomeTunnel->Type->Passengers), 4, 10);

			if (pFlankCaptureTarget)
			{
				for (auto const pInf : InfantryClass::Array)
				{
					if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type
						|| !pInf->Type->Engineer)
					{
						continue;
					}

					// If Engineer emerged from pForwardTunnel, immediately capture the
					// flank target!
					if (pInf->DistanceFrom(pForwardTunnel) <= 2560)
					{
						OrderFootToEnter(pInf, pFlankCaptureTarget);
					}
					else if (pInf->DistanceFrom(pHomeTunnel) <= 7168 && pInf->CurrentMission != Mission::Enter)
					{
						OrderFootToEnter(pInf, pHomeTunnel);
						sentIntoTunnel++;
					}
				}
			}

			for (auto const pI : idleCombatInf)
			{
				if (sentIntoTunnel >= maxBatchEnter)
					break;

				if (pI->DistanceFrom(pForwardTunnel) <= 2304 && (pFlankAttackTarget || pWaveTargetCell))
				{
					if (pFlankAttackTarget)
					{
						pI->ClearMegaMissionData();
						pI->SetTarget(pFlankAttackTarget);

						pI->QueueMission(Mission::Attack, true);
					}
					else
					{
						OrderFootToAttackMove(pI, pWaveTargetCell);
					}
				}
				else if (pI->DistanceFrom(pHomeTunnel) <= 6144
					&& pI->DistanceFrom(pForwardTunnel) > pHomeTunnel->DistanceFrom(pForwardTunnel) - 1024)
				{
					OrderFootToEnter(pI, pHomeTunnel);
					sentIntoTunnel++;
				}
			}

			if (pHomeTunnel->Type->UnitAbsorb)
			{
				const int maxAllowedSize = std::max(1, static_cast<int>(pHomeTunnel->Type->SizeLimit));

				for (auto const pV : idleCombatUnits)
				{
					if (sentIntoTunnel >= maxBatchEnter)
						break;

					if (IsAmphibiousOrAirTransport(pV->Type)
						|| static_cast<int>(pV->Type->TechnoTypeClass::Size) > maxAllowedSize)
					{
						continue;
					}

					if (pV->DistanceFrom(pForwardTunnel) <= 2304 && (pFlankAttackTarget || pWaveTargetCell))
					{
						if (pFlankAttackTarget)
						{
							pV->ClearMegaMissionData();
							pV->SetTarget(pFlankAttackTarget);

							pV->QueueMission(Mission::Attack, true);
						}
						else
						{
							OrderFootToAttackMove(pV, pWaveTargetCell);
						}
					}
					else if (pV->DistanceFrom(pHomeTunnel) <= 6144
						&& pV->DistanceFrom(pForwardTunnel) > pHomeTunnel->DistanceFrom(pForwardTunnel) - 1024)
					{
						OrderFootToEnter(pV, pHomeTunnel);
						sentIntoTunnel++;
					}
				}
			}
		}
	}

	// Frontline Primary Factory Switching & Charged Superweapon Execution
	if (pWaveTargetCell && (Unsorted::CurrentFrame % 150 < 30))
	{
		BuildingClass* pBestWarFac = nullptr;
		BuildingClass* pBestBarracks = nullptr;
		int bestWarDist = 999999;
		int bestBarDist = 999999;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
				continue;

			if (pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval)
			{
				const int d = pBld->DistanceFrom(pWaveTargetCell);

				if (d < bestWarDist)
				{
					bestWarDist = d;

					pBestWarFac = pBld;
				}
			}
			else if (pBld->Type->Factory == AbstractType::InfantryType)
			{
				const int d = pBld->DistanceFrom(pWaveTargetCell);

				if (d < bestBarDist)
				{
					bestBarDist = d;

					pBestBarracks = pBld;
				}
			}
		}

		if (false && pBestWarFac && !pBestWarFac->IsPrimaryFactory)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval)
					pBld->IsPrimaryFactory = (pBld == pBestWarFac);
			}
		}

		if (false && pBestBarracks && !pBestBarracks->IsPrimaryFactory)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Factory == AbstractType::InfantryType)
					pBld->IsPrimaryFactory = (pBld == pBestBarracks);
			}
		}

		{
			SuperClass* pChronoWarp = pHouse->FindSuperWeapon(SuperWeaponType::ChronoWarp);

			for (auto const pSuper : pHouse->Supers)
			{
				if (!pSuper || !pSuper->IsReady || !pSuper->Type)
					continue;

				if (!GameModeOptionsClass::Instance.SWAllowed && pSuper->Type->DisableableFromShell)
					continue;

				if (_stricmp(pSuper->Type->ID, "DONT_USE_THIS_SW") == 0
					|| IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
				{
					continue;
				}

				if (auto const pSWExt = SWTypeExt::Fetch(pSuper->Type))
				{
					if (!pSWExt->LimboDelivery_Types.empty() || !pSWExt->LimboKill_IDs.empty()
						|| !pSWExt->Convert_Pairs.empty())
					{
						continue;
					}
				}

				const SuperWeaponType swType = pSuper->Type->Type;

				if (swType == SuperWeaponType::ChronoWarp)
					continue; // Fired as the second step of ChronoSphere

				// A) IRON CURTAIN & FORCE SHIELD: Defensive Emergency Save OR Offensive
				// Invulnerable Tank Assault
				if (swType == SuperWeaponType::IronCurtain || swType == SuperWeaponType::ForceShield)
				{
					CellStruct icTargetCell = CellStruct::Empty;

					// Defensive save: check if our own OR an ALLIED ConYard, Superweapon,
					// or War Factory is under heavy attack (< 75% HP)
					for (auto const pFriendlyBld : BuildingClass::Array)
					{
						if (!pFriendlyBld || pFriendlyBld->InLimbo || !pFriendlyBld->IsAlive || !pFriendlyBld->Owner
							|| !pFriendlyBld->Type || pFriendlyBld->IsIronCurtained())
						{
							continue;
						}

						if (!pHouse->IsAlliedWith(pFriendlyBld->Owner) || pFriendlyBld->Owner->IsNeutral())
							continue;

						const bool isCriticalBld = pFriendlyBld->Type->ConstructionYard
							|| pFriendlyBld->Type->Factory == AbstractType::UnitType
							|| pFriendlyBld->Type->SuperWeapon != -1;

						if (isCriticalBld && pFriendlyBld->GetHealthPercentage() < 0.75)
						{
							icTargetCell = pFriendlyBld->GetMapCoords();

							break;
						}
					}

					// Offensive Invulnerability (Slot 55): cast Iron Curtain on our own
					// OR an ALLIED attacking tank cluster near enemy
					if (icTargetCell == CellStruct::Empty && swType == SuperWeaponType::IronCurtain)
					{
						const int minCluster = (scores[55] > 120) ? 1 : 2;
						int bestClusterCount = 0;

						for (auto const pU : UnitClass::Array)
						{
							if (!pU || !pU->Owner || !pHouse->IsAlliedWith(pU->Owner) || pU->Owner->IsNeutral()
								|| pU->InLimbo || !pU->IsAlive || !pU->Type || pU->IsIronCurtained())
							{
								continue;
							}

							if (pU->Type->Harvester || pU->Type->ResourceGatherer || pU->Type->Organic)
								continue;

							const bool isEngagingEnemy = (pU->Target != nullptr)
								|| (pVisibleEnemyBld && pU->DistanceFrom(pVisibleEnemyBld) <= 4096);

							if (!isEngagingEnemy)
								continue;

							const CellStruct uCell = pU->GetMapCoords();
							int neighbors = 0;

							for (auto const pOtherU : UnitClass::Array)
							{
								if (!pOtherU || !pOtherU->Owner || !pHouse->IsAlliedWith(pOtherU->Owner)
									|| pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Type)
								{
									continue;
								}

								if (pOtherU->Type->Harvester || pOtherU->Type->Organic)
									continue;

								const CellStruct oCell = pOtherU->GetMapCoords();

								if (std::abs(uCell.X - oCell.X) <= 1 && std::abs(uCell.Y - oCell.Y) <= 1)
									neighbors++;
							}

							if (neighbors >= minCluster && neighbors > bestClusterCount)
							{
								bestClusterCount = neighbors;

								icTargetCell = uCell;
							}
						}
					}

					if (icTargetCell != CellStruct::Empty)
					{
						pSuper->Launch(icTargetCell, false);
						pSuper->Reset();

						break;
					}

					continue;
				}

				// B) CHRONOSPHERE + CHRONOWARP:
				//    Teleport Iron-Curtained tank squad or Expansion MCV to enemy
				// base / island Teleport Enemy Player A's tanks into Enemy Player
				// B's base to cause mutual attrition in FFA Drown Enemy Ground
				// Tanks/MCVs by teleporting them into Water Destroy Enemy Naval
				// Warships by teleporting them onto dry Land
				if (swType == SuperWeaponType::ChronoSphere && pChronoWarp)
				{
					CellStruct chronoSrc = CellStruct::Empty;
					CellStruct chronoDst = CellStruct::Empty;

					// Find a known water cell (for drowning enemy ground tanks) and a
					// known clear land cell (for beaching enemy ships)
					CellStruct knownWaterCell = CellStruct::Empty;
					CellStruct knownDryLandCell = CellStruct::Empty;

					if (homeCoords != CellStruct::Empty)
					{
						for (int dy = -40; dy <= 40; dy += 4)
						{
							for (int dx = -40; dx <= 40; dx += 4)
							{
								const CellStruct tc { static_cast<short>(homeCoords.X + dx),
									static_cast<short>(homeCoords.Y + dy) };

								auto const pC = MapClass::Instance.TryGetCellAt(tc);

								if (!pC || !IsCellVisibleToAI(pHouse, pC) || pC->GetBuilding() != nullptr)
									continue;

								if (knownWaterCell == CellStruct::Empty && pC->LandType == LandType::Water
									&& !pC->ContainsBridge())
								{
									knownWaterCell = tc;
								}

								if (knownDryLandCell == CellStruct::Empty && pC->LandType == LandType::Clear
									&& pC->FirstObject == nullptr && std::abs(dx) >= 12)
								{
									knownDryLandCell = tc;
								}
							}
						}
					}

					// Mode 1: Teleport our own Iron-Curtained tank squad or distant
					// Expansion MCV directly to the target beachhead!
					for (auto const pU : UnitClass::Array)
					{
						if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
							continue;

						const bool isExpansionMCV = (pU->Type->DeploysInto && pU->Type->DeploysInto->ConstructionYard
							&& pHouse->NumConYards >= 1);

						const bool isIronCurtainedTank
							= (pU->IsIronCurtained() && !pU->Type->Harvester && !pU->Type->Organic);

						if ((isIronCurtainedTank || isExpansionMCV) && pWaveTargetCell)
						{
							CellClass* pDropCell = FindSafeIslandLandingCell(
								pHouse, pWaveTargetCell->MapCoords, targetIslandZone, isExpansionMCV);

							if (pDropCell && pU->DistanceFrom(pDropCell) >= 5120)
							{
								chronoSrc = pU->GetMapCoords();

								chronoDst = pDropCell->MapCoords;

								break;
							}
						}
					}

					// Mode 2, 3 & 4: Offensive Chronoshift against visible enemy
					// vehicles/ships
					if (chronoSrc == CellStruct::Empty)
					{
						// Check if there is a SECOND rival enemy house with a visible
						// building (for Enemy-vs-Enemy FFA attrition!)
						CellStruct rivalEnemyBaseCell = CellStruct::Empty;
						int rivalEnemyHouseIdx = -1;

						for (auto const pBld : BuildingClass::Array)
						{
							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type
								|| pBld->Type->Immune)
							{
								continue;
							}

							auto const pOwn = pBld->Owner;

							if (pOwn == pHouse || pHouse->IsAlliedWith(pOwn) || pOwn->IsNeutral() || pOwn->IsObserver()
								|| pOwn->Defeated)
							{
								continue;
							}

							if (pOwn->ArrayIndex != pHouse->EnemyHouseIndex && pBld->GetCell()
								&& IsCellVisibleToAI(pHouse, pBld->GetCell()))
							{
								CellClass* pAdj = FindSafeIslandLandingCell(pHouse, pBld->GetMapCoords(), -1, false);

								if (pAdj)
								{
									rivalEnemyBaseCell = pAdj->MapCoords;

									rivalEnemyHouseIdx = pOwn->ArrayIndex;

									break;
								}
							}
						}

						for (auto const pEnemyU : UnitClass::Array)
						{
							if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
								continue;

							if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner)
								|| pEnemyU->Owner->IsNeutral())
							{
								continue;
							}

							if (pEnemyU->IsIronCurtained() || pEnemyU->Type->Teleporter || pEnemyU->Type->Organic)
								continue;

							auto const pECell = pEnemyU->GetCell();

							if (!pECell || !IsCellVisibleToAI(pHouse, pECell))
								continue;

							const bool isPureGroundUnit = !pEnemyU->Type->Naval
								&& pEnemyU->Type->SpeedType != SpeedType::Hover
								&& pEnemyU->Type->MovementZone != MovementZone::Amphibious;

							const bool isPureNavalShip = pEnemyU->Type->Naval
								&& pEnemyU->Type->SpeedType != SpeedType::Hover
								&& pEnemyU->Type->MovementZone != MovementZone::Amphibious;

							// Mode 2: Pit Enemy A's ground tanks directly inside Enemy B's
							// base to trigger mutual attrition!
							if (isPureGroundUnit && rivalEnemyBaseCell != CellStruct::Empty
								&& pEnemyU->Owner->ArrayIndex != rivalEnemyHouseIdx)
							{
								chronoSrc = pEnemyU->GetMapCoords();

								chronoDst = rivalEnemyBaseCell;

								break;
							}

							// Mode 3 (Slot 55): Drown Enemy ground tanks/MCVs in water!
							const int drownMinCost = std::clamp(700 - static_cast<int>(scores[55]), 450, 950);

							if (isPureGroundUnit && knownWaterCell != CellStruct::Empty
								&& pEnemyU->Type->Cost >= drownMinCost)
							{
								chronoSrc = pEnemyU->GetMapCoords();

								chronoDst = knownWaterCell;

								break;
							}

							// Mode 4: Beach Enemy naval warships onto dry land!
							if (isPureNavalShip && knownDryLandCell != CellStruct::Empty)
							{
								chronoSrc = pEnemyU->GetMapCoords();

								chronoDst = knownDryLandCell;

								break;
							}
						}
					}

					if (chronoSrc != CellStruct::Empty && chronoDst != CellStruct::Empty)
					{
						pSuper->Launch(chronoSrc, false);
						pSuper->Reset();
						pSuper->ChronoMapCoords = chronoSrc;
						pChronoWarp->ChronoMapCoords = chronoSrc;
						pChronoWarp->Launch(chronoDst, false);
						pChronoWarp->Reset();

						break;
					}

					continue;
				}

				// C) YURI GENETIC MUTATOR: Mutate enemy infantry clusters into Brutes!
				if (swType == SuperWeaponType::GeneticMutator)
				{
					CellStruct bestMutateCell = CellStruct::Empty;
					int bestInfCluster = 1;

					for (auto const pEnemyInf : InfantryClass::Array)
					{
						if (!pEnemyInf || pEnemyInf->InLimbo || !pEnemyInf->IsAlive || !pEnemyInf->Owner
							|| !pEnemyInf->Type)
						{
							continue;
						}

						if (pEnemyInf->Owner == pHouse || pHouse->IsAlliedWith(pEnemyInf->Owner)
							|| pEnemyInf->Owner->IsNeutral())
						{
							continue;
						}

						if (pEnemyInf->GetCell() && IsCellVisibleToAI(pHouse, pEnemyInf->GetCell()))
						{
							const CellStruct ic = pEnemyInf->GetMapCoords();
							int cnt = 0;

							for (auto const pOtherInf : InfantryClass::Array)
							{
								if (!pOtherInf || pOtherInf->InLimbo || !pOtherInf->IsAlive || !pOtherInf->Owner)
									continue;

								if (pOtherInf->Owner == pHouse || pHouse->IsAlliedWith(pOtherInf->Owner))
									continue;

								const CellStruct oc = pOtherInf->GetMapCoords();

								if (std::abs(ic.X - oc.X) <= 1 && std::abs(ic.Y - oc.Y) <= 1)
									cnt++;
							}

							if (cnt > bestInfCluster)
							{
								bestInfCluster = cnt;

								bestMutateCell = ic;
							}
						}
					}

					if (bestMutateCell != CellStruct::Empty)
					{
						pSuper->Launch(bestMutateCell, false);
						pSuper->Reset();

						break;
					}
				}

				// D) YURI PSYCHIC DOMINATOR: Permanently capture clusters of expensive
				// enemy tanks/MCVs while crushing nearby structures!
				if (swType == SuperWeaponType::PsychicDominator)
				{
					CellStruct bestDomCell = CellStruct::Empty;
					int bestDomValue = 1200;

					for (auto const pEnemyU : UnitClass::Array)
					{
						if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
							continue;

						if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner)
							|| pEnemyU->Owner->IsNeutral())
						{
							continue;
						}

						if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell())
							&& !pEnemyU->Type->ImmuneToPsionics)
						{
							const CellStruct uc = pEnemyU->GetMapCoords();
							int val = 0;

							for (auto const pOtherU : UnitClass::Array)
							{
								if (!pOtherU || pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Owner
									|| !pOtherU->Type)
								{
									continue;
								}

								if (pOtherU->Owner == pHouse || pHouse->IsAlliedWith(pOtherU->Owner))
									continue;

								const CellStruct oc = pOtherU->GetMapCoords();

								if (std::abs(uc.X - oc.X) <= 1 && std::abs(uc.Y - oc.Y) <= 1)
									val += pOtherU->Type->Cost;
							}

							if (val > bestDomValue)
							{
								bestDomValue = val;

								bestDomCell = uc;
							}
						}
					}

					if (bestDomCell != CellStruct::Empty)
					{
						pSuper->Launch(bestDomCell, false);
						pSuper->Reset();

						break;
					}
				}

				// E) Reconnaissance Superweapons (Psychic Reveal / Spy Plane): Uncover
				// shrouded enemy base candidates!
				if (swType == SuperWeaponType::PsychicReveal || swType == SuperWeaponType::SpyPlane)
				{
					const CellStruct reconCell = pFallbackEnemyCell
						? pFallbackEnemyCell->MapCoords
						: (pVisibleEnemyBld ? pVisibleEnemyBld->GetMapCoords() : CellStruct::Empty);
					if (reconCell != CellStruct::Empty)
					{
						pSuper->Launch(reconCell, false);
						pSuper->Reset();
						break;
					}

					continue;
				}

				// F) Standard Offensive/Support Superweapons (Nuke, Lightning Storm,
				// Ion Cannon, ParaDrop, etc.)
				if (pVisibleEnemyBld)
				{
					pSuper->Launch(pVisibleEnemyBld->GetMapCoords(), false);
					pSuper->Reset();

					break;
				}
			}
		}
	}
}

namespace
{
	// Evaluates buildability strictly under Human Player rules:
	// - Requires valid human TechLevel (0..10), excluding TechLevel=-1 AI script
	// hacks/limbo dummies and TechLevel=11 disabled types
	// - Excludes legacy *_AI hack clones and TEST_BUILDING
	// - Temporarily sets IsHumanPlayer=true and AIBuildThis=true during
	// HouseClass::CanBuild so Ares/YRpp enforce real human Prerequisites,
	// Prerequisite.Negative, Owner/ForbiddenHouses, BuildLimit, and
	// PowersUpBuilding instead of AIBuildThis/PrerequisiteOverride
	bool CanBuildUnderHumanRules(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		if (!pHouse || !pType)
			return false;

		if (pType->TechLevel < 0 || pType->TechLevel > 10 || pType->TechLevel > pHouse->TechLevel)
			return false;

		const char* id = pType->ID;

		if (!id || !id[0])
			return false;

		const size_t idLen = std::strlen(id);

		if (idLen > 3 && _stricmp(id + idLen - 3, "_AI") == 0)
			return false;

		if (_stricmp(id, "TEST_BUILDING") == 0 || std::strstr(id, "_FAKE") != nullptr)
			return false;

		auto const pBld = abstract_cast<BuildingTypeClass*>(pType);

		if (pBld && pBld->SuperWeapon >= 0 && pBld->SuperWeapon < SuperWeaponTypeClass::Array.Count)
		{
			if (RulesClass::Instance->BuildTech.FindItemIndex(pBld) == -1)
			{
				if (auto const pSW = SuperWeaponTypeClass::Array.GetItem(pBld->SuperWeapon))
				{
					if (!IsTunnelNetworkBuilding(pBld))
					{
						if (auto const pSWExt = SWTypeExt::Fetch(pSW))
						{
							if (!pSWExt->SW_AllowAI)
								return false; // Respects SW01_Computers.ini .. SW14_Computers.ini

							// (SW.AllowAI=no)
						}
					}
				}
			}
		}

		// Evaluate full Phobos/Ares human prerequisites: [GenericPrerequisites],
		// Prerequisite<Name>Alternate, Prerequisite.Lists, Prerequisite.Negative,
		// Owner/RequiredHouses/ForbiddenHouses (with ParentCountry),
		// ConsideredSecretLabTech, BuildLimit/BuildLimitGroup, and PowersUpBuilding /
		// PowersUp.Buildings parent upgrade slots.
		if (!HouseExt::PrerequisitesMet(pHouse, pType))
			return false;

		const bool prevHuman = pHouse->IsHumanPlayer;
		const bool prevAIBuild = pBld ? pBld->AIBuildThis : true;

		pHouse->IsHumanPlayer = true;

		if (pBld)
			pBld->AIBuildThis = true;

		const CanBuildResult res = pHouse->CanBuild(pType, false, false);

		pHouse->IsHumanPlayer = prevHuman;

		if (pBld)
			pBld->AIBuildThis = prevAIBuild;

		return res == CanBuildResult::Buildable;
	}
} // namespace

int NeuralAI::UpdateExpertAI(HouseClass* pHouse)
{
	if (!NeuralAI::IsEnabledForHouse(pHouse))
		return 30;

	if (!NeuralAI::IsCampaignRestricted(pHouse))
	{
		pHouse->Production = true;

		pHouse->AIState_1E4 = 0;
	}

	auto const pExt = HouseExt::Fetch(pHouse);
	const int diffIdx = pHouse->GetAIDifficultyIndex();
	const int evalDelay = (diffIdx == 0) ? 15 : ((diffIdx == 1) ? 45 : 105);

	std::array<int8_t, InputVectorSize> features;

	std::array<int16_t, OutputVectorSize> scores;

	NeuralAI::ExtractFogFilteredFeatures(pHouse, features);

	NeuralAI::RunDeterministicMoE(pHouse, features, scores);

	pExt->AIBrain_LastInferenceFrame = Unsorted::CurrentFrame;

	if (diffIdx <= 1)
		NeuralAI::ExecuteTacticalAndSpecOpsMicro(pHouse, scores);

	// Select next targets for Building, Unit, Infantry, and Aircraft queues
	const int money = pHouse->Available_Money();
	const int rawParentIdx = pHouse->Type ? pHouse->Type->FindParentCountryIndex() : -1;
	const int parentCountryIdx = (rawParentIdx >= 0) ? rawParentIdx : (pHouse->Type ? pHouse->Type->ArrayIndex2 : -1);
	const unsigned int ownHouseBit = (parentCountryIdx >= 0 && parentCountryIdx < 32) ? (1u << parentCountryIdx) : 0u;

	// Building Target Selection
	if (NeuralAI::IsCampaignRestricted(pHouse))
	{
		// Campaign restriction: only build what is in Base.BaseNodes
		pExt->AIBrain_NextBuildingTarget = -1;

		for (int i = 0; i < pHouse->Base.BaseNodes.Count; ++i)
		{
			const auto& node = pHouse->Base.BaseNodes.Items[i];

			if (!node.Placed && node.BuildingTypeIndex >= 0 && node.BuildingTypeIndex < BuildingTypeClass::Array.Count)
			{
				auto const pBldType = BuildingTypeClass::Array.GetItem(node.BuildingTypeIndex);

				if (pBldType && pHouse->CanBuild(pBldType, false, false) == CanBuildResult::Buildable)
				{
					pExt->AIBrain_NextBuildingTarget = node.BuildingTypeIndex;

					break;
				}
			}
		}
	}
	else
	{
		int bestBldIdx = -1;
		int bestBldScore = -32000;
		int bestPowerPlantIdx = -1;
		int bestPowerPlantScore = -32000;

		// Count current owned macro & defense structures so the AI builds a
		// balanced macro base (Power -> Refinery -> Barracks -> War Factory -> 2nd
		// Refinery -> Radar -> Battle Lab -> Superweapons/High-Tier Defenses) and
		// NEVER spams an endless sea of cheap Tier-1 Pillboxes/Turrets or Power
		// Plants!
		int ownedRefineries = 0;
		int ownedBarracks = 0;
		int ownedWarFactories = 0;
		int ownedShipyards = 0;
		int ownedAirpads = 0;
		int ownedTechStructures = 0;
		int totalGroundDefenses = 0;
		int totalAADefenses = 0;
		int ownedBattleBunkers = 0;
		int emptyOwnedBattleBunkers = 0;
		int ownedTankBunkers = 0;
		int emptyOwnedTankBunkers = 0;
		int compatibleOccupierInfCount = 0;
		int compatibleBunkerableTankCount = 0;
		int ownedHarvestersCount = 0;
		bool hasTechCenter = false;
		int unfilledBioReactorSlots = 0;
		int bioReactorPotentialPower = 0;

		for (auto const pOwnInf : InfantryClass::Array)
		{
			if (pOwnInf && pOwnInf->Owner == pHouse && !pOwnInf->InLimbo && pOwnInf->IsAlive && pOwnInf->Type
				&& pOwnInf->Type->Occupier && !pOwnInf->Type->Engineer && !pOwnInf->Type->Infiltrate)
			{
				compatibleOccupierInfCount++;
			}
		}

		for (auto const pOwnU : UnitClass::Array)
		{
			if (pOwnU && pOwnU->Owner == pHouse && !pOwnU->InLimbo && pOwnU->IsAlive && pOwnU->Type
				&& pOwnU->Type->Bunkerable && !pOwnU->Type->Naval && !pOwnU->Type->Harvester
				&& !pOwnU->Type->ResourceGatherer && pOwnU->BunkerLinkedItem == nullptr)
			{
				compatibleBunkerableTankCount++;
			}
		}

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			auto const pOT = pOwnBld->Type;

			if (pOT->InfantryAbsorb && (pOT->PowerBonus > 0 || pOT->ExtraPowerBonus > 0)
				&& !IsTunnelNetworkBuilding(pOT) && pOwnBld->Passengers.NumPassengers < pOT->Passengers)
			{
				const int freeSlots = pOT->Passengers - pOwnBld->Passengers.NumPassengers;
				unfilledBioReactorSlots += freeSlots;
				bioReactorPotentialPower
					+= freeSlots * std::max(50, pOT->ExtraPowerBonus > 0 ? pOT->ExtraPowerBonus : 100);
			}

			if (pOT->Refinery || pOT->ResourceGatherer || pOT->ResourceDestination)
			{
				ownedRefineries++;
			}
			else if (pOT->Factory == AbstractType::InfantryType)
			{
				ownedBarracks++;
			}
			else if (pOT->Factory == AbstractType::UnitType && !pOT->Naval)
			{
				ownedWarFactories++;
			}
			else if (pOT->Factory == AbstractType::UnitType && pOT->Naval)
			{
				ownedShipyards++;
			}
			else if (pOT->Factory == AbstractType::AircraftType || pOT->Helipad)
			{
				ownedAirpads++;
			}
			else if (pOT->IsBaseDefense && !IsTunnelNetworkBuilding(pOT))
			{
				const auto pWS = pOT->GetWeapon(0);
				const auto pW = pWS ? pWS->WeaponType : nullptr;
				const bool isUnarmedBunker = (pOT->CanBeOccupied || pOT->Bunker) && (pW == nullptr || pW->Damage <= 0);

				if (pOT->CanBeOccupied && isUnarmedBunker)
				{
					ownedBattleBunkers++;

					if (pOwnBld->Occupants.Count == 0)
						emptyOwnedBattleBunkers++;
				}

				if (pOT->Bunker && isUnarmedBunker)
				{
					ownedTankBunkers++;

					if (pOwnBld->BunkerLinkedItem == nullptr)
						emptyOwnedTankBunkers++;
				}

				const bool isPureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;

				if (isPureAA)
				{
					totalAADefenses++;
				}
				else if (!isUnarmedBunker || pOwnBld->Occupants.Count > 0 || pOwnBld->BunkerLinkedItem != nullptr)
				{
					totalGroundDefenses++;
				}
			}
			else if (pOT->SpySat || pOT->OrePurifier
				|| (pOT->PowerBonus <= pOT->PowerDrain && !pOT->ConstructionYard && pOT->Storage == 0
					&& !pOT->UnitRepair))
			{
				ownedTechStructures++;

				if ((RulesClass::Instance && RulesClass::Instance->BuildTech.FindItemIndex(pOT) != -1)
					|| (pOT->TechLevel >= 6 && pOT->Cost >= 1200))
				{
					hasTechCenter = true;
				}
			}
		}

		// Also count mobile Slave Miners (e.g. Yuri's SMIN when undeployed from
		// YAREFN to harvest ore) as owned Refineries, and count active Harvesters
		for (auto const pOwnUnit : UnitClass::Array)
		{
			if (!pOwnUnit || pOwnUnit->Owner != pHouse || pOwnUnit->InLimbo || !pOwnUnit->IsAlive || !pOwnUnit->Type)
				continue;

			if (pOwnUnit->Type->Harvester || pOwnUnit->Type->ResourceGatherer)
				ownedHarvestersCount++;

			if (auto const pDeploys = pOwnUnit->Type->DeploysInto)
			{
				if (pDeploys->Refinery || pDeploys->ResourceGatherer || pDeploys->ResourceDestination)
				{
					ownedRefineries++;

					ownedHarvestersCount++;
				}
			}
		}

		const int houseStagger = (std::max(0, pHouse->ArrayIndex) % 8) * 31;

		// Sell any temporary Harvester Recovery Refinery once its buildup finishes
		// and its FreeUnit Harvester has spawned (from AdvAI)
		if ((Unsorted::CurrentFrame + houseStagger) % 15 == 0
			&& !GetMacroState(pHouse).TemporaryRecoveryRefineryPlacedFrame.empty())
		{
			auto& tempMap = GetMacroState(pHouse).TemporaryRecoveryRefineryPlacedFrame;
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo)
					continue;

				auto const it = tempMap.find(pBld->UniqueID);
				if (it == tempMap.end())
					continue;

				if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
				{
					tempMap.erase(it);
					break;
				}

				if (pBld->CurrentMission != Mission::Construction && pBld->QueuedMission != Mission::Construction
					&& pBld->BState != static_cast<int>(BStateType::Construction)
					&& Unsorted::CurrentFrame >= it->second + 75)
				{
					for (auto const pUnit : UnitClass::Array)
					{
						if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type
							&& (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer))
						{
							if (pUnit->GetMapCoords().DistanceFrom(pBld->GetMapCoords()) <= 8.0
								&& pUnit->CurrentMission != Mission::Harvest)
							{
								pUnit->QueueMission(Mission::Harvest, false);
							}
						}
					}

					tempMap.erase(it);
					GetMacroState(pHouse).NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 750;
					Debug::Log("[NeuralAI] HarvesterRecovery: House %d selling temporary "
							"recovery refinery %s at (%d,%d) after spawning FreeUnit "
							"harvester.\n",
						pHouse->ArrayIndex, pBld->Type->ID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y);
					pBld->Sell(1);
					break;
				}
			}
		}

		// Periodic Obsolete Refinery Recycling & Emergency Cash Recovery (from
		// AdvAI)
		if (Unsorted::CurrentFrame > pExt->LastObsoleteRefineryCheckFrame + 450 + houseStagger)
		{
			pExt->LastObsoleteRefineryCheckFrame = Unsorted::CurrentFrame;
			RecycleObsoleteRefineries(pHouse);
			RaiseEmergencyMoneyForRefinery(pHouse, ownedRefineries, ownedHarvestersCount, money);
		}

		// Expert 1 Selective Wall Unblocking & Demolition:
		// - If an obstructing wall belongs to the AI house, sell that specific wall cell/segment.
		// - If an obstructing wall is neutral or enemy and is destructible (!Immune), dispatch a
		//   nearby friendly unit/infantry with a wall-capable warhead (or crusher) to demolish it!
		if ((Unsorted::CurrentFrame + houseStagger) % 45 == 0)
		{
			auto canFootDemolishWall = [](FootClass* pFoot, bool wallCrushable, Armor wallArmor) -> bool
			{
				if (!pFoot || !pFoot->IsAlive || pFoot->InLimbo)
					return false;

				if (auto const pU = abstract_cast<UnitClass*>(pFoot))
				{
					if (pU->Type && (pU->Type->Harvester || pU->Type->ResourceGatherer))
						return false;

					if (wallCrushable && pU->Type && pU->Type->Crusher)
						return true;
				}

				for (int wIdx = 0; wIdx < 2; ++wIdx)
				{
					auto const pWS = pFoot->GetWeapon(wIdx);
					auto const pWpn = pWS ? pWS->WeaponType : nullptr;
					if (!pWpn || pWpn->Damage <= 0 || !pWpn->Warhead)
						continue;

					auto const pWH = pWpn->Warhead;
					if (pWH->Wall || pWH->WallAbsoluteDestroyer || (wallArmor == Armor::Wood && pWH->Wood))
						return true;
				}

				return false;
			};

			auto dispatchWallDemolition = [&](AbstractClass* pTarget, const CellStruct& wc, bool wallCrushable,
											Armor wallArmor, FootClass* pBlockedFoot) -> bool
			{
				if (pBlockedFoot && canFootDemolishWall(pBlockedFoot, wallCrushable, wallArmor))
				{
					if (pBlockedFoot->Target != pTarget)
					{
						pBlockedFoot->SetTarget(pTarget);
						pBlockedFoot->QueueMission(Mission::Attack, true);
					}

					return true;
				}

				FootClass* pBestDemo = nullptr;
				int bestDistSq = 14 * 14 + 1;

				for (auto const pOwnU : UnitClass::Array)
				{
					if (!pOwnU || pOwnU->Owner != pHouse || !canFootDemolishWall(pOwnU, wallCrushable, wallArmor))
						continue;

					if (pOwnU->Target == pTarget)
						return true;

					if (pOwnU->Target != nullptr || pOwnU->Destination != nullptr
						|| (pOwnU->CurrentMission != Mission::Guard && pOwnU->CurrentMission != Mission::Area_Guard))
					{
						continue;
					}

					const CellStruct uc = pOwnU->GetMapCoords();
					const int dx = static_cast<int>(uc.X) - static_cast<int>(wc.X);
					const int dy = static_cast<int>(uc.Y) - static_cast<int>(wc.Y);
					const int dSq = dx * dx + dy * dy;
					if (dSq < bestDistSq)
					{
						bestDistSq = dSq;
						pBestDemo = pOwnU;
					}
				}

				for (auto const pOwnI : InfantryClass::Array)
				{
					if (!pOwnI || pOwnI->Owner != pHouse || !canFootDemolishWall(pOwnI, false, wallArmor))
						continue;

					if (pOwnI->Target == pTarget)
						return true;

					if (pOwnI->Target != nullptr || pOwnI->Destination != nullptr
						|| (pOwnI->CurrentMission != Mission::Guard && pOwnI->CurrentMission != Mission::Area_Guard))
					{
						continue;
					}

					const CellStruct ic = pOwnI->GetMapCoords();
					const int dx = static_cast<int>(ic.X) - static_cast<int>(wc.X);
					const int dy = static_cast<int>(ic.Y) - static_cast<int>(wc.Y);
					const int dSq = dx * dx + dy * dy;
					if (dSq < bestDistSq)
					{
						bestDistSq = dSq;
						pBestDemo = pOwnI;
					}
				}

				if (pBestDemo)
				{
					pBestDemo->SetTarget(pTarget);
					pBestDemo->QueueMission(Mission::Attack, true);

					return true;
				}

				return false;
			};

			auto tryUnblockWallAtCell = [&](const CellStruct& wc, FootClass* pBlockedFoot) -> bool
			{
				auto const pWCell = MapClass::Instance.TryGetCellAt(wc);
				if (!pWCell)
					return false;

				if (pWCell->OverlayTypeIndex >= 0)
				{
					if (auto const pOv = OverlayTypeClass::Array.GetItem(pWCell->OverlayTypeIndex))
					{
						if (pOv->Wall)
						{
							if (pWCell->WallOwnerIndex == pHouse->ArrayIndex)
							{
								CellStruct wallCell = wc;
								pHouse->SellWall(wallCell, true);

								return true;
							}

							const bool isAlliedWall = (pWCell->WallOwnerIndex >= 0
								&& pWCell->WallOwnerIndex < HouseClass::Array.Count
								&& pHouse->IsAlliedWith(HouseClass::Array.GetItem(pWCell->WallOwnerIndex)));
							if (!isAlliedWall && !pOv->Immune)
							{
								if (dispatchWallDemolition(pWCell, wc, pOv->Crushable, pOv->Armor, pBlockedFoot))
									return true;
							}
						}
					}
				}

				if (auto const pWBld = pWCell->GetBuilding())
				{
					if (pWBld->IsAlive && !pWBld->InLimbo && pWBld->Type
						&& (pWBld->Type->Wall || pWBld->Type->FirestormWall || pWBld->Type->LaserFence
							|| pWBld->Type->LaserFencePost))
					{
						if (pWBld->Owner == pHouse && pWBld->CurrentMission != Mission::Selling)
						{
							pWBld->Sell(1);

							return true;
						}

						if (pWBld->Owner != pHouse && (!pWBld->Owner || !pHouse->IsAlliedWith(pWBld->Owner))
							&& !pWBld->Type->Immune)
						{
							if (dispatchWallDemolition(pWBld, wc, false, pWBld->Type->Armor, pBlockedFoot))
								return true;
						}
					}
				}

				return false;
			};

			bool unblockedOneWall = false;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (unblockedOneWall || !pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
					continue;

				if (pOwnB->Type->Factory == AbstractType::None && !pOwnB->Type->Refinery
					&& !pOwnB->Type->ResourceDestination && !pOwnB->Type->UnitRepair)
				{
					continue;
				}

				const CellStruct bc = pOwnB->GetMapCoords();
				const int bw = pOwnB->Type->GetFoundationWidth();
				const int bh = pOwnB->Type->GetFoundationHeight(false);
				for (int dy = -1; dy <= bh + 1 && !unblockedOneWall; ++dy)
				{
					for (int dx = -1; dx <= bw + 1 && !unblockedOneWall; ++dx)
					{
						if (dx >= 0 && dx < bw && dy >= 0 && dy < bh)
							continue;

						if (tryUnblockWallAtCell(
								CellStruct { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) }, nullptr))
						{
							unblockedOneWall = true;
						}
					}
				}
			}

			for (auto const pOwnU : UnitClass::Array)
			{
				if (unblockedOneWall || !pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive
					|| !pOwnU->Destination)
				{
					continue;
				}

				if (pOwnU->CurrentMission != Mission::Move && pOwnU->CurrentMission != Mission::Harvest
					&& pOwnU->CurrentMission != Mission::AttackMove && pOwnU->CurrentMission != Mission::Enter)
				{
					continue;
				}

				const CellStruct uc = pOwnU->GetMapCoords();
				const CoordStruct destC = pOwnU->Destination->GetCoords();
				const CellStruct dc = CellClass::Coord2Cell(destC);
				const int stepX = std::clamp(static_cast<int>(dc.X) - static_cast<int>(uc.X), -1, 1);
				const int stepY = std::clamp(static_cast<int>(dc.Y) - static_cast<int>(uc.Y), -1, 1);
				if (stepX != 0 || stepY != 0)
				{
					const CellStruct aheadCell { static_cast<short>(uc.X + stepX), static_cast<short>(uc.Y + stepY) };
					if (tryUnblockWallAtCell(aheadCell, pOwnU))
						unblockedOneWall = true;
				}
			}

			for (auto const pOwnI : InfantryClass::Array)
			{
				if (unblockedOneWall || !pOwnI || pOwnI->Owner != pHouse || pOwnI->InLimbo || !pOwnI->IsAlive
					|| !pOwnI->Destination)
				{
					continue;
				}

				if (pOwnI->CurrentMission != Mission::Move && pOwnI->CurrentMission != Mission::AttackMove
					&& pOwnI->CurrentMission != Mission::Enter)
				{
					continue;
				}

				const CellStruct ic = pOwnI->GetMapCoords();
				const CoordStruct destC = pOwnI->Destination->GetCoords();
				const CellStruct dc = CellClass::Coord2Cell(destC);
				const int stepX = std::clamp(static_cast<int>(dc.X) - static_cast<int>(ic.X), -1, 1);
				const int stepY = std::clamp(static_cast<int>(dc.Y) - static_cast<int>(ic.Y), -1, 1);
				if (stepX != 0 || stepY != 0)
				{
					const CellStruct aheadCell { static_cast<short>(ic.X + stepX), static_cast<short>(ic.Y + stepY) };
					if (tryUnblockWallAtCell(aheadCell, pOwnI))
						unblockedOneWall = true;
				}
			}
		}

		// Periodic Dynamic Primary Factory Switching (from AdvAI)
		if (Unsorted::CurrentFrame > pExt->LastPrimaryFactoryCheckFrame + 60 + houseStagger)
		{
			pExt->LastPrimaryFactoryCheckFrame = Unsorted::CurrentFrame;

			CellStruct frontTarget = pExt->AIBrain_AttackTargetCell;

			if (frontTarget == CellStruct::Empty && pHouse->EnemyHouseIndex >= 0
				&& pHouse->EnemyHouseIndex < HouseClass::Array.Count)
			{
				if (auto const pEnemy = HouseClass::Array.GetItem(pHouse->EnemyHouseIndex))
					frontTarget = pEnemy->BaseCenter != CellStruct::Empty ? pEnemy->BaseCenter : pEnemy->BaseSpawnCell;
			}

			UpdatePrimaryFactories(pHouse, frontTarget);
		}

		// Real Power Drain & Damaged-Plant Safety Cushion:
		// Maintain a generous buffer (+150 baseline, scaling to +300..+550 when
		// owning powered defenses/superweapons or large reactors) so 1 damaged or
		// destroyed Power Plant never plunges the base into LowPower!
		bool hasUnpoweredBuildings = false;
		int poweredDefAndSWCount = 0;
		int maxSinglePlantBonus = 0;
		bool hasAdvancedPowerPlants = false;

		const int realPowerDrain = GetRealPowerDrainAndGridMetrics(
			pHouse, hasUnpoweredBuildings, poweredDefAndSWCount, maxSinglePlantBonus, hasAdvancedPowerPlants);

		const int realPowerSurplus = pHouse->PowerOutput - realPowerDrain;
		const int plantLossBuffer = std::min(500, std::max(150, (maxSinglePlantBonus * 3) / 4));

		const int requiredPowerCushion = (poweredDefAndSWCount > 0)
			? std::max(plantLossBuffer, std::min(550, 220 + poweredDefAndSWCount * 45))
			: std::max(150, plantLossBuffer);

		// Competitive MultipleFactory Arms Race:
		// Measure strongest enemy's factory counts and scale our factory limits by
		// both economy (ownedRefineries) AND enemy production capacity!
		int maxEnemyWarFactories = 0;
		int maxEnemyBarracks = 0;
		int maxEnemyShipyards = 0;
		int maxEnemyAirpads = 0;

		for (auto const pOther : HouseClass::Array)
		{
			if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral()
				|| pOther->IsObserver() || pOther->Defeated)
			{
				continue;
			}

			maxEnemyWarFactories = std::max(maxEnemyWarFactories, pOther->NumWarFactories);

			maxEnemyBarracks = std::max(maxEnemyBarracks, pOther->NumBarracks);

			maxEnemyShipyards = std::max(maxEnemyShipyards, pOther->NumShipyards);

			maxEnemyAirpads = std::max(maxEnemyAirpads, pOther->NumAirpads);
		}

		const int ecoWarFactoryCap = (ownedRefineries >= 6) ? (ownedRefineries / 2) : (1 + ownedRefineries / 3);

		const int maxWarFactories = (hasTechCenter || money >= 2400)
			? std::clamp(std::max(ecoWarFactoryCap, maxEnemyWarFactories + 1), 2, 7)
			: std::clamp(ecoWarFactoryCap, 1, 3);

		const int ecoBarracksCap = (ownedRefineries >= 4) ? 2 : 1;

		const int maxBarracks = (hasTechCenter || money >= 2500)
			? std::clamp(std::max(ecoBarracksCap, maxEnemyBarracks), 2, 4)
			: std::clamp(ecoBarracksCap, 1, 2);

		const int maxShipyards
			= (hasTechCenter && maxEnemyShipyards >= 2) ? std::clamp(maxEnemyShipyards + 1, 2, 4) : 2;

		int ownedDockingAircraft = 0;
		for (auto const pOwnAir : AircraftClass::Array)
		{
			if (pOwnAir && pOwnAir->Owner == pHouse && !pOwnAir->InLimbo && pOwnAir->IsAlive && pOwnAir->Type
				&& !pOwnAir->SpawnOwner && (pOwnAir->Type->AirportBound || pOwnAir->Type->Dock.Count > 0))
			{
				ownedDockingAircraft++;
			}
		}

		const bool expert8NeedsMoreAirpadDocks = (ownedAirpads >= 1 && ownedDockingAircraft >= pHouse->AirportDocks
			&& (scores[32] > 160 || scores[33] > 160 || pExt->AIBrain_ActiveExpert == 8
				|| pExt->AIBrain_Archetype == 6));
		const int dockDemandAirpadCap = expert8NeedsMoreAirpadDocks ? std::clamp((ownedDockingAircraft / 4) + 2, 2, 6) : 2;
		const int maxAirpads = std::max(
			dockDemandAirpadCap, (hasTechCenter && maxEnemyAirpads >= 2) ? std::clamp(maxEnemyAirpads + 1, 2, 5) : 2);

		// Threat & Defense Moderation Analysis:
		// Prune stale attack records and check if our base was recently attacked
		PruneAttackedBuildingsLIFO(pHouse);

		auto& macroState = GetMacroState(pHouse);
		if (macroState.FrontlineThreatNeedsDefenses > 0
			&& Unsorted::CurrentFrame > macroState.FrontlineThreatActiveFrames)
		{
			macroState.FrontlineThreatNeedsDefenses = 0;
			macroState.FrontlineThreatCoords = CellStruct::Empty;
			macroState.FrontlineThreatBuildingCoords = CellStruct::Empty;
		}

		const bool frontlineThreatActive = (macroState.FrontlineThreatNeedsDefenses > 0
			&& Unsorted::CurrentFrame <= macroState.FrontlineThreatActiveFrames);

		const bool wasRecentlyAttacked = (pHouse->LATime > 0 && (Unsorted::CurrentFrame - pHouse->LATime) < 1350)
			|| !macroState.AttackedBuildingsLIFO.empty();

		bool underInfantryAttack = false;
		bool underVehicleAttack = false;
		bool underAirAttack = false;
		NeuralThreatCategory activeThreatCat = NeuralThreatCategory::None;

		for (int i = static_cast<int>(macroState.AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)
		{
			const auto& rec = macroState.AttackedBuildingsLIFO[i];
			if (activeThreatCat == NeuralThreatCategory::None)
				activeThreatCat = rec.Threat;

			if (rec.Threat == NeuralThreatCategory::Infantry)
				underInfantryAttack = true;
			else if (rec.Threat == NeuralThreatCategory::Vehicle)
				underVehicleAttack = true;
			else if (rec.Threat == NeuralThreatCategory::Air)
				underAirAttack = true;
		}

		const CellStruct homeRefCenter = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			? pHouse->ConYards.GetItem(0)->GetMapCoords()
			: (pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell);

		if (activeThreatCat == NeuralThreatCategory::None && (wasRecentlyAttacked || frontlineThreatActive))
		{
			const CellStruct threatCenter
				= (frontlineThreatActive && macroState.FrontlineThreatBuildingCoords != CellStruct::Empty)
				? macroState.FrontlineThreatBuildingCoords
				: homeRefCenter;
			activeThreatCat = GetActiveThreatCategory(pHouse, threatCenter);
			if (activeThreatCat == NeuralThreatCategory::Infantry)
				underInfantryAttack = true;
			else if (activeThreatCat == NeuralThreatCategory::Vehicle)
				underVehicleAttack = true;
			else if (activeThreatCat == NeuralThreatCategory::Air)
				underAirAttack = true;
		}

		const int enemyAirValue = CalculateEnemyAircraftValue(pHouse);
		const bool hasCoreEconomyAndProduction = (ownedRefineries >= 1 && ownedBarracks >= 1 && ownedWarFactories >= 1);

		const bool isBehindInFactories = hasCoreEconomyAndProduction
			&& ((ownedWarFactories < maxEnemyWarFactories && ownedRefineries >= 2) || (ownedRefineries < 2));

		const bool hasPendingCoreTechOrEco = !hasTechCenter || isBehindInFactories || (ownedRefineries < 2);

		// Detect remote captured Tech Buildings (Oil Derricks, Tech Outposts,
		// Hospitals, Airfields) & remote Expansion Refineries that are far from our
		// main base (> 16 cells) and currently lack a nearby point-defense turret
		// within 10 cells!
		int undefendedRemoteAssetsCount = 0;
		std::vector<CellStruct> scanOrigins;
		if (homeRefCenter != CellStruct::Empty)
			scanOrigins.push_back(homeRefCenter);

		for (int yi = 1; yi < pHouse->ConYards.Count; ++yi)
		{
			if (auto const pCY = pHouse->ConYards.GetItem(yi))
				scanOrigins.push_back(pCY->GetMapCoords());
		}

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->InvisibleInGame)
				continue;

			auto const pOT = pOwnBld->Type;
			const CellStruct bc = pOwnBld->GetMapCoords();
			const bool isCapturedTechOrCash = (pOT->ProduceCashAmount > 0 || pOT->ProduceCashStartup > 0
				|| (pOT->Capturable && pOT->NeedsEngineer));
			const bool isRemoteFromHome = (homeRefCenter != CellStruct::Empty
				&& std::max(std::abs(bc.X - homeRefCenter.X), std::abs(bc.Y - homeRefCenter.Y)) > 16);

			if ((isCapturedTechOrCash || isRemoteFromHome)
				&& (pOT->BaseNormal || pOT->ConstructionYard || BuildingTypeExt::IsAIBaseNormal(pOT)))
			{
				bool nearExistingOrigin = false;
				for (const CellStruct& so : scanOrigins)
				{
					if (std::max(std::abs(bc.X - so.X), std::abs(bc.Y - so.Y)) <= 24)
					{
						nearExistingOrigin = true;
						break;
					}
				}

				if (!nearExistingOrigin && scanOrigins.size() < 5)
					scanOrigins.push_back(bc);
			}

			if (isCapturedTechOrCash
				|| (isRemoteFromHome && (pOT->Refinery || pOT->ConstructionYard || pOT->Factory != AbstractType::None)))
			{
				bool hasNearbyGroundTurret = false;
				for (auto const pDefBld : pHouse->Buildings)
				{
					if (!pDefBld || pDefBld->InLimbo || !pDefBld->IsAlive || !pDefBld->Type
						|| !pDefBld->Type->IsBaseDefense || IsTunnelNetworkBuilding(pDefBld->Type))
					{
						continue;
					}

					const auto pWS = pDefBld->Type->GetWeapon(0);
					const auto pW = pWS ? pWS->WeaponType : nullptr;
					const bool isEmptyUnarmedBunker = (pDefBld->Type->CanBeOccupied || pDefBld->Type->Bunker)
						&& (pW == nullptr || pW->Damage <= 0) && pDefBld->Occupants.Count == 0
						&& pDefBld->BunkerLinkedItem == nullptr;
					if (isEmptyUnarmedBunker)
						continue;

					const bool pureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;
					if (pureAA)
						continue;

					const CellStruct dc = pDefBld->GetMapCoords();
					if (std::max(std::abs(bc.X - dc.X), std::abs(bc.Y - dc.Y)) <= 10)
					{
						hasNearbyGroundTurret = true;
						break;
					}
				}

				if (!hasNearbyGroundTurret)
					undefendedRemoteAssetsCount++;
			}
		}

		const int defDensityMod = std::clamp(static_cast<int>(scores[14]) / 240, -1, 2);

		const int baseGroundDefCap = 1 + ownedRefineries + ownedWarFactories + ownedTechStructures
			+ undefendedRemoteAssetsCount + defDensityMod + (wasRecentlyAttacked ? 2 : 0)
			+ (frontlineThreatActive ? macroState.FrontlineThreatNeedsDefenses : 0);

		const int maxGroundDefenses = std::clamp(baseGroundDefCap, 2, (hasTechCenter && money > 4000) ? 14 : 11);
		const int baseMaxAA = 1 + ownedWarFactories + ownedTechStructures + defDensityMod + (underAirAttack ? 2 : 0);
		const int maxAADefenses = std::clamp(std::max(baseMaxAA, 1 + enemyAirValue / 30), 2, 8);

		// AdvAI & StarCraft Resource Expansion State Machine:
		// Measure minimum Chebyshev distance from our closest
		// BaseNormal/ConYard/CapturedTech anchor to the nearest uncolonized Ore/Gem
		// field (>= 10 cells from any owned Refinery).
		// - If minAnchorDistToUncolonizedOre <= 9: our BaseNormal chain, Captured
		// Tech Building, or Expansion ConYard has reached the new Ore field!
		// Trigger ShouldBuildRefinery (+1950)!
		// - If minAnchorDistToUncolonizedOre is 10..38: boost BaseNormal=yes
		// stepping-stone structures (Power Plants / Barracks / Outposts) so the
		// base actively crawls to reach the Ore!
		CellStruct nearestUncolonizedOreCell = CellStruct::Empty;
		int minAnchorDistToUncolonizedOre = 9999;
		for (const CellStruct& scanOrigin : scanOrigins)
		{
			if (scanOrigin == CellStruct::Empty)
				continue;

			const int scanRad = (&scanOrigin == &scanOrigins[0]) ? 78 : 42;
			for (int dy = -scanRad; dy <= scanRad; dy += 3)
			{
				for (int dx = -scanRad; dx <= scanRad; dx += 3)
				{
					const CellStruct tc { static_cast<short>(scanOrigin.X + dx),
						static_cast<short>(scanOrigin.Y + dy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(tc);
					if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
						continue;

					const int oreZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
					if (oreZone < 0)
						continue;

					bool alreadyColonizedByRef = false;
					int closestAnchorDist = 9999;
					for (auto const pOwnBld : pHouse->Buildings)
					{
						if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type
							|| pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
						{
							continue;
						}

						const CellStruct bc = pOwnBld->GetMapCoords();
						const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
						if (bz != oreZone)
							continue;

						const int dCheb = std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y));
						if ((pOwnBld->Type->Refinery || pOwnBld->Type->ResourceGatherer
								|| pOwnBld->Type->ResourceDestination)
							&& dCheb < 10)
						{
							alreadyColonizedByRef = true;
							break;
						}

						if (pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard
							|| BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
						{
							if (dCheb < closestAnchorDist)
								closestAnchorDist = dCheb;
						}
					}

					if (!alreadyColonizedByRef && closestAnchorDist < minAnchorDistToUncolonizedOre)
					{
						minAnchorDistToUncolonizedOre = closestAnchorDist;
						nearestUncolonizedOreCell = tc;
					}
				}
			}
		}

		bool conYardWithNoRefineryInZone = false;
		bool forwardConYardNeedsWarFactory = false;
		bool forwardConYardNeedsBarracks = false;
		bool forwardConYardNeedsDefense = false;
		const int spawnHomeZone = (pHouse->BaseSpawnCell != CellStruct::Empty)
			? MapClass::Instance.GetMovementZoneType(pHouse->BaseSpawnCell, MovementZone::Normal, false)
			: -1;
		for (int yi = 0; yi < pHouse->ConYards.Count; ++yi)
		{
			auto const pCY = pHouse->ConYards.GetItem(yi);
			if (!pCY || pCY->InLimbo || !pCY->IsAlive)
				continue;

			const CellStruct cyc = pCY->GetMapCoords();
			const int cyz = MapClass::Instance.GetMovementZoneType(cyc, MovementZone::Normal, false);
			if (cyz < 0)
				continue;

			const bool isRemoteOrNomadicCY = (yi >= 1)
				|| (pHouse->BaseSpawnCell != CellStruct::Empty
					&& std::max(std::abs(cyc.X - pHouse->BaseSpawnCell.X), std::abs(cyc.Y - pHouse->BaseSpawnCell.Y))
						> 16);
			if (!isRemoteOrNomadicCY)
				continue;

			int refsNearCY = 0;
			bool hasWarFacNearCY = false;
			bool hasBarracksNearCY = false;
			int defsNearCY = 0;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
					continue;

				const CellStruct bc = pOwnB->GetMapCoords();
				const int dCheb = std::max(std::abs(cyc.X - bc.X), std::abs(cyc.Y - bc.Y));
				if (dCheb <= 18 && MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false) == cyz)
				{
					if (pOwnB->Type->Refinery || pOwnB->Type->ResourceGatherer || pOwnB->Type->ResourceDestination)
					{
						refsNearCY++;
					}
					else if (pOwnB->Type->Factory == AbstractType::UnitType && !pOwnB->Type->Naval)
					{
						hasWarFacNearCY = true;
					}
					else if (pOwnB->Type->Factory == AbstractType::InfantryType)
					{
						hasBarracksNearCY = true;
					}
					else if (pOwnB->Type->IsBaseDefense && !IsTunnelNetworkBuilding(pOwnB->Type))
					{
						const auto pDWS = pOwnB->Type->GetWeapon(0);
						const auto pDW = pDWS ? pDWS->WeaponType : nullptr;
						const bool emptyUnarmedBunker = (pOwnB->Type->CanBeOccupied || pOwnB->Type->Bunker)
							&& (pDW == nullptr || pDW->Damage <= 0) && pOwnB->Occupants.Count == 0
							&& pOwnB->BunkerLinkedItem == nullptr;
						if (!emptyUnarmedBunker)
							defsNearCY++;
					}
				}
			}

			const bool hasRefNearCY = (refsNearCY >= 1);
			int oreCellsNearCY = 0;
			int tiberiumTreesNearCY = 0;
			for (int dy = -24; dy <= 24; dy += 3)
			{
				for (int dx = -24; dx <= 24; dx += 3)
				{
					const CellStruct tc { static_cast<short>(cyc.X + dx), static_cast<short>(cyc.Y + dy) };
					if (auto const pC = MapClass::Instance.TryGetCellAt(tc))
					{
						if (MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false) != cyz)
							continue;

						if (pC->GetContainedTiberiumValue() > 0)
							oreCellsNearCY++;

						if (auto const pTerr = pC->GetTerrain(false))
						{
							if (pTerr->Type && pTerr->Type->SpawnsTiberium)
								tiberiumTreesNearCY++;
						}
					}
				}
			}

			const bool hasOreNearCY = (oreCellsNearCY > 0 || tiberiumTreesNearCY > 0);
			const bool wantsSecondIslandRefinery = (refsNearCY == 1 && hasWarFacNearCY
				&& (tiberiumTreesNearCY >= 1 || oreCellsNearCY >= 8) && money >= 1400);

			bool enemyNearCY = false;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
					|| pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
				{
					continue;
				}

				const CellStruct ec = pEB->GetMapCoords();
				if (std::max(std::abs(cyc.X - ec.X), std::abs(cyc.Y - ec.Y)) <= 36
					|| MapClass::Instance.GetMovementZoneType(ec, MovementZone::Normal, false) == cyz)
				{
					enemyNearCY = true;
					break;
				}
			}

			if ((!hasRefNearCY && hasOreNearCY) || wantsSecondIslandRefinery)
				conYardWithNoRefineryInZone = true;

			if (enemyNearCY || (spawnHomeZone >= 0 && cyz != spawnHomeZone) || !hasOreNearCY)
			{
				if (!hasWarFacNearCY && ownedRefineries >= 1)
					forwardConYardNeedsWarFactory = true;

				if (!hasBarracksNearCY && ownedRefineries >= 1)
					forwardConYardNeedsBarracks = true;

				if (defsNearCY < 2 && (hasRefNearCY || hasWarFacNearCY || enemyNearCY))
					forwardConYardNeedsDefense = true;
			}
		}

		const bool reachedNewOreField
			= (conYardWithNoRefineryInZone
				|| (nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre <= 9))
			&& ownedRefineries >= 1;
		const bool needsSteppingStoneCrawlToOre = (GetMacroState(pHouse).ConsecutivePlacementFailures < 2
			&& nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre > 9
			&& minAnchorDistToUncolonizedOre <= 38 && hasCoreEconomyAndProduction
			&& (features[19] < 32 || ownedRefineries < 3 || scores[34] > 100 || Unsorted::CurrentFrame > 2100));

		// AdvAI Start-Rush Emergency Defense Interception:
		// If we are rushed early (CurrentFrame <= with at least 1 Refinery + 1 Barracks
		// and 0 ground defenses while our ConYard is constructing a non-essential building,
		// abandon the non-essential construction so the ConYard can immediately build a defense!
		if (Unsorted::CurrentFrame <= 10000 && ownedRefineries >= 1 && ownedBarracks >= 1 && totalGroundDefenses == 0
			&& (wasRecentlyAttacked || frontlineThreatActive))
		{
			if (FactoryClass* pBldFac
				= pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare))
			{
				if (auto const pInProgBld = abstract_cast<BuildingClass*>(pBldFac->Object))
				{
					auto const pInProgType = pInProgBld->Type;
					if (pInProgType && !pInProgType->IsBaseDefense && pInProgType->PowerBonus <= pInProgType->PowerDrain
						&& !pInProgType->Refinery && !pInProgType->ResourceGatherer && !pInProgType->ResourceDestination
						&& pInProgType->Factory != AbstractType::InfantryType
						&& (pInProgType->Factory != AbstractType::UnitType || pInProgType->Naval)
						&& pBldFac->GetProgress() < 42)
					{
						pBldFac->AbandonProduction();
					}
				}
			}
		}

		int inProgressBldIdx = -1;
		int inProgressDefIdx = -1;
		if (FactoryClass* pBldFac = pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare))
		{
			if (pBldFac->Object && pBldFac->Object->GetType())
				inProgressBldIdx = pBldFac->Object->GetType()->GetArrayIndex();
		}

		if (FactoryClass* pDefFac = pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::Combat))
		{
			if (pDefFac->Object && pDefFac->Object->GetType())
				inProgressDefIdx = pDefFac->Object->GetType()->GetArrayIndex();
		}

		int ownedCashBuildings = 0;
		int ownedSlaveMinerBuildings = 0;
		int staticRefineriesCount = 0;
		int orphanRefineryZoneNeedsReplacement = -1;
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
				continue;

			if (pOwnB->Type->ProduceCashAmount > 0)
				ownedCashBuildings++;

			if (pOwnB->Type->ResourceGatherer)
			{
				ownedSlaveMinerBuildings++;
			}
			else if ((pOwnB->Type->Refinery || pOwnB->Type->ResourceDestination) && pOwnB->Type->Enslaves == nullptr
				&& pOwnB->Type->UndeploysInto == nullptr && pOwnB->CurrentMission != Mission::Selling
				&& pOwnB->QueuedMission != Mission::Selling)
			{
				staticRefineriesCount++;
				const CellStruct rCoords = pOwnB->GetMapCoords();
				const int rz = MapClass::Instance.GetMovementZoneType(rCoords, MovementZone::Normal, false);
				if (rz >= 0 && orphanRefineryZoneNeedsReplacement < 0)
				{
					int harvsInRz = 0;
					for (auto const pHU : UnitClass::Array)
					{
						if (pHU && pHU->Owner == pHouse && !pHU->InLimbo && pHU->IsAlive && pHU->Type
							&& (pHU->Type->Harvester || pHU->Type->ResourceGatherer)
							&& MapClass::Instance.GetMovementZoneType(pHU->GetMapCoords(), MovementZone::Normal, false)
								== rz)
						{
							harvsInRz++;
						}
					}

					if (harvsInRz == 0)
					{
						const int rW = pOwnB->Type->GetFoundationWidth();
						const int rH = pOwnB->Type->GetFoundationHeight(false);
						const int adj = std::max(1, pOwnB->Type->Adjacent);
						bool hasAnchorInRz = false;
						for (auto const pAnc : pHouse->Buildings)
						{
							if (!pAnc || pAnc == pOwnB || pAnc->InLimbo || !pAnc->IsAlive || !pAnc->Type)
								continue;

							if (!pAnc->Type->BaseNormal && !pAnc->Type->ConstructionYard
								&& !BuildingTypeExt::IsAIBaseNormal(pAnc->Type))
							{
								continue;
							}

							const CellStruct ac = pAnc->GetMapCoords();
							const int aW = pAnc->Type->GetFoundationWidth();
							const int aH = pAnc->Type->GetFoundationHeight(false);
							const int gapX = std::max(0,
								std::max(static_cast<int>(rCoords.X) - (static_cast<int>(ac.X) + aW - 1),
									static_cast<int>(ac.X) - (static_cast<int>(rCoords.X) + rW - 1)));
							const int gapY = std::max(0,
								std::max(static_cast<int>(rCoords.Y) - (static_cast<int>(ac.Y) + aH - 1),
									static_cast<int>(ac.Y) - (static_cast<int>(rCoords.Y) + rH - 1)));
							if (std::max(gapX, gapY) <= adj)
							{
								hasAnchorInRz = true;
								break;
							}
						}

						if (hasAnchorInRz && !IsRefineryExhausted(pOwnB, CellStruct::Empty))
							orphanRefineryZoneNeedsReplacement = rz;
					}
				}
			}
		}

		const bool wantsHarvesterRecoveryViaRefinery = (pHouse->ConYards.Count >= 1 && staticRefineriesCount >= 1
			&& Unsorted::CurrentFrame >= GetMacroState(pHouse).NextHarvesterRecoveryFrame
			&& GetMacroState(pHouse).TemporaryRecoveryRefineryPlacedFrame.empty()
			&& (orphanRefineryZoneNeedsReplacement >= 0
				|| (ownedHarvestersCount < staticRefineriesCount * 2
					&& (ownedHarvestersCount == 0 || ownedWarFactories >= 1) && features[19] > 0
					&& (money >= 900 || ownedHarvestersCount == 0))));
		const bool hasActiveBuildingIncome = (ownedCashBuildings >= 1)
			|| (((ownedRefineries >= 1 && ownedHarvestersCount >= 1) || ownedSlaveMinerBuildings >= 1)
				&& (features[19] > 0 || pHouse->OwnedTiberium.GetTotalAmount() > 0));

		for (auto const pBld : BuildingTypeClass::Array)
		{
			if (!pBld || pBld->Wall || pBld->FirestormWall || pBld->Gate || pBld->LaserFence || pBld->LaserFencePost
				|| pBld->ToTile || !pBld->Selectable)
			{
				continue;
			}

			if (HouseExt::IsDisabledFromShell(pHouse, pBld))
				continue;

			if (!CanBuildUnderHumanRules(pHouse, pBld))
				continue;

			auto const& cooldownMap = GetMacroState(pHouse).BuildingPlacementCooldownUntilFrame;
			auto const cdIt = cooldownMap.find(pBld->ArrayIndex);
			if (cdIt != cooldownMap.end() && Unsorted::CurrentFrame < cdIt->second)
				continue;

			const bool isBldInProgress = (pBld->ArrayIndex == inProgressBldIdx || pBld->ArrayIndex == inProgressDefIdx);
			const bool isRefineryOrSlaveMiner = pBld->Refinery || pBld->ResourceGatherer || pBld->ResourceDestination;
			const bool isIncomeRecoveryBld = isRefineryOrSlaveMiner
				|| (pBld->PowerBonus > pBld->PowerDrain && !pBld->IsBaseDefense
					&& (pHouse->HasLowPower() || hasUnpoweredBuildings))
				|| (pBld->Factory == AbstractType::UnitType && !pBld->Naval && ownedWarFactories == 0
					&& ownedHarvestersCount == 0)
				|| (pBld->Factory == AbstractType::InfantryType && ownedBarracks == 0 && features[20] > 0)
				|| (pBld->Factory == AbstractType::UnitType && pBld->Naval && ownedShipyards == 0 && features[19] == 0
					&& features[21] > 0);

			// Human-like credit management:
			// - When active income is flowing (hasActiveBuildingIncome), never block
			// or delay selecting buildings by upfront cost; start building
			// immediately and pay step-by-step!
			// - When all income sources are lost (!hasActiveBuildingIncome), strictly
			// ration remaining credits to recover an income source first!
			if (!hasActiveBuildingIncome && !isBldInProgress && !isIncomeRecoveryBld
				&& pBld->GetActualCost(pHouse) > money)
			{
				continue;
			}

			const int owned = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pBld, pHouse);
			int score = 0;

			if (pBld->PowerBonus > pBld->PowerDrain && !pBld->IsBaseDefense)
			{
				// Hard anti-spam cap: never queue more Power Plants when surplus is
				// already well above our damaged-plant safety cushion (relaxed by +350
				// when stepping-stone crawling to a distant Ore patch)!
				const int maxAllowedSurplus = requiredPowerCushion
					+ ((needsSteppingStoneCrawlToOre && pBld->BaseNormal && money >= 1400) ? 700 : 350);
				if (!pHouse->HasLowPower() && !hasUnpoweredBuildings && realPowerSurplus >= maxAllowedSurplus)
					continue;

				// Track best buildable power plant for pre-build power interception
				// (only after verifying we are below maxAllowedSurplus and not
				// placement-locked!)
				const int plantEfficiencyScore = pBld->PowerBonus * 3 - (pBld->GetActualCost(pHouse) / 5);
				if ((GetMacroState(pHouse).ConsecutivePlacementFailures < 2 || pHouse->HasLowPower()
						|| hasUnpoweredBuildings)
					&& plantEfficiencyScore > bestPowerPlantScore
					&& pBld->GetActualCost(pHouse) <= std::max(money + 1000, 1200))
				{
					bestPowerPlantScore = plantEfficiencyScore;
					bestPowerPlantIdx = pBld->ArrayIndex;
				}

				score = scores[0] + std::min(650, pBld->PowerBonus * 2);

				// Avoid spamming new Bio-Reactors from the ConYard when existing owned
				// Bio-Reactors still have free passenger slots that our Barracks can
				// fill with cheap infantry or mind-controlled enemies for +100W each!
				if (pBld->InfantryAbsorb && unfilledBioReactorSlots >= 2 && ownedBarracks >= 1 && !pHouse->HasLowPower()
					&& !hasUnpoweredBuildings && realPowerSurplus >= 40
					&& (realPowerSurplus + bioReactorPotentialPower >= requiredPowerCushion)
					&& !needsSteppingStoneCrawlToOre)
				{
					score -= 1100;
				}

				// Prefer Advanced Reactors (Nuclear/Bio/Tesla >= 300 PowerBonus) over
				// basic power plants once unlocked and affordable, EXCEPT when building
				// a cheap 2x2 BaseNormal stepping stone to reach Ore!
				if (pBld->PowerBonus >= 300 && money >= pBld->GetActualCost(pHouse) && !needsSteppingStoneCrawlToOre)
				{
					score += 350;
				}
				else if (hasAdvancedPowerPlants && pBld->PowerBonus < 250 && money >= 1200
					&& !needsSteppingStoneCrawlToOre)
				{
					score -= 300;
				}

				if (pHouse->HasLowPower() || hasUnpoweredBuildings || realPowerSurplus < 60)
				{
					score += 1400;
				}
				else if (realPowerSurplus < requiredPowerCushion)
				{
					score += 450 + ((needsSteppingStoneCrawlToOre && pBld->BaseNormal) ? 450 : 0);
				}
				else if (needsSteppingStoneCrawlToOre && pBld->BaseNormal && pBld->GetFoundationWidth() <= 3
					&& money >= 1400)
				{
					// AdvAI Stepping-Stone BaseNormal Crawler: use cheap Power Plants to
					// step the legal Adjacent=2 chain out of spawn toward the uncolonized
					// Ore field!
					score += 680;
				}
				else
				{
					score -= 1400;
				}
			}
			else if (isRefineryOrSlaveMiner)
			{
				const bool spawnsFreeHarvester = !pBld->ResourceGatherer && pBld->Enslaves == nullptr
					&& pBld->UndeploysInto == nullptr && pBld->FreeUnit != nullptr
					&& (pBld->FreeUnit->Harvester || pBld->FreeUnit->ResourceGatherer);
				const bool isHarvRecoveryCandidate = spawnsFreeHarvester && wantsHarvesterRecoveryViaRefinery;
				const int maxRefineries = (reachedNewOreField || pHouse->NumConYards >= 2 || features[21] > 0
											|| (hasTechCenter && money < 3000))
					? std::max(6, pHouse->NumConYards + 4)
					: 4;

				if (owned >= maxRefineries && !isHarvRecoveryCandidate)
					continue;

				score = scores[1] - owned * 480;

				if (owned == 0)
				{
					score += 1200;
				}
				else if (reachedNewOreField)
				{
					score += 1950; // AdvAI ShouldBuildRefinery: our BaseNormal chain or
								// Expansion MCV reached a new Ore field! Claim it
								// immediately!
				}
				else if (isHarvRecoveryCandidate
					&& (orphanRefineryZoneNeedsReplacement >= 0 || ownedHarvestersCount < staticRefineriesCount))
				{
					score = std::max(score, static_cast<int>(scores[1]))
						+ 2150; // AdvAI Harvester Recovery via Refinery Sell-and-Replace
								// (spawns FreeUnit Harvester without tying up War Factory!)
				}
				else if (isHarvRecoveryCandidate)
				{
					score = std::max(score, static_cast<int>(scores[1]))
						+ 1150; // Replenish up to 2 Harvesters per Refinery via ConYard
								// while War Factory builds tanks!
				}
				else if (owned == 1 && ownedWarFactories >= 1)
				{
					score += 450;
				}
			}
			else if (pBld->Factory == AbstractType::InfantryType)
			{
				const int effectiveMaxBarracks = forwardConYardNeedsBarracks
					? std::max({ maxBarracks + 2, owned + 1, 5 })
					: ((needsSteppingStoneCrawlToOre && pBld->BaseNormal) ? std::max(maxBarracks + 1, 4) : maxBarracks);
				if (owned >= effectiveMaxBarracks)
					continue;

				// Slot 82 (Cross-Faction Tech Tree): If we already own our native
				// Barracks (ownedBarracks >= and a captured foreign ConYard unlocks
				// a brand-new Foreign Barracks (owned == 0), boost it so we unlock
				// foreign infantry and foreign tech prerequisites!
				if (owned == 0 && ownedBarracks >= 1 && ownedBarracks < 5 && scores[82] > 60)
				{
					score = std::max(static_cast<int>(scores[2]), 350) + 750
						+ std::clamp(static_cast<int>(scores[82]), 0, 500);
				}
				else if (forwardConYardNeedsBarracks)
				{
					// Forward Outpost / Island Beachhead ConYard lacks its own Barracks:
					// do not penalize by home-island Barracks count!
					score = std::max(static_cast<int>(scores[2]), 300) + 1450;
				}
				else
				{
					score = scores[2] - owned * 820;

					if (ownedBarracks < maxEnemyBarracks && hasTechCenter && money > 1500)
						score += 420;

					if (needsSteppingStoneCrawlToOre && pBld->BaseNormal && money >= 1300)
					{
						score += 780; // Forward Barracks stepping-stone toward uncolonized
									// Ore / Frontline!
					}
				}
			}
			else if (pBld->Factory == AbstractType::UnitType && !pBld->Naval)
			{
				const int effectiveMaxWarFactories
					= forwardConYardNeedsWarFactory ? std::max({ maxWarFactories + 2, owned + 1, 6 }) : maxWarFactories;
				if (owned >= effectiveMaxWarFactories)
					continue;

				// Slot 82 (Cross-Faction Tech Tree): If we already own our native War
				// Factory (ownedWarFactories >= and a captured foreign ConYard
				// unlocks a brand-new Foreign War Factory (owned == 0), build it to
				// unlock foreign tanks, Radar, and Battle Lab!
				if (owned == 0 && ownedWarFactories >= 1 && ownedWarFactories < 5 && scores[82] > 60)
				{
					score = std::max(static_cast<int>(scores[3]), 400) + 850
						+ std::clamp(static_cast<int>(scores[82]), 0, 550);
				}
				else if (forwardConYardNeedsWarFactory)
				{
					// Forward Outpost / Island Beachhead ConYard lacks its own War
					// Factory: do not penalize by home-island War Factory count!
					score = std::max(static_cast<int>(scores[3]), 400) + 1750;
				}
				else
				{
					score = scores[3] - owned * 620;

					// Competitive MultipleFactory Arms Race boost: never fall behind
					// the enemy in War Factory count or when floating macro cash!
					if (ownedWarFactories < maxEnemyWarFactories && ownedRefineries >= 2)
						score += 550;

					if (ownedWarFactories < ecoWarFactoryCap && money >= 2000)
						score += 420;
				}
			}
			else if (pBld->Factory == AbstractType::UnitType && pBld->Naval)
			{
				if (owned >= maxShipyards)
					continue;

				if (NeuralAI::FindSpatialTetrisBuildCell(pHouse, pBld) == CellStruct::Empty)
					continue;

				score = scores[7] - owned * 950;

				if (ownedShipyards < maxEnemyShipyards && ownedShipyards >= 1 && money >= 1800)
					score += 400;
			}
			else if (pBld->Factory == AbstractType::AircraftType)
			{
				if (owned >= maxAirpads)
					continue;

				if (owned == 0 && pHouse->NumAirpads >= 1 && pHouse->NumAirpads < 4 && scores[82] > 60)
				{
					score = std::max(static_cast<int>(scores[8]), 250) + 550
						+ std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
				}
				else
				{
					score = scores[8] - owned * 750 + (expert8NeedsMoreAirpadDocks ? 850 : 0);

					if (ownedAirpads < maxEnemyAirpads && hasTechCenter && money >= 2000)
						score += 350;
				}
			}
			else if (pBld->UnitRepair)
			{
				if (owned >= 1)
					continue;

				score = scores[9];
			}
			else if (pBld->SuperWeapon != -1)
			{
				if (owned >= 1)
					continue;

				score = scores[10] + 450;

				if (pHouse->NumConYards >= 2 && scores[82] > 60)
					score += std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
			}
			else if (pBld->Storage > 0 && !isRefineryOrSlaveMiner)
			{
				// Only build Ore Silos when storage is actually full/short, max 2
				if (owned >= 2 || (!pHouse->IsTiberiumShort && money < 3500))
					continue;

				score = scores[11] - owned * 900;
			}
			else if (IsTunnelNetworkBuilding(pBld))
			{
				// Support both Superweapon-deployed 2nd Tunnel Mouths (C&C: Reloaded
				// ROBOTTUNNEL BuildLimit=1) AND mods where multiple Tunnel Mouths are
				// built directly from the Construction Yard (owned < 3)!
				if (!hasCoreEconomyAndProduction || owned >= 3)
					continue;

				const bool hasTunnelDeliverySW = (pBld->SuperWeapon != -1 || pBld->SuperWeapon2 != -1);

				const int tacticalTunnelBias = std::max({ static_cast<int>(scores[28]), static_cast<int>(scores[36]),
					static_cast<int>(scores[61]), static_cast<int>(scores[83]) });

				if (owned == 0)
					score = 680 + tacticalTunnelBias;
				else if (owned == 1 && !hasTunnelDeliverySW)
					score = 760 + tacticalTunnelBias; // Urgent 2nd Tunnel Mouth to

				// connect the two-way portal!
				else
					score = 460 + tacticalTunnelBias - owned * 140;
			}
			else if (pBld->IsBaseDefense)
			{
				// Allow building base defenses once core Economy + Barracks + War Factory exist,
				// OR during an early rush / frontline threat once at least 1 Refinery + 1 Barracks exist (from AdvAI)
				const bool canBuildEmergencyDefense = (ownedRefineries >= 1 && ownedBarracks >= 1
					&& (wasRecentlyAttacked || frontlineThreatActive || undefendedRemoteAssetsCount > 0));
				if (!hasCoreEconomyAndProduction && !canBuildEmergencyDefense && pHouse->ConYards.Count > 0)
					continue;

				bool isAntiInf = false;
				bool isAntiArmor = false;
				bool isAntiAir = false;
				bool isPureAA = false;
				bool isUnarmedBunker = false;
				GetDefenseCapabilities(pBld, isAntiInf, isAntiArmor, isAntiAir, isPureAA, isUnarmedBunker);

				// Never spam unarmed bunkers (Battle Bunkers / Tank Bunkers):
				// block them completely when under attack or frontline threat, when an empty bunker already exists,
				// when we already own 2, or when we lack compatible idle units to garrison them!
				if (isUnarmedBunker)
				{
					if (wasRecentlyAttacked || frontlineThreatActive || underVehicleAttack || underInfantryAttack
						|| underAirAttack)
					{
						continue;
					}

					if (pBld->CanBeOccupied
						&& (emptyOwnedBattleBunkers >= 1 || ownedBattleBunkers >= 2 || owned >= 2
							|| compatibleOccupierInfCount < 2))
					{
						continue;
					}

					if (pBld->Bunker
						&& (emptyOwnedTankBunkers >= 1 || ownedTankBunkers >= 2 || owned >= 2
							|| compatibleBunkerableTankCount < 1))
					{
						continue;
					}
				}

				if (isPureAA && totalAADefenses >= maxAADefenses)
					continue;

				const bool needsOutpostPointDefense
					= !isPureAA && !isUnarmedBunker && (undefendedRemoteAssetsCount > 0 || forwardConYardNeedsDefense);
				const int effectiveMaxGroundDefenses = needsOutpostPointDefense
					? std::max(maxGroundDefenses + 2, totalGroundDefenses + 1)
					: maxGroundDefenses;

				if (!isPureAA && totalGroundDefenses >= effectiveMaxGroundDefenses)
					continue;

				// Do not queue powered defenses when power cushion is already tight
				if ((pBld->Powered || pBld->PowerDrain >= 50)
					&& (pHouse->HasLowPower() || realPowerSurplus - pBld->PowerDrain < 75))
				{
					continue;
				}

				const bool isAntiArmorOrHighTierDefense = !isPureAA && !isUnarmedBunker && isAntiArmor;
				const bool isBasicAntiInfantryDefense = !isPureAA && !isUnarmedBunker && isAntiInf && !isAntiArmor;

				const int tierQualityBonus
					= isUnarmedBunker ? 100 : std::min(850, (pBld->Cost / 2) + std::max(0, pBld->TechLevel) * 140);

				score = (isPureAA ? scores[6] : scores[5]) + tierQualityBonus;

				if (isPureAA)
				{
					score -= totalAADefenses * 420 + owned * 500;
				}
				else if (needsOutpostPointDefense)
				{
					score = std::max(static_cast<int>(scores[5]), 250) + tierQualityBonus + 550 - owned * 380
						- totalGroundDefenses * 160;
				}
				else
				{
					score -= totalGroundDefenses * 480 + owned * 600 + (isUnarmedBunker ? 450 : 0);
				}

				const bool hasInitialCoreDefenses = isPureAA ? (totalAADefenses >= 1) : (totalGroundDefenses >= 2);

				if (!needsOutpostPointDefense && hasInitialCoreDefenses && !wasRecentlyAttacked
					&& !frontlineThreatActive && hasPendingCoreTechOrEco && money < 4000)
				{
					score -= 1150;
				}

				// AdvAI Proactive Enemy Air Value Scaling
				if (isAntiAir && enemyAirValue > 0 && totalAADefenses < (1 + enemyAirValue / 35))
				{
					score += std::min(550, enemyAirValue * 4);
				}

				if (wasRecentlyAttacked || frontlineThreatActive)
				{
					if ((underAirAttack || activeThreatCat == NeuralThreatCategory::Air) && isAntiAir)
					{
						score += 650 + (isPureAA ? 250 : 0);
					}
					else if (!isPureAA && (underVehicleAttack || activeThreatCat == NeuralThreatCategory::Vehicle))
					{
						if (isAntiArmorOrHighTierDefense)
							score += 750;
						else if (isBasicAntiInfantryDefense && (totalGroundDefenses >= 1 || owned >= 1))
							score -= 950;
					}
					else if (!isPureAA && (underInfantryAttack || activeThreatCat == NeuralThreatCategory::Infantry)
						&& isAntiInf && !isUnarmedBunker)
					{
						score += 520 + (pBld->Cost <= 800 ? 220 : 0);
					}
					else if (!isUnarmedBunker)
					{
						score += 260;
					}

					if (frontlineThreatActive && !isPureAA && !isUnarmedBunker)
					{
						score += 620 + (isAntiArmorOrHighTierDefense ? 220 : 0);
					}

					// AdvAI Cost-Efficiency Emergency Bonus when credits are below 2000
					if (money < 2000 && !isUnarmedBunker)
					{
						score += std::clamp((1500 - pBld->GetActualCost(pHouse)) / 3, 0, 380);
					}
				}

				if (features[72] > 0 && pBld->Sensors)
					score += 350;
			}
			else
			{
				// Tech & Support buildings (Radar, Battle Lab, Ore Purifier, Industrial Plant,
				// Cloning Vats, Grinder, Gap/Cloak Generator)
				const int supportRadius = GetSupportRadius(pBld);
				const int maxAllowed = (supportRadius > 0 && hasTechCenter && money > 3000) ? 2 : 1;

				if (owned >= maxAllowed)
					continue;

				score = scores[4] + 550 + std::min(600, pBld->Cost / 3) - owned * 700;

				const bool isCoreTechUnlock = pBld->SpySat
					|| (RulesClass::Instance
						&& (RulesClass::Instance->BuildTech.FindItemIndex(pBld) != -1
							|| RulesClass::Instance->BuildRadar.FindItemIndex(pBld) != -1))
					|| (pBld->TechLevel >= 3 && pBld->Cost >= 1000 && pBld->PowerDrain > 0);

				if (isCoreTechUnlock && owned == 0 && hasCoreEconomyAndProduction)
				{
					score += 750 + (underVehicleAttack ? 450 : 0);
				}

				if (pHouse->NumConYards >= 2 && scores[82] > 60)
					score += 250 + std::clamp(static_cast<int>(scores[82]) / 2, 0, 300);
			}

			if (GetMacroState(pHouse).ConsecutivePlacementFailures == 1 && pBld->BaseNormal
				&& pBld->GetFoundationWidth() <= 2 && pBld->GetFoundationHeight(false) <= 2)
			{
				score += 900; // Try a compact 2x2 BaseNormal anchor once to expand
							// legal Adjacent= perimeter out of tight terrain!
			}

			if (!hasActiveBuildingIncome)
			{
				if (isIncomeRecoveryBld)
				{
					score += 4500; // Zero-Income Emergency: prioritize restoring an
								// income source above all else!
				}
				else
				{
					score -= 3500;
				}
			}

			if (isBldInProgress && (!pHouse->HasLowPower() || pBld->PowerBonus > pBld->PowerDrain))
			{
				score += 850; // Commit to finishing partially constructed buildings in
							// the ConYard!
			}

			if (score > bestBldScore)
			{
				bestBldScore = score;
				bestBldIdx = pBld->ArrayIndex;
			}
		}

		// Pre-Build Power Cushion Interception (from AdvAI):
		// If the highest-scoring building drains power and constructing it would
		// drop our real power surplus below our safety cushion (risking LowPower
		// if a Power Plant is damaged), build a Power Plant first!
		if (bestBldIdx >= 0 && bestPowerPlantIdx >= 0 && bestBldIdx != bestPowerPlantIdx)
		{
			if (auto const pChosen = BuildingTypeClass::Array.GetItem(bestBldIdx))
			{
				if (pChosen->PowerDrain > 0 && pChosen->PowerBonus <= pChosen->PowerDrain)
				{
					const int projectedSurplus = realPowerSurplus - pChosen->PowerDrain;

					const int minSafeAfterBuild
						= (poweredDefAndSWCount > 0 || (pChosen->IsBaseDefense && pChosen->Powered))
						? std::max(120, requiredPowerCushion / 2)
						: 50;

					if (pHouse->HasLowPower() || hasUnpoweredBuildings || projectedSurplus < minSafeAfterBuild)
						bestBldIdx = bestPowerPlantIdx;
				}
			}
		}

		if (bestBldIdx >= 0)
		{
			auto const pChosenBld = BuildingTypeClass::Array.GetItem(bestBldIdx);
			const bool chosenIsStaticRefWithHarv = pChosenBld
				&& (pChosenBld->Refinery || pChosenBld->ResourceDestination) && !pChosenBld->ResourceGatherer
				&& pChosenBld->Enslaves == nullptr && pChosenBld->UndeploysInto == nullptr
				&& pChosenBld->FreeUnit != nullptr
				&& (pChosenBld->FreeUnit->Harvester || pChosenBld->FreeUnit->ResourceGatherer);
			if (chosenIsStaticRefWithHarv && wantsHarvesterRecoveryViaRefinery
				&& (!reachedNewOreField || orphanRefineryZoneNeedsReplacement >= 0) && staticRefineriesCount >= 1)
			{
				GetMacroState(pHouse).IsBuildingRecoveryRefinery = true;
				GetMacroState(pHouse).TargetRecoveryRefineryZone = orphanRefineryZoneNeedsReplacement;
			}
			else if (!chosenIsStaticRefWithHarv)
			{
				GetMacroState(pHouse).IsBuildingRecoveryRefinery = false;
				GetMacroState(pHouse).TargetRecoveryRefineryZone = -1;
			}
		}

		pExt->AIBrain_NextBuildingTarget = bestBldIdx;
	}

	// Unit & Naval Target Selection
	int bestUnitIdx = -1;
	int bestUnitScore = -32000;
	int bestNavalIdx = -1;
	int bestNavalScore = -32000;
	bool ownsUndeployedMCV = false;
	bool ownsUndeployedKeyStructure = false;
	int ownedAmphibTransports = 0;
	int ownedHeavyIslandTransports = 0;
	int ownedUnarmedSupportVehicles = 0;
	int ownedTransportTotalCapacity = 0;
	int waitingBattalionSizeOnIslands = 0;

	for (auto const pOwnedU : UnitClass::Array)
	{
		if (pOwnedU && pOwnedU->Owner == pHouse && !pOwnedU->InLimbo && pOwnedU->IsAlive && pOwnedU->Type)
		{
			auto const pOwnedType = pOwnedU->Type;
			const auto pOwnedWpn = pOwnedType->GetWeapon(0) ? pOwnedType->GetWeapon(0)->WeaponType : nullptr;
			if (pOwnedType->DeploysInto && pOwnedType->DeploysInto->ConstructionYard)
			{
				ownsUndeployedMCV = true;
			}
			else if (pOwnedType->DeploysInto && IsTrueBaseStructureTarget(pOwnedType->DeploysInto)
				&& !pOwnedType->Harvester && !pOwnedType->ResourceGatherer
				&& (!pOwnedType->DeploysInto->IsBaseDefense || !pOwnedWpn))
			{
				ownsUndeployedKeyStructure = true;
			}
			else if (IsAmphibiousOrAirTransport(pOwnedType)
				|| (TechnoTypeExt::Fetch(pOwnedType) && TechnoTypeExt::Fetch(pOwnedType)->Carryall))
			{
				ownedAmphibTransports++;
				ownedTransportTotalCapacity += std::max(1, static_cast<int>(pOwnedType->Passengers));
				if (static_cast<int>(pOwnedType->SizeLimit) >= 6
					|| (TechnoTypeExt::Fetch(pOwnedType) && TechnoTypeExt::Fetch(pOwnedType)->Carryall))
				{
					ownedHeavyIslandTransports++;
				}
			}
			else if (!pOwnedWpn && !pOwnedType->Harvester && !pOwnedType->ResourceGatherer
				&& pOwnedType->Passengers == 0)
			{
				ownedUnarmedSupportVehicles++;
			}
			else if (!pOwnedType->Naval && !pOwnedType->Harvester && !pOwnedType->ResourceGatherer
				&& !IsImmobileUnitTypeOnLand(pOwnedType) && pOwnedType->MovementZone != MovementZone::Amphibious
				&& pOwnedType->MovementZone != MovementZone::AmphibiousDestroyer
				&& pOwnedType->MovementZone != MovementZone::AmphibiousCrusher
				&& pOwnedType->MovementZone != MovementZone::Fly && pOwnedType->SpeedType != SpeedType::Hover
				&& !pOwnedType->JumpJet && !pOwnedType->Teleporter)
			{
				waitingBattalionSizeOnIslands += std::max(1, static_cast<int>(pOwnedType->TechnoTypeClass::Size));
			}
		}
	}

	for (auto const pOwnedInf : InfantryClass::Array)
	{
		if (pOwnedInf && pOwnedInf->Owner == pHouse && !pOwnedInf->InLimbo && pOwnedInf->IsAlive && pOwnedInf->Type
			&& pOwnedInf->Type->MovementZone != MovementZone::Amphibious
			&& pOwnedInf->Type->MovementZone != MovementZone::Fly && !pOwnedInf->Type->JumpJet
			&& !pOwnedInf->Type->Teleporter)
		{
			waitingBattalionSizeOnIslands += std::max(1, static_cast<int>(pOwnedInf->Type->TechnoTypeClass::Size));
		}
	}

	for (auto const pOwnB : pHouse->Buildings)
	{
		if (pOwnB && !pOwnB->InLimbo && pOwnB->IsAlive && pOwnB->Type && pOwnB->Type->UndeploysInto != nullptr
			&& !IsTrueBaseStructureTarget(pOwnB->Type))
		{
			ownedUnarmedSupportVehicles++;
		}
	}

	// Economy capacity: true refineries plus deployed miners define how many
	// harvesting units are useful
	int trueRefineries = 0;
	int ownedEconomyUnits = 0;
	int ownedCashProduces = 0;
	for (auto const pOwnBldType : pHouse->Buildings)
	{
		if (!pOwnBldType || pOwnBldType->InLimbo || !pOwnBldType->IsAlive || !pOwnBldType->Type)
			continue;

		if (pOwnBldType->Type->ProduceCashAmount > 0)
			ownedCashProduces++;

		if (pOwnBldType->Type->ResourceGatherer)
			ownedEconomyUnits++;
		else if (pOwnBldType->Type->Refinery)
			trueRefineries++;
	}

	for (auto const pEcoUnit : UnitClass::Array)
	{
		if (!pEcoUnit || pEcoUnit->Owner != pHouse || pEcoUnit->InLimbo || !pEcoUnit->IsAlive || !pEcoUnit->Type)
			continue;

		auto const pEcoType = pEcoUnit->Type;

		if (pEcoType->Harvester || pEcoType->ResourceGatherer
			|| (pEcoType->DeploysInto && (pEcoType->DeploysInto->Refinery || pEcoType->DeploysInto->ResourceGatherer)))
		{
			ownedEconomyUnits++;
		}
	}

	const int ecoBiasMod = std::clamp(static_cast<int>(scores[39]) / 220, -1, 2);
	const int economyUnitCap = (trueRefineries > 0) ? std::clamp(2 * trueRefineries + 1 + ecoBiasMod, 2, 18) : 4;

	// Emergency ConYard Reconstruction: If we lost all ConYards and have no MCV
	// on the map, check if our War Factory + Service Depot/Grinder/Tech can
	// build a replacement MCV and whether our economy/situation allows saving up
	// the $3000 for it instead of draining credits on cheap units!
	bool canFerryAnyMCVAcrossWater = (ownedHeavyIslandTransports >= 1);
	if (!canFerryAnyMCVAcrossWater)
	{
		for (auto const pCheckU : UnitTypeClass::Array)
		{
			if (pCheckU && pCheckU->DeploysInto && pCheckU->DeploysInto->ConstructionYard
				&& CanBuildUnderHumanRules(pHouse, pCheckU))
			{
				if (CanHouseFerryTypeAcrossWater(pHouse, pCheckU))
				{
					canFerryAnyMCVAcrossWater = true;
					break;
				}
			}
		}
	}

	const bool hasUncolonizedIslandTarget
		= (features[21] == 50 || features[21] >= 100 || (features[21] > 0 && features[20] > 6));
	const int maxAllowedConYards
		= (features[21] > 0) ? (hasUncolonizedIslandTarget ? 4 : 2) : ((features[20] > 12) ? 3 : 2);
	const bool canSafelySaveForReplacementMCV = (pHouse->NumConYards == 0 && !ownsUndeployedMCV)
		&& ((ownedEconomyUnits > 0 && trueRefineries > 0 && features[65] < 45) || money >= 1500
			|| pHouse->OwnedUnits >= 3);
	const bool needsCrossIslandMCV = (features[21] > 0 && (features[19] == 0 || pHouse->NumConYards >= 1));
	const bool wantsArchipelagoExpansion
		= (hasUncolonizedIslandTarget && ownedHeavyIslandTransports >= 1 && pHouse->NumConYards < maxAllowedConYards
			&& (features[19] < 28 || pHouse->Buildings.Count >= 12 || money >= 2200));
	const bool desperateIslandOrOreExpansion
		= (pHouse->NumConYards >= 1 && pHouse->NumConYards < maxAllowedConYards && !ownsUndeployedMCV
			&& (features[20] > 6 || hasUncolonizedIslandTarget) && (scores[30] >= 450 || wantsArchipelagoExpansion)
			&& (!needsCrossIslandMCV || ownedHeavyIslandTransports >= 1 || canFerryAnyMCVAcrossWater)
			&& (features[19] < 24 || pHouse->Buildings.Count >= 14 || wantsArchipelagoExpansion || scores[81] > 85));
	const bool canSafelySaveForExpansionMCV
		= (pHouse->NumConYards >= 1 && pHouse->NumConYards < maxAllowedConYards && !ownsUndeployedMCV
			&& (features[20] > 6 || hasUncolonizedIslandTarget) && (scores[30] >= 450 || wantsArchipelagoExpansion)
			&& (!needsCrossIslandMCV || ownedHeavyIslandTransports >= 1 || canFerryAnyMCVAcrossWater)
			&& (money >= 1400 || desperateIslandOrOreExpansion));
	const bool canSafelySaveForMCV = canSafelySaveForReplacementMCV || canSafelySaveForExpansionMCV;
	const bool savingForIslandTransport = (features[21] > 0 && pHouse->NumShipyards >= 1
		&& ownedHeavyIslandTransports == 0 && (scores[30] >= 450 || wantsArchipelagoExpansion || features[25] > 0)
		&& money < 1200 && pHouse->OwnedUnits >= 2);

	bool canRebuildMCVNow = false;
	int rebuildMCVCost = 3000;
	int inProgressUnitIdx = -1;
	int inProgressNavalIdx = -1;
	if (FactoryClass* pUnitFac = pHouse->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare))
	{
		if (pUnitFac->Object && pUnitFac->Object->GetType())
			inProgressUnitIdx = pUnitFac->Object->GetType()->GetArrayIndex();
	}

	if (FactoryClass* pNavalFac = pHouse->GetPrimaryFactory(AbstractType::UnitType, true, BuildCat::DontCare))
	{
		if (pNavalFac->Object && pNavalFac->Object->GetType())
			inProgressNavalIdx = pNavalFac->Object->GetType()->GetArrayIndex();
	}

	const bool hasActiveUnitIncome = (ownedCashProduces >= 1)
		|| ((trueRefineries >= 1 && ownedEconomyUnits >= 1)
			&& (features[19] > 0 || pHouse->OwnedTiberium.GetTotalAmount() > 0));

	for (auto const pUnit : UnitTypeClass::Array)
	{
		if (!pUnit || !CanBuildUnderHumanRules(pHouse, pUnit))
			continue;

		const bool isUnitInProgress
			= (pUnit->ArrayIndex == inProgressUnitIdx || pUnit->ArrayIndex == inProgressNavalIdx);
		const bool isMCV = (pUnit->DeploysInto && pUnit->DeploysInto->ConstructionYard);
		if (isMCV)
		{
			const bool isSelfAmphibOrAirMcv = (pUnit->MovementZone == MovementZone::Amphibious
				|| pUnit->MovementZone == MovementZone::AmphibiousDestroyer
				|| pUnit->MovementZone == MovementZone::AmphibiousCrusher || pUnit->MovementZone == MovementZone::Fly
				|| pUnit->JumpJet);
			if (ownsUndeployedMCV || (pHouse->NumConYards >= 1 && scores[30] < 400 && scores[81] < 85)
				|| pHouse->NumConYards >= maxAllowedConYards)
			{
				continue;
			}

			if (pHouse->NumConYards >= 1 && features[20] <= 6 && !hasUncolonizedIslandTarget)
				continue;

			if (pHouse->NumConYards >= 1 && needsCrossIslandMCV
				&& (!CanHouseFerryTypeAcrossWater(pHouse, pUnit)
					|| (!isSelfAmphibOrAirMcv && ownedHeavyIslandTransports == 0)))
			{
				continue;
			}

			if (canSafelySaveForMCV)
			{
				canRebuildMCVNow = true;
				rebuildMCVCost = pUnit->GetActualCost(pHouse);
			}
		}

		const auto pWpnStruct = pUnit->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;

		const bool isKeyStructureDeployer = (pUnit->DeploysInto != nullptr
			&& IsTrueBaseStructureTarget(pUnit->DeploysInto) && !isMCV && !pUnit->Harvester && !pUnit->ResourceGatherer
			&& (!pUnit->DeploysInto->IsBaseDefense || pWpn == nullptr));

		if (isKeyStructureDeployer && ownsUndeployedKeyStructure)
			continue;

		const bool isAmphibTransport = IsAmphibiousOrAirTransport(pUnit);
		const bool isDeployableRefineryOrMiner = (pUnit->DeploysInto
			&& (pUnit->DeploysInto->Refinery || pUnit->DeploysInto->ResourceGatherer
				|| pUnit->DeploysInto->ResourceDestination));
		const bool allowExpansionRefineryDeployer = isDeployableRefineryOrMiner
			&& (features[21] > 0 || scores[30] >= 500) && (trueRefineries < 10 && ownedEconomyUnits < 18)
			&& !ownsUndeployedKeyStructure && (features[21] == 0 || CanHouseFerryTypeAcrossWater(pHouse, pUnit));

		bool hasRefineryZoneWithZeroHarvesters = false;
		bool hasUnderharvestedIslandWithWarFactory = false;
		if (pUnit->Harvester || pUnit->ResourceGatherer)
		{
			for (auto const pRefB : pHouse->Buildings)
			{
				if (!pRefB || pRefB->InLimbo || !pRefB->IsAlive || !pRefB->Type
					|| (!pRefB->Type->Refinery && !pRefB->Type->ResourceDestination))
				{
					continue;
				}

				const int rz
					= MapClass::Instance.GetMovementZoneType(pRefB->GetMapCoords(), MovementZone::Normal, false);
				if (rz < 0)
					continue;

				int refsInRz = 0;
				int warFacsInRz = 0;
				for (auto const pOB : pHouse->Buildings)
				{
					if (!pOB || pOB->InLimbo || !pOB->IsAlive || !pOB->Type
						|| MapClass::Instance.GetMovementZoneType(pOB->GetMapCoords(), MovementZone::Normal, false)
							!= rz)
					{
						continue;
					}

					if (pOB->Type->Refinery || pOB->Type->ResourceDestination)
						refsInRz++;
					else if (pOB->Type->Factory == AbstractType::UnitType && !pOB->Type->Naval)
						warFacsInRz++;
				}

				int harvsInRz = 0;
				for (auto const pHU : UnitClass::Array)
				{
					if (pHU && pHU->Owner == pHouse && !pHU->InLimbo && pHU->IsAlive && pHU->Type
						&& (pHU->Type->Harvester || pHU->Type->ResourceGatherer)
						&& MapClass::Instance.GetMovementZoneType(pHU->GetMapCoords(), MovementZone::Normal, false)
							== rz)
					{
						harvsInRz++;
					}
				}

				if (harvsInRz == 0 && (warFacsInRz >= 1 || CanHouseFerryTypeAcrossWater(pHouse, pUnit)))
					hasRefineryZoneWithZeroHarvesters = true;
				else if (warFacsInRz >= 1 && harvsInRz < refsInRz * 2)
					hasUnderharvestedIslandWithWarFactory = true;
			}
		}

		const bool isEconomyUnit = pUnit->Harvester || pUnit->ResourceGatherer || isDeployableRefineryOrMiner;
		if (isEconomyUnit && ownedEconomyUnits >= economyUnitCap && !allowExpansionRefineryDeployer
			&& !hasRefineryZoneWithZeroHarvesters && !hasUnderharvestedIslandWithWarFactory)
		{
			continue;
		}

		TechnoTypeClass* pConvDeployType = GetConvertDeployTargetType(pUnit);
		const bool isConvertToOpenToppedOrCombat = (pConvDeployType != nullptr)
			&& ((pConvDeployType->OpenTopped && pConvDeployType->Passengers > 0)
				|| (pConvDeployType->GetWeapon(0) && pConvDeployType->GetWeapon(0)->WeaponType
					&& pConvDeployType->GetWeapon(0)->WeaponType->Damage > 0));
		if (isConvertToOpenToppedOrCombat && pConvDeployType->OpenTopped && pConvDeployType->Passengers > 0)
		{
			const int ownedConvTotal = pHouse->CountOwnedNow(pUnit)
				+ (abstract_cast<UnitTypeClass*>(pConvDeployType)
						? pHouse->CountOwnedNow(abstract_cast<UnitTypeClass*>(pConvDeployType))
						: 0);
			if (ownedConvTotal >= 2 || pHouse->OwnedInfantry < 3 || canSafelySaveForMCV || savingForIslandTransport)
				continue;
		}

		const bool isUnarmedSupportVehicle = (!pUnit->Naval && pWpn == nullptr && !isMCV && !isKeyStructureDeployer
			&& !isEconomyUnit && !isConvertToOpenToppedOrCombat && pUnit->Passengers == 0
			&& !(TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall));
		if (isUnarmedSupportVehicle)
		{
			// Hard cap unarmed support/detector/jammer vehicles (Mobile Radar Jammer
			// MRJ_SOV/MRJ_ALL, Mobile Stealth Generator SGEN/GGEN, Mobile Sensor
			// Array LPST/NLPST, Limpet Drone) at 1 across the house so they are never
			// spammed!
			if (ownedUnarmedSupportVehicles >= 1 || pHouse->OwnedUnits < 8 || canSafelySaveForMCV
				|| savingForIslandTransport)
			{
				continue;
			}
		}

		const bool isEmpOrMindControlSupport = (!pUnit->Naval && pWpn != nullptr && pWpn->Warhead != nullptr
			&& (pWpn->Warhead->MindControl || (pWpn->Warhead->EMEffect && pWpn->Damage <= 15)));
		if (isEmpOrMindControlSupport)
		{
			// Cap Mind-Control vehicles (Mastermind MIND) and non-damaging EMP Pulse
			// vehicles (Mobile EMP CMOBILEMP) at 2 owned!
			if (pHouse->CountOwnedNow(pUnit) >= 2 || pHouse->OwnedUnits < 6 || canSafelySaveForMCV
				|| savingForIslandTransport)
			{
				continue;
			}
		}

		const bool isIncomeRecoveryUnit = isEconomyUnit || (isMCV && canSafelySaveForMCV)
			|| (isAmphibTransport && ownedAmphibTransports == 0 && (features[21] > 0 || features[19] == 0));
		// Only restrict unit choices by remaining cash when all income sources are
		// lost (!hasActiveUnitIncome)!
		if (!hasActiveUnitIncome && !isUnitInProgress && !isIncomeRecoveryUnit && pUnit->GetActualCost(pHouse) > money)
			continue;

		int score = (pUnit->Cost / 4) + (pUnit->Strength / 3);
		score -= std::min(900, pHouse->CountOwnedNow(pUnit) * 45);
		if (!hasActiveUnitIncome)
		{
			if (isIncomeRecoveryUnit)
			{
				score += 4500; // Zero-Income Emergency: focus War Factory / Shipyard on
							// restoring Harvester/Miner/MCV/Transport!
			}
			else
			{
				score -= 3500;
			}
		}

		if (isUnitInProgress)
		{
			score += 850; // Commit to finishing partially constructed vehicles/ships
						// in the War Factory / Shipyard!
		}

		if (pUnit->Naval)
		{
			if (isAmphibTransport)
			{
				const bool isHeavyMcvTrans = (static_cast<int>(pUnit->SizeLimit) >= 6);
				const int approxOffensiveUnits
					= std::max(0, pHouse->OwnedUnits - ownedEconomyUnits - ownedAmphibTransports);
				const int unitPassCap = std::max(1, static_cast<int>(pUnit->Passengers));
				const int neededForBattalion = (features[21] > 0 && waitingBattalionSizeOnIslands > 0)
					? ((waitingBattalionSizeOnIslands + unitPassCap - 1) / unitPassCap)
					: 0;
				const int baseMaxTransports = (features[21] > 0)
					? ((approxOffensiveUnits >= 11 || pHouse->OwnedUnits >= 18)
							? 5
							: ((approxOffensiveUnits >= 7 || pHouse->OwnedUnits >= 12 || pHouse->NumConYards >= 2)
										? 4
										: 3))
					: 2;
				const int maxTransports
					= (features[21] > 0) ? std::clamp(std::max(baseMaxTransports, neededForBattalion), 3, 10) : 2;
				const bool battalionExceedsCurrentFleet
					= (features[21] > 0 && waitingBattalionSizeOnIslands > ownedTransportTotalCapacity
						&& ownedAmphibTransports < maxTransports);

				if ((isHeavyMcvTrans && ownedHeavyIslandTransports >= maxTransports)
					|| (!isHeavyMcvTrans && ownedAmphibTransports >= maxTransports))
				{
					score -= 1200;
				}
				else if (isHeavyMcvTrans && ownedHeavyIslandTransports == 0 && (features[21] > 0 || scores[30] >= 450))
				{
					score += scores[28] + 2200;
				}
				else if (battalionExceedsCurrentFleet)
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 1100;
				}
				else if (features[21] > 0 && ownedAmphibTransports == 1
					&& (approxOffensiveUnits >= 4 || pHouse->OwnedUnits >= 6
						|| (wantsArchipelagoExpansion && pHouse->NumConYards >= 2)))
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 1350;
				}
				else if (features[21] > 0 && ownedAmphibTransports == 2
					&& (approxOffensiveUnits >= 6 || pHouse->OwnedUnits >= 10))
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 1050;
				}
				else if (features[21] > 0 && ownedAmphibTransports == 3
					&& (approxOffensiveUnits >= 9 || pHouse->OwnedUnits >= 14))
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 900;
				}
				else if (features[21] > 0 && ownedAmphibTransports == 4
					&& (approxOffensiveUnits >= 12 || pHouse->OwnedUnits >= 18))
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 800;
				}
				else
				{
					score += scores[28] + ((features[21] > 0 && ownedAmphibTransports == 0) ? 900 : 350);
				}
			}
			else
			{
				score += scores[29];
				const auto pWpn1 = pUnit->GetWeapon(1) ? pUnit->GetWeapon(1)->WeaponType : nullptr;
				const bool isUnitCarrierSpawner
					= (pUnit->Spawns != nullptr) || (pWpn && pWpn->Spawner) || (pWpn1 && pWpn1->Spawner);
				const bool wpn0HitsLand = pWpn && (pWpn->Damage > 0 || pWpn->Spawner)
					&& (!pWpn->Projectile || pWpn->Projectile->AG || pWpn->Spawner || isUnitCarrierSpawner);
				const bool wpn1HitsLand = pWpn1 && (pWpn1->Damage > 0 || pWpn1->Spawner)
					&& (!pWpn1->Projectile || pWpn1->Projectile->AG || pWpn1->Spawner);
				const int maxLandSiegeRange = std::max(
					wpn0HitsLand ? (isUnitCarrierSpawner ? std::max(pWpn->Range, 18 * 256) : pWpn->Range) : 0,
					wpn1HitsLand ? pWpn1->Range : 0);
				const bool isLongRangeCoastalSiegeShip
					= (isUnitCarrierSpawner && (wpn0HitsLand || wpn1HitsLand)) || (maxLandSiegeRange >= (10 * 256));
				const bool isCoastalBombardShip = (maxLandSiegeRange >= (5 * 256));

				if (features[21] > 0 || features[67] > 10)
				{
					if (isLongRangeCoastalSiegeShip)
					{
						// Continuously produce long-range capital siege warships
						// (Dreadnought, Aircraft Carrier, Boomer, Nod Missile Sub) to
						// saturate the enemy coastline from afar and end island games
						// rapidly!
						score += 1150 + std::min(700, maxLandSiegeRange / 6);
						score += std::min(720,
							pHouse->CountOwnedNow(pUnit) * 35); // Offset duplicate penalty for capital siege fleets!
					}
					else if (isCoastalBombardShip)
					{
						score += 520 + std::min(300, maxLandSiegeRange / 10);
					}
				}
			}

			if (features[72] > 0 && pUnit->Sensors)
				score += 400;

			if (score > bestNavalScore)
			{
				bestNavalScore = score;
				bestNavalIdx = pUnit->ArrayIndex;
			}
		}
		else
		{
			const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;
			const bool isArtillery = pWpn && pWpn->Range >= (8 * 256);

			if (isMCV)
			{
				score += (pHouse->NumConYards == 0)
					? 12000
					: (scores[30] + (canSafelySaveForExpansionMCV ? 1800 : 0)
						+ ((wantsArchipelagoExpansion && ownedHeavyIslandTransports >= 1) ? 900 : 0));
			}
			else if (isKeyStructureDeployer)
			{
				// Universal Deployable Key Structure Scoring across all mods (Mobile
				// Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret
				// Cores, Nanocores):
				auto const pTargetBld = pUnit->DeploysInto;
				const int ownedTargetBld = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pTargetBld, pHouse);

				const bool isDedicatedOutpost = (pTargetBld->BaseNormal && !pTargetBld->Refinery
					&& pTargetBld->Factory == AbstractType::None && pTargetBld->PowerBonus <= pTargetBld->PowerDrain);

				if (isDedicatedOutpost)
				{
					// Mobile Outpost / Explorer / Expansion Hub (Tiberian Sun / Firestorm
					// / Mental Omega / YR mods):
					const int maxDedicatedOutposts = (features[21] > 0) ? 8 : 4;
					if (scores[30] < 300 || ownedTargetBld >= maxDedicatedOutposts)
						score -= 2000;
					else
						score += scores[30] - (ownedTargetBld * 250);
				}
				else if (pTargetBld->PowerBonus > pTargetBld->PowerDrain && !pTargetBld->IsBaseDefense)
				{
					score += scores[0];
				}
				else if (pTargetBld->Refinery)
				{
					score += scores[1] + ((scores[30] > 700) ? (scores[30] / 2) : 0); // Use Mobile Refinery / Refinery
				}

				// Core for Outpost Expansion!
				else if (pTargetBld->Factory == AbstractType::InfantryType)
					score += scores[2];
				else if (pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval)
					score += scores[3]
						+ ((scores[30] > 800) ? (scores[30] / 3) : 0); // Forward Mobile War Factory / Core!
				else if (pTargetBld->Factory == AbstractType::UnitType && pTargetBld->Naval)
					score += scores[7];
				else if (pTargetBld->Factory == AbstractType::AircraftType)
					score += scores[8];
				else if (pTargetBld->UnitRepair)
					score += scores[9] - (ownedTargetBld * 900);
				else if (pTargetBld->SuperWeapon != -1)
					score += scores[10] - (ownedTargetBld * 900);
				else if (pTargetBld->Storage > 0 && !pTargetBld->Refinery)
					score += scores[11];
				else if (pTargetBld->IsBaseDefense)
				{
					const auto pBldWpn = pTargetBld->GetWeapon(0) ? pTargetBld->GetWeapon(0)->WeaponType : nullptr;
					const bool isBldAA = pBldWpn && pBldWpn->Projectile && pBldWpn->Projectile->AA;

					score += isBldAA ? scores[6] : scores[5];
				}
				else
				{
					score += scores[4] - (ownedTargetBld * 900);
				}
			}
			else if (pUnit->Harvester || pUnit->ResourceGatherer)
			{
				const bool conYardHandlingHarvRecovery = (GetMacroState(pHouse).IsBuildingRecoveryRefinery
					&& ownedEconomyUnits >= 1 && pHouse->ConYards.Count >= 1);
				if (conYardHandlingHarvRecovery)
				{
					score += scores[31] - 900; // Keep War Factory queue focused on combat vehicles while
											// ConYard replenishes Harvester via Refinery replacement!
				}
				else
				{
					score += scores[31]
						+ (hasRefineryZoneWithZeroHarvesters
								? 2600
								: (hasUnderharvestedIslandWithWarFactory
										? 1350
										: ((ownedEconomyUnits < 2)
													? 1200
													: (allowExpansionRefineryDeployer ? scores[30] : 0))));
				}
			}
			else if (pUnit->Passengers > 0 || (TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall))
			{
				const bool isAirOrCarryallVehicle
					= isAmphibTransport || (TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall);
				const int approxOffensiveUnits
					= std::max(0, pHouse->OwnedUnits - ownedEconomyUnits - ownedAmphibTransports);
				const int unitPassCap = std::max(1, static_cast<int>(pUnit->Passengers));
				const int neededForBattalion = (features[21] > 0 && waitingBattalionSizeOnIslands > 0)
					? ((waitingBattalionSizeOnIslands + unitPassCap - 1) / unitPassCap)
					: 0;
				const int baseMaxTransports = (features[21] > 0)
					? ((approxOffensiveUnits >= 11 || pHouse->OwnedUnits >= 18)
							? 5
							: ((approxOffensiveUnits >= 7 || pHouse->OwnedUnits >= 12 || pHouse->NumConYards >= 2)
										? 4
										: 3))
					: 2;
				const int maxTransports
					= (features[21] > 0) ? std::clamp(std::max(baseMaxTransports, neededForBattalion), 3, 10) : 2;
				const bool battalionExceedsCurrentFleet
					= (features[21] > 0 && waitingBattalionSizeOnIslands > ownedTransportTotalCapacity
						&& ownedAmphibTransports < maxTransports);
				if (isAirOrCarryallVehicle && ownedAmphibTransports >= maxTransports)
				{
					score -= 1200;
				}
				else if (isAirOrCarryallVehicle && battalionExceedsCurrentFleet)
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 1050;
				}
				else if (isAirOrCarryallVehicle && features[21] > 0 && ownedAmphibTransports == 1
					&& (approxOffensiveUnits >= 4 || pHouse->OwnedUnits >= 6))
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 1250;
				}
				else if (isAirOrCarryallVehicle && features[21] > 0 && ownedAmphibTransports == 2
					&& (approxOffensiveUnits >= 6 || pHouse->OwnedUnits >= 10))
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 950;
				}
				else if (isAirOrCarryallVehicle && features[21] > 0 && ownedAmphibTransports == 3
					&& (approxOffensiveUnits >= 9 || pHouse->OwnedUnits >= 14))
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 850;
				}
				else if (isAirOrCarryallVehicle && features[21] > 0 && ownedAmphibTransports == 4
					&& (approxOffensiveUnits >= 12 || pHouse->OwnedUnits >= 18))
				{
					score += std::max(static_cast<int>(scores[28]), 0) + 750;
				}
				else if (isAirOrCarryallVehicle)
				{
					score += scores[28] + ((features[21] > 0 && ownedAmphibTransports == 0) ? 900 : 350);
				}
				else
				{
					score += scores[28] + (features[21] > 0 ? -500 : 0);
				}
			}
			else if (false && pUnit->Passengers > 0)
			{
				score += scores[28] + (isAmphibTransport ? 350 : (features[21] > 0 ? -500 : 0));
			}
			else if (isUnarmedSupportVehicle)
			{
				score += scores[60] - 250;
			}
			else if (isAA)
			{
				score += scores[26];
			}
			else if (isArtillery)
			{
				score += scores[27];
			}
			else if (pUnit->Strength < 300)
			{
				score += scores[25];
			}
			else
			{
				score += scores[24];
			}

			if (features[77] > 0 && pUnit->ImmuneToPsionics && !isEconomyUnit && !isMCV)
				score += 420; // Counter enemy MindControl with ImmuneToPsionics

			// vehicles (Drones, Robot Tanks, Automated units)!
			if (features[72] > 0 && (pUnit->Sensors || pUnit->DetectDisguise) && !isEconomyUnit)
				score += 480; // Counter enemy Stealth / Disguised units with mobile

			// Sensor & Detector units!
			if (scores[82] > 60 && ownHouseBit != 0u && !pUnit->InOwners(ownHouseBit) && !isEconomyUnit && !isMCV)
			{
				// Slot 82 (Cross-Faction Arsenal Synergy): Mix newly unlocked
				// foreign-faction tanks/vehicles into our strike force!
				score += 240 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 220);
			}

			// Logistics & Anti-Saturation Check on Islands:
			// If our home island already has a large battalion waiting for amphibious/airlift
			// transport, shift War Factory production toward self-ferrying vehicles (Amphibious,
			// Hover, JumpJet, Flying, Teleporter) or Transports so ground-only tanks never
			// saturate and gridlock the home base!
			if (features[21] > 0 && !isEconomyUnit && !isMCV && !isKeyStructureDeployer && !isAmphibTransport
				&& pUnit->Passengers == 0)
			{
				const bool canSelfCrossWater = (pUnit->MovementZone == MovementZone::Amphibious
					|| pUnit->MovementZone == MovementZone::AmphibiousDestroyer
					|| pUnit->MovementZone == MovementZone::AmphibiousCrusher
					|| pUnit->MovementZone == MovementZone::Fly
					|| pUnit->MovementZone == MovementZone::Subterrannean || pUnit->SpeedType == SpeedType::Hover
					|| pUnit->SpeedType == SpeedType::Winged || pUnit->JumpJet || pUnit->Teleporter);

				if (waitingBattalionSizeOnIslands > ownedTransportTotalCapacity + 8)
				{
					if (canSelfCrossWater)
						score += 620;
					else
						score -= 950;
				}
				else if (canSelfCrossWater && money >= 3500)
				{
					score += 280;
				}
			}

			if (score > bestUnitScore)
			{
				bestUnitScore = score;

				bestUnitIdx = pUnit->ArrayIndex;
			}
		}
	}

	pExt->AIBrain_NextUnitTarget = bestUnitIdx;

	pExt->AIBrain_NextNavalTarget = bestNavalIdx;

	// Infantry Target Selection
	int ownedEngineers = 0;
	int ownedInfiltrators = 0;

	for (auto const pOwnInf : InfantryClass::Array)
	{
		if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type)
			continue;

		if (pOwnInf->Type->Engineer)
			ownedEngineers++;
		else if (pOwnInf->Type->Infiltrate)
			ownedInfiltrators++;
	}

	int neutralCapturableCount = 0;
	int neutralCashDerrickCount = 0;

	for (auto const pMapBld : BuildingClass::Array)
	{
		if (pMapBld && !pMapBld->InLimbo && pMapBld->IsAlive && pMapBld->Type && pMapBld->Owner
			&& pMapBld->Type->Capturable && pMapBld->Owner->IsNeutral())
		{
			neutralCapturableCount++;
			if (pMapBld->Type->ProduceCashAmount > 0 || pMapBld->Type->ProduceCashStartup > 0)
				neutralCashDerrickCount++;
		}
	}

	const bool hasCapturableTechOnMap = (neutralCapturableCount > 0);
	int unfilledBioReactorSlotsForInf = 0;
	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (pOwnBld && !pOwnBld->InLimbo && pOwnBld->IsAlive && pOwnBld->Type && pOwnBld->Type->InfantryAbsorb
			&& (pOwnBld->Type->PowerBonus > 0 || pOwnBld->Type->ExtraPowerBonus > 0)
			&& !IsTunnelNetworkBuilding(pOwnBld->Type) && pOwnBld->Passengers.NumPassengers < pOwnBld->Type->Passengers)
		{
			unfilledBioReactorSlotsForInf += (pOwnBld->Type->Passengers - pOwnBld->Passengers.NumPassengers);
		}
	}

	const bool needsCheapInfForBioReactor = (unfilledBioReactorSlotsForInf > 0
		&& (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350));

	int bestInfIdx = -1;
	int bestInfScore = -32000;
	int inProgressInfIdx = -1;
	if (FactoryClass* pInfFac = pHouse->GetPrimaryFactory(AbstractType::InfantryType, false, BuildCat::DontCare))
	{
		if (pInfFac->Object && pInfFac->Object->GetType())
			inProgressInfIdx = pInfFac->Object->GetType()->GetArrayIndex();
	}

	for (auto const pInf : InfantryTypeClass::Array)
	{
		if (!pInf || !CanBuildUnderHumanRules(pHouse, pInf))
			continue;

		const bool isInfInProgress = (pInf->ArrayIndex == inProgressInfIdx);
		const bool isIncomeRecoveryEng = (pInf->Engineer && neutralCashDerrickCount > ownedEngineers);
		if (!hasActiveUnitIncome && !isInfInProgress && !isIncomeRecoveryEng && pInf->GetActualCost(pHouse) > money)
			continue;

		int score = (pInf->Cost / 2) + pInf->Strength + (isInfInProgress ? 450 : 0);

		// Penalize stacking one infantry type so squads stay mixed
		score -= std::min(600, pHouse->CountOwnedNow(pInf) * 20);

		const auto pWpnStruct = pInf->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
		const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;

		if (pInf->Engineer)
		{
			const int maxActiveEngineers = (neutralCapturableCount >= 2) ? std::min(4, neutralCapturableCount) : 3;
			if (ownedEngineers >= maxActiveEngineers)
				continue;

			const bool moreTechThanEngineers = (ownedEngineers < neutralCapturableCount);
			const int cashDerrickBonus
				= (neutralCashDerrickCount > ownedEngineers) ? 880 : (moreTechThanEngineers ? 680 : 320);
			const int techBonus = hasCapturableTechOnMap ? cashDerrickBonus : 0;

			if (pHouse->OwnedInfantry < 3 && !hasCapturableTechOnMap && scores[36] <= 80)
				score -= 300;
			else
				score += scores[18] + techBonus;
		}
		else if (pInf->Infiltrate)
		{
			const int maxActiveSpies
				= (scores[19] > 180 || pExt->AIBrain_Archetype == 1 || pExt->AIBrain_ActiveExpert == 3) ? 3 : 2;
			if (ownedInfiltrators >= maxActiveSpies)
				continue;

			score += (pHouse->OwnedInfantry < 6) ? -400 : scores[19];
		}
		else if (isAA || pInf->C4)
		{
			score += scores[20];
		}
		else if (pInf->Cost >= 250)
		{
			score += scores[17];
		}
		else
		{
			score += scores[16];
		}

		if (needsCheapInfForBioReactor && !pInf->Engineer && !pInf->Infiltrate && !pInf->Doggie && pInf->Cost <= 220)
		{
			score += 520;
		}

		// Train compatible Occupier infantry whenever we own an empty or under-filled Battle Bunker
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (pOwnB && !pOwnB->InLimbo && pOwnB->IsAlive && pOwnB->Type && pOwnB->Type->IsBaseDefense
				&& pOwnB->Type->CanBeOccupied
				&& pOwnB->Occupants.Count < std::clamp(pOwnB->Type->MaxNumberOccupants, 1, 10) && pInf->Occupier
				&& !pInf->Engineer && !pInf->Infiltrate)
			{
				score += 650;
				break;
			}
		}

		if (features[77] > 0 && !pInf->Engineer && !pInf->Infiltrate
			&& (pInf->ImmuneToPsionics || (pWpn && pWpn->Range >= (6 * 256))))
		{
			score += 380; // Counter enemy MindControl with Snipers/Virus/Cyborgs/Dogs!
		}

		if (features[72] > 0 && (pInf->Sensors || pInf->DetectDisguise || pInf->Doggie))
			score += 420;

		if (scores[82] > 60 && ownHouseBit != 0u && !pInf->InOwners(ownHouseBit) && !pInf->Engineer)
			score += 220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200);

		const bool infCanSelfCrossWater = (pInf->MovementZone == MovementZone::Amphibious
			|| pInf->MovementZone == MovementZone::AmphibiousDestroyer
			|| pInf->MovementZone == MovementZone::AmphibiousCrusher || pInf->MovementZone == MovementZone::Fly
			|| pInf->SpeedType == SpeedType::Hover || pInf->SpeedType == SpeedType::Winged || pInf->JumpJet
			|| pInf->Teleporter);

		if (features[21] > 0 && !pInf->Engineer)
		{
			if (waitingBattalionSizeOnIslands > ownedTransportTotalCapacity + 8)
			{
				if (infCanSelfCrossWater)
					score += 550;
				else
					score -= 750;
			}
			else if (infCanSelfCrossWater && money >= 3000)
			{
				score += 250;
			}
		}

		if (score > bestInfScore)
		{
			bestInfScore = score;

			bestInfIdx = pInf->ArrayIndex;
		}
	}

	// Dynamic Base-Congestion & High-Money Infantry Scaling:
	// Allow continuous Barracks production when money is abundant and logistics
	// can deploy troops across the map, pausing only if idle infantry congests
	// the home base or island transport queues!
	int congestedBaseInfantry = 0;
	const CellStruct infHomeCenter = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		? pHouse->ConYards.GetItem(0)->GetMapCoords()
		: (pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell);

	if (infHomeCenter != CellStruct::Empty)
	{
		for (auto const pOwnI : InfantryClass::Array)
		{
			if (!pOwnI || pOwnI->Owner != pHouse || pOwnI->InLimbo || !pOwnI->IsAlive)
				continue;

			if (pOwnI->CurrentMission == Mission::Move || pOwnI->CurrentMission == Mission::Attack
				|| pOwnI->CurrentMission == Mission::AttackMove || pOwnI->CurrentMission == Mission::Enter)
			{
				continue;
			}

			const CellStruct ic = pOwnI->GetMapCoords();
			const int dx = static_cast<int>(ic.X) - static_cast<int>(infHomeCenter.X);
			const int dy = static_cast<int>(ic.Y) - static_cast<int>(infHomeCenter.Y);
			if (dx * dx + dy * dy <= 12 * 12)
				congestedBaseInfantry++;
		}
	}

	const bool islandInfantryQueueFull
		= (features[21] > 0 && waitingBattalionSizeOnIslands > ownedTransportTotalCapacity + 12);
	const int dynamicMaxInfantry = (money >= 4000 && congestedBaseInfantry < 20 && !islandInfantryQueueFull)
		? 85
		: ((money >= 2500 && congestedBaseInfantry < 18 && !islandInfantryQueueFull) ? 62 : 45);
	const bool baseInfantrySaturated = (pHouse->OwnedInfantry >= dynamicMaxInfantry) || (congestedBaseInfantry >= 24);

	// Only pause secondary queues to ration finite credits when all income
	// sources are lost (!hasActiveUnitIncome) or when the base is saturated
	if (!hasActiveUnitIncome
			&& (((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || savingForIslandTransport
					|| ownedEconomyUnits == 0 || trueRefineries == 0)
				&& features[65] < 35)
		|| baseInfantrySaturated)
	{
		if (bestInfIdx < 0 || !InfantryTypeClass::Array.GetItem(bestInfIdx)->Engineer
			|| ownedEngineers >= std::max(1, neutralCashDerrickCount))
		{
			bestInfIdx = -1; // Zero-Income Emergency: ration remaining finite credits to
							// recover Refinery/Harvester/MCV or capture an Oil Derrick!
		}
	}

	if (!hasActiveUnitIncome
		&& ((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || savingForIslandTransport)
		&& !pHouse->HasLowPower() && pHouse->CountResourceDestinations >= 1 && pHouse->NumWarFactories >= 1
		&& pHouse->NumBarracks >= 1 && (features[21] == 0 || pHouse->NumShipyards >= 1)
		&& pHouse->UnitRepairStations.Count >= 1)
	{
		pExt->AIBrain_NextBuildingTarget = -1;
	}

	if (!hasActiveUnitIncome && savingForIslandTransport && bestUnitIdx >= 0)
	{
		if (auto const pChosenU = UnitTypeClass::Array.GetItem(bestUnitIdx))
		{
			if (!pChosenU->Harvester && !pChosenU->ResourceGatherer
				&& !(pChosenU->DeploysInto && pChosenU->DeploysInto->ConstructionYard))
			{
				pExt->AIBrain_NextUnitTarget = -1;
			}
		}
	}

	pExt->AIBrain_NextInfantryTarget = bestInfIdx;

	// Aircraft Target Selection
	int bestAirIdx = -1;
	int bestAirScore = -32000;

	if (pHouse->NumAirpads > 0)
	{
		int ownedDockingAircraft = 0;
		for (auto const pOwnAir : AircraftClass::Array)
		{
			if (pOwnAir && pOwnAir->Owner == pHouse && !pOwnAir->InLimbo && pOwnAir->IsAlive && pOwnAir->Type
				&& !pOwnAir->SpawnOwner && (pOwnAir->Type->AirportBound || pOwnAir->Type->Dock.Count > 0))
			{
				ownedDockingAircraft++;
			}
		}

		int inProgressAirIdx = -1;
		if (FactoryClass* pAirFac = pHouse->GetPrimaryFactory(AbstractType::AircraftType, false, BuildCat::DontCare))
		{
			if (pAirFac->Object && pAirFac->Object->GetType())
				inProgressAirIdx = pAirFac->Object->GetType()->GetArrayIndex();
		}

		for (auto const pAir : AircraftTypeClass::Array)
		{
			if (!pAir || !CanBuildUnderHumanRules(pHouse, pAir))
				continue;

			const bool isAirInProgress = (pAir->ArrayIndex == inProgressAirIdx);
			if (!hasActiveUnitIncome && !isAirInProgress && !pAir->Carryall && pAir->GetActualCost(pHouse) > money)
				continue;

			const int slotScore = (pAir->Strength >= 300 || pAir->Cost >= 1500) ? scores[33] : scores[32];

			const int crossFactionAirBonus = (scores[82] > 60 && ownHouseBit != 0u && !pAir->InOwners(ownHouseBit))
				? (220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200))
				: 0;

			int airTransBonus = 0;
			if (pAir->Carryall || pAir->Passengers >= 3)
			{
				const int ownedAirTrans = pHouse->CountOwnedNow(pAir);
				if (ownedAmphibTransports + ownedAirTrans >= 2)
				{
					airTransBonus = -1200;
				}
				else
				{
					airTransBonus = scores[28]
						+ ((features[21] > 0 && ownedAmphibTransports == 0 && ownedAirTrans == 0) ? 900 : 350);
				}
			}

			const int nonDockBoundBonus = (ownedDockingAircraft >= pHouse->AirportDocks
				&& !pAir->AirportBound && pAir->Dock.Count == 0)
				? 650
				: 0;

			const int score = (pAir->Cost / 3) + (pAir->Strength / 2) + slotScore + crossFactionAirBonus + airTransBonus
				+ nonDockBoundBonus + (isAirInProgress ? 850 : 0);

			if (score > bestAirScore)
			{
				bestAirScore = score;

				bestAirIdx = pAir->ArrayIndex;
			}
		}
	}

	if (!hasActiveUnitIncome
		&& ((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || ownedEconomyUnits == 0
			|| trueRefineries == 0))
	{
		bestAirIdx = -1; // Zero-Income Emergency: save remaining finite credits to
						// restore economy/MCV first!
	}

	pExt->AIBrain_NextAircraftTarget = bestAirIdx;

	if (Unsorted::CurrentFrame % 450 < evalDelay)
	{
		const int bldIdx = pExt->AIBrain_NextBuildingTarget;

		const char* bldName = (bldIdx >= 0 && bldIdx < BuildingTypeClass::Array.Count)
			? BuildingTypeClass::Array.GetItem(bldIdx)->ID
			: "None";

		const char* unitName = (bestUnitIdx >= 0 && bestUnitIdx < UnitTypeClass::Array.Count)
			? UnitTypeClass::Array.GetItem(bestUnitIdx)->ID
			: "None";

		const char* infName = (bestInfIdx >= 0 && bestInfIdx < InfantryTypeClass::Array.Count)
			? InfantryTypeClass::Array.GetItem(bestInfIdx)->ID
			: "None";

		const char* navalName = (bestNavalIdx >= 0 && bestNavalIdx < UnitTypeClass::Array.Count)
			? UnitTypeClass::Array.GetItem(bestNavalIdx)->ID
			: "None";
		const char* airName = (bestAirIdx >= 0 && bestAirIdx < AircraftTypeClass::Array.Count)
			? AircraftTypeClass::Array.GetItem(bestAirIdx)->ID
			: "None";

		Debug::Log("[NeuralAI] Frame %d | House #%d (%s) | Expert=%d Arch=%d | Credits=%d "
				"| NextBld=%s | NextUnit=%s | NextNaval=%s | NextInf=%s | NextAir=%s\n",
			Unsorted::CurrentFrame, pHouse->ArrayIndex, (pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI",
			pExt->AIBrain_ActiveExpert, pExt->AIBrain_Archetype, money, bldName, unitName, navalName, infName, airName);
	}

	return evalDelay;
}

void NeuralAI::UpdateBuildingProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingBuildingTypeIndex = pExt->AIBrain_NextBuildingTarget;
}

void NeuralAI::UpdateUnitProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingUnitTypeIndex = pExt->AIBrain_NextUnitTarget;

	pExt->ProducingNavalUnitTypeIndex = pExt->AIBrain_NextNavalTarget;
}

void NeuralAI::UpdateInfantryProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingInfantryTypeIndex = pExt->AIBrain_NextInfantryTarget;
}

void NeuralAI::UpdateAircraftProduction(HouseClass* pHouse)
{
	auto const pExt = HouseExt::Fetch(pHouse);

	if (Unsorted::CurrentFrame - pExt->AIBrain_LastInferenceFrame > 60)
		NeuralAI::UpdateExpertAI(pHouse);

	pHouse->ProducingAircraftTypeIndex = pExt->AIBrain_NextAircraftTarget;
}

bool NeuralAI::IsCellClearedOfExitCorridors(HouseClass* pHouse, BuildingTypeClass* pType, CellStruct candidateCell)
{
	const int candW = pType->GetFoundationWidth();
	const int candH = pType->GetFoundationHeight(false);
	const bool isCandDefense = pType->IsBaseDefense;

	for (auto const pExisting : BuildingClass::Array)
	{
		if (!pExisting || pExisting->InLimbo || !pExisting->IsAlive || !pExisting->Type
			|| pExisting->Type->InvisibleInGame)
		{
			continue;
		}

		auto const pExType = pExisting->Type;
		const CellStruct exCell = pExisting->GetMapCoords();
		const int exW = pExType->GetFoundationWidth();
		const int exH = pExType->GetFoundationHeight(false);

		// Strictly prevent overlapping any existing building footprint on the map
		if ((candidateCell.X < exCell.X + exW) && (candidateCell.X + candW > exCell.X)
			&& (candidateCell.Y < exCell.Y + exH) && (candidateCell.Y + candH > exCell.Y))
		{
			return false;
		}

		if (pExisting->Owner != pHouse && (!pExisting->Owner || !pHouse->IsAlliedWith(pExisting->Owner)))
			continue;

		// Enforce minimum 4-cell spacing between Base Defense turrets so the AI
		// spreads defenses along the perimeter instead of clumping
		if (isCandDefense && pExType->IsBaseDefense)
		{
			const int turretDist = std::abs(candidateCell.X - exCell.X) + std::abs(candidateCell.Y - exCell.Y);

			if (turretDist < 4)
				return false;
		}

		// Enforce a 1-cell street margin between non-defense structures when
		// Adjacent >= 2 (from AdvAI). Note: A 1-cell gap equals Chebyshev
		// foundation distance 2, which fits inside standard Adjacent=2 while
		// keeping a passable 1-cell road around all 4 sides of every Factory,
		// Refinery, and ConYard!
		const int basePad = (!isCandDefense && !pExType->IsBaseDefense && pType->Adjacent >= 2) ? 1 : 0;
		if (basePad > 0)
		{
			const int exMinX = exCell.X - basePad;
			const int exMaxX = exCell.X + exW + basePad;
			const int exMinY = exCell.Y - basePad;
			const int exMaxY = exCell.Y + exH + basePad;
			const bool overlapX = (candidateCell.X < exMaxX) && (candidateCell.X + candW > exMinX);
			const bool overlapY = (candidateCell.Y < exMaxY) && (candidateCell.Y + candH > exMinY);
			if (overlapX && overlapY)
				return false;
		}
	}

	return true;
}

CellStruct NeuralAI::FindSpatialTetrisBuildCell(HouseClass* pHouse, BuildingTypeClass* pBuildingType)
{
	if (!pHouse || !pBuildingType || pBuildingType->PowersUpBuilding[0])
		return CellStruct::Empty;

	CellStruct center = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	const bool isRefineryOrSlaveMiner
		= pBuildingType->Refinery || pBuildingType->ResourceGatherer || pBuildingType->ResourceDestination;

	if (pHouse->ConYards.Count > 0)
	{
		// If we own an expansion or nomadic ConYard, prioritize planting structures
		// around the newest ConYard (especially when it has < 2 nearby structures
		// so a Nomadic MCV can drop its outpost and hop on!)
		const bool canBuildAtOutpostYard = isRefineryOrSlaveMiner || pBuildingType->IsBaseDefense
			|| pBuildingType->Factory == AbstractType::UnitType || pBuildingType->Factory == AbstractType::InfantryType
			|| pBuildingType->PowerBonus > 0 || pBuildingType->BaseNormal;
		int yardIdx = (pHouse->ConYards.Count > 1 && canBuildAtOutpostYard) ? (pHouse->ConYards.Count - 1) : 0;
		if (pHouse->ConYards.Count > 1)
		{
			for (int yi = pHouse->ConYards.Count - 1; yi >= 1; --yi)
			{
				auto const pCY = pHouse->ConYards.GetItem(yi);
				if (!pCY || pCY->InLimbo || !pCY->IsAlive)
					continue;

				const CellStruct cyc = pCY->GetMapCoords();
				int nearCount = 0;
				for (auto const pB : pHouse->Buildings)
				{
					if (!pB || pB == pCY || pB->InLimbo || !pB->IsAlive || !pB->Type)
						continue;

					if (pB->Type->ConstructionYard)
						continue;

					const CellStruct bc = pB->GetMapCoords();
					if (std::max(std::abs(cyc.X - bc.X), std::abs(cyc.Y - bc.Y)) <= 12)
						nearCount++;
				}

				if (nearCount < 4)
				{
					yardIdx = yi;
					break;
				}
			}
		}

		center = pHouse->ConYards.GetItem(yardIdx)->GetMapCoords();
	}
	else if (pHouse->Buildings.Count > 0)
	{
		// When all ConYards are lost (ConYards.Count == 0), anchor around our
		// safest surviving War Factory, Refinery, or Power Plant that is not under
		// enemy fire!
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			if (pOwnBld->Type->Factory == AbstractType::UnitType || pOwnBld->Type->Refinery || pOwnBld->Type->BaseNormal
				|| pOwnBld->Type->PowerBonus > 0)
			{
				center = pOwnBld->GetMapCoords();

				if (!pOwnBld->HasBeenAttacked)
					break;
			}
		}
	}

	// Support universal Mobile Outpost / Expansion Hub structures, newly
	// colonized archipelago islands, and forward siege outposts: Always
	// prioritize the least-developed expansion island or forward outpost (< 8
	// local structures) so newly colonized islands and forward ConYards
	// immediately receive their Refinery, War Factory, Barracks, and Base
	// Defenses!
	const CellStruct homeSpawnRef = (pHouse->BaseSpawnCell != CellStruct::Empty)
		? pHouse->BaseSpawnCell
		: ((pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)) ? pHouse->ConYards.GetItem(0)->GetMapCoords()
																	: center);
	const int homeSpawnZone = (homeSpawnRef != CellStruct::Empty)
		? MapClass::Instance.GetMovementZoneType(homeSpawnRef, MovementZone::Normal, false)
		: -1;
	int targetExpansionZone = -1;
	int bestHubPriorityScore = -9999;
	const bool placingWarFactory = (pBuildingType->Factory == AbstractType::UnitType && !pBuildingType->Naval);
	const bool placingBarracks = (pBuildingType->Factory == AbstractType::InfantryType);
	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
			|| pOwnBld->Type->InvisibleInGame)
		{
			continue;
		}

		const bool isHubAnchor = pOwnBld->Type->ConstructionYard
			|| (pOwnBld->Type->BaseNormal && !pOwnBld->Type->Refinery
				&& pOwnBld->Type->PowerBonus <= pOwnBld->Type->PowerDrain);
		if (!isHubAnchor && !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
			continue;

		const CellStruct bc = pOwnBld->GetMapCoords();
		const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
		const bool isOtherIsland = (bz >= 0 && homeSpawnZone >= 0 && bz != homeSpawnZone);
		const bool isRemoteHubOnSameLand = (homeSpawnRef != CellStruct::Empty
			&& std::max(std::abs(bc.X - homeSpawnRef.X), std::abs(bc.Y - homeSpawnRef.Y)) > 16);
		if (isOtherIsland || (isRemoteHubOnSameLand && isHubAnchor))
		{
			int localHubBlds = 0;
			int localRefs = 0;
			int localDefs = 0;
			bool hasLocalWarFac = false;
			bool hasLocalBarracks = false;
			for (auto const pCheckB : pHouse->Buildings)
			{
				if (!pCheckB || pCheckB == pOwnBld || pCheckB->InLimbo || !pCheckB->IsAlive || !pCheckB->Type
					|| pCheckB->Type->Insignificant || pCheckB->Type->InvisibleInGame)
				{
					continue;
				}

				if (pCheckB->Type->ConstructionYard)
					continue;

				const CellStruct cbc = pCheckB->GetMapCoords();
				if (MapClass::Instance.GetMovementZoneType(cbc, MovementZone::Normal, false) == bz
					&& std::max(std::abs(bc.X - cbc.X), std::abs(bc.Y - cbc.Y)) <= 18)
				{
					localHubBlds++;
					if (pCheckB->Type->Factory == AbstractType::UnitType && !pCheckB->Type->Naval)
					{
						hasLocalWarFac = true;
					}
					else if (pCheckB->Type->Factory == AbstractType::InfantryType)
					{
						hasLocalBarracks = true;
					}
					else if (pCheckB->Type->Refinery || pCheckB->Type->ResourceGatherer
						|| pCheckB->Type->ResourceDestination)
					{
						localRefs++;
					}
					else if (pCheckB->Type->IsBaseDefense && !IsTunnelNetworkBuilding(pCheckB->Type))
					{
						localDefs++;
					}
				}
			}

			const bool hasLocalRef = (localRefs >= 1);
			bool enemyInHubZone = false;
			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
					|| pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
				{
					continue;
				}

				const CellStruct ec = pEB->GetMapCoords();
				if ((bz >= 0 && MapClass::Instance.GetMovementZoneType(ec, MovementZone::Normal, false) == bz)
					|| std::max(std::abs(bc.X - ec.X), std::abs(bc.Y - ec.Y)) <= 36)
				{
					enemyInHubZone = true;
					break;
				}
			}

			const bool missingThisCoreBld = (placingWarFactory && !hasLocalWarFac)
				|| (placingBarracks && !hasLocalBarracks)
				|| (isRefineryOrSlaveMiner && (!hasLocalRef || (localRefs == 1 && hasLocalWarFac)))
				|| (pBuildingType->IsBaseDefense && localDefs < 2);
			if (localHubBlds < 18 || missingThisCoreBld)
			{
				int hubScore = (18 - std::min(18, localHubBlds)) * 10 + (pOwnBld->Type->ConstructionYard ? 50 : 0)
					+ (enemyInHubZone ? 250 : 0) + (missingThisCoreBld ? 500 : 0);
				if (hubScore > bestHubPriorityScore)
				{
					bestHubPriorityScore = hubScore;
					center = bc;
					targetExpansionZone = bz;
				}
			}
		}
	}

	// Allied Base-Sharing (BuildOffAlly=yes): If we lost our ConYard OR an Ally's
	// ConYard is under attack (< 80% HP) while placing a Base Defense, build
	// around our Ally's ConYard!
	if (GameModeOptionsClass::Instance.BuildOffAlly && (pHouse->ConYards.Count == 0 || pBuildingType->IsBaseDefense))
	{
		for (auto const pAllyBld : BuildingClass::Array)
		{
			if (!pAllyBld || pAllyBld->InLimbo || !pAllyBld->IsAlive || !pAllyBld->Owner || !pAllyBld->Type)
				continue;

			if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral()
				&& pAllyBld->Type->EligibileForAllyBuilding)
			{
				if (pHouse->ConYards.Count == 0
					|| (pBuildingType->IsBaseDefense && pAllyBld->HasBeenAttacked
						&& pAllyBld->GetHealthPercentage() < 0.80))
				{
					center = pAllyBld->GetMapCoords();

					break;
				}
			}
		}
	}

	if (center == CellStruct::Empty)
		return CellStruct::Empty;

	const int w = pBuildingType->GetFoundationWidth();
	const int h = pBuildingType->GetFoundationHeight(false);
	auto const pExt = HouseExt::Fetch(pHouse);

	const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);

	const int crawlBias
		= static_cast<int>(policy.OutputBias[34]) + ((pExt && pExt->AIBrain_ActiveExpert == 6) ? 220 : 0);

	const int turretCreepBias
		= static_cast<int>(policy.OutputBias[38]) + ((pExt && pExt->AIBrain_ActiveExpert == 7) ? 220 : 0);

	// Discover nearest uncolonized Ore/Tiberium patch across an 84-cell radius
	// (for Expert 6, Slot 34 & AdvAI Stepping-Stone Base-Crawling). Measure
	// distance from our closest BaseNormal/ConYard anchor and prefer uncolonized
	// Ore patches >= 10 cells from any existing Refinery!
	const int centerZone = MapClass::Instance.GetMovementZoneType(center, MovementZone::Normal, false);
	CellStruct nearestOreCell = CellStruct::Empty;
	CellStruct fallbackOreCell = CellStruct::Empty;
	int bestOreDist = 9999;
	int bestFallbackOreDist = 9999;

	for (int dy = -84; dy <= 84; dy += (std::abs(dy) <= 44 ? 2 : 3))
	{
		for (int dx = -84; dx <= 84; dx += (std::abs(dx) <= 44 ? 2 : 3))
		{
			const int dCenter = std::abs(dx) + std::abs(dy);
			if (dCenter < 3)
				continue;

			const CellStruct tc { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
			auto const pCell = MapClass::Instance.TryGetCellAt(tc);
			if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
				continue;

			const int oreZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
			if (centerZone >= 0 && oreZone != centerZone)
			{
				continue; // Never try to Adjacent=2 base-crawl across water to a
						// different island!
			}

			bool nearExistingRef = false;
			int minAnchorManhattan = dCenter;
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
					|| pOwnBld->Type->InvisibleInGame)
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();
				if (centerZone >= 0
					&& MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false) != centerZone)
				{
					continue;
				}

				if ((pOwnBld->Type->Refinery || pOwnBld->Type->ResourceGatherer || pOwnBld->Type->ResourceDestination)
					&& std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 10)
				{
					nearExistingRef = true;
				}

				if (pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard
					|| BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
				{
					const int dAnc = std::abs(tc.X - bc.X) + std::abs(tc.Y - bc.Y);
					if (dAnc < minAnchorManhattan)
						minAnchorManhattan = dAnc;
				}
			}

			if (minAnchorManhattan < bestFallbackOreDist)
			{
				bestFallbackOreDist = minAnchorManhattan;
				fallbackOreCell = tc;
			}

			if (pHouse->CountResourceDestinations >= 1)
			{
				if (!nearExistingRef && minAnchorManhattan < bestOreDist)
				{
					bestOreDist = minAnchorManhattan;
					nearestOreCell = tc;
				}
			}
			else if (minAnchorManhattan < bestOreDist)
			{
				bestOreDist = minAnchorManhattan;
				nearestOreCell = tc;
			}
		}
	}

	if (nearestOreCell == CellStruct::Empty)
		nearestOreCell = fallbackOreCell;

	// Discover primary Enemy Threat Vector (for Expert 7 & Slot 38: Forward
	// Defense Crawling & Rear Tech Shielding)
	CellStruct enemyTargetCell = CellStruct::Empty;

	if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
	{
		if (auto const pEnemy = HouseClass::Array.GetItem(pHouse->EnemyHouseIndex))
		{
			enemyTargetCell = pEnemy->BaseCenter != CellStruct::Empty ? pEnemy->BaseCenter : pEnemy->BaseSpawnCell;

			if (enemyTargetCell == CellStruct::Empty && pEnemy->ConYards.Count > 0)
				enemyTargetCell = pEnemy->ConYards.GetItem(0)->GetMapCoords();
		}
	}

	if (enemyTargetCell == CellStruct::Empty)
	{
		for (auto const pOther : HouseClass::Array)
		{
			if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral()
				|| pOther->IsObserver() || pOther->Defeated)
			{
				continue;
			}

			enemyTargetCell = pOther->BaseCenter != CellStruct::Empty ? pOther->BaseCenter : pOther->BaseSpawnCell;

			if (enemyTargetCell != CellStruct::Empty)
				break;
		}
	}

	const bool isResourceBld = isRefineryOrSlaveMiner || pBuildingType->Storage > 0 || pBuildingType->OrePurifier;
	const bool isCombatDefense = pBuildingType->IsBaseDefense;
	const auto pWpnStruct = pBuildingType->GetWeapon(0);
	const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;
	const bool isPureAA = isCombatDefense && pWpn && pWpn->Projectile && pWpn->Projectile->AA && !pWpn->Projectile->AG;
	const bool isGroundDefense = isCombatDefense && !isPureAA;
	const bool isTunnelBld = IsTunnelNetworkBuilding(pBuildingType);

	std::vector<CellStruct> existingTunnelCells;

	CellStruct homeFactoryCell = CellStruct::Empty;

	if (isTunnelBld)
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			if (IsTunnelNetworkBuilding(pOwnBld->Type))
			{
				existingTunnelCells.push_back(pOwnBld->GetMapCoords());
			}
			else if (homeFactoryCell == CellStruct::Empty
				&& (pOwnBld->Type->Factory == AbstractType::InfantryType
					|| pOwnBld->Type->Factory == AbstractType::UnitType))
			{
				homeFactoryCell = pOwnBld->GetMapCoords();
			}
		}
	}

	const bool isForwardTunnelMouth = (isTunnelBld && !existingTunnelCells.empty());
	const bool isNavalBld = pBuildingType->Naval || (pBuildingType->SpeedType == SpeedType::Float);

	const bool isFactoryOrBarracks = !isNavalBld
		&& ((pBuildingType->Factory != AbstractType::None) || pBuildingType->WeaponsFactory
			|| pBuildingType->UnitRepair);

	const int supportRadius = GetSupportRadius(pBuildingType);

	const bool isSupportRadiusBld
		= (supportRadius > 0) && !isCombatDefense && !isResourceBld && !isFactoryOrBarracks && !isNavalBld;

	const bool isPowerPlantBld = (pBuildingType->PowerBonus > pBuildingType->PowerDrain) && !isCombatDefense;
	// Stepping-Stone BaseNormal Expander (from AdvAI): After our 1st Power Plant,
	// BaseNormal Power Plants (and Outposts) step outward toward uncolonized Ore
	// fields or the frontline instead of crowding into the rear corner behind the
	// ConYard!
	const bool isSteppingStoneExpander = pBuildingType->BaseNormal && isPowerPlantBld && (pHouse->PowerOutput >= 150)
		&& (nearestOreCell != CellStruct::Empty || crawlBias > 0);
	const bool isRearProtected = !isNavalBld && !isResourceBld && !isCombatDefense && !isFactoryOrBarracks
		&& !isTunnelBld && !isSupportRadiusBld && !isSteppingStoneExpander;

	// Gather Support Radius coverage & separation targets
	// (Gap/Cloak/Sensor/Jammer)
	std::vector<CellStruct> existingSameSupportCells;

	std::vector<CellStruct> uncoveredKeyBldCells;

	const int targetSupportSeparation = supportRadius > 1 ? std::max(7, (supportRadius * 3) / 2) : 8;

	if (isSupportRadiusBld)
	{
		std::vector<CellStruct> keyBaseCells;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			auto const pOT = pOwnBld->Type;
			const CellStruct bc = pOwnBld->GetMapCoords();

			if (pOT == pBuildingType || (pOT->GapGenerator && pBuildingType->GapGenerator)
				|| (pOT->CloakGenerator && pBuildingType->CloakGenerator)
				|| (pOT->SensorArray && pBuildingType->SensorArray))
			{
				existingSameSupportCells.push_back(bc);
			}

			if (pOT->ConstructionYard || pOT->Factory != AbstractType::None || pOT->SuperWeapon != -1 || pOT->Refinery
				|| pOT->Radar)
			{
				keyBaseCells.push_back(bc);
			}
		}

		for (const CellStruct& kc : keyBaseCells)
		{
			bool covered = false;

			for (const CellStruct& sc : existingSameSupportCells)
			{
				if (kc.DistanceFrom(sc) <= std::max(3, supportRadius - 1))
				{
					covered = true;

					break;
				}
			}

			if (!covered)
				uncoveredKeyBldCells.push_back(kc);
		}
	}

	// Gather Directional Attack Vector (from FrontlineThreat & AttackedBuildingsLIFO) and
	// Undefended Key Assets / Vulnerable Power Plant Clusters (3+ plants grouped)
	CellStruct directionalAttackedBld = CellStruct::Empty;
	CellStruct directionalAttacker = CellStruct::Empty;
	CellStruct undefendedKeyAssetCell = CellStruct::Empty;
	int totalOwnedDefensesCount = 0;
	std::vector<CellStruct> ownMatchingDefCells;

	auto& macroState = GetMacroState(pHouse);
	macroState.UnsafePlacementZones.erase(
		std::remove_if(macroState.UnsafePlacementZones.begin(), macroState.UnsafePlacementZones.end(),
			[](const NeuralUnsafeZone& z) { return Unsorted::CurrentFrame > z.ExpirationFrame; }),
		macroState.UnsafePlacementZones.end());

	if (isCombatDefense)
	{
		bool defAntiInf = false;
		bool defAntiArmor = false;
		bool defAntiAir = false;
		bool defPureAA = false;
		bool defUnarmedBunker = false;
		GetDefenseCapabilities(pBuildingType, defAntiInf, defAntiArmor, defAntiAir, defPureAA, defUnarmedBunker);

		// Priority 1 (AdvAI FrontlineThreat): If a recently placed building detected an
		// enemy threat within 8 cells, place this ground defense directly between our
		// building and that enemy threat!
		if (!isPureAA && macroState.FrontlineThreatNeedsDefenses > 0
			&& Unsorted::CurrentFrame <= macroState.FrontlineThreatActiveFrames
			&& macroState.FrontlineThreatBuildingCoords != CellStruct::Empty
			&& macroState.FrontlineThreatCoords != CellStruct::Empty)
		{
			directionalAttackedBld = macroState.FrontlineThreatBuildingCoords;
			directionalAttacker = macroState.FrontlineThreatCoords;
		}
		else if (!macroState.AttackedBuildingsLIFO.empty())
		{
			PruneAttackedBuildingsLIFO(pHouse);

			int chosenIdx = -1;

			for (int i = static_cast<int>(macroState.AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)
			{
				const auto& rec = macroState.AttackedBuildingsLIFO[i];

				if ((rec.Threat == NeuralThreatCategory::Air && defAntiAir)
					|| (rec.Threat == NeuralThreatCategory::Vehicle && defAntiArmor)
					|| (rec.Threat == NeuralThreatCategory::Infantry && defAntiInf))
				{
					chosenIdx = i;

					break;
				}
			}

			if (chosenIdx < 0 && !macroState.AttackedBuildingsLIFO.empty())
			{
				for (int i = static_cast<int>(macroState.AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)
				{
					const auto& rec = macroState.AttackedBuildingsLIFO[i];
					if ((rec.Threat == NeuralThreatCategory::Air && isPureAA)
						|| (rec.Threat != NeuralThreatCategory::Air && isGroundDefense))
					{
						chosenIdx = i;
						break;
					}
				}
			}

			if (chosenIdx >= 0)
			{
				directionalAttackedBld = macroState.AttackedBuildingsLIFO[chosenIdx].BuildingCoords;

				directionalAttacker = macroState.AttackedBuildingsLIFO[chosenIdx].AttackerCoords;
			}
		}

		std::vector<CellStruct> powerPlantCells;

		std::vector<CellStruct> priorityAssetCells;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			auto const pOT = pOwnBld->Type;
			const CellStruct bc = pOwnBld->GetMapCoords();

			if (pOT->IsBaseDefense && !IsTunnelNetworkBuilding(pOT))
			{
				totalOwnedDefensesCount++;

				const auto pWS = pOT->GetWeapon(0);
				const auto pW = pWS ? pWS->WeaponType : nullptr;
				const bool otherPureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;

				if (otherPureAA == isPureAA)
					ownMatchingDefCells.push_back(bc);
			}
			else if (pOT->PowerBonus > pOT->PowerDrain)
			{
				powerPlantCells.push_back(bc);
			}

			const bool isCapturedTechOrCash = (pOT->ProduceCashAmount > 0 || pOT->ProduceCashStartup > 0
				|| pOT->ToProtect || (pOT->Capturable && pOT->NeedsEngineer));
			const bool isRemoteAsset = (std::max(std::abs(bc.X - center.X), std::abs(bc.Y - center.Y)) > 15);
			if (isCapturedTechOrCash
				|| (isRemoteAsset && (pOT->Refinery || pOT->ConstructionYard || pOT->Factory != AbstractType::None)))
			{
				priorityAssetCells.insert(priorityAssetCells.begin(),
					bc); // Highest priority: defend remote Oil Derricks, Tech Outposts
						// & Expansion Refineries!
			}
			else if (pOT->ConstructionYard || pOT->Factory == AbstractType::UnitType || pOT->SuperWeapon != -1
				|| pOT->Refinery)
			{
				priorityAssetCells.push_back(bc);
			}
		}

		// Detect Vulnerable Power Plant Clusters (3+ Power Plants within 8 cells)
		for (const CellStruct& ppc : powerPlantCells)
		{
			int nearbyPlants = 0;

			for (const CellStruct& otherPpc : powerPlantCells)
			{
				if (std::max(std::abs(ppc.X - otherPpc.X), std::abs(ppc.Y - otherPpc.Y)) <= 8)
					nearbyPlants++;
			}

			if (nearbyPlants >= 3)
				priorityAssetCells.push_back(ppc);
		}

		// Find the first priority asset or power cluster that lacks a nearby
		// defense
		for (const CellStruct& ac : priorityAssetCells)
		{
			bool protectedByDef = false;

			for (const CellStruct& dc : ownMatchingDefCells)
			{
				if (std::max(std::abs(ac.X - dc.X), std::abs(ac.Y - dc.Y)) <= 10)
				{
					protectedByDef = true;

					break;
				}
			}

			if (!protectedByDef)
			{
				undefendedKeyAssetCell = ac;

				break;
			}
		}
	}

	// True C&C Base-Crawling Chain & Forward Turret Creep: Instead of always
	// anchoring at ConYards[0] (which forms a static circle around spawn), anchor
	// at our most forward owned structure toward the Ore field or Enemy base so
	// buildings chain across the map! Forward Tunnel Mouths (2nd+ mouth built
	// from Construction Yard) ALWAYS anchor at our most forward
	// outpost/base-crawl structure!
	const bool enableBaseCrawl = isForwardTunnelMouth || isSteppingStoneExpander
		|| ((isResourceBld || isFactoryOrBarracks)
			&& (crawlBias > -20 || bestOreDist > 10 || pHouse->CountResourceDestinations >= 1))
		|| (turretCreepBias > 10 && isCombatDefense && totalOwnedDefensesCount >= 2
			&& directionalAttackedBld == CellStruct::Empty);

	if (isCombatDefense && directionalAttackedBld != CellStruct::Empty)
	{
		center = directionalAttackedBld;
	}
	else if (isCombatDefense && undefendedKeyAssetCell != CellStruct::Empty)
	{
		center = undefendedKeyAssetCell;
	}
	else if (enableBaseCrawl)
	{
		const bool crawlTowardOre = isResourceBld || isSteppingStoneExpander
			|| (isFactoryOrBarracks && nearestOreCell != CellStruct::Empty && bestOreDist > 10
				&& pHouse->CountResourceDestinations < 3);
		const CellStruct crawlGoal = crawlTowardOre
			? (nearestOreCell != CellStruct::Empty ? nearestOreCell : enemyTargetCell)
			: (enemyTargetCell != CellStruct::Empty ? enemyTargetCell : nearestOreCell);

		if (crawlGoal != CellStruct::Empty)
		{
			int bestAnchorDist = std::abs(center.X - crawlGoal.X) + std::abs(center.Y - crawlGoal.Y);

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant
					|| pOwnBld->Type->InvisibleInGame)
				{
					continue;
				}

				if (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard
					&& !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
				{
					continue;
				}

				const CellStruct bc = pOwnBld->GetMapCoords();
				if (centerZone >= 0
					&& MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false) != centerZone)
				{
					continue;
				}

				const int dGoal = std::abs(bc.X - crawlGoal.X) + std::abs(bc.Y - crawlGoal.Y);

				if (dGoal < bestAnchorDist)
				{
					bestAnchorDist = dGoal;

					center = bc;
				}
			}
		}
	}

	const bool hasPendingClearanceForOther =

		(GetMacroState(pHouse).PendingClearanceType != nullptr
			&& GetMacroState(pHouse).PendingClearanceType != pBuildingType
			&& GetMacroState(pHouse).PendingClearanceCell != CellStruct::Empty);

	// Collect all valid BaseNormal / ConstructionYard adjacency anchor structures
	// (from AdvAI)
	std::vector<BuildingClass*> adjacencyAnchors;

	if (!pBuildingType->ConstructionYard)
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->InvisibleInGame)
				continue;

			if (pOwnBld->CurrentMission == Mission::Selling || pOwnBld->QueuedMission == Mission::Selling)
				continue;

			if (pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard
				|| BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type) || (isNavalBld && pOwnBld->Type->Naval))
			{
				adjacencyAnchors.push_back(pOwnBld);
			}
		}

		if (GameModeOptionsClass::Instance.BuildOffAlly && !isRefineryOrSlaveMiner)
		{
			for (auto const pAllyBld : BuildingClass::Array)
			{
				if (!pAllyBld || pAllyBld->InLimbo || !pAllyBld->IsAlive || !pAllyBld->Owner || !pAllyBld->Type
					|| pAllyBld->Type->InvisibleInGame)
				{
					continue;
				}

				if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral()
					&& pAllyBld->Type->EligibileForAllyBuilding && pAllyBld->Type->BaseNormal)
				{
					adjacencyAnchors.push_back(pAllyBld);
				}
			}
		}

		if (adjacencyAnchors.empty())
			return CellStruct::Empty;
	}

	// Helper that enforces real C&C / YR building adjacency
	// (pBuildingType->Adjacent / AINavalYardAdjacency)
	// against our BaseNormal anchor structures using both fast bounding box and
	// exact GetFoundationData(true) cells
	auto passesBaseAdjacency = [&](const CellStruct& cell) -> bool
	{
		if (pBuildingType->ConstructionYard)
			return true;

		for (auto const pAnchor : adjacencyAnchors)
		{
			int adjacency = std::max(1, pBuildingType->Adjacent);

			if (isNavalBld && pAnchor->Type->ConstructionYard && RulesClass::Instance)
				adjacency = std::max(adjacency, RulesClass::Instance->AINavalYardAdjacency);

			const int otherW = pAnchor->Type->GetFoundationWidth();
			const int otherH = pAnchor->Type->GetFoundationHeight(false);
			const CellStruct origin = pAnchor->GetMapCoords();

			const int gapX = (cell.X > origin.X + otherW - 1)
				? (cell.X - (origin.X + otherW - 1))
				: ((cell.X + w - 1 < origin.X) ? (origin.X - (cell.X + w - 1)) : 0);

			if (gapX > adjacency)
				continue;

			const int gapY = (cell.Y > origin.Y + otherH - 1)
				? (cell.Y - (origin.Y + otherH - 1))
				: ((cell.Y + h - 1 < origin.Y) ? (origin.Y - (cell.Y + h - 1)) : 0);

			if (gapY > adjacency)
				continue;

			CellStruct const* occupy = pAnchor->GetFoundationData(true);

			while (occupy && (occupy->X != 0x7FFF || occupy->Y != 0x7FFF))
			{
				const CellStruct sum = origin + *occupy;
				CellStruct const* newOccupy = pBuildingType->GetFoundationData(true);

				while (newOccupy && (newOccupy->X != 0x7FFF || newOccupy->Y != 0x7FFF))
				{
					const CellStruct newSum = cell + *newOccupy;

					if (std::abs(newSum.X - sum.X) <= adjacency && std::abs(newSum.Y - sum.Y) <= adjacency)
						return true;

					++newOccupy;
				}

				++occupy;
			}
		}

		return false;
	};

	CellStruct bestCandidate = CellStruct::Empty;
	int bestSpatialScore = -999999;
	int validCandidatesChecked = 0;

	std::vector<CellStruct> candidateList;
	candidateList.reserve(1024);

	// Gather exact Adjacent= ring around EVERY BaseNormal / ConstructionYard
	// anchor (from AdvAI) so we never miss a valid placement slot even when
	// ConYard[0] is surrounded or an expansion is far away!
	if (!pBuildingType->ConstructionYard && !isNavalBld && !adjacencyAnchors.empty())
	{
		const int adjRange = std::max(1, pBuildingType->Adjacent);
		for (auto const pAnchor : adjacencyAnchors)
		{
			const CellStruct ac = pAnchor->GetMapCoords();
			const int aW = pAnchor->Type->GetFoundationWidth();
			const int aH = pAnchor->Type->GetFoundationHeight(false);
			const int xMin = ac.X - adjRange - w + 1;
			const int xMax = ac.X + aW + adjRange - 1;
			const int yMin = ac.Y - adjRange - h + 1;
			const int yMax = ac.Y + aH + adjRange - 1;
			for (int cy = yMin; cy <= yMax; ++cy)
			{
				for (int cx = xMin; cx <= xMax; ++cx)
				{
					// Skip cells directly inside the anchor's own footprint
					if (cx + w > ac.X && cx < ac.X + aW && cy + h > ac.Y && cy < ac.Y + aH)
						continue;

					candidateList.push_back(CellStruct { static_cast<short>(cx), static_cast<short>(cy) });
				}
			}
		}
	}
	else if (isNavalBld)
	{
		// AdvAI Naval Yard candidate scan: scan INNER-TO-OUTER (r = 1 .. maxRadius)
		// around coastal BaseNormal/Naval anchors first and around every ConYard,
		// so shoreline water cells next to the allied base are always evaluated
		// first!
		const int baseAdjRange = std::max(1, pBuildingType->Adjacent);
		for (auto const pAnchor : adjacencyAnchors)
		{
			if (pAnchor->Type->ConstructionYard)
				continue;

			const CellStruct ac = pAnchor->GetMapCoords();
			const int anchorScanRange = baseAdjRange + w + 3;
			for (int r = 1; r <= anchorScanRange; ++r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != r)
							continue;

						const CellStruct cand { static_cast<short>(ac.X + dx), static_cast<short>(ac.Y + dy) };
						if (auto const pC = MapClass::Instance.TryGetCellAt(cand))
						{
							if (pC->LandType == LandType::Water)
								candidateList.push_back(cand);
						}
					}
				}
			}
		}

		const int navalMaxAdj = std::max(std::max(1, pBuildingType->Adjacent),
			RulesClass::Instance ? RulesClass::Instance->AINavalYardAdjacency : 36);
		const int maxRadius = std::clamp(navalMaxAdj + w + 2, 24, 48);
		for (int r = 1; r <= maxRadius; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;

					const CellStruct cand { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
					if (auto const pC = MapClass::Instance.TryGetCellAt(cand))
					{
						if (pC->LandType == LandType::Water)
							candidateList.push_back(cand);
					}
				}
			}
		}

		for (auto const pAnchor : adjacencyAnchors)
		{
			if (!pAnchor->Type->ConstructionYard)
				continue;

			const CellStruct ac = pAnchor->GetMapCoords();
			if (std::max(std::abs(ac.X - center.X), std::abs(ac.Y - center.Y)) <= 4)
				continue;

			for (int r = 1; r <= maxRadius; ++r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != r)
							continue;

						const CellStruct cand { static_cast<short>(ac.X + dx), static_cast<short>(ac.Y + dy) };
						if (auto const pC = MapClass::Instance.TryGetCellAt(cand))
						{
							if (pC->LandType == LandType::Water)
								candidateList.push_back(cand);
						}
					}
				}
			}
		}
	}
	else
	{
		const int startRadius = 1;
		const int maxRadius = 28;
		for (int r = startRadius; r <= maxRadius; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;

					candidateList.push_back(
						CellStruct { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) });
				}
			}
		}
	}

	const int maxCandidatesToEvaluate = isNavalBld ? 2400 : 900;
	std::vector<int> strategicWaterZones;
	if (isNavalBld)
	{
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || !pEB->Type->Naval
				|| pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
			{
				continue;
			}

			const int ewz = MapClass::Instance.GetMovementZoneType(pEB->GetMapCoords(), MovementZone::Water, false);
			if (ewz >= 0
				&& std::find(strategicWaterZones.begin(), strategicWaterZones.end(), ewz) == strategicWaterZones.end())
			{
				strategicWaterZones.push_back(ewz);
			}
		}

		for (auto const pEU : UnitClass::Array)
		{
			if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || !pEU->Type->Naval
				|| pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral())
			{
				continue;
			}

			const int uwz = MapClass::Instance.GetMovementZoneType(pEU->GetMapCoords(), MovementZone::Water, false);
			if (uwz >= 0
				&& std::find(strategicWaterZones.begin(), strategicWaterZones.end(), uwz) == strategicWaterZones.end())
			{
				strategicWaterZones.push_back(uwz);
			}
		}
	}
	// Two-pass evaluation: Pass 0 strictly requires the 1-cell street margin
	// (IsCellClearedOfExitCorridors); Pass 1 only runs if Pass 0 found 0 valid
	// cells (e.g. tight cliff/island corner), allowing 0-cell touching with a
	// heavy score penalty (like AdvAI).
	for (int relaxMarginPass = 0; relaxMarginPass < 2 && bestCandidate == CellStruct::Empty; ++relaxMarginPass)
	{
		{
			for (const CellStruct& candidate : candidateList)
			{
				const int radius
					= std::max(1, std::max(std::abs(candidate.X - center.X), std::abs(candidate.Y - center.Y)));

				if (!MapClass::Instance.CoordinatesLegal(candidate))
					continue;

				// Strictly enforce BaseNormal Adjacency (Adjacent= /
				// AINavalYardAdjacency)
				if (!passesBaseAdjacency(candidate))
					continue;

				// Do not place on a cell reserved by PendingClearanceCell for another
				// building
				if (hasPendingClearanceForOther)
				{
					const int rX = GetMacroState(pHouse).PendingClearanceCell.X;
					const int rY = GetMacroState(pHouse).PendingClearanceCell.Y;
					const int rW = GetMacroState(pHouse).PendingClearanceFoundationWidth;
					const int rH = GetMacroState(pHouse).PendingClearanceFoundationHeight;

					if ((candidate.X < rX + rW) && (candidate.X + w > rX) && (candidate.Y < rY + rH)
						&& (candidate.Y + h > rY))
					{
						continue;
					}
				}

				// Never crush or block Ore/Tiberium Spawner Trees or place on Bridges
				if (OverlapsTiberiumTreeZone(candidate, pBuildingType) || OverlapsBridge(candidate, pBuildingType))
					continue;

				bool foundationValid = true;

				for (int fy = 0; fy < h && foundationValid; ++fy)
				{
					for (int fx = 0; fx < w; ++fx)
					{
						const CellStruct fc { static_cast<short>(candidate.X + fx),
							static_cast<short>(candidate.Y + fy) };

						auto const pCell = MapClass::Instance.TryGetCellAt(fc);

						if (!pCell || !pCell->CanThisExistHere(pBuildingType->SpeedType, pBuildingType, pHouse))
						{
							foundationValid = false;

							break;
						}
					}
				}

				if (!foundationValid)
					continue;

				const bool hasOneCellMargin = NeuralAI::IsCellClearedOfExitCorridors(pHouse, pBuildingType, candidate);
				if (!hasOneCellMargin)
				{
					if (relaxMarginPass == 0)
						continue;

					// Even in relaxed pass, never overlap an existing building footprint
					// or clump turrets < 4 cells
					bool overlapsFootprint = false;
					for (auto const pEx : BuildingClass::Array)
					{
						if (!pEx || pEx->InLimbo || !pEx->IsAlive || !pEx->Type || pEx->Type->InvisibleInGame)
							continue;

						const CellStruct exC = pEx->GetMapCoords();
						const int exW = pEx->Type->GetFoundationWidth();
						const int exH = pEx->Type->GetFoundationHeight(false);
						if ((candidate.X < exC.X + exW) && (candidate.X + w > exC.X) && (candidate.Y < exC.Y + exH)
							&& (candidate.Y + h > exC.Y))
						{
							overlapsFootprint = true;
							break;
						}

						if (isCombatDefense && pEx->Type->IsBaseDefense
							&& (std::abs(candidate.X - exC.X) + std::abs(candidate.Y - exC.Y)) < 4)
						{
							overlapsFootprint = true;
							break;
						}
					}

					if (overlapsFootprint)
						continue;
				}

				// When placing/deploying a Construction Yard, never pick a cell within
				// 14 cells of an enemy building or combat unit!
				if (pBuildingType->ConstructionYard)
				{
					bool enemyThreatNearCandidate = false;

					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune
							|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
						{
							continue;
						}

						if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
							continue;

						const CellStruct ec = pEB->GetMapCoords();

						if (std::max(std::abs(candidate.X - ec.X), std::abs(candidate.Y - ec.Y)) <= 14)
						{
							enemyThreatNearCandidate = true;

							break;
						}
					}

					if (!enemyThreatNearCandidate)
					{
						for (auto const pEU : UnitClass::Array)
						{
							if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type
								|| pEU->Type->Harvester || pEU->Type->ResourceGatherer
								|| IsAmphibiousOrAirTransport(pEU->Type))
							{
								continue;
							}

							if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral())
								continue;

							auto const pEWpn = pEU->GetWeapon(0) ? pEU->GetWeapon(0)->WeaponType : nullptr;
							if (!pEWpn
								|| (pEU->Type->Naval
									&& (pEWpn->Range <= 2048 || (pEWpn->Projectile && !pEWpn->Projectile->AG))))
							{
								continue;
							}

							const CellStruct euc = pEU->GetMapCoords();
							if (std::max(std::abs(candidate.X - euc.X), std::abs(candidate.Y - euc.Y)) <= 12)
							{
								enemyThreatNearCandidate = true;
								break;
							}
						}
					}

					if (enemyThreatNearCandidate && relaxMarginPass == 0)
						continue;
				}

				const int candZone = MapClass::Instance.GetMovementZoneType(candidate, MovementZone::Normal, false);
				const int expansionZoneBonus
					= (!isNavalBld && targetExpansionZone >= 0 && candZone == targetExpansionZone) ? 650 : 0;
				int spatialScore = (enableBaseCrawl ? (-radius * 5) : (-radius * 10)) - (hasOneCellMargin ? 0 : 4000)
					+ expansionZoneBonus;

				// AdvAI UnsafePlacementZones Graduated Soft-Avoidance (Never a hard block!):
				// Apply a distance-graduated penalty within 6 cells of a recently destroyed/critically
				// damaged building so the AI prefers building on the safer opposite side of its base,
				// while NEVER self-blocking if the whole buildable area is small or under attack!
				if (!isCombatDefense && !isNavalBld && relaxMarginPass == 0 && !macroState.UnsafePlacementZones.empty())
				{
					for (const auto& uz : macroState.UnsafePlacementZones)
					{
						const int dChebU
							= std::max(std::abs(candidate.X - uz.Coords.X), std::abs(candidate.Y - uz.Coords.Y));
						if (dChebU <= 6)
						{
							spatialScore -= (7 - dChebU) * 280;
							break;
						}
					}
				}

				if (isNavalBld)
				{
					// Exact AdvAI NavalYard_Placement_Cell_Value +
					// Modify_Rating_By_Terrain_Passability + minNavalDist spacing: AdvAI
					// minimizes: (conyardDist * + (closestDist * 100), keeping Naval
					// Yards hugged right next to our closest shoreline structures and
					// ConYard while maintaining >= 5.0 cells between multiple Naval
					// Yards.
					const CellStruct candCenter { static_cast<short>(candidate.X + w / 2),
						static_cast<short>(candidate.Y + h / 2) };
					double closestDistSq = 1e12;
					double conyardDistSq = 1e12;
					int navalProximityPenalty = 0;

					for (auto const pOwnB : pHouse->Buildings)
					{
						if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->InvisibleInGame)
							continue;

						const CellStruct nbc = pOwnB->GetMapCoords();
						const int oW = pOwnB->Type->GetFoundationWidth();
						const int oH = pOwnB->Type->GetFoundationHeight(false);
						const CellStruct otherCenter { static_cast<short>(nbc.X + oW / 2),
							static_cast<short>(nbc.Y + oH / 2) };
						const double dxB = static_cast<double>(candCenter.X - otherCenter.X);
						const double dyB = static_cast<double>(candCenter.Y - otherCenter.Y);
						const double dSq = dxB * dxB + dyB * dyB;

						if (dSq < closestDistSq)
							closestDistSq = dSq;

						if (pOwnB->Type->ConstructionYard)
						{
							const double dxCY = static_cast<double>(candidate.X - otherCenter.X);
							const double dyCY = static_cast<double>(candidate.Y - otherCenter.Y);
							const double cySq = dxCY * dxCY + dyCY * dyCY;
							if (cySq < conyardDistSq)
								conyardDistSq = cySq;
						}

						if (pOwnB->Type->Naval || (pOwnB->Type->SpeedType == SpeedType::Float))
						{
							// AdvAI NavalMode minNavalDist = 5.0 cells between Naval Yards
							// (dSq < 25.0)
							if (dSq < 25.0)
								navalProximityPenalty = 50000;
						}
					}

					if (conyardDistSq >= 1e11)
					{
						const double dxC = static_cast<double>(candidate.X - center.X);
						const double dyC = static_cast<double>(candidate.Y - center.Y);
						conyardDistSq = dxC * dxC + dyC * dyC;
					}

					if (closestDistSq >= 1e11)
						closestDistSq = conyardDistSq;

					const double closestDist = std::sqrt(closestDistSq);
					const double conyardDist = std::sqrt(conyardDistSq);
					int advAiCost
						= std::max(1, static_cast<int>(conyardDist * 10.0) + static_cast<int>(closestDist * 100.0));

					// AdvAI Modify_Rating_By_Terrain_Passability (4 diagonal water
					// corners around foundation):
					auto isCornerFloatPassable = [&](short cx, short cy) -> bool
					{
						const CellStruct cc { cx, cy };
						if (!MapClass::Instance.CoordinatesLegal(cc))
							return true;

						if (auto const pCorner = MapClass::Instance.TryGetCellAt(cc))
						{
							return pCorner->IsClearToMove(
								SpeedType::Float, true, true, 0, MovementZone::Normal, 0, false);
						}

						return true;
					};

					const bool passableAbove = isCornerFloatPassable(
						static_cast<short>(candidate.X - 1), static_cast<short>(candidate.Y - 1));
					const bool passableBelow = isCornerFloatPassable(
						static_cast<short>(candidate.X + w), static_cast<short>(candidate.Y + h));
					const bool passableEast = isCornerFloatPassable(
						static_cast<short>(candidate.X + w), static_cast<short>(candidate.Y - 1));
					const bool passableWest = isCornerFloatPassable(
						static_cast<short>(candidate.X - 1), static_cast<short>(candidate.Y + h));

					if (!passableAbove && !passableBelow)
						advAiCost *= 3;

					if (!passableEast && !passableWest)
						advAiCost *= 3;

					if (!passableAbove)
						advAiCost = (advAiCost * 4) / 3;

					if (!passableBelow)
						advAiCost = (advAiCost * 4) / 3;

					if (!passableEast)
						advAiCost = (advAiCost * 4) / 3;

					if (!passableWest)
						advAiCost = (advAiCost * 4) / 3;

					spatialScore = 50000 - advAiCost - navalProximityPenalty - (hasOneCellMargin ? 0 : 4000);

					// Expert 4 (Naval) -> Expert 1 (BaseBuild) Strategic Water-Body Signaling:
					// Prefer placing Naval Yards in water bodies (MovementZone::Water) that contain
					// enemy Naval Yards / warships or border enemy/resource islands, and penalize isolated inland ponds!
					if (!strategicWaterZones.empty())
					{
						const int candWaterZone
							= MapClass::Instance.GetMovementZoneType(candidate, MovementZone::Water, false);
						if (candWaterZone >= 0)
						{
							const bool isStrategicSea
								= std::find(strategicWaterZones.begin(), strategicWaterZones.end(), candWaterZone)
								!= strategicWaterZones.end();
							if (isStrategicSea)
								spatialScore += 3500;
							else
								spatialScore -= 6000;
						}
					}
				}

				// Bonus when South/East exit of a Factory or Refinery has 2+ clear
				// cells in front of it
				if (isFactoryOrBarracks || isRefineryOrSlaveMiner)
				{
					const CellStruct southExit { static_cast<short>(candidate.X + w / 2),
						static_cast<short>(candidate.Y + h + 1) };
					const CellStruct eastExit { static_cast<short>(candidate.X + w + 1),
						static_cast<short>(candidate.Y + h / 2) };
					if (auto const pSE = MapClass::Instance.TryGetCellAt(southExit))
					{
						if (pSE->GetBuilding() == nullptr && pSE->LandType != LandType::Water
							&& pSE->LandType != LandType::Rock)
						{
							spatialScore += 220;
						}
					}

					if (auto const pEE = MapClass::Instance.TryGetCellAt(eastExit))
					{
						if (pEE->GetBuilding() == nullptr && pEE->LandType != LandType::Water
							&& pEE->LandType != LandType::Rock)
						{
							spatialScore += 220;
						}
					}
				}

				if ((isResourceBld || isSteppingStoneExpander) && nearestOreCell != CellStruct::Empty)
				{
					// Expert 6, Slot 34 & AdvAI Stepping-Stone Crawler:
					// Chain Refineries, Silos, Ore Purifiers AND BaseNormal
					// stepping-stone Power Plants directly toward the nearest uncolonized
					// Ore/Gem patch!
					const int distToOre
						= std::abs(candidate.X - nearestOreCell.X) + std::abs(candidate.Y - nearestOreCell.Y);
					const int orePull = std::clamp(52 + (crawlBias / 8), 32, 95);
					spatialScore -= distToOre * orePull;

					if (enableBaseCrawl && radius >= 2 && radius <= 12)
						spatialScore += 220 + std::clamp(crawlBias / 3, 0, 180);
				}
				else if (isCombatDefense)
				{
					// AdvAI Turret Dispersion: avoid clumping multiple defenses of the same domain
					// within 7 cells so splash damage / artillery cannot disable two turrets at once!
					for (const CellStruct& dc : ownMatchingDefCells)
					{
						const int dCheb = std::max(std::abs(candidate.X - dc.X), std::abs(candidate.Y - dc.Y));
						if (dCheb < 7)
							spatialScore -= (7 - dCheb) * 240;
					}

					// Priority 1: Directional Defense Placement along active attack
					// vector (B -> A)
					if (directionalAttackedBld != CellStruct::Empty && directionalAttacker != CellStruct::Empty)
					{
						const double dxA = static_cast<double>(directionalAttacker.X - directionalAttackedBld.X);
						const double dyA = static_cast<double>(directionalAttacker.Y - directionalAttackedBld.Y);
						const double L2 = dxA * dxA + dyA * dyA;

						const int distToAttacked = std::abs(candidate.X - directionalAttackedBld.X)
							+ std::abs(candidate.Y - directionalAttackedBld.Y);

						if (L2 >= 9.0)
						{
							const double cx = static_cast<double>(candidate.X - directionalAttackedBld.X);
							const double cy = static_cast<double>(candidate.Y - directionalAttackedBld.Y);
							const double distPerp = std::abs(cx * dyA - cy * dxA) / std::sqrt(L2);
							const double dot = cx * dxA + cy * dyA;

							spatialScore -= static_cast<int>(distPerp * 45.0) + distToAttacked * 35;

							if (dot < 0.0)
								spatialScore -= 1200; // Behind the attacked building instead of

							// facing attacker
							else
								spatialScore += 350;
						}
						else
						{
							spatialScore -= distToAttacked * 45;
						}
					}
					else if (undefendedKeyAssetCell != CellStruct::Empty)
					{
						// Priority 2: Shield undefended ConYard, War Factory, Superweapon,
						// exposed Refinery, or Power Plant Cluster (3+ plants grouped)!
						const int distToAsset = std::abs(candidate.X - undefendedKeyAssetCell.X)
							+ std::abs(candidate.Y - undefendedKeyAssetCell.Y);

						spatialScore -= distToAsset * 42;

						if (distToAsset >= 4 && distToAsset <= 9)
							spatialScore += 280;

						if (enemyTargetCell != CellStruct::Empty)
						{
							const int distToEnemy
								= std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

							spatialScore -= distToEnemy * 16;
						}
					}
					else if (isGroundDefense && enemyTargetCell != CellStruct::Empty)
					{
						// Priority 3: Frontline perimeter facing the enemy
						const int distToEnemy
							= std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

						const int enemyPull = std::clamp(38 + (turretCreepBias / 10), 20, 80);

						spatialScore -= distToEnemy * enemyPull;

						if (radius >= 6 && radius <= 15)
							spatialScore += 220 + std::clamp(turretCreepBias / 2, 0, 220);
					}
					else if (isPureAA)
					{
						// Spread AA defenses around mid-ring (radius 5..11) to cover both
						// ConYard/Tech/Power clusters and frontline
						if (radius >= 5 && radius <= 11)
							spatialScore += 180;
					}
				}
				else if (isSupportRadiusBld)
				{
					// Area-Support Dispersion (Gap Generator, Cloak Generator, Sensor
					// Array): Cover uncovered base buildings while keeping optimal
					// distance from existing support structures of the same network!
					for (const CellStruct& sc : existingSameSupportCells)
					{
						const int dSame = std::abs(candidate.X - sc.X) + std::abs(candidate.Y - sc.Y);

						if (dSame < targetSupportSeparation)
							spatialScore -= 4500;
					}

					if (!uncoveredKeyBldCells.empty())
					{
						int minDistUncovered = 9999;

						for (const CellStruct& uc : uncoveredKeyBldCells)
						{
							const int dU = std::abs(candidate.X - uc.X) + std::abs(candidate.Y - uc.Y);

							if (dU < minDistUncovered)
								minDistUncovered = dU;
						}

						spatialScore -= minDistUncovered * 40;
					}
				}
				else if (isRearProtected && enemyTargetCell != CellStruct::Empty)
				{
					// Expert 1: Tuck Power Plants, Tech Centers & Superweapons safely
					// into the rear perimeter away from enemy attacks
					const int distToEnemy
						= std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

					spatialScore += distToEnemy * 18;
				}
				else if (isTunnelBld)
				{
					if (!isForwardTunnelMouth)
					{
						// 1st Tunnel Mouth (Home Entrance): Place near our home
						// Barracks/War Factory so newly produced infantry & vehicles
						// immediately enter the portal!
						if (homeFactoryCell != CellStruct::Empty)
						{
							const int distToFac
								= std::abs(candidate.X - homeFactoryCell.X) + std::abs(candidate.Y - homeFactoryCell.Y);

							spatialScore -= distToFac * 35;

							if (distToFac >= 4 && distToFac <= 9)
								spatialScore += 260;
						}
					}
					else
					{
						// 2nd+ Tunnel Mouth built directly from Construction Yard (Forward
						// Exit): Maximize separation from existing Tunnel Mouths and push
						// toward the enemy flank/outpost!
						int minDistToExistingTunnel = 9999;

						for (const CellStruct& tc : existingTunnelCells)
						{
							const int dT = std::abs(candidate.X - tc.X) + std::abs(candidate.Y - tc.Y);

							if (dT < minDistToExistingTunnel)
								minDistToExistingTunnel = dT;
						}

						if (minDistToExistingTunnel < 10)
							spatialScore -= 5000;
						else
							spatialScore += std::min(minDistToExistingTunnel, 45) * 45;

						if (enemyTargetCell != CellStruct::Empty)
						{
							const int distToEnemy
								= std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

							spatialScore -= distToEnemy * 40;
						}
					}
				}
				else if (isFactoryOrBarracks
					&& (enemyTargetCell != CellStruct::Empty || nearestOreCell != CellStruct::Empty))
				{
					// Place War Factories & Barracks forward along the crawl chain
					// (stepping toward uncolonized Ore when expanding economy, or toward
					// Enemy frontline!)
					const bool stepFactoryToOre = (nearestOreCell != CellStruct::Empty && bestOreDist > 10
						&& (pHouse->CountResourceDestinations < 3 || Unsorted::CurrentFrame > 2400));
					const CellStruct facGoal = stepFactoryToOre
						? nearestOreCell
						: (enemyTargetCell != CellStruct::Empty ? enemyTargetCell : nearestOreCell);
					const int distToGoal = std::abs(candidate.X - facGoal.X) + std::abs(candidate.Y - facGoal.Y);
					const int fwdPull = stepFactoryToOre ? std::clamp(38 + (crawlBias / 10), 24, 75)
														: std::clamp(22 + (crawlBias / 12), 12, 55);
					spatialScore -= distToGoal * fwdPull;

					if (enableBaseCrawl && radius >= 2 && radius <= 12)
						spatialScore += 180;
				}

				if (spatialScore > bestSpatialScore)
				{
					bestSpatialScore = spatialScore;

					bestCandidate = candidate;
				}

				if (++validCandidatesChecked >= maxCandidatesToEvaluate)
					return bestCandidate;
			}
		}
	}

	return bestCandidate;
}

int NeuralAI::PlaceCompletedBuilding(BuildingClass* pFactory, BuildingClass* pBuilding, CellStruct& outCell)
{
	outCell = CellStruct::Empty;

	auto const pHouse = pFactory ? pFactory->Owner : (pBuilding ? pBuilding->Owner : nullptr);

	if (!pHouse || !pBuilding || !pBuilding->Type)
		return 0;

	auto const pBuildingType = pBuilding->Type;
	auto const pExt = HouseExt::Fetch(pHouse);
	const int w = pBuildingType->GetFoundationWidth();
	const int h = pBuildingType->GetFoundationHeight(false);

	// Step 1: Check active PendingClearanceCell reservation (waiting for sold
	// structure or scattered units to clear the footprint)
	if (GetMacroState(pHouse).PendingClearanceType != nullptr)
	{
		const bool coordsValid = GetMacroState(pHouse).PendingClearanceCell != CellStruct::Empty
			&& MapClass::Instance.CoordinatesLegal(GetMacroState(pHouse).PendingClearanceCell);

		const bool foundationMatches = (GetMacroState(pHouse).PendingClearanceType == pBuildingType)
			|| (GetMacroState(pHouse).PendingClearanceFoundationWidth == w
				&& GetMacroState(pHouse).PendingClearanceFoundationHeight == h
				&& GetMacroState(pHouse).PendingClearanceFoundation == pBuildingType->Foundation);

		if (!coordsValid || !foundationMatches)
		{
			GetMacroState(pHouse).ClearClearanceReservation();
		}
		else
		{
			GetMacroState(pHouse).PendingClearanceType = pBuildingType;

			if (Unsorted::CurrentFrame < GetMacroState(pHouse).NextClearanceCheckFrame)
				return 1; // Keep building waiting in Construction Yard!

			bool stillObstructed = false;

			for (int fy = 0; fy < h; ++fy)
			{
				for (int fx = 0; fx < w; ++fx)
				{
					const CellStruct fc { static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.X + fx),
						static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.Y + fy) };

					if (auto const pCell = MapClass::Instance.TryGetCellAt(fc))
					{
						if (pCell->GetBuilding() != nullptr)
							stillObstructed = true;

						if (pCell->GetUnit(false) || pCell->GetInfantry(false))
						{
							if (pCell->OverlayTypeIndex >= 0 && pCell->WallOwnerIndex == pHouse->ArrayIndex)
							{
								if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
								{
									if (pOv->Wall)
									{
										CellStruct wallCell = fc;
										pHouse->SellWall(wallCell, true);
									}
								}
							}

							pCell->ScatterContent(CellClass::Cell2Coord(fc), true, true, false);

							stillObstructed = true;
						}
					}
				}
			}

			if (!stillObstructed)
			{
				bool canExistNow = true;

				for (int fy = 0; fy < h && canExistNow; ++fy)
				{
					for (int fx = 0; fx < w; ++fx)
					{
						const CellStruct fc { static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.X + fx),
							static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.Y + fy) };

						auto const pCell = MapClass::Instance.TryGetCellAt(fc);

						if (!pCell || !pCell->CanThisExistHere(pBuildingType->SpeedType, pBuildingType, pHouse))
						{
							canExistNow = false;

							break;
						}
					}
				}

				if (canExistNow)
				{
					outCell = GetMacroState(pHouse).PendingClearanceCell;

					Debug::Log("[NeuralAI] House %d placed %s at cleared reservation "
							"cell (%d,%d).\n",
						pHouse->ArrayIndex, pBuildingType->ID, outCell.X, outCell.Y);

					GetMacroState(pHouse).ClearClearanceReservation();

					return 2;
				}
			}

			GetMacroState(pHouse).ClearanceAttempts++;

			if (GetMacroState(pHouse).ClearanceAttempts < 6)
			{
				GetMacroState(pHouse).NextClearanceCheckFrame = Unsorted::CurrentFrame + 45;

				return 1; // Keep waiting in Construction Yard!
			}

			GetMacroState(pHouse).ClearClearanceReservation();

			GetMacroState(pHouse).LastClearanceFailedFrame = Unsorted::CurrentFrame;
		}
	}

	// AdvAI Harvester Recovery via Refinery Sell-and-Replace (Variant A) or
	// Temporary Placement (Variant B): Selling an existing static Refinery
	// (especially one on an island that lost its Harvester, or a damaged/idle
	// Refinery) and placing the newly built Refinery in its exact footprint
	// spawns a FreeUnit Harvester immediately while recovering 50% cash and
	// keeping the War Factory vehicle queue 100% dedicated to combat units!
	const bool isStaticRefineryWithFreeHarv = (pBuildingType->Refinery || pBuildingType->ResourceDestination)
		&& !pBuildingType->ResourceGatherer && pBuildingType->Enslaves == nullptr
		&& pBuildingType->UndeploysInto == nullptr && pBuildingType->FreeUnit != nullptr
		&& (pBuildingType->FreeUnit->Harvester || pBuildingType->FreeUnit->ResourceGatherer);

	auto tryReplaceExistingRefineryForHarvester = [&]() -> bool
	{
		if (!isStaticRefineryWithFreeHarv)
			return false;

		const int adj = std::max(1, pBuildingType->Adjacent);
		const int targetZone = GetMacroState(pHouse).TargetRecoveryRefineryZone;
		std::vector<BuildingClass*> zeroHarvZoneCandidates;
		std::vector<BuildingClass*> damagedIdleCandidates;
		std::vector<BuildingClass*> idleCandidates;
		std::vector<BuildingClass*> fallbackCandidates;

		for (auto const pOldRef : pHouse->Buildings)
		{
			if (!pOldRef || !pOldRef->IsAlive || pOldRef->InLimbo || pOldRef == pBuilding || !pOldRef->Type)
				continue;

			if ((!pOldRef->Type->Refinery && !pOldRef->Type->ResourceDestination) || pOldRef->Type->ResourceGatherer
				|| pOldRef->Type->Enslaves != nullptr || pOldRef->Type->UndeploysInto != nullptr)
			{
				continue;
			}

			if (pOldRef->CurrentMission == Mission::Construction || pOldRef->QueuedMission == Mission::Construction
				|| pOldRef->CurrentMission == Mission::Selling || pOldRef->QueuedMission == Mission::Selling)
			{
				continue;
			}

			if (pOldRef->Type->GetFoundationWidth() != w || pOldRef->Type->GetFoundationHeight(false) != h
				|| pOldRef->Type->Foundation != pBuildingType->Foundation)
			{
				continue;
			}

			const CellStruct oldCell = pOldRef->GetMapCoords();
			bool hasAdjacentAnchor = false;
			for (auto const pOther : pHouse->Buildings)
			{
				if (!pOther || !pOther->IsAlive || pOther->InLimbo || pOther == pOldRef || pOther == pBuilding
					|| !pOther->Type)
				{
					continue;
				}

				if ((!pOther->Type->BaseNormal && !pOther->Type->ConstructionYard
						&& !BuildingTypeExt::IsAIBaseNormal(pOther->Type))
					|| pOther->CurrentMission == Mission::Selling || pOther->QueuedMission == Mission::Selling)
				{
					continue;
				}

				const CellStruct otherCell = pOther->GetMapCoords();
				const int otherW = pOther->Type->GetFoundationWidth();
				const int otherH = pOther->Type->GetFoundationHeight(false);
				const int gapX = std::max(0,
					std::max(static_cast<int>(oldCell.X) - (static_cast<int>(otherCell.X) + otherW - 1),
						static_cast<int>(otherCell.X) - (static_cast<int>(oldCell.X) + w - 1)));
				const int gapY = std::max(0,
					std::max(static_cast<int>(oldCell.Y) - (static_cast<int>(otherCell.Y) + otherH - 1),
						static_cast<int>(otherCell.Y) - (static_cast<int>(oldCell.Y) + h - 1)));
				if (std::max(gapX, gapY) <= adj)
				{
					hasAdjacentAnchor = true;
					break;
				}
			}

			if (!hasAdjacentAnchor)
				continue;

			const int oldZone = MapClass::Instance.GetMovementZoneType(oldCell, MovementZone::Normal, false);
			int harvsInOldZone = 0;
			if (oldZone >= 0)
			{
				for (auto const pHU : UnitClass::Array)
				{
					if (pHU && pHU->Owner == pHouse && !pHU->InLimbo && pHU->IsAlive && pHU->Type
						&& (pHU->Type->Harvester || pHU->Type->ResourceGatherer)
						&& MapClass::Instance.GetMovementZoneType(pHU->GetMapCoords(), MovementZone::Normal, false)
							== oldZone)
					{
						harvsInOldZone++;
					}
				}
			}

			if ((targetZone >= 0 && oldZone == targetZone) || (oldZone >= 0 && harvsInOldZone == 0))
				zeroHarvZoneCandidates.push_back(pOldRef);

			fallbackCandidates.push_back(pOldRef);
			if (!pOldRef->HasAnyLink())
			{
				idleCandidates.push_back(pOldRef);
				if (pOldRef->Health < pOldRef->Type->Strength)
					damagedIdleCandidates.push_back(pOldRef);
			}
		}

		const auto& pool = !zeroHarvZoneCandidates.empty()
			? zeroHarvZoneCandidates
			: (!damagedIdleCandidates.empty() ? damagedIdleCandidates
											: (!idleCandidates.empty() ? idleCandidates : fallbackCandidates));
		if (pool.empty())
			return false;

		const int randIdx = ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(pool.size()) - 1);
		BuildingClass* pChosenOldRef = pool[randIdx];
		const CellStruct replaceCell = pChosenOldRef->GetMapCoords();

		Debug::Log("[NeuralAI] HarvesterRecovery: House %d selling existing refinery %s "
				"at (%d,%d) to place new refinery %s in its exact spot.\n",
			pHouse->ArrayIndex, pChosenOldRef->Type->ID, replaceCell.X, replaceCell.Y, pBuildingType->ID);

		pChosenOldRef->Sell(1);
		for (int fy = 0; fy < h; ++fy)
		{
			for (int fx = 0; fx < w; ++fx)
			{
				const CellStruct fc { static_cast<short>(replaceCell.X + fx), static_cast<short>(replaceCell.Y + fy) };
				if (auto const pCell = MapClass::Instance.TryGetCellAt(fc))
				{
					if (pCell->OverlayTypeIndex >= 0 && pCell->WallOwnerIndex == pHouse->ArrayIndex)
					{
						if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
						{
							if (pOv->Wall)
							{
								CellStruct wallCell = fc;
								pHouse->SellWall(wallCell, true);
							}
						}
					}

					pCell->ScatterContent(CellClass::Cell2Coord(fc), true, true, false);
				}
			}
		}

		auto& st = GetMacroState(pHouse);
		st.PendingClearanceCell = replaceCell;
		st.PendingClearanceType = pBuildingType;
		st.PendingClearanceFoundationWidth = w;
		st.PendingClearanceFoundationHeight = h;
		st.PendingClearanceFoundation = pBuildingType->Foundation;
		st.ClearanceAttempts = 0;
		st.NextClearanceCheckFrame = Unsorted::CurrentFrame + 60;
		st.IsBuildingRecoveryRefinery = false;
		st.TargetRecoveryRefineryZone = -1;
		st.NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 750;
		return true;
	};

	if (GetMacroState(pHouse).IsBuildingRecoveryRefinery && isStaticRefineryWithFreeHarv
		&& (GetMacroState(pHouse).TargetRecoveryRefineryZone >= 0
			|| ScenarioClass::Instance->Random.RandomRanged(0, 99) < 65))
	{
		if (tryReplaceExistingRefineryForHarvester())
		{
			return 1; // Keep building waiting in ConYard while old refinery finishes
					// selling!
		}
	}

	// Step 2: Normal Spatial Tetris placement search
	const CellStruct bestCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pBuildingType);

	if (bestCell != CellStruct::Empty)
	{
		auto& st = GetMacroState(pHouse);
		st.ConsecutivePlacementFailures = 0;
		st.BuildingPlacementCooldownUntilFrame.erase(pBuildingType->ArrayIndex);
		if (pBuildingType->IsBaseDefense)
		{
			bool defAntiInf = false;
			bool defAntiArmor = false;
			bool defAntiAir = false;
			bool defPureAA = false;
			bool defUnarmedBunker = false;
			GetDefenseCapabilities(pBuildingType, defAntiInf, defAntiArmor, defAntiAir, defPureAA, defUnarmedBunker);

			if (!defPureAA && st.FrontlineThreatNeedsDefenses > 0
				&& Unsorted::CurrentFrame <= st.FrontlineThreatActiveFrames)
			{
				st.FrontlineThreatNeedsDefenses--;
				if (st.FrontlineThreatNeedsDefenses <= 0)
				{
					st.FrontlineThreatNeedsDefenses = 0;
					st.FrontlineThreatCoords = CellStruct::Empty;
					st.FrontlineThreatBuildingCoords = CellStruct::Empty;
				}
			}
			else if (!st.AttackedBuildingsLIFO.empty())
			{
				bool poppedMatched = false;
				for (int i = static_cast<int>(st.AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)
				{
					const auto& rec = st.AttackedBuildingsLIFO[i];
					if ((rec.Threat == NeuralThreatCategory::Air && defAntiAir)
						|| (rec.Threat == NeuralThreatCategory::Vehicle && defAntiArmor)
						|| (rec.Threat == NeuralThreatCategory::Infantry && defAntiInf))
					{
						st.AttackedBuildingsLIFO.erase(st.AttackedBuildingsLIFO.begin() + i);
						poppedMatched = true;
						break;
					}
				}

				if (!poppedMatched)
					st.AttackedBuildingsLIFO.pop_back();
			}
		}
		else
		{
			// AdvAI Local Frontline Threat Detection on Building Placement (Building_Body_AdvAI.cpp:525-605):
			// If we just placed a non-defense building within 8.0 cells of an armed enemy building or unit,
			// immediately flag FrontlineThreatNeedsDefenses = 2 so the ConYard builds defenses facing it!
			double nearestThreatDistSq = 64.0;
			CellStruct nearestThreatCoords = CellStruct::Empty;

			for (auto const pEB : BuildingClass::Array)
			{
				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Owner == pHouse
					|| pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
				{
					continue;
				}

				auto const pEW = pEB->GetWeapon(0) ? pEB->GetWeapon(0)->WeaponType : nullptr;
				if ((!pEW || pEW->Damage <= 0) && !pEB->Type->IsBaseDefense && pEB->Occupants.Count == 0)
					continue;

				const CellStruct ec = pEB->GetMapCoords();
				const double dx = static_cast<double>(bestCell.X - ec.X);
				const double dy = static_cast<double>(bestCell.Y - ec.Y);
				const double dSq = dx * dx + dy * dy;
				if (dSq <= nearestThreatDistSq)
				{
					nearestThreatDistSq = dSq;
					nearestThreatCoords = ec;
				}
			}

			for (auto const pTechno : TechnoClass::Array)
			{
				if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || !pTechno->Owner || pTechno->Owner == pHouse
					|| pHouse->IsAlliedWith(pTechno->Owner) || pTechno->Owner->IsNeutral()
					|| pTechno->WhatAmI() == AbstractType::Building)
				{
					continue;
				}

				auto const pEW = pTechno->GetWeapon(0) ? pTechno->GetWeapon(0)->WeaponType : nullptr;
				if (!pEW || pEW->Damage <= 0)
					continue;

				const CellStruct tc = pTechno->GetMapCoords();
				const double dx = static_cast<double>(bestCell.X - tc.X);
				const double dy = static_cast<double>(bestCell.Y - tc.Y);
				const double dSq = dx * dx + dy * dy;
				if (dSq <= nearestThreatDistSq)
				{
					nearestThreatDistSq = dSq;
					nearestThreatCoords = tc;
				}
			}

			if (nearestThreatCoords != CellStruct::Empty)
			{
				st.FrontlineThreatCoords = nearestThreatCoords;
				st.FrontlineThreatBuildingCoords = bestCell;
				st.FrontlineThreatActiveFrames = Unsorted::CurrentFrame + 1800;
				st.FrontlineThreatNeedsDefenses = 2;
			}

			// AdvAI Cluster Congestion Avoidance: if >= 3 owned buildings touch this newly placed
			// footprint within 1 cell, record a short-term UnsafePlacementZone so subsequent buildings
			// spread out along streets instead of creating a deadlocked cluster!
			int touchingOwnedCount = 0;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || pOwnB == pBuilding || !pOwnB->Type)
					continue;

				const CellStruct oc = pOwnB->GetMapCoords();
				const int oW = pOwnB->Type->GetFoundationWidth();
				const int oH = pOwnB->Type->GetFoundationHeight(false);
				const int gapX = std::max(0,
					std::max(static_cast<int>(bestCell.X) - (static_cast<int>(oc.X) + oW - 1),
						static_cast<int>(oc.X) - (static_cast<int>(bestCell.X) + w - 1)));
				const int gapY = std::max(0,
					std::max(static_cast<int>(bestCell.Y) - (static_cast<int>(oc.Y) + oH - 1),
						static_cast<int>(oc.Y) - (static_cast<int>(bestCell.Y) + h - 1)));
				if (std::max(gapX, gapY) <= 1)
					touchingOwnedCount++;
			}

			if (touchingOwnedCount >= 3)
				RecordUnsafePlacementZone(pHouse, bestCell, 900);
		}

		if (st.IsBuildingRecoveryRefinery && isStaticRefineryWithFreeHarv)
		{
			st.TemporaryRecoveryRefineryPlacedFrame[pBuilding->UniqueID] = Unsorted::CurrentFrame;
			st.IsBuildingRecoveryRefinery = false;
			st.TargetRecoveryRefineryZone = -1;
			st.NextHarvesterRecoveryFrame = Unsorted::CurrentFrame + 750;
			Debug::Log("[NeuralAI] HarvesterRecovery: House %d placed temporary recovery "
					"refinery %s at (%d,%d). Will sell once FreeUnit harvester spawns.\n",
				pHouse->ArrayIndex, pBuildingType->ID, bestCell.X, bestCell.Y);
		}

		outCell = bestCell;
		return 2;
	}

	// Fallback for Refinery / Harvester Recovery when no free cell exists: sell
	// an existing Refinery and place in its exact spot!
	if (isStaticRefineryWithFreeHarv && tryReplaceExistingRefineryForHarvester())
		return 1;

	// Step 3: Surgical Space Clearance & Obsolete Structure Replacement when the
	// base is congested and no free cell exists!
	auto recordPlacementFailure = [&]() -> int
	{
		auto& st = GetMacroState(pHouse);
		st.ConsecutivePlacementFailures++;
		const int cdUntil = Unsorted::CurrentFrame + std::min(3600, 300 * st.ConsecutivePlacementFailures);
		st.BuildingPlacementCooldownUntilFrame[pBuildingType->ArrayIndex] = cdUntil;
		if (pBuildingType->PowerBonus > pBuildingType->PowerDrain && !pBuildingType->IsBaseDefense
			&& !pHouse->HasLowPower())
		{
			for (auto const pOtherBld : BuildingTypeClass::Array)
			{
				if (pOtherBld && pOtherBld->PowerBonus > pOtherBld->PowerDrain && !pOtherBld->IsBaseDefense)
					st.BuildingPlacementCooldownUntilFrame[pOtherBld->ArrayIndex] = cdUntil;
			}
		}

		if (FactoryClass* pBldFac
			= pHouse->GetPrimaryFactory(AbstractType::BuildingType, pBuildingType->Naval, pBuildingType->BuildCat))
		{
			pBldFac->AbandonProduction();
		}

		return 0;
	};

	if (Unsorted::CurrentFrame < GetMacroState(pHouse).LastClearanceFailedFrame + 150)
		return recordPlacementFailure();

	const int money = pHouse->Available_Money();
	const bool isCombatDef = pBuildingType->IsBaseDefense;

	const bool isLateGameHighTierDefense
		= isCombatDef && pBuildingType->TechLevel >= 6 && pBuildingType->Cost >= 1000 && money > 4000;

	if (isCombatDef && !isLateGameHighTierDefense)
	{
		return recordPlacementFailure(); // Only sacrifice structures for key
										// non-defense buildings or
	}

	// late-game high-tier defense upgrades
	bool hasUnpowered = false;
	int poweredDefAndSWCount = 0;
	int maxSinglePlantBonus = 0;
	bool hasAdvancedPowerPlants = false;

	const int realDrain = GetRealPowerDrainAndGridMetrics(
		pHouse, hasUnpowered, poweredDefAndSWCount, maxSinglePlantBonus, hasAdvancedPowerPlants);

	const int realSurplus = pHouse->PowerOutput - realDrain;

	const int requiredCushion
		= (poweredDefAndSWCount > 0) ? std::max(300, std::min(550, 220 + poweredDefAndSWCount * 45)) : 150;

	// Strict Silo Rule: NEVER sell Ore Silos if the house has any stored
	// Tiberium/Ore (because selling a Silo with ore destroys the stored credits!)
	const bool canSacrificeEmptySilos = (pHouse->OwnedTiberium.GetTotalAmount() <= 0);
	int ownedRefineries = 0;

	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->Type
			&& (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination))
		{
			ownedRefineries++;
		}
	}

	const CellStruct baseCenter = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		? pHouse->ConYards.GetItem(0)->GetMapCoords()
		: (pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell);

	if (baseCenter == CellStruct::Empty)
		return 0;

	const int adjRange = std::max(1, pBuildingType->Adjacent + 1);
	CellStruct bestClearanceCell = CellStruct::Empty;

	std::vector<BuildingClass*> bestToSell;

	int bestSacrificeCost = 999999;

	for (int dy = -20; dy <= 20; ++dy)
	{
		for (int dx = -20; dx <= 20; ++dx)
		{
			const CellStruct cand { static_cast<short>(baseCenter.X + dx), static_cast<short>(baseCenter.Y + dy) };

			if (!MapClass::Instance.CoordinatesLegal(cand))
				continue;

			if (OverlapsTiberiumTreeZone(cand, pBuildingType) || OverlapsBridge(cand, pBuildingType))
				continue;

			bool terrainAndUnitsValid = true;

			std::vector<BuildingClass*> obstructingBlds;

			int sacrificeScore = 0;
			int lostPowerBonus = 0;

			for (int fy = 0; fy < h && terrainAndUnitsValid; ++fy)
			{
				for (int fx = 0; fx < w; ++fx)
				{
					const CellStruct fc { static_cast<short>(cand.X + fx), static_cast<short>(cand.Y + fy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(fc);

					if (!pCell || pCell->GetUnit(false) || pCell->GetInfantry(false))
					{
						terrainAndUnitsValid = false;

						break;
					}

					if (pBuildingType->Naval)
					{
						if (pCell->LandType != LandType::Water)
						{
							terrainAndUnitsValid = false;

							break;
						}
					}
					else if (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach
						|| pCell->LandType == LandType::Rock || pCell->ContainsBridge())
					{
						terrainAndUnitsValid = false;

						break;
					}

					if (pCell->OverlayTypeIndex >= 0)
					{
						if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
						{
							if (pOv->Wall && pCell->WallOwnerIndex != pHouse->ArrayIndex)
							{
								terrainAndUnitsValid = false;

								break;
							}
						}
					}

					if (auto const pOcc = pCell->GetBuilding())
					{
						if (pOcc->Owner != pHouse || !pOcc->IsAlive || pOcc->InLimbo || !pOcc->Type
							|| pOcc->Type->ToProtect || pOcc->Type->ConstructionYard
							|| pOcc->CurrentMission == Mission::Construction
							|| pOcc->CurrentMission == Mission::Selling)
						{
							terrainAndUnitsValid = false;

							break;
						}

						if (std::find(obstructingBlds.begin(), obstructingBlds.end(), pOcc) == obstructingBlds.end())
						{
							auto const pOT = pOcc->Type;
							int itemCost = -1;

							// Priority 0: Obstructing AI-owned Wall / LaserFence / FirestormWall
							if (pOT->Wall || pOT->FirestormWall || pOT->LaserFence || pOT->LaserFencePost)
							{
								itemCost = 10;
							}

							// Priority 1: Exhausted or redundant idle Refinery
							else if ((pOT->Refinery || pOT->ResourceDestination) && !pOT->ResourceGatherer
								&& !pOT->UndeploysInto && !pOcc->HasAnyLink()
								&& (IsRefineryExhausted(pOcc, baseCenter)
									|| (ownedRefineries > 2 && pBuildingType->Refinery)))
							{
								itemCost = 100;
							}

							// Priority 2: Empty Ore Silo (STRICTLY ONLY when stored Tiberium
							// == 0!)
							else if (pOT->Storage > 0 && !pOT->Refinery && !pOT->ResourceGatherer
								&& canSacrificeEmptySilos)
							{
								itemCost = 250;
							}

							// Priority 3: Redundant Basic Power Plant when Advanced Reactors
							// exist and power cushion stays safe
							else if (pOT->PowerBonus > pOT->PowerDrain && pOT->PowerBonus <= 200 && !pOT->IsBaseDefense
								&& hasAdvancedPowerPlants
								&& (realSurplus - (lostPowerBonus + pOT->PowerBonus) - pBuildingType->PowerDrain)
									>= requiredCushion)
							{
								itemCost = 400;

								lostPowerBonus += pOT->PowerBonus;
							}

							// Priority 4: Obstructing Tier-1 Defense (for non-defense
							// infrastructure or late-game Tier-2/3 defense upgrade)
							else if (pOT->IsBaseDefense && !IsTunnelNetworkBuilding(pOT)
								&& (!isCombatDef
									|| (isLateGameHighTierDefense && pBuildingType->Cost >= pOT->Cost + 450)))
							{
								itemCost = 600 + (pOT->Cost / 4);
							}

							if (itemCost < 0)
							{
								terrainAndUnitsValid = false;

								break;
							}

							obstructingBlds.push_back(pOcc);

							sacrificeScore += itemCost;
						}
					}
					else if (!pCell->CanThisExistHere(pBuildingType->SpeedType, pBuildingType, pHouse))
					{
						terrainAndUnitsValid = false;

						break;
					}
				}
			}

			if (!terrainAndUnitsValid || obstructingBlds.empty() || obstructingBlds.size() > 2)
				continue;

			// Verify that after selling obstructingBlds, cand still has at least one
			// surviving BaseNormal / ConstructionYard anchor within adjRange!
			bool hasSurvivingAnchor = false;

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || !pOwnBld->IsAlive || pOwnBld->InLimbo || !pOwnBld->Type)
					continue;

				if (std::find(obstructingBlds.begin(), obstructingBlds.end(), pOwnBld) != obstructingBlds.end())
					continue;

				if (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard
					&& !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
				{
					continue;
				}

				const CellStruct ac = pOwnBld->GetMapCoords();
				const int aW = pOwnBld->Type->GetFoundationWidth();
				const int aH = pOwnBld->Type->GetFoundationHeight(false);

				const int gapX = std::max(0,
					std::max(static_cast<int>(cand.X) - (static_cast<int>(ac.X) + aW - 1),
						static_cast<int>(ac.X) - (static_cast<int>(cand.X) + w - 1)));

				const int gapY = std::max(0,
					std::max(static_cast<int>(cand.Y) - (static_cast<int>(ac.Y) + aH - 1),
						static_cast<int>(ac.Y) - (static_cast<int>(cand.Y) + h - 1)));

				if (std::max(gapX, gapY) <= adjRange)
				{
					hasSurvivingAnchor = true;

					break;
				}
			}

			if (!hasSurvivingAnchor)
				continue;

			const int totalScore = sacrificeScore + static_cast<int>(obstructingBlds.size()) * 500
				+ (std::abs(cand.X - baseCenter.X) + std::abs(cand.Y - baseCenter.Y)) * 4;

			if (totalScore < bestSacrificeCost)
			{
				bestSacrificeCost = totalScore;

				bestClearanceCell = cand;

				bestToSell = std::move(obstructingBlds);
			}
		}
	}

	if (bestClearanceCell != CellStruct::Empty && !bestToSell.empty())
	{
		for (auto const pObs : bestToSell)
		{
			if (pObs && pObs->IsAlive && !pObs->InLimbo)
			{
				Debug::Log("[NeuralAI] House %d selling obstructing %s at (%d,%d) to "
						"clear space for %s at (%d,%d).\n",
					pHouse->ArrayIndex, pObs->Type->ID, pObs->GetMapCoords().X, pObs->GetMapCoords().Y,
					pBuildingType->ID, bestClearanceCell.X, bestClearanceCell.Y);

				pObs->Sell(1);
			}
		}

		for (int fy = 0; fy < h; ++fy)
		{
			for (int fx = 0; fx < w; ++fx)
			{
				const CellStruct fc { static_cast<short>(bestClearanceCell.X + fx),
					static_cast<short>(bestClearanceCell.Y + fy) };

				if (auto const pCell = MapClass::Instance.TryGetCellAt(fc))
				{
					if (pCell->OverlayTypeIndex >= 0 && pCell->WallOwnerIndex == pHouse->ArrayIndex)
					{
						if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
						{
							if (pOv->Wall)
							{
								CellStruct wallCell = fc;
								pHouse->SellWall(wallCell, true);
							}
						}
					}

					pCell->ScatterContent(CellClass::Cell2Coord(fc), true, true, false);
				}
			}
		}

		GetMacroState(pHouse).PendingClearanceCell = bestClearanceCell;

		GetMacroState(pHouse).PendingClearanceType = pBuildingType;

		GetMacroState(pHouse).PendingClearanceFoundationWidth = w;

		GetMacroState(pHouse).PendingClearanceFoundationHeight = h;

		GetMacroState(pHouse).PendingClearanceFoundation = pBuildingType->Foundation;

		GetMacroState(pHouse).ClearanceAttempts = 0;

		GetMacroState(pHouse).NextClearanceCheckFrame = Unsorted::CurrentFrame + 65;

		return 1; // Keep building waiting in Construction Yard while obstructing

		// structure sells!
	}

	GetMacroState(pHouse).LastClearanceFailedFrame = Unsorted::CurrentFrame;

	return recordPlacementFailure();
}

