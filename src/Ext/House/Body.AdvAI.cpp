#include "Body.h"
#include <Utilities/GeneralUtils.h>
#include <Ext/BuildingType/Body.h>
#include <Ext/Building/Body.h>
#include <Ext/SWType/Body.h>
#include <Ext/TechnoType/Body.h>

#include <AircraftClass.h>
#include <FactoryClass.h>
#include <functional>
#include <GameOptionsClass.h>
#include <OverlayClass.h>
#include <TerrainClass.h>
#include <CellClass.h>
#include <AStarClass.h>

#define TICKS_PER_SECOND    15
#define TICKS_PER_MINUTE    (TICKS_PER_SECOND * 60)
#define TICKS_PER_HOUR      (TICKS_PER_MINUTE * 60)

std::vector<BuildingTypeClass*> HouseExt::BaseDefenses;
bool HouseExt::BaseDefensesInitialized;

static int GetRealPowerDrain(const HouseClass* pHouse);
static bool IsBuildingTypeQueued(HouseClass* pHouse, TechTreeTypeClass::BuildType buildType);

enum class SupportRadiusType { None, Cloak, Gap, Inhibitor, RadarJam, EMPulseCannon };

static SupportRadiusType GetSupportRadiusType(const BuildingTypeClass* pType)
{
	if (pType->CloakGenerator && pType->CloakRadiusInCells > 0)
		return SupportRadiusType::Cloak;

	const auto pBldExt = BuildingTypeExt::ExtMap.Find(pType);
	if (pBldExt->GapGenerator && (pBldExt->GapRadiusInCells != 0 || pBldExt->SuperGapRadiusInCells != 0))
		return SupportRadiusType::Gap;

	const auto pTechnoTypeExt = TechnoTypeExt::ExtMap.Find(pType);
	if (pTechnoTypeExt->InhibitorRange.isset() && pTechnoTypeExt->InhibitorRange.Get() > 0)
		return SupportRadiusType::Inhibitor;

	if (pTechnoTypeExt->RadarJamRadius.Get() > 0)
		return SupportRadiusType::RadarJam;

	if (pType->EMPulseCannon)
		return SupportRadiusType::EMPulseCannon;

	return SupportRadiusType::None;
}

static bool IsSameSupportNetwork(const BuildingTypeClass* pTypeA, const BuildingTypeClass* pTypeB)
{
	const SupportRadiusType typeA = GetSupportRadiusType(pTypeA);
	const SupportRadiusType typeB = GetSupportRadiusType(pTypeB);

	if (typeA == SupportRadiusType::None || typeB == SupportRadiusType::None)
		return false;

	if (typeA != typeB)
		return false;

	const char* groupA = TechnoTypeExt::GetSelectionGroupID(const_cast<BuildingTypeClass*>(pTypeA));
	const char* groupB = TechnoTypeExt::GetSelectionGroupID(const_cast<BuildingTypeClass*>(pTypeB));

	return _stricmp(groupA, groupB) == 0;
}

static int GetSupportRadius(const BuildingTypeClass* pType)
{
	int minRadius = 999;

	if (pType->CloakGenerator && pType->CloakRadiusInCells > 0 && pType->CloakRadiusInCells < minRadius)
		minRadius = pType->CloakRadiusInCells;

	const auto pBldExt = BuildingTypeExt::ExtMap.Find(pType);
	if (pBldExt->GapGenerator)
	{
		if (pBldExt->GapRadiusInCells > 0 && pBldExt->GapRadiusInCells < minRadius)
			minRadius = pBldExt->GapRadiusInCells;

		if (pBldExt->SuperGapRadiusInCells > 0 && pBldExt->SuperGapRadiusInCells < minRadius)
			minRadius = pBldExt->SuperGapRadiusInCells;
	}

	const auto pExt = TechnoTypeExt::ExtMap.Find(pType);
	if (pExt->InhibitorRange.isset() && pExt->InhibitorRange.Get() > 0 && pExt->InhibitorRange.Get() < minRadius)
		minRadius = pExt->InhibitorRange.Get();

	if (pExt->RadarJamRadius.Get() > 0 && pExt->RadarJamRadius.Get() < minRadius)
		minRadius = pExt->RadarJamRadius.Get();

	if (pType->EMPulseCannon)
	{
		std::vector<int> swIndices;
		if (pType->SuperWeapon != -1)
			swIndices.push_back(pType->SuperWeapon);
		if (pType->SuperWeapon2 != -1)
			swIndices.push_back(pType->SuperWeapon2);

		const auto pBldTypeExt = BuildingTypeExt::ExtMap.Find(pType);
		for (const auto swIdx : pBldTypeExt->SuperWeapons)
		{
			swIndices.push_back(swIdx);
		}

		for (const auto swIdx : swIndices)
		{
			if (swIdx >= 0 && swIdx < SuperWeaponTypeClass::Array.Count)
			{
				const auto pSW = SuperWeaponTypeClass::Array[swIdx];
				const auto pSWExt = SWTypeExt::ExtMap.Find(pSW);

				double swRadius = -1.0;
				if (pSWExt->SW_RangeMaximum.Get() > 0.0)
				{
					swRadius = pSWExt->SW_RangeMaximum.Get();
				}
				else if (pSWExt->SW_RangeMinimum.Get() > 0.0)
				{
					swRadius = pSWExt->SW_RangeMinimum.Get();
				}
				else if (const auto pWeapon = pType->GetWeapon(0u, false).WeaponType)
				{
					swRadius = pWeapon->Range / (double)Unsorted::LeptonsPerCell;
				}

				if (swRadius > 0.0 && swRadius < minRadius)
				{
					minRadius = static_cast<int>(swRadius);
				}
			}
		}
	}

	return minRadius == 999 ? 0 : minRadius;
}

static const char* GetGroupAsID(BuildingTypeClass* pType)
{
	if (const auto pExt = TechnoTypeExt::ExtMap.Find(pType))
	{
		if (pExt->GroupAs.data() && pExt->GroupAs.data()[0] != '\0' && _stricmp(pExt->GroupAs.data(), "none") != 0)
		{
			return pExt->GroupAs.data();
		}
	}
	return pType->ID;
}

static int CountBuildingOfGroup(HouseClass* pHouse, BuildingTypeClass* pType)
{
	const char* groupID = GetGroupAsID(pType);
	int count = 0;
	for (const auto pBuilding : pHouse->Buildings)
	{
		if (pBuilding && pBuilding->IsAlive && !pBuilding->InLimbo)
		{
			if (_stricmp(GetGroupAsID(pBuilding->Type), groupID) == 0)
			{
				count++;
			}
		}
	}
	return count;
}

static const BuildingTypeClass* GetBasicDefense(HouseClass* pHouse, TechTreeTypeClass* pTechTree)
{
	if (pTechTree == nullptr || pHouse == nullptr)
		return nullptr;

	auto canBuildFunction = [pHouse](BuildingTypeClass* pType)
	{
		return HouseExt::AdvAI_Can_Build_Building(pHouse, pType, true, true);
	};

	for (const auto pType : pTechTree->BuildDefense)
	{
		if (canBuildFunction(pType))
		{
			if (pType->Cost <= 1000)
			{
				return pType;
			}
		}
	}

	for (const auto pType : pTechTree->BuildDefense)
	{
		if (canBuildFunction(pType))
		{
			return pType;
		}
	}

	return nullptr;
}

static bool IsBuildingTypeQueued(HouseClass* pHouse, TechTreeTypeClass::BuildType buildType)
{
	std::set<BuildingTypeClass*>* typeList = nullptr;
	switch (buildType)
	{
	case TechTreeTypeClass::BuildType::BuildPower:
		typeList = &TechTreeTypeClass::TotalBuildPower;
		break;
	case TechTreeTypeClass::BuildType::BuildRefinery:
		typeList = &TechTreeTypeClass::TotalBuildRefinery;
		break;
	case TechTreeTypeClass::BuildType::BuildBarracks:
		typeList = &TechTreeTypeClass::TotalBuildBarracks;
		break;
	case TechTreeTypeClass::BuildType::BuildWeapons:
		typeList = &TechTreeTypeClass::TotalBuildWeapons;
		break;
	case TechTreeTypeClass::BuildType::BuildRadar:
		typeList = &TechTreeTypeClass::TotalBuildRadar;
		break;
	case TechTreeTypeClass::BuildType::BuildHelipad:
		typeList = &TechTreeTypeClass::TotalBuildHelipad;
		break;
	case TechTreeTypeClass::BuildType::BuildNavalYard:
		typeList = &TechTreeTypeClass::TotalBuildNavalYard;
		break;
	case TechTreeTypeClass::BuildType::BuildTech:
		typeList = &TechTreeTypeClass::TotalBuildTech;
		break;
	case TechTreeTypeClass::BuildType::BuildAdvancedPower:
		typeList = &TechTreeTypeClass::TotalBuildAdvancedPower;
		break;
	case TechTreeTypeClass::BuildType::BuildServiceDepot:
		typeList = &TechTreeTypeClass::TotalBuildServiceDepot;
		break;
	default:
		break;
	}

	if (typeList != nullptr)
	{
		for (const auto pBldType : *typeList)
		{
			if (FactoryClass::FindByOwnerAndProduct(pHouse, pBldType) != nullptr)
			{
				return true;
			}
		}
	}

	return false;
}

void HouseExt::InitializeBaseDefenses()
{
	for (const auto pBuilding : BuildingTypeClass::Array)
	{
		if (pBuilding->IsBaseDefense)
		{
			BaseDefenses.push_back(pBuilding);
		}
	}

	BaseDefensesInitialized = true;
}

///
/// Advanced AI
///	Credits to Rampastring
///

static int GetCellLandZone(const CellStruct& cell)
{
	int zone = MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false);
	if (zone > 0)
		return zone;

	// If the cell itself is impassable (e.g. because it contains the tree itself, or is blocked by an obstacle), check adjacent cells
	static const int dx[] = { 0, 1, 1, 1, 0, -1, -1, -1 };
	static const int dy[] = { -1, -1, 0, 1, 1, 1, 0, -1 };
	for (int i = 0; i < 8; ++i)
	{
		CellStruct neighbor(static_cast<short>(cell.X + dx[i]), static_cast<short>(cell.Y + dy[i]));
		if (MapClass::Instance.CoordinatesLegal(neighbor))
		{
			int nZone = MapClass::Instance.GetMovementZoneType(neighbor, MovementZone::Normal, false);
			if (nZone > 0)
				return nZone;
		}
	}
	return 0;
}



struct ResourceSector
{
	CellStruct BoundsMin;
	CellStruct BoundsMax;
	CellStruct CachedCoords;
	bool HasResources;
};

static std::vector<ResourceSector> GlobalResourceSectors;
static std::vector<CellStruct> GlobalTiberiumTrees;
static bool SectorsInitialized = false;
static int NumSectorsX = 0;
static size_t NextActiveSectorToScan = 0;
static size_t NextFringeSectorToScan = 0;
static size_t NextPassiveSectorToScan = 0;
static int GetTiberiumSectorIndex(CellStruct coords);

static bool ScanSectorForResources(ResourceSector& sector)
{
	bool found = false;
	for (int y = sector.BoundsMin.Y; y < sector.BoundsMax.Y; ++y)
	{
		for (int x = sector.BoundsMin.X; x < sector.BoundsMax.X; ++x)
		{
			CellStruct cellCoords(static_cast<short>(x), static_cast<short>(y));
			if (MapClass::Instance.CoordinatesLegal(cellCoords))
			{
				const CellClass* cell = MapClass::Instance.GetCellAt(cellCoords);
				if (cell && cell->OverlayTypeIndex != -1)
				{
					if (const auto pOverlayType = OverlayTypeClass::Array.GetItem(cell->OverlayTypeIndex))
					{
						if (pOverlayType->Tiberium)
						{
							sector.CachedCoords = cellCoords;
							found = true;
							break;
						}
					}
				}
			}
		}
		if (found)
			break;
	}
	return found;
}

static bool IsFringeSector(size_t idx)
{
	if (NumSectorsX <= 0) return false;

	int gx = static_cast<int>(idx % NumSectorsX);
	int gy = static_cast<int>(idx / NumSectorsX);

	for (int dy = -1; dy <= 1; ++dy)
	{
		for (int dx = -1; dx <= 1; ++dx)
		{
			if (dx == 0 && dy == 0) continue;
			int ngx = gx + dx;
			int ngy = gy + dy;
			if (ngx >= 0 && ngx < NumSectorsX)
			{
				size_t nIdx = static_cast<size_t>(ngy * NumSectorsX + ngx);
				if (nIdx < GlobalResourceSectors.size())
				{
					if (GlobalResourceSectors[nIdx].HasResources)
					{
						return true;
					}
				}
			}
		}
	}
	return false;
}

static void InitializeGlobalSectors()
{
	int xMin = 512, xMax = 0, yMin = 512, yMax = 0;

	// Scan the 512x512 grid to find the real bounding box of playable cell coordinates
	for (short y = 0; y < 512; ++y)
	{
		for (short x = 0; x < 512; ++x)
		{
			CellStruct cell{ x, y };
			if (MapClass::Instance.IsWithinUsableArea(cell, false))
			{
				if (x < xMin) xMin = x;
				if (x > xMax) xMax = x;
				if (y < yMin) yMin = y;
				if (y > yMax) yMax = y;
			}
		}
	}

	// Fallback to full engine dimensions if no usable cells are found
	if (xMin >= xMax || yMin >= yMax)
	{
		xMin = 0;
		xMax = MapClass::Instance.MaxWidth;
		yMin = 0;
		yMax = MapClass::Instance.MaxHeight;
	}

	if (xMax <= 0 || yMax <= 0)
	{
		// Map bounds are not yet loaded/initialized
		return;
	}

	GlobalResourceSectors.clear();
	GlobalTiberiumTrees.clear();
	NextActiveSectorToScan = 0;
	NextFringeSectorToScan = 0;
	NextPassiveSectorToScan = 0;
	const int SECTOR_SIZE = 18;

	NumSectorsX = 0;
	for (int sx = xMin; sx < xMax; sx += SECTOR_SIZE)
	{
		NumSectorsX++;
	}

	for (int sy = yMin; sy < yMax; sy += SECTOR_SIZE)
	{
		for (int sx = xMin; sx < xMax; sx += SECTOR_SIZE)
		{
			ResourceSector sector;
			sector.BoundsMin = CellStruct(static_cast<short>(sx), static_cast<short>(sy));
			sector.BoundsMax = CellStruct(static_cast<short>(sx + SECTOR_SIZE), static_cast<short>(sy + SECTOR_SIZE));
			sector.CachedCoords = CellStruct(0, 0);
			sector.HasResources = false;

			// Perform a one-time initial scan of this sector at startup
			sector.HasResources = ScanSectorForResources(sector);
			
			GlobalResourceSectors.push_back(sector);
		}
	}

	for (const auto pTerrain : TerrainClass::Array)
	{
		if (pTerrain && pTerrain->IsAlive && !pTerrain->InLimbo && pTerrain->Type->SpawnsTiberium)
		{
			GlobalTiberiumTrees.push_back(pTerrain->GetMapCoords());
		}
	}

	SectorsInitialized = true;
	Debug::Log("AdvAI: Initialized & pre-scanned %d global resource sectors (18x18). Map visible area: (%d,%d) to (%d,%d)\n",
		static_cast<int>(GlobalResourceSectors.size()), xMin, yMin, xMax, yMax);

	int activeSectorsCount = 0;
	for (size_t idx = 0; idx < GlobalResourceSectors.size(); ++idx)
	{
		const auto& sector = GlobalResourceSectors[idx];
		if (sector.HasResources)
		{
			activeSectorsCount++;
			Debug::Log("AdvAI: Startup active resource sector #%d bounds: (%d,%d) to (%d,%d), cached resource cell: (%d,%d)\n",
				static_cast<int>(idx), sector.BoundsMin.X, sector.BoundsMin.Y, sector.BoundsMax.X - 1, sector.BoundsMax.Y - 1, sector.CachedCoords.X, sector.CachedCoords.Y);
		}
	}
	Debug::Log("AdvAI: Total active tiberium sectors found on startup: %d\n", activeSectorsCount);
}

