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
}

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

			if (CCINIClass::INI_Rules->ReadString(pBldType->ID, "Tunnel", "", tunnelBuf, sizeof(tunnelBuf)) > 0 && !INIClass::IsBlank(tunnelBuf))
				isTunnel = true;
		}

		if (!isTunnel)

		{

			if (pBldType->Passengers > 0 && (pBldType->InfantryAbsorb || pBldType->UnitAbsorb) && pBldType->PowerBonus == 0 && !pBldType->Bunker && !pBldType->CanBeOccupied && !pBldType->Grinding &&

				!pBldType->Hospital && !pBldType->Armory)

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

	bool IsTunnelDeliverySuperWeapon(const SuperWeaponTypeClass* pSWType, HouseClass* pOwner)

	{

		if (!pSWType)
			return false;

		if (pSWType->ID && (std::strstr(pSWType->ID, "Tunnel") != nullptr || std::strstr(pSWType->ID, "TUNNEL") != nullptr))
			return true;

		if (pOwner)

		{

			for (auto const pBld : pOwner->Buildings)

			{

				if (pBld && !pBld->InLimbo && pBld->IsAlive && pBld->Type && IsTunnelNetworkBuilding(pBld->Type))

				{

					if (pBld->Type->SuperWeapon == pSWType->ArrayIndex || pBld->Type->SuperWeapon2 == pSWType->ArrayIndex)
						return true;
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

				if (pTerrain && pTerrain->IsAlive && !pTerrain->InLimbo && pTerrain->Type && pTerrain->Type->SpawnsTiberium)

				{

					auto const pTerrainTypeExt = TerrainTypeExt::Fetch(pTerrain->Type);

					const short range = static_cast<short>(pTerrainTypeExt ? std::max(1, pTerrainTypeExt->SpawnsTiberium_Range.Get()) : 1);

					const CellStruct treeCell = pTerrain->GetMapCoords();

					s_CachedTrees.push_back({treeCell.X, treeCell.Y, range});
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

			if (minX <= tree.X + tree.Range && maxX >= tree.X - tree.Range && minY <= tree.Y + tree.Range && maxY >= tree.Y - tree.Range)
				return true;
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

				const CellStruct checkCell{static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy)};

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

		const int gapRadius = (pBldExt && pBldExt->GapRadiusInCells != 0) ? pBldExt->GapRadiusInCells.Get() : pType->GapRadiusInCells;

		const int superGapRadius = (pBldExt && pBldExt->SuperGapRadiusInCells != 0) ? pBldExt->SuperGapRadiusInCells.Get() : pType->SuperGapRadiusInCells;

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

			if (pTechExt->InhibitorRange.isset() && pTechExt->InhibitorRange.Get() > 0 && pTechExt->InhibitorRange.Get() < minRadius)
				minRadius = pTechExt->InhibitorRange.Get();

			if (pTechExt->RadarJamRadius.Get() > 0 && pTechExt->RadarJamRadius.Get() < minRadius)
				minRadius = pTechExt->RadarJamRadius.Get();
		}

		return minRadius == 999 ? 0 : minRadius;
	}

	// Calculates real power drain (including toggled-off structures) and power grid

	// vulnerability metrics

	int GetRealPowerDrainAndGridMetrics(HouseClass* pHouse, bool& outHasUnpoweredBuildings, int& outPoweredDefAndSWCount, int& outMaxSinglePlantBonus, bool& outHasAdvancedPowerPlants)

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

		if ((!pBld->Type->Refinery && !pBld->Type->ResourceDestination) || pBld->Type->ResourceGatherer || pBld->Type->Enslaves != nullptr || pBld->Type->UndeploysInto != nullptr)
			return false;

		if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling || pBld->CurrentMission == Mission::Construction || pBld->QueuedMission == Mission::Construction)
			return false;

		const CellStruct refCoords = pBld->GetMapCoords();

		if (baseCenter != CellStruct::Empty && refCoords.DistanceFrom(baseCenter) < 20.0)
			return false;

		for (int dy = -27; dy <= 27; ++dy)

		{

			for (int dx = -27; dx <= 27; ++dx)

			{

				if (dx * dx + dy * dy > 729)
					continue;

				const CellStruct scanCell{static_cast<short>(refCoords.X + dx), static_cast<short>(refCoords.Y + dy)};

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

			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && (pBld->Type->Refinery || pBld->Type->ResourceDestination) && !pBld->Type->ResourceGatherer)
				staticRefineryCount++;
		}

		// Never sell our last 2 refineries proactively

		if (staticRefineryCount <= 2)
			return;

		const CellStruct baseCenter = pHouse->ConYards.GetItem(0)->GetMapCoords();

		for (auto const pBld : pHouse->Buildings)

		{

			if (IsRefineryExhausted(pBld, baseCenter) && !pBld->HasAnyLink())

			{

				Debug::Log("[NeuralAI] House %d recycling exhausted distant refinery %s at (%d,%d).\n", pHouse->ArrayIndex, pBld->Type->ID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y);

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

			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->ToProtect || pBld->Type->ConstructionYard)
				continue;

			if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling || pBld->CurrentMission == Mission::Construction)
				continue;

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

			Debug::Log("[NeuralAI] House %d in total economic emergency (0 Refineries, 0 Harvesters, %d credits): selling %s to fund recovery!\n", pHouse->ArrayIndex, money, pBestToSell->Type->ID);

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

		CellStruct BuildingCoords{CellStruct::Empty};

		CellStruct AttackerCoords{CellStruct::Empty};

		TechnoTypeClass* AttackerType{nullptr};

		NeuralThreatCategory Threat{NeuralThreatCategory::None};

		int AttackFrame{0};
	};

	struct NeuralHouseMacroState

	{

		CellStruct PendingClearanceCell{CellStruct::Empty};

		BuildingTypeClass* PendingClearanceType{nullptr};

		int PendingClearanceFoundationWidth{0};

		int PendingClearanceFoundationHeight{0};

		Foundation PendingClearanceFoundation{Foundation::_1x1};

		int ClearanceAttempts{0};

		int NextClearanceCheckFrame{0};

		int LastClearanceFailedFrame{-1000};
		int ConsecutivePlacementFailures{0};
		std::map<int, int> BuildingPlacementCooldownUntilFrame{};

		std::vector<NeuralAttackedBuildingRecord> AttackedBuildingsLIFO{};

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
			this->BuildingPlacementCooldownUntilFrame.clear();
			this->AttackedBuildingsLIFO.clear();
		}
	};

	std::array<NeuralHouseMacroState, 32> MacroStates{};

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

	void PruneAttackedBuildingsLIFO(HouseClass* pHouse)

	{

		if (!pHouse)
			return;

		auto& state = GetMacroState(pHouse);

		if (state.AttackedBuildingsLIFO.empty())
			return;

		state.AttackedBuildingsLIFO.erase(std::remove_if(state.AttackedBuildingsLIFO.begin(), state.AttackedBuildingsLIFO.end(),

											[&](const NeuralAttackedBuildingRecord& rec)

											{
												if (Unsorted::CurrentFrame - rec.AttackFrame > 1800)
													return true;

												auto const pCell = MapClass::Instance.TryGetCellAt(rec.BuildingCoords);

												if (!pCell)
													return true;

												auto const pBld = pCell->GetBuilding();

												return !pBld || !pBld->IsAlive || pBld->InLimbo || (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner));
											}),

			state.AttackedBuildingsLIFO.end());
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
				if (!isNaval && type != AbstractType::AircraftType && !AreZonesConnected(pBld->GetMapCoords(), goalCoords, MovementZone::Normal))
					continue;

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

					if (pBld && pBld->IsAlive && pBld->Type && pBld->Type->Factory == type && pBld->Type->Naval == isNaval && pBld->Factory != nullptr)

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
		const CellStruct homeCenter = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
		PruneAttackedBuildingsLIFO(pHouse);
		auto& macroState = GetMacroState(pHouse);
		CellStruct defenseGoal = CellStruct::Empty;
		if (!macroState.AttackedBuildingsLIFO.empty() && (Unsorted::CurrentFrame - macroState.AttackedBuildingsLIFO.back().AttackFrame) <= 270)
			defenseGoal = macroState.AttackedBuildingsLIFO.back().BuildingCoords;

		CellStruct vehGoal = (defenseGoal != CellStruct::Empty) ? defenseGoal : targetCoords;
		const UnitTypeClass* pCurUnitType = nullptr;
		if (FactoryClass* pVF = pHouse->GetPrimaryFactory(AbstractType::UnitType, false, BuildCat::DontCare))
		{
			if (pVF->Object)
				pCurUnitType = abstract_cast<UnitClass*>(pVF->Object) ? abstract_cast<UnitClass*>(pVF->Object)->Type : nullptr;
		}
		if (!pCurUnitType && pExt && pExt->AIBrain_NextUnitTarget >= 0 && pExt->AIBrain_NextUnitTarget < UnitTypeClass::Array.Count)
			pCurUnitType = UnitTypeClass::Array.GetItem(pExt->AIBrain_NextUnitTarget);
		if (!pCurUnitType && pHouse->ProducingUnitTypeIndex >= 0 && pHouse->ProducingUnitTypeIndex < UnitTypeClass::Array.Count)
			pCurUnitType = UnitTypeClass::Array.GetItem(pHouse->ProducingUnitTypeIndex);

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
						if (pU && pU->Owner == pHouse && pU->IsAlive && !pU->InLimbo && pU->Type && (pU->Type->Harvester || pU->Type->ResourceGatherer))
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
			else if (defenseGoal == CellStruct::Empty && homeCenter != CellStruct::Empty && pCurUnitType->DeploysInto != nullptr &&
				(pCurUnitType->DeploysInto->ConstructionYard || pCurUnitType->DeploysInto->BaseNormal || pCurUnitType->DeploysInto->Refinery))
			{
				CellStruct bestExpOre = CellStruct::Empty;
				int bestOreDist = 999999;
				for (int dy = -48; dy <= 48; dy += 4)
				{
					for (int dx = -48; dx <= 48; dx += 4)
					{
						const CellStruct tc{static_cast<short>(homeCenter.X + dx), static_cast<short>(homeCenter.Y + dy)};
						auto const pC = MapClass::Instance.TryGetCellAt(tc);
						if (!pC || pC->GetContainedTiberiumValue() <= 0)
							continue;
						bool nearHub = false;
						for (auto const pOwnB : pHouse->Buildings)
						{
							if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->Type && (pOwnB->Type->Refinery || pOwnB->Type->ConstructionYard || pOwnB->Type->BaseNormal))
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
				pCurInfType = abstract_cast<InfantryClass*>(pIF->Object) ? abstract_cast<InfantryClass*>(pIF->Object)->Type : nullptr;
		}
		if (!pCurInfType && pExt && pExt->AIBrain_NextInfantryTarget >= 0 && pExt->AIBrain_NextInfantryTarget < InfantryTypeClass::Array.Count)
			pCurInfType = InfantryTypeClass::Array.GetItem(pExt->AIBrain_NextInfantryTarget);
		if (!pCurInfType && pHouse->ProducingInfantryTypeIndex >= 0 && pHouse->ProducingInfantryTypeIndex < InfantryTypeClass::Array.Count)
			pCurInfType = InfantryTypeClass::Array.GetItem(pHouse->ProducingInfantryTypeIndex);

		if (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->InfantryAbsorb &&
					(pBld->Type->PowerBonus > 0 || pBld->Type->ExtraPowerBonus > 0) && pBld->Passengers.NumPassengers < pBld->Type->Passengers)
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

	const char* basePath = (GetFileAttributesA("Resources\\AI\\ai_base.bin") != INVALID_FILE_ATTRIBUTES) ? "Resources\\AI\\ai_base.bin" : "AI\\ai_base.bin";

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

			pHdr->ExpertWeightsOffset[e] = offset;

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

		pHdr->BaseModelHash = HashBytesFNV1a(buffer.data() + sizeof(AIBaseHeader), totalBootstrapSize - sizeof(AIBaseHeader));

		HANDLE hOut = CreateFileA(basePath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);

		if (hOut != INVALID_HANDLE_VALUE)

		{

			DWORD written = 0;

			WriteFile(hOut, buffer.data(), totalBootstrapSize, &written, NULL);

			CloseHandle(hOut);
		}
	}

	NeuralAI::BaseFileHandle = CreateFileA(basePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (NeuralAI::BaseFileHandle == INVALID_HANDLE_VALUE)
		return;

	LARGE_INTEGER fileSize = {};

	if (!GetFileSizeEx(NeuralAI::BaseFileHandle, &fileSize) || fileSize.QuadPart < static_cast<LONGLONG>(sizeof(AIBaseHeader)))

	{

		CloseHandle(NeuralAI::BaseFileHandle);

		NeuralAI::BaseFileHandle = INVALID_HANDLE_VALUE;

		return;
	}

	NeuralAI::BaseMappedSize = static_cast<size_t>(fileSize.QuadPart);

	NeuralAI::BaseMappingHandle = CreateFileMappingA(NeuralAI::BaseFileHandle, NULL, PAGE_READONLY, 0, 0, "Local\\PhobosAIBaseSharedMapping");

	if (!NeuralAI::BaseMappingHandle)
		NeuralAI::BaseMappingHandle = CreateFileMappingA(NeuralAI::BaseFileHandle, NULL, PAGE_READONLY, 0, 0, NULL);

	if (NeuralAI::BaseMappingHandle)
		NeuralAI::BaseMappedView = static_cast<const uint8_t*>(MapViewOfFile(NeuralAI::BaseMappingHandle, FILE_MAP_READ, 0, 0, 0));
}

bool NeuralAI::LoadBackpackFile(const char* pPath, AIBackpackFile& outBackpack, bool requireSignature)

{

	HANDLE hFile = CreateFileA(pPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	AIBackpackFile loaded = {};

	DWORD bytesRead = 0;

	const bool valid = ReadFile(hFile, &loaded, sizeof(AIBackpackFile), &bytesRead, NULL) && bytesRead == sizeof(AIBackpackFile) && std::memcmp(loaded.Magic, "PHOBOSBP", 8) == 0 &&

					loaded.SchemaVersion == 3 && (!requireSignature || loaded.BaseSignature == NeuralAI::CurrentBaseSignature);

	CloseHandle(hFile);

	if (valid)
		outBackpack = loaded;

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

		std::snprintf(policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_slot_%d.dat", NeuralAI::TrainSlotIndex);

		const bool challengerLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::Backpack, false);

		std::memcpy(&NeuralAI::OpponentBackpack, &NeuralAI::Backpack, sizeof(AIBackpackFile));

		std::snprintf(policyPath, sizeof(policyPath), "Resources\\AI\\ai_backpack_opp_slot_%d.dat", NeuralAI::TrainSlotIndex);

		NeuralAI::OpponentLoaded = NeuralAI::LoadBackpackFile(policyPath, NeuralAI::OpponentBackpack, false);

		NeuralAI::AllowBackpackInference = true;

		NeuralAI::BackpackLoaded = true;

		Debug::Log("[NeuralAI] Headless policies: challenger=%s opponent=%s\n", challengerLoaded ? "loaded" : "default", NeuralAI::OpponentLoaded ? "loaded" : "shared");

		return;
	}

	NeuralAI::AllowBackpackInference = true;

	const char* bpPath = (GetFileAttributesA("Resources\\AI\\ai_backpack.dat") != INVALID_FILE_ATTRIBUTES) ? "Resources\\AI\\ai_backpack.dat" : "AI\\ai_backpack.dat";

	const bool valid = NeuralAI::LoadBackpackFile(bpPath, NeuralAI::Backpack, true);

	if (!valid)

	{

		std::memset(&NeuralAI::Backpack, 0, sizeof(AIBackpackFile));

		std::memcpy(NeuralAI::Backpack.Magic, "PHOBOSBP", 8);

		NeuralAI::Backpack.SchemaVersion = 3;

		NeuralAI::Backpack.BaseSignature = NeuralAI::CurrentBaseSignature;

		NeuralAI::Backpack.EstimatedELO = 1200;

		NeuralAI::Backpack.BackpackHash = HashBytesFNV1a(&NeuralAI::Backpack.LoRA_A[0], sizeof(NeuralAI::Backpack.LoRA_A));

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

	NeuralAI::Backpack.BackpackHash = HashBytesFNV1a(&NeuralAI::Backpack.LoRA_A[0], sizeof(NeuralAI::Backpack.LoRA_A) + sizeof(NeuralAI::Backpack.LoRA_B));

	char bpPath[MAX_PATH];

	if (NeuralAI::HeadlessMode && NeuralAI::TrainSlotIndex > 0)
		std::snprintf(bpPath, sizeof(bpPath),

			"Resources\\AI\\ai_backpack_slot_%d.dat", NeuralAI::TrainSlotIndex);

	else
		std::snprintf(bpPath, sizeof(bpPath), "Resources\\AI\\ai_backpack.dat");

	HANDLE hFile = CreateFileA(bpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (hFile != INVALID_HANDLE_VALUE)

	{

		DWORD written = 0;

		WriteFile(hFile, &NeuralAI::Backpack, sizeof(AIBackpackFile), &written, NULL);

		CloseHandle(hFile);
	}

	if (!NeuralAI::HeadlessMode || NeuralAI::TrainSlotIndex == 0)

	{

		HANDLE hFileAlt = CreateFileA("AI\\ai_backpack.dat", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

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

		TelemetryJsonShmHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, TelemetryJsonShmCapacity, jsonShmName);

		if (TelemetryJsonShmHandle)

		{

			TelemetryJsonView = static_cast<uint8_t*>(MapViewOfFile(TelemetryJsonShmHandle, FILE_MAP_ALL_ACCESS, 0, 0, TelemetryJsonShmCapacity));

			if (TelemetryJsonView)
				std::memset(TelemetryJsonView, 0, 16);
		}
	}

	if (NeuralAI::TelemetryView)
		return;

	char shmName[128];

	std::snprintf(shmName, sizeof(shmName), "Local\\PhobosAITrainerShm_%d", NeuralAI::TrainSlotIndex);

	NeuralAI::TelemetryShmHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(AITrainerSharedTelemetry), shmName);

	if (NeuralAI::TelemetryShmHandle)

	{

		NeuralAI::TelemetryView = static_cast<AITrainerSharedTelemetry*>(MapViewOfFile(NeuralAI::TelemetryShmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AITrainerSharedTelemetry)));

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

	static const wchar_t* const ArchetypeNamesW[5] = {L"Balanced Grandmaster", L"SpecOps & Infiltration", L"Tech & Fortress Turtle", L"Blitzkrieg Rush", L"Air & Artillery Siege"};

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

				const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + a * 8) % 512);

				const int memBonus = policy.Entries[eIdx].VisitCount > 0 ? static_cast<int>(policy.Entries[eIdx].ArchetypeScoreDelta[a]) * 6 : 0;

				const int exploreJitter = ScenarioClass::Instance->Random.RandomRanged(-90, 90);

				const int aScore = static_cast<int>(policy.OutputBias[48 + a]) + memBonus + exploreJitter;

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

		const int archIdx = std::clamp(pExt->AIBrain_Archetype, 0, 4);

		Debug::Log("[NeuralAI] Activated for House #%d (%s, Diff=%d) -> Archetype=%d (%ls)\n", pHouse->ArrayIndex, countryId, pHouse->GetAIDifficultyIndex(), archIdx, ArchetypeNamesW[archIdx]);

		if (!NeuralAI::HeadlessMode)

		{

			wchar_t msgBuf[256];

			std::swprintf(msgBuf, 256, L"[Neural AI] House #%d (%hs) -> Style: %ls", pHouse->ArrayIndex, countryId, ArchetypeNamesW[archIdx]);

			MessageListClass::Instance.PrintMessage(msgBuf, RulesClass::Instance->MessageDelay, HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : 0, true);
		}
	}

	if (activeAIHouses > 0)

	{

		Debug::Log("[NeuralAI] ScenarioStart: %d AI Houses controlled by NeuralAI (aimd.ini bypassed) | BaseSignature=%016llX | ELO=%d\n", activeAIHouses, NeuralAI::CurrentBaseSignature,

			NeuralAI::Backpack.EstimatedELO);

		if (!NeuralAI::HeadlessMode)

		{

			wchar_t bannerBuf[256];

			std::swprintf(bannerBuf, 256,

				L"[Phobos Neural AI] ACTIVE (%d AI Houses | Brain Sig:%08X | ELO:%d "

				L"| aimd.ini bypassed)",

				activeAIHouses, static_cast<uint32_t>(NeuralAI::CurrentBaseSignature & 0xFFFFFFFFULL), NeuralAI::Backpack.EstimatedELO);

			MessageListClass::Instance.PrintMessage(bannerBuf, RulesClass::Instance->MessageDelay, HouseClass::CurrentPlayer ? HouseClass::CurrentPlayer->ColorSchemeIndex : 0, true);
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

		const int score = (pHouse->TotalKilledUnits * 200) + (pHouse->TotalKilledBuildings * 450) + (pHouse->OwnedBuildings * 300) + (pHouse->OwnedUnits * 200);

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

			const int exp = std::clamp(pExt->AIBrain_ActiveExpert, 0, 7);

			const int fastBonus = (!NeuralAI::MatchEndStalemate && won && Unsorted::CurrentFrame > 0 && Unsorted::CurrentFrame <= 18000) ? 4 : 0;

			const int delta = NeuralAI::MatchEndStalemate ? (won ? -4 : -8) : (won ? (12 + fastBonus) : -6);

			uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + arch * 8 + exp) % 512);

			if (NeuralAI::Backpack.NumEntries < 512)
				NeuralAI::Backpack.NumEntries = std::max(NeuralAI::Backpack.NumEntries, entryIdx + 1);

			auto& entry = NeuralAI::Backpack.Entries[entryIdx];

			entry.StateClusterHash = (static_cast<uint32_t>(pHouse->SideIndex) << 16) | static_cast<uint32_t>(arch << 8) | static_cast<uint32_t>(exp);

			entry.ArchetypeScoreDelta[arch] = ClampInt16(entry.ArchetypeScoreDelta[arch] + delta);

			entry.ExpertScoreDelta[exp] = ClampInt16(entry.ExpertScoreDelta[exp] + delta);

			entry.VisitCount++;

			// Update LoRA rank-16 adapter weights deterministically

			const int loraIdx = (arch * 64 + exp * 8) % (256 * 16);

			NeuralAI::Backpack.LoRA_A[loraIdx] = ClampInt8(NeuralAI::Backpack.LoRA_A[loraIdx] + (won ? 2 : -1));
		}

		NeuralAI::SaveBackpack();
	}

	if (NeuralAI::HeadlessMode)
		NeuralAI::UpdateTelemetrySnapshot(true,

			NeuralAI::MatchEndWinnerHouse >= 0 ? NeuralAI::MatchEndWinnerHouse : bestHouseIdx, NeuralAI::MatchEndStalemate);
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

		if (pAttacker->IsInAir() || pAttacker->WhatAmI() == AbstractType::Aircraft)
			threat = NeuralThreatCategory::Air;

		else if (pAttacker->WhatAmI() == AbstractType::Infantry)
			threat = NeuralThreatCategory::Infantry;

		else
			threat = NeuralThreatCategory::Vehicle;
	}

	else if (pAttackerHouse && pAttackerHouse->ConYards.Count > 0)

	{

		attackerCoords = pAttackerHouse->ConYards.GetItem(0)->GetMapCoords();
	}

	auto it = std::find_if(state.AttackedBuildingsLIFO.begin(), state.AttackedBuildingsLIFO.end(), [&](const NeuralAttackedBuildingRecord& r) { return r.BuildingCoords == bldCoords; });

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

		NeuralAI::CurrentMeasuredFPS = (NeuralAI::FramesSinceLastFPSCheck * 1000) / std::max<DWORD>(1, now - NeuralAI::LastFPSCheckTick);

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

		totalKillsAndCaptures += pHouse->TotalKilledUnits + pHouse->TotalKilledBuildings + pHouse->StolenBuildingsCredits;

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

			if (!aliveCombatList[i]->IsAlliedWith(aliveCombatList[j]) || !aliveCombatList[j]->IsAlliedWith(aliveCombatList[i]))

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

	const bool stalemate = !onlyOneSideLeft && ((frame > 18000 && (frame - NeuralAI::LastCombatFrame) >= noCombatLimit) || (frame >= maxMatchFrames));

	if (stalemate || onlyOneSideLeft)

	{

		int bestHouseIdx = lastAliveHouseIdx;

		int bestScore = -1;

		for (auto const pHouse : HouseClass::Array)

		{

			if (!pHouse || pHouse->IsNeutral() || pHouse->IsObserver() || pHouse->Defeated)
				continue;

			const int score = (pHouse->TotalKilledUnits * 200) + (pHouse->TotalKilledBuildings * 450) + (pHouse->OwnedBuildings * 300) + (pHouse->OwnedUnits * 200);

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

		hs.NetWorthScore = (hs.KilledUnits * 200) + (hs.KilledBuildings * 450) + (hs.OwnedBuildings * 300) + (hs.OwnedUnits * 200);

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

		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame || pBld->Owner->IsNeutral())
			continue;

		if (pBld->Type->CanBeOccupied && !pBld->Type->BaseNormal && !pBld->Type->ConstructionYard && pBld->Type->TechLevel < 0)
			continue;

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

			const float isoX = static_cast<float>((static_cast<int>(c.X) - static_cast<int>(c.Y) + mapW - 1) - (2 * localX));

			const float isoY = static_cast<float>((static_cast<int>(c.X) + static_cast<int>(c.Y) - mapW - 1) - (2 * localY));

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
			"{\n  \"slot\": %u,\n  \"pid\": %u,\n  \"frame\": %u,\n  \"fps\": %u,\n  \"finished\": %s,\n  \"winnerHouse\": %d,\n  \"stalemate\": %s,\n  \"elo\": %d,\n  \"gamesPlayed\": %u,\n  "
			"\"rulesSignature\": \"%016llX\",\n  \"baseSignature\": \"%016llX\",\n  \"houses\": [\n",
			dst->SlotIndex, dst->ProcessId, dst->CurrentFrame, dst->FramesPerSecond, matchEnded ? "true" : "false", dst->WinnerHouseIndex, stalemate ? "true" : "false",
			NeuralAI::Backpack.EstimatedELO, NeuralAI::Backpack.GamesPlayed, static_cast<unsigned long long>(NeuralAI::CurrentRulesSignature),
			static_cast<unsigned long long>(NeuralAI::CurrentBaseSignature));

		if (lineLen > 0)
			jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

		for (uint32_t i = 0; i < dst->NumHouses; ++i)

		{

			const auto& h = dst->Houses[i];

			auto const pOwner = (h.ArrayIndex >= 0 && h.ArrayIndex < HouseClass::Array.Count) ? HouseClass::Array.GetItem(h.ArrayIndex) : nullptr;

			int defenses = 0;

			std::map<const BuildingTypeClass*, int> bldComp;

			if (pOwner)

			{

				for (auto const pOwned : pOwner->Buildings)

				{

					if (pOwned && pOwned->Type && !pOwned->InLimbo && pOwned->IsAlive && !pOwned->Type->InvisibleInGame)

					{

						if (pOwned->Type->CanBeOccupied && !pOwned->Type->BaseNormal && !pOwned->Type->ConstructionYard && pOwned->Type->TechLevel < 0)
							continue;

						const bool isDeployedVehicle =
							pOwned->Type->ResourceGatherer || (pOwned->Type->UndeploysInto != nullptr && !pOwned->Type->ConstructionYard && !pOwned->Type->BaseNormal && !pOwned->Type->Refinery);
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

			std::sort(rankedBlds.begin(), rankedBlds.end(), [](auto const& a, auto const& b) { return a.first > b.first; });

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

					if (pOwnedUnit && pOwnedUnit->Owner == pOwner && pOwnedUnit->Type && !pOwnedUnit->InLimbo && pOwnedUnit->IsAlive)
						composition[pOwnedUnit->Type]++;
				}

				for (auto const pOwnedInf : InfantryClass::Array)

				{

					if (pOwnedInf && pOwnedInf->Owner == pOwner && pOwnedInf->Type && !pOwnedInf->InLimbo && pOwnedInf->IsAlive)
						composition[pOwnedInf->Type]++;
				}

				for (auto const pOwnedAir : AircraftClass::Array)

				{

					if (pOwnedAir && pOwnedAir->Owner == pOwner && pOwnedAir->Type && !pOwnedAir->InLimbo && pOwnedAir->IsAlive)
						composition[pOwnedAir->Type]++;
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
				"    {\"idx\": %d, \"side\": %d, \"country\": %d, \"archetype\": %d, \"expert\": %d, \"credits\": %d, \"killedUnits\": %d, \"killedBlds\": %d, \"ownedUnits\": %d, \"ownedBlds\": "
				"%d, \"score\": %d, \"winner\": %u, \"defeated\": %u, \"policy\": %u, \"conYards\": %d, \"harvesters\": %d, \"refineries\": %d, \"warFactories\": %d, \"barracks\": %d, "
				"\"defenses\": %d, \"blds\": \"%s\", \"units\": \"%s\"}%s\n",
				h.ArrayIndex, h.SideIndex, h.CountryIndex, h.Archetype, h.ActiveExpert, h.Credits, h.KilledUnits, h.KilledBuildings, h.OwnedUnits, h.OwnedBuildings, h.NetWorthScore, h.IsWinner,
				h.IsDefeated, h.PolicySlot, pOwner ? pOwner->NumConYards : 0, pOwner ? pOwner->CountResourceGatherers : 0, pOwner ? pOwner->CountResourceDestinations : 0,
				pOwner ? pOwner->NumWarFactories : 0, pOwner ? pOwner->NumBarracks : 0, defenses, bldText.c_str(), compositionText.c_str(), (i + 1 < dst->NumHouses) ? "," : "");

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

			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame || pBld->Owner->IsNeutral() || pBld->Owner->IsObserver())
				continue;

			if (pBld->Type->CanBeOccupied && !pBld->Type->BaseNormal && !pBld->Type->ConstructionYard && pBld->Type->TechLevel < 0)
				continue;

			if (ptCount >= 320 || jsonBuf.size() > 56000)
				break;

			const CellStruct c = pBld->GetMapCoords();

			if (c.X <= 1 && c.Y <= 1)
				continue;

			float rx = 0.0f;

			float ry = 0.0f;

			projectToRadar(c, rx, ry);

			const bool isDeployedVehicleBld =
				pBld->Type->ResourceGatherer || (pBld->Type->UndeploysInto != nullptr && !pBld->Type->ConstructionYard && !pBld->Type->BaseNormal && !pBld->Type->Refinery);
			// ptKind: 0 = unit/deployed vehicle, 1 = structure, 2 = defense structure (IsBaseDefense=yes)
			const int ptKind = isDeployedVehicleBld ? 0 : (pBld->Type->IsBaseDefense ? 2 : 1);

			char idClean[32];
			char nameClean[64];
			sanitizeJsonStr(pBld->Type->ID, idClean, sizeof(idClean));
			sanitizeJsonStr(pBld->Type->Name, nameClean, sizeof(nameClean));

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,%d,\"%s\",\"%s\"]",
				firstPt ? "" : ",", rx, ry, pBld->Owner->ArrayIndex, ptKind, idClean, nameClean);

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

			firstPt = false;

			ptCount++;
		}

		for (auto const pUnit : UnitClass::Array)

		{

			if (!pUnit || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Owner || pUnit->Owner->IsNeutral() || pUnit->Owner->IsObserver())
				continue;

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

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,0,\"%s\",\"%s\"]",
				firstPt ? "" : ",", rx, ry, pUnit->Owner->ArrayIndex, idClean, nameClean);

			if (lineLen > 0)
				jsonBuf.append(lineBuf, std::min<size_t>(static_cast<size_t>(lineLen), sizeof(lineBuf) - 1));

			firstPt = false;

			ptCount++;
		}

		int infStep = 0;

		for (auto const pInf : InfantryClass::Array)

		{

			if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || pInf->Owner->IsNeutral() || pInf->Owner->IsObserver())
				continue;

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

			lineLen = std::snprintf(lineBuf, sizeof(lineBuf), "%s[%.1f,%.1f,%d,0,\"%s\",\"%s\"]",
				firstPt ? "" : ",", rx, ry, pInf->Owner->ArrayIndex, idClean, nameClean);

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

	features[38] = ClampInt8(pExt->AIBrain_Archetype * 24);

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

	const BuildingTypeClass* pPrimaryConYardType = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)) ? pHouse->ConYards.GetItem(0)->Type : nullptr;
	CellStruct earlyHomeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		earlyHomeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();
	const int earlyHomeZone = (earlyHomeCoords != CellStruct::Empty) ? MapClass::Instance.GetMovementZoneType(earlyHomeCoords, MovementZone::Normal, false) : -1;
	int islandCapturableTechs = 0;

	for (auto const pBld : BuildingClass::Array)
	{
		if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant || pBld->Type->InvisibleInGame)
			continue;

		if (pBld->Type->BridgeRepairHut && MapClass::Instance.IsLinkedBridgeDestroyed(pBld->GetMapCoords()))
			destroyedBridgesCount++;

		if (pBld->Owner && pBld->Owner->IsNeutral() && pBld->Type->Capturable)
		{
			neutralCapturableTechs++;
			if (earlyHomeZone >= 0)
			{
				const int bz = MapClass::Instance.GetMovementZoneType(pBld->GetMapCoords(), MovementZone::Normal, false);
				if (bz >= 0 && bz != earlyHomeZone)
					islandCapturableTechs += ((pBld->Type->ProduceCashAmount > 0 || pBld->Type->ProduceCashStartup > 0) ? 2 : 1);
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

			else if (pBld->Occupants.GetItem(0) && pHouse->IsAlliedWith(pBld->Occupants.GetItem(0)->Owner) && pBld->Type->Repairable && pBld->GetHealthPercentage() < 0.75)

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

			if ((pBld->Type->ConstructionYard || pBld->Type->Factory != AbstractType::None) &&

				((pPrimaryConYardType && pBld->Type->ConstructionYard && pBld->Type != pPrimaryConYardType) || (ownCountryBit != 0 && !pBld->Type->InOwners(ownCountryBit))))

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
				ownReadySupers++;

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

	const int homeGroundZone = (homeCoords != CellStruct::Empty) ? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false) : -1;

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

					if ((pOwnU->Type->Harvester || pOwnU->Type->ResourceGatherer) && pOwnU->GetHealthPercentage() < 0.85)
						ownHarvestersUnderAttack++;

					else if (pOwnU->Type->Cost >= 700 && pOwnU->GetHealthPercentage() < 0.75)
						ownDamagedVehicles++;

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

					if (pOwnU->Type->DeploysInto && pOwnU->Type->DeploysInto->ConstructionYard && pPrimaryConYardType && pOwnU->Type->DeploysInto != pPrimaryConYardType)
						foreignConYardAndCaptureScore += 4;

					if (!pOwnU->Type->Naval && !pOwnU->Type->Harvester && !pOwnU->Type->ResourceGatherer && (pOwnU->Type->CanDisguise || pOwnU->Type->PermaDisguise || pOwnU->Type->Cloakable))
						camouflageAmbushAndGroveScore += 3;
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

		const bool cellVisibleToAI = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell) || spyInfiltrated || (pOwner->WhoLastHurtMe == pHouse->ArrayIndex);

		if (!cellVisibleToAI)

		{

			if (auto const pHiddenTType = pTechno->GetTechnoType())

			{

				const bool isHiddenThreat = (pTechno->CloakState != CloakState::Uncloaked || pTechno->Disguised || pHiddenTType->Cloakable || pHiddenTType->CanDisguise || pHiddenTType->PermaDisguise);

				if (isHiddenThreat && (pOwner->OwnedBuildings == 0 || (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame - NeuralAI::LastCombatFrame) >= 3600)))
					visibleEnemyStealthUnits += 2;
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

			if (pTType->Cloakable || pTechno->CloakState != CloakState::Uncloaked || pTechno->Disguised || pTType->CanDisguise || pTType->PermaDisguise)
				visibleEnemyStealthUnits++;

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

			if (pBldType && (pBldType->PowerBonus > 0 || pBldType->SuperWeapon != -1 || pBldType->Factory != AbstractType::None || pBldType->ConstructionYard))
				enemyExposedPowerAndFactories++;

			if (pBldType && pBldType->Capturable && (pBldType->ConstructionYard || pBldType->Factory != AbstractType::None || pBldType->TechLevel >= 6))
				foreignConYardAndCaptureScore += (pBldType->ConstructionYard ? 3 : 1);

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

	features[16] = GameModeOptionsClass::Instance.MCVRedeploy ? ClampInt8((pHouse->NumConYards * 25) + (features[20] > 0 ? 40 : 15)) : 0;

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
		baseCenter =

			pHouse->ConYards.GetItem(pHouse->ConYards.Count - 1)->GetMapCoords();

	int localOreCells = 0;

	int expansionOreCells = 0;

	int islandExpansionOreCells = 0;

	int visibleCratesNearby = 0;
	int nearbyTreeGroveCells = 0;
	int uncolonizedIslandLandCells = 0;
	if (baseCenter != CellStruct::Empty)
	{
		for (int dy = -96; dy <= 96; dy += (std::abs(dy) <= 48 ? 3 : 6))
		{
			for (int dx = -96; dx <= 96; dx += (std::abs(dx) <= 48 ? 3 : 6))
			{
				const CellStruct tc{static_cast<short>(baseCenter.X + dx), static_cast<short>(baseCenter.Y + dy)};
				auto const pCell = MapClass::Instance.TryGetCellAt(tc);
				if (!pCell)
					continue;
				const bool withinNearScan = (std::abs(dx) <= 48 && std::abs(dy) <= 48);
				if (withinNearScan && pCell->GetTerrain(false) != nullptr)
					nearbyTreeGroveCells++;
				if (withinNearScan && pCell->OverlayTypeIndex >= 0 && pCell->OverlayTypeIndex < OverlayTypeClass::Array.Count && IsCellVisibleToAI(pHouse, pCell))
				{
					if (auto const pOvl = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))
					{
						if (pOvl->Crate)
							visibleCratesNearby++;
					}
				}
				if (homeGroundZone >= 0 && pCell->LandType != LandType::Water && pCell->LandType != LandType::Rock && pCell->GetBuilding() == nullptr)
				{
					const int landZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
					if (landZone >= 0 && landZone != homeGroundZone)
						uncolonizedIslandLandCells++;
				}
				if (pCell->GetContainedTiberiumValue() <= 0)
					continue;
				const int oreZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
				int minSameZoneYardDist = 9999;
				for (auto const pOwnB : pHouse->Buildings)
				{
					if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->Insignificant || pOwnB->Type->InvisibleInGame)
						continue;
					if (!pOwnB->Type->ConstructionYard && !pOwnB->Type->Refinery && !pOwnB->Type->ResourceDestination)
						continue;
					const CellStruct yc = pOwnB->GetMapCoords();
					const int yz = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, false);
					if (oreZone >= 0 && yz != oreZone)
						continue;
					const int d = std::max(std::abs(tc.X - yc.X), std::abs(tc.Y - yc.Y));
					if (d < minSameZoneYardDist)
						minSameZoneYardDist = d;
				}
				if (minSameZoneYardDist <= 14 && (homeGroundZone < 0 || oreZone == homeGroundZone))
				{
					localOreCells++;
				}
				else if (minSameZoneYardDist > 14)
				{
					expansionOreCells++;
					if (homeGroundZone >= 0 && oreZone >= 0 && oreZone != homeGroundZone)
						islandExpansionOreCells++;
				}
			}
		}
	}
	const bool lowMoneyAndScarceLocalOre = (pHouse->Available_Money() < 1600 && localOreCells <= 8 && (expansionOreCells > 0 || islandExpansionOreCells > 0 || islandCapturableTechs > 0));
	const bool homeIslandCongestedOrDepleted = (uncolonizedIslandLandCells > 12 && (localOreCells <= 6 || pHouse->Buildings.Count >= 20 || lowMoneyAndScarceLocalOre));
	features[19] = ClampInt8(localOreCells * 4);
	features[20] = ClampInt8(expansionOreCells * 3 + (homeIslandCongestedOrDepleted ? 28 : 0) + (lowMoneyAndScarceLocalOre ? 24 : 0));
	features[21] = ClampInt8((islandEnemyBuildings > 0 ? 75 : 0) + ((islandExpansionOreCells > 1 || islandCapturableTechs > 0 || homeIslandCongestedOrDepleted) ? 50 : 0));

	features[27] = ClampInt8(visibleCratesNearby * 45);

	features[98] = ClampInt8(camouflageAmbushAndGroveScore * 16 + std::min(40, nearbyTreeGroveCells * 2) + visibleEnemyHarvesters * 12);

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