bool HouseExt::AdvAI_House_Search_For_Next_Expansion_Point(HouseClass* pHouse)
{
	const auto ext = ExtMap.Find(pHouse);

	const bool needCombatTarget = (ext->CombatCrawlingTarget.X == 0 || ext->CombatCrawlingTarget.Y == 0);
	const bool needResourceTarget = (ext->ResourceCrawlingTarget.X == 0 || ext->ResourceCrawlingTarget.Y == 0);

	if (!needCombatTarget && !needResourceTarget)
	{
		return false;
	}

	bool foundAny = false;

	// Delay active expansion until we have completed our Tech Center,
	// ensuring all early building cycles and funds go directly to teching up.
	const auto pTechTree = TechTreeTypeClass::GetAnySuitable(pHouse);
	const bool hasTechCenterSupport = pTechTree != nullptr && !pTechTree->BuildTech.empty();
	if (hasTechCenterSupport)
	{
		const bool hasTechCenter = TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildTech) >= 1;
		if (!hasTechCenter)
			return false;
	}

	// Check that we have at least one ConYard (needed to place buildings at the expansion point).
	bool hasConYard = false;
	for (const auto pBuilding : BuildingClass::Array)
	{
		if (pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Owner == pHouse && pBuilding->Type->Factory == AbstractType::BuildingType)
		{
			hasConYard = true;
			break;
		}
	}

	if (!hasConYard)
	{
		return false;
	}

	// If we already have a solid economy (>= 3 refineries/miners),
	// attempt to pivot directly to combat crawling / beachhead towards the enemy base if they are within crawling range.
	const int slaveMinerCount = RulesClass::Instance->PrerequisiteProcAlternate != nullptr ?
		pHouse->ActiveUnitTypes.GetItemCount(RulesClass::Instance->PrerequisiteProcAlternate->ArrayIndex) : 0;
	size_t refineryCount = TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildRefinery);
	refineryCount += slaveMinerCount;

	double maxDist = 80.0 + (refineryCount > 3 ? (refineryCount - 3) * 10.0 : 0.0) + (Unsorted::CurrentFrame / 9000.0) * 10.0;
	double maxDistSq = maxDist * maxDist;

	if (needCombatTarget && refineryCount >= 3)
	{
		const HouseClass* pEnemy = nullptr;
		if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
		{
			pEnemy = HouseClass::Array[pHouse->EnemyHouseIndex];
		}

		if (pEnemy != nullptr)
		{
			double nearestEnemyDistSq = std::numeric_limits<double>::max();
			CellStruct enemyTarget = CellStruct(0, 0);

			for (const auto pBld : pEnemy->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo)
				{
					for (const auto pOurBld : pHouse->Buildings)
					{
						if (pOurBld && pOurBld->IsAlive && !pOurBld->InLimbo)
						{
							double distSq = pOurBld->GetMapCoords().DistanceFromSquared(pBld->GetMapCoords());


							if (distSq < nearestEnemyDistSq && GeneralUtils::AreZonesConnected(pOurBld->GetMapCoords(), pBld->GetMapCoords()))
							{
								nearestEnemyDistSq = distSq;
								enemyTarget = pBld->GetMapCoords();
							}
						}
					}
				}
			}

			// If the enemy base is within crawling range
			int factoryCount = TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildBarracks);
			int warFactoryCount = TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildWeapons);
			if (nearestEnemyDistSq <= maxDistSq && factoryCount > 0 && warFactoryCount > 0)
			{
				const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
				const CellStruct baseCenter = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pHouse->Base_Center();

				CellStruct bestEnemyTarget(0, 0);
				double closestEnemyDist = 999999.0;

				for (const auto pBld : pEnemy->Buildings)
				{
					if (pBld && pBld->IsAlive && !pBld->InLimbo)
					{
						CellStruct bldCell = pBld->GetMapCoords();
						if (AdvAI_Is_Failed_Expansion_Point(pHouse, bldCell))
							continue;

						double dist = baseCenter.DistanceFrom(bldCell);
						if (dist < closestEnemyDist)
						{
							closestEnemyDist = dist;
							bestEnemyTarget = bldCell;
						}
					}
				}

				// Fallback to base center if no individual building is found/valid (and center not blacklisted)
				if (bestEnemyTarget.X == 0 && bestEnemyTarget.Y == 0)
				{
					CellStruct center = pEnemy->Base_Center();
					if (!AdvAI_Is_Failed_Expansion_Point(pHouse, center))
					{
						bestEnemyTarget = center;
					}
				}

				Debug::Log("AdvAI Combat Crawl Evaluation: House %d: refineryCount=%d, enemyIndex=%d, nearestEnemyDist=%.1f (Limit=%.1f), hasFactories=%s, hasTarget=%s\n",
					pHouse->ArrayIndex, refineryCount, pEnemy->ArrayIndex,
					(nearestEnemyDistSq != std::numeric_limits<double>::max() ? sqrt(nearestEnemyDistSq) : -1.0),
					maxDist,
					(factoryCount > 0 && warFactoryCount > 0 ? "YES" : "NO"),
					(bestEnemyTarget.X > 0 ? "YES" : "NO"));

				if (bestEnemyTarget.X > 0 && bestEnemyTarget.Y > 0)
				{
					ext->CombatCrawlingTarget = bestEnemyTarget;
					Debug::Log("AdvAI SearchExpansion: House %d starting COMBAT crawling towards enemy %d (%s) at (%d,%d).\n",
						pHouse->ArrayIndex, pEnemy->ArrayIndex, pEnemy->Type->ID, bestEnemyTarget.X, bestEnemyTarget.Y);
					foundAny = true;
					if (!needResourceTarget)
						return true;
				}
			}
		}
	}

	// Scan through terrain objects that spawn Tiberium (Tiberium Trees) AND map cells containing
	// Tiberium/Ore overlays. Pick the one that is closest to any of our own structures and does not
	// have a refinery near it yet. This allows the AI to expand on any map, whether it uses Tiberium trees or not.
	struct CandidateInfo
	{
		CellStruct Coords;
		bool IsTree;
	};
	std::vector<CandidateInfo> candidates;

	// Reset cache on new game or map change
	static int NextActiveScanFrame = -1;
	static int NextFringeScanFrame = -1;
	static int NextPassiveScanFrame = -1;

	if (Unsorted::CurrentFrame < 10)
	{
		SectorsInitialized = false;
		GlobalResourceSectors.clear();
		GlobalTiberiumTrees.clear();
		NextActiveSectorToScan = 0;
		NextFringeSectorToScan = 0;
		NextPassiveSectorToScan = 0;
		NextActiveScanFrame = 450;  // 30 seconds
		NextFringeScanFrame = 675;  // 45 seconds
		NextPassiveScanFrame = 1350; // 90 seconds
	}

	// 1. Scan Cached Tiberium Tree Nodes (Nodos de Árbol)
	for (const auto& treeCoords : GlobalTiberiumTrees)
	{
		candidates.push_back({ treeCoords, true });
	}

	// 2. Scan Map for cells containing Tiberium or Ore overlays using 20x20 sectors (Ground Tiberium Zones / Zonas de Suelo) and cyclical round-robin updates
	if (!SectorsInitialized)
	{
		InitializeGlobalSectors();
	}

	if (SectorsInitialized && !GlobalResourceSectors.empty())
	{
		// 1. Active Scan (1 sector every 30 seconds / 450 frames)
		if (Unsorted::CurrentFrame >= NextActiveScanFrame)
		{
			NextActiveScanFrame = Unsorted::CurrentFrame + 450;

			size_t startedAt = NextActiveSectorToScan;
			bool foundAny = false;
			do
			{
				if (NextActiveSectorToScan >= GlobalResourceSectors.size())
				{
					NextActiveSectorToScan = 0;
				}

				if (GlobalResourceSectors[NextActiveSectorToScan].HasResources)
				{
					foundAny = true;
					break;
				}
				NextActiveSectorToScan++;
			} while (NextActiveSectorToScan != startedAt);

			if (foundAny)
			{
				auto& sector = GlobalResourceSectors[NextActiveSectorToScan];
				bool oldHasResources = sector.HasResources;

				sector.HasResources = ScanSectorForResources(sector);

				if (sector.HasResources != oldHasResources)
				{
					Debug::Log("AdvAI: Active Sector #%d at (%d,%d) to (%d,%d) state changed. HasResources: %s\n",
						static_cast<int>(NextActiveSectorToScan), sector.BoundsMin.X, sector.BoundsMin.Y, sector.BoundsMax.X, sector.BoundsMax.Y,
						sector.HasResources ? "YES" : "NO");
				}
				NextActiveSectorToScan++;
			}
		}

		// 2. Fringe Scan (2 sectors every 45 seconds / 675 frames)
		if (Unsorted::CurrentFrame >= NextFringeScanFrame)
		{
			NextFringeScanFrame = Unsorted::CurrentFrame + 675;

			int scannedCount = 0;
			size_t startedAt = NextFringeSectorToScan;
			do
			{
				if (NextFringeSectorToScan >= GlobalResourceSectors.size())
				{
					NextFringeSectorToScan = 0;
				}

				auto& sector = GlobalResourceSectors[NextFringeSectorToScan];
				if (!sector.HasResources && IsFringeSector(NextFringeSectorToScan))
				{
					bool oldHasResources = sector.HasResources;

					sector.HasResources = ScanSectorForResources(sector);

					if (sector.HasResources != oldHasResources)
					{
						Debug::Log("AdvAI: Fringe Sector #%d at (%d,%d) to (%d,%d) state changed. HasResources: %s\n",
							static_cast<int>(NextFringeSectorToScan), sector.BoundsMin.X, sector.BoundsMin.Y, sector.BoundsMax.X, sector.BoundsMax.Y,
							sector.HasResources ? "YES" : "NO");
					}

					scannedCount++;
					if (scannedCount >= 2)
					{
						NextFringeSectorToScan++;
						break;
					}
				}
				NextFringeSectorToScan++;
			} while (NextFringeSectorToScan != startedAt);
		}

		// 3. Passive/Deep Scan (4 sectors every 90 seconds / 1350 frames)
		if (Unsorted::CurrentFrame >= NextPassiveScanFrame)
		{
			NextPassiveScanFrame = Unsorted::CurrentFrame + 1350;

			int scannedCount = 0;
			size_t startedAt = NextPassiveSectorToScan;
			do
			{
				if (NextPassiveSectorToScan >= GlobalResourceSectors.size())
				{
					NextPassiveSectorToScan = 0;
				}

				auto& sector = GlobalResourceSectors[NextPassiveSectorToScan];
				if (!sector.HasResources && !IsFringeSector(NextPassiveSectorToScan))
				{
					bool oldHasResources = sector.HasResources;

					sector.HasResources = ScanSectorForResources(sector);

					if (sector.HasResources != oldHasResources)
					{
						Debug::Log("AdvAI: Deep Sector #%d at (%d,%d) to (%d,%d) state changed. HasResources: %s\n",
							static_cast<int>(NextPassiveSectorToScan), sector.BoundsMin.X, sector.BoundsMin.Y, sector.BoundsMax.X, sector.BoundsMax.Y,
							sector.HasResources ? "YES" : "NO");
					}

					scannedCount++;
					if (scannedCount >= 4)
					{
						NextPassiveSectorToScan++;
						break;
					}
				}
				NextPassiveSectorToScan++;
			} while (NextPassiveSectorToScan != startedAt);
		}
	}

	// Add cached tiberium ground sector representatives to candidates, refined to the closest tiberium cell to our base
	const CellStruct baseCenter = pHouse->Base_Center();
	for (const auto& sector : GlobalResourceSectors)
	{
		if (sector.HasResources)
		{
			const int SECTOR_SIZE = 18;
			int sx = sector.BoundsMin.X;
			int sy = sector.BoundsMin.Y;

			CellStruct closestResourceCell = sector.CachedCoords;
			double minDistanceSq = std::numeric_limits<double>::max();

			for (int dy = 0; dy < SECTOR_SIZE; ++dy)
			{
				for (int dx = 0; dx < SECTOR_SIZE; ++dx)
				{
					CellStruct cellCoords(static_cast<short>(sx + dx), static_cast<short>(sy + dy));
					if (MapClass::Instance.CoordinatesLegal(cellCoords))
					{
						const CellClass* cell = MapClass::Instance.GetCellAt(cellCoords);
						if (cell && cell->OverlayTypeIndex != -1)
						{
							if (const auto pOverlayType = OverlayTypeClass::Array.GetItem(cell->OverlayTypeIndex))
							{
								if (pOverlayType->Tiberium)
								{
									double distSq = cellCoords.DistanceFromSquared(baseCenter);
									if (distSq < minDistanceSq)
									{
										minDistanceSq = distSq;
										closestResourceCell = cellCoords;
									}
								}
							}
						}
					}
				}
			}

			candidates.push_back({ closestResourceCell, false });
		}
	}

	struct ValidCandidate
	{
		CellStruct Coords;
		double Distance;
		BuildingClass* NearestBuilding;
		bool IsTree;
	};
	std::vector<ValidCandidate> validList;

	double nearestDistance = std::numeric_limits<double>::max();
	CellStruct target = CellStruct();
	const BuildingClass* pBestNearestBuilding = nullptr;

	int totalNodesChecked = 0;
	int occupiedNodes = 0;

	for (const auto& candidateInfo : candidates)
	{
		const auto& candidateCell = candidateInfo.Coords;
		bool isTree = candidateInfo.IsTree;
		totalNodesChecked++;

		// Check if this candidate is close to a recently failed expansion point
		bool isBlacklisted = false;
		for (size_t i = 0; i < std::size(ext->PermanentlyBlockedExpansionPointLocations); i++)
		{
			const auto& blocked = ext->PermanentlyBlockedExpansionPointLocations[i];
			if (blocked.Coords.X > 0 && blocked.Coords.Y > 0 && Unsorted::CurrentFrame < blocked.ExpiryFrame)
			{
				if (candidateCell.DistanceFromSquared(blocked.Coords) < 225.0) // 15-cell radius
				{
					isBlacklisted = true;
					break;
				}
			}
		}
		if (isBlacklisted)
		{
			occupiedNodes++;
			continue;
		}

		// Fetch the cell. If the cell has a non-Tiberium/Ore overlay on it,
		// we should not expand towards it.
		const CellClass* cell = MapClass::Instance.GetCellAt(candidateCell);
		if (cell && cell->OverlayTypeIndex != -1)
		{
			if (OverlayClass::GetTiberiumType(cell->OverlayTypeIndex) < 0)
			{
				occupiedNodes++;
				continue; // Blocked by a non-Tiberium overlay (mapper-placed barrier)
			}
		}

		size_t nearbyRefineries = 0;
		bool found = false;
		for (const auto pBuilding : BuildingClass::Array)
		{
			if (!pBuilding->IsAlive || pBuilding->InLimbo || !pBuilding->Type->ResourceDestination)
				continue;

			// Check if any existing AI refinery has been assigned for this expansion point yet.
			// If yes, consider it occupied, but only if it is ours.
			const auto buildingExt = BuildingExt::ExtMap.Find(pBuilding);
			if (pBuilding->Owner == pHouse && buildingExt->AssignedExpansionPoint == candidateCell)
			{
				found = true;
				break;
			}

			// Not all refineries have an assigned expansion point. For example, initial
			// base refineries and human players' refineries do not.
			// For these refineries, we rely on a distance check.
			const double dist = pBuilding->GetMapCoords().DistanceFrom(candidateCell);
			const double refineryRange = 22.0;
			if (pBuilding->Owner == pHouse && dist < refineryRange)
			{
				found = true;
				break;
			}

			if (pBuilding->Owner == pHouse && dist < 28.0)
				nearbyRefineries++;
		}

		// Count tiberium trees and ore cells within 20 cells of candidateCell
		int tiberiumTreeCount = 0;
		int tiberiumOreCellCount = 0;
		for (int dy = -20; dy <= 20; ++dy)
		{
			for (int dx = -20; dx <= 20; ++dx)
			{
				CellStruct scanCell(candidateCell.X + dx, candidateCell.Y + dy);
				if (!MapClass::Instance.CoordinatesLegal(scanCell))
					continue;

				if (candidateCell.DistanceFrom(scanCell) > 20.0)
					continue;

				const CellClass* cell = MapClass::Instance.GetCellAt(scanCell);
				if (cell)
				{
					const TerrainClass* pTerrain = cell->GetTerrain(false);
					if (pTerrain != nullptr && pTerrain->IsAlive && pTerrain->Type->SpawnsTiberium)
						tiberiumTreeCount++;

					if (cell->OverlayTypeIndex != -1)
					{
						if (const auto pOverlayType = OverlayTypeClass::Array.GetItem(cell->OverlayTypeIndex))
						{
							if (pOverlayType->Tiberium)
								tiberiumOreCellCount++;
						}
					}
				}
			}
		}

		int allowedRefineries = 1;
		if (tiberiumTreeCount > 0)
			allowedRefineries = std::min(tiberiumTreeCount, 3);
		else if (tiberiumOreCellCount > 0)
			allowedRefineries = std::min(1 + (tiberiumOreCellCount / 40), 3); // 1 refinery per 40 ore cells, up to 3

		if (found || nearbyRefineries >= allowedRefineries)
		{
			/*
			const int sIdx = isTree ? -1 : GetTiberiumSectorIndex(candidateCell);
			if (sIdx >= 0)
			{
				Debug::Log("AdvAI Search Ground Tiberium Zone Sector #%d at (%d,%d): skipped (occupied: found=%d, nearby=%d, allowed=%d, trees=%d)\n",
					sIdx, candidateCell.X, candidateCell.Y, found, static_cast<int>(nearbyRefineries), allowedRefineries, tiberiumTreeCount);
			}
			else
			{
				Debug::Log("AdvAI Search Tiberium Tree Node at (%d,%d): skipped (occupied: found=%d, nearby=%d, allowed=%d, trees=%d)\n",
					candidateCell.X, candidateCell.Y, found, static_cast<int>(nearbyRefineries), allowedRefineries, tiberiumTreeCount);
			}
			*/
			occupiedNodes++;
			continue; // Someone is already occupying this Tiberium cell/field
		}

		// Find the distance from our nearest owned structure to this candidate cell.
		double distanceFromNearestOwnedStructure = std::numeric_limits<double>::max();
		BuildingClass* pNearestBuilding = nullptr;
		int failedZonesCount = 0;
		int lastZoneBld = -1;
		int lastZoneCand = -1;

		struct BuildingDistance
		{
			BuildingClass* pBld;
			double Distance;
		};
		std::vector<BuildingDistance> buildingsByDist;

		for (const auto pBuilding : BuildingClass::Array)
		{
			if (!pBuilding->IsAlive || pBuilding->InLimbo || pBuilding->Owner != pHouse || pBuilding->Type->Naval)
				continue;

			double dist = pBuilding->GetMapCoords().DistanceFrom(candidateCell);
			buildingsByDist.push_back({ pBuilding, dist });
		}

		// Sort by distance ascending
		std::sort(buildingsByDist.begin(), buildingsByDist.end(), [](const BuildingDistance& a, const BuildingDistance& b) {
			return a.Distance < b.Distance;
		});

		// Find the closest reachable one (limit check to top 3 closest buildings to avoid A* spam)
		int checkedCount = 0;
		for (const auto& bldDist : buildingsByDist)
		{
			if (checkedCount >= 3)
				break;
			checkedCount++;

			if (GeneralUtils::AreZonesConnected(bldDist.pBld->GetMapCoords(), candidateCell))
			{
				distanceFromNearestOwnedStructure = bldDist.Distance;
				pNearestBuilding = bldDist.pBld;
				break;
			}
		}

		if (pNearestBuilding == nullptr)
		{
			// Debug::Log("AdvAI Search node (%d,%d): skipped (no nearest land building found)\n", candidateCell.X, candidateCell.Y);
			continue;
		}

		/*
		const int sIdx = isTree ? -1 : GetTiberiumSectorIndex(candidateCell);
		if (sIdx >= 0)
		{
			Debug::Log("AdvAI Search Ground Tiberium Zone Sector #%d at (%d,%d): reachable. Nearest structure: %s at dist %.1f cells\n",
				sIdx, candidateCell.X, candidateCell.Y, pNearestBuilding->Type->ID, distanceFromNearestOwnedStructure);
		}
		else
		{
			Debug::Log("AdvAI Search Tiberium Tree Node at (%d,%d): reachable. Nearest structure: %s at dist %.1f cells\n",
				candidateCell.X, candidateCell.Y, pNearestBuilding->Type->ID, distanceFromNearestOwnedStructure);
		}
		*/

		validList.push_back({ candidateCell, distanceFromNearestOwnedStructure, pNearestBuilding, isTree });
	}

	if (!validList.empty())
	{
		// Separate into trees and ground candidates
		std::vector<ValidCandidate> treeList;
		std::vector<ValidCandidate> groundList;
		for (const auto& cand : validList)
		{
			if (cand.IsTree)
				treeList.push_back(cand);
			else
				groundList.push_back(cand);
		}

		// Sort both lists by distance
		std::sort(treeList.begin(), treeList.end(), [](const ValidCandidate& a, const ValidCandidate& b) {
			return a.Distance < b.Distance;
		});
		std::sort(groundList.begin(), groundList.end(), [](const ValidCandidate& a, const ValidCandidate& b) {
			return a.Distance < b.Distance;
		});

		bool chooseTree = false;
		if (!treeList.empty())
		{
			if (groundList.empty())
			{
				chooseTree = true;
			}
			else
			{
				// 75% probability to prioritize trees
				int dice = ScenarioClass::Instance->Random.RandomRanged(0, 99);
				chooseTree = (dice < 75);
			}
		}

		const auto& chosenList = chooseTree ? treeList : groundList;
		size_t selectionSize = chooseTree ? std::min(chosenList.size(), size_t(3)) : 1;
		int randomIndex = chooseTree ? ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(selectionSize) - 1) : 0;
		const auto& chosen = chosenList[randomIndex];

		target = chosen.Coords;
		pBestNearestBuilding = chosen.NearestBuilding;
		nearestDistance = chosen.Distance;

		const int targetSectorIdx = chosen.IsTree ? -1 : GetTiberiumSectorIndex(target);
		if (targetSectorIdx >= 0)
		{
			Debug::Log("AdvAI ExpansionSearch: House %d: Ground Tiberium Zone Sector #%d at (%d,%d) chosen (75%% tree prioritization: %s). Selection size: %d. Nearest friendly structure: %s at dist %.1f cells.\n",
				pHouse->ArrayIndex,
				targetSectorIdx,
				target.X, target.Y,
				chooseTree ? "YES" : "NO",
				static_cast<int>(selectionSize),
				pBestNearestBuilding ? pBestNearestBuilding->Type->ID : "None",
				nearestDistance);
		}
		else
		{
			Debug::Log("AdvAI ExpansionSearch: House %d: Tiberium Tree Node at (%d,%d) chosen (75%% tree prioritization: %s). Selection size: %d. Nearest friendly structure: %s at dist %.1f cells.\n",
				pHouse->ArrayIndex,
				target.X, target.Y,
				chooseTree ? "YES" : "NO",
				static_cast<int>(selectionSize),
				pBestNearestBuilding ? pBestNearestBuilding->Type->ID : "None",
				nearestDistance);
		}
	}

	Debug::Log("AdvAI ExpansionSearch: House %d: Checked %d Tiberium nodes, %d occupied/blocked. Target: (%d,%d).\n",
		pHouse->ArrayIndex, totalNodesChecked, occupiedNodes, target.X, target.Y);

	if (target.X == 0 || target.Y == 0)
	{
		Debug::Log("AdvAI ExpansionSearch Safeguard: House %d: Scanning Tiberium trees on the map...\n", pHouse->ArrayIndex);
		nearestDistance = std::numeric_limits<double>::max();

		for (const auto pTerrain : TerrainClass::Array)
		{
			if (pTerrain && pTerrain->IsAlive && pTerrain->Type->SpawnsTiberium)
			{
				const CellStruct treeCoords = pTerrain->GetMapCoords();
				double nearestRefineryDist = std::numeric_limits<double>::max();
				const BuildingClass* pNearestRefinery = nullptr;

				for (const auto pBuilding : pHouse->Buildings)
				{
					if (pBuilding && pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Type->ResourceDestination)
					{
						const double dist = treeCoords.DistanceFrom(pBuilding->GetMapCoords());
						if (dist < nearestRefineryDist)
						{
							nearestRefineryDist = dist;
							pNearestRefinery = pBuilding;
						}
					}
				}

				const bool occupied = (nearestRefineryDist < 15.0);
				Debug::Log("AdvAI ExpansionSearch Safeguard: House %d: Tree %s at (%d,%d) -> Occupied: %s (Nearest friendly refinery: %s at (%d,%d), dist %.1f cells)\n",
					pHouse->ArrayIndex,
					pTerrain->Type->ID, treeCoords.X, treeCoords.Y,
					(occupied ? "YES" : "NO"),
					(pNearestRefinery ? pNearestRefinery->Type->ID : "None"),
					(pNearestRefinery ? pNearestRefinery->GetMapCoords().X : 0),
					(pNearestRefinery ? pNearestRefinery->GetMapCoords().Y : 0),
					nearestRefineryDist);

				if (!occupied)
				{
					// Find the nearest structure of ours to crawl from
					double minStructureDist = std::numeric_limits<double>::max();
					const BuildingClass* pNearestStructure = nullptr;
					int failedZonesCount = 0;
					int lastZoneBld = -1;
					int lastZoneTree = -1;

					for (const auto pBld : pHouse->Buildings)
					{
						if (pBld && pBld->IsAlive && !pBld->InLimbo)
						{
							// Avoid targeting resource nodes on other islands
							if (!GeneralUtils::AreZonesConnected(pBld->GetMapCoords(), treeCoords))
								continue;

							const double dist = treeCoords.DistanceFrom(pBld->GetMapCoords());
							if (dist < minStructureDist)
							{
								minStructureDist = dist;
								pNearestStructure = pBld;
							}
						}
					}

					if (pNearestStructure == nullptr)
					{
						Debug::Log("AdvAI ExpansionSearch Safeguard: House %d: Tree %s at (%d,%d) -> Unreachable (no buildings found)\n",
							pHouse->ArrayIndex, pTerrain->Type->ID, treeCoords.X, treeCoords.Y);
					}
					else
					{
						Debug::Log("AdvAI ExpansionSearch Safeguard: House %d: Tree %s at (%d,%d) -> Reachable. Nearest structure: %s at dist %.1f cells\n",
							pHouse->ArrayIndex, pTerrain->Type->ID, treeCoords.X, treeCoords.Y,
							pNearestStructure->Type->ID, minStructureDist);
					}

					if (minStructureDist < nearestDistance)
					{
						nearestDistance = minStructureDist;
						target = treeCoords;
						pBestNearestBuilding = pNearestStructure;
					}
				}
			}
		}

		if (target.X > 0 && target.Y > 0)
		{
			Debug::Log("AdvAI ExpansionSearch Safeguard: House %d: Recovered target (%d,%d) because it has no refinery within its radius.\n",
				pHouse->ArrayIndex, target.X, target.Y);
		}
	}

	if (target.X == 0 || target.Y == 0)
	{
		// If we couldn't find any Tiberium fields, crawl towards the enemy if they are within reach
		const HouseClass* pEnemy = nullptr;
		if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
		{
			pEnemy = HouseClass::Array[pHouse->EnemyHouseIndex];
		}

		if (pEnemy != nullptr)
		{
			double nearestEnemyDistSq = std::numeric_limits<double>::max();
			CellStruct enemyTarget = CellStruct(0, 0);

			for (const auto pBld : pEnemy->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo)
				{
					for (const auto pOurBld : pHouse->Buildings)
					{
						if (pOurBld && pOurBld->IsAlive && !pOurBld->InLimbo)
						{
							double distSq = pOurBld->GetMapCoords().DistanceFromSquared(pBld->GetMapCoords());


							if (distSq < nearestEnemyDistSq && GeneralUtils::AreZonesConnected(pOurBld->GetMapCoords(), pBld->GetMapCoords()))
							{
								nearestEnemyDistSq = distSq;
								enemyTarget = pBld->GetMapCoords();
							}
						}
					}
				}
			}

			Debug::Log("AdvAI: House %d: All Tiberium fields taken. Checking combat crawling. nearestEnemyDist=%.1f (Limit=%.1f), hasTarget=%s\n",
				pHouse->ArrayIndex,
				(nearestEnemyDistSq != std::numeric_limits<double>::max() ? sqrt(nearestEnemyDistSq) : -1.0),
				maxDist,
				(enemyTarget.X > 0 ? "YES" : "NO"));

			// If the enemy base is within crawling range
			if (nearestEnemyDistSq <= maxDistSq && enemyTarget.X > 0 && enemyTarget.Y > 0)
			{
				ext->CombatCrawlingTarget = enemyTarget;
				Debug::Log("AdvAI ExpansionSearch: House %d: All Tiberium fields taken. Crawling towards enemy House %d (%s) base at (%d,%d) at dist %.1f cells (Limit: %.1f).\n",
					pHouse->ArrayIndex, pEnemy->ArrayIndex, pEnemy->Type->ID, enemyTarget.X, enemyTarget.Y, std::sqrt(nearestEnemyDistSq), maxDist);
				foundAny = true;
			}
		}

		return foundAny;
	}

	if (needResourceTarget && target.X > 0 && target.Y > 0)
	{
		ext->ResourceCrawlingTarget = target;
		foundAny = true;
	}

	return foundAny;
}