void NeuralAI::RunDeterministicMoE(HouseClass* pHouse, const std::array<int8_t, InputVectorSize>& features, std::array<int16_t, OutputVectorSize>& outScores)

{

	outScores.fill(0);

	auto const pExt = HouseExt::Fetch(pHouse);

	const AIBackpackFile& policy = NeuralAI::PolicyFor(pHouse);

	// Select active specialist expert (0..7) combining situational priors +

	// evolved Expert Routing Bias (Slots 40..47) + learned Faction Memory

	std::array<int, 8> expScores = {100, 100, 100, 80, 100, 160, 110, 110};

	if (pHouse->HasLowPower() || pHouse->CountResourceGatherers == 0 || pHouse->CountResourceDestinations == 0 || pHouse->NumConYards == 0)
		expScores[0] += 950; // Expert 0: Economy, Silos & Crisis Micro

	if (pHouse->NumBarracks == 0 || pHouse->NumWarFactories == 0 || (pHouse->Available_Money() > 2500 && pHouse->OwnedBuildings < 8))
		expScores[1] += 700; // Expert 1: Spatial Tetris Base Architect

	if (features[66] > 24 || features[65] > 36)
		expScores[2] += 420; // Expert 2: Dynamic Counter-Strategy

	if (HouseClass::Array.Count > 4 && (Unsorted::CurrentFrame % 240 < 60))
		expScores[3] += 380; // Expert 3: Multi-Player Opportunism & Diplomacy

	if (pExt->AIBrain_Archetype == 1 || (Unsorted::CurrentFrame % 300 < 60 && pHouse->OwnedUnits >= 4))
		expScores[4] += 360; // Expert 4: Special Operations & Trickster

	if (pHouse->OwnedUnits >= 3 || pHouse->OwnedInfantry >= 5)
		expScores[5] += 300; // Expert 5: Tactical Squad Micro, Kiting & Beacons

	const bool desperateForExpansionResources = (features[19] < 28 && (features[20] > 6 || features[21] > 0 || features[25] > 0));
	if (desperateForExpansionResources || ((pHouse->CountResourceDestinations < 3 || features[19] < 28) && features[20] > 10 && pHouse->Available_Money() >= 1200))
		expScores[6] += desperateForExpansionResources ? 720 : 480; // Expert 6: Base-Crawling & Ore Outpost Expansion

	if ((features[68] > 16 || pExt->AIBrain_Archetype == 2 || pExt->AIBrain_Archetype == 4) && pHouse->OwnedBuildings >= 7)
		expScores[7] += 440; // Expert 7: Forward Defense Crawling & Chokepoint Siege

	const int curArch = std::clamp(pExt->AIBrain_Archetype, 0, 4);

	int activeExpert = 0;

	int bestExpScore = -999999;

	for (int e = 0; e < 8; ++e)

	{

		int s = expScores[e];

		if (NeuralAI::AllowBackpackInference && NeuralAI::BackpackLoaded)

		{

			s += static_cast<int>(policy.OutputBias[40 + e]);

			const uint32_t eIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + curArch * 8 + e) % 512);

			if (policy.Entries[eIdx].VisitCount > 0)
				s += static_cast<int>(policy.Entries[eIdx].ExpertScoreDelta[e]) * 6;
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

		uint32_t offset = pHdr->ExpertWeightsOffset[activeExpert % 8];

		if (offset == 0)
			offset = pHdr->ExpertWeightsOffset[activeExpert % 6];

		const size_t requiredBytes = static_cast<size_t>(offset) + (NeuralAI::InputVectorSize * NeuralAI::OutputVectorSize);

		if (offset >= sizeof(AIBaseHeader) && requiredBytes <= NeuralAI::BaseMappedSize)

		{

			const int8_t* weights = reinterpret_cast<const int8_t*>(NeuralAI::BaseMappedView + offset);

			for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)

			{

				int32_t acc = 0;

				const int8_t* row = weights + (o * NeuralAI::InputVectorSize);

				for (int i = 0; i < NeuralAI::InputVectorSize; ++i)
					acc +=

						static_cast<int32_t>(features[i]) * static_cast<int32_t>(row[i]);

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
				acc += static_cast<int32_t>(features[i]) *

					static_cast<int32_t>(policy.LoRA_A[r * NeuralAI::InputVectorSize + i]);

			rankAct[r] = acc >> 8;
		}

		for (int o = 0; o < NeuralAI::OutputVectorSize; ++o)

		{

			int32_t delta = 0;

			for (int r = 0; r < 16; ++r)
				delta +=

					rankAct[r] * static_cast<int32_t>(policy.LoRA_B[r * NeuralAI::OutputVectorSize + o]);

			outScores[o] = ClampInt16(outScores[o] + (delta >> 7) + policy.OutputBias[o]);
		}

		// Recall learned strategy outcome deltas from Backpack Entries for this

		// Faction + Archetype + Expert

		const uint32_t entryIdx = static_cast<uint32_t>((pHouse->SideIndex * 40 + curArch * 8 + activeExpert) % 512);

		const auto& memEntry = policy.Entries[entryIdx];

		if (memEntry.VisitCount > 0)

		{

			const int16_t learnedBoost = ClampInt16(memEntry.ArchetypeScoreDelta[curArch] * 4 + memEntry.ExpertScoreDelta[activeExpert] * 4);

			for (int o = 16; o < 36; ++o)
				outScores[o] = ClampInt16(outScores[o] + learnedBoost);

			// If this Archetype has a negative win/loss record in the Backpack for

			// this faction, pivot to the highest-scoring alternative Archetype

			// (weighted by Slots 48..52)

			if (memEntry.ArchetypeScoreDelta[curArch] < -18 && (Unsorted::CurrentFrame % 900 < 30))

			{

				int bestAltArch = (curArch + 1) % 5;

				int bestAltScore = -999999;

				for (int a = 0; a < 5; ++a)

				{

					if (a == curArch)
						continue;

					const int altScore = static_cast<int>(outScores[48 + a]) + static_cast<int>(memEntry.ArchetypeScoreDelta[a]) * 6;

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

	if (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame % 2700 == 0) && pHouse->OwnedUnits < 4 && features[65] > 20)

	{

		int bestPivotArch = (pExt->AIBrain_Archetype + 1) % 5;

		int bestPivotScore = -999999;

		for (int a = 0; a < 5; ++a)

		{

			if (a == pExt->AIBrain_Archetype)
				continue;

			const int pScore = static_cast<int>(outScores[48 + a]);

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

	outScores[0] = ClampInt16(outScores[0] + (pHouse->HasLowPower() ? 2200 : ((pHouse->PowerOutput - pHouse->PowerDrain < 60) ? 1050 : -350)));

	outScores[1] = ClampInt16(outScores[1] + ((pHouse->CountResourceDestinations == 0)

													? 2100

													: ((pHouse->CountResourceDestinations < 2) ? 1150 : ((pHouse->CountResourceDestinations < 3 && pHouse->Available_Money() > 2200) ? 650 : -150))));

	outScores[2] = ClampInt16(outScores[2] + ((pHouse->NumBarracks == 0) ? 1750 : -200));

	outScores[3] = ClampInt16(outScores[3] + ((pHouse->NumWarFactories == 0) ? 1950 : ((pHouse->Available_Money() > 3200 && pHouse->NumWarFactories < 2) ? 850 : -100)));

	outScores[4] = ClampInt16(outScores[4] + ((pHouse->NumWarFactories > 0) ? 1150 : 100));

	outScores[5] = ClampInt16(outScores[5] + ((features[65] > 18 || arch == 2) ? 420 : 120));

	outScores[6] = ClampInt16(outScores[6] + ((features[66] > 12) ? 750 : 80));

	outScores[7] = ClampInt16(outScores[7] + (((features[67] > 10 || features[21] > 0) && pHouse->NumShipyards == 0) ? 1550 : -200));

	outScores[8] = ClampInt16(outScores[8] + ((pHouse->NumWarFactories > 0 && pHouse->NumAirpads == 0) ? ((arch == 4) ? 1100 : 550) : -150));

	const bool needsDepotForMCVExpansion = (pHouse->NumWarFactories > 0 && pHouse->CountResourceDestinations >= 1 && pHouse->UnitRepairStations.Count == 0 &&
		(features[19] < 28 || features[20] > 12 || features[21] > 0 || pHouse->Buildings.Count >= 20));
	outScores[9] = ClampInt16(outScores[9] + ((pHouse->NumWarFactories > 0 && pHouse->CountResourceDestinations >= 1 && pHouse->UnitRepairStations.Count == 0) ? (needsDepotForMCVExpansion ? 1650 : 1050) : -250));

	outScores[10] = ClampInt16(outScores[10] + ((GameModeOptionsClass::Instance.SWAllowed && pHouse->Available_Money() > 4500) ? 850 : -600));

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

	outScores[18] = ClampInt16(outScores[18] + ((arch == 1 || pHouse->ToCapture >= 0 || (features[18] > 0 && pHouse->OwnedUnits >= 5)) ? 650 : 250));

	outScores[19] = ClampInt16(outScores[19] + ((arch == 1 && pExt->AIBrain_InfiltratedEnemyMask == 0) ? 500 : 100));

	outScores[20] = ClampInt16(outScores[20] + ((features[66] > 12) ? 700 : 300));

	outScores[23] = ClampInt16(outScores[23] + ((arch == 2 || activeExpert == 7) ? 260 : 80));

	// Tactical & Spatial Strategy Output Slots (34..39) driven by Neural MoE +
	// LoRA + Evolved OutputBias + StarCraft/AdvAI Depleting-Spawn Urgency:
	const bool localOreDepleting = (features[19] < 28 || (Unsorted::CurrentFrame > 2700 && pHouse->CountResourceDestinations < 3));
	outScores[34] = ClampInt16(outScores[34] + ((activeExpert == 6 || localOreDepleting || features[20] > 12) ? 420 : 90));

	outScores[35] = ClampInt16(outScores[35] + ((activeExpert == 6 || localOreDepleting || features[20] > 10) ? 440 : 60));

	outScores[36] = ClampInt16(outScores[36] + ((arch == 1 || pHouse->ToCapture >= 0) ? 300 : 80));

	outScores[37] = ClampInt16(outScores[37] + ((arch == 3) ? 260 : ((arch == 2) ? -220 : 0)));

	outScores[38] = ClampInt16(outScores[38] + ((activeExpert == 7 || arch == 2) ? 280 : 40));

	outScores[39] = ClampInt16(outScores[39] + ((activeExpert == 0 || activeExpert == 6) ? 220 : ((arch == 3) ? -220 : 0)));

	// Feed evolved tactical/spatial slots into production & expansion priorities

	outScores[1] = ClampInt16(outScores[1] + std::clamp(static_cast<int>(outScores[34]) / 3 + static_cast<int>(outScores[39]) / 2, -300, 550));

	outScores[3] = ClampInt16(outScores[3] + std::clamp(static_cast<int>(outScores[34]) / 3, -200, 400));

	outScores[5] = ClampInt16(outScores[5] + std::clamp(static_cast<int>(outScores[38]) / 2, -250, 500));

	outScores[6] = ClampInt16(outScores[6] + std::clamp(static_cast<int>(outScores[38]) / 3, -200, 350));

	outScores[18] = ClampInt16(outScores[18] + std::clamp(static_cast<int>(outScores[36]), -300, 750));

	// Slot 21: Repair Destroyed Bridge via Engineer + BridgeRepairHut (only when

	// ready to push offensively)

	if (features[18] > 0)

	{

		if (pHouse->OwnedUnits * 6 >= features[65] && pHouse->OwnedUnits >= 4)
			outScores[21] = ClampInt16(

				outScores[21] + 1200); // We have ground superiority: repair bridge

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

	outScores[24] = ClampInt16(outScores[24] + 800 + (arch == 3 ? 300 : 0) - std::clamp(static_cast<int>(outScores[39]) / 2, -450, 350));

	outScores[25] = ClampInt16(outScores[25] + ((features[64] > 20) ? 700 : 300));

	outScores[26] = ClampInt16(outScores[26] + ((features[66] > 12) ? 950 : 250));

	outScores[27] = ClampInt16(outScores[27] + ((features[68] > 16 || arch == 4 || activeExpert == 7) ? 950 : 350));

	outScores[28] = ClampInt16(outScores[28] + ((features[21] > 0) ? 1450 : ((arch == 1 && pHouse->OwnedInfantry >= 3) ? 650 : 150)) + std::clamp(static_cast<int>(outScores[53]) / 2, -250, 450));

	outScores[29] = ClampInt16(outScores[29] + 600 + std::clamp(static_cast<int>(outScores[15]) / 2, -200, 450));

	// Slot 30: Multi-Outpost Expansion MCV, Multi-ConYard Production Speed Boost & Nomadic MCV
	// (No hard cap on NumConYards: the neural network weighs whether to build more MCVs for expansion,
	// nomadic hopping, or MultipleFactory building speed vs being moderate based on economy & biases)
	const bool homeCongestedOrIslandTarget = (features[21] > 0 || pHouse->Buildings.Count >= 20 || GetMacroState(pHouse).ConsecutivePlacementFailures > 0);
	const bool urgentResourceOrIslandExpansion = (localOreDepleting && (features[20] > 6 || homeCongestedOrIslandTarget)) || (features[21] > 0 && pHouse->NumConYards == 1 && pHouse->CountResourceDestinations < 3);
	const int depleteDiscount = (features[19] < 20 && (features[20] > 6 || homeCongestedOrIslandTarget)) ? 850 : ((features[19] < 28 || homeCongestedOrIslandTarget) ? 500 : 0);
	const int mcvReserveThreshold = std::clamp(2000 + (pHouse->NumConYards * 750) - depleteDiscount - (static_cast<int>(outScores[35]) * 2) - (static_cast<int>(outScores[81]) * 2), 1100, 5500);
	if (pHouse->NumConYards >= 1 && pHouse->NumWarFactories > 0 && (urgentResourceOrIslandExpansion || pHouse->Available_Money() > mcvReserveThreshold) &&
		(features[20] > 6 || localOreDepleting || homeCongestedOrIslandTarget || outScores[35] > 80 || outScores[81] > 80 || outScores[30] > 160) &&
		(features[19] < 32 || homeCongestedOrIslandTarget || pHouse->Available_Money() > mcvReserveThreshold + 1000 || outScores[30] > 220 || outScores[81] > 120))
	{
		outScores[30] = ClampInt16(
			outScores[30] + 1050 + (localOreDepleting ? 420 : 0) + (homeCongestedOrIslandTarget ? 400 : 0) + (urgentResourceOrIslandExpansion ? 350 : 0) +
			std::clamp(static_cast<int>(outScores[35]) + (static_cast<int>(outScores[81]) / 2), -400, 800) - ((pHouse->NumConYards - 1) * 180));
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

	outScores[32] = ClampInt16(outScores[32] + ((arch == 4) ? 850 : 400));

	outScores[33] = ClampInt16(outScores[33] + ((arch == 4) ? 900 : 450));

	// New Real Tactical & Economic Hands (Slots 56..60):

	// Slot 56: Eco-Raid / Enemy Harvester Hunting & Own Harvester Defense

	outScores[56] = ClampInt16(outScores[56] + (features[78] * 8) + (features[31] * 10) + ((arch == 1 || arch == 3 || activeExpert == 4 || activeExpert == 5) ? 280 : 60));

	// Slot 57: Siege, Urban Garrison Repair/Assault & Combat Deploy/Undeploy

	outScores[57] = ClampInt16(outScores[57] + (features[73] * 10) + (features[79] * 9) + (features[68] * 6) + (features[76] * 6) + ((arch == 4 || activeExpert == 7) ? 320 : 80));

	outScores[27] = ClampInt16(outScores[27] + std::clamp(static_cast<int>(outScores[57]) / 3, -200, 450));

	if (features[79] > 0)

	{

		outScores[18] = ClampInt16(outScores[18] + 320); // Train Engineer to repair damaged urban strongholds!

		outScores[23] = ClampInt16(outScores[23] + 280); // Train Occupier infantry to hold repaired urban buildings!
	}

	// Slot 58: Tactical Kiting, Mind-Control Breaker, Tank Crush & Anti-SW

	// Scatter

	outScores[58] = ClampInt16(outScores[58] + (features[75] * 8) + (features[76] * 7) + (features[77] * 9) + (features[22] * 8) + ((activeExpert == 5) ? 340 : 100));

	// Slot 59: Smart Wrench Building Repair & Map Crate Pickup

	outScores[59] = ClampInt16(outScores[59] + (features[27] * 12) + (features[29] * 9) + ((pHouse->Available_Money() > 1400) ? 240 : -150));

	// Slot 60: AA & Sensor Escort, LowPower Defense Toggle & Spawner Standoff

	outScores[60] = ClampInt16(outScores[60] + (features[80] * 10) + (features[66] * 8) + (features[72] * 9) + (pHouse->HasLowPower() ? 360 : 80));

	outScores[26] = ClampInt16(outScores[26] + std::clamp(static_cast<int>(outScores[60]) / 3, -150, 380));

	// Slot 61: Dynamic Spy/Mirage Disguise (DisguiseAs) & Counter-Spy Detection

	// (DetectDisguise)

	outScores[61] = ClampInt16(outScores[61] + (features[81] * 10) + (features[72] * 8) + ((arch == 1 || activeExpert == 4) ? 320 : 90));

	// Slot 62: Slave Miner Liberation (Kill SlaveOwner -> Free Slaves) & Tesla

	// Coil Overpowering

	outScores[62] = ClampInt16(outScores[62] + (features[82] * 11) + (features[78] * 6) + (pHouse->HasLowPower() ? 280 : 90));

	// Slot 63: Desolator Rad-Field Chokepoint Deploy & Mobile Gap/Stealth Shroud

	// Escort

	outScores[63] = ClampInt16(outScores[63] + (features[83] * 10) + (features[64] * 7) + (features[80] * 6) + ((activeExpert == 5 || activeExpert == 7) ? 260 : 80));

	// Slot 64: Parasite Warfare (Terror Drone/Squid), De-Parasiting

	// (Sonic/RemoveParasite/Repair) & Smart OpenTopped

	outScores[64] = ClampInt16(outScores[64] + (features[84] * 10) + (features[22] * 9) + ((activeExpert == 4 || activeExpert == 5) ? 280 : 90));

	// Slot 65: Grinder Mind-Control Recycling & Emergency Salvage

	outScores[65] = ClampInt16(outScores[65] + (features[85] * 11) + (features[77] * 6) + ((activeExpert == 0 || activeExpert == 4) ? 260 : 80));

	// Slot 66: EMP Pulse Targeting (Warhead->EMEffect) & EMP-Disabled Finisher

	// (IsUnderEMP)

	outScores[66] = ClampInt16(outScores[66] + (features[86] * 11) + (features[65] * 6) + ((activeExpert == 2 || activeExpert == 5) ? 280 : 90));

	// Slot 67: Tank Bunker Garrisoning (Bunker/Bunkerable) & Manual Ammo Reload

	// (ManualReload)

	outScores[67] = ClampInt16(outScores[67] + (features[87] * 10) + (features[68] * 6) + ((activeExpert == 1 || activeExpert == 7) ? 260 : 80));

	// Slot 68: Sniper/Poison Specialist Hunter, Chaos Gas (Psychedelic) &

	// TiberiumHeal Regeneration

	outScores[68] = ClampInt16(outScores[68] + (features[88] * 10) + (features[64] * 6) + ((activeExpert == 2 || activeExpert == 4) ? 280 : 90));

	// Slot 69: Submarine Cloak Ambush & Anti-Sub Destroyer Escort (Naval

	// Cloakable & Sensors)

	outScores[69] = ClampInt16(outScores[69] + (features[89] * 10) + (features[67] * 8) + ((arch == 4 || activeExpert == 2) ? 260 : 80));

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

	outScores[75] = ClampInt16(outScores[75] + (features[93] * 11) + (features[21] * 10) + ((activeExpert == 4 || activeExpert == 6) ? 320 : 110));

	if (features[21] > 0 && pHouse->NumConYards >= 1 && (pHouse->Available_Money() > 1400 || localOreDepleting || pHouse->Buildings.Count >= 20))
	{
		outScores[30] = ClampInt16(std::max(static_cast<int>(outScores[30]), 250) + 680);
		outScores[28] = ClampInt16(outScores[28] + 650);
	}

	// Slot 76: Squad Focus-Fire on Wounded In-Range Enemy (Lanchester

	// Target-Fire)

	outScores[76] = ClampInt16(outScores[76] + (features[94] * 10) + (features[65] * 6) + ((activeExpert == 5) ? 300 : 120));

	// Slot 77: Wounded Frontline Tank Pull-Back Micro (Step Behind Healthy Tanks)

	outScores[77] = ClampInt16(outScores[77] + (features[94] * 9) + (features[30] * 8) + ((activeExpert == 5) ? 280 : 110));

	// Slot 78: High-Ground Cliff Advantage Positioning (CellClass::Level)

	outScores[78] = ClampInt16(outScores[78] + (features[80] * 8) + (features[68] * 6) + ((activeExpert == 7) ? 280 : 100));

	// Slot 79: Surgical Enemy Power, Superweapon & Factory Snipe inside Enemy

	// Base

	outScores[79] = ClampInt16(outScores[79] + (features[95] * 11) + (features[75] * 8) + ((activeExpert == 4 || activeExpert == 5) ? 300 : 120));

	// Slot 80: Fast Flanker & Jumpjet Counter-Battery vs Enemy Artillery

	outScores[80] = ClampInt16(outScores[80] + (features[76] * 12) + (features[95] * 7) + ((arch == 3 || activeExpert == 2 || activeExpert == 5) ? 300 : 110));

	// Slot 81: Nomadic MCV Hop, Outpost Drop & Forward Siege Turret-Push

	// (MCVRedeploy)

	outScores[81] = ClampInt16(outScores[81] + (features[16] * 10) + (features[20] * 7) + (features[68] * 5) + ((activeExpert == 6 || activeExpert == 7) ? 320 : 100));

	// Slot 82: Enemy ConYard Heist, Stolen MCV Safe Extraction & Cross-Faction

	// Tech Tree

	outScores[82] = ClampInt16(outScores[82] + (features[17] * 11) + (features[95] * 6) + ((arch == 1 || activeExpert == 1 || activeExpert == 4) ? 320 : 120));

	if (features[17] >= 40)

	{

		outScores[18] = ClampInt16(outScores[18] + 450); // Train Engineers to capture Enemy ConYard/Tech!

		outScores[4] = ClampInt16(outScores[4] + 400); // Climb newly captured Foreign Tech Tree!
	}

	// Slot 83: Mirage Tank & Camouflage Tree-Grove Ambush, Retreat When Spotted &

	// Ambush Zone Rotation

	outScores[83] = ClampInt16(outScores[83] + (features[98] * 9) + (features[78] * 6) + ((arch == 1 || activeExpert == 4 || activeExpert == 5) ? 300 : 110));

	// Slots 84..127 (44 slots) are FREE RESERVE SLOTS kept at 0 until assigned
}

namespace

{

	bool IsAmphibiousOrAirTransport(const TechnoTypeClass* pType)

	{

		if (!pType || pType->Passengers < 4)
			return false;

		return pType->SpeedType == SpeedType::Hover || pType->SpeedType == SpeedType::Amphibious || pType->SpeedType == SpeedType::FloatBeach || pType->SpeedType == SpeedType::Winged ||

			pType->MovementZone == MovementZone::Amphibious || pType->MovementZone == MovementZone::AmphibiousDestroyer || pType->MovementZone == MovementZone::AmphibiousCrusher ||
			pType->MovementZone == MovementZone::Fly || pType->MovementZone == MovementZone::Subterrannean || pType->JumpJet || pType->ConsideredAircraft;
	}

	// Finds a safe coastal/flank cell on a target island (matching

	// targetIslandZone) away from enemy turrets & units

	// Validates Carryall pickup against rulesmd.ini tags (Carryall.Allowed, Carryall.SizeLimit, SizeLimit, Organic, Naval, NonVehicle) and engine Action::Capture
	bool CanCarryallLiftUnit(AircraftClass* pCarryall, UnitClass* pUnit)
	{
		if (!pCarryall || !pUnit || pCarryall->InLimbo || !pCarryall->IsAlive || pUnit->InLimbo || !pUnit->IsAlive)
			return false;
		auto const pCarryType = pCarryall->Type;
		auto const pUnitType = pUnit->Type;
		if (!pCarryType || !pUnitType || !pCarryType->Carryall || pCarryType->Passengers <= 0)
			return false;
		if (pCarryall->Passengers.NumPassengers >= pCarryType->Passengers || pUnit->IsOnCarryall || pUnit->IsIronCurtained())
			return false;
		if (pUnitType->Organic || pUnitType->Naval || pUnitType->NonVehicle)
			return false;
		if (pCarryType->SizeLimit > 0 && static_cast<int>(pUnitType->TechnoTypeClass::Size) > static_cast<int>(pCarryType->SizeLimit))
			return false;
		if (auto const pCarryTypeExt = TechnoTypeExt::Fetch(pCarryType))
		{
			if (pCarryTypeExt->Carryall_SizeLimit.isset() && static_cast<int>(pUnitType->TechnoTypeClass::Size) > pCarryTypeExt->Carryall_SizeLimit.Get())
				return false;
		}
		if (auto const pUnitTypeExt = TechnoTypeExt::Fetch(pUnitType))
		{
			if (!pUnitTypeExt->Carryall_Allowed.Get())
				return false;
		}
		return !pUnit->Transporter;
	}

	CellClass* FindSafeIslandLandingCell(HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone, bool preferOre)

	{

		CellClass* pBestLanding = nullptr;

		int bestScore = -999999;

		for (int dy = -32; dy <= 32; dy += 2)

		{

			for (int dx = -32; dx <= 32; dx += 2)

			{

				const CellStruct cand{static_cast<short>(islandRefCell.X + dx), static_cast<short>(islandRefCell.Y + dy)};

				auto const pCell = MapClass::Instance.TryGetCellAt(cand);

				if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock || pCell->GetBuilding())
					continue;

				if (targetIslandZone >= 0)

				{

					const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);

					if (cz != targetIslandZone)
						continue;
				}

				if (!pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
					continue;

				int score = 200 - (std::abs(dx) + std::abs(dy)) * 3;

				// Bonus for coastal/beach cells so amphibious hovercraft can beach and

				// unload without driving inland through defenses

				bool nearCoast = (pCell->LandType == LandType::Beach);

				if (!nearCoast)

				{

					const CellStruct neighbors[4] = {

						{static_cast<short>(cand.X + 2), cand.Y}, {static_cast<short>(cand.X - 2), cand.Y}, {cand.X, static_cast<short>(cand.Y + 2)}, {cand.X, static_cast<short>(cand.Y - 2)}};

					for (const auto& nc : neighbors)

					{

						if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))

						{

							if (pNC->LandType == LandType::Water || pNC->LandType == LandType::Beach)

							{

								nearCoast = true;

								break;
							}
						}
					}
				}

				if (nearCoast)
					score += 350;

				// Bonus for nearby Ore/Tiberium when landing an Expansion MCV on an

				// island

				if (preferOre && pCell->GetContainedTiberiumValue() > 0)
					score += 350;

				// Avoid visible enemy defensive structures (Prism Towers, Tesla Coils,

				// Obelisks, SAMs, Bunkers) and enemy armies

				for (auto const pBld : BuildingClass::Array)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Insignificant || pBld->Type->InvisibleInGame)
						continue;

					if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral())
						continue;

					const CellStruct bc = pBld->GetMapCoords();

					const int dist = std::max(std::abs(cand.X - bc.X), std::abs(cand.Y - bc.Y));

					if (dist <= 11)

					{

						if (pBld->Type->IsBaseDefense)
							score -= (12 - dist) *

									140; // Heavy penalty for enemy defensive turrets in range!

						else if (dist <= 5)
							score -= 120; // Don't try to unload directly on top of congested

						// enemy buildings
					}
				}

				for (auto const pEnemyU : UnitClass::Array)

				{

					if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner)
						continue;

					if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
						continue;

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

		return pBestLanding ? pBestLanding : MapClass::Instance.TryGetCellAt(islandRefCell);
	}

} // namespace

void NeuralAI::ExecuteTacticalAndSpecOpsMicro(HouseClass* pHouse, const std::array<int16_t, OutputVectorSize>& scores)

{
	auto const FindDeployableFoundationNearCell = [](HouseClass* pH, BuildingTypeClass* pBldType, const CellStruct& originCell, int requiredZone) -> CellStruct
	{
		if (!pH || !pBldType || originCell == CellStruct::Empty)
			return CellStruct::Empty;
		const int w = pBldType->GetFoundationWidth();
		const int h = pBldType->GetFoundationHeight(false);
		for (int r = 0; r <= 18; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;
					const CellStruct cand{static_cast<short>(originCell.X + dx), static_cast<short>(originCell.Y + dy)};
					if (!MapClass::Instance.CoordinatesLegal(cand))
						continue;
					if (requiredZone >= 0)
					{
						const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
						if (cz != requiredZone)
							continue;
					}
					if (!NeuralAI::IsCellClearedOfExitCorridors(pH, pBldType, cand))
						continue;
					bool canExist = true;
					for (int fy = 0; fy < h && canExist; ++fy)
					{
						for (int fx = 0; fx < w; ++fx)
						{
							const CellStruct fc{static_cast<short>(cand.X + fx), static_cast<short>(cand.Y + fy)};
							auto const pFC = MapClass::Instance.TryGetCellAt(fc);
							if (!pFC || !pFC->CanThisExistHere(pBldType->SpeedType, pBldType, pH))
							{
								canExist = false;
								break;
							}
						}
					}
					if (canExist)
						return cand;
				}
			}
		}
		return CellStruct::Empty;
	};

	auto const pExt = HouseExt::Fetch(pHouse);

	CellStruct homeCoords = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;

	if (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
		homeCoords = pHouse->ConYards.GetItem(0)->GetMapCoords();

	const int homeGroundZone = (homeCoords != CellStruct::Empty) ? MapClass::Instance.GetMovementZoneType(homeCoords, MovementZone::Normal, false) : -1;

	// Universal Cross-Island Reachability & Transport Dispatcher:
	// Any ground-only unit or infantry whose action target is on a disconnected landmass (footZone != targetZone)
	// is automatically routed via an Amphibious, Hover, Air, or Carryall Transport to the target island's landing beach,
	// and once unloaded on the target island (footZone == targetZone), executes its native mission.
	static struct PassengerIslandGoalEntry
	{
		DWORD UniqueID;
		CellStruct TargetCoords;
		int Frame;
	} s_PassengerIslandGoals[64]{};

	auto RecordPassengerIslandGoal = [](DWORD uid, const CellStruct& targetCoords)
	{
		int oldestIdx = 0;
		int oldestFrame = INT_MAX;
		for (int i = 0; i < 64; ++i)
		{
			if (s_PassengerIslandGoals[i].UniqueID == uid || s_PassengerIslandGoals[i].UniqueID == 0)
			{
				s_PassengerIslandGoals[i] = {uid, targetCoords, Unsorted::CurrentFrame};
				return;
			}
			if (s_PassengerIslandGoals[i].Frame < oldestFrame)
			{
				oldestFrame = s_PassengerIslandGoals[i].Frame;
				oldestIdx = i;
			}
		}
		s_PassengerIslandGoals[oldestIdx] = {uid, targetCoords, Unsorted::CurrentFrame};
	};

	auto LookupPassengerIslandGoal = [](DWORD uid) -> CellStruct
	{
		for (int i = 0; i < 64; ++i)
		{
			if (s_PassengerIslandGoals[i].UniqueID == uid && (Unsorted::CurrentFrame - s_PassengerIslandGoals[i].Frame) <= 9000)
				return s_PassengerIslandGoals[i].TargetCoords;
		}
		return CellStruct::Empty;
	};

	auto CanFootCrossWaterDirectly = [](const FootClass* pF) -> bool
	{
		if (!pF)
			return false;
		auto const pT = pF->GetTechnoType();
		if (!pT)
			return false;
		return (pT->MovementZone == MovementZone::Amphibious || pT->MovementZone == MovementZone::AmphibiousDestroyer ||
			pT->MovementZone == MovementZone::AmphibiousCrusher || pT->MovementZone == MovementZone::Fly ||
			pT->MovementZone == MovementZone::Subterrannean || pT->MovementZone == MovementZone::Water ||
			pT->SpeedType == SpeedType::Hover || pT->SpeedType == SpeedType::Winged ||
			pT->JumpJet || pT->Teleporter || pT->Naval);
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
		if (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard ||
			pFoot->CurrentMission == Mission::Harvest || pFoot->CurrentMission == Mission::Unload)
			return false;
		AbstractClass* pGoal = pFoot->Destination ? pFoot->Destination : pFoot->Target;
		if (!pGoal)
			return false;
		if (auto const pDestFoot = abstract_cast<FootClass*>(pFoot->Destination))
		{
			if (pDestFoot->Owner == pHouse && pDestFoot->GetTechnoType() && pDestFoot->GetTechnoType()->Passengers > 0)
			{
				const int fz = MapClass::Instance.GetMovementZoneType(pFoot->GetMapCoords(), MovementZone::Normal, false);
				const int tz = MapClass::Instance.GetMovementZoneType(pDestFoot->GetMapCoords(), MovementZone::Normal, false);
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
				const auto pW = pFoot->GetTechnoType()->GetWeapon(0) ? pFoot->GetTechnoType()->GetWeapon(0)->WeaponType : nullptr;
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

	auto DispatchFootToTargetOrTransport = [&](FootClass* pFoot, AbstractClass* pTarget, Mission desiredMission, bool useSetTarget = false) -> bool
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

		if (footZone < 0 || targetZone < 0 || footZone == targetZone || CanFootCrossWaterDirectly(pFoot) || canShootFromHere)
		{
			if (useSetTarget || desiredMission == Mission::Attack)
			{
				pFoot->SetTarget(pTarget);
				pFoot->QueueMission(desiredMission, true);
			}
			else
			{
				pFoot->SetDestination(pTarget, true);
				pFoot->QueueMission(desiredMission, true);
			}
			return true;
		}

		RecordPassengerIslandGoal(pFoot->UniqueID, targetCoords);
		const int passSize = std::max(1, static_cast<int>(pFootType->Size));
		if (CellClass* pTargetLanding = FindSafeIslandLandingCell(pHouse, targetCoords, targetZone, false))
			pendingCrossIslandOrders.push_back({pFoot, footZone, pTargetLanding, targetZone, passSize});

		FootClass* pSameIslandTrans = nullptr;
		int bestTransDist = 999999;
		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || !IsAmphibiousOrAirTransport(pU->Type))
				continue;
			if (passSize > static_cast<int>(pU->Type->SizeLimit) || (pU->Passengers.GetTotalSize() + passSize) > pU->Type->Passengers)
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
				if (!pAirT->Type->Carryall && pAirT->Type->Passengers >= 1 && !pAirT->IsInAir() && pAirT->GetHeight() == 0 &&
					passSize <= static_cast<int>(pAirT->Type->SizeLimit) && (pAirT->Passengers.GetTotalSize() + passSize) <= pAirT->Type->Passengers)
				{
					const int tz = MapClass::Instance.GetMovementZoneType(pAirT->GetMapCoords(), MovementZone::Normal, false);
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
			pFoot->SetTarget(nullptr);
			pFoot->SetDestination(pSameIslandTrans, true);
			pFoot->QueueMission(Mission::Enter, true);
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
		if (!pAirT->Type->Carryall && pAirT->Type->Passengers >= 3 && !pAirT->IsInAir() && pAirT->GetHeight() == 0 &&
			pAirT->Passengers.NumPassengers < pAirT->Type->Passengers)
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

	// Discover uncolonized island Resource (Ore/Gems), buildable Island Land (when home island is full), and Strategic Neutral Tech Building targets
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
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type || pOwnB->Type->Insignificant || pOwnB->Type->InvisibleInGame)
				continue;
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
				const int uz = MapClass::Instance.GetMovementZoneType(pOwnU->GetMapCoords(), MovementZone::Normal, false);
				if (uz == homeGroundZone)
				{
					refineriesOnHomeIsland++;
					if (refineriesOnHomeIsland >= 3)
						pExtraHomeSlaveMiner = pOwnU;
				}
			}
		}
		const bool homeIslandCongested = (pHouse->Buildings.Count >= 20 || GetMacroState(pHouse).ConsecutivePlacementFailures > 0);
		int bestResScore = -999999;
		CellClass* pFallbackIslandLandCell = nullptr;
		int fallbackIslandLandZone = -1;
		int bestLandScore = -999999;
		for (int dy = -112; dy <= 112; dy += 6)
		{
			for (int dx = -112; dx <= 112; dx += 6)
			{
				const CellStruct cand{static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy)};
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
							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant || pBld->Type->InvisibleInGame)
								continue;
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
				bool alreadyColonized = false;
				for (auto const pBld : pHouse->Buildings)
				{
					if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant || pBld->Type->InvisibleInGame)
						continue;
					if (pBld->Type->Refinery || pBld->Type->ConstructionYard || pBld->Type->ResourceDestination)
					{
						const int bz = MapClass::Instance.GetMovementZoneType(pBld->GetMapCoords(), MovementZone::Normal, false);
						if (bz == cz)
						{
							alreadyColonized = true;
							break;
						}
					}
				}
				if (alreadyColonized)
					continue;
				const int dist = std::abs(dx) + std::abs(dy);
				if (tibVal > 0)
				{
					const int sc = std::min(tibVal, 600) - dist * 2;
					if (sc > bestResScore)
					{
						bestResScore = sc;
						resourceIslandZone = cz;
						pIslandResourceGoalCell = pCell;
					}
				}
				else if (homeIslandCongested && pCell->GetBuilding() == nullptr && pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
				{
					const int lsc = 300 - dist * 2;
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
			if (auto const pSafe = FindSafeIslandLandingCell(pHouse, pIslandResourceGoalCell->MapCoords, resourceIslandZone, true))
				pIslandResourceGoalCell = pSafe;
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
				if (pOwnInf && pOwnInf->Owner == pHouse && !pOwnInf->InLimbo && pOwnInf->IsAlive && pOwnInf->Type && pOwnInf->Type->Engineer)
				{
					if (MapClass::Instance.GetMovementZoneType(pOwnInf->GetMapCoords(), MovementZone::Normal, false) == bz)
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

		if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver() || pOther->Defeated)
			continue;

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

	BuildingClass* pRepairDepot = pHouse->UnitRepairStations.Count > 0 ? pHouse->UnitRepairStations.GetItem(0) : nullptr;

	BuildingClass* pGrinder = pHouse->Grinders.Count > 0 ? pHouse->Grinders.GetItem(0) : nullptr;

	CellClass* pBeaconCell = (pExt->AIBrain_AttackTargetCell != CellStruct::Empty) ? MapClass::Instance.TryGetCellAt(pExt->AIBrain_AttackTargetCell) : nullptr;

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

	UnitClass* pVisibleEnemyHarvester = nullptr;

	UnitClass* pEnemySlaveMiner = nullptr;

	UnitClass* pLeadHeavyTank = nullptr;

	UnitClass* pLeadWarship = nullptr;

	UnitClass* pParasitedOwnUnit = nullptr;

	UnitClass* pBestParasiteVictim = nullptr;

	UnitClass* pEnemyChaosTarget = nullptr;

	InfantryClass* pSampleEnemyInfantry = nullptr;

	InfantryClass* pEnemySpecialistInf = nullptr;

	TechnoClass* pDetectedEnemyInfiltrator = nullptr;

	TechnoClass* pHiddenOrStealthEnemy = nullptr;

	UnitClass* pEnemySubOrShip = nullptr;

	const bool combatStalledForHunt = (Unsorted::CurrentFrame > 5400 && (Unsorted::CurrentFrame - NeuralAI::LastCombatFrame) >= 3600);

	TechnoClass* pEnemyMindController = nullptr;

	TechnoClass* pEnemyEMPedTarget = nullptr;

	TechnoClass* pEnemyNonEMPedHeavy = nullptr;

	TechnoClass* pEnemyTemporalAttacker = nullptr;

	TechnoClass* pEnemyDrainer = nullptr;

	TechnoClass* pEnemyArtilleryUnit = nullptr;

	UnitClass* pWoundedEnemyVehicle = nullptr;

	BuildingClass* pSurgicalEnemyBld = nullptr;

	BuildingClass* pPrioritySiegeBld = nullptr;

	BuildingClass* pEnemyGarrisonToClear = nullptr;

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

		if (!pOtherH || pOtherH == pHouse || pHouse->IsAlliedWith(pOtherH) || pOtherH->IsNeutral() || pOtherH->IsObserver() || pOtherH->Defeated)
			continue;

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

		if (pCheckU->MindControlledBy && pCheckU->MindControlledBy->Owner && pCheckU->MindControlledBy->Owner != pHouse && !pHouse->IsAlliedWith(pCheckU->MindControlledBy->Owner) &&

			pCheckU->MindControlledBy->GetCell() && IsCellVisibleToAI(pHouse, pCheckU->MindControlledBy->GetCell()))

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

			if ((pCheckU->Type->Harvester || pCheckU->Type->ResourceGatherer) && (pCheckU->HasBeenAttacked || pCheckU->GetHealthPercentage() < 0.90))

			{

				pAttackedOwnHarvester = pCheckU;
			}

			else if (!pLeadHeavyTank && !pCheckU->Type->Naval && !pCheckU->Type->Harvester && !pCheckU->Type->ResourceGatherer)

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

				if (!pVisibleEnemyHarvester && (pCheckU->Type->Harvester || pCheckU->Type->ResourceGatherer))
					pVisibleEnemyHarvester = pCheckU;

				if (!pEnemySlaveMiner && pCheckU->Type->Enslaves != nullptr)
					pEnemySlaveMiner = pCheckU;

				if (!pBestParasiteVictim && pCheckU->Type->Parasiteable && !pCheckU->Type->Organic && pCheckU->ParasiteEatingMe == nullptr && pCheckU->Type->Cost >= 700)
					pBestParasiteVictim = pCheckU;

				if (pCheckU->IsUnderEMP() && !pEnemyEMPedTarget)

				{

					pEnemyEMPedTarget = pCheckU;
				}

				else if (!pCheckU->IsUnderEMP() && pCheckU->Type->Cost >= 800 && !pCheckU->Type->Organic && !pEnemyNonEMPedHeavy)

				{

					pEnemyNonEMPedHeavy = pCheckU;
				}

				if (!pEnemyChaosTarget && !pCheckU->Berzerk && !pCheckU->Type->ImmuneToPsionics && pCheckU->Type->Cost >= 600)
					pEnemyChaosTarget = pCheckU;

				if (!pWoundedEnemyVehicle && pCheckU->GetHealthPercentage() < 0.55 && pCheckU->Type->Cost >= 500)
					pWoundedEnemyVehicle = pCheckU;

				const auto pEWS = pCheckU->Type->GetWeapon(0);

				const auto pEW = pEWS ? pEWS->WeaponType : nullptr;

				if (!pEnemyArtilleryUnit && pEW && (pEW->Range >= (8 * 256) || pEW->Spawner) && !pCheckU->Type->Naval)
					pEnemyArtilleryUnit = pCheckU;

				if (!pDetectedEnemyInfiltrator && pCheckU->Disguised && homeCoords != CellStruct::Empty && pCheckU->GetMapCoords().DistanceFrom(homeCoords) <= 22.0)
					pDetectedEnemyInfiltrator = pCheckU;

				if (pCheckU->Type->Naval && (!pEnemySubOrShip || pCheckU->Type->Cloakable || pCheckU->CloakState != CloakState::Uncloaked))
					pEnemySubOrShip = pCheckU;

				if (!pCheckU->Type->Naval && !pHiddenOrStealthEnemy &&

					(pCheckU->CloakState != CloakState::Uncloaked || pCheckU->Disguised || pCheckU->Type->Cloakable || pCheckU->Type->CanDisguise || pCheckU->Type->PermaDisguise))

				{

					pHiddenOrStealthEnemy = pCheckU;
				}
			}

			else if (combatStalledForHunt || pCheckU->Owner->OwnedBuildings == 0)

			{

				if (pCheckU->Type->Naval && (!pEnemySubOrShip || pCheckU->Type->Cloakable || pCheckU->CloakState != CloakState::Uncloaked))

				{

					pEnemySubOrShip = pCheckU;
				}

				else if (!pCheckU->Type->Naval && !pHiddenOrStealthEnemy &&

						(pCheckU->CloakState != CloakState::Uncloaked || pCheckU->Disguised || pCheckU->Type->Cloakable || pCheckU->Type->CanDisguise || pCheckU->Type->PermaDisguise ||

							pCheckU->Owner->OwnedBuildings == 0))

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

			if ((combatStalledForHunt || pCheckInf->Owner->OwnedBuildings == 0) && !pHiddenOrStealthEnemy &&

				(pCheckInf->CloakState != CloakState::Uncloaked || pCheckInf->Disguised || pCheckInf->Type->Cloakable || pCheckInf->Type->CanDisguise || pCheckInf->Type->PermaDisguise ||

					pCheckInf->Owner->OwnedBuildings == 0))

			{

				pHiddenOrStealthEnemy = pCheckInf;
			}

			continue;
		}

		if (!pSampleEnemyInfantry && !pCheckInf->Type->Doggie && !pCheckInf->Type->Slaved)
			pSampleEnemyInfantry = pCheckInf;

		if (!pEnemySpecialistInf && (pCheckInf->Type->Engineer || pCheckInf->Type->Infiltrate || pCheckInf->Type->C4 || pCheckInf->Type->Ivan || pCheckInf->Type->Deployer ||

										pCheckInf->Type->VehicleThief || pCheckInf->Type->Thief))

		{

			pEnemySpecialistInf = pCheckInf;
		}

		if (!pDetectedEnemyInfiltrator && homeCoords != CellStruct::Empty &&

			(pCheckInf->Disguised || pCheckInf->Type->Infiltrate || pCheckInf->Type->Engineer || pCheckInf->Type->Thief || pCheckInf->Type->VehicleThief || pCheckInf->Type->C4) &&

			pCheckInf->GetMapCoords().DistanceFrom(homeCoords) <= 20.0)

		{

			pDetectedEnemyInfiltrator = pCheckInf;
		}
	}

	int bestSiegeScore = -1;

	for (auto const pEBld : BuildingClass::Array)

	{

		if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Type || pEBld->Type->Immune || pEBld->Type->Insignificant || pEBld->Type->InvisibleInGame)
			continue;

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

		if (pEBld->Type->CanBeOccupied && pEBld->Type->Repairable && pEBld->GetHealthPercentage() < 0.72)

		{

			const bool isFriendlyOrEmpty = (pEBld->Occupants.Count == 0) || (pEBld->Occupants.GetItem(0) && pHouse->IsAlliedWith(pEBld->Occupants.GetItem(0)->Owner));

			if (isFriendlyOrEmpty && !pDamagedUrbanToRepair)
				pDamagedUrbanToRepair = pEBld;
		}

		if (!pEBld->Owner || pEBld->Owner == pHouse || pHouse->IsAlliedWith(pEBld->Owner) || pEBld->Owner->IsNeutral())
			continue;

		if (!pEBld->GetCell() || !IsCellVisibleToAI(pHouse, pEBld->GetCell()))
			continue;

		if (pEBld->Type->CanBeOccupied && pEBld->Occupants.Count > 0 && !pEnemyGarrisonToClear)
			pEnemyGarrisonToClear = pEBld;

		if (pEBld->Type->Capturable && pEBld->Type->ConstructionYard && !pCapturableEnemyConYard)
			pCapturableEnemyConYard = pEBld;

		if (scores[79] > 80 && (pEBld->Type->SuperWeapon != -1 || pEBld->Type->PowerBonus > 0 || pEBld->Type->Factory != AbstractType::None || pEBld->Type->ConstructionYard))

		{

			if (!pSurgicalEnemyBld || pEBld->Type->SuperWeapon != -1 || (pEBld->Type->PowerBonus > 0 && pSurgicalEnemyBld->Type->SuperWeapon == -1))
				pSurgicalEnemyBld = pEBld;
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

			if (pEng && pEng->Owner == pHouse && !pEng->InLimbo && pEng->IsAlive && pEng->Type && pEng->Type->Engineer && pEng->DistanceFrom(pCapturableEnemyConYard) <= 2560)

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

			if (pHouse->NumConYards >= 2 && pOwnB != pHouse->ConYards.GetItem(0) && pOwnB->GetMapCoords().DistanceFrom(homeCoords) > 16.0)

			{

				pStolenConYardOrMCV = pOwnB;

				break;
			}
		}

		for (auto const pOwnU : UnitClass::Array)

		{

			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type || !pOwnU->Type->DeploysInto || !pOwnU->Type->DeploysInto->ConstructionYard)
				continue;

			const CellStruct mc = pOwnU->GetMapCoords();

			const bool isForeignMCV = (pHouse->CountOwnedNow(pOwnU->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1);

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

				const CellStruct tc{static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy)};

				auto const pC = MapClass::Instance.TryGetCellAt(tc);

				if (!pC || pC->OverlayTypeIndex < 0 || pC->OverlayTypeIndex >= OverlayTypeClass::Array.Count || !IsCellVisibleToAI(pHouse, pC))
					continue;

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
				if (pLinkedBld->Type && (pLinkedBld->Type->WeaponsFactory || pLinkedBld->Type->Factory == AbstractType::UnitType))
				{
					if (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr)
						continue; // Never interrupt a vehicle's short exit move while still linked to its War Factory / Shipyard bib!
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

		if (enemySuperImminent && scores[58] > 140 && !pType->Harvester && !pType->ResourceGatherer && pUnit->CurrentMission != Mission::Move && (Unsorted::CurrentFrame % 90 < 15))

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

		// Slot 70 (Dying Transport / Battle Fortress Emergency Passenger Unload):

		// If a transport or OpenTopped vehicle carrying passengers on land is about

		// to explode (< 24% HP), immediately Unload so the squad inside survives!

		if (pUnit->Passengers.NumPassengers > 0 && !pType->Naval && pUnit->GetHealthPercentage() < 0.24 && scores[70] > 60)

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

			TechnoClass* pKamikazeTarget = pSurgicalEnemyBld ? static_cast<TechnoClass*>(pSurgicalEnemyBld) : static_cast<TechnoClass*>(pWoundedEnemyVehicle);

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

			if (pUnit->CurrentMission != Mission::Enter)

			{

				pUnit->SetDestination(pRepairDepot, true);

				pUnit->QueueMission(Mission::Enter, true);
			}

			continue;
		}

		const int unitZoneForEco = MapClass::Instance.GetMovementZoneType(pUnit->GetMapCoords(), MovementZone::Normal, false);
		const bool isIslandColonizerSlaveMiner = (pType->ResourceGatherer && pType->DeploysInto != nullptr &&
			((pUnit == pExtraHomeSlaveMiner && (pIslandResourceGoalCell != nullptr || !hasUncolonizedOreOnHomeIsland)) ||
			(homeGroundZone >= 0 && unitZoneForEco >= 0 && unitZoneForEco != homeGroundZone)));
		if ((pType->Harvester || pType->ResourceGatherer) && !isIslandColonizerSlaveMiner)
		{
			// IMPORTANT: Never call SetDestination() on Harvesters or Chrono Miners
			// (Teleporter=yes)! In Yuri's Revenge, calling SetDestination() on a
			// ChronoLocomotionClass unit triggers an immediate Chrono-shift and
			// resets UnitClass::Mission_Harvest's internal state machine, causing
			// Allied Chrono Miners to loop-teleport endlessly to the Refinery
			// entrance cell. Vanilla Mission_Harvest natively handles ore/gem search
			// and Chrono return.
			if (pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::None)
				pUnit->QueueMission(Mission::Harvest, false);

			continue;
		}

		// Yuri Grinder Economy: Recycle heavily damaged Mind-Controlled enemy

		// vehicles into the Grinder for instant cash!

		if (pGrinder && pUnit->MindControlledBy != nullptr && pUnit->GetHealthPercentage() < 0.45 && pUnit->CurrentMission != Mission::Enter)

		{

			pUnit->SetDestination(pGrinder, true);

			pUnit->QueueMission(Mission::Enter, true);

			continue;
		}

		// Auto-deploy MCV if we have no ConYards (ensuring a safe, corridor-clear

		// 4x4 foundation when rebuilt from a War Factory or retreating after

		// emergency undeploy), or march any additional Expansion MCV to an Ore

		// field

		if (pType->DeploysInto && pType->DeploysInto->ConstructionYard)

		{

			const CellStruct mcvCell = pUnit->GetMapCoords();

			const bool isForeignTechMCV = (pHouse->CountOwnedNow(pType->DeploysInto) == 0 && pHouse->NumConYards >= 1);

			bool enemyNearMCV = false;

			for (auto const pEB : BuildingClass::Array)

			{

				if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune || pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
					continue;

				if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
					continue;

				const CellStruct ec = pEB->GetMapCoords();

				if (std::max(std::abs(mcvCell.X - ec.X), std::abs(mcvCell.Y - ec.Y)) <= 14)

				{

					enemyNearMCV = true;

					break;
				}
			}

			if (!enemyNearMCV)

			{

				for (auto const pEU : UnitClass::Array)

				{

					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Harvester || pEU->Type->ResourceGatherer)
						continue;

					if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral())
						continue;

					const CellStruct euc = pEU->GetMapCoords();

					if (std::max(std::abs(mcvCell.X - euc.X), std::abs(mcvCell.Y - euc.Y)) <= 12)

					{

						enemyNearMCV = true;

						break;
					}
				}
			}

			const int mcvZone = MapClass::Instance.GetMovementZoneType(mcvCell, MovementZone::Normal, false);
			int minAnchorDist = 9999;
			int conYardsInMcvZone = 0;
			CellStruct newestYard = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : mcvCell;
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
					continue;

				if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery && !pOwnBld->Type->BaseNormal && pOwnBld->Type->Factory == AbstractType::None)
					continue;

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

				const int d = std::max(std::abs(mcvCell.X - bc.X), std::abs(mcvCell.Y - bc.Y));
				if (d < minAnchorDist)
					minAnchorDist = d;
			}

			// If our single ConYard just packed up into a Nomadic MCV (scores[81] > 140) after planting structures
			// at its previous hub (minAnchorDist <= 10), let it travel to its next destination instead of re-deploying on the same spot!
			const bool isNomadicSingleMCVHop = (pHouse->NumConYards == 0 && scores[81] > 140 && pHouse->OwnedBuildings >= 6 && pHouse->NumWarFactories >= 1 && minAnchorDist <= 10 && !enemyNearMCV);

			if (pHouse->NumConYards == 0 && !isNomadicSingleMCVHop)
			{
				auto const pYardType = pType->DeploysInto;
				// At initial match spawn (no buildings yet and no enemy nearby), unpack
				// immediately on the spot!
				if (pHouse->Buildings.Count == 0 && !enemyNearMCV)

				{

					if (pUnit->CurrentMission != Mission::Unload)
						pUnit->QueueMission(Mission::Unload, true);

					continue;
				}

				// Check if current MCV cell is safe (!enemyNearMCV), has a legal

				// ConYard foundation, and does not block our War Factory / Refinery

				// exit doors (critical when a newly rebuilt MCV just rolled out of the

				// War Factory!).

				bool canDeployHereSafely = !enemyNearMCV && NeuralAI::IsCellClearedOfExitCorridors(pHouse, pYardType, mcvCell);

				if (canDeployHereSafely)

				{

					const int yw = pYardType->GetFoundationWidth();

					const int yh = pYardType->GetFoundationHeight(false);

					for (int fy = 0; fy < yh && canDeployHereSafely; ++fy)

					{

						for (int fx = 0; fx < yw; ++fx)

						{

							const CellStruct fc{static_cast<short>(mcvCell.X + fx), static_cast<short>(mcvCell.Y + fy)};

							auto const pFC = MapClass::Instance.TryGetCellAt(fc);

							if (!pFC || !pFC->CanThisExistHere(pYardType->SpeedType, pYardType, pHouse))

							{

								canDeployHereSafely = false;

								break;
							}
						}
					}
				}

				if (canDeployHereSafely)

				{

					if (pUnit->CurrentMission != Mission::Unload)
						pUnit->QueueMission(Mission::Unload, true);
				}

				else if (pUnit->CurrentMission != Mission::Move || (Unsorted::CurrentFrame % 45 < 15))

				{

					const CellStruct safeYardCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pYardType);

					if (safeYardCell != CellStruct::Empty)

					{

						if (auto const pSafeDest = MapClass::Instance.TryGetCellAt(safeYardCell))

						{

							pUnit->SetDestination(pSafeDest, true);

							pUnit->QueueMission(Mission::Move, true);
						}
					}

					else if (!enemyNearMCV && pUnit->CurrentMission != Mission::Unload)

					{

						pUnit->QueueMission(Mission::Unload, true);
					}
				}

				continue;
			}

			// Slot 82 (Stolen / Foreign MCV Safe Extraction, Island Transport

			// Evacuation & Home Base Deployment): 1) If the stolen MCV is on a

			// disconnected island (mcvZone != homeGroundZone), board a nearby

			// Amphibious Transport (or move to the safest pickup beach while a

			// transport sails over to rescue it) to ferry it back to our safe Home

			// Island! 2) If on the same landmass and inside a hostile enemy zone

			// (enemyNearMCV) OR outside our home base (> 16 cells from homeCoords),

			// evacuate it by land back to our safe home base! 3) Once the stolen

			// foreign-faction MCV reaches our safe home base (<= 16 cells from

			// homeCoords and !enemyNearMCV), deploy it safely inside our base

			// perimeter so we can build its foreign tech tree!

			if (scores[82] > 60 && homeCoords != CellStruct::Empty && (enemyNearMCV || isForeignTechMCV))
			{
				const bool onDisconnectedEnemyIsland = (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone);

				if (onDisconnectedEnemyIsland && isForeignTechMCV)

				{

					UnitClass* pRescueTrans = nullptr;

					int bestRescueDist = 999999;

					for (auto const pT : UnitClass::Array)

					{

						if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type || !IsAmphibiousOrAirTransport(pT->Type))
							continue;

						if (pType->TechnoTypeClass::Size > pT->Type->SizeLimit || (pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size)) > pT->Type->Passengers)
							continue;

						const int d = pUnit->DistanceFrom(pT);

						if (d < bestRescueDist)

						{

							bestRescueDist = d;

							pRescueTrans = pT;
						}
					}

					if (pRescueTrans && bestRescueDist <= 3584)

					{

						if (pUnit->CurrentMission != Mission::Enter)

						{

							pUnit->SetDestination(pRescueTrans, true);

							pUnit->QueueMission(Mission::Enter, true);
						}

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

						const CellStruct safeHomeCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pType->DeploysInto);

						const CellStruct goalCoords = (safeHomeCell != CellStruct::Empty) ? safeHomeCell : homeCoords;

						if (auto const pHomeDest = MapClass::Instance.TryGetCellAt(goalCoords))

						{

							pUnit->SetDestination(pHomeDest, true);

							pUnit->QueueMission(Mission::Move, true);
						}
					}

					continue;
				}

				if (isForeignTechMCV && !enemyNearMCV && distToHome <= 16.0)

				{

					if (NeuralAI::IsCellClearedOfExitCorridors(pHouse, pType->DeploysInto, mcvCell))

					{

						if (pUnit->CurrentMission != Mission::Unload)
							pUnit->QueueMission(Mission::Unload, true);
					}

					else if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)

					{

						const CellStruct clearCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pType->DeploysInto);

						if (auto const pClear = MapClass::Instance.TryGetCellAt(clearCell))

						{

							pUnit->SetDestination(pClear, true);

							pUnit->QueueMission(Mission::Move, true);
						}
					}

					continue;
				}
			}

			const bool isMovingToDest = (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr && pUnit->DistanceFrom(pUnit->Destination) > 768);
			const bool mustColonizeOtherIsland = (homeGroundZone >= 0 && mcvZone == homeGroundZone && conYardsOnHomeIsland >= 1 && !isForeignTechMCV &&
				(pIslandResourceGoalCell != nullptr || !hasUncolonizedOreOnHomeIsland || pHouse->Buildings.Count >= 20));
			const bool landedOnNewIsland = (homeGroundZone >= 0 && mcvZone >= 0 && mcvZone != homeGroundZone && conYardsInMcvZone == 0);
			const bool wantsMultiConYardSpeed = (pHouse->NumConYards >= 1 && !mustColonizeOtherIsland && scores[30] > 400 && scores[35] <= 60 && scores[81] <= 90);
			const int requiredAnchorDist = landedOnNewIsland ? 0 : (wantsMultiConYardSpeed ? 5 : 13);

			if (mustColonizeOtherIsland)
			{
				const bool isSelfAmphibOrAir = (pType->MovementZone == MovementZone::Amphibious || pType->MovementZone == MovementZone::AmphibiousDestroyer ||
					pType->MovementZone == MovementZone::AmphibiousCrusher || pType->MovementZone == MovementZone::Fly || pType->JumpJet);
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
					UnitClass* pBeachedMcvTrans = nullptr;
					int bestTransDist = 999999;
					for (auto const pT : UnitClass::Array)
					{
						if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type || !IsAmphibiousOrAirTransport(pT->Type))
							continue;
						if (static_cast<int>(pType->TechnoTypeClass::Size) > static_cast<int>(pT->Type->SizeLimit) ||
							(pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size)) > pT->Type->Passengers)
							continue;
						auto const pTCell = pT->GetCell();
						const int tz = MapClass::Instance.GetMovementZoneType(pT->GetMapCoords(), MovementZone::Normal, false);
						if (tz == homeGroundZone && pTCell && pTCell->LandType != LandType::Water)
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
						if (pUnit->CurrentMission != Mission::Enter || (Unsorted::CurrentFrame % 60 < 20))
						{
							pUnit->SetDestination(pBeachedMcvTrans, true);
							pUnit->QueueMission(Mission::Enter, true);
						}
					}
					else if (!isMovingToDest || (Unsorted::CurrentFrame % 90 < 25))
					{
						if (!isMovingToDest && !NeuralAI::IsCellClearedOfExitCorridors(pHouse, pType->DeploysInto, mcvCell))
						{
							pUnit->Scatter(CoordStruct::Empty, true, false);
						}
						else if (auto const pHomePickupBeach = FindSafeIslandLandingCell(pHouse, mcvCell, homeGroundZone, false))
						{
							if (mcvCell.DistanceFrom(pHomePickupBeach->MapCoords) > 2.5)
							{
								pUnit->SetDestination(pHomePickupBeach, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
					}
				}
			}
			else if ((landedOnNewIsland || (!isMovingToDest && minAnchorDist >= requiredAnchorDist)) && !enemyNearMCV)
			{
				const CellStruct deployCell = FindDeployableFoundationNearCell(pHouse, pType->DeploysInto, mcvCell, mcvZone);
				if (deployCell == mcvCell)
				{
					if (pUnit->CurrentMission != Mission::Unload)
						pUnit->QueueMission(Mission::Unload, true);
				}
				else if (deployCell != CellStruct::Empty)
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
				else if (!NeuralAI::IsCellClearedOfExitCorridors(pHouse, pType->DeploysInto, mcvCell))
				{
					pUnit->Scatter(CoordStruct::Empty, true, false);
				}
			}
			else if (!isMovingToDest && (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard || pUnit->CurrentMission == Mission::Move))
			{
				CellClass* pExpansionDest = nullptr;
				BuildingClass* pHarassEnemyBld = pSurgicalEnemyBld ? pSurgicalEnemyBld : pPrioritySiegeBld;
				if (scores[81] > 140 && pHarassEnemyBld && pHouse->CountResourceDestinations >= 2)
				{
					const CellStruct ec = pHarassEnemyBld->GetMapCoords();
					const int dxE = newestYard.X - ec.X;
					const int dyE = newestYard.Y - ec.Y;
					const int distE = std::max(1, std::max(std::abs(dxE), std::abs(dyE)));
					if (distE >= 22)
					{
						const CellStruct siegeStage{static_cast<short>(ec.X + (dxE * 15) / distE), static_cast<short>(ec.Y + (dyE * 15) / distE)};
						if (auto const pSC = MapClass::Instance.TryGetCellAt(siegeStage))
						{
							if (pSC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
								pExpansionDest = pSC;
						}
					}
				}

				int bestExpDist = 99999;
				for (int dy = -56; dy <= 56 && !pExpansionDest; dy += 3)
				{
					for (int dx = -56; dx <= 56; dx += 3)
					{
						const CellStruct tc{static_cast<short>(mcvCell.X + dx), static_cast<short>(mcvCell.Y + dy)};
						auto const pCell = MapClass::Instance.TryGetCellAt(tc);
						if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
							continue;
						const int candZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
						if (mcvZone >= 0 && candZone != mcvZone)
							continue;

						bool tooCloseToExistingHub = false;
						for (auto const pOwnBld : pHouse->Buildings)
						{
							if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
								continue;
							if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery && !pOwnBld->Type->BaseNormal)
								continue;
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
				else if (!enemyNearMCV)
				{
					const CellStruct localYardCell = FindDeployableFoundationNearCell(pHouse, pType->DeploysInto, mcvCell, mcvZone);
					if (localYardCell != CellStruct::Empty)
					{
						if (auto const pLocalDest = MapClass::Instance.TryGetCellAt(localYardCell))
						{
							pUnit->SetDestination(pLocalDest, true);
							pUnit->QueueMission(Mission::Move, true);
						}
					}
					else if (!NeuralAI::IsCellClearedOfExitCorridors(pHouse, pType->DeploysInto, mcvCell))
					{
						pUnit->Scatter(CoordStruct::Empty, true, false);
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

			const bool hasPrimaryWeapon = (pType->GetWeapon(0) != nullptr && pType->GetWeapon(0)->WeaponType != nullptr);

			auto const pTargetBld = pType->DeploysInto;

			const bool isKeyStructureDeployer = (pTargetBld != nullptr && (!pType->Harvester && !pType->ResourceGatherer || isIslandColonizerSlaveMiner) && (!pTargetBld->IsBaseDefense || !hasPrimaryWeapon));

			if (isKeyStructureDeployer)
			{
				const CellStruct nanoCell = pUnit->GetMapCoords();
				const int nanoZone = MapClass::Instance.GetMovementZoneType(nanoCell, MovementZone::Normal, false);
				const bool isNanoIdle = (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard || pUnit->CurrentMission == Mission::Harvest);
				const bool isNanoMoving = (pUnit->CurrentMission == Mission::Move && pUnit->Destination != nullptr && pUnit->DistanceFrom(pUnit->Destination) > 768);

				int ownedRefineries = 0;
				int ownedWarFactories = 0;
				int ownedBarracks = 0;
				int minDistFromBaseBld = 9999;
				int minDistFromRefOrYard = 9999;
				int hubsInNanoZone = 0;

				for (auto const pOwnBld : pHouse->Buildings)
				{
					if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
						continue;

					const CellStruct bc = pOwnBld->GetMapCoords();
					const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
					const int d = std::max(std::abs(nanoCell.X - bc.X), std::abs(nanoCell.Y - bc.Y));

					if (nanoZone < 0 || bz == nanoZone)
					{
						if (d < minDistFromBaseBld)
							minDistFromBaseBld = d;

						if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard || pOwnBld->Type->BaseNormal || pOwnBld->Type->ResourceDestination)
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

				const bool isDedicatedMobileOutpost =
					(pTargetBld->BaseNormal && !pTargetBld->Refinery && pTargetBld->Factory == AbstractType::None && pTargetBld->PowerBonus <= pTargetBld->PowerDrain && pHouse->Buildings.Count >= 1);

				const bool isExpansionDeployer = isIslandColonizerSlaveMiner || isDedicatedMobileOutpost ||
					((pTargetBld->Refinery || pTargetBld->ResourceDestination) && ownedRefineries >= 1 && scores[30] > 450) ||
					(pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval && ownedWarFactories >= 1 && scores[30] > 600) ||
					(pTargetBld->Factory == AbstractType::InfantryType && ownedBarracks >= 1 && scores[30] > 650);

				if (isExpansionDeployer)
				{
					const bool mustColonizeIslandWithDeployer = (homeGroundZone >= 0 && nanoZone == homeGroundZone &&
						(isIslandColonizerSlaveMiner || pIslandResourceGoalCell != nullptr || !hasUncolonizedOreOnHomeIsland || pHouse->Buildings.Count >= 20));
					const bool landedOnNewIsland = (homeGroundZone >= 0 && nanoZone >= 0 && nanoZone != homeGroundZone && hubsInNanoZone == 0);

					if (mustColonizeIslandWithDeployer)
					{
						const bool isSelfAmphibOrAir = (pType->MovementZone == MovementZone::Amphibious || pType->MovementZone == MovementZone::AmphibiousDestroyer ||
							pType->MovementZone == MovementZone::AmphibiousCrusher || pType->MovementZone == MovementZone::Fly || pType->JumpJet);
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
							UnitClass* pBeachedTrans = nullptr;
							int bestTransDist = 999999;
							for (auto const pT : UnitClass::Array)
							{
								if (!pT || pT->Owner != pHouse || pT->InLimbo || !pT->IsAlive || !pT->Type || !IsAmphibiousOrAirTransport(pT->Type))
									continue;
								if (static_cast<int>(pType->TechnoTypeClass::Size) > static_cast<int>(pT->Type->SizeLimit) ||
									(pT->Passengers.GetTotalSize() + static_cast<int>(pType->TechnoTypeClass::Size)) > pT->Type->Passengers)
									continue;
								auto const pTCell = pT->GetCell();
								const int tz = MapClass::Instance.GetMovementZoneType(pT->GetMapCoords(), MovementZone::Normal, false);
								if (tz == homeGroundZone && pTCell && pTCell->LandType != LandType::Water)
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
								if (pUnit->CurrentMission != Mission::Enter || (Unsorted::CurrentFrame % 60 < 20))
								{
									pUnit->SetDestination(pBeachedTrans, true);
									pUnit->QueueMission(Mission::Enter, true);
								}
							}
							else if (!isNanoMoving || (Unsorted::CurrentFrame % 90 < 25))
							{
								if (auto const pHomePickupBeach = FindSafeIslandLandingCell(pHouse, nanoCell, homeGroundZone, false))
								{
									if (nanoCell.DistanceFrom(pHomePickupBeach->MapCoords) > 2.5)
									{
										pUnit->SetDestination(pHomePickupBeach, true);
										pUnit->QueueMission(Mission::Move, true);
									}
								}
							}
						}
					}
					else if (landedOnNewIsland || (!isNanoMoving && minDistFromRefOrYard >= 12))
					{
						const CellStruct deployCell = FindDeployableFoundationNearCell(pHouse, pTargetBld, nanoCell, nanoZone);
						if (deployCell == nanoCell)
						{
							if (pUnit->CurrentMission != Mission::Unload)
								pUnit->QueueMission(Mission::Unload, true);
						}
						else if (deployCell != CellStruct::Empty)
						{
							if (!isNanoMoving || (Unsorted::CurrentFrame % 45 < 15))
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
					else if (isNanoIdle && homeCoords != CellStruct::Empty)
					{
						CellClass* pExpOreCell = nullptr;
						int bestOreDist = 99999;
						for (int dy = -56; dy <= 56; dy += 3)
						{
							for (int dx = -56; dx <= 56; dx += 3)
							{
								const CellStruct tc{static_cast<short>(nanoCell.X + dx), static_cast<short>(nanoCell.Y + dy)};
								auto const pC = MapClass::Instance.TryGetCellAt(tc);
								if (!pC || pC->GetContainedTiberiumValue() <= 0)
									continue;
								const int candZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
								if (nanoZone >= 0 && candZone != nanoZone)
									continue;

								bool nearExistingRef = false;
								for (auto const pOwnBld : pHouse->Buildings)
								{
									if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
										continue;
									if (pOwnBld->Type->Refinery || pOwnBld->Type->ConstructionYard || pOwnBld->Type->BaseNormal || pOwnBld->Type->ResourceDestination)
									{
										const CellStruct bc = pOwnBld->GetMapCoords();
										if (MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false) != candZone)
											continue;
										if (std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 14)
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

						if (pExpOreCell)
						{
							pUnit->SetDestination(pExpOreCell, true);
							pUnit->QueueMission(Mission::Move, true);
						}
						else
						{
							const CellStruct deployCell = FindDeployableFoundationNearCell(pHouse, pTargetBld, nanoCell, nanoZone);
							if (deployCell == nanoCell)
								pUnit->QueueMission(Mission::Unload, true);
							else if (auto const pClear = MapClass::Instance.TryGetCellAt(deployCell))
							{
								pUnit->SetDestination(pClear, true);
								pUnit->QueueMission(Mission::Move, true);
							}
						}
					}
					continue;
				}

				// Defense Turret Deployer (Mobile Turret Cores / ROBOTEGG_OBELISK2,

				// ROBOTEGG_CERBERUS2, etc.): Move 6..10 cells away from

				// factories/ConYard to the defensive perimeter before unpacking!

				if (pTargetBld->IsBaseDefense)

				{

					if (minDistFromBaseBld >= 6 && NeuralAI::IsCellClearedOfExitCorridors(pHouse, pTargetBld, nanoCell))

					{

						if (pUnit->CurrentMission != Mission::Unload)
							pUnit->QueueMission(Mission::Unload, true);

						continue;
					}

					if (isNanoIdle && homeCoords != CellStruct::Empty)

					{

						const CellStruct perimCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pTargetBld);

						if (auto const pPerim = MapClass::Instance.TryGetCellAt(perimCell))

						{

							pUnit->SetDestination(pPerim, true);

							pUnit->QueueMission(Mission::Move, true);

							continue;
						}
					}
				}

				// Base Infrastructure Deployer (Power, Barracks, Radar, Tech, Repair

				// Bay, Shield/Gap Generator, or first Refinery/Factory): Unpack as soon

				// as clear of factory exit corridors; if blocking a factory door, step

				// aside first!

				if (isNanoIdle)

				{

					if (NeuralAI::IsCellClearedOfExitCorridors(pHouse, pTargetBld, nanoCell))

					{

						pUnit->QueueMission(Mission::Unload, true);
					}

					else

					{

						const CellStruct clearCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pTargetBld);

						if (auto const pClear = MapClass::Instance.TryGetCellAt(clearCell))

						{

							pUnit->SetDestination(pClear, true);

							pUnit->QueueMission(Mission::Move, true);
						}
					}

					continue;
				}
			}

			else if (pUnit->Target != nullptr && pUnit->CurrentMission == Mission::Attack)

			{

				pUnit->QueueMission(Mission::Unload, true);

				continue;
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

		if (pRepairDepot && ((pUnit->ParasiteEatingMe != nullptr && scores[64] > 60) || ((pType->Cost >= 800 || pUnit->Veterancy.IsElite()) && pUnit->GetHealthPercentage() < repairThreshold)))

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

		if (pGrinderBld && !pType->Naval && scores[65] > 80 && (pUnit->MindControlledBy != nullptr || (!pRepairDepot && pUnit->GetHealthPercentage() < 0.18 && pHouse->Available_Money() < 600)))

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

		if (pType->TiberiumHeal && pUnit->GetHealthPercentage() < 0.55 && scores[68] > 60 && pUnit->CurrentMission != Mission::Move)

		{

			if (pUnit->MoveToTiberium(10, false))
				continue;
		}

		UnstickCrossIslandFoot(pUnit);
		const bool isUnitIdle = (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard);

		const auto pPrimaryWpn = pType->GetWeapon(0) ? pType->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Engineer ConYard Heist Cease-Fire): If our tank is firing at an

		// enemy ConYard that is already in red health (< 35% HP) while our own

		// Engineer is within 10 cells moving in to capture it, hold fire on the

		// ConYard so our Engineer can steal it alive!

		if (ownEngineerNearEnemyConYard && pCapturableEnemyConYard && pUnit->Target == pCapturableEnemyConYard && pCapturableEnemyConYard->GetHealthPercentage() < 0.35 && scores[82] > 60)

		{

			pUnit->SetTarget(nullptr);

			pUnit->QueueMission(Mission::Guard, true);

			continue;
		}

		// Slot 71 (Chrono Legionnaire Temporal Breaker): Immediately kill any enemy

		// Chrono Legionnaire erasing our building or tank so it is restored at

		// 100%!

		if (pEnemyTemporalAttacker && pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[71] > 60 && pUnit->DistanceFrom(pEnemyTemporalAttacker) <= 4096)

		{

			DispatchFootToTargetOrTransport(pUnit, pEnemyTemporalAttacker, Mission::Attack, true);

			continue;
		}

		// Slot 74 (Anti-Drain Floating Disc Interceptor): Mobile AA units

		// immediately shoot down any enemy Floating Disc draining our buildings!

		if (pEnemyDrainer && pPrimaryWpn && pPrimaryWpn->Projectile && pPrimaryWpn->Projectile->AA && scores[74] > 60 && pUnit->DistanceFrom(pEnemyDrainer) <= 4608)

		{

			pUnit->SetTarget(pEnemyDrainer);

			pUnit->QueueMission(Mission::Attack, true);

			continue;
		}

		// Slot 77 (Wounded Frontline Tank Pull-Back Micro): When our tank is

		// focused below 38% HP in active combat and healthy allied tanks are right

		// beside it, pull back 4 cells behind our healthy frontline so the enemy

		// switches target!

		if (pUnit->Target != nullptr && !pType->Naval && pUnit->GetHealthPercentage() < 0.38 && scores[77] > 80 && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 45 < 15))

		{

			int healthyAlliesBeside = 0;

			for (auto const pAllyT : UnitClass::Array)

			{

				if (pAllyT && pAllyT != pUnit && pAllyT->Owner == pHouse && !pAllyT->InLimbo && pAllyT->IsAlive && pAllyT->GetHealthPercentage() > 0.70 && pUnit->DistanceFrom(pAllyT) <= 1280)
					healthyAlliesBeside++;
			}

			if (healthyAlliesBeside >= 1)

			{

				const CellStruct uc = pUnit->GetMapCoords();

				const int sx = (homeCoords.X > uc.X) ? 3 : ((homeCoords.X < uc.X) ? -3 : 0);

				const int sy = (homeCoords.Y > uc.Y) ? 3 : ((homeCoords.Y < uc.Y) ? -3 : 0);

				const CellStruct backCell{static_cast<short>(uc.X + sx), static_cast<short>(uc.Y + sy)};

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

		if (pWoundedEnemyVehicle && pUnit->CurrentMission == Mission::Attack && pUnit->Target != pWoundedEnemyVehicle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[76] > 80 &&

			pUnit->DistanceFrom(pWoundedEnemyVehicle) <= pPrimaryWpn->Range)

		{

			pUnit->SetTarget(pWoundedEnemyVehicle);

			continue;
		}

		// Slot 79 (Surgical Enemy Power, Superweapon & Factory Snipe): When inside

		// range of a critical enemy structure, prioritize it over walls/silos!

		if (pSurgicalEnemyBld && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && scores[79] > 80 && pUnit->DistanceFrom(pSurgicalEnemyBld) <= 2816)

		{

			DispatchFootToTargetOrTransport(pUnit, pSurgicalEnemyBld, Mission::Attack, true);

			continue;
		}

		// Slot 80 (Fast Flanker & Jumpjet Counter-Battery vs Enemy Artillery):

		if (pEnemyArtilleryUnit && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && (pType->Speed >= 8 || pType->JumpJet) && scores[80] > 80 && pUnit->DistanceFrom(pEnemyArtilleryUnit) <= 4608)

		{

			DispatchFootToTargetOrTransport(pUnit, pEnemyArtilleryUnit, Mission::Attack, true);

			continue;
		}

		// Slot 78 (High-Ground Cliff Advantage Positioning): Siege/long-range units

		// step onto nearby higher-elevation cells (CellClass::Level) when idle!

		if (isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256) && !pType->Naval && scores[78] > 90 && (Unsorted::CurrentFrame % 120 < 20))

		{

			if (auto const pCurC = pUnit->GetCell())

			{

				const CellStruct uc = pUnit->GetMapCoords();

				for (int dy = -4; dy <= 4; dy += 2)

				{

					bool movedHigh = false;

					for (int dx = -4; dx <= 4; dx += 2)

					{

						const CellStruct hc{static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy)};

						if (auto const pHC = MapClass::Instance.TryGetCellAt(hc))

						{

							if (pHC->Level > pCurC->Level && pHC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))

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

		if (pEnemyMindController && isUnitIdle && (pType->ImmuneToPsionics || (pPrimaryWpn && pPrimaryWpn->Range >= (6 * 256))) && pUnit->DistanceFrom(pEnemyMindController) <= 4608)

		{

			DispatchFootToTargetOrTransport(pUnit, pEnemyMindController, Mission::Attack, true);

			continue;
		}

		// Slot 58 (Tank Infantry Crushing): Heavy Crusher=yes tanks run over nearby

		// Crushable=yes enemy infantry (<= 3 cells) instead of wasting slow cannon

		// shells!

		if (pType->Crusher && !pType->Naval && scores[58] > 100 && (isUnitIdle || pUnit->CurrentMission == Mission::Attack) && (Unsorted::CurrentFrame % 45 < 15))

		{

			CellClass* pCrushCell = nullptr;

			for (auto const pEInf : InfantryClass::Array)

			{

				if (!pEInf || pEInf->InLimbo || !pEInf->IsAlive || !pEInf->Owner || !pEInf->Type || !pEInf->Type->Crushable || pEInf->Type->C4 || pEInf->Type->Ivan)
					continue;

				if (pEInf->Owner == pHouse || pHouse->IsAlliedWith(pEInf->Owner) || pEInf->Owner->IsNeutral())
					continue;

				if (pEInf->GetCell() && pEInf->GetCell()->GetBuilding() == nullptr && pUnit->DistanceFrom(pEInf) <= 768 && IsCellVisibleToAI(pHouse, pEInf->GetCell()))

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

		if (pPrimaryWpn && pPrimaryWpn->Range >= (8 * 256) && scores[58] > 140 && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 45 < 15))

		{

			bool enemyTooClose = false;

			for (auto const pCloseEnemy : UnitClass::Array)

			{

				if (!pCloseEnemy || pCloseEnemy->InLimbo || !pCloseEnemy->IsAlive || !pCloseEnemy->Owner)
					continue;

				if (pCloseEnemy->Owner == pHouse || pHouse->IsAlliedWith(pCloseEnemy->Owner) || pCloseEnemy->Owner->IsNeutral())
					continue;

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

				const CellStruct kiteCoords{static_cast<short>(uc.X + stepX), static_cast<short>(uc.Y + stepY)};

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

		// Slot 56 (Own Harvester Defense & Enemy Harvester Eco-Raid):

		if (pAttackedOwnHarvester && isUnitIdle && pUnit->DistanceFrom(pAttackedOwnHarvester) <= 4608)

		{

			if (auto const pHarvCell = pAttackedOwnHarvester->GetCell())

			{

				DispatchFootToTargetOrTransport(pUnit, pHarvCell, Mission::AttackMove, false);

				continue;
			}
		}

		else if (pVisibleEnemyHarvester && isUnitIdle && scores[56] > 220 && pType->Speed >= 6 && pUnit->DistanceFrom(pVisibleEnemyHarvester) <= 6144)

		{

			DispatchFootToTargetOrTransport(pUnit, pVisibleEnemyHarvester, Mission::Attack, true);

			continue;
		}

		// Slot 57 (Surgical Bunker/Garrison Penetrator & Siege Focus):

		// 1) Units with PenetratesBunker=yes kill enemy occupants inside civilian

		// structures without demolishing the building so our infantry can take it!

		// 2) Long-range siege vehicles prioritize enemy garrisons & base defenses!

		if (pEnemyGarrisonToClear && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Warhead && pPrimaryWpn->Warhead->PenetratesBunker && pUnit->DistanceFrom(pEnemyGarrisonToClear) <= 4608)

		{

			DispatchFootToTargetOrTransport(pUnit, pEnemyGarrisonToClear, Mission::Attack, true);

			continue;
		}

		if (pPrioritySiegeBld && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Range >= (7 * 256) && pUnit->DistanceFrom(pPrioritySiegeBld) <= 5632)

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

		if (pStolenConYardOrMCV && pUnit != pStolenConYardOrMCV && !pType->Naval && scores[82] > 60 && pUnit->DistanceFrom(pStolenConYardOrMCV) <= 4096)

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

		// 1) Keep Spawner=yes units (Aircraft Carrier, Dreadnought, V3) at standoff

		// distance (> 5 cells) from approaching enemies!

		if (pPrimaryWpn && pPrimaryWpn->Spawner && scores[60] > 100 && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 45 < 15))

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

				const CellStruct retreatCell{static_cast<short>(uc.X + sx), static_cast<short>(uc.Y + sy)};

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

		// 2) Escort our lead Heavy/Siege Tank with mobile AA, Sensor, GapGenerator

		// & Mobile Stealth Generator vehicles (Slots 60 & 63)!

		if (pLeadHeavyTank && pUnit != pLeadHeavyTank && isUnitIdle && !pType->Naval &&

			(((pPrimaryWpn && pPrimaryWpn->Projectile && pPrimaryWpn->Projectile->AA) || pType->Sensors) && scores[60] > 120 || pType->GapGenerator && scores[63] > 80) &&

			(Unsorted::CurrentFrame % 90 < 20))

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

		if (pType->CanDisguise && !pType->PermaDisguise && !pUnit->Disguised && pSampleEnemyInfantry && pUnit->CanDisguiseAs(pSampleEnemyInfantry) && scores[61] > 80)
			pUnit->DisguiseAs(pSampleEnemyInfantry);

		// Slot 83 (Mirage Tank & Camouflage Tree-Grove Ambush, Spotted Retreat &

		// Ambush Zone Rotation): 1) Position Mirage Tanks (CanDisguise /

		// PermaDisguise) and ground Stealth Ambush tanks (Cloakable && !Naval)

		// inside tree groves (GetTerrain) along enemy Harvester/reinforcement

		// corridors and mimic the exact tree species of the grove! 2) Ambush

		// unsuspecting critical enemy units (Harvesters, Engineers, Artillery,

		// wounded vehicles) that pass within weapon range without leaving the tree

		// grove. 3) If spotted/attacked by the enemy (HasBeenAttacked, HP < 78%,

		// targeted by enemy tanks, or inside enemy DisguiseSensors), retreat

		// immediately back to our home base / Service Depot! 4) After regrouping or

		// after an ambush epoch (~900 frames), rotate to a different tree grove

		// sector to keep harassing the enemy from new angles!

		const bool isCamoAmbushTank =

			!pType->Naval && !pType->Harvester && !pType->ResourceGatherer && (pType->CanDisguise || pType->PermaDisguise || pType->Cloakable) && pPrimaryWpn && pPrimaryWpn->Damage > 0;

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

					const CellStruct nc{static_cast<short>(uc.X + dx), static_cast<short>(uc.Y + dy)};

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

			if ((pType->CanDisguise || pType->PermaDisguise) && pNearTree && isUnitIdle && (!pUnit->Disguised || (Unsorted::CurrentFrame % 300 == 0)) &&

				pUnit->CanDisguiseAs(reinterpret_cast<AbstractClass*>(pNearTree)))

			{

				pUnit->DisguiseAs(reinterpret_cast<AbstractClass*>(pNearTree));
			}

			// Check if the enemy has spotted / compromised our ambush ("si les

			// pillan")

			bool ambushSpotted = (pUnit->GetHealthPercentage() < 0.78) || (pUnit->HasBeenAttacked && pUnit->GetHealthPercentage() < 0.92);

			if (!ambushSpotted && pUC && pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < 24 && pUC->DisguiseSensors_InclHouse(static_cast<unsigned int>(pHouse->EnemyHouseIndex)))
				ambushSpotted = true;

			if (!ambushSpotted)

			{

				int closeHostileTanks = 0;

				for (auto const pEU : UnitClass::Array)

				{

					if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Harvester || pEU->Type->ResourceGatherer)
						continue;

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

			if (isUnitIdle && pUnit->GetHealthPercentage() >= 0.85 && (adjacentTrees == 0 || uc.DistanceFrom(homeCoords) <= 11.0 || shouldRotateGrove) && (Unsorted::CurrentFrame % 60 < 20))

			{

				CellStruct enemyFocus = homeCoords;

				if (pVisibleEnemyHarvester)
					enemyFocus = pVisibleEnemyHarvester->GetMapCoords();

				else if (pSurgicalEnemyBld)
					enemyFocus = pSurgicalEnemyBld->GetMapCoords();

				else if (pPrioritySiegeBld)
					enemyFocus = pPrioritySiegeBld->GetMapCoords();

				const CellStruct searchCenter{static_cast<short>((homeCoords.X + enemyFocus.X) / 2), static_cast<short>((homeCoords.Y + enemyFocus.Y) / 2)};

				const int desiredSector = ((Unsorted::CurrentFrame / 900) + pUnit->UniqueID) % 4;

				CellClass* pBestGroveCell = nullptr;

				int bestGroveScore = -99999;

				for (int dy = -28; dy <= 28; dy += 2)

				{

					for (int dx = -28; dx <= 28; dx += 2)

					{

						const CellStruct gc{static_cast<short>(searchCenter.X + dx), static_cast<short>(searchCenter.Y + dy)};

						auto const pGC = MapClass::Instance.TryGetCellAt(gc);

						if (!pGC || pGC->GetTerrain(false) != nullptr || !pGC->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							continue;

						if (std::max(std::abs(gc.X - homeCoords.X), std::abs(gc.Y - homeCoords.Y)) <= 11 || std::max(std::abs(gc.X - uc.X), std::abs(gc.Y - uc.Y)) <= 6)
							continue;

						// Count adjacent trees & nearby Ore (Harvester transit route!)

						int groveTrees = 0;

						bool nearOreRoute = (pGC->GetContainedTiberiumValue() > 0);

						for (int ty = -1; ty <= 1; ++ty)

						{

							for (int tx = -1; tx <= 1; ++tx)

							{

								const CellStruct tc2{static_cast<short>(gc.X + tx), static_cast<short>(gc.Y + ty)};

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

							if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || !pEB->Type->IsBaseDefense)
								continue;

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

						int gScore = groveTrees * 35 + (nearOreRoute ? 90 : 0) + (cellSector == desiredSector ? 120 : 0) - (std::abs(gc.X - enemyFocus.X) + std::abs(gc.Y - enemyFocus.Y));

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

			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner && pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && scores[62] > 60)

			{

				DispatchFootToTargetOrTransport(pUnit, pCurTargetInf->SlaveOwner, Mission::Attack, true);

				continue;
			}
		}

		if (pEnemySlaveMiner && isUnitIdle && !pType->Naval && scores[62] > 140 && pUnit->DistanceFrom(pEnemySlaveMiner) <= 4096)

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

		const bool canCureParasite = pPrimaryWpn && (pPrimaryWpn->Damage < 0 || (pWH && pWH->Sonic) || (pWHExt && pWHExt->RemoveParasite.Get(false)));

		if (pParasitedOwnUnit && pParasitedOwnUnit != pUnit && isUnitIdle && canCureParasite && scores[64] > 60 && pUnit->DistanceFrom(pParasitedOwnUnit) <= 4096)

		{

			DispatchFootToTargetOrTransport(pUnit, pParasitedOwnUnit, Mission::Attack, true);

			continue;
		}

		// Slot 64 (Offensive Parasite Targeting: Terror Drone, Giant Squid,

		// Cyberworm): Target expensive enemy tanks/harvesters/warships that do NOT

		// yet have a parasite!

		if (pWH && pWH->Parasite && isUnitIdle && pBestParasiteVictim && pUnit->ParasiteImUsing == nullptr && scores[64] > 60 && pType->Naval == pBestParasiteVictim->Type->Naval &&

			pUnit->DistanceFrom(pBestParasiteVictim) <= 4608)

		{

			DispatchFootToTargetOrTransport(pUnit, pBestParasiteVictim, Mission::Attack, true);

			continue;
		}

		// Slot 66 (EMP Pulse Targeting & EMP-Paralyzed Target Finisher):

		if (pWH && pWH->EMEffect && isUnitIdle && pEnemyNonEMPedHeavy && scores[66] > 80 && pUnit->DistanceFrom(pEnemyNonEMPedHeavy) <= 4096)

		{

			DispatchFootToTargetOrTransport(pUnit, pEnemyNonEMPedHeavy, Mission::Attack, true);

			continue;
		}

		else if (pEnemyEMPedTarget && isUnitIdle && pPrimaryWpn && pPrimaryWpn->Damage > 0 && (!pWH || !pWH->EMEffect) && scores[66] > 100 && pUnit->DistanceFrom(pEnemyEMPedTarget) <= 3584)

		{

			DispatchFootToTargetOrTransport(pUnit, pEnemyEMPedTarget, Mission::Attack, true);

			continue;
		}

		// Slot 67 (Tank Bunker Garrisoning): Put idle Bunkerable=yes combat tanks

		// into an empty Tank Bunker for armored perimeter defense!

		if (pEmptyTankBunker && isUnitIdle && pType->Bunkerable && !pType->Naval && !pType->Harvester && pUnit->BunkerLinkedItem == nullptr && scores[67] > 80 &&

			pUnit->DistanceFrom(pEmptyTankBunker) <= 3584)

		{

			DispatchFootToTargetOrTransport(pUnit, pEmptyTankBunker, Mission::Enter, false);

			pEmptyTankBunker = nullptr;

			continue;
		}

		// Slot 68 (Psychedelic Chaos Gas): Target non-berserk heavy enemy tanks!

		if (pWH && pWH->Psychedelic && isUnitIdle && pEnemyChaosTarget && scores[68] > 80 && pUnit->DistanceFrom(pEnemyChaosTarget) <= 3584)

		{

			DispatchFootToTargetOrTransport(pUnit, pEnemyChaosTarget, Mission::Attack, true);

			continue;
		}

		// Slot 69 (Submarine Cloak Ambush & Anti-Submarine Destroyer Escort):

		if (pType->Naval && scores[69] > 80)

		{

			if (pType->Cloakable && pUnit->GetHealthPercentage() < 0.45 && homeCoords != CellStruct::Empty && pUnit->CurrentMission != Mission::Move)

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

						pUnit->SetDestination(pSubCell, true);

						pUnit->QueueMission(Mission::AttackMove, true);

						continue;
					}
				}
			}

			else if (pType->Sensors && pLeadWarship && pUnit != pLeadWarship && isUnitIdle && (Unsorted::CurrentFrame % 90 < 20))

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

				if (!pAllyU || pAllyU == pUnit || !pAllyU->Owner || !pHouse->IsAlliedWith(pAllyU->Owner) || pAllyU->InLimbo || !pAllyU->IsAlive || !pAllyU->Type || pAllyU->Type->Organic)
					continue;

				if ((pAllyU->ParasiteEatingMe != nullptr || pAllyU->GetHealthPercentage() < 0.85) && pUnit->DistanceFrom(pAllyU) <= 3584)

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

				if (!pEBld || pEBld->InLimbo || !pEBld->IsAlive || !pEBld->Owner || !pEBld->Type || pEBld->DrainingMe != nullptr)
					continue;

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

		// Tank Bunker Garrisoning (Bunkerable=yes into empty owned Tank Bunker):

		// Fortify base perimeter!

		if (pType->Bunkerable && isUnitIdle && pUnit->BunkerLinkedItem == nullptr && (Unsorted::CurrentFrame % 90 < 20))

		{

			for (auto const pBld : pHouse->Buildings)

			{

				if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Bunker || pBld->BunkerLinkedItem != nullptr)
					continue;

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

		if (scores[21] > 400 && !pTargetBridgeHut && pBld->Type->BridgeRepairHut && MapClass::Instance.IsLinkedBridgeDestroyed(pBld->GetMapCoords()))
			pTargetBridgeHut = pBld;

		if (pBld->Owner && pHouse->IsAlliedWith(pBld->Owner))

		{

			if (pBld->Owner == pHouse && pBld->Type->InfantryAbsorb && (pBld->Type->PowerBonus > 0 || pBld->Type->ExtraPowerBonus > 0) && !IsTunnelNetworkBuilding(pBld->Type) &&

				pBld->Passengers.NumPassengers < pBld->Type->Passengers)

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
			if (pCheckInf && pCheckInf->Owner == pHouse && !pCheckInf->InLimbo && pCheckInf->IsAlive && pCheckInf->Destination == pBioReactor)
				infAlreadyHeadingToBioReactor++;
		}
		if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor >= pBioReactor->Type->Passengers)
			pBioReactor = nullptr;
	}
	const bool bioReactorNeedsPower = pBioReactor && (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350);
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

			else if (pInf->Type->Deployer && pInf->Type->ImmuneToRadiation && scores[63] > 80 && (Unsorted::CurrentFrame % 45 < 15))

			{

				int enemiesInRadRange = 0;

				for (auto const pET : TechnoClass::Array)

				{

					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner || pET->WhatAmI() == AbstractType::Building || pET->WhatAmI() == AbstractType::Aircraft)
						continue;

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
		const bool isIdle = (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard);

		const auto pInfWpn = pInf->Type->GetWeapon(0) ? pInf->Type->GetWeapon(0)->WeaponType : nullptr;

		// Slot 82 (Infantry Human Shield & Anti-Engineer Recapture Screen for

		// Stolen ConYard / Escaping MCV): Nearby friendly combat infantry (<= 14

		// cells) immediately gun down enemy Engineers trying to recapture our

		// stolen ConYard, or form a human shield perimeter around the stolen

		// ConYard/MCV to absorb enemy fire during extraction!

		if (pStolenConYardOrMCV && !pInf->Type->Engineer && !pInf->Type->Infiltrate && scores[82] > 60 && pInf->DistanceFrom(pStolenConYardOrMCV) <= 3584)

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

		if (pInf->Type->Deployer && pInf->Target != nullptr && pInf->CurrentMission == Mission::Attack && scores[57] > 100 && (Unsorted::CurrentFrame % 60 < 20))

		{

			pInf->QueueMission(Mission::Unload, true);

			continue;
		}

		// Slot 58 (Mind-Control Breaker): Snipers / Virus / ImmuneToPsionics

		// infantry eliminate enemy Yuri/Mastermind controllers!

		if (pEnemyMindController && isIdle && (pInf->Type->ImmuneToPsionics || (pInfWpn && pInfWpn->Range >= (6 * 256))) && pInf->DistanceFrom(pEnemyMindController) <= 4096)

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
			if (pBioReactor && pInf->Destination != pBioReactor &&
				(bioReactorNeedsPower || !pGrinderBld || pInf->DistanceFrom(pBioReactor) <= pInf->DistanceFrom(pGrinderBld) + 1536))
			{
				DispatchFootToTargetOrTransport(pInf, pBioReactor, Mission::Enter, false);
				infAlreadyHeadingToBioReactor++;
				if (pBioReactor->Passengers.NumPassengers + infAlreadyHeadingToBioReactor >= pBioReactor->Type->Passengers)
					pBioReactor = nullptr;
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

		if (pInf->Type->TiberiumHeal && pInf->GetHealthPercentage() < 0.55 && scores[68] > 60 && pInf->CurrentMission != Mission::Move)

		{

			if (pInf->MoveToTiberium(8, false))
				continue;
		}

		if (pEnemySpecialistInf && isIdle && pInfWpn && pInfWpn->Damage > 0 && !pInf->Type->Engineer && ((pInfWpn->Warhead && pInfWpn->Warhead->Poison) || pInfWpn->Range >= (6 * 256)) &&

			scores[68] > 80 && pInf->DistanceFrom(pEnemySpecialistInf) <= 4096)

		{

			DispatchFootToTargetOrTransport(pInf, pEnemySpecialistInf, Mission::Attack, true);

			continue;
		}

		// Retreat wounded valuable/Elite infantry (< 55% HP) to Hospital if owned!

		if (pHospital && !pInf->Type->Engineer && (pInf->Type->Cost >= 300 || pInf->Veterancy.IsElite()) && pInf->GetHealthPercentage() < 0.55)

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

				if (!pAllyInf || pAllyInf == pInf || !pAllyInf->Owner || !pHouse->IsAlliedWith(pAllyInf->Owner) || pAllyInf->InLimbo || !pAllyInf->IsAlive)
					continue;

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

				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) && pEnemyU->Type->Cost > bestSpecCost && pInf->DistanceFrom(pEnemyU) <= 4608)

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

				if (pAllyBld && !pAllyBld->InLimbo && pAllyBld->IsAlive && pAllyBld->Owner && pHouse->IsAlliedWith(pAllyBld->Owner) && pAllyBld->AttachedBomb != nullptr &&

					pInf->DistanceFrom(pAllyBld) <= 3584)

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

				const bool isHighValueEnemyBld = !isNeutralTech && (isCashTechBld || pBld->Type->ConstructionYard || pBld->Type->Refinery || pBld->Type->Factory != AbstractType::None ||
																	pBld->Type->SuperWeapon != -1 || pBld->Type->PowerBonus > 0);

				const bool canCaptureNow =
					isNeutralTech || (pBld->GetHealthPercentage() <= capLevel + 0.05) || (engCaptureBias > 60 && isHighValueEnemyBld) || (scores[82] > 60 && pBld->Type->ConstructionYard);

				if (!canCaptureNow)
					continue;

				// Coordinate multiple Engineers: do not send a 2nd Engineer to a neutral tech building already targeted by another friendly Engineer!
				if (isNeutralTech)
				{
					bool alreadyTargetedByOtherEng = false;
					for (auto const pOtherInf : InfantryClass::Array)
					{
						if (pOtherInf && pOtherInf != pInf && pOtherInf->Owner == pHouse && !pOtherInf->InLimbo && pOtherInf->IsAlive && pOtherInf->Type && pOtherInf->Type->Engineer &&
							pOtherInf->Destination == pBld)
						{
							alreadyTargetedByOtherEng = true;
							break;
						}
					}
					if (alreadyTargetedByOtherEng)
						continue;
				}

				const int dist = pInf->DistanceFrom(pBld);

				// Strongly prioritize Cash-Producing Oil Derricks (ProduceCashAmount > 0) and BaseNormal expansion hubs!
				const bool isStrategicTechHub = isCashTechBld || BuildingTypeExt::IsAIBaseNormal(pBld->Type);
				const int effectiveDist = (!isNeutralTech && (pBld->Type->ConstructionYard || isCashTechBld) && scores[82] > 60)
					? (dist / 3)
					: (isNeutralTech ? (isCashTechBld ? (dist / 3) : (isStrategicTechHub ? (dist * 2 / 5) : (dist / 2))) : dist);

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

		if (pInf->Type->CanDisguise && pSampleEnemyInfantry && scores[61] > 60 && (!pInf->Disguised || !pInf->DisguisedAsHouse || pHouse->IsAlliedWith(pInf->DisguisedAsHouse)) &&

			pInf->CanDisguiseAs(pSampleEnemyInfantry))

		{

			pInf->DisguiseAs(pSampleEnemyInfantry);
		}

		// Slot 62 (Slave Miner Liberation for Infantry): Redirect fire from Slaved

		// workers onto their SlaveOwner so killing the miner frees the slaves!

		if (auto const pCurTargetInf = abstract_cast<InfantryClass*>(pInf->Target))

		{

			if (pCurTargetInf->Type && pCurTargetInf->Type->Slaved && pCurTargetInf->SlaveOwner && pCurTargetInf->SlaveOwner->IsAlive && !pCurTargetInf->SlaveOwner->InLimbo && scores[62] > 60)

			{

				DispatchFootToTargetOrTransport(pInf, pCurTargetInf->SlaveOwner, Mission::Attack, true);

				continue;
			}
		}

		// Spy Full-Spectrum Sabotage: Infiltrate Enemy Power Plants (Blackout!),

		// Battle Labs (Secret Tech!), Superweapons, Refineries & Factories

		if (pInf->Type->Infiltrate && isIdle)

		{

			BuildingClass* pBestSpyTarget = nullptr;

			int bestSpyScore = -1;

			for (auto const pEnemyBld : BuildingClass::Array)

			{

				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;

				if (pHouse->IsAlliedWith(pEnemyBld->Owner) || pEnemyBld->Owner->IsNeutral())
					continue;

				int spyScore = 0;

				if (pEnemyBld->Type->SuperWeapon != -1)
					spyScore = 100; // Reset enemy Superweapon countdown!

				else if (pEnemyBld->Type->Refinery)
					spyScore = 90; // Steal enemy credits!

				else if (pEnemyBld->Type->PowerBonus > 0)
					spyScore = 85; // Black out enemy base defenses!

				else if (pEnemyBld->Type->Factory != AbstractType::None)
					spyScore = 80; // Gain Veteran production!

				else if (pEnemyBld->Type->TechLevel >= 6)
					spyScore = 75; // Battle Lab / Radar sabotage!

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

		// Credit Thief (Thief=yes): Infiltrate enemy Refineries & Ore Silos to

		// steal 50% of the enemy house's credits!

		if (pInf->Type->Thief && !pInf->Type->Infiltrate && isIdle)

		{

			BuildingClass* pBestThiefTarget = nullptr;

			int bestThiefDist = 999999;

			for (auto const pEnemyBld : BuildingClass::Array)

			{

				if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type)
					continue;

				if (pHouse->IsAlliedWith(pEnemyBld->Owner) || pEnemyBld->Owner->IsNeutral())
					continue;

				if (pEnemyBld->Type->Refinery || pEnemyBld->Type->Storage > 0)

				{

					const int d = pInf->DistanceFrom(pEnemyBld);

					if (d < bestThiefDist)

					{

						bestThiefDist = d;

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

		// Hijacker (VehicleThief=yes): Steal the most expensive visible enemy tank

		// (Apocalypse, Mammoth Mk.II, Avatar, Prism Tank)!

		if (pInf->Type->VehicleThief && isIdle)

		{

			UnitClass* pBestStealTank = nullptr;

			int bestTankCost = 700;

			for (auto const pEnemyU : UnitClass::Array)

			{

				if (!pEnemyU || pEnemyU->InLimbo || !pEnemyU->IsAlive || !pEnemyU->Owner || !pEnemyU->Type)
					continue;

				if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
					continue;

				if (pEnemyU->Type->Naval || pEnemyU->Type->Organic || pEnemyU->ParasiteEatingMe != nullptr)
					continue;

				if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) && pEnemyU->Type->Cost > bestTankCost && pInf->DistanceFrom(pEnemyU) <= 5120)

				{

					bestTankCost = pEnemyU->Type->Cost;

					pBestStealTank = pEnemyU;
				}
			}

			if (pBestStealTank)

			{

				DispatchFootToTargetOrTransport(pInf, pBestStealTank, Mission::Enter, false);

				continue;
			}
		}

		// Slot 61 (Guard Dogs & Counter-Spy Detectors): Immediately intercept any

		// enemy Spy, Engineer, Thief, Hijacker, or Commando approaching our base,

		// or patrol around our ConYard!

		if ((pInf->Type->Doggie || pInf->Type->DetectDisguise || pInf->Type->Sensors) && isIdle)

		{

			if (pDetectedEnemyInfiltrator && pInf->DistanceFrom(pDetectedEnemyInfiltrator) <= 3840)

			{

				DispatchFootToTargetOrTransport(pInf, pDetectedEnemyInfiltrator, Mission::Attack, true);

				continue;
			}

			if (pHiddenOrStealthEnemy && (Unsorted::CurrentFrame % 90 < 25))

			{

				if (auto const pSweepCell = pHiddenOrStealthEnemy->GetCell())

				{

					DispatchFootToTargetOrTransport(pInf, pSweepCell, Mission::AttackMove, false);

					continue;
				}
			}

			if (pInf->Type->Doggie && homeCoords != CellStruct::Empty && (Unsorted::CurrentFrame % 120 < 20))

			{

				if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(homeCoords))

				{

					if (pInf->DistanceFrom(pHomeCell) > 2048)

					{

						DispatchFootToTargetOrTransport(pInf, pHomeCell, Mission::Area_Guard, false);
					}
				}

				continue;
			}
		}

		// C4 Commandos (Tanya, Navy SEAL, Boris, Cyborg Commando): Demolish visible

		// enemy Warships (if amphibious) or high-value enemy buildings!

		if (pInf->Type->C4 && isIdle)

		{

			TechnoClass* pBestC4Target = nullptr;

			int bestC4Dist = 5120;

			const bool canSwim = (pInf->Type->MovementZone == MovementZone::Amphibious || pInf->Type->MovementZone == MovementZone::AmphibiousDestroyer || pInf->Type->SpeedType == SpeedType::Hover);

			if (canSwim)

			{

				for (auto const pEnemyShip : UnitClass::Array)

				{

					if (!pEnemyShip || pEnemyShip->InLimbo || !pEnemyShip->IsAlive || !pEnemyShip->Owner || !pEnemyShip->Type || !pEnemyShip->Type->Naval)
						continue;

					if (pEnemyShip->Owner == pHouse || pHouse->IsAlliedWith(pEnemyShip->Owner) || pEnemyShip->Owner->IsNeutral())
						continue;

					if (pEnemyShip->GetCell() && IsCellVisibleToAI(pHouse, pEnemyShip->GetCell()))

					{

						const int d = pInf->DistanceFrom(pEnemyShip);

						if (d < bestC4Dist)

						{

							bestC4Dist = d;

							pBestC4Target = pEnemyShip;
						}
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

		// IFV (Gunner=yes), Battle Fortress (OpenTopped=yes) & Subterranean/Ground

		// APC Strike Team Boarding (Slot 53)!

		const int apcBoardBias = static_cast<int>(scores[53]);

		const int apcBoardWindow = std::clamp(20 + (apcBoardBias / 20), 10, 45);

		const int apcBoardRange = std::clamp(2304 + apcBoardBias * 2, 1536, 3840);

		if (isIdle && !pInf->Type->Infiltrate && (Unsorted::CurrentFrame % 90 < apcBoardWindow))

		{

			bool boardedCombatVehicle = false;

			for (auto const pCombVeh : UnitClass::Array)

			{

				if (!pCombVeh || pCombVeh->Owner != pHouse || pCombVeh->InLimbo || !pCombVeh->IsAlive || !pCombVeh->Type)
					continue;

				const int infPassengerSize = std::max(1, static_cast<int>(pInf->Type->TechnoTypeClass::Size));

				const bool fitsInVehicle = infPassengerSize <= pCombVeh->Type->SizeLimit && (pCombVeh->Passengers.GetTotalSize() + infPassengerSize) <= pCombVeh->Type->Passengers;

				const bool isUsefulOpenToppedGunner = !pInf->Type->Engineer && !pInf->Type->Doggie && !pInf->Type->Thief && !pInf->Type->VehicleThief && !pInf->Type->Ivan && pInfWpn &&

													pInfWpn->Damage > 0 && pInfWpn->Range >= (4 * 256);

				const bool isUsefulIFVPassenger = !pInf->Type->Doggie && !pInf->Type->Thief && !pInf->Type->VehicleThief && (!pInf->Type->Engineer || pParasitedOwnUnit != nullptr);

				const bool isGunnerIFV = (fitsInVehicle && isUsefulIFVPassenger && pCombVeh->Type->Gunner && pCombVeh->Passengers.NumPassengers == 0);

				const bool isOpenToppedFortress = (fitsInVehicle && isUsefulOpenToppedGunner && pCombVeh->Type->OpenTopped && pCombVeh->Passengers.NumPassengers < pCombVeh->Type->Passengers);

				const bool isSubterraneanOrGroundAPC = (!pCombVeh->Type->Naval && !pCombVeh->Type->Gunner && !pCombVeh->Type->OpenTopped && pCombVeh->Type->Passengers >= 3 &&

														pCombVeh->Passengers.NumPassengers < pCombVeh->Type->Passengers &&

														(pCombVeh->Type->MovementZone == MovementZone::Subterrannean || pInf->Type->Engineer || pInf->Type->C4 || apcBoardBias > 80));

				if ((isGunnerIFV || isOpenToppedFortress || isSubterraneanOrGroundAPC) && pInf->DistanceFrom(pCombVeh) <= apcBoardRange)

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

		if (pOverpowerCoil && isIdle && (pInf->Type->Assaulter || (pInfWpn && pInfWpn->IsElectricBolt) || (pSecWpn && pSecWpn->IsElectricBolt)) && pInf->DistanceFrom(pOverpowerCoil) <= 3584)

		{

			DispatchFootToTargetOrTransport(pInf, pOverpowerCoil, Mission::Attack, true);

			pOverpowerCoil = nullptr;

			continue;
		}

		// Surgical Urban Garrison Clearing (Assaulter=yes or PenetratesBunker=yes):

		// Evict/kill enemy occupants inside civilian buildings without destroying

		// the structure so our Engineer can repair it and our infantry can occupy

		// it!

		if (pEnemyGarrisonToClear && isIdle && pInf->DistanceFrom(pEnemyGarrisonToClear) <= 3840)

		{

			if (pInf->Type->Assaulter)

			{

				DispatchFootToTargetOrTransport(pInf, pEnemyGarrisonToClear, Mission::Enter, false);

				continue;
			}

			else if (pInfWpn && pInfWpn->Warhead && pInfWpn->Warhead->PenetratesBunker)

			{

				DispatchFootToTargetOrTransport(pInf, pEnemyGarrisonToClear, Mission::Attack, true);

				continue;
			}
		}

		// Yuri Bio-Reactor Power Surge: Enter Bio-Reactor with basic infantry

		// during LowPower!

		if (pBioReactor && bioReactorNeedsPower && isIdle && pInf->Destination != pBioReactor &&
			!pInf->Type->Engineer && !pInf->Type->Infiltrate && !pInf->Type->Doggie &&
			pInf->Type->Cost <= 250 && pInf->DistanceFrom(pBioReactor) <= 4096)
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

				if (pBld->CanBeOccupiedBy(pInf) && pBld->Occupants.Count < maxOcc && pInf->DistanceFrom(pBld) <= garrisonRange)

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

	CellClass* pFallbackEnemyCell = nullptr;

	for (auto const pEnemyBld : BuildingClass::Array)

	{

		if (!pEnemyBld || pEnemyBld->InLimbo || !pEnemyBld->IsAlive || !pEnemyBld->Owner || !pEnemyBld->Type || pEnemyBld->Type->Immune || pEnemyBld->Type->Insignificant || pEnemyBld->Type->InvisibleInGame)
			continue;
		const CellStruct ebc = pEnemyBld->GetMapCoords();
		if (ebc.X <= 1 && ebc.Y <= 1)
			continue;

		auto const pOwner = pEnemyBld->Owner;

		if (pOwner == pHouse || pHouse->IsAlliedWith(pOwner) || pOwner->IsNeutral() || pOwner->IsObserver() || pOwner->Defeated)
			continue;

		auto const pCell = pEnemyBld->GetCell();

		if (!pCell)
			continue;

		if (!pFallbackEnemyCell || pOwner->ArrayIndex == pHouse->EnemyHouseIndex)
			pFallbackEnemyCell = pCell;

		const int ownerIdx = std::clamp(pOwner->ArrayIndex, 0, 7);

		const bool visible = pHouse->SpySatActive || IsCellVisibleToAI(pHouse, pCell) || ((pExt->AIBrain_InfiltratedEnemyMask & (1 << ownerIdx)) != 0) || (pOwner->WhoLastHurtMe == pHouse->ArrayIndex);

		if (visible)

		{

			pVisibleEnemyBld = pEnemyBld;

			if (pOwner->ArrayIndex == pHouse->EnemyHouseIndex)
				break;
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

				if (!pRemT || pRemT->InLimbo || !pRemT->IsAlive || !pRemT->Owner || pRemT->WhatAmI() == AbstractType::Building)
					continue;

				auto const pRO = pRemT->Owner;

				if (pRO == pHouse || pHouse->IsAlliedWith(pRO) || pRO->IsNeutral() || pRO->IsObserver() || pRO->Defeated)
					continue;

				if (auto const pRC = pRemT->GetCell())

				{

					pFallbackEnemyCell = pRC;

					if (pRO->ArrayIndex == pHouse->EnemyHouseIndex)
						break;
				}
			}
		}
	}

	const bool endgameRemnantHunt = (!pVisibleEnemyBld && pFallbackEnemyCell != nullptr && (allEnemyBldsGone || combatStalledForHunt));

	// Count idle combat units and infantry ready at base

	std::vector<UnitClass*> idleCombatUnits;

	std::vector<InfantryClass*> idleCombatInf;

	for (auto const pUnit : UnitClass::Array)

	{

		if (!pUnit || pUnit->Owner != pHouse || pUnit->InLimbo || !pUnit->IsAlive || !pUnit->Type)
			continue;

		if (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer || (pUnit->Type->DeploysInto && pUnit->Type->DeploysInto->ConstructionYard))
			continue;

		if (pUnit->GetHealthPercentage() < 0.40)
			continue;

		// Slot 83: Keep Mirage Tanks disguised inside their ambush tree grove

		// instead of pulling them into frontal suicide waves (unless all enemy

		// buildings are gone and we are hunting down final remnants)!

		if (!endgameRemnantHunt && (pUnit->Type->CanDisguise || pUnit->Type->PermaDisguise) && scores[83] > 60 && homeCoords != CellStruct::Empty &&

			pUnit->GetMapCoords().DistanceFrom(homeCoords) > 11.0)

		{

			continue;
		}

		if (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Area_Guard)
			idleCombatUnits.push_back(pUnit);
	}

	for (auto const pInf : InfantryClass::Array)

	{

		if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type)
			continue;

		if (pInf->Type->Engineer || pInf->Type->Infiltrate)
			continue;

		if (pInf->CurrentMission == Mission::Guard || pInf->CurrentMission == Mission::Area_Guard)
			idleCombatInf.push_back(pInf);
	}

	const int arch = pExt->AIBrain_Archetype;
	const int rushWaveBias = NeuralAI::PolicyFor(pHouse).OutputBias[37];
	const int reserveBias = -rushWaveBias;
	const int baseWave = (arch == 3) ? 3 : ((arch == 2 || arch == 4) ? 7 : 5);
	const size_t minWaveUnits = endgameRemnantHunt ? 1u : static_cast<size_t>(std::clamp(baseWave - (rushWaveBias / 140), 2, 10));
	const int blindPushFrame = (rushWaveBias > 100) ? 1350 : 3200;

	CellClass* pWaveTargetCell = pVisibleEnemyBld ? pVisibleEnemyBld->GetCell() : ((endgameRemnantHunt || Unsorted::CurrentFrame > blindPushFrame) ? pFallbackEnemyCell : nullptr);
	CellClass* pIslandGoalCell = pWaveTargetCell ? pWaveTargetCell : pFallbackEnemyCell;
	int targetIslandZone = -1;
	if (pIslandGoalCell)
		targetIslandZone = MapClass::Instance.GetMovementZoneType(pIslandGoalCell->MapCoords, MovementZone::Normal, false);
	const bool enemyOnDisconnectedIsland = (homeGroundZone >= 0 && targetIslandZone >= 0 && targetIslandZone != homeGroundZone);

	// If no enemy building is visible yet, send a fast scout unit/infantry
	const int scoutPeriod = std::clamp(225 - (static_cast<int>(scores[37]) / 4), 105, 360);
	if (!pVisibleEnemyBld && pFallbackEnemyCell && !enemyOnDisconnectedIsland && (Unsorted::CurrentFrame % scoutPeriod < 30))
	{
		if (!idleCombatUnits.empty())
		{
			auto const pScout = idleCombatUnits.front();
			pScout->SetDestination(pFallbackEnemyCell, true);
			pScout->QueueMission(Mission::AttackMove, true);
		}
		else if (!idleCombatInf.empty())
		{
			auto const pScoutInf = idleCombatInf.front();
			pScoutInf->SetDestination(pFallbackEnemyCell, true);
			pScoutInf->QueueMission(Mission::AttackMove, true);
		}
	}

	// Coordinated Offensive Strike Wave when strike force reaches evolved + archetype threshold (Slot 37: Rush vs Mass Timing)
	if (pWaveTargetCell && (endgameRemnantHunt || idleCombatUnits.size() >= minWaveUnits || (idleCombatUnits.size() + idleCombatInf.size() >= minWaveUnits + 4)))
	{
		const size_t desiredUnitReserve = endgameRemnantHunt ? 0u : static_cast<size_t>(std::clamp(1 + (reserveBias / 180), 0, 3));
		const size_t reserveCount = idleCombatUnits.size() > (desiredUnitReserve + 1) ? desiredUnitReserve : 0;
		size_t dispatchedVeh = 0;
		for (size_t i = reserveCount; i < idleCombatUnits.size(); ++i)
		{
			auto const pU = idleCombatUnits[i];
			const int uz = MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false);
			const bool canCrossWaterDirectly = (pU->Type->MovementZone == MovementZone::Amphibious || pU->Type->MovementZone == MovementZone::AmphibiousDestroyer ||
				pU->Type->MovementZone == MovementZone::AmphibiousCrusher || pU->Type->MovementZone == MovementZone::Fly ||
				pU->Type->MovementZone == MovementZone::Subterrannean || pU->Type->JumpJet || pU->Type->Teleporter);
			if (enemyOnDisconnectedIsland && uz != targetIslandZone && !canCrossWaterDirectly)
				continue; // Keep ground-only vehicles in Guard state on home island so they board Amphibious/Air Transports!

			if (pU->Type->JumpJet && pU->Type->Strength >= 1000 && pU->DistanceFrom(pWaveTargetCell) > 7680)
			{
				const CellStruct uCoords = pU->GetMapCoords();
				const CellStruct flankCoords{uCoords.X, pWaveTargetCell->MapCoords.Y};
				if (auto const pFlankCell = MapClass::Instance.TryGetCellAt(flankCoords))
				{
					pU->SetDestination(pFlankCell, true);
					pU->QueueMission(Mission::Move, true);
					dispatchedVeh++;
					continue;
				}
			}

			if ((pVisibleEnemyBld || endgameRemnantHunt) && pU->Type->GetWeapon(0) && pU->Type->GetWeapon(0)->WeaponType)
			{
				pU->SetDestination(pWaveTargetCell, true);
				pU->QueueMission(Mission::AttackMove, true);
			}
			else
			{
				pU->QueueMission(Mission::Hunt, true);
			}
			dispatchedVeh++;
		}

		const size_t desiredInfReserve = static_cast<size_t>(std::clamp(2 + (reserveBias / 140), 0, 4));
		const size_t infReserve = idleCombatInf.size() > (desiredInfReserve + 2) ? desiredInfReserve : 0;
		size_t dispatchedInf = 0;
		for (size_t i = infReserve; i < idleCombatInf.size(); ++i)
		{
			auto const pI = idleCombatInf[i];
			const int iz = MapClass::Instance.GetMovementZoneType(pI->GetMapCoords(), MovementZone::Normal, false);
			const bool canInfCrossWater = (pI->Type->MovementZone == MovementZone::Amphibious || pI->Type->MovementZone == MovementZone::AmphibiousDestroyer ||
				pI->Type->MovementZone == MovementZone::Fly || pI->Type->JumpJet || pI->Type->Teleporter);
			if (enemyOnDisconnectedIsland && iz != targetIslandZone && !canInfCrossWater)
				continue; // Keep ground infantry ready at home island to board transports!

			pI->SetDestination(pWaveTargetCell, true);
			pI->QueueMission(Mission::AttackMove, true);
			dispatchedInf++;
		}

		if (dispatchedVeh > 0 || dispatchedInf > 0)
		{
			Debug::Log("[NeuralAI] Frame %d | House #%d (%s) launched Offensive Wave (%zu Vehicles + %zu Infantry) -> Target Cell (%d,%d)\n", Unsorted::CurrentFrame, pHouse->ArrayIndex,
				(pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI", dispatchedVeh, dispatchedInf, pWaveTargetCell->MapCoords.X,
				pWaveTargetCell->MapCoords.Y);
		}
	}

	// Slot 75 (Immediate Beachhead Ground Push): Any combat vehicle or infantry

	// that has already landed on the enemy's island (unitZone ==

	// targetIslandZone) immediately pushes inland toward the enemy base instead

	// of waiting on the beach!

	if (enemyOnDisconnectedIsland && pIslandGoalCell && (Unsorted::CurrentFrame % 45 < 15))

	{

		for (auto const pBeachU : idleCombatUnits)

		{

			const int uz = MapClass::Instance.GetMovementZoneType(pBeachU->GetMapCoords(), MovementZone::Normal, false);

			if (uz == targetIslandZone)

			{

				pBeachU->SetDestination(pIslandGoalCell, true);

				pBeachU->QueueMission(Mission::AttackMove, true);
			}
		}

		for (auto const pBeachI : idleCombatInf)

		{

			const int iz = MapClass::Instance.GetMovementZoneType(pBeachI->GetMapCoords(), MovementZone::Normal, false);

			if (iz == targetIslandZone)

			{

				pBeachI->SetDestination(pIslandGoalCell, true);

				pBeachI->QueueMission(Mission::AttackMove, true);
			}
		}
	}

	// Count how many amphibious transports are currently loading on our home

	// island so loaded transports can synchronize and sail together as a D-Day

	// Convoy!

	UnitClass* pHomeExpansionDeployer = nullptr;
	if (homeGroundZone >= 0 && (pIslandResourceGoalCell != nullptr || !hasUncolonizedOreOnHomeIsland || pHouse->Buildings.Count >= 20))
	{
		for (auto const pOwnU : UnitClass::Array)
		{
			if (!pOwnU || pOwnU->Owner != pHouse || pOwnU->InLimbo || !pOwnU->IsAlive || !pOwnU->Type || !pOwnU->Type->DeploysInto)
				continue;
			const auto pUT = pOwnU->Type;
			const bool isSelfAmphibOrAir = (pUT->MovementZone == MovementZone::Amphibious || pUT->MovementZone == MovementZone::AmphibiousDestroyer ||
				pUT->MovementZone == MovementZone::AmphibiousCrusher || pUT->MovementZone == MovementZone::Fly || pUT->JumpJet);
			if (isSelfAmphibOrAir)
				continue;
			const int uz = MapClass::Instance.GetMovementZoneType(pOwnU->GetMapCoords(), MovementZone::Normal, false);
			if (uz != homeGroundZone)
				continue;
			const bool isExpMCV = (pUT->DeploysInto->ConstructionYard && conYardsOnHomeIsland >= 1);
			const bool isExpOutpostOrRef = (!pUT->DeploysInto->ConstructionYard && (pOwnU == pExtraHomeSlaveMiner ||
				(!pUT->Harvester && !pUT->ResourceGatherer && (pUT->DeploysInto->Refinery || pUT->DeploysInto->ResourceDestination || pUT->DeploysInto->BaseNormal))));
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
			if (!pOwnInf || pOwnInf->Owner != pHouse || pOwnInf->InLimbo || !pOwnInf->IsAlive || !pOwnInf->Type || !pOwnInf->Type->Engineer)
				continue;
			const int iz = MapClass::Instance.GetMovementZoneType(pOwnInf->GetMapCoords(), MovementZone::Normal, false);
			if (iz == homeGroundZone)
			{
				pHomeIslandEngineer = pOwnInf;
				break;
			}
		}
	}

	int homeTransportsStillLoading = 0;

	for (auto const pCheckT : UnitClass::Array)

	{

		if (!pCheckT || pCheckT->Owner != pHouse || pCheckT->InLimbo || !pCheckT->IsAlive || !pCheckT->Type || !IsAmphibiousOrAirTransport(pCheckT->Type))
			continue;

		const int tz = MapClass::Instance.GetMovementZoneType(pCheckT->GetMapCoords(), MovementZone::Normal, false);

		if (tz == homeGroundZone && pCheckT->Passengers.NumPassengers > 0 && pCheckT->Passengers.NumPassengers < 3 && pCheckT->Passengers.GetTotalSize() < 6 && !idleCombatUnits.empty())
			homeTransportsStillLoading++;
	}

	for (auto const pTrans : UnitClass::Array)

	{

		if (!pTrans || pTrans->Owner != pHouse || pTrans->InLimbo || !pTrans->IsAlive || !pTrans->Type)
			continue;

		// Subterranean APC / Ground APC Strike Team: If carrying Engineers or >= 3

		// Infantry, burrow/drive to enemy base and Unload!

		if (!IsAmphibiousOrAirTransport(pTrans->Type) && pTrans->Type->Passengers >= 3 && !pTrans->Type->OpenTopped && !pTrans->Type->Gunner)

		{

			if (pTrans->Passengers.NumPassengers > 0 && pVisibleEnemyBld)

			{

				bool hasEngineerOrFullSquad = (pTrans->Passengers.NumPassengers >= 3);

				for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr; pPass = abstract_cast<FootClass*>(pPass->NextObject))

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
							const int cz = MapClass::Instance.GetMovementZoneType(customCoords, MovementZone::Normal, false);
							pDropCell = FindSafeIslandLandingCell(pHouse, customCoords, cz, pCarriedUnit->Type && pCarriedUnit->Type->DeploysInto != nullptr);
						}
						if (!pDropCell && pCarriedUnit->Type)
						{
							if (pRepairDepot && pCarriedUnit->GetHealthPercentage() < 0.45)
								pDropCell = pRepairDepot->GetCell();
							else if ((pCarriedUnit->Type->DeploysInto != nullptr || pCarriedUnit->Type->Harvester || pCarriedUnit->Type->Weeder) && pIslandResourceGoalCell)
								pDropCell = pIslandResourceGoalCell;
						}
						if (!pDropCell)
							pDropCell = pIslandGoalCell ? pIslandGoalCell : (pIslandTechGoalCell ? pIslandTechGoalCell : (pIslandResourceGoalCell ? pIslandResourceGoalCell : pWaveTargetCell));
						if (pDropCell && pCarryExt->CarryallState == JumpjetCarryallState::Carrying)
						{
							if (pTrans->GetMapCoords().DistanceFrom(pDropCell->MapCoords) <= 2.5 && pCarryExt->CarryallCanDropAt(pTrans->GetCell()))
							{
								pTrans->QueueMission(Mission::Unload, true);
							}
							else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 90 < 25))
							{
								pTrans->SetDestination(pDropCell, true);
								pTrans->QueueMission(Mission::Move, true);
							}
						}
						continue;
					}
					else if (pCarryExt->CarryallState == JumpjetCarryallState::Idle &&
						(pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard) &&
						(Unsorted::CurrentFrame % 90 < 35))
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
							if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || !pCarryExt->CarryallCanLift(pU))
								continue;
							const int uZone = MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false);
							const bool isCriticallyDamagedTank = (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
							const bool isLoadedStrikeAPC = (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 1 && (pWaveTargetCell || pIslandGoalCell || pIslandTechGoalCell));
							const bool isIslandExpansionMCV = (pU->Type->DeploysInto != nullptr && pIslandResourceGoalCell && uZone == homeGroundZone &&
								(pHouse->NumConYards >= 1 || pHouse->CountOwnedNow(pU->Type) >= 2));
							const bool isIslandHarvester = ((pU->Type->Harvester || pU->Type->Weeder) && pIslandResourceGoalCell && uZone == homeGroundZone &&
								pHouse->CountOwnedNow(pU->Type) >= 2);
							const bool isIslandCombatVehicle = (pIslandGoalCell != nullptr && uZone == homeGroundZone && !IsAmphibiousOrAirTransport(pU->Type) &&
								!pU->Type->Harvester && !pU->Type->Weeder && pU->Type->DeploysInto == nullptr &&
								(pU->CurrentMission == Mission::Guard || pU->CurrentMission == Mission::Area_Guard));
							if (isCriticallyDamagedTank || isLoadedStrikeAPC || isIslandExpansionMCV || isIslandHarvester || isIslandCombatVehicle)
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

		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr; pPass = abstract_cast<FootClass*>(pPass->NextObject))

		{

			if (auto const pPassUnit = abstract_cast<UnitClass*>(pPass))

			{

				if (pPassUnit->Type && pPassUnit->Type->DeploysInto != nullptr)

				{

					carryingMCV = true; // MCV or Red Alert 3-style CABAL Expansion Nanocore!

					if (pPassUnit->Type->DeploysInto->ConstructionYard && pHouse->CountOwnedNow(pPassUnit->Type->DeploysInto) == 0 && pHouse->NumConYards >= 1)
						carryingStolenForeignMCV = true;

					break;
				}
			}
		}

		const bool isReadyToSail = carryingMCV || numPass >= 3 || totalPassSize >= 6;

		const CellStruct transCoords = pTrans->GetMapCoords();

		const int transZone = MapClass::Instance.GetMovementZoneType(transCoords, MovementZone::Normal, false);

		// Slot 82 (Island Extraction of Stolen Foreign MCV to Safe Home Island):

		// 1) If this transport is carrying a stolen foreign-faction MCV from an

		// enemy island, sail straight back to our safe Home Island (homeCoords /

		// homeGroundZone) and unload it there so it can deploy safely!

		if (carryingStolenForeignMCV && homeCoords != CellStruct::Empty && scores[82] > 60)

		{

			if (CellClass* pHomeSafeLanding = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, true))

			{

				auto const pTransCell = pTrans->GetCell();

				const bool onHomeIslandLand = (homeGroundZone >= 0 && transZone == homeGroundZone && pTransCell && pTransCell->LandType != LandType::Water);

				if (onHomeIslandLand && pTrans->DistanceFrom(pHomeSafeLanding) <= 1024)

				{

					if (pTrans->CurrentMission != Mission::Unload)
						pTrans->QueueMission(Mission::Unload, true);
				}

				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 90 < 25))

				{

					pTrans->SetDestination(pHomeSafeLanding, true);

					pTrans->QueueMission(Mission::Move, true);
				}

				continue;
			}
		}

		// 2) If we have a stolen foreign MCV stranded on an enemy island

		// (pStrandedStolenMCVOnIsland) and this transport is empty, dispatch it to

		// the island beach near the stolen MCV to pick it up!

		if (numPass == 0 && pStrandedStolenMCVOnIsland && scores[82] > 60)

		{

			const CellStruct mcvIslandCell = pStrandedStolenMCVOnIsland->GetMapCoords();

			const int mcvIslandZone = MapClass::Instance.GetMovementZoneType(mcvIslandCell, MovementZone::Normal, false);

			if (CellClass* pRescueBeach = FindSafeIslandLandingCell(pHouse, mcvIslandCell, mcvIslandZone, false))

			{

				if (pTrans->DistanceFrom(pRescueBeach) > 1024 && (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 90 < 25)))

				{

					pTrans->SetDestination(pRescueBeach, true);

					pTrans->QueueMission(Mission::Move, true);
				}

				continue;
			}
		}

		const bool waitForConvoySister = (homeTransportsStillLoading > 0 && transZone == homeGroundZone && !carryingMCV && (Unsorted::CurrentFrame % 240 < 150));

		bool carryingEngineer = false;
		bool carryingHarvester = false;
		CellClass* pCustomPassengerGoal = nullptr;
		int customPassengerZone = -1;
		for (FootClass* pPass = pTrans->Passengers.GetFirstPassenger(); pPass != nullptr; pPass = abstract_cast<FootClass*>(pPass->NextObject))
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
		if ((carryingMCV || carryingHarvester) && pIslandResourceGoalCell)
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
		const bool isReadyToSailEffective = isReadyToSail || carryingHarvester || (carryingEngineer && pIslandTechGoalCell != nullptr) || (pCustomPassengerGoal != nullptr);
		if (isReadyToSailEffective && pActiveIslandGoal && !waitForConvoySister)
		{
			CellClass* pSafeLanding = FindSafeIslandLandingCell(pHouse, pActiveIslandGoal->MapCoords, activeIslandZone, carryingMCV || carryingHarvester);
			if (pSafeLanding)
			{
				auto const pTransCell = pTrans->GetCell();
				const bool onTargetIslandLand = (activeIslandZone >= 0 && transZone == activeIslandZone && pTransCell && pTransCell->LandType != LandType::Water);
				if (onTargetIslandLand && transCoords.DistanceFrom(pSafeLanding->MapCoords) <= 4.0)
				{
					if (pTrans->Type->MovementZone == MovementZone::Fly || pTrans->Type->JumpJet)
						pTrans->SetDestination(nullptr, true);
					if (pTrans->CurrentMission != Mission::Unload)
						pTrans->QueueMission(Mission::Unload, true);
				}
				else if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 120 < 30))
				{
					pTrans->SetDestination(pSafeLanding, true);
					pTrans->QueueMission(Mission::Move, true);
				}
			}
		}

		else if (!isReadyToSailEffective && (pHomeExpansionDeployer != nullptr || !pendingCrossIslandOrders.empty() || enemyOnDisconnectedIsland || pIslandResourceGoalCell != nullptr || pIslandTechGoalCell != nullptr || scores[28] > 600 || scores[75] > 100))
		{
			auto const pTransCell = pTrans->GetCell();
			const bool canCarryHomeDeployer = (pHomeExpansionDeployer != nullptr && !carryingMCV &&
				static_cast<int>(pHomeExpansionDeployer->Type->TechnoTypeClass::Size) <= static_cast<int>(pTrans->Type->SizeLimit));
			const int reservedSizeForDeployer = canCarryHomeDeployer ? static_cast<int>(pHomeExpansionDeployer->Type->TechnoTypeClass::Size) : 0;

			if (canCarryHomeDeployer && (totalPassSize + reservedSizeForDeployer) <= pTrans->Type->Passengers)
			{
				CellClass* pDeployerPickupBeach = FindSafeIslandLandingCell(pHouse, pHomeExpansionDeployer->GetMapCoords(), homeGroundZone, false);
				const bool onHomeLand = (transZone == homeGroundZone && pTransCell && pTransCell->LandType != LandType::Water);
				if (pDeployerPickupBeach && (!onHomeLand || transCoords.DistanceFrom(pDeployerPickupBeach->MapCoords) > 3.5))
				{
					if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 60 < 20))
					{
						pTrans->SetDestination(pDeployerPickupBeach, true);
						pTrans->QueueMission(Mission::Move, true);
					}
					continue;
				}
				if (onHomeLand)
				{
					pHomeExpansionDeployer->SetDestination(pTrans, true);
					pHomeExpansionDeployer->QueueMission(Mission::Enter, true);
				}
			}
			else if (pHomeIslandEngineer != nullptr && !carryingEngineer && (totalPassSize + 1) <= pTrans->Type->Passengers)
			{
				CellClass* pEngPickupBeach = FindSafeIslandLandingCell(pHouse, pHomeIslandEngineer->GetMapCoords(), homeGroundZone, false);
				const bool onHomeLand = (transZone == homeGroundZone && pTransCell && pTransCell->LandType != LandType::Water);
				if (pEngPickupBeach && (!onHomeLand || transCoords.DistanceFrom(pEngPickupBeach->MapCoords) > 3.5))
				{
					if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 60 < 20))
					{
						pTrans->SetDestination(pEngPickupBeach, true);
						pTrans->QueueMission(Mission::Move, true);
					}
					continue;
				}
				if (onHomeLand)
				{
					pHomeIslandEngineer->SetDestination(pTrans, true);
					pHomeIslandEngineer->QueueMission(Mission::Enter, true);
				}
			}
			else if (!pendingCrossIslandOrders.empty())
			{
				bool handledPendingOrder = false;
				for (auto const& ord : pendingCrossIslandOrders)
				{
					if (!ord.Passenger || !ord.Passenger->IsAlive || ord.Passenger->InLimbo)
						continue;
					if (ord.PassengerSize > static_cast<int>(pTrans->Type->SizeLimit) || (totalPassSize + ord.PassengerSize) > pTrans->Type->Passengers)
						continue;
					CellClass* pOrdPickupBeach = FindSafeIslandLandingCell(pHouse, ord.Passenger->GetMapCoords(), ord.PickupZone, false);
					const bool onOrdLand = (transZone == ord.PickupZone && pTransCell && pTransCell->LandType != LandType::Water);
					if (pOrdPickupBeach && (!onOrdLand || transCoords.DistanceFrom(pOrdPickupBeach->MapCoords) > 3.5))
					{
						if (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 60 < 20))
						{
							pTrans->SetDestination(pOrdPickupBeach, true);
							pTrans->QueueMission(Mission::Move, true);
						}
						handledPendingOrder = true;
						break;
					}
					if (onOrdLand)
					{
						ord.Passenger->SetDestination(pTrans, true);
						ord.Passenger->QueueMission(Mission::Enter, true);
						handledPendingOrder = true;
						break;
					}
				}
				if (handledPendingOrder)
					continue;
			}

			const bool notAtHomeShore = (pTransCell && pTransCell->LandType == LandType::Water) || (homeGroundZone >= 0 && transZone != homeGroundZone);
			if (notAtHomeShore && homeCoords != CellStruct::Empty)
			{
				CellClass* pHomeBeach = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false);
				if (pHomeBeach && (pTrans->CurrentMission == Mission::Guard || pTrans->CurrentMission == Mission::Area_Guard || (Unsorted::CurrentFrame % 90 < 25)))
				{
					pTrans->SetDestination(pHomeBeach, true);
					pTrans->QueueMission(Mission::Move, true);
				}
				continue;
			}

			if ((pTrans->Type->MovementZone == MovementZone::Fly || pTrans->Type->JumpJet) && pTrans->GetHeight() > 0 && pTrans->Destination == nullptr &&
				pTransCell && pTransCell->LandType != LandType::Water && !pTransCell->GetBuilding())
			{
				if (pTrans->CurrentMission != Mission::Unload)
					pTrans->QueueMission(Mission::Unload, true);
			}
			int boardedNow = numPass;
			int sizeNow = totalPassSize;
			const int maxAllowedPassSize = std::max(0, pTrans->Type->Passengers - reservedSizeForDeployer);

			for (auto const pV : idleCombatUnits)
			{
				const int vehSize = static_cast<int>(pV->Type->TechnoTypeClass::Size);
				if (boardedNow >= pTrans->Type->Passengers || (sizeNow + vehSize) > maxAllowedPassSize)
					break;

				if (!IsAmphibiousOrAirTransport(pV->Type) && pV->Type->TechnoTypeClass::Size <= pTrans->Type->SizeLimit && pV->GetMapCoords().DistanceFrom(transCoords) <= 18.0)

				{

					pV->SetDestination(pTrans, true);

					pV->QueueMission(Mission::Enter, true);

					boardedNow++;

					sizeNow += vehSize;
				}
			}

			for (auto const pI : idleCombatInf)
			{
				if (boardedNow >= pTrans->Type->Passengers || (sizeNow + 1) > maxAllowedPassSize)
					break;

				if (static_cast<int>(pTrans->Type->SizeLimit) >= 1 && pI->GetMapCoords().DistanceFrom(transCoords) <= 18.0)

				{

					pI->SetDestination(pTrans, true);

					pI->QueueMission(Mission::Enter, true);

					boardedNow++;

					sizeNow += 1;
				}
			}
		}
	}

	// Dispatch loaded/idle combat Aircraft, Carryall «Matryoshka» Airlift &

	// Emergency Sell-Dodge / ConYard Undeploy

	for (auto const pAir : AircraftClass::Array)

	{

		if (!pAir || pAir->Owner != pHouse || pAir->InLimbo || !pAir->IsAlive || !pAir->Type)
			continue;

		const bool isAirIdle = (pAir->CurrentMission == Mission::Guard || pAir->CurrentMission == Mission::Area_Guard || pAir->CurrentMission == Mission::Sleep);

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
			for (FootClass* pPass = pAir->Passengers.GetFirstPassenger(); pPass != nullptr; pPass = abstract_cast<FootClass*>(pPass->NextObject))
			{
				if (!pAirCustomPassGoal)
				{
					const CellStruct passGoalCoords = LookupPassengerIslandGoal(pPass->UniqueID);
					if (passGoalCoords != CellStruct::Empty)
					{
						const int pgz = MapClass::Instance.GetMovementZoneType(passGoalCoords, MovementZone::Normal, false);
						if (pgz >= 0)
						{
							if (CellClass* pSafePassLanding = FindSafeIslandLandingCell(pHouse, passGoalCoords, pgz, false))
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
			const bool airReadyToFly = airCarryingMCV || airCarryingHarvester || (airCarryingEngineer && pIslandTechGoalCell != nullptr) ||
				(pAirCustomPassGoal != nullptr) || numAirPass >= 3 || totalAirPassSize >= 6 || numAirPass >= pAir->Type->Passengers;
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
					pAirGoal = pIslandResourceGoalCell ? pIslandResourceGoalCell : (pIslandTechGoalCell ? pIslandTechGoalCell : pWaveTargetCell);
					airGoalZone = pIslandResourceGoalCell ? resourceIslandZone : (pIslandTechGoalCell ? techIslandZone : -1);
				}
				if (pAirGoal)
				{
					CellClass* pSafeAirDrop = FindSafeIslandLandingCell(pHouse, pAirGoal->MapCoords, airGoalZone, airCarryingMCV || airCarryingHarvester);
					if (pSafeAirDrop)
					{
						if (airCoords.DistanceFrom(pSafeAirDrop->MapCoords) <= 4.0 && pSafeAirDrop->LandType != LandType::Water)
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
			else if (numAirPass == 0 && homeGroundZone >= 0 && airZone != homeGroundZone && homeCoords != CellStruct::Empty)
			{
				if (CellClass* pHomeAirBeach = FindSafeIslandLandingCell(pHouse, homeCoords, homeGroundZone, false))
				{
					if (isAirIdle)
					{
						pAir->SetDestination(pHomeAirBeach, true);
						pAir->QueueMission(Mission::Move, true);
					}
				}
			}
			else if (!pAir->IsInAir() && pAir->GetHeight() == 0 &&
				(!pendingCrossIslandOrders.empty() || enemyOnDisconnectedIsland || pIslandResourceGoalCell != nullptr || pIslandTechGoalCell != nullptr || scores[28] > 600 || scores[75] > 100))
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
						ord.Passenger->SetDestination(pAir, true);
						ord.Passenger->QueueMission(Mission::Enter, true);
						boardedAir++;
						sizeAir += ord.PassengerSize;
					}
				}
				for (auto const pV : idleCombatUnits)
				{
					const int vehSize = static_cast<int>(pV->Type->TechnoTypeClass::Size);
					if (boardedAir >= pAir->Type->Passengers || (sizeAir + vehSize) > pAir->Type->Passengers)
						break;
					if (!IsAmphibiousOrAirTransport(pV->Type) && vehSize <= static_cast<int>(pAir->Type->SizeLimit) &&
						pV->GetMapCoords().DistanceFrom(airCoords) <= 18.0)
					{
						pV->SetDestination(pAir, true);
						pV->QueueMission(Mission::Enter, true);
						boardedAir++;
						sizeAir += vehSize;
					}
				}
				for (auto const pI : idleCombatInf)
				{
					if (boardedAir >= pAir->Type->Passengers || (sizeAir + 1) > pAir->Type->Passengers)
						break;
					if (static_cast<int>(pAir->Type->SizeLimit) >= 1 && pI->GetMapCoords().DistanceFrom(airCoords) <= 18.0)
					{
						pI->SetDestination(pAir, true);
						pI->QueueMission(Mission::Enter, true);
						boardedAir++;
						sizeAir += 1;
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
					pDropCell = pRepairDepot->GetCell();
				else if ((pCarriedUnit->Type->DeploysInto != nullptr || pCarriedUnit->Type->Harvester || pCarriedUnit->Type->Weeder) && pIslandResourceGoalCell)
					pDropCell = pIslandResourceGoalCell;
			}
			if (!pDropCell)
				pDropCell = pIslandGoalCell ? pIslandGoalCell : (pIslandTechGoalCell ? pIslandTechGoalCell : (pIslandResourceGoalCell ? pIslandResourceGoalCell : pWaveTargetCell));
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

		if (pAir->Type->Carryall && isAirIdle && pAir->Passengers.NumPassengers == 0 && (Unsorted::CurrentFrame % 90 < carryallWindow))
		{
			for (auto const pU : UnitClass::Array)
			{
				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || !CanCarryallLiftUnit(pAir, pU))
					continue;
				const int uZone = MapClass::Instance.GetMovementZoneType(pU->GetMapCoords(), MovementZone::Normal, false);
				const bool isCriticallyDamagedTank = (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);
				const bool isLoadedStrikeAPC = (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 1 && (pWaveTargetCell || pIslandGoalCell || pIslandTechGoalCell));
				const bool isIslandExpansionMCV = (pU->Type->DeploysInto != nullptr && pIslandResourceGoalCell && uZone == homeGroundZone &&
					(pHouse->NumConYards >= 1 || pHouse->CountOwnedNow(pU->Type) >= 2));
				const bool isIslandHarvester = ((pU->Type->Harvester || pU->Type->Weeder) && pIslandResourceGoalCell && uZone == homeGroundZone &&
					pHouse->CountOwnedNow(pU->Type) >= 2);
				if (isCriticallyDamagedTank || isLoadedStrikeAPC || isIslandExpansionMCV || isIslandHarvester)
				{
					pAir->SetTarget(pU);
					pAir->QueueMission(Mission::Capture, true);
					break;
				}
			}
			continue;
		}
		if (false && pAir->Type->Carryall && isAirIdle && pAir->Passengers.NumPassengers == 0 && (Unsorted::CurrentFrame % 90 < carryallWindow))

		{

			for (auto const pU : UnitClass::Array)

			{

				if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type || pU->Type->Naval)
					continue;

				const bool isCriticallyDamagedTank = (pRepairDepot && pU->Type->Cost >= 1000 && pU->GetHealthPercentage() < 0.40);

				const bool isLoadedStrikeAPC = (pU->Type->Passengers >= 3 && pU->Passengers.NumPassengers >= 2 && pWaveTargetCell);

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

			if (pBld->Occupants.GetItem(0) && pBld->Occupants.GetItem(0)->Owner == pHouse && pBld->GetHealthPercentage() < 0.18)
				pBld->QueueMission(Mission::Unload, true);
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

				if (pCheckBld && !pCheckBld->InLimbo && pCheckBld->IsAlive && pCheckBld->Type && pCheckBld->Type->IsBaseDefense && pCheckBld->Type->Powered && pCheckBld->StuffEnabled)

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

				if (isLowPower && hasPoweredDefense && scores[60] > 80 && pOwnBld->StuffEnabled && !pOwnBld->Type->IsBaseDefense && pOwnBld->Type->PowerDrain >= 100 &&

					(pOwnBld->Type->SuperWeapon != -1 || pOwnBld->Type->Factory == AbstractType::None))

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

			if (enemyOnDisconnectedIsland && !pOwnBld->IsPrimaryFactory && (pOwnBld->Type->Factory == AbstractType::UnitType || pOwnBld->Type->Factory == AbstractType::InfantryType) &&

				!pOwnBld->Type->Naval)

			{

				const int bldZone = MapClass::Instance.GetMovementZoneType(pOwnBld->GetMapCoords(), MovementZone::Normal, false);

				if (false && bldZone == targetIslandZone)
					pOwnBld->TogglePrimaryFactory();
			}

			const double hpPct = pOwnBld->GetHealthPercentage();

			// Slot 73 (Dynamic Tank Bunker Unload): Eject the bunkered tank if the

			// Tank Bunker is about to collapse (< 22% HP) or if no enemies are near!

			if (pOwnBld->Type->Bunker && pOwnBld->BunkerLinkedItem != nullptr && scores[73] > 60)

			{

				if (hpPct < 0.22)
					pOwnBld->UnloadBunker();
			}

			if (hpPct < undeployThresh && pOwnBld->Type->ConstructionYard && pHouse->NumConYards == 1 && pOwnBld->Type->UndeploysInto != nullptr)

			{

				// Last ConYard is about to be destroyed! Pack back into an MCV to

				// survive!

				if (pOwnBld->CurrentMission != Mission::Unload)
					pOwnBld->QueueMission(Mission::Unload, true);
			}

			else if (pOwnBld->Type->ConstructionYard && pHouse->NumConYards >= 2 && pOwnBld != pHouse->ConYards.GetItem(0) && scores[82] > 60 && homeCoords != CellStruct::Empty &&

					pOwnBld->GetMapCoords().DistanceFrom(homeCoords) > 18.0)

			{

				// Slot 82 (Captured Enemy ConYard Immediate Pack, Extraction OR

				// Emergency Cash-Out Sale): 1) If we just captured an enemy ConYard

				// inside a hostile enemy base

				// (> 18 cells from our home base and enemy structures/units nearby):

				//    - If it CANNOT be extracted alive (e.g., MCVRedeploy=no while

				//    under

				// enemy fire, OR its HP is critically low < 32% with >= 2 enemies

				// attacking so it would die before escaping, OR it is trapped on an

				// enemy island with no Amphibious Transport / Shipyard while under

				// fire), immediately SELL IT (Mission::Selling) just like the classic

				// game to cash in ~$1500 extra credits + survivor infantry!

				//    - Otherwise, when MCVRedeploy=yes and extraction is viable, pack

				//    it

				// into an MCV (Mission::Unload) so our Human Shield escorts and/or

				// Amphibious Transports can bring it home!

				const CellStruct yc = pOwnBld->GetMapCoords();

				bool hostileZone = false;

				int closeHostileAttackers = 0;

				for (auto const pEB : BuildingClass::Array)

				{

					if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune)
						continue;

					if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral())
						continue;

					const CellStruct ec = pEB->GetMapCoords();

					const int dB = std::max(std::abs(yc.X - ec.X), std::abs(yc.Y - ec.Y));

					if (dB <= 16)

					{

						hostileZone = true;

						if (pEB->Type->IsBaseDefense && dB <= 10)
							closeHostileAttackers++;
					}
				}

				for (auto const pET : TechnoClass::Array)

				{

					if (!pET || pET->InLimbo || !pET->IsAlive || !pET->Owner || pET->WhatAmI() == AbstractType::Building)
						continue;

					if (pET->Owner == pHouse || pHouse->IsAlliedWith(pET->Owner) || pET->Owner->IsNeutral())
						continue;

					if (pOwnBld->DistanceFrom(pET) <= 2560)

					{

						hostileZone = true;

						closeHostileAttackers++;
					}
				}

				if (hostileZone)

				{

					const bool canUndeployToMCV = (GameModeOptionsClass::Instance.MCVRedeploy && pOwnBld->Type->UndeploysInto != nullptr);

					const int yardZone = MapClass::Instance.GetMovementZoneType(yc, MovementZone::Normal, false);

					const bool onEnemyIsland = (homeGroundZone >= 0 && yardZone >= 0 && yardZone != homeGroundZone);

					bool hasAmphibTransportOrNavalYard = (pHouse->NumShipyards > 0);

					if (onEnemyIsland && !hasAmphibTransportOrNavalYard)

					{

						for (auto const pT : UnitClass::Array)

						{

							if (pT && pT->Owner == pHouse && !pT->InLimbo && pT->IsAlive && pT->Type && IsAmphibiousOrAirTransport(pT->Type))

							{

								hasAmphibTransportOrNavalYard = true;

								break;
							}
						}
					}

					const bool impossibleToExtractAlive = (!canUndeployToMCV && (hpPct < 0.65 || closeHostileAttackers >= 2)) || (hpPct < 0.32 && closeHostileAttackers >= 2) ||

														(onEnemyIsland && !hasAmphibTransportOrNavalYard && (hpPct < 0.60 || closeHostileAttackers >= 1));

					if (impossibleToExtractAlive)

					{

						if (pOwnBld->CurrentMission != Mission::Selling)
							pOwnBld->QueueMission(Mission::Selling, true);
					}

					else if (canUndeployToMCV)

					{

						if (pOwnBld->CurrentMission != Mission::Unload)
							pOwnBld->QueueMission(Mission::Unload, true);
					}

					else if (pOwnBld->Type->ClickRepairable && hpPct < 0.90 && currentCredits > 600 && !pOwnBld->IsBeingRepaired)

					{

						pOwnBld->SetRepairState(1);
					}
				}
			}

			else if (GameModeOptionsClass::Instance.MCVRedeploy && pOwnBld->Type->ConstructionYard && pOwnBld->Type->UndeploysInto != nullptr && scores[81] > 90 &&
					(Unsorted::CurrentFrame % 360 < 15) &&
					((pHouse->NumConYards >= 2 && pOwnBld != pHouse->ConYards.GetItem(0)) ||
						(pHouse->NumConYards == 1 && pHouse->OwnedBuildings >= 6 && pHouse->NumWarFactories >= 1 && scores[81] > 140)))

			{

				// Slot 81 (Nomadic MCV Hop & Outpost/Turret Drop): Once an expansion

				// ConYard (or a fully-developed main ConYard) has already dropped >= 2

				// structures around itself (Refineries, BaseNormal Outposts, Factories,

				// or Base Defenses), pack it back into a mobile MCV so it can travel to

				// the next Ore field or forward enemy siege position while leaving its

				// dropped structures behind!

				const CellStruct yc = pOwnBld->GetMapCoords();

				int structuresDroppedHere = 0;

				for (auto const pNearB : pHouse->Buildings)

				{

					if (!pNearB || pNearB == pOwnBld || pNearB->InLimbo || !pNearB->IsAlive || !pNearB->Type)
						continue;

					const CellStruct nc = pNearB->GetMapCoords();

					if (std::max(std::abs(yc.X - nc.X), std::abs(yc.Y - nc.Y)) <= 10 &&

						(pNearB->Type->Refinery || pNearB->Type->BaseNormal || pNearB->Type->Factory != AbstractType::None || pNearB->Type->IsBaseDefense))

					{

						structuresDroppedHere++;
					}
				}

				if (structuresDroppedHere >= 2 && pOwnBld->CurrentMission != Mission::Unload)
					pOwnBld->QueueMission(Mission::Unload, true);
			}

			else if (hpPct < sellDodgeThresh && !pOwnBld->Type->ConstructionYard && pOwnBld->Type->Cost >= 500 && pOwnBld->CurrentMission != Mission::Selling)

			{

				// Classic C&C Sell-Dodge right before destruction!

				pOwnBld->QueueMission(Mission::Selling, true);
			}

			else if (pOwnBld->Type->UndeploysInto != nullptr && !pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery && !pOwnBld->Type->BaseNormal &&

					(pOwnBld->Type->IsBaseDefense || pOwnBld->Type->GetWeapon(0) != nullptr) && pOwnBld->Target == nullptr && (Unsorted::CurrentFrame % 150 < 30))

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

				if (!enemyInRange && pOwnBld->CurrentMission != Mission::Unload)
					pOwnBld->QueueMission(Mission::Unload, true);
			}

			else if (pOwnBld->Type->ClickRepairable)

			{

				// Slot 59 (Smart Wrench Building Repair): Activate engine wrench repair

				// on damaged key structures when credits are healthy, and pause repair

				// if credits drop critically low!

				if (hpPct < 0.82 && hpPct >= sellDodgeThresh && scores[59] > 80 && currentCredits > 1200 && !pOwnBld->IsBeingRepaired)

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
			const bool isLimboResearchUpgrade = pSWExt && (!pSWExt->LimboDelivery_Types.empty() || !pSWExt->LimboKill_IDs.empty() || !pSWExt->Convert_Pairs.empty());

			if (!pSuper->Type->DisableableFromShell && isLimboResearchUpgrade && pSuper->Type->Type != SuperWeaponType::ChronoWarp && !IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
			{
				pSuper->Launch(homeCoords, false);
				pSuper->Reset();
				break;
			}
		}
	}

	// CABAL / Ares / Antares Subterranean Tunnel Network (ROBOTTUNNEL +

	// CABALTunnelSpecial -> ROBOTCTUNNEL -> ROBOTTUNNEL): 1) Creative Tunnel Exit

	// Placement via Tunnel Delivery SW on unprotected enemy flanks, behind

	// defenses, on islands, or near neutral Techs 2) Two-Way Subterranean Portal

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

				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || !IsTunnelNetworkBuilding(pOwnBld->Type))
					continue;

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

				if (!pOwnBld || pOwnBld == pHomeTunnel || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || !IsTunnelNetworkBuilding(pOwnBld->Type))
					continue;

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

						if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Immune)
							continue;

						const bool isNeutralTech = (pBld->Owner->IsNeutral() && pBld->Type->Capturable);

						const bool isEnemyBld = (pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner) && !pBld->Owner->IsNeutral() && !pBld->Owner->IsObserver() && !pBld->Owner->Defeated);

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

								const CellStruct tc{static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy)};

								auto const pCand = MapClass::Instance.TryGetCellAt(tc);

								if (!pCand || !IsCellVisibleToAI(pHouse, pCand))
									continue;

								if ((pCand->LandType != LandType::Clear && pCand->LandType != LandType::Road) || pCand->ContainsBridge() || pCand->GetBuilding() != nullptr ||

									pCand->FirstObject != nullptr)

								{

									continue;
								}

								// Count nearby enemy defensive turrets within 9 cells of this

								// flank candidate

								int nearbyEnemyTurrets = 0;

								for (auto const pOtherBld : BuildingClass::Array)

								{

									if (!pOtherBld || pOtherBld->InLimbo || !pOtherBld->IsAlive || !pOtherBld->Owner || !pOtherBld->Type)
										continue;

									if (pOtherBld->Owner == pHouse || pHouse->IsAlliedWith(pOtherBld->Owner) || pOtherBld->Owner->IsNeutral())
										continue;

									if (pOtherBld->Type->IsBaseDefense && pOtherBld->Type->GetWeapon(0) != nullptr)

									{

										const CellStruct oc = pOtherBld->GetMapCoords();

										if (std::abs(tc.X - oc.X) <= 9 && std::abs(tc.Y - oc.Y) <= 9)
											nearbyEnemyTurrets++;
									}
								}

								int candScore = 400 - nearbyEnemyTurrets * 260;

								if (nearbyEnemyTurrets == 0)
									candScore +=

										450; // Completely unprotected flank / rear blindspot!

								if (isNeutralTech)

								{

									candScore += 380 + (scores[36] > 60 ? 200 : 0);
								}

								else

								{

									if (pBld->Type->ConstructionYard || pBld->Type->SuperWeapon != -1)
										candScore += 420;

									else if (pBld->Type->Factory != AbstractType::None || pBld->Type->PowerBonus > 0 || pBld->Type->Refinery)
										candScore += 280;
								}

								const int candZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);

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

		if (ownTunnelCount >= 2 && pHomeTunnel && pForwardTunnel && pHomeTunnel != pForwardTunnel && pHomeTunnel->DistanceFrom(pForwardTunnel) >= 2560)

		{

			// Keep all non-forward Tunnel mouths in Guard so passengers only unload

			// at pForwardTunnel!

			for (auto const pOwnBld : pHouse->Buildings)

			{

				if (!pOwnBld || pOwnBld == pForwardTunnel || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || !IsTunnelNetworkBuilding(pOwnBld->Type))
					continue;

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

				if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsObserver() || pBld->Owner->Defeated)
					continue;

				const int dFwd = pBld->DistanceFrom(pForwardTunnel);

				if (dFwd <= bestCapDist && pBld->Type->Capturable &&

					(pBld->Owner->IsNeutral() || pBld->Type->ConstructionYard || pBld->Type->Factory != AbstractType::None || pBld->Type->Refinery || pBld->Type->SuperWeapon != -1 ||

						pBld->Type->TechLevel >= 5))

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

					if (!pInf || pInf->Owner != pHouse || pInf->InLimbo || !pInf->IsAlive || !pInf->Type || !pInf->Type->Engineer)
						continue;

					// If Engineer emerged from pForwardTunnel, immediately capture the

					// flank target!

					if (pInf->DistanceFrom(pForwardTunnel) <= 2560)

					{

						if (pInf->CurrentMission != Mission::Enter)

						{

							pInf->SetDestination(pFlankCaptureTarget, true);

							pInf->QueueMission(Mission::Enter, true);
						}
					}

					else if (pInf->DistanceFrom(pHomeTunnel) <= 7168 && pInf->CurrentMission != Mission::Enter)

					{

						pInf->SetDestination(pHomeTunnel, true);

						pInf->QueueMission(Mission::Enter, true);

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

						pI->SetTarget(pFlankAttackTarget);

						pI->QueueMission(Mission::Attack, true);
					}

					else

					{

						pI->SetDestination(pWaveTargetCell, true);

						pI->QueueMission(Mission::AttackMove, true);
					}
				}

				else if (pI->DistanceFrom(pHomeTunnel) <= 6144 && pI->DistanceFrom(pForwardTunnel) > pHomeTunnel->DistanceFrom(pForwardTunnel) - 1024)

				{

					pI->SetDestination(pHomeTunnel, true);

					pI->QueueMission(Mission::Enter, true);

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

					if (IsAmphibiousOrAirTransport(pV->Type) || static_cast<int>(pV->Type->TechnoTypeClass::Size) > maxAllowedSize)
						continue;

					if (pV->DistanceFrom(pForwardTunnel) <= 2304 && (pFlankAttackTarget || pWaveTargetCell))

					{

						if (pFlankAttackTarget)

						{

							pV->SetTarget(pFlankAttackTarget);

							pV->QueueMission(Mission::Attack, true);
						}

						else

						{

							pV->SetDestination(pWaveTargetCell, true);

							pV->QueueMission(Mission::AttackMove, true);
						}
					}

					else if (pV->DistanceFrom(pHomeTunnel) <= 6144 && pV->DistanceFrom(pForwardTunnel) > pHomeTunnel->DistanceFrom(pForwardTunnel) - 1024)

					{

						pV->SetDestination(pHomeTunnel, true);

						pV->QueueMission(Mission::Enter, true);

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

				if (_stricmp(pSuper->Type->ID, "DONT_USE_THIS_SW") == 0 || IsTunnelDeliverySuperWeapon(pSuper->Type, pHouse))
					continue;

				if (auto const pSWExt = SWTypeExt::Fetch(pSuper->Type))
				{
					if (!pSWExt->LimboDelivery_Types.empty() || !pSWExt->LimboKill_IDs.empty() || !pSWExt->Convert_Pairs.empty())
						continue;
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

						if (!pFriendlyBld || pFriendlyBld->InLimbo || !pFriendlyBld->IsAlive || !pFriendlyBld->Owner || !pFriendlyBld->Type || pFriendlyBld->IsIronCurtained())
							continue;

						if (!pHouse->IsAlliedWith(pFriendlyBld->Owner) || pFriendlyBld->Owner->IsNeutral())
							continue;

						const bool isCriticalBld = pFriendlyBld->Type->ConstructionYard || pFriendlyBld->Type->Factory == AbstractType::UnitType || pFriendlyBld->Type->SuperWeapon != -1;

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

							if (!pU || !pU->Owner || !pHouse->IsAlliedWith(pU->Owner) || pU->Owner->IsNeutral() || pU->InLimbo || !pU->IsAlive || !pU->Type || pU->IsIronCurtained())
								continue;

							if (pU->Type->Harvester || pU->Type->ResourceGatherer || pU->Type->Organic)
								continue;

							const bool isEngagingEnemy = (pU->Target != nullptr) || (pVisibleEnemyBld && pU->DistanceFrom(pVisibleEnemyBld) <= 4096);

							if (!isEngagingEnemy)
								continue;

							const CellStruct uCell = pU->GetMapCoords();

							int neighbors = 0;

							for (auto const pOtherU : UnitClass::Array)

							{

								if (!pOtherU || !pOtherU->Owner || !pHouse->IsAlliedWith(pOtherU->Owner) || pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Type)
									continue;

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

				//    1) Teleport Iron-Curtained tank squad or Expansion MCV to enemy

				// base / island 2) Teleport Enemy Player A's tanks into Enemy Player

				// B's base to cause mutual attrition in FFA 3) Drown Enemy Ground

				// Tanks/MCVs by teleporting them into Water 4) Destroy Enemy Naval

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

								const CellStruct tc{static_cast<short>(homeCoords.X + dx), static_cast<short>(homeCoords.Y + dy)};

								auto const pC = MapClass::Instance.TryGetCellAt(tc);

								if (!pC || !IsCellVisibleToAI(pHouse, pC) || pC->GetBuilding() != nullptr)
									continue;

								if (knownWaterCell == CellStruct::Empty && pC->LandType == LandType::Water && !pC->ContainsBridge())
									knownWaterCell = tc;

								if (knownDryLandCell == CellStruct::Empty && pC->LandType == LandType::Clear && pC->FirstObject == nullptr && std::abs(dx) >= 12)
									knownDryLandCell = tc;
							}
						}
					}

					// Mode 1: Teleport our own Iron-Curtained tank squad or distant

					// Expansion MCV directly to the target beachhead!

					for (auto const pU : UnitClass::Array)

					{

						if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
							continue;

						const bool isExpansionMCV = (pU->Type->DeploysInto && pU->Type->DeploysInto->ConstructionYard && pHouse->NumConYards >= 1);

						const bool isIronCurtainedTank = (pU->IsIronCurtained() && !pU->Type->Harvester && !pU->Type->Organic);

						if ((isIronCurtainedTank || isExpansionMCV) && pWaveTargetCell)

						{

							CellClass* pDropCell = FindSafeIslandLandingCell(pHouse, pWaveTargetCell->MapCoords, targetIslandZone, isExpansionMCV);

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

							if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Immune)
								continue;

							auto const pOwn = pBld->Owner;

							if (pOwn == pHouse || pHouse->IsAlliedWith(pOwn) || pOwn->IsNeutral() || pOwn->IsObserver() || pOwn->Defeated)
								continue;

							if (pOwn->ArrayIndex != pHouse->EnemyHouseIndex && pBld->GetCell() && IsCellVisibleToAI(pHouse, pBld->GetCell()))

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

							if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
								continue;

							if (pEnemyU->IsIronCurtained() || pEnemyU->Type->Teleporter || pEnemyU->Type->Organic)
								continue;

							auto const pECell = pEnemyU->GetCell();

							if (!pECell || !IsCellVisibleToAI(pHouse, pECell))
								continue;

							const bool isPureGroundUnit = !pEnemyU->Type->Naval && pEnemyU->Type->SpeedType != SpeedType::Hover && pEnemyU->Type->MovementZone != MovementZone::Amphibious;

							const bool isPureNavalShip = pEnemyU->Type->Naval && pEnemyU->Type->SpeedType != SpeedType::Hover && pEnemyU->Type->MovementZone != MovementZone::Amphibious;

							// Mode 2: Pit Enemy A's ground tanks directly inside Enemy B's

							// base to trigger mutual attrition!

							if (isPureGroundUnit && rivalEnemyBaseCell != CellStruct::Empty && pEnemyU->Owner->ArrayIndex != rivalEnemyHouseIdx)

							{

								chronoSrc = pEnemyU->GetMapCoords();

								chronoDst = rivalEnemyBaseCell;

								break;
							}

							// Mode 3 (Slot 55): Drown Enemy ground tanks/MCVs in water!

							const int drownMinCost = std::clamp(700 - static_cast<int>(scores[55]), 450, 950);

							if (isPureGroundUnit && knownWaterCell != CellStruct::Empty && pEnemyU->Type->Cost >= drownMinCost)

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

						if (!pEnemyInf || pEnemyInf->InLimbo || !pEnemyInf->IsAlive || !pEnemyInf->Owner || !pEnemyInf->Type)
							continue;

						if (pEnemyInf->Owner == pHouse || pHouse->IsAlliedWith(pEnemyInf->Owner) || pEnemyInf->Owner->IsNeutral())
							continue;

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

						if (pEnemyU->Owner == pHouse || pHouse->IsAlliedWith(pEnemyU->Owner) || pEnemyU->Owner->IsNeutral())
							continue;

						if (pEnemyU->GetCell() && IsCellVisibleToAI(pHouse, pEnemyU->GetCell()) && !pEnemyU->Type->ImmuneToPsionics)

						{

							const CellStruct uc = pEnemyU->GetMapCoords();

							int val = 0;

							for (auto const pOtherU : UnitClass::Array)

							{

								if (!pOtherU || pOtherU->InLimbo || !pOtherU->IsAlive || !pOtherU->Owner || !pOtherU->Type)
									continue;

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
					const CellStruct reconCell = pFallbackEnemyCell ? pFallbackEnemyCell->MapCoords
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

		int ownedHarvestersCount = 0;

		bool hasTechCenter = false;
		int unfilledBioReactorSlots = 0;
		int bioReactorPotentialPower = 0;

		for (auto const pOwnBld : pHouse->Buildings)

		{

			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type)
				continue;

			auto const pOT = pOwnBld->Type;

			if (pOT->InfantryAbsorb && (pOT->PowerBonus > 0 || pOT->ExtraPowerBonus > 0) && !IsTunnelNetworkBuilding(pOT) && pOwnBld->Passengers.NumPassengers < pOT->Passengers)
			{
				const int freeSlots = pOT->Passengers - pOwnBld->Passengers.NumPassengers;
				unfilledBioReactorSlots += freeSlots;
				bioReactorPotentialPower += freeSlots * std::max(50, pOT->ExtraPowerBonus > 0 ? pOT->ExtraPowerBonus : 100);
			}
			if (pOT->Refinery || pOT->ResourceGatherer || pOT->ResourceDestination)
				ownedRefineries++;

			else if (pOT->Factory == AbstractType::InfantryType)
				ownedBarracks++;

			else if (pOT->Factory == AbstractType::UnitType && !pOT->Naval)
				ownedWarFactories++;

			else if (pOT->Factory == AbstractType::UnitType && pOT->Naval)
				ownedShipyards++;

			else if (pOT->Factory == AbstractType::AircraftType || pOT->Helipad)
				ownedAirpads++;

			else if (pOT->IsBaseDefense && !IsTunnelNetworkBuilding(pOT))

			{

				const auto pWS = pOT->GetWeapon(0);

				const auto pW = pWS ? pWS->WeaponType : nullptr;

				const bool isPureAA = pW && pW->Projectile && pW->Projectile->AA && !pW->Projectile->AG;

				if (isPureAA)
					totalAADefenses++;

				else
					totalGroundDefenses++;
			}

			else if (pOT->SpySat || pOT->OrePurifier || (pOT->PowerBonus <= pOT->PowerDrain && !pOT->ConstructionYard && pOT->Storage == 0 && !pOT->UnitRepair))

			{

				ownedTechStructures++;

				if ((RulesClass::Instance && RulesClass::Instance->BuildTech.FindItemIndex(pOT) != -1) || (pOT->TechLevel >= 6 && pOT->Cost >= 1200))
					hasTechCenter = true;
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

		// Periodic Obsolete Refinery Recycling & Emergency Cash Recovery (from

		// AdvAI)

		if (Unsorted::CurrentFrame > pExt->LastObsoleteRefineryCheckFrame + 450 + houseStagger)

		{

			pExt->LastObsoleteRefineryCheckFrame = Unsorted::CurrentFrame;

			RecycleObsoleteRefineries(pHouse);

			RaiseEmergencyMoneyForRefinery(pHouse, ownedRefineries, ownedHarvestersCount, money);
		}

		// Periodic Dynamic Primary Factory Switching (from AdvAI)

		if (Unsorted::CurrentFrame > pExt->LastPrimaryFactoryCheckFrame + 60 + houseStagger)

		{

			pExt->LastPrimaryFactoryCheckFrame = Unsorted::CurrentFrame;

			CellStruct frontTarget = pExt->AIBrain_AttackTargetCell;

			if (frontTarget == CellStruct::Empty && pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)

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

		const int realPowerDrain = GetRealPowerDrainAndGridMetrics(pHouse, hasUnpoweredBuildings, poweredDefAndSWCount, maxSinglePlantBonus, hasAdvancedPowerPlants);

		const int realPowerSurplus = pHouse->PowerOutput - realPowerDrain;

		const int plantLossBuffer = std::min(500, std::max(150, (maxSinglePlantBonus * 3) / 4));

		const int requiredPowerCushion = (poweredDefAndSWCount > 0) ? std::max(plantLossBuffer, std::min(550, 220 + poweredDefAndSWCount * 45)) : std::max(150, plantLossBuffer);

		// Competitive MultipleFactory Arms Race:

		// Measure strongest enemy's factory counts and scale our factory limits by

		// both economy (ownedRefineries) AND enemy production capacity!

		int maxEnemyWarFactories = 0;

		int maxEnemyBarracks = 0;

		int maxEnemyShipyards = 0;

		int maxEnemyAirpads = 0;

		for (auto const pOther : HouseClass::Array)

		{

			if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver() || pOther->Defeated)
				continue;

			maxEnemyWarFactories = std::max(maxEnemyWarFactories, pOther->NumWarFactories);

			maxEnemyBarracks = std::max(maxEnemyBarracks, pOther->NumBarracks);

			maxEnemyShipyards = std::max(maxEnemyShipyards, pOther->NumShipyards);

			maxEnemyAirpads = std::max(maxEnemyAirpads, pOther->NumAirpads);
		}

		const int ecoWarFactoryCap = (ownedRefineries >= 6) ? (ownedRefineries / 2) : (1 + ownedRefineries / 3);

		const int maxWarFactories = (hasTechCenter || money >= 2400) ? std::clamp(std::max(ecoWarFactoryCap, maxEnemyWarFactories + 1), 2, 7) : std::clamp(ecoWarFactoryCap, 1, 3);

		const int ecoBarracksCap = (ownedRefineries >= 4) ? 2 : 1;

		const int maxBarracks = (hasTechCenter || money >= 2500) ? std::clamp(std::max(ecoBarracksCap, maxEnemyBarracks), 2, 4) : std::clamp(ecoBarracksCap, 1, 2);

		const int maxShipyards = (hasTechCenter && maxEnemyShipyards >= 2) ? std::clamp(maxEnemyShipyards + 1, 2, 4) : 2;

		const int maxAirpads = (hasTechCenter && maxEnemyAirpads >= 2) ? std::clamp(maxEnemyAirpads + 1, 2, 4) : 2;

		// Threat & Defense Moderation Analysis:

		// Prune stale attack records and check if our base was recently attacked

		PruneAttackedBuildingsLIFO(pHouse);

		const bool wasRecentlyAttacked = (pHouse->LATime > 0 && (Unsorted::CurrentFrame - pHouse->LATime) < 1350) || !GetMacroState(pHouse).AttackedBuildingsLIFO.empty();

		bool underInfantryAttack = false;

		bool underVehicleAttack = false;

		bool underAirAttack = false;

		for (const auto& rec : GetMacroState(pHouse).AttackedBuildingsLIFO)

		{

			if (rec.Threat == NeuralThreatCategory::Infantry)
				underInfantryAttack = true;

			else if (rec.Threat == NeuralThreatCategory::Vehicle)
				underVehicleAttack = true;

			else if (rec.Threat == NeuralThreatCategory::Air)
				underAirAttack = true;
		}

		const bool hasCoreEconomyAndProduction = (ownedRefineries >= 1 && ownedBarracks >= 1 && ownedWarFactories >= 1);

		const bool isBehindInFactories = hasCoreEconomyAndProduction && ((ownedWarFactories < maxEnemyWarFactories && ownedRefineries >= 2) || (ownedRefineries < 2));

		const bool hasPendingCoreTechOrEco = !hasTechCenter || isBehindInFactories || (ownedRefineries < 2);

		// Detect remote captured Tech Buildings (Oil Derricks, Tech Outposts, Hospitals, Airfields) & remote Expansion Refineries
		// that are far from our main base (> 16 cells) and currently lack a nearby point-defense turret within 10 cells!
		const CellStruct homeRefCenter = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
			? pHouse->ConYards.GetItem(0)->GetMapCoords()
			: (pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell);
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
			const bool isCapturedTechOrCash = (pOT->ProduceCashAmount > 0 || pOT->ProduceCashStartup > 0 || (pOT->Capturable && pOT->NeedsEngineer));
			const bool isRemoteFromHome = (homeRefCenter != CellStruct::Empty && std::max(std::abs(bc.X - homeRefCenter.X), std::abs(bc.Y - homeRefCenter.Y)) > 16);

			if ((isCapturedTechOrCash || isRemoteFromHome) && (pOT->BaseNormal || pOT->ConstructionYard || BuildingTypeExt::IsAIBaseNormal(pOT)))
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

			if (isCapturedTechOrCash || (isRemoteFromHome && (pOT->Refinery || pOT->ConstructionYard || pOT->Factory != AbstractType::None)))
			{
				bool hasNearbyGroundTurret = false;
				for (auto const pDefBld : pHouse->Buildings)
				{
					if (!pDefBld || pDefBld->InLimbo || !pDefBld->IsAlive || !pDefBld->Type || !pDefBld->Type->IsBaseDefense || IsTunnelNetworkBuilding(pDefBld->Type))
						continue;
					const auto pWS = pDefBld->Type->GetWeapon(0);
					const auto pW = pWS ? pWS->WeaponType : nullptr;
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

		const int baseGroundDefCap = 1 + ownedRefineries + ownedWarFactories + ownedTechStructures + undefendedRemoteAssetsCount + defDensityMod + (wasRecentlyAttacked ? 2 : 0);

		const int maxGroundDefenses = std::clamp(baseGroundDefCap, 2, (hasTechCenter && money > 4000) ? 14 : 11);

		const int maxAADefenses = std::clamp(1 + ownedWarFactories + ownedTechStructures + defDensityMod + (underAirAttack ? 2 : 0), 2, 6);

		// AdvAI & StarCraft Resource Expansion State Machine:
		// Measure minimum Chebyshev distance from our closest BaseNormal/ConYard/CapturedTech anchor to the nearest uncolonized Ore/Gem field (>= 10 cells from any owned Refinery).
		// - If minAnchorDistToUncolonizedOre <= 9: our BaseNormal chain, Captured Tech Building, or Expansion ConYard has reached the new Ore field! Trigger ShouldBuildRefinery (+1950)!
		// - If minAnchorDistToUncolonizedOre is 10..38: boost BaseNormal=yes stepping-stone structures (Power Plants / Barracks / Outposts) so the base actively crawls to reach the Ore!
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
					const CellStruct tc{static_cast<short>(scanOrigin.X + dx), static_cast<short>(scanOrigin.Y + dy)};
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
						if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
							continue;

						const CellStruct bc = pOwnBld->GetMapCoords();
						const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
						if (bz != oreZone)
							continue;

						const int dCheb = std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y));
						if ((pOwnBld->Type->Refinery || pOwnBld->Type->ResourceGatherer || pOwnBld->Type->ResourceDestination) && dCheb < 10)
						{
							alreadyColonizedByRef = true;
							break;
						}

						if (pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard || BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
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
		for (int yi = 1; yi < pHouse->ConYards.Count; ++yi)
		{
			auto const pCY = pHouse->ConYards.GetItem(yi);
			if (!pCY || pCY->InLimbo || !pCY->IsAlive)
				continue;
			const int cyz = MapClass::Instance.GetMovementZoneType(pCY->GetMapCoords(), MovementZone::Normal, false);
			if (cyz < 0)
				continue;
			bool hasRefInZone = false;
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
					continue;
				if (pOwnB->Type->Refinery || pOwnB->Type->ResourceGatherer || pOwnB->Type->ResourceDestination)
				{
					if (MapClass::Instance.GetMovementZoneType(pOwnB->GetMapCoords(), MovementZone::Normal, false) == cyz)
					{
						hasRefInZone = true;
						break;
					}
				}
			}
			if (!hasRefInZone)
			{
				conYardWithNoRefineryInZone = true;
				break;
			}
		}

		const bool reachedNewOreField = (conYardWithNoRefineryInZone || (nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre <= 9)) && ownedRefineries >= 1;
		const bool needsSteppingStoneCrawlToOre = (GetMacroState(pHouse).ConsecutivePlacementFailures < 2 &&
												nearestUncolonizedOreCell != CellStruct::Empty && minAnchorDistToUncolonizedOre > 9 && minAnchorDistToUncolonizedOre <= 38 &&
												hasCoreEconomyAndProduction && (features[19] < 32 || ownedRefineries < 3 || scores[34] > 100 || Unsorted::CurrentFrame > 2100));

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
		for (auto const pOwnB : pHouse->Buildings)
		{
			if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
				continue;
			if (pOwnB->Type->ProduceCashAmount > 0)
				ownedCashBuildings++;
			if (pOwnB->Type->ResourceGatherer)
				ownedSlaveMinerBuildings++;
		}
		const bool hasActiveBuildingIncome = (ownedCashBuildings >= 1) ||
			(((ownedRefineries >= 1 && ownedHarvestersCount >= 1) || ownedSlaveMinerBuildings >= 1) && (features[19] > 0 || pHouse->OwnedTiberium.GetTotalAmount() > 0));

		for (auto const pBld : BuildingTypeClass::Array)
		{
			if (!pBld || pBld->Wall || pBld->Gate || pBld->LaserFence || pBld->LaserFencePost || pBld->ToTile)
				continue;

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
			const bool isIncomeRecoveryBld = isRefineryOrSlaveMiner ||
				(pBld->PowerBonus > pBld->PowerDrain && !pBld->IsBaseDefense && (pHouse->HasLowPower() || hasUnpoweredBuildings)) ||
				(pBld->Factory == AbstractType::UnitType && !pBld->Naval && ownedWarFactories == 0 && ownedHarvestersCount == 0) ||
				(pBld->Factory == AbstractType::InfantryType && ownedBarracks == 0 && features[20] > 0) ||
				(pBld->Factory == AbstractType::UnitType && pBld->Naval && ownedShipyards == 0 && features[19] == 0 && features[21] > 0);

			// Human-like credit management:
			// - When active income is flowing (hasActiveBuildingIncome), never block or delay selecting buildings by upfront cost; start building immediately and pay step-by-step!
			// - When all income sources are lost (!hasActiveBuildingIncome), strictly ration remaining credits to recover an income source first!
			if (!hasActiveBuildingIncome && !isBldInProgress && !isIncomeRecoveryBld && pBld->GetActualCost(pHouse) > money)
				continue;

			const int owned = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pBld, pHouse);

			int score = 0;

			if (pBld->PowerBonus > pBld->PowerDrain && !pBld->IsBaseDefense)
			{
				// Hard anti-spam cap: never queue more Power Plants when surplus is
				// already well above our damaged-plant safety cushion (relaxed by +350 when stepping-stone crawling to a distant Ore patch)!
				const int maxAllowedSurplus = requiredPowerCushion + ((needsSteppingStoneCrawlToOre && pBld->BaseNormal && money >= 1400) ? 700 : 350);
				if (!pHouse->HasLowPower() && !hasUnpoweredBuildings && realPowerSurplus >= maxAllowedSurplus)
					continue;

				// Track best buildable power plant for pre-build power interception (only after verifying we are below maxAllowedSurplus and not placement-locked!)
				const int plantEfficiencyScore = pBld->PowerBonus * 3 - (pBld->GetActualCost(pHouse) / 5);
				if ((GetMacroState(pHouse).ConsecutivePlacementFailures < 2 || pHouse->HasLowPower() || hasUnpoweredBuildings) &&
					plantEfficiencyScore > bestPowerPlantScore && pBld->GetActualCost(pHouse) <= std::max(money + 1000, 1200))
				{
					bestPowerPlantScore = plantEfficiencyScore;
					bestPowerPlantIdx = pBld->ArrayIndex;
				}

				score = scores[0] + std::min(650, pBld->PowerBonus * 2);

				// Avoid spamming new Bio-Reactors from the ConYard when existing owned Bio-Reactors still have free passenger slots
				// that our Barracks can fill with cheap infantry or mind-controlled enemies for +100W each!
				if (pBld->InfantryAbsorb && unfilledBioReactorSlots >= 2 && ownedBarracks >= 1 &&
					!pHouse->HasLowPower() && !hasUnpoweredBuildings && realPowerSurplus >= 40 &&
					(realPowerSurplus + bioReactorPotentialPower >= requiredPowerCushion) && !needsSteppingStoneCrawlToOre)
				{
					score -= 1100;
				}

				// Prefer Advanced Reactors (Nuclear/Bio/Tesla >= 300 PowerBonus) over
				// basic power plants once unlocked and affordable, EXCEPT when building a cheap 2x2 BaseNormal stepping stone to reach Ore!
				if (pBld->PowerBonus >= 300 && money >= pBld->GetActualCost(pHouse) && !needsSteppingStoneCrawlToOre)
					score += 350;
				else if (hasAdvancedPowerPlants && pBld->PowerBonus < 250 && money >= 1200 && !needsSteppingStoneCrawlToOre)
					score -= 300;

				if (pHouse->HasLowPower() || hasUnpoweredBuildings || realPowerSurplus < 60)
				{
					score += 1400;
				}
				else if (realPowerSurplus < requiredPowerCushion)
				{
					score += 450 + ((needsSteppingStoneCrawlToOre && pBld->BaseNormal) ? 450 : 0);
				}
				else if (needsSteppingStoneCrawlToOre && pBld->BaseNormal && pBld->GetFoundationWidth() <= 3 && money >= 1400)
				{
					// AdvAI Stepping-Stone BaseNormal Crawler: use cheap Power Plants to step the legal Adjacent=2 chain out of spawn toward the uncolonized Ore field!
					score += 680;
				}
				else
				{
					score -= 1400;
				}
			}

			else if (isRefineryOrSlaveMiner)

			{

				const int maxRefineries = (reachedNewOreField || pHouse->NumConYards >= 2 || (hasTechCenter && money < 3000)) ? 6 : 4;

				if (owned >= maxRefineries)
					continue;

				score = scores[1] - owned * 480;

				if (owned == 0)
					score += 1200;
				else if (reachedNewOreField)
					score += 1950; // AdvAI ShouldBuildRefinery: our BaseNormal chain or Expansion MCV reached a new Ore field! Claim it immediately!
				else if (owned == 1 && ownedWarFactories >= 1)
					score += 450;
			}

			else if (pBld->Factory == AbstractType::InfantryType)

			{

				const int effectiveMaxBarracks = (needsSteppingStoneCrawlToOre && pBld->BaseNormal) ? std::max(maxBarracks, 3) : maxBarracks;
				if (owned >= effectiveMaxBarracks)
					continue;

				// Slot 82 (Cross-Faction Tech Tree): If we already own our native
				// Barracks (ownedBarracks >= 1) and a captured foreign ConYard unlocks
				// a brand-new Foreign Barracks (owned == 0), boost it so we unlock
				// foreign infantry and foreign tech prerequisites!
				if (owned == 0 && ownedBarracks >= 1 && ownedBarracks < 5 && scores[82] > 60)
				{
					score = std::max(static_cast<int>(scores[2]), 350) + 750 + std::clamp(static_cast<int>(scores[82]), 0, 500);
				}
				else
				{
					score = scores[2] - owned * 820;

					if (ownedBarracks < maxEnemyBarracks && hasTechCenter && money > 1500)
						score += 420;

					if (needsSteppingStoneCrawlToOre && pBld->BaseNormal && money >= 1300)
						score += 780; // Forward Barracks stepping-stone toward uncolonized Ore / Frontline!
				}
			}

			else if (pBld->Factory == AbstractType::UnitType && !pBld->Naval)

			{

				if (owned >= maxWarFactories)
					continue;

				// Slot 82 (Cross-Faction Tech Tree): If we already own our native War

				// Factory (ownedWarFactories >= 1) and a captured foreign ConYard

				// unlocks a brand-new Foreign War Factory (owned == 0), build it to

				// unlock foreign tanks, Radar, and Battle Lab!

				if (owned == 0 && ownedWarFactories >= 1 && ownedWarFactories < 5 && scores[82] > 60)

				{

					score = std::max(static_cast<int>(scores[3]), 400) + 850 + std::clamp(static_cast<int>(scores[82]), 0, 550);
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

				const int navalAdj = std::max(std::max(1, pBld->Adjacent), RulesClass::Instance ? RulesClass::Instance->AINavalYardAdjacency : 36);
				const CellStruct baseOrigin = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0))
					? pHouse->ConYards.GetItem(0)->GetMapCoords()
					: (pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell);
				bool hasPlaceableWaterInAdjacency = false;
				if (baseOrigin != CellStruct::Empty)
				{
					for (int dy = -navalAdj; dy <= navalAdj && !hasPlaceableWaterInAdjacency; dy += 2)
					{
						for (int dx = -navalAdj; dx <= navalAdj; dx += 2)
						{
							CellStruct waterCand{static_cast<short>(baseOrigin.X + dx), static_cast<short>(baseOrigin.Y + dy)};
							if (MapClass::Instance.CoordinatesLegal(waterCand) && pBld->CanPlaceHere(&waterCand, pHouse))
							{
								hasPlaceableWaterInAdjacency = true;
								break;
							}
						}
					}
				}

				if (!hasPlaceableWaterInAdjacency)
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

					score = std::max(static_cast<int>(scores[8]), 250) + 550 + std::clamp(static_cast<int>(scores[82]) / 2, 0, 350);
				}

				else

				{

					score = scores[8] - owned * 750;

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

				const int tacticalTunnelBias = std::max({static_cast<int>(scores[28]), static_cast<int>(scores[36]), static_cast<int>(scores[61]), static_cast<int>(scores[83])});

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

				// Never build turrets before establishing core Economy + Barracks + War

				// Factory

				if (!hasCoreEconomyAndProduction && pHouse->ConYards.Count > 0)
					continue;

				const auto pWpnStruct = pBld->GetWeapon(0);

				const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;

				const bool isAA = pWpn && pWpn->Projectile && pWpn->Projectile->AA;

				const bool isPureAA = isAA && !pWpn->Projectile->AG;

				if (isPureAA && totalAADefenses >= maxAADefenses)
					continue;

				if (!isPureAA && totalGroundDefenses >= maxGroundDefenses)
					continue;

				// Do not queue powered defenses when power cushion is already tight

				if ((pBld->Powered || pBld->PowerDrain >= 50) && (pHouse->HasLowPower() || realPowerSurplus - pBld->PowerDrain < 75))
					continue;

				// Scale defense score by Cost and TechLevel so once Radar/Battle Lab

				// are online, advanced Tier-2/3 defenses (Prism Tower, Tesla Coil,

				// Psychic Tower, SAM, Patriot) strongly outscore cheap $500 Tier-1

				// Pillboxes/Sentry Guns!

				const int tierQualityBonus = std::min(850, (pBld->Cost / 2) + std::max(0, pBld->TechLevel) * 140);

				score = (isPureAA ? scores[6] : scores[5]) + tierQualityBonus;

				if (isPureAA)
					score -= totalAADefenses * 420 + owned * 500;

				else
					score -= totalGroundDefenses * 480 + owned * 600;

				// Human-like Defense Moderation (Opportunity Cost of ConYard Queue &

				// Credits): Allow 1-2 initial defenses near the ConYard/Factory; beyond

				// that, if we are NOT under attack and still need core Tech (Radar,

				// Battle Lab) or Factories/Refineries, prioritize macro/tech unless

				// floating > 4000 credits!

				const bool needsOutpostPointDefense = (!isPureAA && undefendedRemoteAssetsCount > 0);
				const bool hasInitialCoreDefenses = isPureAA ? (totalAADefenses >= 1) : (totalGroundDefenses >= 2);

				if (needsOutpostPointDefense)
					score += 680; // Proactively guard captured Oil Derricks, Tech Outposts, and remote Refineries against enemy Engineers & raids!
				else if (hasInitialCoreDefenses && !wasRecentlyAttacked && hasPendingCoreTechOrEco && money < 4000)
					score -= 1150;

				// Reactive Threat Countering (from AttackedBuildingsLIFO):

				// When under attack, boost the defense type that counters the attacker!

				if (wasRecentlyAttacked)

				{

					if (isPureAA && underAirAttack)
						score += 550;

					else if (!isPureAA && underVehicleAttack && pBld->Cost >= 800)
						score += 480;

					else if (!isPureAA && underInfantryAttack && pBld->Cost <= 1000)
						score += 420;

					else
						score += 220;
				}

				if (features[72] > 0 && pBld->Sensors)
					score += 350; // Anti-Stealth / Submarine detection turret boost!
			}

			else

			{

				// Tech & Support buildings (Radar, Battle Lab, Ore Purifier, Industrial

				// Plant, Cloning Vats, Grinder, Gap/Cloak Generator):

				const int supportRadius = GetSupportRadius(pBld);

				const int maxAllowed = (supportRadius > 0 && hasTechCenter && money > 3000) ? 2 : 1;

				if (owned >= maxAllowed)
					continue;

				score = scores[4] + 550 + std::min(600, pBld->Cost / 3) - owned * 700;

				if (pHouse->NumConYards >= 2 && scores[82] > 60)
					score += 250 + std::clamp(static_cast<int>(scores[82]) / 2, 0, 300);
			}

			if (GetMacroState(pHouse).ConsecutivePlacementFailures == 1 && pBld->BaseNormal && pBld->GetFoundationWidth() <= 2 && pBld->GetFoundationHeight(false) <= 2)
				score += 900; // Try a compact 2x2 BaseNormal anchor once to expand legal Adjacent= perimeter out of tight terrain!

			if (!hasActiveBuildingIncome)
			{
				if (isIncomeRecoveryBld)
					score += 4500; // Zero-Income Emergency: prioritize restoring an income source above all else!
				else
					score -= 3500;
			}

			if (isBldInProgress && (!pHouse->HasLowPower() || pBld->PowerBonus > pBld->PowerDrain))
				score += 850; // Commit to finishing partially constructed buildings in the ConYard!

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

					const int minSafeAfterBuild = (poweredDefAndSWCount > 0 || (pChosen->IsBaseDefense && pChosen->Powered)) ? std::max(120, requiredPowerCushion / 2) : 50;

					if (pHouse->HasLowPower() || hasUnpoweredBuildings || projectedSurplus < minSafeAfterBuild)
						bestBldIdx = bestPowerPlantIdx;
				}
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

	for (auto const pOwnedU : UnitClass::Array)
	{
		if (pOwnedU && pOwnedU->Owner == pHouse && !pOwnedU->InLimbo && pOwnedU->IsAlive && pOwnedU->Type)
		{
			auto const pOwnedType = pOwnedU->Type;
			if (pOwnedType->DeploysInto && pOwnedType->DeploysInto->ConstructionYard)
			{
				ownsUndeployedMCV = true;
			}
			else if (pOwnedType->DeploysInto && !pOwnedType->Harvester && !pOwnedType->ResourceGatherer &&
					(!pOwnedType->DeploysInto->IsBaseDefense || !pOwnedType->GetWeapon(0) || !pOwnedType->GetWeapon(0)->WeaponType))
			{
				ownsUndeployedKeyStructure = true;
			}
			else if (IsAmphibiousOrAirTransport(pOwnedType) || (TechnoTypeExt::Fetch(pOwnedType) && TechnoTypeExt::Fetch(pOwnedType)->Carryall))
			{
				ownedAmphibTransports++;
				if (static_cast<int>(pOwnedType->SizeLimit) >= 6 || (TechnoTypeExt::Fetch(pOwnedType) && TechnoTypeExt::Fetch(pOwnedType)->Carryall))
					ownedHeavyIslandTransports++;
			}
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

		if (pEcoType->Harvester || pEcoType->ResourceGatherer || (pEcoType->DeploysInto && (pEcoType->DeploysInto->Refinery || pEcoType->DeploysInto->ResourceGatherer)))
			ownedEconomyUnits++;
	}

	const int ecoBiasMod = std::clamp(static_cast<int>(scores[39]) / 220, -1, 2);

	const int economyUnitCap = (trueRefineries > 0) ? std::clamp(2 * trueRefineries + 1 + ecoBiasMod, 2, 9) : 4;

	// Emergency ConYard Reconstruction: If we lost all ConYards and have no MCV

	// on the map, check if our War Factory + Service Depot/Grinder/Tech can

	// build a replacement MCV and whether our economy/situation allows saving up

	// the $3000 for it instead of draining credits on cheap units!

	const bool canSafelySaveForReplacementMCV =
		(pHouse->NumConYards == 0 && !ownsUndeployedMCV) && ((ownedEconomyUnits > 0 && trueRefineries > 0 && features[65] < 45) || money >= 1500 || pHouse->OwnedUnits >= 3);
	const bool desperateIslandOrOreExpansion = (pHouse->NumConYards >= 1 && pHouse->NumConYards < 3 && !ownsUndeployedMCV && scores[30] >= 500 &&
		(features[19] < 24 || pHouse->Buildings.Count >= 20 || (features[21] > 0 && pHouse->CountResourceDestinations < 3)));
	const bool canSafelySaveForExpansionMCV =
		(pHouse->NumConYards >= 1 && pHouse->NumConYards < 3 && !ownsUndeployedMCV && scores[30] >= 500 &&
		(features[21] == 0 || ownedHeavyIslandTransports >= 1 || pHouse->NumShipyards == 0) && (money >= 1400 || desperateIslandOrOreExpansion));
	const bool canSafelySaveForMCV = canSafelySaveForReplacementMCV || canSafelySaveForExpansionMCV;
	const bool savingForIslandTransport = (features[21] > 0 && pHouse->NumShipyards >= 1 && ownedHeavyIslandTransports == 0 &&
		(scores[30] >= 500 || features[25] > 0) && money < 1200 && pHouse->OwnedUnits >= 2);

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
	const bool hasActiveUnitIncome = (ownedCashProduces >= 1) ||
		((trueRefineries >= 1 && ownedEconomyUnits >= 1) && (features[19] > 0 || pHouse->OwnedTiberium.GetTotalAmount() > 0));

	for (auto const pUnit : UnitTypeClass::Array)
	{
		if (!pUnit || !CanBuildUnderHumanRules(pHouse, pUnit))
			continue;

		const bool isUnitInProgress = (pUnit->ArrayIndex == inProgressUnitIdx || pUnit->ArrayIndex == inProgressNavalIdx);
		const bool isMCV = (pUnit->DeploysInto && pUnit->DeploysInto->ConstructionYard);
		if (isMCV)
		{
			if (ownsUndeployedMCV || (pHouse->NumConYards >= 1 && scores[30] < 400) || pHouse->NumConYards >= 3)
				continue;
			if (canSafelySaveForMCV)
			{
				canRebuildMCVNow = true;
				rebuildMCVCost = pUnit->GetActualCost(pHouse);
			}
		}

		const auto pWpnStruct = pUnit->GetWeapon(0);
		const auto pWpn = pWpnStruct ? pWpnStruct->WeaponType : nullptr;

		const bool isKeyStructureDeployer =
			(pUnit->DeploysInto != nullptr && !isMCV && !pUnit->Harvester && !pUnit->ResourceGatherer && (!pUnit->DeploysInto->IsBaseDefense || pWpn == nullptr));

		if (isKeyStructureDeployer && ownsUndeployedKeyStructure)
			continue;

		const bool isAmphibTransport = IsAmphibiousOrAirTransport(pUnit);
		const bool isDeployableRefineryOrMiner = (pUnit->DeploysInto && (pUnit->DeploysInto->Refinery || pUnit->DeploysInto->ResourceGatherer || pUnit->DeploysInto->ResourceDestination));
		const bool allowExpansionRefineryDeployer = isDeployableRefineryOrMiner && (features[21] > 0 || scores[30] >= 500) &&
			(trueRefineries + ownedEconomyUnits < 7) && !ownsUndeployedKeyStructure &&
			(features[21] == 0 || ownedHeavyIslandTransports >= 1 || pUnit->MovementZone == MovementZone::Amphibious);

		const bool isEconomyUnit = pUnit->Harvester || pUnit->ResourceGatherer || isDeployableRefineryOrMiner;
		if (isEconomyUnit && ownedEconomyUnits >= economyUnitCap && !allowExpansionRefineryDeployer)
			continue;

		const bool isIncomeRecoveryUnit = isEconomyUnit || (isMCV && canSafelySaveForMCV) || (isAmphibTransport && ownedAmphibTransports == 0 && (features[21] > 0 || features[19] == 0));
		// Only restrict unit choices by remaining cash when all income sources are lost (!hasActiveUnitIncome)!
		if (!hasActiveUnitIncome && !isUnitInProgress && !isIncomeRecoveryUnit && pUnit->GetActualCost(pHouse) > money)
			continue;

		int score = (pUnit->Cost / 4) + (pUnit->Strength / 3);
		score -= std::min(900, pHouse->CountOwnedNow(pUnit) * 45);
		if (!hasActiveUnitIncome)
		{
			if (isIncomeRecoveryUnit)
				score += 4500; // Zero-Income Emergency: focus War Factory / Shipyard on restoring Harvester/Miner/MCV/Transport!
			else
				score -= 3500;
		}
		if (isUnitInProgress)
			score += 850; // Commit to finishing partially constructed vehicles/ships in the War Factory / Shipyard!

		if (pUnit->Naval)
		{
			if (isAmphibTransport)
			{
				const bool isHeavyMcvTrans = (static_cast<int>(pUnit->SizeLimit) >= 6);
				const int maxTransports = (features[21] > 0 && scores[75] > 120) ? 3 : 2;
				if ((isHeavyMcvTrans && ownedHeavyIslandTransports >= 2) || (!isHeavyMcvTrans && ownedAmphibTransports >= maxTransports))
					score -= 1200;
				else if (isHeavyMcvTrans && ownedHeavyIslandTransports == 0 && (features[21] > 0 || scores[30] >= 500))
					score += scores[28] + 1250;
				else
					score += scores[28] + ((features[21] > 0 && ownedAmphibTransports == 0) ? 900 : 250);
			}
			else
			{
				score += scores[29];
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
				score += (pHouse->NumConYards == 0) ? 12000 : (scores[30] + (canSafelySaveForExpansionMCV ? 1400 : 0));

			else if (isKeyStructureDeployer)

			{

				// Universal Deployable Key Structure Scoring across all mods (Mobile

				// Outposts/Explorers, Mobile Refineries/Factories, Power/Tech/Turret

				// Cores, Nanocores):

				auto const pTargetBld = pUnit->DeploysInto;

				const int ownedTargetBld = BuildingTypeExt::CountOwnedNowWithDeployOrUpgrade(pTargetBld, pHouse);

				const bool isDedicatedOutpost = (pTargetBld->BaseNormal && !pTargetBld->Refinery && pTargetBld->Factory == AbstractType::None && pTargetBld->PowerBonus <= pTargetBld->PowerDrain);

				if (isDedicatedOutpost)

				{

					// Mobile Outpost / Explorer / Expansion Hub (Tiberian Sun / Firestorm

					// / Mental Omega / YR mods):

					if (scores[30] < 300 || ownedTargetBld >= 2)
						score -= 2000;

					else
						score += scores[30] - (ownedTargetBld * 600);
				}

				else if (pTargetBld->PowerBonus > pTargetBld->PowerDrain && !pTargetBld->IsBaseDefense)
					score += scores[0];

				else if (pTargetBld->Refinery)
					score += scores[1] + ((scores[30] > 700) ? (scores[30] / 2) : 0); // Use Mobile Refinery / Refinery

				// Core for Outpost Expansion!

				else if (pTargetBld->Factory == AbstractType::InfantryType)
					score += scores[2];

				else if (pTargetBld->Factory == AbstractType::UnitType && !pTargetBld->Naval)
					score += scores[3] + ((scores[30] > 800) ? (scores[30] / 3) : 0); // Forward Mobile War Factory / Core!

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
				score += scores[31] + ((ownedEconomyUnits < 2) ? 1200 : (allowExpansionRefineryDeployer ? scores[30] : 0));

			else if (pUnit->Passengers > 0 || (TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall))
			{
				const bool isAirOrCarryallVehicle = isAmphibTransport || (TechnoTypeExt::Fetch(pUnit) && TechnoTypeExt::Fetch(pUnit)->Carryall);
				const int maxTransports = (features[21] > 0 && scores[75] > 120) ? 3 : 2;
				if (isAirOrCarryallVehicle && ownedAmphibTransports >= maxTransports)
					score -= 1200;
				else if (isAirOrCarryallVehicle)
					score += scores[28] + ((features[21] > 0 && ownedAmphibTransports == 0) ? 900 : 350);
				else
					score += scores[28] + (features[21] > 0 ? -500 : 0);
			}
			else if (false && pUnit->Passengers > 0)
				score += scores[28] + (isAmphibTransport ? 350 : (features[21] > 0 ? -500 : 0));

			else if (isAA)
				score += scores[26];

			else if (isArtillery)
				score += scores[27];

			else if (pUnit->Strength < 300)
				score += scores[25];

			else
				score += scores[24];

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
		if (pMapBld && !pMapBld->InLimbo && pMapBld->IsAlive && pMapBld->Type && pMapBld->Owner && pMapBld->Type->Capturable && pMapBld->Owner->IsNeutral())
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
		if (pOwnBld && !pOwnBld->InLimbo && pOwnBld->IsAlive && pOwnBld->Type && pOwnBld->Type->InfantryAbsorb &&
			(pOwnBld->Type->PowerBonus > 0 || pOwnBld->Type->ExtraPowerBonus > 0) && !IsTunnelNetworkBuilding(pOwnBld->Type) &&
			pOwnBld->Passengers.NumPassengers < pOwnBld->Type->Passengers)
		{
			unfilledBioReactorSlotsForInf += (pOwnBld->Type->Passengers - pOwnBld->Passengers.NumPassengers);
		}
	}
	const bool needsCheapInfForBioReactor = (unfilledBioReactorSlotsForInf > 0 && (pHouse->HasLowPower() || (pHouse->PowerOutput - pHouse->PowerDrain) < 350));

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
			const int cashDerrickBonus = (neutralCashDerrickCount > ownedEngineers) ? 880 : (moreTechThanEngineers ? 680 : 320);
			const int techBonus = hasCapturableTechOnMap ? cashDerrickBonus : 0;

			if (pHouse->OwnedInfantry < 3 && !hasCapturableTechOnMap && scores[36] <= 80)
				score -= 300;
			else
				score += scores[18] + techBonus;
		}

		else if (pInf->Infiltrate)

		{

			if (ownedInfiltrators >= 1)
				continue;

			score += (pHouse->OwnedInfantry < 6) ? -400 : scores[19];
		}

		else if (isAA || pInf->C4)
			score += scores[20];

		else if (pInf->Cost >= 250)
			score += scores[17];

		else
			score += scores[16];

		if (needsCheapInfForBioReactor && !pInf->Engineer && !pInf->Infiltrate && !pInf->Doggie && pInf->Cost <= 220)
			score += 520; // Train cheap basic infantry (Initiates/Conscripts) to fill unfilled Bio-Reactor slots for +100W ExtraPowerBonus!

		if (features[77] > 0 && !pInf->Engineer && !pInf->Infiltrate && (pInf->ImmuneToPsionics || (pWpn && pWpn->Range >= (6 * 256))))
			score += 380; // Counter enemy MindControl with Snipers/Virus/Cyborgs/Dogs!

		if (features[72] > 0 && (pInf->Sensors || pInf->DetectDisguise || pInf->Doggie))
			score += 420;

		if (scores[82] > 60 && ownHouseBit != 0u && !pInf->InOwners(ownHouseBit) && !pInf->Engineer)
			score += 220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200);

		if (score > bestInfScore)

		{

			bestInfScore = score;

			bestInfIdx = pInf->ArrayIndex;
		}
	}

	// Only pause secondary queues to ration finite credits when all income sources are lost (!hasActiveUnitIncome)
	if (!hasActiveUnitIncome && (((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || savingForIslandTransport || ownedEconomyUnits == 0 || trueRefineries == 0) && features[65] < 35) || pHouse->OwnedInfantry >= 45)
	{
		if (bestInfIdx < 0 || !InfantryTypeClass::Array.GetItem(bestInfIdx)->Engineer || ownedEngineers >= std::max(1, neutralCashDerrickCount))
			bestInfIdx = -1; // Zero-Income Emergency: ration remaining finite credits to recover Refinery/Harvester/MCV or capture an Oil Derrick!
	}

	if (!hasActiveUnitIncome && ((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || savingForIslandTransport) &&
		!pHouse->HasLowPower() && pHouse->CountResourceDestinations >= 1 && pHouse->NumWarFactories >= 1 && pHouse->NumBarracks >= 1 &&
		(features[21] == 0 || pHouse->NumShipyards >= 1) && pHouse->UnitRepairStations.Count >= 1)
	{
		pExt->AIBrain_NextBuildingTarget = -1;
	}
	if (!hasActiveUnitIncome && savingForIslandTransport && bestUnitIdx >= 0)
	{
		if (auto const pChosenU = UnitTypeClass::Array.GetItem(bestUnitIdx))
		{
			if (!pChosenU->Harvester && !pChosenU->ResourceGatherer && !(pChosenU->DeploysInto && pChosenU->DeploysInto->ConstructionYard))
				pExt->AIBrain_NextUnitTarget = -1;
		}
	}

	pExt->AIBrain_NextInfantryTarget = bestInfIdx;

	// Aircraft Target Selection

	int bestAirIdx = -1;

	int bestAirScore = -32000;

	if (pHouse->NumAirpads > 0)
	{
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

			const int crossFactionAirBonus = (scores[82] > 60 && ownHouseBit != 0u && !pAir->InOwners(ownHouseBit)) ? (220 + std::clamp(static_cast<int>(scores[82]) / 3, 0, 200)) : 0;

			int airTransBonus = 0;
			if (pAir->Carryall || pAir->Passengers >= 3)
			{
				const int ownedAirTrans = pHouse->CountOwnedNow(pAir);
				if (ownedAmphibTransports + ownedAirTrans >= 2)
					airTransBonus = -1200;
				else
					airTransBonus = scores[28] + ((features[21] > 0 && ownedAmphibTransports == 0 && ownedAirTrans == 0) ? 900 : 350);
			}
			const int score = (pAir->Cost / 3) + (pAir->Strength / 2) + slotScore + crossFactionAirBonus + airTransBonus + (isAirInProgress ? 850 : 0);

			if (score > bestAirScore)

			{

				bestAirScore = score;

				bestAirIdx = pAir->ArrayIndex;
			}
		}
	}

	if (!hasActiveUnitIncome && ((canRebuildMCVNow && canSafelySaveForMCV && money < rebuildMCVCost) || ownedEconomyUnits == 0 || trueRefineries == 0))
		bestAirIdx = -1; // Zero-Income Emergency: save remaining finite credits to restore economy/MCV first!

	pExt->AIBrain_NextAircraftTarget = bestAirIdx;

	if (Unsorted::CurrentFrame % 450 < evalDelay)

	{

		const int bldIdx = pExt->AIBrain_NextBuildingTarget;

		const char* bldName = (bldIdx >= 0 && bldIdx < BuildingTypeClass::Array.Count) ? BuildingTypeClass::Array.GetItem(bldIdx)->ID : "None";

		const char* unitName = (bestUnitIdx >= 0 && bestUnitIdx < UnitTypeClass::Array.Count) ? UnitTypeClass::Array.GetItem(bestUnitIdx)->ID : "None";

		const char* infName = (bestInfIdx >= 0 && bestInfIdx < InfantryTypeClass::Array.Count) ? InfantryTypeClass::Array.GetItem(bestInfIdx)->ID : "None";

		const char* navalName = (bestNavalIdx >= 0 && bestNavalIdx < UnitTypeClass::Array.Count) ? UnitTypeClass::Array.GetItem(bestNavalIdx)->ID : "None";
		const char* airName = (bestAirIdx >= 0 && bestAirIdx < AircraftTypeClass::Array.Count) ? AircraftTypeClass::Array.GetItem(bestAirIdx)->ID : "None";

		Debug::Log("[NeuralAI] Frame %d | House #%d (%s) | Expert=%d Arch=%d | Credits=%d | NextBld=%s | NextUnit=%s | NextNaval=%s | NextInf=%s | NextAir=%s\n", Unsorted::CurrentFrame, pHouse->ArrayIndex,
			(pHouse->Type && pHouse->Type->ID[0]) ? pHouse->Type->ID : "AI", pExt->AIBrain_ActiveExpert, pExt->AIBrain_Archetype, money, bldName, unitName, navalName, infName, airName);
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

		if (!pExisting || pExisting->InLimbo || !pExisting->IsAlive || !pExisting->Type || pExisting->Type->InvisibleInGame)
			continue;

		auto const pExType = pExisting->Type;

		const CellStruct exCell = pExisting->GetMapCoords();

		const int exW = pExType->GetFoundationWidth();

		const int exH = pExType->GetFoundationHeight(false);

		// Strictly prevent overlapping any existing building footprint on the map

		if ((candidateCell.X < exCell.X + exW) && (candidateCell.X + candW > exCell.X) && (candidateCell.Y < exCell.Y + exH) && (candidateCell.Y + candH > exCell.Y))
			return false;

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

		// Enforce a 1-cell street margin between non-defense structures when Adjacent >= 2 (from AdvAI).
		// Note: A 1-cell gap equals Chebyshev foundation distance 2, which fits inside standard Adjacent=2
		// while keeping a passable 1-cell road around all 4 sides of every Factory, Refinery, and ConYard!
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

	const bool isRefineryOrSlaveMiner = pBuildingType->Refinery || pBuildingType->ResourceGatherer || pBuildingType->ResourceDestination;

	if (pHouse->ConYards.Count > 0)
	{
		// If we own an expansion or nomadic ConYard, prioritize planting structures around the newest ConYard
		// (especially when it has < 2 nearby structures so a Nomadic MCV can drop its outpost and hop on!)
		const bool canBuildAtOutpostYard = isRefineryOrSlaveMiner || pBuildingType->IsBaseDefense || pBuildingType->Factory == AbstractType::UnitType ||
										pBuildingType->Factory == AbstractType::InfantryType || pBuildingType->PowerBonus > 0 || pBuildingType->BaseNormal;
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

					const CellStruct bc = pB->GetMapCoords();
					if (std::max(std::abs(cyc.X - bc.X), std::abs(cyc.Y - bc.Y)) <= 10)
						nearCount++;
				}

				if (nearCount < 2)
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

			if (pOwnBld->Type->Factory == AbstractType::UnitType || pOwnBld->Type->Refinery || pOwnBld->Type->BaseNormal || pOwnBld->Type->PowerBonus > 0)

			{

				center = pOwnBld->GetMapCoords();

				if (!pOwnBld->HasBeenAttacked)
					break;
			}
		}
	}

	// Support universal Mobile Outpost / Expansion Hub structures & newly colonized islands (BaseNormal=yes / Expansion ConYards)
	const CellStruct homeSpawnRef = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)) ? pHouse->ConYards.GetItem(0)->GetMapCoords() : center;
	const int homeSpawnZone = (homeSpawnRef != CellStruct::Empty) ? MapClass::Instance.GetMovementZoneType(homeSpawnRef, MovementZone::Normal, false) : -1;
	int targetExpansionZone = -1;
	for (auto const pOwnBld : pHouse->Buildings)
	{
		if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
			continue;
		if (!pOwnBld->Type->ConstructionYard && !pOwnBld->Type->BaseNormal && !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
			continue;
		const CellStruct bc = pOwnBld->GetMapCoords();
		const int bz = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false);
		if (bz >= 0 && homeSpawnZone >= 0 && bz != homeSpawnZone)
		{
			int bldsInIslandZone = 0;
			for (auto const pCheckB : pHouse->Buildings)
			{
				if (!pCheckB || pCheckB->InLimbo || !pCheckB->IsAlive || !pCheckB->Type || pCheckB->Type->Insignificant || pCheckB->Type->InvisibleInGame)
					continue;
				if (MapClass::Instance.GetMovementZoneType(pCheckB->GetMapCoords(), MovementZone::Normal, false) == bz)
					bldsInIslandZone++;
			}
			if (bldsInIslandZone < 14)
			{
				center = bc;
				targetExpansionZone = bz;
				break;
			}
		}
	}
	if (targetExpansionZone < 0 && (isRefineryOrSlaveMiner || pBuildingType->IsBaseDefense))
	{
		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (pOwnBld && !pOwnBld->InLimbo && pOwnBld->IsAlive && pOwnBld->Type && pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard && !pOwnBld->Type->Refinery)
				center = pOwnBld->GetMapCoords();
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

			if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral() && pAllyBld->Type->EligibileForAllyBuilding)

			{

				if (pHouse->ConYards.Count == 0 || (pBuildingType->IsBaseDefense && pAllyBld->HasBeenAttacked && pAllyBld->GetHealthPercentage() < 0.80))

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

	const int crawlBias = static_cast<int>(policy.OutputBias[34]) + ((pExt && pExt->AIBrain_ActiveExpert == 6) ? 220 : 0);

	const int turretCreepBias = static_cast<int>(policy.OutputBias[38]) + ((pExt && pExt->AIBrain_ActiveExpert == 7) ? 220 : 0);

	// Discover nearest uncolonized Ore/Tiberium patch across an 84-cell radius (for
	// Expert 6, Slot 34 & AdvAI Stepping-Stone Base-Crawling). Measure distance from our closest
	// BaseNormal/ConYard anchor and prefer uncolonized Ore patches >= 10 cells from any existing Refinery!
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

			const CellStruct tc{static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy)};
			auto const pCell = MapClass::Instance.TryGetCellAt(tc);
			if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
				continue;
			const int oreZone = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
			if (centerZone >= 0 && oreZone != centerZone)
				continue; // Never try to Adjacent=2 base-crawl across water to a different island!

			bool nearExistingRef = false;
			int minAnchorManhattan = dCenter;
			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
					continue;

				const CellStruct bc = pOwnBld->GetMapCoords();
				if (centerZone >= 0 && MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false) != centerZone)
					continue;

				if ((pOwnBld->Type->Refinery || pOwnBld->Type->ResourceGatherer || pOwnBld->Type->ResourceDestination) && std::max(std::abs(tc.X - bc.X), std::abs(tc.Y - bc.Y)) < 10)
					nearExistingRef = true;

				if (pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard || BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
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

			if (!pOther || pOther == pHouse || pHouse->IsAlliedWith(pOther) || pOther->IsNeutral() || pOther->IsObserver() || pOther->Defeated)
				continue;

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
				existingTunnelCells.push_back(pOwnBld->GetMapCoords());

			else if (homeFactoryCell == CellStruct::Empty && (pOwnBld->Type->Factory == AbstractType::InfantryType || pOwnBld->Type->Factory == AbstractType::UnitType))
				homeFactoryCell = pOwnBld->GetMapCoords();
		}
	}

	const bool isForwardTunnelMouth = (isTunnelBld && !existingTunnelCells.empty());

	const bool isNavalBld = pBuildingType->Naval || (pBuildingType->SpeedType == SpeedType::Float);

	const bool isFactoryOrBarracks = !isNavalBld && ((pBuildingType->Factory != AbstractType::None) || pBuildingType->WeaponsFactory || pBuildingType->UnitRepair);

	const int supportRadius = GetSupportRadius(pBuildingType);

	const bool isSupportRadiusBld = (supportRadius > 0) && !isCombatDefense && !isResourceBld && !isFactoryOrBarracks && !isNavalBld;

	const bool isPowerPlantBld = (pBuildingType->PowerBonus > pBuildingType->PowerDrain) && !isCombatDefense;
	// Stepping-Stone BaseNormal Expander (from AdvAI): After our 1st Power Plant, BaseNormal Power Plants (and Outposts)
	// step outward toward uncolonized Ore fields or the frontline instead of crowding into the rear corner behind the ConYard!
	const bool isSteppingStoneExpander = pBuildingType->BaseNormal && isPowerPlantBld && (pHouse->PowerOutput >= 150) && (nearestOreCell != CellStruct::Empty || crawlBias > 0);
	const bool isRearProtected = !isNavalBld && !isResourceBld && !isCombatDefense && !isFactoryOrBarracks && !isTunnelBld && !isSupportRadiusBld && !isSteppingStoneExpander;

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

			if (pOT == pBuildingType || (pOT->GapGenerator && pBuildingType->GapGenerator) || (pOT->CloakGenerator && pBuildingType->CloakGenerator) ||

				(pOT->SensorArray && pBuildingType->SensorArray))

			{

				existingSameSupportCells.push_back(bc);
			}

			if (pOT->ConstructionYard || pOT->Factory != AbstractType::None || pOT->SuperWeapon != -1 || pOT->Refinery || pOT->Radar)
				keyBaseCells.push_back(bc);
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

	// Gather Directional Attack Vector (from AttackedBuildingsLIFO) and

	// Undefended Key Assets / Vulnerable Power Plant Clusters (3+ plants grouped)

	CellStruct directionalAttackedBld = CellStruct::Empty;

	CellStruct directionalAttacker = CellStruct::Empty;

	CellStruct undefendedKeyAssetCell = CellStruct::Empty;

	int totalOwnedDefensesCount = 0;

	if (isCombatDefense)

	{

		if (!GetMacroState(pHouse).AttackedBuildingsLIFO.empty())

		{

			PruneAttackedBuildingsLIFO(pHouse);

			int chosenIdx = -1;

			for (int i = static_cast<int>(GetMacroState(pHouse).AttackedBuildingsLIFO.size()) - 1; i >= 0; --i)

			{

				const auto& rec = GetMacroState(pHouse).AttackedBuildingsLIFO[i];

				if ((rec.Threat == NeuralThreatCategory::Air && isPureAA) || ((rec.Threat == NeuralThreatCategory::Vehicle || rec.Threat == NeuralThreatCategory::Infantry) && isGroundDefense))

				{

					chosenIdx = i;

					break;
				}
			}

			if (chosenIdx < 0 && !GetMacroState(pHouse).AttackedBuildingsLIFO.empty())
				chosenIdx = static_cast<int>(GetMacroState(pHouse).AttackedBuildingsLIFO.size()) - 1;

			if (chosenIdx >= 0)

			{

				directionalAttackedBld = GetMacroState(pHouse).AttackedBuildingsLIFO[chosenIdx].BuildingCoords;

				directionalAttacker = GetMacroState(pHouse).AttackedBuildingsLIFO[chosenIdx].AttackerCoords;
			}
		}

		std::vector<CellStruct> ownMatchingDefCells;

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

			const bool isCapturedTechOrCash = (pOT->ProduceCashAmount > 0 || pOT->ProduceCashStartup > 0 || pOT->ToProtect || (pOT->Capturable && pOT->NeedsEngineer));
			const bool isRemoteAsset = (std::max(std::abs(bc.X - center.X), std::abs(bc.Y - center.Y)) > 15);
			if (isCapturedTechOrCash || (isRemoteAsset && (pOT->Refinery || pOT->ConstructionYard || pOT->Factory != AbstractType::None)))
				priorityAssetCells.insert(priorityAssetCells.begin(), bc); // Highest priority: defend remote Oil Derricks, Tech Outposts & Expansion Refineries!
			else if (pOT->ConstructionYard || pOT->Factory == AbstractType::UnitType || pOT->SuperWeapon != -1 || pOT->Refinery)
				priorityAssetCells.push_back(bc);
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

	const bool enableBaseCrawl = isForwardTunnelMouth || isSteppingStoneExpander ||
								((isResourceBld || isFactoryOrBarracks) && (crawlBias > -20 || bestOreDist > 10 || pHouse->CountResourceDestinations >= 1)) ||
								(turretCreepBias > 10 && isCombatDefense && totalOwnedDefensesCount >= 2 && directionalAttackedBld == CellStruct::Empty);

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

		const bool crawlTowardOre =
			isResourceBld || isSteppingStoneExpander || (isFactoryOrBarracks && nearestOreCell != CellStruct::Empty && bestOreDist > 10 && pHouse->CountResourceDestinations < 3);
		const CellStruct crawlGoal =
			crawlTowardOre ? (nearestOreCell != CellStruct::Empty ? nearestOreCell : enemyTargetCell) : (enemyTargetCell != CellStruct::Empty ? enemyTargetCell : nearestOreCell);

		if (crawlGoal != CellStruct::Empty)

		{

			int bestAnchorDist = std::abs(center.X - crawlGoal.X) + std::abs(center.Y - crawlGoal.Y);

			for (auto const pOwnBld : pHouse->Buildings)
			{
				if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->Insignificant || pOwnBld->Type->InvisibleInGame)
					continue;

				if (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard && !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
					continue;

				const CellStruct bc = pOwnBld->GetMapCoords();
				if (centerZone >= 0 && MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, false) != centerZone)
					continue;

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

		(GetMacroState(pHouse).PendingClearanceType != nullptr && GetMacroState(pHouse).PendingClearanceType != pBuildingType && GetMacroState(pHouse).PendingClearanceCell != CellStruct::Empty);

	// Collect all valid BaseNormal / ConstructionYard adjacency anchor structures (from AdvAI)

	std::vector<BuildingClass*> adjacencyAnchors;

	if (!pBuildingType->ConstructionYard)

	{

		for (auto const pOwnBld : pHouse->Buildings)

		{

			if (!pOwnBld || pOwnBld->InLimbo || !pOwnBld->IsAlive || !pOwnBld->Type || pOwnBld->Type->InvisibleInGame)
				continue;

			if (pOwnBld->CurrentMission == Mission::Selling || pOwnBld->QueuedMission == Mission::Selling)
				continue;

			if (pOwnBld->Type->BaseNormal || pOwnBld->Type->ConstructionYard || BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type) || (isNavalBld && pOwnBld->Type->Naval))
				adjacencyAnchors.push_back(pOwnBld);
		}

		if (GameModeOptionsClass::Instance.BuildOffAlly && !isRefineryOrSlaveMiner)

		{

			for (auto const pAllyBld : BuildingClass::Array)

			{

				if (!pAllyBld || pAllyBld->InLimbo || !pAllyBld->IsAlive || !pAllyBld->Owner || !pAllyBld->Type || pAllyBld->Type->InvisibleInGame)
					continue;

				if (pAllyBld->Owner != pHouse && pHouse->IsAlliedWith(pAllyBld->Owner) && !pAllyBld->Owner->IsNeutral() && pAllyBld->Type->EligibileForAllyBuilding && pAllyBld->Type->BaseNormal)
					adjacencyAnchors.push_back(pAllyBld);
			}
		}

		if (adjacencyAnchors.empty())
			return CellStruct::Empty;
	}

	// Helper that enforces real C&C / YR building adjacency (pBuildingType->Adjacent / AINavalYardAdjacency)

	// against our BaseNormal anchor structures using both fast bounding box and exact GetFoundationData(true) cells

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

	// 1) Gather exact Adjacent= ring around EVERY BaseNormal / ConstructionYard anchor (from AdvAI)
	// so we never miss a valid placement slot even when ConYard[0] is surrounded or an expansion is far away!
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

					candidateList.push_back(CellStruct{static_cast<short>(cx), static_cast<short>(cy)});
				}
			}
		}
	}
	else if (isNavalBld)
	{
		const int navalMaxAdj = std::max(std::max(1, pBuildingType->Adjacent), RulesClass::Instance ? RulesClass::Instance->AINavalYardAdjacency : 36);
		const int maxRadius = std::clamp(navalMaxAdj + w + 4, 28, 56);
		for (int r = maxRadius; r >= 1; --r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != r)
						continue;

					const CellStruct cand{static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy)};
					if (auto const pC = MapClass::Instance.TryGetCellAt(cand))
					{
						if (pC->LandType == LandType::Water)
							candidateList.push_back(cand);
					}
				}
			}
		}

		const int baseAdjRange = std::max(1, pBuildingType->Adjacent);
		for (auto const pAnchor : adjacencyAnchors)
		{
			const CellStruct ac = pAnchor->GetMapCoords();
			const bool isOtherConYard = pAnchor->Type->ConstructionYard;
			if (isOtherConYard && std::max(std::abs(ac.X - center.X), std::abs(ac.Y - center.Y)) <= 4)
				continue;

			if (!isOtherConYard && std::max(std::abs(ac.X - center.X), std::abs(ac.Y - center.Y)) <= 8)
				continue;

			const int anchorScanRange = isOtherConYard ? maxRadius : (baseAdjRange + w + 2);
			for (int r = anchorScanRange; r >= 1; --r)
			{
				for (int dy = -r; dy <= r; ++dy)
				{
					for (int dx = -r; dx <= r; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != r)
							continue;

						const CellStruct cand{static_cast<short>(ac.X + dx), static_cast<short>(ac.Y + dy)};
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

					candidateList.push_back(CellStruct{static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy)});
				}
			}
		}
	}

	const int maxCandidatesToEvaluate = 900;
	// Two-pass evaluation: Pass 0 strictly requires the 1-cell street margin (IsCellClearedOfExitCorridors);
	// Pass 1 only runs if Pass 0 found 0 valid cells (e.g. tight cliff/island corner), allowing 0-cell touching with a heavy score penalty (like AdvAI).
	for (int relaxMarginPass = 0; relaxMarginPass < 2 && bestCandidate == CellStruct::Empty; ++relaxMarginPass)
	{
		{
			for (const CellStruct& candidate : candidateList)
			{
				const int radius = std::max(1, std::max(std::abs(candidate.X - center.X), std::abs(candidate.Y - center.Y)));

				if (!MapClass::Instance.CoordinatesLegal(candidate))
					continue;

				// Strictly enforce BaseNormal Adjacency (Adjacent= / AINavalYardAdjacency)

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

					if ((candidate.X < rX + rW) && (candidate.X + w > rX) && (candidate.Y < rY + rH) && (candidate.Y + h > rY))
						continue;
				}

				// Never crush or block Ore/Tiberium Spawner Trees or place on Bridges

				if (OverlapsTiberiumTreeZone(candidate, pBuildingType) || OverlapsBridge(candidate, pBuildingType))
					continue;

				bool foundationValid = true;

				for (int fy = 0; fy < h && foundationValid; ++fy)

				{

					for (int fx = 0; fx < w; ++fx)

					{

						const CellStruct fc{static_cast<short>(candidate.X + fx), static_cast<short>(candidate.Y + fy)};

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

					// Even in relaxed pass, never overlap an existing building footprint or clump turrets < 4 cells
					bool overlapsFootprint = false;
					for (auto const pEx : BuildingClass::Array)
					{
						if (!pEx || pEx->InLimbo || !pEx->IsAlive || !pEx->Type || pEx->Type->InvisibleInGame)
							continue;

						const CellStruct exC = pEx->GetMapCoords();
						const int exW = pEx->Type->GetFoundationWidth();
						const int exH = pEx->Type->GetFoundationHeight(false);
						if ((candidate.X < exC.X + exW) && (candidate.X + w > exC.X) && (candidate.Y < exC.Y + exH) && (candidate.Y + h > exC.Y))
						{
							overlapsFootprint = true;
							break;
						}

						if (isCombatDefense && pEx->Type->IsBaseDefense && (std::abs(candidate.X - exC.X) + std::abs(candidate.Y - exC.Y)) < 4)
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
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Immune || pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
							continue;

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

							if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Harvester || pEU->Type->ResourceGatherer)
								continue;

							if (pEU->Owner == pHouse || pHouse->IsAlliedWith(pEU->Owner) || pEU->Owner->IsNeutral())
								continue;

							const CellStruct euc = pEU->GetMapCoords();

							if (std::max(std::abs(candidate.X - euc.X), std::abs(candidate.Y - euc.Y)) <= 12)

							{

								enemyThreatNearCandidate = true;

								break;
							}
						}
					}

					if (enemyThreatNearCandidate)
						continue;
				}

				const int candZone = MapClass::Instance.GetMovementZoneType(candidate, MovementZone::Normal, false);
				const int expansionZoneBonus = (!isNavalBld && targetExpansionZone >= 0 && candZone == targetExpansionZone) ? 650 : 0;
				int spatialScore = (isNavalBld ? (radius * 28) : (enableBaseCrawl ? (-radius * 5) : (-radius * 10))) - (hasOneCellMargin ? 0 : 4000) + expansionZoneBonus;
				if (isNavalBld)
				{
					int openWaterCells = 0;
					int coastalBlockedCells = 0;
					for (int fy = -2; fy <= h + 1; ++fy)
					{
						for (int fx = -2; fx <= w + 1; ++fx)
						{
							if (fx >= 0 && fx < w && fy >= 0 && fy < h)
								continue;

							const CellStruct wc{static_cast<short>(candidate.X + fx), static_cast<short>(candidate.Y + fy)};
							auto const pWC = MapClass::Instance.TryGetCellAt(wc);
							if (pWC && pWC->LandType == LandType::Water && pWC->GetBuilding() == nullptr && !pWC->ContainsBridge())
								openWaterCells++;
							else
								coastalBlockedCells++;
						}
					}

					spatialScore += openWaterCells * 35 - coastalBlockedCells * 60;

					int minLandBldDist = 999;
					for (auto const pOwnB : pHouse->Buildings)
					{
						if (!pOwnB || pOwnB->InLimbo || !pOwnB->IsAlive || !pOwnB->Type)
							continue;

						const CellStruct nbc = pOwnB->GetMapCoords();
						const int bldDist = std::max(std::abs(candidate.X - nbc.X), std::abs(candidate.Y - nbc.Y));
						if (pOwnB->Type->Naval || (pOwnB->Type->SpeedType == SpeedType::Float))
						{
							if (bldDist < 8)
								spatialScore -= 3500;
						}
						else
						{
							if (bldDist < minLandBldDist)
								minLandBldDist = bldDist;
						}
					}

					if (minLandBldDist < 999)
						spatialScore += std::min(minLandBldDist, 42) * 35;

					if (enemyTargetCell != CellStruct::Empty)
					{
						const int distToEnemy = std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);
						spatialScore -= distToEnemy * 6;
					}
				}
				// Bonus when South/East exit of a Factory or Refinery has 2+ clear cells in front of it
				if (isFactoryOrBarracks || isRefineryOrSlaveMiner)
				{
					const CellStruct southExit{static_cast<short>(candidate.X + w / 2), static_cast<short>(candidate.Y + h + 1)};
					const CellStruct eastExit{static_cast<short>(candidate.X + w + 1), static_cast<short>(candidate.Y + h / 2)};
					if (auto const pSE = MapClass::Instance.TryGetCellAt(southExit))
					{
						if (pSE->GetBuilding() == nullptr && pSE->LandType != LandType::Water && pSE->LandType != LandType::Rock)
							spatialScore += 220;
					}

					if (auto const pEE = MapClass::Instance.TryGetCellAt(eastExit))
					{
						if (pEE->GetBuilding() == nullptr && pEE->LandType != LandType::Water && pEE->LandType != LandType::Rock)
							spatialScore += 220;
					}
				}

				if ((isResourceBld || isSteppingStoneExpander) && nearestOreCell != CellStruct::Empty)
				{
					// Expert 6, Slot 34 & AdvAI Stepping-Stone Crawler:
					// Chain Refineries, Silos, Ore Purifiers AND BaseNormal stepping-stone Power Plants
					// directly toward the nearest uncolonized Ore/Gem patch!
					const int distToOre = std::abs(candidate.X - nearestOreCell.X) + std::abs(candidate.Y - nearestOreCell.Y);
					const int orePull = std::clamp(52 + (crawlBias / 8), 32, 95);
					spatialScore -= distToOre * orePull;

					if (enableBaseCrawl && radius >= 2 && radius <= 12)
						spatialScore += 220 + std::clamp(crawlBias / 3, 0, 180);
				}

				else if (isCombatDefense)

				{

					// Priority 1: Directional Defense Placement along active attack

					// vector (B -> A)

					if (directionalAttackedBld != CellStruct::Empty && directionalAttacker != CellStruct::Empty)

					{

						const double dxA = static_cast<double>(directionalAttacker.X - directionalAttackedBld.X);

						const double dyA = static_cast<double>(directionalAttacker.Y - directionalAttackedBld.Y);

						const double L2 = dxA * dxA + dyA * dyA;

						const int distToAttacked = std::abs(candidate.X - directionalAttackedBld.X) + std::abs(candidate.Y - directionalAttackedBld.Y);

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

						const int distToAsset = std::abs(candidate.X - undefendedKeyAssetCell.X) + std::abs(candidate.Y - undefendedKeyAssetCell.Y);

						spatialScore -= distToAsset * 42;

						if (distToAsset >= 4 && distToAsset <= 9)
							spatialScore += 280;

						if (enemyTargetCell != CellStruct::Empty)

						{

							const int distToEnemy = std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

							spatialScore -= distToEnemy * 16;
						}
					}

					else if (isGroundDefense && enemyTargetCell != CellStruct::Empty)

					{

						// Priority 3: Frontline perimeter facing the enemy

						const int distToEnemy = std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

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

					const int distToEnemy = std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

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

							const int distToFac = std::abs(candidate.X - homeFactoryCell.X) + std::abs(candidate.Y - homeFactoryCell.Y);

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

							const int distToEnemy = std::abs(candidate.X - enemyTargetCell.X) + std::abs(candidate.Y - enemyTargetCell.Y);

							spatialScore -= distToEnemy * 40;
						}
					}
				}

				else if (isFactoryOrBarracks && (enemyTargetCell != CellStruct::Empty || nearestOreCell != CellStruct::Empty))
				{
					// Place War Factories & Barracks forward along the crawl chain (stepping toward uncolonized Ore when expanding economy, or toward Enemy frontline!)
					const bool stepFactoryToOre = (nearestOreCell != CellStruct::Empty && bestOreDist > 10 && (pHouse->CountResourceDestinations < 3 || Unsorted::CurrentFrame > 2400));
					const CellStruct facGoal = stepFactoryToOre ? nearestOreCell : (enemyTargetCell != CellStruct::Empty ? enemyTargetCell : nearestOreCell);
					const int distToGoal = std::abs(candidate.X - facGoal.X) + std::abs(candidate.Y - facGoal.Y);
					const int fwdPull = stepFactoryToOre ? std::clamp(38 + (crawlBias / 10), 24, 75) : std::clamp(22 + (crawlBias / 12), 12, 55);
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

		const bool coordsValid = GetMacroState(pHouse).PendingClearanceCell != CellStruct::Empty && MapClass::Instance.CoordinatesLegal(GetMacroState(pHouse).PendingClearanceCell);

		const bool foundationMatches = (GetMacroState(pHouse).PendingClearanceType == pBuildingType) ||

									(GetMacroState(pHouse).PendingClearanceFoundationWidth == w && GetMacroState(pHouse).PendingClearanceFoundationHeight == h &&

										GetMacroState(pHouse).PendingClearanceFoundation == pBuildingType->Foundation);

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

					const CellStruct fc{static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.X + fx), static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.Y + fy)};

					if (auto const pCell = MapClass::Instance.TryGetCellAt(fc))

					{

						if (pCell->GetBuilding() != nullptr)
							stillObstructed = true;

						if (pCell->GetUnit(false) || pCell->GetInfantry(false))

						{

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

						const CellStruct fc{static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.X + fx), static_cast<short>(GetMacroState(pHouse).PendingClearanceCell.Y + fy)};

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

					Debug::Log("[NeuralAI] House %d placed %s at cleared reservation cell (%d,%d).\n", pHouse->ArrayIndex, pBuildingType->ID, outCell.X, outCell.Y);

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

	// Step 2: Normal Spatial Tetris placement search

	const CellStruct bestCell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pBuildingType);

	if (bestCell != CellStruct::Empty)
	{
		GetMacroState(pHouse).ConsecutivePlacementFailures = 0;
		GetMacroState(pHouse).BuildingPlacementCooldownUntilFrame.erase(pBuildingType->ArrayIndex);
		if (pBuildingType->IsBaseDefense && !GetMacroState(pHouse).AttackedBuildingsLIFO.empty())
			GetMacroState(pHouse).AttackedBuildingsLIFO.pop_back();

		outCell = bestCell;

		return 2;
	}

	// Step 3: Surgical Space Clearance & Obsolete Structure Replacement when the

	// base is congested and no free cell exists!

	auto recordPlacementFailure = [&]() -> int
	{
		auto& st = GetMacroState(pHouse);
		st.ConsecutivePlacementFailures++;
		const int cdUntil = Unsorted::CurrentFrame + std::min(3600, 300 * st.ConsecutivePlacementFailures);
		st.BuildingPlacementCooldownUntilFrame[pBuildingType->ArrayIndex] = cdUntil;
		if (pBuildingType->PowerBonus > pBuildingType->PowerDrain && !pBuildingType->IsBaseDefense && !pHouse->HasLowPower())
		{
			for (auto const pOtherBld : BuildingTypeClass::Array)
			{
				if (pOtherBld && pOtherBld->PowerBonus > pOtherBld->PowerDrain && !pOtherBld->IsBaseDefense)
					st.BuildingPlacementCooldownUntilFrame[pOtherBld->ArrayIndex] = cdUntil;
			}
		}
		if (FactoryClass* pBldFac = pHouse->GetPrimaryFactory(AbstractType::BuildingType, pBuildingType->Naval, pBuildingType->BuildCat))
			pBldFac->AbandonProduction();

		return 0;
	};

	if (Unsorted::CurrentFrame < GetMacroState(pHouse).LastClearanceFailedFrame + 150)
		return recordPlacementFailure();

	const int money = pHouse->Available_Money();

	const bool isCombatDef = pBuildingType->IsBaseDefense;

	const bool isLateGameHighTierDefense = isCombatDef && pBuildingType->TechLevel >= 6 && pBuildingType->Cost >= 1000 && money > 4000;

	if (isCombatDef && !isLateGameHighTierDefense)
		return recordPlacementFailure(); // Only sacrifice structures for key non-defense buildings or

	// late-game high-tier defense upgrades

	bool hasUnpowered = false;

	int poweredDefAndSWCount = 0;

	int maxSinglePlantBonus = 0;

	bool hasAdvancedPowerPlants = false;

	const int realDrain = GetRealPowerDrainAndGridMetrics(pHouse, hasUnpowered, poweredDefAndSWCount, maxSinglePlantBonus, hasAdvancedPowerPlants);

	const int realSurplus = pHouse->PowerOutput - realDrain;

	const int requiredCushion = (poweredDefAndSWCount > 0) ? std::max(300, std::min(550, 220 + poweredDefAndSWCount * 45)) : 150;

	// Strict Silo Rule: NEVER sell Ore Silos if the house has any stored

	// Tiberium/Ore (because selling a Silo with ore destroys the stored credits!)

	const bool canSacrificeEmptySilos = (pHouse->OwnedTiberium.GetTotalAmount() <= 0);

	int ownedRefineries = 0;

	for (auto const pOwnBld : pHouse->Buildings)

	{

		if (pOwnBld && pOwnBld->IsAlive && !pOwnBld->InLimbo && pOwnBld->Type && (pOwnBld->Type->Refinery || pOwnBld->Type->ResourceDestination))
			ownedRefineries++;
	}

	const CellStruct baseCenter = (pHouse->ConYards.Count > 0 && pHouse->ConYards.GetItem(0)) ? pHouse->ConYards.GetItem(0)->GetMapCoords()

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

			const CellStruct cand{static_cast<short>(baseCenter.X + dx), static_cast<short>(baseCenter.Y + dy)};

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

					const CellStruct fc{static_cast<short>(cand.X + fx), static_cast<short>(cand.Y + fy)};

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

					else if (pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach || pCell->LandType == LandType::Rock || pCell->ContainsBridge())

					{

						terrainAndUnitsValid = false;

						break;
					}

					if (pCell->OverlayTypeIndex >= 0)

					{

						if (auto const pOv = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex))

						{

							if (pOv->Wall)

							{

								terrainAndUnitsValid = false;

								break;
							}
						}
					}

					if (auto const pOcc = pCell->GetBuilding())

					{

						if (pOcc->Owner != pHouse || !pOcc->IsAlive || pOcc->InLimbo || !pOcc->Type || pOcc->Type->ToProtect || pOcc->Type->ConstructionYard ||

							pOcc->CurrentMission == Mission::Construction || pOcc->CurrentMission == Mission::Selling)

						{

							terrainAndUnitsValid = false;

							break;
						}

						if (std::find(obstructingBlds.begin(), obstructingBlds.end(), pOcc) == obstructingBlds.end())

						{

							auto const pOT = pOcc->Type;

							int itemCost = -1;

							// Priority 1: Exhausted or redundant idle Refinery

							if ((pOT->Refinery || pOT->ResourceDestination) && !pOT->ResourceGatherer && !pOT->UndeploysInto && !pOcc->HasAnyLink() &&

								(IsRefineryExhausted(pOcc, baseCenter) || (ownedRefineries > 2 && pBuildingType->Refinery)))

							{

								itemCost = 100;
							}

							// Priority 2: Empty Ore Silo (STRICTLY ONLY when stored Tiberium

							// == 0!)

							else if (pOT->Storage > 0 && !pOT->Refinery && !pOT->ResourceGatherer && canSacrificeEmptySilos)

							{

								itemCost = 250;
							}

							// Priority 3: Redundant Basic Power Plant when Advanced Reactors

							// exist and power cushion stays safe

							else if (pOT->PowerBonus > pOT->PowerDrain && pOT->PowerBonus <= 200 && !pOT->IsBaseDefense && hasAdvancedPowerPlants &&

									(realSurplus - (lostPowerBonus + pOT->PowerBonus) - pBuildingType->PowerDrain) >= requiredCushion)

							{

								itemCost = 400;

								lostPowerBonus += pOT->PowerBonus;
							}

							// Priority 4: Obstructing Tier-1 Defense (for non-defense

							// infrastructure or late-game Tier-2/3 defense upgrade)

							else if (pOT->IsBaseDefense && !IsTunnelNetworkBuilding(pOT) && (!isCombatDef || (isLateGameHighTierDefense && pBuildingType->Cost >= pOT->Cost + 450)))

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

				if (!pOwnBld->Type->BaseNormal && !pOwnBld->Type->ConstructionYard && !BuildingTypeExt::IsAIBaseNormal(pOwnBld->Type))
					continue;

				const CellStruct ac = pOwnBld->GetMapCoords();

				const int aW = pOwnBld->Type->GetFoundationWidth();

				const int aH = pOwnBld->Type->GetFoundationHeight(false);

				const int gapX = std::max(0, std::max(static_cast<int>(cand.X) - (static_cast<int>(ac.X) + aW - 1), static_cast<int>(ac.X) - (static_cast<int>(cand.X) + w - 1)));

				const int gapY = std::max(0, std::max(static_cast<int>(cand.Y) - (static_cast<int>(ac.Y) + aH - 1), static_cast<int>(ac.Y) - (static_cast<int>(cand.Y) + h - 1)));

				if (std::max(gapX, gapY) <= adjRange)

				{

					hasSurvivingAnchor = true;

					break;
				}
			}

			if (!hasSurvivingAnchor)
				continue;

			const int totalScore = sacrificeScore + static_cast<int>(obstructingBlds.size()) * 500 + (std::abs(cand.X - baseCenter.X) + std::abs(cand.Y - baseCenter.Y)) * 4;

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

				Debug::Log("[NeuralAI] House %d selling obstructing %s at (%d,%d) to clear space for %s at (%d,%d).\n", pHouse->ArrayIndex, pObs->Type->ID, pObs->GetMapCoords().X,

					pObs->GetMapCoords().Y, pBuildingType->ID, bestClearanceCell.X, bestClearanceCell.Y);

				pObs->Sell(1);
			}
		}

		for (int fy = 0; fy < h; ++fy)

		{

			for (int fx = 0; fx < w; ++fx)

			{

				const CellStruct fc{static_cast<short>(bestClearanceCell.X + fx), static_cast<short>(bestClearanceCell.Y + fy)};

				if (auto const pCell = MapClass::Instance.TryGetCellAt(fc))
					pCell->ScatterContent(CellClass::Cell2Coord(fc), true, true, false);
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