bool HouseExt::AdvAI_Can_Build_Building(HouseClass* pHouse, BuildingTypeClass* pBuildingType, bool checkPrereqs, bool isTechTree)
{
	if (BuildingTypeClass::Array.FindItemIndex(pBuildingType) != pBuildingType->ArrayIndex ||
		pBuildingType->What_Am_I() != AbstractType::BuildingType)
	{
		Debug::FatalErrorAndExit("Invalid BuildingTypeClass pointer in AdvAI_Can_Build_Building!!!");
	}

	// Check if this building type is currently on placement failure cooldown.
	// This prevents the AI queue from getting locked by repeatedly building structures it cannot place.
	const auto houseExt = ExtMap.Find(pHouse);
	auto it = houseExt->PlacementFailedCooldowns.find(pBuildingType);
	if (it != houseExt->PlacementFailedCooldowns.end())
	{
		if (Unsorted::CurrentFrame < it->second)
		{
			return false;
		}
	}

	// Debug::Log("Checking if AI %d can build %s. ", house->ArrayIndex, int->Name);

	if (!isTechTree && !pBuildingType->AIBuildThis)
		return false;

	if (pBuildingType->Unbuildable)
		return false;

	const auto pExt = BuildingTypeExt::ExtMap.Find(pBuildingType);

	if (!(pExt->PrerequisiteTheaters & (1 << static_cast<int>(ScenarioClass::Instance->Theater))))
		return false;

	// This should be expanded to support Ares
	if (pBuildingType->RequiresStolenAlliedTech && !pHouse->Side0TechInfiltrated ||
		pBuildingType->RequiresStolenSovietTech && !pHouse->Side1TechInfiltrated ||
		pBuildingType->RequiresStolenThirdTech && !pHouse->Side2TechInfiltrated)
	{
		return false;
	}

	if (!pHouse->CanExpectToBuild(pBuildingType))
		return false;

	if (pBuildingType->TechLevel > pHouse->TechLevel || (pBuildingType->TechLevel < 0 && pBuildingType->TechLevel != -1))
		return false;

	// Per-session build limit
	if (pBuildingType->BuildLimit < 0 &&
		pHouse->FactoryProducedBuildingTypes.GetItemCount(pBuildingType->ArrayIndex) >= -pBuildingType->BuildLimit)
	{
		return false;
	}

	// Normal build limit: BuildLimit=0 means no limit in RA2 convention.
	if (pBuildingType->BuildLimit > 0 &&
		pHouse->ActiveBuildingTypes.GetItemCount(pBuildingType->ArrayIndex) >= pBuildingType->BuildLimit)
	{
		return false;
	}


	if (!GameModeOptionsClass::Instance.SWAllowed)
	{
		if (BuildingTypeExt::HasDisableableSuperWeapons(pBuildingType))
		{
			// Debug::Log("Result: false (SuperWeapon)\n");
			return false;
		}
	}

	if (!checkPrereqs || pExt->IsAdvancedAIIgnoresPrerequisites)
		goto prereqsChecked;

	// Prerequisite.Negatives from BuildingTypeExt (Ares vanilla negatives)
	for (const auto pPrerequisiteNegative : pExt->PrerequisiteNegatives)
	{
		if (pHouse->ActiveBuildingTypes.GetItemCount(pPrerequisiteNegative->ArrayIndex) > 0)
			return false;
	}

	// Full prerequisite evaluation using Phobos extended system (supports GenericPrerequisites, Lists, etc.)
	if (!PrerequisitesMet(pHouse, pBuildingType))
		return false;

	prereqsChecked:


	// If this is an upgrade, do we have a building we could upgrade with it?
	if (!pExt->PowersUp_Buildings.empty())
	{
		// Enforce AI powerplant upgrade restrictions:
		// Check if the upgrade type's target base buildings are powerplants
		bool isPowerUpgrade = false;
		for (auto const pPowerUpBld : pExt->PowersUp_Buildings)
		{
			if (pPowerUpBld != nullptr && (TechTreeTypeClass::TotalBuildPower.count(pPowerUpBld) > 0 || TechTreeTypeClass::TotalBuildAdvancedPower.count(pPowerUpBld) > 0))
			{
				isPowerUpgrade = true;
				break;
			}
		}

		if (isPowerUpgrade)
		{
			const int surplusPower = pHouse->PowerOutput - pHouse->PowerDrain;
			const int requiredSurplus = pHouse->PowerSurplus > 0 ? pHouse->PowerSurplus : RulesClass::Instance->PowerSurplus;
			if (surplusPower >= requiredSurplus)
			{
				// Check if there is at least one upgradeable powerplant in the main base
				const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
				const CellStruct conyardCell = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pHouse->Base_Center();

				bool hasUpgradeableInBase = false;
				for (const auto pBld : BuildingClass::Array)
				{
					if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner == pHouse)
					{
						if (pExt->PowersUp_Buildings.Contains(pBld->Type))
						{
							if (pBld->UpgradeLevel < pBld->Type->Upgrades)
							{
								const double bldDist = pBld->GetMapCoords().DistanceFrom(conyardCell);
								if (bldDist < 20.0)
								{
									hasUpgradeableInBase = true;
									break;
								}
							}
						}
					}
				}

				// Only block building upgrades if there are NO upgradeable powerplants in the main base
				if (!hasUpgradeableInBase)
				{
					return false; // Block!
				}
			}
		}

		bool anyBaseExists = false;
		for (const auto base : pExt->PowersUp_Buildings)
		{
			if (pHouse->ActiveBuildingTypes.GetItemCount(base->ArrayIndex) > 0)
			{
				anyBaseExists = true;
				break;
			}
		}

		if (!anyBaseExists)
		{
			return false;
		}

		bool foundUpgradeable = false;

		// Scan through the buildings...
		for (const auto pBuilding : BuildingClass::Array)
		{
			if (!pBuilding->IsAlive ||
				pBuilding->InLimbo ||
				pBuilding->Owner != pHouse)
			{
				continue;
			}

			if (pExt->PowersUp_Buildings.Contains(pBuilding->Type))
			{
				if (pBuilding->UpgradeLevel < pBuilding->Type->Upgrades)
				{
					foundUpgradeable = true;
					break;
				}
			}
		}

		if (!foundUpgradeable)
		{
			return false;
		}
	}

	if (pBuildingType->Naval)
	{
		const int adjacency = RulesClass::Instance->AINavalYardAdjacency;
		const RectangleStruct baseArea = BuildingExt::Get_Base_Rect(pHouse, adjacency, pBuildingType->GetFoundationWidth(), pBuildingType->GetFoundationHeight(false));

		bool canPlaceAnywhere = false;
		const int resCells = 2000;
		const int areaSize = baseArea.Width * baseArea.Height;
		const int resolution = 1 + (areaSize / resCells);

		for (int y = baseArea.Y; y < baseArea.Y + baseArea.Height; y += resolution)
		{
			for (int x = baseArea.X; x < baseArea.X + baseArea.Width; x += resolution)
			{
				CellStruct cell = CellStruct(x, y);
				if (MapClass::Instance.CoordinatesLegal(cell) && pBuildingType->CanPlaceHere(&cell, pHouse))
				{
					canPlaceAnywhere = true;
					break;
				}
			}
			if (canPlaceAnywhere) break;
		}

		if (!canPlaceAnywhere)
		{
			return false;
		}
	}
	else if (pExt->PowersUp_Buildings.empty())
	{
		// Feasibility Skip: for non-upgrade, non-naval ground structures,
		// check if there is at least one valid cell in the base layout.
		BuildingExt::PopulateAdjacencyAnchors(pHouse, pBuildingType);

		const int adjacency = pBuildingType->Adjacent;
		RectangleStruct baseArea;
		if (pBuildingType->Refinery || pBuildingType->ResourceDestination)
		{
			baseArea = BuildingExt::GetRefinerySearchRect(pHouse, pBuildingType);
		}
		else
		{
			baseArea = BuildingExt::Get_Base_Rect(pHouse, adjacency, pBuildingType->GetFoundationWidth(), pBuildingType->GetFoundationHeight(false));
		}

		bool canPlaceAnywhere = false;
		const int resCells = 2000;
		const int areaSize = baseArea.Width * baseArea.Height;
		const int resolution = 1 + (areaSize / resCells);

		for (int y = baseArea.Y; y < baseArea.Y + baseArea.Height; y += resolution)
		{
			for (int x = baseArea.X; x < baseArea.X + baseArea.Width; x += resolution)
			{
				CellStruct cell = CellStruct(x, y);
				if (MapClass::Instance.CoordinatesLegal(cell) &&
					BuildingExt::Should_Evaluate_Cell_For_Placement(cell, pBuildingType, pHouse, 0) &&
					pBuildingType->CanPlaceHere(&cell, pHouse))
				{
					canPlaceAnywhere = true;
					break;
				}
			}
			if (canPlaceAnywhere) break;
		}

		if (!canPlaceAnywhere)
		{
			return false;
		}
	}

	// Debug::Log("Result: true\n");
	return true;
}


bool HouseExt::AdvAI_Is_Recently_Attacked(HouseClass* pHouse)
{
	return pHouse->LATime + TICKS_PER_MINUTE > Unsorted::CurrentFrame;
}


/**
 *  Checks if AdvAI is under threat of being start rushed.
 *  Start rushes require specific tactics to counter.
 *
 *  Author: Rampastring
 */
bool HouseExt::AdvAI_Is_Under_Start_Rush_Threat(HouseClass* pHouse, int enemyAircraftValue)
{
	// If the game has progressed for long enough, it is no longer considered a start rush.
	if (Unsorted::CurrentFrame > 10000)
	{
		return false;
	}

	if (enemyAircraftValue > 0 || AdvAI_Is_Recently_Attacked(pHouse))
	{
		return true;
	}

	// Counter infantry rushing. If a human enemy has more infantry than we do, we are at risk.

	static int houseInfantryStrength[10] = {};

	// Go through all infantry on the map and gather infantry strength of all enemy human houses.
	for (const auto pInfantry : InfantryClass::Array)
	{
		if (pInfantry->InLimbo)
		{
			continue;
		}

		// Also calculate our own infantry strength for comparison.
		if (pInfantry->Owner == pHouse)
		{
			houseInfantryStrength[pHouse->ArrayIndex] += pInfantry->Type->Points;
			continue;
		}

		if (pInfantry->Owner->Type->MultiplayPassive)
		{
			continue;
		}

		if (!pInfantry->Owner->IsControlledByHuman())
		{
			continue;
		}

		if (pInfantry->Owner->IsAlliedWith(pHouse))
		{
			continue;
		}

		if (pInfantry->Owner->ArrayIndex >= std::size(houseInfantryStrength))
		{
			continue;
		}

		// Humans can typically micromanage better than the AI, so increase points for human infantry.
		houseInfantryStrength[pInfantry->Owner->ArrayIndex] += pInfantry->Type->Points * 2;
	}

	const int ourInfantryStrength = houseInfantryStrength[pHouse->ArrayIndex];
	for (const int i : houseInfantryStrength)
	{
		if (i > ourInfantryStrength)
		{
			return true;
		}
	}

	return false;
}


/**
 *  Calculates the total number of enemy aircraft in the game.
 *
 *  Author: Rampastring
 */
int HouseExt::AdvAI_Calculate_Enemy_Aircraft_Value(HouseClass* pHouse)
{
	int enemyAircraftValue = 0;

	for (const auto pOtherHouse : HouseClass::Array)
	{
		if (pOtherHouse->IsAlliedWith(pHouse) || pOtherHouse->Type->MultiplayPassive)
			continue;

		enemyAircraftValue += pOtherHouse->ActiveAircraftTypes.GetTotal() * 10;

		for (const auto pUnitType : UnitTypeClass::Array)
		{
			// Count vehicles that fly, are jumpjets, or spawn something that flies
			if (pUnitType->MovementZone == MovementZone::Fly || pUnitType->Spawns != nullptr || pUnitType->ConsideredAircraft || pUnitType->JumpJet)
				enemyAircraftValue += pOtherHouse->ActiveUnitTypes.GetItemCount(pUnitType->ArrayIndex) * 5;
		}

		for (const auto pInfantryType : InfantryTypeClass::Array)
		{
			// Same for infantry, including jumpjets (Rocketeers)
			if (pInfantryType->MovementZone == MovementZone::Fly || pInfantryType->Spawns != nullptr || pInfantryType->ConsideredAircraft || pInfantryType->JumpJet)
				enemyAircraftValue += pOtherHouse->ActiveInfantryTypes.GetItemCount(pInfantryType->ArrayIndex) * 4;
		}
	}

	return enemyAircraftValue;
}


/**
 *  Gets the building that the Advanced AI should build in its current game situation.
 *
 *  Author: Rampastring
 */

enum class ThreatCategory { None, Infantry, Vehicle, Air };

static ThreatCategory GetActiveThreatCategory(HouseClass* pHouse, CellStruct threatCenter)
{
	if (threatCenter.X == 0 && threatCenter.Y == 0)
		return ThreatCategory::None;

	int enemyInfantryCount = 0;
	int enemyVehicleCount = 0;
	int enemyAirCount = 0;

	auto CheckThreat = [&](TechnoClass* pObj) {
		if (pObj && pObj->IsAlive && !pObj->InLimbo && pObj->Owner != pHouse && !pHouse->IsAlliedWith(pObj->Owner))
		{
			if (pObj->GetMapCoords().DistanceFrom(threatCenter) <= 15.0)
			{
				const auto pType = pObj->GetTechnoType();
				if (pType && (pType->JumpJet || pObj->WhatAmI() == AbstractType::Aircraft))
				{
					enemyAirCount++;
				}
				else if (pObj->WhatAmI() == AbstractType::Unit)
				{
					enemyVehicleCount++;
				}
				else if (pObj->WhatAmI() == AbstractType::Infantry)
				{
					enemyInfantryCount++;
				}
			}
		}
	};

	for (const auto pInf : InfantryClass::Array)
		CheckThreat(pInf);

	for (const auto pUnit : UnitClass::Array)
		CheckThreat(pUnit);

	for (const auto pAir : AircraftClass::Array)
		CheckThreat(pAir);

	// Treat enemy structures (defenses, conyards) as armored ground threats (Vehicles)
	for (const auto pBld : BuildingClass::Array)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner))
		{
			if (pBld->GetMapCoords().DistanceFrom(threatCenter) <= 15.0)
			{
				enemyVehicleCount++;
			}
		}
	}

	if (enemyAirCount > 0 && enemyAirCount >= enemyVehicleCount && enemyAirCount >= enemyInfantryCount)
		return ThreatCategory::Air;
	if (enemyVehicleCount > 0 && enemyVehicleCount >= enemyInfantryCount)
		return ThreatCategory::Vehicle;
	if (enemyInfantryCount > 0)
		return ThreatCategory::Infantry;

	return ThreatCategory::None;
}

const BuildingTypeClass* HouseExt::AdvAI_Evaluate_Get_Best_Building(HouseClass* pHouse)
{
	constexpr bool LogVerboseAdvAI = false;

	auto canBuildFunction = [pHouse](auto&& PH1)
	{
		return AdvAI_Can_Build_Building(pHouse, std::forward<decltype(PH1)>(PH1), true, true);
	};

	const auto houseExt = ExtMap.Find(pHouse);

	auto GetTargetBuildCount = [&](BuildingTypeClass* pType, int defaultCount, TechTreeTypeClass* pTechTree) -> int {
		// If BuildOtherCounts in the TechTree explicitly specifies a count for this building,
		// that takes priority and overrides AIBuildCounts/AIExtraCounts.
		int buildOtherIndex = -1;
		if (pTechTree != nullptr)
		{
			for (size_t i = 0; i < pTechTree->BuildOther.size(); ++i)
			{
				if (pTechTree->BuildOther[i] == pType)
				{
					buildOtherIndex = static_cast<int>(i);
					break;
				}
			}
		}

		if (pTechTree != nullptr && buildOtherIndex >= 0 && buildOtherIndex < static_cast<int>(pTechTree->BuildOtherCounts.size()))
		{
			return pTechTree->BuildOtherCounts[buildOtherIndex];
		}

		const auto pTypeExt = BuildingTypeExt::ExtMap.Find(pType);
		const unsigned int difficulty = pHouse->GetAIDifficultyIndex(); // Hard=0, Normal=1, Easy=2

		if (pTypeExt->AIBuildCounts.size() > difficulty)
		{
			auto it = houseExt->AICachedBuildCounts.find(pType);
			if (it != houseExt->AICachedBuildCounts.end())
			{
				return it->second;
			}

			int baseCount = pTypeExt->AIBuildCounts[difficulty];
			int extraCount = 0;
			if (pTypeExt->AIExtraCounts.size() > difficulty)
			{
				int extraMax = pTypeExt->AIExtraCounts[difficulty];
				if (extraMax > 0)
				{
					extraCount = ScenarioClass::Instance->Random.RandomRanged(0, extraMax);
				}
			}

			int totalCount = baseCount + extraCount;
			houseExt->AICachedBuildCounts[pType] = totalCount;
			return totalCount;
		}

		return defaultCount;
	};

	TechTreeTypeClass* pPrimaryTechTree = houseExt->PrimaryTechTreeType;
	TechTreeTypeClass* pSecondaryTechTree = houseExt->SecondaryTechTreeType;

	// Initialize tech trees, should be moved elsewhere
	if (pPrimaryTechTree == nullptr || pSecondaryTechTree == nullptr)
	{
		pPrimaryTechTree = TechTreeTypeClass::GetForSide(houseExt->OwnerObject()->Type->SideIndex);
		pSecondaryTechTree = TechTreeTypeClass::GetForSide(houseExt->OwnerObject()->Type->SideIndex);
		houseExt->PrimaryTechTreeType = pPrimaryTechTree;
		houseExt->SecondaryTechTreeType = pPrimaryTechTree;
	}

	// If we can't build using our primary tech tree, choose another one. If there is none, we shouldn't be here (no ConYard).
	if (!pPrimaryTechTree->IsSuitable(pHouse))
	{
		pPrimaryTechTree = TechTreeTypeClass::GetAnySuitable(pHouse);
		if (pPrimaryTechTree == nullptr)
		{
			Debug::Log("AdvAI: Could not find a suitable tech tree for AI %d\n", pHouse->ArrayIndex);
			return nullptr;
		}
		houseExt->PrimaryTechTreeType = pPrimaryTechTree;
	}

	// If we can't build using our secondary tech tree, reset it to the primary one
	if (!pSecondaryTechTree->IsSuitable(pHouse))
	{
		pSecondaryTechTree = pPrimaryTechTree;
	}

	// If we're done with our secondary tree, attempt to find another one
	if (pSecondaryTechTree->IsCompleted(pHouse, canBuildFunction))
	{
		for (const auto& pTechTree : TechTreeTypeClass::Array)
		{
			if (pTechTree->IsSuitable(pHouse) && !pTechTree->IsCompleted(pHouse, canBuildFunction))
			{
				pSecondaryTechTree = pTechTree.get();
				break;
			}
		}
	}

	/// Primary tech tree
	///	Handles the main expansion
	{
		const bool limitFactories = pPrimaryTechTree == nullptr || pPrimaryTechTree->LimitedFactories;
		const bool hasTechCenter = TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildTech) >= 1;

		// If we have no power plants yet, then build one
		const BuildingTypeClass* pPowerPlantToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildPower, 1, 1);
		if (pPowerPlantToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it has 0 basic power plants\n", pPowerPlantToBuild->Name);
			return pPowerPlantToBuild;
		}

		// On Medium and Hard, build a barracks if we do not have any yet
		if (pHouse->AIDifficulty < AIDifficulty::Hard && pHouse->Balance >= RulesClass::Instance->AIAlternateProductionCreditCutoff)
		{
			const BuildingTypeClass* pBarracksToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildBarracks, 1, 1);
			if (pBarracksToBuild != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because it does not have a Barracks at all.\n", pBarracksToBuild->Name);
				return pBarracksToBuild;
			}
		}

		// Check how much air power our opponents have.
		const int enemyAircraftValue = AdvAI_Calculate_Enemy_Aircraft_Value(pHouse);

		// Check whether we're in threat of being rushed right in the beginning of the game.
		const bool isUnderThreat = AdvAI_Is_Under_Start_Rush_Threat(pHouse, enemyAircraftValue);
		houseExt->IsUnderStartRushThreat = isUnderThreat;

		// Build refinery if we're expanding and we're not under immediate air rush threat
		if (!isUnderThreat)
		{
			const BuildingTypeClass* pOurRefinery = pPrimaryTechTree->GetRandomBuildable(TechTreeTypeClass::BuildType::BuildRefinery, canBuildFunction);
			if (houseExt->ShouldBuildRefinery)
			{
				bool hasPlacement = false;
				if (pOurRefinery != nullptr)
				{
					CellStruct placementCell = BuildingExt::Get_Best_Expansion_Placement_Position_Helper(pHouse, const_cast<BuildingTypeClass*>(pOurRefinery), nullptr);
					if (placementCell.X > 0 && placementCell.Y > 0)
					{
						hasPlacement = true;
					}
				}

				if (hasPlacement && pOurRefinery != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s because it has reached an expansion point\n", pOurRefinery->Name);
					return pOurRefinery;
				}
				else
				{
					const CellStruct targetCell = houseExt->NextExpansionPointLocation;

					// Check if we are combat crawling (target is an enemy building)
					bool isCombatCrawling = false;
					if (const auto pTargetCell = MapClass::Instance.TryGetCellAt(targetCell))
					{
						if (const auto pTargetBld = pTargetCell->GetBuilding())
						{
							if (pTargetBld->Owner != pHouse && !pHouse->IsAlliedWith(pTargetBld->Owner))
							{
								isCombatCrawling = true;
							}
						}
					}

					if (!isCombatCrawling)
					{
						Debug::Log("AdvAI: House %d reached expansion point (%d,%d), but no valid refinery placement could be found near it. Blacklisting target and marking expansion as done.\n",
							pHouse->ArrayIndex, targetCell.X, targetCell.Y);
						AdvAI_Add_Failed_Expansion_Point(pHouse, targetCell);
						BuildingExt::Mark_Expansion_As_Done(pHouse);
					}
					else
					{
						Debug::Log("AdvAI: House %d reached expansion point (%d,%d), but refinery is physically unbuildable here. Combat crawling in progress: reverting ShouldBuildRefinery to crawl further.\n",
							pHouse->ArrayIndex, targetCell.X, targetCell.Y);
					}

					houseExt->ShouldBuildRefinery = false;
				}
			}
		}

		// Count all refineries, including vehicle slave miners
		const int slaveMinerCount = RulesClass::Instance->PrerequisiteProcAlternate != nullptr ?
			pHouse->ActiveUnitTypes.GetItemCount(RulesClass::Instance->PrerequisiteProcAlternate->ArrayIndex) : 0;
		size_t refineryCount = houseExt->PrimaryTechTreeType->CountSideOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildRefinery);
		refineryCount += slaveMinerCount;

		// Build a refinery if we have 0 left. Can't use the generic function as we need to also count slave miners
		const BuildingTypeClass* pRefineryToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildRefinery, 0, 1, slaveMinerCount);
		if (pRefineryToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it has 0 refineries\n", pRefineryToBuild->Name);
			return pRefineryToBuild;
		}

		const size_t powerPlantCount = TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildPower) +
			TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildAdvancedPower) * 4;

		// Build power if necessary
		bool hasUnpoweredBuildings = false;
		if (!pHouse->PowerBlackoutTimer.HasTimeLeft())
		{
			for (const auto pBuilding : BuildingClass::Array)
			{
				if (pBuilding->Owner == pHouse && pBuilding->IsAlive && !pBuilding->InLimbo && !pBuilding->IsUnderEMP() && !pBuilding->HasPower)
				{
					hasUnpoweredBuildings = true;
					break;
				}
			}
		}

		/*for (const auto pBuilding : BuildingClass::Array)
		{
			if (pBuilding->Owner == pHouse && !pBuilding->IsPowerOnline())
			{
				hasUnpoweredBuildings = true;
				break;
			}
		}*/

		if (!isUnderThreat && Unsorted::CurrentFrame > 5000 && (pHouse->PowerOutput - GetRealPowerDrain(pHouse) < 100 || hasUnpoweredBuildings))
		{
			const BuildingTypeClass* pOurAdvancedPowerPlant = pPrimaryTechTree->GetRandomBuildable(TechTreeTypeClass::BuildType::BuildAdvancedPower, canBuildFunction);
			if (pOurAdvancedPowerPlant != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because it is out of power and can build an adv. power plant\n", pOurAdvancedPowerPlant->Name);
				return pOurAdvancedPowerPlant;
			}

			const BuildingTypeClass* pOurPowerPlant = pPrimaryTechTree->GetRandomBuildable(TechTreeTypeClass::BuildType::BuildPower, canBuildFunction);
			if (pOurPowerPlant != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because it is out of power and can only build a basic power plant\n", pOurPowerPlant->Name);
				return pOurPowerPlant;
			}
		}

		// If we don't have enough barracks, then build one
		int ourBarracksCount = 0;
		for (const auto pBuilding : BuildingClass::Array)
		{
			if (pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Owner == pHouse)
			{
				if (pBuilding->Type->Factory == AbstractType::InfantryType)
				{
					ourBarracksCount++;
				}
			}
		}

		int maxBarracksOwnedByOther = 0;
		for (const auto pOtherHouse : HouseClass::Array)
		{
			if (pOtherHouse == pHouse || pHouse->IsAlliedWith(pOtherHouse))
				continue;

			int otherBarracksCount = 0;
			for (const auto pBuilding : BuildingClass::Array)
			{
				if (pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Owner == pOtherHouse)
				{
					if (pBuilding->Type->Factory == AbstractType::InfantryType)
					{
						otherBarracksCount++;
					}
				}
			}

			if (otherBarracksCount > maxBarracksOwnedByOther)
			{
				maxBarracksOwnedByOther = otherBarracksCount;
			}
		}

		size_t optimalBarracksCount = 1 + (refineryCount / 3);
		if (!hasTechCenter)
			optimalBarracksCount = 1;

		// Competitive scaling: if a competitor has at least double our barracks count, increase our limit
		if (maxBarracksOwnedByOther >= ourBarracksCount * 2 && ourBarracksCount > 0)
		{
			optimalBarracksCount = std::max(optimalBarracksCount, static_cast<size_t>(ourBarracksCount + 1));
		}

		// Enforce difficulty-based safety cap for barracks (Easy: 6, Normal: 8, Hard: 12)
		// Reduced in naval mode (Easy: 2, Normal: 3, Hard: 4) to save space on islands
		bool isNavalMode = RulesExt::Global()->AdvancedAI_NavalMode;
		size_t maxBarracksLimit = 8;
		if (isNavalMode)
		{
			maxBarracksLimit = 3;
			if (pHouse->AIDifficulty == AIDifficulty::Easy)
				maxBarracksLimit = 2;
			else if (pHouse->AIDifficulty == AIDifficulty::Hard)
				maxBarracksLimit = 4;
		}
		else
		{
			if (pHouse->AIDifficulty == AIDifficulty::Easy)
				maxBarracksLimit = 6;
			else if (pHouse->AIDifficulty == AIDifficulty::Hard)
				maxBarracksLimit = 12;
		}

		if (limitFactories && optimalBarracksCount > maxBarracksLimit)
			optimalBarracksCount = maxBarracksLimit;

		AdvAI_Recycle_Furthest_Factory(pHouse, AbstractType::InfantryType, false, optimalBarracksCount, houseExt->NextExpansionPointLocation);

		const BuildingTypeClass* pBarracksToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildBarracks, 1, optimalBarracksCount);

		if (pBarracksToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it does not have enough Barracks. Wanted: %d (Competitor max: %d)\n",
				pBarracksToBuild->Name, optimalBarracksCount, maxBarracksOwnedByOther);

			return pBarracksToBuild;
		}

		// Get defenses and calculate for deficiencies on them before making further decisions.
		BuildingTypeClass* ourAntiInfantryDefense = nullptr;
		BuildingTypeClass* ourAntiVehicleDefense = nullptr;
		BuildingTypeClass* ourAntiAirDefense = nullptr;

		double bestAntiInfantryScore = -1.0;
		double bestAntiVehicleScore = -1.0;
		double bestAntiAirScore = -1.0;

		if (LogVerboseAdvAI)
			Debug::Log("AdvAI Eval: House %d (%s), TechTree: %s, Raw BuildDefense count in INI: %u\n", pHouse->ArrayIndex, pHouse->Type->ID, pPrimaryTechTree->Name.data(), pPrimaryTechTree->BuildDefense.size());
		for (const auto pDefType : pPrimaryTechTree->BuildDefense)
		{
			bool canBuild = AdvAI_Can_Build_Building(pHouse, pDefType, true, true);
			if (LogVerboseAdvAI)
			{
				Debug::Log("AdvAI Eval: House %d checking Defense INI item %s -> CanBuild: %s (AIBuildThis=%d, TechLvl=%d vs HouseLvl=%d, CanExpectToBuild=%d)\n",
					pHouse->ArrayIndex, pDefType ? pDefType->ID : "NULL", canBuild ? "YES" : "NO",
					pDefType ? pDefType->AIBuildThis : 0,
					pDefType ? pDefType->TechLevel : -1,
					pHouse->TechLevel,
					pDefType ? pHouse->CanExpectToBuild(pDefType) : 0);
			}
		}

		auto buildableDefenses = pPrimaryTechTree->GetBuildable(TechTreeTypeClass::BuildType::BuildDefense, canBuildFunction);
		if (LogVerboseAdvAI)
			Debug::Log("AdvAI Eval: House %d (%s), TechTree: %s, BuildableDefenses count: %u\n", pHouse->ArrayIndex, pHouse->Type->ID, pPrimaryTechTree->Name.data(), buildableDefenses.size());

		int paranoiaDuration = TICKS_PER_MINUTE;

		bool hasEnemiesClose = false;
		{
			const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
			CellStruct baseCenter = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pHouse->Base_Center();
			const double checkDistSq = 30.0 * 30.0;

			for (const auto pFoot : FootClass::Array)
			{
				if (pFoot && pFoot->IsAlive && !pFoot->InLimbo && pFoot->Owner != pHouse && !pFoot->Owner->IsNeutral() && !pHouse->IsAlliedWith(pFoot->Owner))
				{
					if (baseCenter.DistanceFromSquared(pFoot->GetMapCoords()) <= checkDistSq)
					{
						hasEnemiesClose = true;
						break;
					}
				}
			}

			if (!hasEnemiesClose)
			{
				for (const auto pBld : BuildingClass::Array)
				{
					if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner != pHouse && !pBld->Owner->IsNeutral() && !pHouse->IsAlliedWith(pBld->Owner))
					{
						const auto& primary = pBld->Type->GetWeapon(0, false);
						const auto& secondary = pBld->Type->GetWeapon(1, false);

						if (primary.WeaponType != nullptr || secondary.WeaponType != nullptr)
						{
							if (baseCenter.DistanceFromSquared(pBld->GetMapCoords()) <= checkDistSq)
							{
								hasEnemiesClose = true;
								break;
							}
						}
					}
				}
			}
		}

		const bool wasRecentlyAttacked = pHouse->LATime + paranoiaDuration > Unsorted::CurrentFrame;
		const bool isParanoid = (isUnderThreat && hasEnemiesClose) || wasRecentlyAttacked;

		bool hasSomethingToProtect = false;
		for (const auto pBld : pHouse->Buildings)
		{
			if (pBld && pBld->Type && pBld->Type->ToProtect)
			{
				bool isProtected = false;
				for (const auto pOther : pHouse->Buildings)
				{
					if (pOther && pOther->IsAlive && !pOther->InLimbo && pOther != pBld)
					{
						if (TechTreeTypeClass::TotalBuildDefense.contains(pOther->Type))
						{
							if (pBld->GetMapCoords().DistanceFromSquared(pOther->GetMapCoords()) < 49.0)
							{
								isProtected = true;
								break;
							}
						}
					}
				}

				if (!isProtected)
				{
					hasSomethingToProtect = true;
					break;
				}
			}
		}

		bool hasUndefendedExpansionStructure = false;
		if (!hasSomethingToProtect)
		{
			const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
			if (pOurConYard != nullptr)
			{
				for (const auto pBld : pHouse->Buildings)
				{
					if (pBld && pBld->Type && (pBld->Type->Refinery || pBld->Type->ResourceDestination || pBld->Type->PowerBonus > 0))
					{
						if (pBld->GetMapCoords().DistanceFromSquared(pOurConYard->GetMapCoords()) >= 400.0)
						{
							bool isProtected = false;
							for (const auto pOther : pHouse->Buildings)
							{
								if (pOther && pOther->IsAlive && !pOther->InLimbo && pOther != pBld)
								{
									if (TechTreeTypeClass::TotalBuildDefense.contains(pOther->Type))
									{
										if (pBld->GetMapCoords().DistanceFromSquared(pOther->GetMapCoords()) < 225.0)
										{
											isProtected = true;
											break;
										}
									}
								}
							}

							if (!isProtected)
							{
								hasSomethingToProtect = true;
								hasUndefendedExpansionStructure = true;
								break;
							}
						}
					}
				}
			}
		}

		for (const auto pDefense : buildableDefenses)
		{
			if (!isParanoid && !hasSomethingToProtect && houseExt->NextExpansionPointLocation.X <= 0)
				continue;

			double antiInfantryScore = pDefense->AntiInfantryValue;
			double antiVehicleScore = pDefense->AntiArmorValue;
			double antiAirScore = pDefense->AntiAirValue;

			// 1. Power constraint check:
			// If building this defense will cause a low power state (net power < 0),
			// apply a severe penalty to its score, unless we have no other choice.
			if (pDefense->PowerDrain > 0)
			{
				int powerSurplus = pHouse->PowerOutput - GetRealPowerDrain(pHouse);
				if (powerSurplus < pDefense->PowerDrain)
				{
					// Applying a 99% penalty to the score if it would overload our power grid.
					antiInfantryScore *= 0.01;
					antiVehicleScore *= 0.01;
					antiAirScore *= 0.01;
				}
			}

			// 2. Budget constraint check:
			// If we are low on money, prioritize cheaper defenses (cost-effective).
			// If we are rich, we can afford expensive, powerful defenses (high absolute value).
			int currentMoney = pHouse->Available_Money();
			if (currentMoney < pDefense->Cost)
			{
				// We can't even afford it right now, penalize it heavily so we choose something buildable.
				antiInfantryScore *= 0.01;
				antiVehicleScore *= 0.01;
				antiAirScore *= 0.01;
			}
			else if (currentMoney < 2000)
			{
				// Budget is tight: scale score by cost efficiency (value / cost)
				// We normalize by a baseline cost (e.g. 500) to keep the magnitude comparable.
				double costFactor = 500.0 / pDefense->Cost;
				antiInfantryScore *= costFactor;
				antiVehicleScore *= costFactor;
				antiAirScore *= costFactor;
			}

			if (LogVerboseAdvAI)
			{
				Debug::Log("AdvAI Eval: House %d Defense Option %s -> Cost: %d, PowerDrain: %d, InfVal: %d (Score: %.4f), ArmVal: %d (Score: %.4f), AirVal: %d (Score: %.4f)\n",
					pHouse->ArrayIndex, pDefense->ID, pDefense->Cost, pDefense->PowerDrain, pDefense->AntiInfantryValue, antiInfantryScore, pDefense->AntiArmorValue, antiVehicleScore, pDefense->AntiAirValue, antiAirScore);
			}

			if (antiInfantryScore > bestAntiInfantryScore)
			{
				bestAntiInfantryScore = antiInfantryScore;
				ourAntiInfantryDefense = pDefense;
			}

			if (antiVehicleScore > bestAntiVehicleScore)
			{
				bestAntiVehicleScore = antiVehicleScore;
				ourAntiVehicleDefense = pDefense;
			}

			if (antiAirScore > bestAntiAirScore)
			{
				bestAntiAirScore = antiAirScore;
				ourAntiAirDefense = pDefense;
			}
		}

		int antiInfantryDefenseValue = 0;
		int antiVehicleDefenseValue = 0;
		int antiAirDefenseValue = 0;

		for (const auto pDefense : TechTreeTypeClass::TotalBuildDefense)
		{
			antiInfantryDefenseValue += pHouse->ActiveBuildingTypes.GetItemCount(pDefense->ArrayIndex) * pDefense->AntiInfantryValue;
			antiVehicleDefenseValue += pHouse->ActiveBuildingTypes.GetItemCount(pDefense->ArrayIndex) * pDefense->AntiArmorValue;
			antiAirDefenseValue += pHouse->ActiveBuildingTypes.GetItemCount(pDefense->ArrayIndex) * pDefense->AntiAirValue;
		}

		int optimalDefenseValue = refineryCount + powerPlantCount / 4;
		if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
			optimalDefenseValue += 5;

		// Special check for early infantry rushes.
		// If we are getting infantry-rushed, build more anti-infantry defenses.
		if (isUnderThreat && enemyAircraftValue == 0)
		{
			if (hasEnemiesClose)
				optimalDefenseValue *= 3;
			else
				optimalDefenseValue = (optimalDefenseValue * 3) / 2;
		}

		// If we are under attack, prioritize defense.
		if (wasRecentlyAttacked)
			optimalDefenseValue += 2;

		// Scale this to match the values used in vanilla in Rules.
		optimalDefenseValue *= 15;

		// Check which type of defense is most desperately needed.
		int antiInfDeficiency = 0;
		int antiVehicleDeficiency = 0;
		int antiAirDeficiency = 0;

		if (ourAntiInfantryDefense != nullptr)
		{
			antiInfDeficiency = optimalDefenseValue - antiInfantryDefenseValue;
		}

		if (ourAntiVehicleDefense != nullptr)
		{
			antiVehicleDeficiency = optimalDefenseValue - antiVehicleDefenseValue;
		}

		if (ourAntiAirDefense != nullptr)
		{
			// Dynamically scale AA deficiency against enemy aircraft/jumpjets without an artificial cap
			antiAirDeficiency = enemyAircraftValue - antiAirDefenseValue;
		}

		if (LogVerboseAdvAI)
		{
			Debug::Log("AdvAI Eval: House %d Totals -> OptVal: %d, InfVal: %d (Defic: %d), ArmVal: %d (Defic: %d), AirVal: %d (Defic: %d). BestCandidates -> Inf: %s (Own: %d, Limit: %d), Arm: %s (Own: %d, Limit: %d), Air: %s (Own: %d, Limit: %d)\n",
				pHouse->ArrayIndex, optimalDefenseValue, antiInfantryDefenseValue, antiInfDeficiency, antiVehicleDefenseValue, antiVehicleDeficiency, antiAirDefenseValue, antiAirDeficiency,
				ourAntiInfantryDefense ? ourAntiInfantryDefense->ID : "None",
				ourAntiInfantryDefense ? pHouse->ActiveBuildingTypes.GetItemCount(ourAntiInfantryDefense->ArrayIndex) : 0,
				ourAntiInfantryDefense ? ourAntiInfantryDefense->BuildLimit : 0,
				ourAntiVehicleDefense ? ourAntiVehicleDefense->ID : "None",
				ourAntiVehicleDefense ? pHouse->ActiveBuildingTypes.GetItemCount(ourAntiVehicleDefense->ArrayIndex) : 0,
				ourAntiVehicleDefense ? ourAntiVehicleDefense->BuildLimit : 0,
				ourAntiAirDefense ? ourAntiAirDefense->ID : "None",
				ourAntiAirDefense ? pHouse->ActiveBuildingTypes.GetItemCount(ourAntiAirDefense->ArrayIndex) : 0,
				ourAntiAirDefense ? ourAntiAirDefense->BuildLimit : 0);
		}

		// Prioritize defense construction if paranoid or if we have undefended nodes to protect!
		if (isParanoid || hasSomethingToProtect)
		{
			const int rollChance = isParanoid ? 85 : 70;
			if (ScenarioClass::Instance->Random.RandomRanged(0, 99) < rollChance)
			{
				int maxDeficiency = 0;
				const BuildingTypeClass* pBestDefense = nullptr;

				// Analyze the local active threat type to choose the correct counter defense
				CellStruct threatCenter = CellStruct::Empty;
				if (houseExt->LastAttackerCoords.X > 0 && houseExt->LastAttackerCoords.Y > 0)
				{
					threatCenter = houseExt->LastAttackerCoords;
				}
				else if (houseExt->FrontlineThreatCoords.X > 0 && houseExt->FrontlineThreatCoords.Y > 0)
				{
					threatCenter = houseExt->FrontlineThreatCoords;
				}

				ThreatCategory threat = GetActiveThreatCategory(pHouse, threatCenter);
				if (threat == ThreatCategory::Air && ourAntiAirDefense != nullptr)
				{
					pBestDefense = ourAntiAirDefense;
				}
				else if (threat == ThreatCategory::Vehicle && ourAntiVehicleDefense != nullptr)
				{
					pBestDefense = ourAntiVehicleDefense;
				}
				else if (threat == ThreatCategory::Infantry && ourAntiInfantryDefense != nullptr)
				{
					pBestDefense = ourAntiInfantryDefense;
				}
				else if (hasUndefendedExpansionStructure && ourAntiInfantryDefense != nullptr)
				{
					pBestDefense = ourAntiInfantryDefense;
				}
				else
				{
					// Fallback to deficiency calculations
					if (antiInfDeficiency > maxDeficiency && ourAntiInfantryDefense != nullptr)
					{
						maxDeficiency = antiInfDeficiency;
						pBestDefense = ourAntiInfantryDefense;
					}

					if (antiVehicleDeficiency > maxDeficiency && ourAntiVehicleDefense != nullptr)
					{
						maxDeficiency = antiVehicleDeficiency;
						pBestDefense = ourAntiVehicleDefense;
					}

					if (antiAirDeficiency > maxDeficiency && ourAntiAirDefense != nullptr)
					{
						maxDeficiency = antiAirDeficiency;
						pBestDefense = ourAntiAirDefense;
					}
				}

				if (pBestDefense != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s (highest deficiency, paranoid/protecting). InfDef: %d, VehDef: %d, AirDef: %d\n",
						pBestDefense->Name, antiInfDeficiency, antiVehicleDeficiency, antiAirDeficiency);
					return pBestDefense;
				}
			}
		}

		// If we are under threat of an immediate early-game rush, then skip the WF and refinery minimums.
		// Instead build defenses or tech up so we can get AA ASAP.
		if (!isUnderThreat || (antiInfDeficiency <= 0 && antiAirDeficiency <= 0))
		{
bool isNavalMode = RulesExt::Global()->AdvancedAI_NavalMode;

			auto evaluateWeaponsFactory = [&]() -> const BuildingTypeClass* {
				// If we don't have enough weapons factories, then build one.
				int ourWFCount = 0;
				for (const auto pBuilding : BuildingClass::Array)
				{
					if (pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Owner == pHouse)
					{
						if (pBuilding->Type->Factory == AbstractType::UnitType && !pBuilding->Type->Naval)
						{
							ourWFCount++;
						}
					}
				}

				int maxWFOwnedByOther = 0;
				for (const auto pOtherHouse : HouseClass::Array)
				{
					if (pOtherHouse == pHouse || pHouse->IsAlliedWith(pOtherHouse))
						continue;

					int otherWFCount = 0;
					for (const auto pBuilding : BuildingClass::Array)
					{
						if (pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Owner == pOtherHouse)
						{
							if (pBuilding->Type->Factory == AbstractType::UnitType && !pBuilding->Type->Naval)
							{
								otherWFCount++;
							}
						}
					}

					if (otherWFCount > maxWFOwnedByOther)
					{
						maxWFOwnedByOther = otherWFCount;
					}
				}

				size_t optimalWeaponsCount = 1 + (refineryCount / 3);
				if (pHouse->AIDifficulty == AIDifficulty::Easy)
					optimalWeaponsCount = 1 + (refineryCount / 4);
				else if (refineryCount >= 6)
					optimalWeaponsCount = refineryCount / 2;

				if (!hasTechCenter)
					optimalWeaponsCount = 1;

				// Competitive scaling: if a competitor has at least double our War Factory count, increase our limit
				if (maxWFOwnedByOther >= ourWFCount * 2 && ourWFCount > 0)
				{
					optimalWeaponsCount = std::max(optimalWeaponsCount, static_cast<size_t>(ourWFCount + 1));
				}

				// Enforce difficulty-based safety cap for War Factories (Easy: 6, Normal: 8, Hard: 12)
				// Reduced in naval mode (Easy: 2, Normal: 3, Hard: 4) to save space on islands
				size_t maxWFLimit = 8;
				if (isNavalMode)
				{
					maxWFLimit = 3;
					if (pHouse->AIDifficulty == AIDifficulty::Easy)
						maxWFLimit = 2;
					else if (pHouse->AIDifficulty == AIDifficulty::Hard)
						maxWFLimit = 4;
				}
				else
				{
					if (pHouse->AIDifficulty == AIDifficulty::Easy)
						maxWFLimit = 6;
					else if (pHouse->AIDifficulty == AIDifficulty::Hard)
						maxWFLimit = 12;
				}

				if (limitFactories && optimalWeaponsCount > maxWFLimit)
					optimalWeaponsCount = maxWFLimit;

				AdvAI_Recycle_Furthest_Factory(pHouse, AbstractType::UnitType, false, optimalWeaponsCount, houseExt->NextExpansionPointLocation);

				const BuildingTypeClass* pWeaponsFactoryToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildWeapons, 1, optimalWeaponsCount);

				if (pWeaponsFactoryToBuild != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s because it does not have enough Weapons Factories. Wanted: %d (Competitor max: %d)\n",
						pWeaponsFactoryToBuild->Name, optimalWeaponsCount, maxWFOwnedByOther);

					return pWeaponsFactoryToBuild;
				}
				return nullptr;
			};

			auto evaluateNavalYards = [&]() -> const BuildingTypeClass* {
				// Find the number of Naval Yards owned by the current enemy
				int enemyNavalYardsCount = 0;
				HouseClass* pEnemy = nullptr;
				if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
				{
					pEnemy = HouseClass::Array[pHouse->EnemyHouseIndex];
				}
				if (pEnemy == nullptr)
				{
					pEnemy = BuildingExt::Find_Closest_Opponent(pHouse);
				}

				if (pEnemy != nullptr)
				{
					for (const auto pBuilding : BuildingClass::Array)
					{
						if (pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Owner == pEnemy)
						{
							if (pBuilding->Type->Factory == AbstractType::UnitType && pBuilding->Type->Naval)
							{
								enemyNavalYardsCount++;
							}
						}
					}
				}

				size_t optimalNavalYardCount = 1;
				if (isNavalMode)
				{
					if (refineryCount >= 6)
						optimalNavalYardCount = 4;
					else if (refineryCount >= 3)
						optimalNavalYardCount = 3;
					else
						optimalNavalYardCount = 2;
				}
				else
				{
					if (refineryCount >= 6)
						optimalNavalYardCount = 3;
					else if (refineryCount >= 3)
						optimalNavalYardCount = 2;
				}

				if (!hasTechCenter)
					optimalNavalYardCount = 1;

				// Scale up to match competitors if they build more
				if (static_cast<size_t>(enemyNavalYardsCount) > optimalNavalYardCount)
				{
					optimalNavalYardCount = static_cast<size_t>(enemyNavalYardsCount);
				}

				// Apply difficulty-based safety cap for Naval Yards (Easy: 3, Normal: 5, Hard: 8)
				// Under naval mode, the cap increases to (Easy: 4, Normal: 6, Hard: 9)
				// And can dynamically scale up to the enemy's shipyard count - 1
				size_t maxNavalYardLimit = 5;
				if (isNavalMode)
				{
					maxNavalYardLimit = 6;
					if (pHouse->AIDifficulty == AIDifficulty::Easy)
						maxNavalYardLimit = 4;
					else if (pHouse->AIDifficulty == AIDifficulty::Hard)
						maxNavalYardLimit = 9;

					if (enemyNavalYardsCount > 0 && pHouse->AIDifficulty == AIDifficulty::Hard)
					{
						maxNavalYardLimit = std::max(maxNavalYardLimit, static_cast<size_t>(enemyNavalYardsCount - 1));
					}
				}
				else
				{
					if (pHouse->AIDifficulty == AIDifficulty::Easy)
						maxNavalYardLimit = 3;
					else if (pHouse->AIDifficulty == AIDifficulty::Hard)
						maxNavalYardLimit = 8;
				}

				if (limitFactories && optimalNavalYardCount > maxNavalYardLimit)
					optimalNavalYardCount = maxNavalYardLimit;

				AdvAI_Recycle_Furthest_Factory(pHouse, AbstractType::UnitType, true, optimalNavalYardCount, houseExt->NextExpansionPointLocation);

				const BuildingTypeClass* pNavalYardToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildNavalYard, 1, optimalNavalYardCount);
				if (pNavalYardToBuild != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s because it does not have enough Naval Yards. Wanted: %d (Enemy max: %d)\n",
						pNavalYardToBuild->Name, optimalNavalYardCount, enemyNavalYardsCount);

					return pNavalYardToBuild;
				}
				return nullptr;
			};

			if (isNavalMode)
			{
				const BuildingTypeClass* pBld = evaluateNavalYards();
				if (pBld != nullptr)
					return pBld;

				pBld = evaluateWeaponsFactory();
				if (pBld != nullptr)
					return pBld;
			}
			else
			{
				const BuildingTypeClass* pBld = evaluateWeaponsFactory();
				if (pBld != nullptr)
					return pBld;

				pBld = evaluateNavalYards();
				if (pBld != nullptr)
					return pBld;
			}

			// If we have too few refineries, build enough to match the minimum.
			// Because this is not for expanding but an emergency situation,
			// cancel any potential expanding.
			int minRefineryCount = RulesExt::Global()->AdvancedAI_MinimumRefineryCount;
			if (!hasTechCenter)
				minRefineryCount = std::min(minRefineryCount, 2);

			pRefineryToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildRefinery, 1, minRefineryCount, slaveMinerCount);
			if (pRefineryToBuild != nullptr)
			{
				houseExt->NextExpansionPointLocation = CellStruct(0, 0);
				houseExt->ShouldBuildRefinery = false;
				Debug::Log("AdvAI: Making AI build %s because it only has too few refineries\n", pRefineryToBuild->Name);
				return pRefineryToBuild;
			}
		}

		// Probabilistic roll: 70% chance to build defense when paranoid (threat/attack), 50% chance in normal state.
		const int rollChance = isParanoid ? 70 : 50;
		bool shouldBuildDefenseThisCycle = (ScenarioClass::Instance->Random.RandomRanged(0, 99) < rollChance);
		if (houseExt->FrontlineThreatCoords.X > 0 && houseExt->FrontlineThreatActiveFrames > Unsorted::CurrentFrame && houseExt->FrontlineThreatNeedsDefenses > 0)
		{
			shouldBuildDefenseThisCycle = true;
		}

		if (shouldBuildDefenseThisCycle)
		{
			int maxDeficiency = 0;
			const BuildingTypeClass* pBestDefense = nullptr;

			if (antiInfDeficiency > maxDeficiency && ourAntiInfantryDefense != nullptr)
			{
				maxDeficiency = antiInfDeficiency;
				pBestDefense = ourAntiInfantryDefense;
			}

			if (antiVehicleDeficiency > maxDeficiency && ourAntiVehicleDefense != nullptr)
			{
				maxDeficiency = antiVehicleDeficiency;
				pBestDefense = ourAntiVehicleDefense;
			}

			if (antiAirDeficiency > maxDeficiency && ourAntiAirDefense != nullptr)
			{
				maxDeficiency = antiAirDeficiency;
				pBestDefense = ourAntiAirDefense;
			}

			if (pBestDefense != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s (highest deficiency). InfDef: %d, VehDef: %d, AirDef: %d\n",
					pBestDefense->Name, antiInfDeficiency, antiVehicleDeficiency, antiAirDeficiency);
				return pBestDefense;
			}

			// Fallback: If forced by frontline threat but all global deficiencies are 0
			if (houseExt->FrontlineThreatNeedsDefenses > 0)
			{
				if (enemyAircraftValue > 0 && ourAntiAirDefense != nullptr)
				{
					Debug::Log("AdvAI: Forced local defense build fallback: Choosing %s (Anti-Air) due to enemy airborne threat.\n", ourAntiAirDefense->Name);
					return ourAntiAirDefense;
				}
				else if (ourAntiVehicleDefense != nullptr)
				{
					Debug::Log("AdvAI: Forced local defense build fallback: Choosing %s (Anti-Vehicle).\n", ourAntiVehicleDefense->Name);
					return ourAntiVehicleDefense;
				}
				else if (ourAntiInfantryDefense != nullptr)
				{
					Debug::Log("AdvAI: Forced local defense build fallback: Choosing %s (Anti-Infantry).\n", ourAntiInfantryDefense->Name);
					return ourAntiInfantryDefense;
				}
				else if (!buildableDefenses.empty())
				{
					const BuildingTypeClass* pRandomDefense = buildableDefenses[ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(buildableDefenses.size()) - 1)];
					Debug::Log("AdvAI: Forced local defense build fallback: Choosing random buildable defense %s.\n", pRandomDefense->Name);
					return pRandomDefense;
				}
			}
		}
		else if (antiInfDeficiency > 0 || antiVehicleDeficiency > 0 || antiAirDeficiency > 0)
		{
			if (LogVerboseAdvAI)
			{
				Debug::Log("AdvAI: House %d deferred defense construction this cycle due to probability roll (%d%%), continuing base expansion.\n",
					pHouse->ArrayIndex, 100 - rollChance);
			}
		}
	// If we have no radar, then build one
		const BuildingTypeClass* pRadarToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildRadar, 1, 1);
		if (pRadarToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it does not have a radar.\n",
				pRadarToBuild->Name);

			return pRadarToBuild;
		}

		// If we have no tech center, then build one
		const BuildingTypeClass* pTechCenterToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildTech, 1, 1);
		if (pTechCenterToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it does not have a tech center.\n",
				pTechCenterToBuild->Name);

			return pTechCenterToBuild;
		}

		// If we don't have enough helipads/airfields (based on total aircraft docks needed), then build one
		const BuildingTypeClass* pHelipadType = pPrimaryTechTree->GetRandomBuildable(TechTreeTypeClass::BuildType::BuildHelipad, canBuildFunction);
		if (pHelipadType != nullptr)
		{
			const int docksPerHelipad = pHelipadType->NumberOfDocks > 0 ? pHelipadType->NumberOfDocks : 1;

			int totalCurrentDocks = 0;
			int totalHelipadsOwned = 0;
			for (const auto pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo)
				{
					if (pBld->Type->Helipad)
					{
						const int docks = pBld->Type->NumberOfDocks > 0 ? pBld->Type->NumberOfDocks : 1;
						totalCurrentDocks += docks;
						totalHelipadsOwned++;
					}
				}
			}

			// Count all airport-bound aircraft currently owned by this house (including those flying or attacking)
			int totalAircraft = 0;
			for (const auto pAircraft : AircraftClass::Array)
			{
				if (pAircraft && pAircraft->IsAlive && !pAircraft->InLimbo && pAircraft->Owner == pHouse)
				{
					if (pAircraft->Type->AirportBound)
					{
						totalAircraft++;
					}
				}
			}

			// Also count actual occupied docks just in case some custom planes are docked but not marked AirportBound
			int totalOccupiedDocks = 0;
			for (const auto pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type->Helipad)
				{
					totalOccupiedDocks += BuildingExt::CountOccupiedDocks(pBld);
				}
			}

			if (totalOccupiedDocks > totalAircraft)
			{
				totalAircraft = totalOccupiedDocks;
			}

			bool isNavalMode = RulesExt::Global()->AdvancedAI_NavalMode;

			// Find the number of helipad docks owned by the current enemy
			int enemyHelipadDocks = 0;
			HouseClass* pEnemyHelipad = nullptr;
			if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
			{
				pEnemyHelipad = HouseClass::Array[pHouse->EnemyHouseIndex];
			}
			if (pEnemyHelipad == nullptr)
			{
				pEnemyHelipad = BuildingExt::Find_Closest_Opponent(pHouse);
			}

			if (pEnemyHelipad != nullptr)
			{
				for (const auto pBuilding : BuildingClass::Array)
				{
					if (pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Owner == pEnemyHelipad)
					{
						if (pBuilding->Type->Helipad)
						{
							enemyHelipadDocks += pBuilding->Type->NumberOfDocks > 0 ? pBuilding->Type->NumberOfDocks : 1;
						}
					}
				}
			}

			int minInitialDocks = 8;
			if (pHouse->AIDifficulty == AIDifficulty::Easy)
				minInitialDocks = 2;
			else if (pHouse->AIDifficulty == AIDifficulty::Normal)
				minInitialDocks = 6;

			if (isNavalMode)
			{
				minInitialDocks = 10;
				if (pHouse->AIDifficulty == AIDifficulty::Easy)
					minInitialDocks = 6;
				else if (pHouse->AIDifficulty == AIDifficulty::Normal)
					minInitialDocks = 8;
			}

			size_t optimalHelipadCount = 1;
			if (totalCurrentDocks < minInitialDocks)
			{
				optimalHelipadCount = totalHelipadsOwned + 1;
			}
			else
			{
				optimalHelipadCount = (totalAircraft >= totalCurrentDocks) ? totalHelipadsOwned + 1 : totalHelipadsOwned;
			}

			// Competitive scaling for helipads under naval mode (non-Easy difficulties)
			if (isNavalMode && pHouse->AIDifficulty != AIDifficulty::Easy)
			{
				const size_t targetHelipadsToMatchEnemyDocks = (static_cast<size_t>(enemyHelipadDocks) + docksPerHelipad - 1) / docksPerHelipad;
				if (targetHelipadsToMatchEnemyDocks > optimalHelipadCount)
				{
					optimalHelipadCount = targetHelipadsToMatchEnemyDocks;
				}
			}

			bool limitHelipadFactories = limitFactories && !isNavalMode;
			if (limitHelipadFactories)
			{
				// Enforce difficulty-based safety cap of docks (Easy: 8 docks, Normal: 12 docks, Hard: 16 docks)
				int maxDocks = 16;
				if (pHouse->AIDifficulty == AIDifficulty::Easy)
					maxDocks = 8;
				else if (pHouse->AIDifficulty == AIDifficulty::Normal)
					maxDocks = 12;

				const size_t maxHelipadCount = (maxDocks + docksPerHelipad - 1) / docksPerHelipad;
				if (optimalHelipadCount > maxHelipadCount)
					optimalHelipadCount = maxHelipadCount;
			}

			const BuildingTypeClass* pHelipadToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildHelipad, 1, optimalHelipadCount);
			if (pHelipadToBuild != nullptr && IsBuildingTypeQueued(pHouse, TechTreeTypeClass::BuildType::BuildHelipad))
			{
				pHelipadToBuild = nullptr;
			}

			if (pHelipadToBuild != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because it has no free aircraft docks (Total Docks: %d, Total Aircraft: %d, Wanted helipads: %d)\n",
					pHelipadToBuild->Name, totalCurrentDocks, totalAircraft, optimalHelipadCount);

				return pHelipadToBuild;
			}
		}

		// BuildSupport network evaluation.
		// 90% chance to skip (10% entry chance) to avoid starving other tech decisions.
		const bool skipSupport = ScenarioClass::Instance->Random.RandomRanged(0, 99) < 90;

		if (!skipSupport)
		{
			const auto canBuildSupportFunction = [pHouse](BuildingTypeClass* pType)
			{
				return AdvAI_Can_Build_Building(pHouse, pType, true, true);
			};

			const auto buildableSupportTypes = pPrimaryTechTree->GetBuildable(TechTreeTypeClass::BuildType::BuildSupport, canBuildSupportFunction);

			if (!buildableSupportTypes.empty())
			{
				std::vector<BuildingTypeClass*> neededSupportCandidates;

				for (const auto pSupportType : buildableSupportTypes)
				{
					const char* groupID = GetGroupAsID(const_cast<BuildingTypeClass*>(pSupportType));
					auto it = houseExt->GroupPlacementCooldowns.find(groupID);
					if (it != houseExt->GroupPlacementCooldowns.end())
					{
						if (Unsorted::CurrentFrame < it->second)
						{
							continue; // Excluded from candidates due to active progressive cooldown
						}
					}

					const SupportRadiusType supportType = GetSupportRadiusType(pSupportType);
					const int radius = GetSupportRadius(pSupportType);
					
					int coverageDistance = radius > 1 ? radius - 1 : 3;
					int targetSeparation = 8;
					if (supportType == SupportRadiusType::EMPulseCannon)
					{
						coverageDistance = radius > 1 ? radius - 1 : 29;
						targetSeparation = radius > 1 ? radius : 30;
					}
					else if (radius > 1)
					{
						targetSeparation = static_cast<int>(radius * 1.8);
					}

					const int targetBuildCount = GetTargetBuildCount(const_cast<BuildingTypeClass*>(pSupportType), -1, pPrimaryTechTree);
					const int ownedThisSpecificType = CountBuildingOfGroup(pHouse, const_cast<BuildingTypeClass*>(pSupportType));

					bool respectLimit = false;
					int limit = 0;
					if (targetBuildCount >= 0)
					{
						respectLimit = true;
						limit = targetBuildCount;
					}
					else if (pSupportType->BuildLimit > 0)
					{
						respectLimit = true;
						limit = pSupportType->BuildLimit;
					}

					if (respectLimit && ownedThisSpecificType >= limit)
						continue;

					// Check if we are already building this specific support structure type (or an equivalent one under GroupAs)
					if (pHouse->ProducingBuildingTypeIndex != -1)
					{
						BuildingTypeClass* pProducingType = BuildingTypeClass::Array[pHouse->ProducingBuildingTypeIndex];
						if (_stricmp(GetGroupAsID(pProducingType), GetGroupAsID(pSupportType)) == 0)
							continue;
					}

					// Check coverage: does any functional base structure lack this support coverage?
					bool needMoreSupport = false;

					if (ownedThisSpecificType == 0)
					{
						needMoreSupport = true;
					}
					else
					{
						for (const auto pBld : pHouse->Buildings)
						{
							if (pBld && pBld->IsAlive && !pBld->InLimbo)
							{
								const bool isBaseBuilding = pBld->Type->ConstructionYard ||
									pBld->Type->Factory != AbstractType::None ||
									pBld->Type->Refinery ||
									pBld->Type->Radar ||
									pBld->Type->Helipad ||
									(pBld->Type->TechLevel > 0 && !pBld->Type->IsBaseDefense && GetSupportRadiusType(pBld->Type) == SupportRadiusType::None);

								if (!isBaseBuilding)
									continue;

								bool covered = false;
								for (const auto pOtherBld : pHouse->Buildings)
								{
									if (pOtherBld && pOtherBld->IsAlive && !pOtherBld->InLimbo &&
										pOtherBld != pBld &&
										IsSameSupportNetwork(pOtherBld->Type, pSupportType) &&
										pBld->GetMapCoords().DistanceFrom(pOtherBld->GetMapCoords()) <= coverageDistance)
									{
										covered = true;
										break;
									}
								}

								if (!covered)
								{
									needMoreSupport = true;
									break;
								}
							}
						}
					}

					if (needMoreSupport)
						neededSupportCandidates.push_back(pSupportType);
				}

				if (!neededSupportCandidates.empty())
				{
					BuildingTypeClass* pChosenSupport = neededSupportCandidates[
						ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(neededSupportCandidates.size()) - 1)];

					Debug::Log("AdvAI: Making AI build %s for support coverage (uncovered base buildings detected).\n",
						pChosenSupport->Name);

					return pChosenSupport;
				}
			}
		}



		// PreBuildOtherRandom evaluation
		const auto buildablePreRandomTypes = pPrimaryTechTree->GetBuildable(TechTreeTypeClass::BuildType::PreBuildOtherRandom, canBuildFunction);
		if (!buildablePreRandomTypes.empty())
		{
			// 50% skip chance per cycle
			const bool skipPreRandom = ScenarioClass::Instance->Random.RandomRanged(0, 99) < 50;
			if (!skipPreRandom)
			{
				std::vector<BuildingTypeClass*> preRandomCandidates;
				for (const auto pBldType : buildablePreRandomTypes)
				{
					const int targetCount = GetTargetBuildCount(pBldType, -1, pPrimaryTechTree);
					if (pHouse->ActiveBuildingTypes.GetItemCount(pBldType->ArrayIndex) < targetCount)
					{
						preRandomCandidates.push_back(pBldType);
					}
				}

				if (!preRandomCandidates.empty())
				{
					BuildingTypeClass* pChosen = preRandomCandidates[
						ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(preRandomCandidates.size()) - 1)];

					Debug::Log("AdvAI: Making AI build %s from PreBuildOtherRandom.\n",
						pChosen->Name);

					return pChosen;
				}
			}
		}

		for (const auto buildOtherPair : pPrimaryTechTree->BuildOtherCountMap)
		{
			if (!AdvAI_Can_Build_Building(pHouse, buildOtherPair.first, true))
			{
				continue;
			}

			const int targetCount = GetTargetBuildCount(buildOtherPair.first, buildOtherPair.second, pPrimaryTechTree);
			if (pHouse->ActiveBuildingTypes.GetItemCount(buildOtherPair.first->ArrayIndex) < targetCount)
			{
				Debug::Log("AdvAI: Making AI build %s because it does not have enough of it. Wanted: %d\n",
					buildOtherPair.first->Name, targetCount);

				return buildOtherPair.first;
			}
		}

		// Are there other AIBuildThis=yes buildings that we haven't built yet?
		for (const auto pBuilding : BuildingTypeClass::Array)
		{
			// Exclude defenses here, no need to build defenses just to have them
			if (TechTreeTypeClass::TotalBuildDefense.contains(pBuilding))
			{
				continue;
			}

			// Exclude helipads here, as they are handled dynamically based on occupied docks
			if (pBuilding->Helipad)
			{
				continue;
			}

			if (pBuilding->AIBasePlanningSide == pPrimaryTechTree->SideIndex && AdvAI_Can_Build_Building(pHouse, pBuilding, true))
			{
				if (pHouse->ActiveBuildingTypes.GetItemCount(pBuilding->ArrayIndex) < 1)
				{
					// Special case for the slave miner to count its undeployed form
					if (RulesClass::Instance->PrerequisiteProcAlternate != nullptr &&
						pBuilding->UndeploysInto == RulesClass::Instance->PrerequisiteProcAlternate &&
						pHouse->ActiveUnitTypes.GetItemCount(RulesClass::Instance->PrerequisiteProcAlternate->ArrayIndex) > 0)
						continue;

					Debug::Log("AdvAI: Making AI build %s because it has AIBuildThis=yes and the AI has none.\n",
						pBuilding->Name);
					return pBuilding;
				}
			}
		}

		// PostBuildOtherRandom evaluation
		const auto buildablePostRandomTypes = pPrimaryTechTree->GetBuildable(TechTreeTypeClass::BuildType::PostBuildOtherRandom, canBuildFunction);
		if (!buildablePostRandomTypes.empty())
		{
			std::vector<BuildingTypeClass*> postRandomCandidates;
			for (const auto pBldType : buildablePostRandomTypes)
			{
				const int targetCount = GetTargetBuildCount(pBldType, -1, pPrimaryTechTree);
				if (pHouse->ActiveBuildingTypes.GetItemCount(pBldType->ArrayIndex) < targetCount)
				{
					postRandomCandidates.push_back(pBldType);
				}
			}

			if (!postRandomCandidates.empty())
			{
				BuildingTypeClass* pChosen = postRandomCandidates[
					ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(postRandomCandidates.size()) - 1)];

				Debug::Log("AdvAI: Making AI build %s from PostBuildOtherRandom.\n",
					pChosen->Name);

				return pChosen;
			}
		}

		// Build Service Depot if we have War Factories but not enough Service Depots (1 initial + 1 per 4 refineries)
		const size_t ourRefineryCount = pPrimaryTechTree->CountSideOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildRefinery);
		const size_t optimalDepotCount = 1 + (ourRefineryCount / 4);

		if (Unsorted::CurrentFrame > houseExt->LastServiceDepotPlacementFailedFrame + 3000)
		{
			const BuildingTypeClass* pDepotToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildServiceDepot, 1, optimalDepotCount);
			if (pDepotToBuild != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s (Refinery count: %d, Max depots: %d).\n",
					pDepotToBuild->Name, static_cast<int>(ourRefineryCount), static_cast<int>(optimalDepotCount));
				return pDepotToBuild;
			}
		}


	}

	/// Secondary tech tree
	///	Make the AI build other tech if it isn't too busy
	{
		const BuildingTypeClass* pPowerPlantToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pSecondaryTechTree, TechTreeTypeClass::BuildType::BuildPower, 1, 0);
		if (pPowerPlantToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it is currently developing its tech tree for side %d and does not have a basic power plant.\n", pPowerPlantToBuild->Name, pSecondaryTechTree->SideIndex.Get());
			return pPowerPlantToBuild;
		}

		const BuildingTypeClass* pBarracksToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pSecondaryTechTree, TechTreeTypeClass::BuildType::BuildBarracks, 1, 0);
		if (pBarracksToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it is currently developing its tech tree for side %d and does not have a barracks.\n", pBarracksToBuild->Name, pSecondaryTechTree->SideIndex.Get());
			return pBarracksToBuild;
		}

		const BuildingTypeClass* pRefineryToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pSecondaryTechTree, TechTreeTypeClass::BuildType::BuildRefinery, 1, 0);
		if (pRefineryToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it is currently developing its tech tree for side %d and does not have a refinery.\n", pRefineryToBuild->Name, pSecondaryTechTree->SideIndex.Get());
			return pRefineryToBuild;
		}

		const BuildingTypeClass* pWeaponsToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pSecondaryTechTree, TechTreeTypeClass::BuildType::BuildWeapons, 1, 0);
		if (pWeaponsToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it is currently developing its tech tree for side %d and does not have a weapons factory.\n", pWeaponsToBuild->Name, pSecondaryTechTree->SideIndex.Get());
			return pWeaponsToBuild;
		}

		const BuildingTypeClass* pNavalYardToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pSecondaryTechTree, TechTreeTypeClass::BuildType::BuildNavalYard, 1, 0);
		if (pNavalYardToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it is currently developing its tech tree for side %d and does not have a naval yard.\n", pNavalYardToBuild->Name, pSecondaryTechTree->SideIndex.Get());
			return pNavalYardToBuild;
		}

		const BuildingTypeClass* pRadarToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pSecondaryTechTree, TechTreeTypeClass::BuildType::BuildRadar, 1, 0);
		if (pRadarToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it is currently developing its tech tree for side %d and does not have a radar.\n", pRadarToBuild->Name, pSecondaryTechTree->SideIndex.Get());
			return pRadarToBuild;
		}

		const BuildingTypeClass* pHelipadToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pSecondaryTechTree, TechTreeTypeClass::BuildType::BuildHelipad, 1, 0);
		if (pHelipadToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it is currently developing its tech tree for side %d and does not have a helipad.\n", pHelipadToBuild->Name, pSecondaryTechTree->SideIndex.Get());
			return pHelipadToBuild;
		}

		const BuildingTypeClass* pTechCenterToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pSecondaryTechTree, TechTreeTypeClass::BuildType::BuildTech, 1, 0);
		if (pTechCenterToBuild != nullptr)
		{
			Debug::Log("AdvAI: Making AI build %s because it is currently developing its tech tree for side %d and does not have a tech center.\n", pTechCenterToBuild->Name, pSecondaryTechTree->SideIndex.Get());
			return pTechCenterToBuild;
		}

		for (const auto buildOtherPair : pSecondaryTechTree->BuildOtherCountMap)
		{
			if (!AdvAI_Can_Build_Building(pHouse, buildOtherPair.first, true))
			{
				continue;
			}

			const int targetCount = GetTargetBuildCount(buildOtherPair.first, buildOtherPair.second, pSecondaryTechTree);
			if (pHouse->ActiveBuildingTypes.GetItemCount(buildOtherPair.first->ArrayIndex) < targetCount)
			{
				Debug::Log("AdvAI: Making AI build %s because it does not have enough of it. Wanted: %d\n",
					buildOtherPair.first->Name, targetCount);

				return buildOtherPair.first;
			}
		}
	}

	// Are there other AIBuildThis=yes buildings that we haven't built yet?
	for (const auto pBuilding : BuildingTypeClass::Array)
	{
		// Exclude defenses here, no need to build defenses just to have them
		if (TechTreeTypeClass::TotalBuildDefense.contains(pBuilding))
		{
			continue;
		}

		// Exclude helipads here, as they are handled dynamically based on occupied docks
		if (pBuilding->Helipad)
		{
			continue;
		}

		if (AdvAI_Can_Build_Building(pHouse, pBuilding, true))
		{
			const int targetCount = GetTargetBuildCount(pBuilding, 1, nullptr);
			if (pHouse->ActiveBuildingTypes.GetItemCount(pBuilding->ArrayIndex) < targetCount)
			{
				// Special case for the slave miner to count its undeployed form
				if (RulesClass::Instance->PrerequisiteProcAlternate != nullptr &&
					pBuilding->UndeploysInto == RulesClass::Instance->PrerequisiteProcAlternate &&
					pHouse->ActiveUnitTypes.GetItemCount(RulesClass::Instance->PrerequisiteProcAlternate->ArrayIndex) > 0)
					continue;

				Debug::Log("AdvAI: Making AI build %s because it has AIBuildThis=yes and the AI does not have enough of it. Wanted: %d\n",
					pBuilding->Name, targetCount);
				return pBuilding;
			}
		}
	}

	// Build power by default, but only if we have somewhere to expand towards.
	if (houseExt->NextExpansionPointLocation.X != 0 && houseExt->NextExpansionPointLocation.Y != 0)
	{
		const BuildingTypeClass* pOurPowerPlant = pPrimaryTechTree->GetRandomBuildable(TechTreeTypeClass::BuildType::BuildPower, canBuildFunction);
		if (pOurPowerPlant != nullptr)
		{
			CellStruct placementCell = BuildingExt::Get_Best_Expansion_Placement_Position_Helper(pHouse, const_cast<BuildingTypeClass*>(pOurPowerPlant), nullptr);
			if (placementCell.X > 0 && placementCell.Y > 0)
			{
				Debug::Log("AdvAI: Making AI build %s because the AI is expanding.\n",
					pOurPowerPlant->Name);
				return pOurPowerPlant;
			}
			else
			{
				const CellStruct targetCell = houseExt->NextExpansionPointLocation;
				if (!houseExt->ShouldPlaceDefenseAtBlockedEdge)
				{
					const BuildingTypeClass* pDefense = GetBasicDefense(pHouse, pPrimaryTechTree);
					if (pDefense != nullptr)
					{
						Debug::Log("AdvAI: House %d crawler blocked towards target (%d,%d). Building sentinel defense %s at the edge first.\n",
							pHouse->ArrayIndex, targetCell.X, targetCell.Y, pDefense->ID);
						
						houseExt->ShouldPlaceDefenseAtBlockedEdge = true;
						return pDefense;
					}
				}

				Debug::Log("AdvAI: House %d cannot find any valid adjacent cells to place %s towards target (%d,%d). Blacklisting target and aborting expansion.\n",
					pHouse->ArrayIndex, pOurPowerPlant->Name, targetCell.X, targetCell.Y);

				AdvAI_Add_Failed_Expansion_Point(pHouse, targetCell);
				BuildingExt::Mark_Expansion_As_Done(pHouse);
				houseExt->ShouldBuildRefinery = false;
				houseExt->ShouldPlaceDefenseAtBlockedEdge = false;
			}
		}
	}

	return nullptr;
}

const BuildingTypeClass* HouseExt::AdvAI_BuildAtLeastNOfSideAndMInTotal(HouseClass* pHouse, TechTreeTypeClass* techTree, TechTreeTypeClass::BuildType buildType, int sideBuildingsWanted, int totalBuildingsWanted, int extraCount)
{
	auto canBuildFunction = [pHouse](auto&& PH1)
	{
		return AdvAI_Can_Build_Building(pHouse, std::forward<decltype(PH1)>(PH1), true, true);
	};

	const BuildingTypeClass* pOurBuilding = techTree->GetRandomBuildable(buildType, canBuildFunction);
	const size_t ourBuildingCount = techTree->CountSideOwnedBuildings(pHouse, buildType) + extraCount;
	const size_t totalBuildingCount = TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, buildType) + extraCount;

	if (pOurBuilding != nullptr && (ourBuildingCount < sideBuildingsWanted || totalBuildingCount < totalBuildingsWanted))
	{
		return pOurBuilding;
	}

	return nullptr;
}

const BuildingTypeClass* HouseExt::AdvAI_Get_Building_To_Build(HouseClass* pHouse)
{
	const auto houseExt = ExtMap.Find(pHouse);
	if (houseExt != nullptr && !houseExt->UnclaimedTiberiumZones.empty())
	{
		const auto pTechTree = TechTreeTypeClass::GetAnySuitable(pHouse);
		if (pTechTree != nullptr && !pTechTree->BuildRefinery.empty())
		{
			BuildingTypeClass* pRefinery = pTechTree->BuildRefinery[0];
			if (pRefinery != nullptr && AdvAI_Can_Build_Building(pHouse, pRefinery, true, true))
			{
				for (auto it = houseExt->UnclaimedTiberiumZones.begin(); it != houseExt->UnclaimedTiberiumZones.end(); ++it)
				{
					CellStruct target = *it;
					if (AdvAI_Is_Failed_Expansion_Point(pHouse, target))
					{
						continue; // Skip targets currently under cooldown (failed placements)
					}
					
					bool isTree = false;
					for (const auto pTerrain : TerrainClass::Array)
					{
						if (pTerrain->IsAlive && !pTerrain->InLimbo && pTerrain->Type->SpawnsTiberium && pTerrain->GetMapCoords() == target)
						{
							isTree = true;
							break;
						}
					}
					const double refineryRange = isTree ? 22.0 : 27.0;

					bool hasRefinery = false;
					for (const auto pBld : BuildingClass::Array)
					{
						if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner == pHouse && pBld->Type->ResourceDestination)
						{
							if (target.DistanceFrom(pBld->GetMapCoords()) < refineryRange)
							{
								hasRefinery = true;
								break;
							}
						}
					}

					if (!hasRefinery)
					{
						houseExt->NextRefineryPlacementLocation = target;
						Debug::Log("AdvAI: Intercepting build loop to construct refinery %s for unclaimed tiberium zone at (%d,%d).\n",
							pRefinery->Name, target.X, target.Y);
						return pRefinery;
					}
				}
			}
		}
	}

	const BuildingTypeClass* buildChoice = AdvAI_Evaluate_Get_Best_Building(pHouse);

	if (buildChoice == nullptr)
	{
		return nullptr;
	}

	if (buildChoice->PowerDrain > 0 && !buildChoice->ResourceDestination && !buildChoice->ConstructionYard)
	{
		const int expectedSurplus = (pHouse->PowerOutput - GetRealPowerDrain(pHouse)) - buildChoice->PowerDrain;
		const int requiredSurplus = pHouse->PowerSurplus > 0 ? pHouse->PowerSurplus : RulesClass::Instance->PowerSurplus;

		if (expectedSurplus < requiredSurplus)
		{
			const auto pTechTree = TechTreeTypeClass::GetAnySuitable(pHouse);
			if (pTechTree != nullptr)
			{
				auto canBuildFunction = [pHouse](BuildingTypeClass* pType) {
					return AdvAI_Can_Build_Building(pHouse, pType, true, true);
				};

				const BuildingTypeClass* pAdvPower = pTechTree->GetRandomBuildable(TechTreeTypeClass::BuildType::BuildAdvancedPower, canBuildFunction);
				if (pAdvPower != nullptr)
				{
					Debug::Log("AdvAI: Intercepted building %s to build %s first due to insufficient power surplus.\n", buildChoice->Name, pAdvPower->Name);
					return pAdvPower;
				}

				const BuildingTypeClass* pPower = pTechTree->GetRandomBuildable(TechTreeTypeClass::BuildType::BuildPower, canBuildFunction);
				if (pPower != nullptr)
				{
					Debug::Log("AdvAI: Intercepted building %s to build %s first due to insufficient power surplus.\n", buildChoice->Name, pPower->Name);
					return pPower;
				}
			}
		}
	}

	return buildChoice;
}


static int GetRealPowerDrain(const HouseClass* pHouse)
{
	int realPowerDrain = pHouse->PowerDrain;
	for (const auto pBld : BuildingClass::Array)
	{
		if (pBld && pBld->Owner == pHouse && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->TogglePower && (!pBld->HasPower || !pBld->StuffEnabled))
			realPowerDrain += pBld->Type->PowerDrain;
	}
	return realPowerDrain;
}



static bool IsLocatedInAlliedBase(BuildingClass* pBuilding, HouseClass* pHouse)
{
	if (pBuilding == nullptr || pHouse == nullptr)
		return false;

	for (const auto pOtherHouse : HouseClass::Array)
	{
		if (pOtherHouse != pHouse && pHouse->IsAlliedWith(pOtherHouse))
		{
			for (const auto pOtherBld : pOtherHouse->Buildings)
			{
				if (pOtherBld && pOtherBld->IsAlive && !pOtherBld->InLimbo)
				{
					if (pBuilding->GetMapCoords().DistanceFrom(pOtherBld->GetMapCoords()) < 25.0)
						return true;
				}
			}
		}
	}
	return false;
}

/**
 *  Checks if AdvAI should raise money.
 *  If it should, then raises money.
 *
 *  Author: Rampastring
 */
void HouseExt::AdvAI_Raise_Money(HouseClass* pHouse)
{
	// We should raise money if we are low on funds and have zero refineries.

	if (pHouse->Balance > 1000)
	{
		return;
	}

	int refineryCount = 0;
	for (const auto pRefinery : RulesClass::Instance->BuildRefinery)
	{
		refineryCount += pHouse->ActiveBuildingTypes.GetItemCount(pRefinery->ArrayIndex);
	}

	if (refineryCount > 0)
	{
		return;
	}

	// Look for buildings to sell.
	Debug::Log("AdvAI: Attempting to raise money.\n");

	BuildingClass* pBestBuilding = nullptr;
	int bestCost = INT_MIN;

	for (const auto pBuilding : BuildingClass::Array)
	{
		if (!pBuilding->IsAlive || pBuilding->InLimbo || pBuilding->Owner != pHouse || pBuilding->Type->ConstructionYard)
		{
			continue;
		}

		// Safety check: do not sell any buildings built in allied bases (backup/life insurance)
		if (IsLocatedInAlliedBase(pBuilding, pHouse))
			continue;

		if (pBuilding->CurrentMission == Mission::Construction || pBuilding->QueuedMission == Mission::Construction)
		{
			// Don't sell something that we've just built.
			continue;
		}

		if (pBuilding->CurrentMission == Mission::Selling || pBuilding->QueuedMission == Mission::Selling)
		{

			// We are already in the process of selling something.
			return;
		}

		// Prefer selling the most expensive stuff first.
		// Give a lower priority to super-weapon buildings, however.
		// They'll be expensive to replace later on.
		int cost = pBuilding->Type->Cost;

		if (BuildingTypeExt::HasDisableableSuperWeapons(pBuilding->Type))
		{
			cost = cost / 3;
		}

		if (cost > bestCost)
		{
			pBestBuilding = pBuilding;
			bestCost = cost;
		}
	}

	// If we found something to sell, then sell it.
	if (pBestBuilding != nullptr)
	{
		Debug::Log("AdvAI: Found a building to sell.\n");
		pBestBuilding->Sell(1);
	}
}


/**
 *  Perfoms some general economy maintenance.
 *  Raises money if necessary.
 *
 *  Author: Rampastring
 */
void HouseExt::AdvAI_Economy_Upkeep(HouseClass* pHouse)
{
	AdvAI_Raise_Money(pHouse);

	const auto houseExt = ExtMap.Find(pHouse);
	auto pPrimaryTechTree = houseExt->PrimaryTechTreeType;
	if (pPrimaryTechTree == nullptr)
	{
		pPrimaryTechTree = TechTreeTypeClass::GetForSide(pHouse->Type->SideIndex);
	}

	if (pPrimaryTechTree != nullptr)
	{
		// Check threat levels
		int enemyAircraftValue = 0;
		for (const auto pOtherHouse : HouseClass::Array)
		{
			if (pOtherHouse == pHouse || pHouse->IsAlliedWith(pOtherHouse))
				continue;
			for (const auto pAircraft : AircraftClass::Array)
			{
				if (pAircraft->Owner == pOtherHouse && pAircraft->IsAlive && !pAircraft->InLimbo)
				{
					if (pAircraft->Type->AirportBound || pAircraft->Type->JumpJet)
						enemyAircraftValue += 15;
				}
			}
		}
		const bool isUnderThreat = AdvAI_Is_Under_Start_Rush_Threat(pHouse, enemyAircraftValue);

		// hasEnemiesClose scan
		bool hasEnemiesClose = false;
		{
			const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
			CellStruct baseCenter = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pHouse->Base_Center();
			const double checkDistSq = 30.0 * 30.0;
			for (const auto pFoot : FootClass::Array)
			{
				if (pFoot && pFoot->IsAlive && !pFoot->InLimbo && pFoot->Owner != pHouse && !pFoot->Owner->IsNeutral() && !pHouse->IsAlliedWith(pFoot->Owner))
				{
					if (baseCenter.DistanceFromSquared(pFoot->GetMapCoords()) <= checkDistSq)
					{
						hasEnemiesClose = true;
						break;
					}
				}
			}
			if (!hasEnemiesClose)
			{
				for (const auto pBld : BuildingClass::Array)
				{
					if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner != pHouse && !pBld->Owner->IsNeutral() && !pHouse->IsAlliedWith(pBld->Owner))
					{
						const auto& primary = pBld->Type->GetWeapon(0, false);
						const auto& secondary = pBld->Type->GetWeapon(1, false);

						if (primary.WeaponType != nullptr || secondary.WeaponType != nullptr)
						{
							if (baseCenter.DistanceFromSquared(pBld->GetMapCoords()) <= checkDistSq)
							{
								hasEnemiesClose = true;
								break;
							}
						}
					}
				}
			}
		}

		int paranoiaDuration = TICKS_PER_MINUTE;

		const bool wasRecentlyAttacked = pHouse->LATime + paranoiaDuration > Unsorted::CurrentFrame;
		const bool isParanoid = (isUnderThreat && hasEnemiesClose) || wasRecentlyAttacked;

		// hasSomethingToProtect scan
		bool hasSomethingToProtect = false;
		for (const auto pBld : pHouse->Buildings)
		{
			if (pBld && pBld->Type && pBld->Type->ToProtect)
			{
				bool isProtected = false;
				for (const auto pOther : pHouse->Buildings)
				{
					if (pOther && pOther->IsAlive && !pOther->InLimbo && pOther != pBld)
					{
						if (TechTreeTypeClass::TotalBuildDefense.contains(pOther->Type))
						{
							if (pBld->GetMapCoords().DistanceFromSquared(pOther->GetMapCoords()) < 49.0)
							{
								isProtected = true;
								break;
							}
						}
					}
				}
				if (!isProtected)
				{
					hasSomethingToProtect = true;
					break;
				}
			}
		}

		if (!hasSomethingToProtect)
		{
			const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
			if (pOurConYard != nullptr)
			{
				for (const auto pBld : pHouse->Buildings)
				{
					if (pBld && pBld->Type && pBld->Type->Refinery)
					{
						if (pBld->GetMapCoords().DistanceFromSquared(pOurConYard->GetMapCoords()) >= 400.0)
						{
							bool isProtected = false;
							for (const auto pOther : pHouse->Buildings)
							{
								if (pOther && pOther->IsAlive && !pOther->InLimbo && pOther != pBld)
								{
									if (TechTreeTypeClass::TotalBuildDefense.contains(pOther->Type))
									{
										if (pBld->GetMapCoords().DistanceFromSquared(pOther->GetMapCoords()) < 225.0)
										{
											isProtected = true;
											break;
										}
									}
								}
							}
							if (!isProtected)
							{
								hasSomethingToProtect = true;
								break;
							}
						}
					}
				}
			}
		}

		// Only sell redundant defenses when completely safe (not paranoid, nothing needs protection, and no threat)
		if (!isParanoid && !hasSomethingToProtect && !isUnderThreat)
		{
			bool alreadySelling = false;
			for (const auto pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && TechTreeTypeClass::TotalBuildDefense.contains(pBld->Type))
				{
					if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
					{
						alreadySelling = true;
						break;
					}
				}
			}

			if (!alreadySelling)
			{
				const int slaveMinerCount = RulesClass::Instance->PrerequisiteProcAlternate != nullptr ?
					pHouse->ActiveUnitTypes.GetItemCount(RulesClass::Instance->PrerequisiteProcAlternate->ArrayIndex) : 0;
				size_t refineryCount = pPrimaryTechTree->CountSideOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildRefinery);
				refineryCount += slaveMinerCount;

				const size_t powerPlantCount = TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildPower) +
					TechTreeTypeClass::CountTotalOwnedBuildings(pHouse, TechTreeTypeClass::BuildType::BuildAdvancedPower) * 4;

				int optimalDefenseValue = refineryCount + powerPlantCount / 4;
				if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
					optimalDefenseValue += 5;

				optimalDefenseValue *= 15;

				int antiInfantryDefenseValue = 0;
				int antiVehicleDefenseValue = 0;
				for (const auto pDefense : TechTreeTypeClass::TotalBuildDefense)
				{
					antiInfantryDefenseValue += pHouse->ActiveBuildingTypes.GetItemCount(pDefense->ArrayIndex) * pDefense->AntiInfantryValue;
					antiVehicleDefenseValue += pHouse->ActiveBuildingTypes.GetItemCount(pDefense->ArrayIndex) * pDefense->AntiArmorValue;
				}

				if (antiInfantryDefenseValue > optimalDefenseValue + 35 || antiVehicleDefenseValue > optimalDefenseValue + 35)
				{
					BuildingClass* pDefToSell = nullptr;
					double closestDistSq = std::numeric_limits<double>::max();
					const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
					CellStruct center = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pHouse->Base_Center();

					for (const auto pBld : pHouse->Buildings)
					{
						if (pBld && pBld->IsAlive && !pBld->InLimbo && TechTreeTypeClass::TotalBuildDefense.contains(pBld->Type))
						{
							bool safeToSell = true;

							// 1. Keep at least 2 defenses within the main base (within 20 cells of the Construction Yard),
							// and NEVER sell any defenses that are outside the main base (>= 20 cells from ConYard).
							if (pOurConYard != nullptr)
							{
								double distToConYardSq = pBld->GetMapCoords().DistanceFromSquared(pOurConYard->GetMapCoords());
								if (distToConYardSq >= 400.0)
								{
									safeToSell = false;
								}
								else
								{
									int defensesInBase = 0;
									for (const auto pOther : pHouse->Buildings)
									{
										if (pOther && pOther->IsAlive && !pOther->InLimbo && TechTreeTypeClass::TotalBuildDefense.contains(pOther->Type))
										{
											if (pOther->GetMapCoords().DistanceFromSquared(pOurConYard->GetMapCoords()) < 400.0)
												defensesInBase++;
										}
									}
									if (defensesInBase <= 2)
										safeToSell = false;
								}
							}

							// 2. Keep the defense if it is the only protector of a ToProtect structure or expansion refinery
							if (safeToSell)
							{
								for (const auto pTarget : pHouse->Buildings)
								{
									if (pTarget && pTarget->IsAlive && !pTarget->InLimbo && pTarget->Type)
									{
										if (pTarget->Type->ToProtect)
										{
											if (pBld->GetMapCoords().DistanceFromSquared(pTarget->GetMapCoords()) < 49.0)
											{
												int defenders = 0;
												for (const auto pOther : pHouse->Buildings)
												{
													if (pOther && pOther->IsAlive && !pOther->InLimbo && TechTreeTypeClass::TotalBuildDefense.contains(pOther->Type))
													{
														if (pOther->GetMapCoords().DistanceFromSquared(pTarget->GetMapCoords()) < 49.0)
															defenders++;
													}
												}
												if (defenders <= 1)
												{
													safeToSell = false;
													break;
												}
											}
										}
										else if (pTarget->Type->Refinery && pOurConYard != nullptr)
										{
											if (pTarget->GetMapCoords().DistanceFromSquared(pOurConYard->GetMapCoords()) >= 400.0)
											{
												if (pBld->GetMapCoords().DistanceFromSquared(pTarget->GetMapCoords()) < 225.0)
												{
													safeToSell = false;
													break;
												}
											}
										}
									}
								}
							}

							// 3. Keep the defense if it is close to any allied building (within 10 cells)
							if (safeToSell)
							{
								for (const auto pOtherHouse : HouseClass::Array)
								{
									if (pOtherHouse != pHouse && pHouse->IsAlliedWith(pOtherHouse))
									{
										for (const auto pTarget : pOtherHouse->Buildings)
										{
											if (pTarget && pTarget->IsAlive && !pTarget->InLimbo)
											{
												if (pBld->GetMapCoords().DistanceFromSquared(pTarget->GetMapCoords()) < 100.0)
												{
													safeToSell = false;
													break;
												}
											}
										}
									}
									if (!safeToSell)
										break;
								}
							}

							// 4. Only sell the defense if it is clustered (pegada, touching with 0 empty cells) with another defense
							if (safeToSell)
							{
								bool isClustered = false;
								const int b1X = pBld->GetMapCoords().X;
								const int b1Y = pBld->GetMapCoords().Y;
								const int b1W = pBld->Type->GetFoundationWidth();
								const int b1H = pBld->Type->GetFoundationHeight(false);

								for (const auto pOther : pHouse->Buildings)
								{
									if (pOther && pOther->IsAlive && !pOther->InLimbo && pOther != pBld && TechTreeTypeClass::TotalBuildDefense.contains(pOther->Type))
									{
										const int b2X = pOther->GetMapCoords().X;
										const int b2Y = pOther->GetMapCoords().Y;
										const int b2W = pOther->Type->GetFoundationWidth();
										const int b2H = pOther->Type->GetFoundationHeight(false);

										// Check if touching (0 empty cells margin)
										if ((b1X - 1 <= b2X + b2W - 1) && (b1X + b1W >= b2X) &&
											(b1Y - 1 <= b2Y + b2H - 1) && (b1Y + b1H >= b2Y))
										{
											isClustered = true;
											break;
										}
									}
								}
								if (!isClustered)
								{
									safeToSell = false;
								}
							}

							if (safeToSell)
							{
								bool chooseThis = false;
								if (pDefToSell == nullptr)
								{
									chooseThis = true;
								}
								else
								{
									// Prefer selling lower cost (worse) defenses first to keep our premium defenses intact
									if (pBld->Type->Cost < pDefToSell->Type->Cost)
									{
										chooseThis = true;
									}
									else if (pBld->Type->Cost == pDefToSell->Type->Cost)
									{
										// If costs are equal, sell the one closer to the base center
										double distSq1 = pBld->GetMapCoords().DistanceFromSquared(center);
										double distSq2 = pDefToSell->GetMapCoords().DistanceFromSquared(center);
										if (distSq1 < distSq2)
										{
											chooseThis = true;
										}
									}
								}

								if (chooseThis)
								{
									pDefToSell = pBld;
								}
							}
						}
					}

					if (pDefToSell != nullptr)
					{
						Debug::Log("AdvAI: Selling redundant base defense %s at (%d,%d) because rush threat is over. Owned value: %d, Calm optimal: %d\n",
							pDefToSell->Type->ID, pDefToSell->GetMapCoords().X, pDefToSell->GetMapCoords().Y, antiInfantryDefenseValue, optimalDefenseValue);
						pDefToSell->Sell(1);
					}
				}
			}
		}
	}

	// Don't sell refineries on Easy mode.
	if (pHouse->AIDifficulty == AIDifficulty::Hard)
	{
		return;
	}

	int refineryCount = 0;
	for (const auto pRefinery : RulesClass::Instance->BuildRefinery)
	{
		// Don't count slave miners as those don't even use harvesters
		if (pRefinery->Enslaves != nullptr)
			continue;

		refineryCount += pHouse->ActiveBuildingTypes.GetItemCount(pRefinery->ArrayIndex);
	}

	int harvesterCount = 0;
	for (const auto pHarvester : RulesClass::Instance->HarvesterUnit)
	{
		harvesterCount += pHouse->ActiveUnitTypes.GetItemCount(pHarvester->ArrayIndex);
	}

	const int toSellCount = refineryCount - harvesterCount;
	if (toSellCount <= 0)
	{
		return;
	}

	Debug::Log("AdvAI: Looking for a refinery to sell because we have %d excess.\n", toSellCount);

	// Sell the refinery that is closest to our primary enemy.
	// If we have extra refineries, we have lost harvesters, and harvesters are most likely
	// lost near the expansion that is closest to our primary enemy.
	// If we have no primary enemy, then sell one near our base center.
	// It probably won't go horribly wrong anyway.

	const HouseClass* pEnemy = nullptr;
	if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
	{
		pEnemy = HouseClass::Array[pHouse->EnemyHouseIndex];
	}

	CellStruct centerPoint;

	if (pEnemy != nullptr)
	{
		centerPoint = pEnemy->Base_Center();
	}
	else
	{
		centerPoint = pHouse->Base_Center();
	}

	BuildingClass* farthest_refinery = nullptr;
	double closest_distance = std::numeric_limits<double>::max();

	for (const auto pBuilding : BuildingClass::Array)
	{
		if (!pBuilding->IsAlive || pBuilding->InLimbo || pBuilding->Owner != pHouse || !pBuilding->Type->Refinery)
		{
			continue;
		}

		if (pBuilding->CurrentMission == Mission::Construction || pBuilding->QueuedMission == Mission::Construction)
		{
			// If a refinery is in process of being constructed, it hasn't got the spawn its FreeUnit
			// harvester yet.
			Debug::Log("AdvAI: We have a refinery in construction phase, skip.\n");
			return;
		}

		if (pBuilding->CurrentMission == Mission::Selling || pBuilding->QueuedMission == Mission::Selling)
		{

			// We are already in the process of selling a refinery, don't sell more
			// until it's finished.
			Debug::Log("AdvAI: We are already selling a refinery, skip.\n");
			return;
		}

		const double distance = centerPoint.DistanceFrom(GeneralUtils::CellFromCoordinates(pBuilding->GetCenterCoords()));
		if (distance < closest_distance)
		{
			closest_distance = distance;
			farthest_refinery = pBuilding;
		}
	}

	if (farthest_refinery != nullptr)
	{
		Debug::Log("AdvAI: Found a Refinery to sell.\n");
		farthest_refinery->Sell(1);
	}
}


/**
 *  Checks for sleeping harvesters. If found, puts them to Harvest mode.
 *
 *  Author: Rampastring
 */
void HouseExt::AdvAI_Awaken_Sleeping_Harvesters(HouseClass* pHouse)
{
	for (const auto pUnit : UnitClass::Array)
	{
		if (!pUnit->IsAlive || pUnit->InLimbo || pUnit->Owner != pHouse || !pUnit->Type->Harvester)
		{
			continue;
		}

		if (pUnit->CurrentMission == Mission::Sleep || pUnit->CurrentMission == Mission::Guard)
		{
			Debug::Log("AdvAI: Waking up a sleeping harvester.\n");
			pUnit->QueueMission(Mission::Harvest, true);
			pUnit->NextMission();
		}
	}
}


/**
 *  Sells extra construction yards of the specific house until there is one one left.
 *
 *  Author: Rampastring
 */
void HouseExt::AdvAI_Sell_Extra_ConYards(HouseClass* pHouse)
{
	const int toSellCount = pHouse->ConYards.Count - 1;

	Debug::Log("AdvAI: AI %d has too many Construction Yards (%d). Selling off %d of them. Frame: %d\n", pHouse->ArrayIndex, toSellCount, Unsorted::CurrentFrame);

	if (toSellCount < 1)
	{
		return;
	}

	int soldCount = 0;

	for (int i = pHouse->ConYards.Count - 1; i > 0; i--)
	{
		BuildingClass* building = pHouse->ConYards[i];

		if (!building->IsAlive || building->InLimbo)
		{
			continue;
		}

		if (building->CurrentMission == Mission::Selling || building->QueuedMission == Mission::Selling)
		{
			soldCount++;

			if (soldCount >= toSellCount)
			{
				break;
			}

			continue;
		}

		Debug::Log("AdvAI: Found a Construction Yard to sell.\n");

		building->Sell(1);
		soldCount++;

		if (soldCount >= toSellCount)
		{
			break;
		}
	}
}


/**
 *  Implements DTA's custom AI building selection logic.
 *
 *  Author: Rampastring
 */
void HouseExt::Vinifera_HouseClass_AI_Building(HouseClass* pHouse)
{
	// Decide what to build.
	// If we already have something to build, do nothing.
	if (pHouse->ProducingBuildingTypeIndex != -1)
		return;

	if (pHouse->ConYards.Count <= 0)
		return;

	const auto houseExt = ExtMap.Find(pHouse);

	// If either target is missing, check for a new location to expand to.
	if (houseExt->CombatCrawlingTarget.X <= 0 || houseExt->CombatCrawlingTarget.Y <= 0 ||
		houseExt->ResourceCrawlingTarget.X <= 0 || houseExt->ResourceCrawlingTarget.Y <= 0)
	{
		if (Unsorted::CurrentFrame >= houseExt->NextExpansionSearchFrame)
		{
			AdvAI_House_Search_For_Next_Expansion_Point(pHouse);
			houseExt->NextExpansionSearchFrame = Unsorted::CurrentFrame + 300; // Cooldown of 20 seconds at 15 FPS
		}
	}

	// Sync the previous ShouldBuildRefinery state back to ResourceShouldBuildRefinery if we were targeting resources
	if (houseExt->NextExpansionPointLocation == houseExt->ResourceCrawlingTarget)
	{
		houseExt->ResourceShouldBuildRefinery = houseExt->ShouldBuildRefinery;
	}

	// Dynamic alternating target logic
	const bool isParanoid = (pHouse->LATime + 900 > Unsorted::CurrentFrame);
	const int combatLimit = isParanoid ? 5 : 3;

	if (houseExt->CombatCrawlingTarget.X > 0 && houseExt->CombatCrawlingTarget.Y > 0 &&
		houseExt->ResourceCrawlingTarget.X > 0 && houseExt->ResourceCrawlingTarget.Y > 0)
	{
		if (houseExt->ConsecutiveCombatBuilds < combatLimit)
		{
			houseExt->NextExpansionPointLocation = houseExt->CombatCrawlingTarget;
			houseExt->ShouldBuildRefinery = false;
		}
		else
		{
			houseExt->NextExpansionPointLocation = houseExt->ResourceCrawlingTarget;
			houseExt->ShouldBuildRefinery = houseExt->ResourceShouldBuildRefinery;
		}
	}
	else if (houseExt->CombatCrawlingTarget.X > 0 && houseExt->CombatCrawlingTarget.Y > 0)
	{
		houseExt->NextExpansionPointLocation = houseExt->CombatCrawlingTarget;
		houseExt->ShouldBuildRefinery = false;
	}
	else if (houseExt->ResourceCrawlingTarget.X > 0 && houseExt->ResourceCrawlingTarget.Y > 0)
	{
		houseExt->NextExpansionPointLocation = houseExt->ResourceCrawlingTarget;
		houseExt->ShouldBuildRefinery = houseExt->ResourceShouldBuildRefinery;
	}
	else
	{
		houseExt->NextExpansionPointLocation = CellStruct(0, 0);
		houseExt->ShouldBuildRefinery = false;
	}

	const BuildingTypeClass* toBuild = AdvAI_Get_Building_To_Build(pHouse);

	if (toBuild == nullptr)
	{
		return;
	}

	Debug::Log("AI %d selected building %s to build. Frame: %d\n", pHouse->ArrayIndex, toBuild->Name, Unsorted::CurrentFrame);

	pHouse->ProducingBuildingTypeIndex = toBuild->ArrayIndex;
}

/**
 *  Performs some maintenance for the Advanced AI.
 *
 *  Author: Rampastring
 */
void HouseExt::AdvAI_HouseClass_Expert_AI(HouseClass* pHouse)
{
	if (pHouse->Type->MultiplayPassive)
	{
		return;
	}

	// Only enable our custom logic when using Advanced AI.
	if (!RulesExt::Global()->AdvancedAI)
	{
		return;
	}

	// If we have more than 1 ConYard without Rules allowing it, sell some of them off
	// to avoid the "Extreme AI" syndrome.
	if (pHouse->ConYards.Count > 1 && !RulesExt::Global()->AdvancedAI_MultiConYard)
	{
		AdvAI_Sell_Extra_ConYards(pHouse);
	}

	// If we have no enemy, then pick one.
	if (pHouse->EnemyHouseIndex == -1)
	{
		pHouse->Unknown_Timer_5640.Start(0);
	}

	const auto houseExt = ExtMap.Find(pHouse);

	// Clear attacker and building coords if paranoia has expired
	int paranoiaDuration = TICKS_PER_MINUTE;
	if (pHouse->LATime == 0 || pHouse->LATime + paranoiaDuration + 1800 <= Unsorted::CurrentFrame)
	{
		houseExt->LastAttackerCoords = CellStruct(0, 0);
		houseExt->LastAttackedBuildingCoords = CellStruct(0, 0);
	}

	// Do some economy upkeep to keep the AI running.

	if (Unsorted::CurrentFrame > houseExt->LastExcessRefineryCheckFrame + 500)
	{
		houseExt->LastExcessRefineryCheckFrame = Unsorted::CurrentFrame;
		AdvAI_Economy_Upkeep(pHouse);
	}

	if (Unsorted::CurrentFrame > houseExt->LastObsoleteRefineryCheckFrame + 4500)
	{
		houseExt->LastObsoleteRefineryCheckFrame = Unsorted::CurrentFrame;
		AdvAI_Recycle_Obsolete_Refineries(pHouse);
	}

	if (Unsorted::CurrentFrame > houseExt->LastSleepingHarvesterCheckFrame + 1000)
	{
		houseExt->LastSleepingHarvesterCheckFrame = Unsorted::CurrentFrame;
		AdvAI_Awaken_Sleeping_Harvesters(pHouse);
	}

	if (Unsorted::CurrentFrame > houseExt->LastUnclaimedTiberiumCheckFrame + 450)
	{
		houseExt->LastUnclaimedTiberiumCheckFrame = Unsorted::CurrentFrame;
		AdvAI_Update_Unclaimed_Tiberium_Zones(pHouse);
	}

	if (Unsorted::CurrentFrame > houseExt->LastPrimaryFactoryCheckFrame + 400)
	{
		houseExt->LastPrimaryFactoryCheckFrame = Unsorted::CurrentFrame;
		AdvAI_Update_Primary_Factories(pHouse);
	}

	// If we have 0 ConYards and 0 War Factories, it is very unlikely we could get
	// back into the game. Send all our non-Harvester vehicles into Hunt mode.
	if (Unsorted::CurrentFrame > 5000 && pHouse->ConYards.Count == 0 && pHouse->NumWarFactories == 0 && !houseExt->HasPerformedVehicleCharge)
	{
		houseExt->HasPerformedVehicleCharge = true;

		for (const auto pUnit : UnitClass::Array)
		{
			if (pUnit->Owner == pHouse &&
				(pUnit->Type->DeploysInto == nullptr || !pUnit->Type->DeploysInto->ConstructionYard) &&
				!pUnit->Type->Harvester &&
				!pUnit->Type->Weeder)
			{
				if (pUnit->Team != nullptr)
				{
					pUnit->Team->LiberateMember(pUnit);
				}

				pUnit->QueueMission(Mission::Hunt, false);
			}
		}
	}

	// If we are under threat of getting rushed early and our ConYard is producing something non-defensive and non-power-granting, abandon it.
	const int enemyAircraftCount = AdvAI_Calculate_Enemy_Aircraft_Value(pHouse);
	const bool isUnderThreat = AdvAI_Is_Under_Start_Rush_Threat(pHouse, enemyAircraftCount);

	if (isUnderThreat)
	{
		FactoryClass* buildingFactory = pHouse->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare);
		if (buildingFactory != nullptr)
		{
			if (buildingFactory->Object != nullptr)
			{
				const BuildingClass* pBuilding = reinterpret_cast<BuildingClass*>(buildingFactory->Object);

				if (pBuilding->Type->PowerBonus <= 0 ||
					pBuilding->Type->GetWeapon(0, false).WeaponType == nullptr ||
					pBuilding->Type->Factory != AbstractType::InfantryType)
				{
					buildingFactory->AbandonProduction();
				}
			}
		}
	}
}

void HouseExt::AdvAI_Update_Primary_Factories(HouseClass* pHouse)
{
	if (!pHouse || pHouse->IsControlledByHuman())
		return;

	const auto houseExt = ExtMap.Find(pHouse);
	CellStruct targetCoords = CellStruct(0, 0);
	bool hasTarget = false;

	if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		targetCoords = houseExt->NextExpansionPointLocation;
		hasTarget = true;
	}
	else
	{
		const HouseClass* pEnemy = nullptr;
		if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
		{
			pEnemy = HouseClass::Array[pHouse->EnemyHouseIndex];
		}

		if (pEnemy != nullptr && pEnemy->Buildings.Count > 0)
		{
			targetCoords = pEnemy->Base_Center();
			hasTarget = true;
		}
	}

	if (!hasTarget)
		return;

	auto updatePrimary = [pHouse, targetCoords](AbstractType type, bool isNaval) {
		BuildingClass* pOldPrimary = nullptr;
		for (const auto pBuilding : pHouse->Buildings)
		{
			if (pBuilding && pBuilding->IsAlive && pBuilding->Type->Factory == type && pBuilding->Type->Naval == isNaval && pBuilding->IsPrimaryFactory)
			{
				pOldPrimary = pBuilding;
				break;
			}
		}

		BuildingClass* pBestFactory = nullptr;
		double bestDistanceSq = std::numeric_limits<double>::max();

		// Find the primary enemy's base center for connectivity verification
		CellStruct enemyBaseCenter = CellStruct(0, 0);
		bool hasEnemy = false;
		const HouseClass* pEnemy = nullptr;
		if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
		{
			pEnemy = HouseClass::Array[pHouse->EnemyHouseIndex];
		}
		if (pEnemy != nullptr && pEnemy->Buildings.Count > 0)
		{
			enemyBaseCenter = pEnemy->Base_Center();
			hasEnemy = true;
		}

		for (const auto pBuilding : pHouse->Buildings)
		{
			if (pBuilding && pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Type->Factory == type && pBuilding->Type->Naval == isNaval)
			{
				// For land factories, verify they can actually reach the enemy base by land
				if (!isNaval && hasEnemy && !GeneralUtils::AreZonesConnected(pBuilding->GetMapCoords(), enemyBaseCenter))
					continue;

				double distSq = pBuilding->GetMapCoords().DistanceFromSquared(targetCoords);
				if (distSq < bestDistanceSq)
				{
					bestDistanceSq = distSq;
					pBestFactory = pBuilding;
				}
			}
		}

		// Fallback: If no land factory can reach the target (e.g. they are all on the main island and the target is on another island),
		// fall back to distance check to make sure at least some factory remains primary.
		if (pBestFactory == nullptr)
		{
			for (const auto pBuilding : pHouse->Buildings)
			{
				if (pBuilding && pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Type->Factory == type && pBuilding->Type->Naval == isNaval)
				{
					double distSq = pBuilding->GetMapCoords().DistanceFromSquared(targetCoords);
					if (distSq < bestDistanceSq)
					{
						bestDistanceSq = distSq;
						pBestFactory = pBuilding;
					}
				}
			}
		}

		if (pBestFactory != nullptr)
		{
			// Find the active FactoryClass for this house/type/naval
			FactoryClass* pActiveFactory = pHouse->GetPrimaryFactory(type, isNaval, BuildCat::DontCare);

			// Fallback: if GetPrimaryFactory returned null, try to find any building currently holding the FactoryClass pointer
			if (pActiveFactory == nullptr)
			{
				for (const auto pBuilding : pHouse->Buildings)
				{
					if (pBuilding && pBuilding->IsAlive && pBuilding->Type->Factory == type && pBuilding->Type->Naval == isNaval && pBuilding->Factory != nullptr)
					{
						pActiveFactory = pBuilding->Factory;
						break;
					}
				}
			}

			if (pActiveFactory != nullptr)
			{
				if (pBestFactory != pOldPrimary)
				{
					Debug::Log("AdvAI PrimaryFactory: House %d (%s) changing primary %s %s factory from %s (%d,%d) to %s (%d,%d).\n",
						pHouse->ArrayIndex, pHouse->Type->ID,
						(isNaval ? "Naval" : "Land"),
						(type == AbstractType::InfantryType ? "Infantry" : "Unit/Vehicle"),
						pOldPrimary ? pOldPrimary->Type->ID : "None",
						pOldPrimary ? pOldPrimary->GetMapCoords().X : 0,
						pOldPrimary ? pOldPrimary->GetMapCoords().Y : 0,
						pBestFactory->Type->ID,
						pBestFactory->GetMapCoords().X,
						pBestFactory->GetMapCoords().Y);
				}

				// Assign the active factory to the best building, and detach it from all others of this category
				for (const auto pBuilding : pHouse->Buildings)
				{
					if (pBuilding && pBuilding->Type->Factory == type && pBuilding->Type->Naval == isNaval)
					{
						pBuilding->IsPrimaryFactory = (pBuilding == pBestFactory);
						if (pBuilding == pBestFactory)
							pBuilding->Factory = pActiveFactory;
						else if (pBuilding->Factory == pActiveFactory)
							pBuilding->Factory = nullptr;
					}
				}

				pHouse->SetPrimaryFactory(pActiveFactory, type, isNaval, BuildCat::DontCare);
			}
		}
	};

	updatePrimary(AbstractType::InfantryType, false);
	updatePrimary(AbstractType::UnitType, false);
	updatePrimary(AbstractType::UnitType, true);
}

void HouseExt::AdvAI_Recycle_Furthest_Factory(HouseClass* pHouse, AbstractType factoryType, bool isNaval, size_t optimalCount, CellStruct targetCell)
{
	return; // Permanently disabled per user request.

	const auto houseExt = HouseExt::ExtMap.Find(pHouse);
	const auto pPrimaryTechTree = houseExt->PrimaryTechTreeType;
	const bool limitFactories = pPrimaryTechTree == nullptr || pPrimaryTechTree->LimitedFactories;

	if (!limitFactories)
		return;

	if (targetCell.X <= 0 || targetCell.Y <= 0)
		return;

	if (Unsorted::CurrentFrame < houseExt->LastFactoryRecycleFrame + 2700)
		return;

	// Check if we have a foothold near the target (any building of ours within 22.0 cells of targetCell)
	bool hasFoothold = false;
	for (const auto pBuilding : pHouse->Buildings)
	{
		if (pBuilding && pBuilding->IsAlive && !pBuilding->InLimbo)
		{
			if (pBuilding->GetMapCoords().DistanceFrom(targetCell) <= 22.0)
			{
				hasFoothold = true;
				break;
			}
		}
	}

	if (!hasFoothold)
		return; // Cannot build close to the target anyway, wait for crawler to advance

	// Count our active factories of this type
	std::vector<BuildingClass*> ourFactories;
	for (const auto pBuilding : pHouse->Buildings)
	{
		if (pBuilding && pBuilding->IsAlive && !pBuilding->InLimbo)
		{
			if (pBuilding->Type->Factory == factoryType && pBuilding->Type->Naval == isNaval)
				ourFactories.push_back(pBuilding);
		}
	}

	if (ourFactories.size() < optimalCount || ourFactories.size() < 3)
		return;

	// Find the nearest and furthest distances to the targetCell
	double nearestDist = std::numeric_limits<double>::max();
	double furthestDist = 0.0;
	BuildingClass* pFurthestFactory = nullptr;

	const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
	const CellStruct conyardCell = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pHouse->Base_Center();

	for (const auto pFactory : ourFactories)
	{
		if (pFactory->CurrentMission == Mission::Selling || pFactory->QueuedMission == Mission::Selling)
			return;

		const double dist = pFactory->GetMapCoords().DistanceFrom(targetCell);
		if (dist < nearestDist)
			nearestDist = dist;

		const double conyardDist = pFactory->GetMapCoords().DistanceFrom(conyardCell);
		if (conyardDist > 20.0)
		{
			if (dist > furthestDist)
			{
				furthestDist = dist;
				pFurthestFactory = pFactory;
			}
		}
	}

	if (nearestDist > 22.0 && pFurthestFactory != nullptr && furthestDist > 35.0)
	{
		Debug::Log("AdvAI Recycling: House %d selling furthest %s %s factory %s at (%d,%d) (dist %.1f from target) to rebuild closer (nearest is %.1f).\n",
			pHouse->ArrayIndex,
			(isNaval ? "Naval" : "Land"),
			(factoryType == AbstractType::InfantryType ? "Infantry" : "Unit/Vehicle"),
			pFurthestFactory->Type->ID,
			pFurthestFactory->GetMapCoords().X, pFurthestFactory->GetMapCoords().Y,
			furthestDist, nearestDist);
		pFurthestFactory->Sell(1);
		houseExt->LastFactoryRecycleFrame = Unsorted::CurrentFrame;
	}
}

void HouseExt::AdvAI_Recycle_Obsolete_Refineries(HouseClass* pHouse)
{
	const BuildingClass* pOurConYard = pHouse->ConYards.Count > 0 ? pHouse->ConYards[0] : nullptr;
	const CellStruct baseCenter = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pHouse->Base_Center();

	for (const auto pBld : pHouse->Buildings)
	{
		if (!pBld || !pBld->IsAlive || pBld->InLimbo)
		{
			continue;
		}

		if (!pBld->Type->Refinery && !pBld->Type->ResourceDestination)
		{
			continue;
		}

		if (pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
		{
			continue;
		}

		// Skip refineries that are close to the core base (ConYard) to avoid leaving the base defenseless or base-less.
		const double distToConYard = pBld->GetMapCoords().DistanceFrom(baseCenter);
		if (distToConYard < 20.0)
		{
			continue;
		}

		// Scan a radius of 14 cells around the refinery to check for tiberium trees or tiberium overlays.
		const CellStruct refCoords = GeneralUtils::CellFromCoordinates(pBld->GetCenterCoords());
		bool hasResources = false;

		for (int dy = -27; dy <= 27; ++dy)
		{
			for (int dx = -27; dx <= 27; ++dx)
			{
				CellStruct scanCell(refCoords.X + dx, refCoords.Y + dy);
				if (!MapClass::Instance.CoordinatesLegal(scanCell))
				{
					continue;
				}

				double distSq = refCoords.DistanceFromSquared(scanCell);
				if (distSq > 729.0) // 35.0 * 35.0 = 1225.0
				{
					continue;
				}

				const CellClass* cell = MapClass::Instance.GetCellAt(scanCell);
				if (cell)
				{
					const bool within22 = (distSq <= 484.0); // 22.0 * 22.0 = 484.0

					// Ground Tiberium / Ore is accepted up to 35 cells
					if (cell->OverlayTypeIndex != -1 && OverlayClass::GetTiberiumType(cell->OverlayTypeIndex) >= 0)
					{
						hasResources = true;
						break;
					}

					// Tiberium Trees are ONLY accepted if within 14 cells
					if (within22)
					{
						const TerrainClass* pTerrain = cell->GetTerrain(false);
						if (pTerrain != nullptr && pTerrain->IsAlive && pTerrain->Type->SpawnsTiberium)
						{
							hasResources = true;
							break;
						}
					}
				}
			}
			if (hasResources)
			{
				break;
			}
		}

		if (!hasResources)
		{
			Debug::Log("AdvAI: Refinery %s at (%d,%d) is obsolete (no resources within 27 cells, no trees within 22 cells). Selling it.\n",
				pBld->Type->ID, refCoords.X, refCoords.Y);
			pBld->Sell(1);
		}
	}
}

void HouseExt::AdvAI_Add_Failed_Expansion_Point(HouseClass* pHouse, CellStruct coords)
{
	const auto houseExt = ExtMap.Find(pHouse);

	// 1. Check if this coordinate (or close to it, within 5.0 cells) is already blacklisted
	bool found = false;
	for (size_t i = 0; i < std::size(houseExt->PermanentlyBlockedExpansionPointLocations); i++)
	{
		auto& blocked = houseExt->PermanentlyBlockedExpansionPointLocations[i];
		if (blocked.Coords.X > 0 && blocked.Coords.Y > 0)
		{
			if (coords.DistanceFromSquared(blocked.Coords) < 25.0) // 5-cell radius
			{
				blocked.FailureCount++;
				blocked.Coords = coords; // Update exact coordinates

				// Linear backoff: 3 minutes * FailureCount
				// Cap multiplier at 240 to prevent extreme values (240 * 3 mins = 12 hours)
				int multiplier = std::min(blocked.FailureCount, 240);

				blocked.ExpiryFrame = Unsorted::CurrentFrame + (2700 * multiplier);

				Debug::Log("AdvAI: House %d updated failed expansion point (%d,%d). Failure count: %d, multiplier: %dx, cooldown: %d mins.\n",
					pHouse->ArrayIndex, coords.X, coords.Y, blocked.FailureCount, multiplier, 3 * multiplier);
				found = true;
				break;
			}
		}
	}

	// 2. If not found, add to an empty slot or recycle the oldest expired slot
	if (!found)
	{
		int bestIndex = -1;
		int oldestExpiry = std::numeric_limits<int>::max();

		for (size_t i = 0; i < std::size(houseExt->PermanentlyBlockedExpansionPointLocations); i++)
		{
			const auto& b = houseExt->PermanentlyBlockedExpansionPointLocations[i];
			if (b.Coords.X == 0 && b.Coords.Y == 0)
			{
				bestIndex = i;
				break;
			}
			if (Unsorted::CurrentFrame >= b.ExpiryFrame && b.ExpiryFrame < oldestExpiry)
			{
				oldestExpiry = b.ExpiryFrame;
				bestIndex = i;
			}
		}

		if (bestIndex != -1)
		{
			auto& blocked = houseExt->PermanentlyBlockedExpansionPointLocations[bestIndex];
			blocked.Coords = coords;
			blocked.FailureCount = 1;
			blocked.ExpiryFrame = Unsorted::CurrentFrame + 2700; // 3 minute cooldown at 15 FPS
			Debug::Log("AdvAI: House %d blacklisted failed expansion point (%d,%d) for 3 minutes.\n",
				pHouse->ArrayIndex, coords.X, coords.Y);
		}
	}
}

bool HouseExt::AdvAI_Is_Failed_Expansion_Point(HouseClass* pHouse, CellStruct coords)
{
	const auto houseExt = ExtMap.Find(pHouse);
	for (size_t i = 0; i < std::size(houseExt->PermanentlyBlockedExpansionPointLocations); i++)
	{
		const auto& blocked = houseExt->PermanentlyBlockedExpansionPointLocations[i];
		if (blocked.Coords.X > 0 && blocked.Coords.Y > 0)
		{
			// Entry is only active if the frame hasn't reached its individual expiry frame
			if (Unsorted::CurrentFrame < blocked.ExpiryFrame)
			{
				if (coords.DistanceFromSquared(blocked.Coords) < 225.0) // 15-cell radius
				{
					return true;
				}
			}
		}
	}
	return false;
}

bool HouseExt::AdvAI_Has_Failed_Placement_Three_Times(HouseClass* pHouse, CellStruct coords)
{
	const auto houseExt = ExtMap.Find(pHouse);
	for (size_t i = 0; i < std::size(houseExt->PermanentlyBlockedExpansionPointLocations); i++)
	{
		const auto& blocked = houseExt->PermanentlyBlockedExpansionPointLocations[i];
		if (blocked.Coords.X > 0 && blocked.Coords.Y > 0)
		{
			if (coords.DistanceFromSquared(blocked.Coords) < 225.0) // 15-cell radius
			{
				if (blocked.FailureCount >= 3)
				{
					return true;
				}
			}
		}
	}
	return false;
}


static int GetTiberiumSectorIndex(CellStruct coords)
{
	for (size_t i = 0; i < GlobalResourceSectors.size(); ++i)
	{
		const auto& sector = GlobalResourceSectors[i];
		if (coords.X >= sector.BoundsMin.X && coords.X < sector.BoundsMax.X &&
			coords.Y >= sector.BoundsMin.Y && coords.Y < sector.BoundsMax.Y)
		{
			return static_cast<int>(i);
		}
	}
	return -1;
}

void HouseExt::AdvAI_Update_Unclaimed_Tiberium_Zones(HouseClass* pHouse)
{
	const auto houseExt = ExtMap.Find(pHouse);
	if (houseExt == nullptr)
		return;

	// 1. Clean up existing registered coordinates in UnclaimedTiberiumZones
	auto& zones = houseExt->UnclaimedTiberiumZones;
	
	Debug::Log("AdvAI: Update_Unclaimed_Tiberium_Zones for House %d. Current tracked zones count: %d\n", pHouse->ArrayIndex, static_cast<int>(zones.size()));

	zones.erase(std::remove_if(zones.begin(), zones.end(), [pHouse](const CellStruct& coords) {
		const int failedIdx = GetTiberiumSectorIndex(coords);
		if (AdvAI_Has_Failed_Placement_Three_Times(pHouse, coords))
		{
			if (failedIdx >= 0)
				Debug::Log("AdvAI: Sector #%d at (%d,%d) has failed refinery placement 3 times. Removing permanently from tracking.\n", failedIdx, coords.X, coords.Y);
			else
				Debug::Log("AdvAI: Tree Node at (%d,%d) has failed refinery placement 3 times. Removing permanently from tracking.\n", coords.X, coords.Y);
			return true; // remove
		}

		bool stillHasTiberium = false;
		for (int dy = -4; dy <= 4; ++dy)
		{
			for (int dx = -4; dx <= 4; ++dx)
			{
				CellStruct cellCoords(static_cast<short>(coords.X + dx), static_cast<short>(coords.Y + dy));
				if (!MapClass::Instance.CoordinatesLegal(cellCoords))
					continue;

				const CellClass* cell = MapClass::Instance.GetCellAt(cellCoords);
				if (cell)
				{
					if (cell->OverlayTypeIndex != -1 && OverlayClass::GetTiberiumType(cell->OverlayTypeIndex) >= 0)
					{
						stillHasTiberium = true;
						break;
					}

					TerrainClass* pTerrain = cell->GetTerrain(false);
					if (pTerrain != nullptr && pTerrain->IsAlive && pTerrain->Type->SpawnsTiberium)
					{
						stillHasTiberium = true;
						break;
					}
				}
			}
			if (stillHasTiberium)
				break;
		}

		const int sIdx = GetTiberiumSectorIndex(coords);

		if (!stillHasTiberium)
		{
			if (sIdx >= 0)
				Debug::Log("AdvAI: Unclaimed tiberium zone Sector #%d at (%d,%d) has been mined out or cleared. Removing from tracking.\n", sIdx, coords.X, coords.Y);
			else
				Debug::Log("AdvAI: Unclaimed tiberium Tree Node at (%d,%d) has been mined out or cleared. Removing from tracking.\n", coords.X, coords.Y);
			return true; // remove
		}

		// Check if a refinery has been placed near it
		bool isTree = false;
		for (const auto pTerrain : TerrainClass::Array)
		{
			if (pTerrain->IsAlive && !pTerrain->InLimbo && pTerrain->Type->SpawnsTiberium && pTerrain->GetMapCoords() == coords)
			{
				isTree = true;
				break;
			}
		}
		const double refineryRange = 22.0;

		bool hasRefinery = false;
		for (const auto pBld : BuildingClass::Array)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner == pHouse && pBld->Type->ResourceDestination)
			{
				if (coords.DistanceFrom(pBld->GetMapCoords()) < refineryRange)
				{
					hasRefinery = true;
					break;
				}
			}
		}

		if (hasRefinery)
		{
			if (sIdx >= 0)
				Debug::Log("AdvAI: Refinery now exists near Sector #%d at (%d,%d). Removing from tracking.\n", sIdx, coords.X, coords.Y);
			else
				Debug::Log("AdvAI: Refinery now exists near Tree Node at (%d,%d). Removing from tracking.\n", coords.X, coords.Y);
			return true; // remove
		}

		return false; // keep
	}), zones.end());

	// 2. Discover new unclaimed tiberium zones near our base normal structures
	struct CandidateZone
	{
		CellStruct Coords;
		int SectorIndex; // -1 if tree node
	};
	std::vector<CandidateZone> candidates;

	// Ground resource sectors
	for (size_t idx = 0; idx < GlobalResourceSectors.size(); ++idx)
	{
		const auto& sector = GlobalResourceSectors[idx];
		if (sector.HasResources)
		{
			candidates.push_back({ sector.CachedCoords, static_cast<int>(idx) });
		}
	}

	// Tiberium tree structures near our base (from cached list)
	for (const auto& treeCoords : GlobalTiberiumTrees)
	{
		candidates.push_back({ treeCoords, -1 });
	}

	Debug::Log("AdvAI: House %d: Scanned %d candidates (ground sectors + trees).\n", pHouse->ArrayIndex, static_cast<int>(candidates.size()));

	// Filter candidates close to our base buildings and without refineries
	for (const auto& candidate : candidates)
	{
		const CellStruct targetCoords = candidate.Coords;
		const int sIdx = candidate.SectorIndex;
		const bool isTree = (sIdx == -1);
		const double maxBaseDistance = isTree ? 22.0 : 27.0;

		bool isNearBase = false;
		double minBldDist = 9999.0;
		BuildingClass* pClosestBld = nullptr;
		for (const auto pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo)
			{
				const auto pBldExt = BuildingTypeExt::ExtMap.Find(pBld->Type);
				if (pBldExt->AIBaseNormal.Get(pBld->Type->BaseNormal))
				{
					double d = targetCoords.DistanceFrom(pBld->GetMapCoords());
					if (d < minBldDist)
					{
						minBldDist = d;
						pClosestBld = pBld;
					}
					if (d < maxBaseDistance)
					{
						isNearBase = true;
					}
				}
			}
		}

		if (!isNearBase)
		{
			continue;
		}

		if (AdvAI_Has_Failed_Placement_Three_Times(pHouse, targetCoords))
		{
			continue; // Skip permanently blocked/failed locations
		}

		// Check if already in UnclaimedTiberiumZones
		bool alreadyRegistered = std::find(zones.begin(), zones.end(), targetCoords) != zones.end();
		if (alreadyRegistered)
		{
			/*
			if (sIdx >= 0)
				Debug::Log("AdvAI: Candidate Sector #%d at (%d,%d) already registered in vector. Vector size: %d.\n", sIdx, targetCoords.X, targetCoords.Y, static_cast<int>(zones.size()));
			else
				Debug::Log("AdvAI: Candidate Tree Node at (%d,%d) already registered in vector. Vector size: %d.\n", targetCoords.X, targetCoords.Y, static_cast<int>(zones.size()));
			*/
			continue;
		}

		// Check if a refinery already exists near this coordinate
		const double refineryRange = 22.0;
		bool hasRefinery = false;
		BuildingClass* pClashingRefinery = nullptr;
		for (const auto pBld : BuildingClass::Array)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner == pHouse && pBld->Type->ResourceDestination)
			{
				double d = targetCoords.DistanceFrom(pBld->GetMapCoords());
				if (d < refineryRange)
				{
					hasRefinery = true;
					pClashingRefinery = pBld;
					break;
				}
			}
		}

		if (hasRefinery)
		{
			/*
			if (sIdx >= 0)
				Debug::Log("AdvAI: Candidate Sector #%d at (%d,%d) is already covered by refinery %s at (%d,%d) (distance: %.1f < %.1f).\n",
					sIdx, targetCoords.X, targetCoords.Y, pClashingRefinery->Type->ID, pClashingRefinery->GetMapCoords().X, pClashingRefinery->GetMapCoords().Y,
					targetCoords.DistanceFrom(pClashingRefinery->GetMapCoords()), refineryRange);
			else
				Debug::Log("AdvAI: Candidate Tree Node at (%d,%d) is already covered by refinery %s at (%d,%d) (distance: %.1f < %.1f).\n",
					targetCoords.X, targetCoords.Y, pClashingRefinery->Type->ID, pClashingRefinery->GetMapCoords().X, pClashingRefinery->GetMapCoords().Y,
					targetCoords.DistanceFrom(pClashingRefinery->GetMapCoords()), refineryRange);
			*/
			continue;
		}

		zones.push_back(targetCoords);
		if (sIdx >= 0)
			Debug::Log("AdvAI: Registered new unclaimed Sector #%d at (%d,%d) near building %s (distance: %.1f < %.1f). Vector size: %d.\n",
				sIdx, targetCoords.X, targetCoords.Y, pClosestBld ? pClosestBld->Type->ID : "???", minBldDist, maxBaseDistance, static_cast<int>(zones.size()));
		else
			Debug::Log("AdvAI: Registered new unclaimed Tree Node at (%d,%d) near building %s (distance: %.1f < %.1f). Vector size: %d.\n",
				targetCoords.X, targetCoords.Y, pClosestBld ? pClosestBld->Type->ID : "???", minBldDist, maxBaseDistance, static_cast<int>(zones.size()));
	}
}

CellStruct HouseExt::ExtData::GetCrawlingWaypoint(CellStruct targetCell)
{
	if (targetCell.X <= 0 || targetCell.Y <= 0)
	{
		return targetCell;
	}

	const auto pHouse = this->OwnerObject();

	// Find closest building to the target to start the path
	BuildingClass* pStartBld = nullptr;
	double minDist = 99999.0;
	for (const auto pBld : pHouse->Buildings)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo)
		{
			double dist = pBld->GetMapCoords().DistanceFrom(targetCell);
			if (dist < minDist)
			{
				minDist = dist;
				pStartBld = pBld;
			}
		}
	}

	CellStruct currentStartCoords = pStartBld ? pStartBld->GetMapCoords() : CellStruct(0, 0);

	// 1. Manage Cached A* Path (Recalculate if target changes OR if our bridgehead building changes)
	if (this->CachedExpansionPathTarget != targetCell || this->CachedExpansionPathStart != currentStartCoords || this->CachedExpansionPath.empty())
	{
		if (pStartBld != nullptr)
		{
			this->CachedExpansionPath = GeneralUtils::GetAStarPath(currentStartCoords, targetCell, MovementZone::Normal);
			this->CachedExpansionPathTarget = targetCell;
			this->CachedExpansionPathStart = currentStartCoords;

			if (!this->CachedExpansionPath.empty())
			{
				Debug::Log("AdvAI: Recalculated A* crawl path from (%d,%d) to (%d,%d). Path size: %d cells.\n",
					currentStartCoords.X, currentStartCoords.Y, targetCell.X, targetCell.Y, static_cast<int>(this->CachedExpansionPath.size()));
			}
		}
		else
		{
			this->CachedExpansionPath.clear();
			this->CachedExpansionPathTarget = CellStruct(0, 0);
			this->CachedExpansionPathStart = CellStruct(0, 0);
		}
	}

	if (this->CachedExpansionPath.empty())
	{
		return targetCell;
	}

	// 2. Find the furthest cell on the path that is within building adjacency range
	size_t furthestIdx = 0;
	bool foundAny = false;

	for (size_t i = 0; i < this->CachedExpansionPath.size(); ++i)
	{
		bool closeToExisting = false;
		for (const auto pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo)
			{
				// Typically build range is up to 15.0 cells
				if (pBld->GetMapCoords().DistanceFrom(this->CachedExpansionPath[i]) <= 15.0)
				{
					closeToExisting = true;
					break;
				}
			}
		}

		if (closeToExisting)
		{
			furthestIdx = i;
			foundAny = true;
		}
	}

	if (!foundAny)
	{
		return targetCell;
	}

	// 3. Set waypoint to be 10 cells further along the path
	size_t waypointIdx = furthestIdx + 10;
	if (waypointIdx >= this->CachedExpansionPath.size())
	{
		return targetCell;
	}

	return this->CachedExpansionPath[waypointIdx];
}
