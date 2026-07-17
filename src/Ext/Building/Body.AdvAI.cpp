#include "Body.h"
#include "Ext/House/Body.h"
#include <algorithm>
#include "Ext/TechnoType/Body.h"
#include "Ext/TerrainType/Body.h"
#include "Ext/SWType/Body.h"
#include <CellClass.h>
#include <MapClass.h>
#include <OverlayClass.h>
#include <TerrainClass.h>

enum class SupportRadiusType { None, Cloak, Gap, Inhibitor, RadarJam, EMPulseCannon };

static bool IsAIBaseNormal(const BuildingTypeClass* pType);
static bool HasWeapons(TechnoClass* pTechno);
static bool IsAIInnerBase(const BuildingTypeClass* pType);
static SupportRadiusType GetSupportRadiusType(const BuildingTypeClass* pType);
static bool IsSameSupportNetwork(const BuildingTypeClass* pTypeA, const BuildingTypeClass* pTypeB);
static int GetSupportRadius(const BuildingTypeClass* pType);
static const char* GetGroupAsID(BuildingTypeClass* pType);

static bool IsAIBaseNormal(const BuildingTypeClass* pType)
{
	const auto pExt = BuildingTypeExt::ExtMap.Find(pType);
	return pExt->AIBaseNormal.Get(pType->BaseNormal);
}

static bool HasWeapons(TechnoClass* pTechno);

static bool IsAIInnerBase(const BuildingTypeClass* pType)
{
	const auto pExt = BuildingTypeExt::ExtMap.Find(pType);
	if (pExt->AIInnerBase.isset())
	{
		return pExt->AIInnerBase.Get();
	}
	return pType->CloakGenerator || pExt->GapGenerator;
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

bool BuildingExt::OverlapsTiberiumTreeZone(CellStruct cell, BuildingTypeClass* pType)
{
	if (pType->ResourceDestination)
	{
		return false;
	}
	const int foundationW = pType->GetFoundationWidth();
	const int foundationH = pType->GetFoundationHeight(false);

	// Scan an area around the proposed building large enough to cover the max spawning range of any nearby tree.
	// Typically, the spawning range is 1. If we scan up to 3 cells away from the foundation boundary,
	// we will definitely find any relevant trees.
	const int scanMargin = 3;

	for (int dy = -scanMargin; dy < foundationH + scanMargin; ++dy)
	{
		for (int dx = -scanMargin; dx < foundationW + scanMargin; ++dx)
		{
			CellStruct checkCell(cell.X + dx, cell.Y + dy);
			if (!MapClass::Instance.CoordinatesLegal(checkCell))
			{
				continue;
			}

			const CellClass* pCell = MapClass::Instance.GetCellAt(checkCell);
			if (pCell)
			{
				const TerrainClass* pTerrain = pCell->GetTerrain(false);
				if (pTerrain != nullptr && pTerrain->IsAlive && pTerrain->Type->SpawnsTiberium)
				{
					// Found a tree! Get its spawning range.
					const auto pTerrainTypeExt = TerrainTypeExt::ExtMap.Find(pTerrain->Type);
					const int range = pTerrainTypeExt ? pTerrainTypeExt->SpawnsTiberium_Range : 1;

					// Now check if this tree's spawning zone overlaps the building's foundation.
					// The tree's spawning zone is: all cells within 'range' Chebyshev distance from the tree.
					// So for each cell F in the building's foundation:
					// if max(abs(F.X - tree.X), abs(F.Y - tree.Y)) <= range, then it overlaps!
					bool overlaps = false;
					for (int fy = 0; fy < foundationH; ++fy)
					{
						for (int fx = 0; fx < foundationW; ++fx)
						{
							const int fX = cell.X + fx;
							const int fY = cell.Y + fy;
							const int dist = std::max(std::abs(fX - checkCell.X), std::abs(fY - checkCell.Y));
							if (dist <= range)
							{
								overlaps = true;
								break;
							}
						}
						if (overlaps)
						{
							break;
						}
					}

					if (overlaps)
					{
						return true;
					}
				}
			}
		}
	}

	return false;
}



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


#define	REFRESH_EOL         32767		// This number ends a refresh/occupy offset list.

#define TICKS_PER_SECOND    15
#define TICKS_PER_MINUTE    (TICKS_PER_SECOND * 60)
#define TICKS_PER_HOUR      (TICKS_PER_MINUTE * 60)

///
/// Advanced AI
///	Credits to Rampastring
///

HouseClass* BuildingExt::Find_Closest_Opponent(const HouseClass* pHouse)
{
	double nearestDistance = std::numeric_limits<double>::max();
	HouseClass* pNearestHouse = nullptr;

	for (const auto pOtherHouse : HouseClass::Array)
	{
		if (pOtherHouse == pHouse)
		{
			continue;
		}

		if (pOtherHouse->Type->MultiplayPassive)
		{
			continue;
		}

		if (pHouse->IsAlliedWith(pOtherHouse))
		{
			continue;
		}

		const double distance = pHouse->Base_Center().DistanceFrom(pOtherHouse->Base_Center());

		if (distance < nearestDistance)
		{
			nearestDistance = distance;
			pNearestHouse = pOtherHouse;
		}
	}

	return pNearestHouse;
}

int BuildingExt::Get_Distance_To_Primary_Enemy(CellStruct cell, HouseClass* pHouse)
{
	HouseClass* enemy = nullptr;

	if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
	{
		enemy = HouseClass::Array[pHouse->EnemyHouseIndex];
	}

	if (enemy == nullptr)
	{
		enemy = Find_Closest_Opponent(pHouse);
	}

	int enemyDistance = 0;

	if (enemy != nullptr)
	{
		enemyDistance = static_cast<int>(cell.DistanceFrom(enemy->Base_Center()));
	}

	return enemyDistance;
}

/**
*  Stores a record of buildings owned by the house
*  that is currently placing down a building.
*  Allows performing lookups on them without
*  needing to go through the list of all buildings on
*  the entire map.
*/
BuildingClass* BuildingExt::ExtData::OurBuildings[1000];
size_t BuildingExt::ExtData::OurBuildingCount;
BuildingClass* BuildingExt::ExtData::AdjacencyAnchors[1000];
size_t BuildingExt::ExtData::AdjacencyAnchorCount;

void BuildingExt::Mark_Expansion_As_Done(HouseClass* pHouse)
{
	const auto ext = HouseExt::ExtMap.Find(pHouse);

	if (ext->NextExpansionPointLocation.X == 0 || ext->NextExpansionPointLocation.Y == 0)
		return;

	ext->NextExpansionPointLocation = CellStruct(0, 0);
	ext->ShouldBuildRefinery = false;

	// Set timer to clear the blocked list in 10 minutes (9000 frames at 15 FPS)
	ext->NextBlacklistClearFrame = Unsorted::CurrentFrame + 9000;
}

int BuildingExt::Try_Place(BuildingClass* pBuilding, CellStruct cell)
{
	HouseClass* owner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(owner);

	if (!pBuilding->Type->CanPlaceHere(&cell, owner))
		return 1;

	const CellStruct finalPlacementCell = cell;
	CoordStruct coord = GeneralUtils::CoordinatesFromCell(finalPlacementCell);

	if (pBuilding->Unlimbo(coord, DirType::North))
	{
		owner->ProducingBuildingTypeIndex = -1;

		// This is necessary or the building's build-up anim is played twice.
		// RA doesn't do this, must be a difference somewhere in the engine.
		pBuilding->QueueMission(Mission::Construction, true);
		pBuilding->NextMission();

		int closeEnough = 8;

		// If we just placed down our first barracks, then set our team timer to 0
		// so we can immediately start producing infantry.
		// Do not do this on Easy mode to avoid overwhelming the player.
		if (owner->AIDifficulty < AIDifficulty::Hard &&
			!houseExt->HasBuiltFirstBarracks &&
			pBuilding->Type->Factory == AbstractType::InfantryType)
		{
			houseExt->HasBuiltFirstBarracks = true;
			owner->TeamDelayTimer.Start(0);
		}

		// Local Frontline Threat Detection:
		// If we placed a building (e.g. powerplant/refinery) and there is an enemy unit or structure within 8.0 cells:
		// flag a local threat and request 2 defenses to be built in direction to the enemy threat.
		bool threatFound = false;
		CellStruct threatCoords(0, 0);
		double nearestThreatDistSq = 64.0; // 8.0 cells squared
		const char* threatID = nullptr;
		const char* threatHouseID = nullptr;

		// Check enemy buildings
		for (const auto pBld : BuildingClass::Array)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner != owner && !owner->IsAlliedWith(pBld->Owner))
			{
				// Ignore neutral buildings unless they have weapons (e.g. civilian defenses)
				if (pBld->Owner->IsNeutral())
				{
					const auto& primary = pBld->Type->GetWeapon(0, false);
					const auto& secondary = pBld->Type->GetWeapon(1, false);
					if (primary.WeaponType == nullptr && secondary.WeaponType == nullptr)
						continue;
				}

				double distSq = cell.DistanceFromSquared(pBld->GetMapCoords());
				if (distSq < nearestThreatDistSq)
				{
					nearestThreatDistSq = distSq;
					threatCoords = pBld->GetMapCoords();
					threatID = pBld->Type->ID;
					threatHouseID = pBld->Owner->Type->ID;
					threatFound = true;
				}
			}
		}

		// Check enemy units (infantry, vehicles, aircraft)
		for (const auto pFoot : FootClass::Array)
		{
			if (pFoot && pFoot->IsAlive && !pFoot->InLimbo && pFoot->Owner != owner && !owner->IsAlliedWith(pFoot->Owner))
			{
				// Ignore neutral units unless they have weapons (e.g. avoid triggering on cows/ambient animals)
				if (pFoot->Owner->IsNeutral() && !HasWeapons(pFoot))
					continue;

				double distSq = cell.DistanceFromSquared(pFoot->GetMapCoords());
				if (distSq < nearestThreatDistSq)
				{
					nearestThreatDistSq = distSq;
					threatCoords = pFoot->GetMapCoords();				threatID = pFoot->GetTechnoType()->ID;
					threatHouseID = pFoot->Owner->Type->ID;
					threatFound = true;
				}
			}
		}

		if (threatFound)
		{
			houseExt->FrontlineThreatCoords = threatCoords;
			houseExt->FrontlineThreatBuildingCoords = cell;
			houseExt->FrontlineThreatActiveFrames = Unsorted::CurrentFrame + 1800; // 2 minutes active
			houseExt->FrontlineThreatNeedsDefenses = 2; // Request 2 local defenses
			Debug::Log("AdvAI Crawler: Placed building %s at (%d,%d) near enemy at (%d,%d). Local threat detected! Threat: %s (House: %s). Requesting 2 defenses.\n",
				pBuilding->Type->ID, cell.X, cell.Y, threatCoords.X, threatCoords.Y,
				threatID ? threatID : "???", threatHouseID ? threatHouseID : "???");
		}

		// If we placed a base defense and a threat is active, decrement the threat defense requirement
		if (TechTreeTypeClass::TotalBuildDefense.contains(pBuilding->Type))
		{
			if (houseExt->FrontlineThreatCoords.X > 0 && houseExt->FrontlineThreatActiveFrames > Unsorted::CurrentFrame && houseExt->FrontlineThreatNeedsDefenses > 0)
			{
				houseExt->FrontlineThreatNeedsDefenses--;
				Debug::Log("AdvAI Crawler: Placed defense %s at (%d,%d). Remaining defenses needed for local threat: %d.\n",
					pBuilding->Type->ID, cell.X, cell.Y, houseExt->FrontlineThreatNeedsDefenses);
				if (houseExt->FrontlineThreatNeedsDefenses == 0)
				{
					houseExt->FrontlineThreatCoords = CellStruct(0, 0);
				}
			}
		}

		// Check if we placed a refinery.
		// If yes, check if we were expanding. If yes, the expanding is done.
		// If not, but we're close to an expansion field, then flag us to build a refinery as our next building.
		if (pBuilding->Type->ResourceDestination)
		{
			if (houseExt->NextRefineryPlacementLocation.X > 0 && houseExt->NextRefineryPlacementLocation.Y > 0)
			{
				CellStruct targetZone = houseExt->NextRefineryPlacementLocation;
				auto& zones = houseExt->UnclaimedTiberiumZones;
				zones.erase(std::remove(zones.begin(), zones.end(), targetZone), zones.end());
				houseExt->NextRefineryPlacementLocation = CellStruct(0, 0);
				Debug::Log("AdvAI: Successfully placed refinery %s at (%d,%d) to claim tiberium zone at (%d,%d). Remaining unclaimed zones in list: %d.\n",
					pBuilding->Type->ID, cell.X, cell.Y, targetZone.X, targetZone.Y, static_cast<int>(zones.size()));
			}

			bool completedExpansion = false;
			if (houseExt->NextExpansionPointLocation.X != 0 && houseExt->NextExpansionPointLocation.Y != 0)
			{
				const double distToTarget = cell.DistanceFrom(houseExt->NextExpansionPointLocation);
				if (houseExt->ShouldBuildRefinery || distToTarget < 22.0)
				{
					const auto buildingExt = ExtMap.Find(pBuilding);
					buildingExt->AssignedExpansionPoint = houseExt->NextExpansionPointLocation;
					completedExpansion = true;
				}
			}

			if (completedExpansion)
			{
				const CellStruct targetCell = houseExt->NextExpansionPointLocation;
				for (size_t i = 0; i < std::size(houseExt->PermanentlyBlockedExpansionPointLocations); i++)
				{
					auto& blocked = houseExt->PermanentlyBlockedExpansionPointLocations[i];
					if (blocked.Coords.X > 0 && blocked.Coords.Y > 0)
					{
						if (targetCell.DistanceFromSquared(blocked.Coords) < 225.0) // 15-cell radius
						{
							blocked.Coords = CellStruct(0, 0);
							blocked.ExpiryFrame = 0;
							blocked.FailureCount = 0;
							Debug::Log("AdvAI: House %d successfully placed refinery at (%d,%d). Cleared failed expansion history for this area.\n",
								owner->ArrayIndex, targetCell.X, targetCell.Y);
						}
					}
				}

				Mark_Expansion_As_Done(owner);
				houseExt->ShouldBuildRefinery = false;
			}
		}
		else if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
		{
			const double distToTarget = GeneralUtils::CellFromCoordinates(pBuilding->GetCenterCoords()).DistanceFrom(houseExt->NextExpansionPointLocation);

			// If we are crawling towards the enemy base and have successfully reached within 12.0 cells of the target,
			// mark the combat crawl as completed to prevent endless powerplant spam near the enemy base.
			bool isEnemyBaseTarget = false;
			for (const auto pEnemyHouse : HouseClass::Array)
			{
				if (pEnemyHouse != owner && !owner->IsAlliedWith(pEnemyHouse))
				{
					for (const auto pBld : pEnemyHouse->Buildings)
					{
						if (pBld && pBld->IsAlive && !pBld->InLimbo)
						{
							if (pBld->GetMapCoords().DistanceFrom(houseExt->NextExpansionPointLocation) <= 5.0)
							{
								isEnemyBaseTarget = true;
								break;
							}
						}
					}
				}
				if (isEnemyBaseTarget)
					break;
			}

			if (isEnemyBaseTarget && distToTarget < 12.0)
			{
				Debug::Log("AdvAI Crawler: Reached enemy base target (%d,%d) within %.1f cells. Clearing combat crawl target to prevent spam.\n",
					houseExt->NextExpansionPointLocation.X, houseExt->NextExpansionPointLocation.Y, distToTarget);
				Mark_Expansion_As_Done(owner);
			}
			else
			{
				if (distToTarget < 12.0)
				{
					const CellStruct buildingCell = GeneralUtils::CellFromCoordinates(pBuilding->GetCenterCoords());
					bool foundTiberium = false;
					for (int dy = -8; dy <= 8; ++dy)
					{
						for (int dx = -8; dx <= 8; ++dx)
						{
							CellStruct scanCell(buildingCell.X + dx, buildingCell.Y + dy);
							if (!MapClass::Instance.CoordinatesLegal(scanCell))
								continue;

							if (buildingCell.DistanceFrom(scanCell) > 8.0)
								continue;

							const CellClass* cell = MapClass::Instance.GetCellAt(scanCell);
							if (cell)
							{
								if (cell->OverlayTypeIndex != -1 && OverlayClass::GetTiberiumType(cell->OverlayTypeIndex) >= 0)
								{
									foundTiberium = true;
									break;
								}

								TerrainClass* pTerrain = cell->GetTerrain(false);
								if (pTerrain != nullptr && pTerrain->IsAlive && pTerrain->Type->SpawnsTiberium)
								{
									foundTiberium = true;
									break;
								}
							}
						}
						if (foundTiberium)
							break;
					}

					if (foundTiberium)
					{
						houseExt->ShouldBuildRefinery = true;
						Debug::Log("AdvAI Crawler: Placed building %s at (%d,%d) within %.1f cells of target (%d,%d). Tiberium detected within 8.0 cells in the same zone! Setting ShouldBuildRefinery = true.\n",
							pBuilding->Type->ID, buildingCell.X, buildingCell.Y, distToTarget, houseExt->NextExpansionPointLocation.X, houseExt->NextExpansionPointLocation.Y);
					}
				}

				if (distToTarget < closeEnough)
					houseExt->ShouldBuildRefinery = true;
			}

			// Congestion Detection Check:
			// If this new building is touching another building, and there is a third building
			// touching either of them, mark this area as unsafe/congested (not recommended for expansion)
			// for the next 4500 frames (5 minutes).
			auto AreBuildingsTouching = [&cell, pBuilding](const BuildingClass* b2) -> bool {
				if (!b2) return false;
				const int b1X = cell.X;
				const int b1Y = cell.Y;
				const int b1W = pBuilding->Type->GetFoundationWidth();
				const int b1H = pBuilding->Type->GetFoundationHeight(false);

				const int b2X = b2->GetMapCoords().X;
				const int b2Y = b2->GetMapCoords().Y;
				const int b2W = b2->Type->GetFoundationWidth();
				const int b2H = b2->Type->GetFoundationHeight(false);

				return (b1X - 1 <= b2X + b2W - 1) && (b1X + b1W >= b2X) &&
					   (b1Y - 1 <= b2Y + b2H - 1) && (b1Y + b1H >= b2Y);
			};

			auto AreTwoBuildingsTouching = [](const BuildingClass* b1, const BuildingClass* b2) -> bool {
				if (!b1 || !b2 || b1 == b2) return false;
				const int b1X = b1->GetMapCoords().X;
				const int b1Y = b1->GetMapCoords().Y;
				const int b1W = b1->Type->GetFoundationWidth();
				const int b1H = b1->Type->GetFoundationHeight(false);

				const int b2X = b2->GetMapCoords().X;
				const int b2Y = b2->GetMapCoords().Y;
				const int b2W = b2->Type->GetFoundationWidth();
				const int b2H = b2->Type->GetFoundationHeight(false);

				return (b1X - 1 <= b2X + b2W - 1) && (b1X + b1W >= b2X) &&
					   (b1Y - 1 <= b2Y + b2H - 1) && (b1Y + b1H >= b2Y);
			};

			std::vector<const BuildingClass*> touchingNew;
			for (const auto pBld : BuildingClass::Array)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld != pBuilding)
				{
					if (pBld->Owner == owner || owner->IsAlliedWith(pBld->Owner))
					{
						if (AreBuildingsTouching(pBld))
						{
							touchingNew.push_back(pBld);
						}
					}
				}
			}

			bool isCongested = false;
			if (!touchingNew.empty())
			{
				// Check 1: 2 touching of the same building type
				for (const auto pBld : touchingNew)
				{
					if (pBld->Type == pBuilding->Type)
					{
						isCongested = true;
						break;
					}
				}

				// Check 2: 3+ cluster of touching buildings
				if (!isCongested)
				{
					if (touchingNew.size() >= 2)
					{
						isCongested = true;
					}
					else
					{
						const BuildingClass* b2 = touchingNew[0];
						for (const auto pBld : BuildingClass::Array)
						{
							if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld != pBuilding && pBld != b2)
							{
								if (pBld->Owner == owner || owner->IsAlliedWith(pBld->Owner))
								{
									if (AreTwoBuildingsTouching(b2, pBld))
									{
										isCongested = true;
										break;
									}
								}
							}
						}
					}
				}
			}

			Debug::Log("AdvAI Crawler: Placed building %s at (%d,%d). Touching: %d existing buildings. Congested: %s\n",
				pBuilding->Type->ID, cell.X, cell.Y,
				static_cast<int>(touchingNew.size()), isCongested ? "YES" : "NO");

			if (isCongested)
			{
				houseExt->UnsafePlacementZones.push_back({ cell, Unsorted::CurrentFrame + 4500 });
				Debug::Log("AdvAI Crawler: Congestion detected around %s at (%d,%d). Marking area within 10.0 cells as not recommended for expansion for 4500 frames. Abandoning current expansion node.\n",
					pBuilding->Type->ID, cell.X, cell.Y);

				// Blacklist this target to prevent endless loop crawls if we get congested
				HouseExt::AdvAI_Add_Failed_Expansion_Point(owner, houseExt->NextExpansionPointLocation);

				Mark_Expansion_As_Done(owner);
				houseExt->ShouldBuildRefinery = false;
			}
		}

		return 2;
	}

	return 0;
}

/**
 *  Fetches a house's base area as a rectangle.
 *  We can use this as a rough zone for placing new buildings.
 */
static bool CanAIBuildOffThisAllyBuilding(HouseClass* pOwner, BuildingTypeClass* pBuildingType, BuildingClass* pAlliedBuilding)
{
	bool buildOffAlly = SessionClass::IsCampaign() ? RulesClass::Instance->BuildOffAlly : GameModeOptionsClass::Instance.BuildOffAlly;
	if (!buildOffAlly || pAlliedBuilding == nullptr || pBuildingType == nullptr)
		return false;

	// Refineries and resource destinations must never be built off allies
	if (pBuildingType->Refinery || pBuildingType->ResourceDestination)
		return false;

	const auto houseExt = HouseExt::ExtMap.Find(pOwner);
	const auto pOtherOwner = pAlliedBuilding->Owner;

	// 1. Defenses (joint defense): Allowed if the ally was recently attacked (and has valid target coords)
	const bool isDefense = pBuildingType->IsBaseDefense || pBuildingType->GetWeapon(0u, false).WeaponType != nullptr || pBuildingType->GetWeapon(1u, false).WeaponType != nullptr;
	if (isDefense && pOtherOwner != nullptr && pOtherOwner->LATime > 0)
	{
		int otherParanoia = TICKS_PER_MINUTE + (30 * TICKS_PER_SECOND);

		if (pOtherOwner->LATime + otherParanoia + 1800 > Unsorted::CurrentFrame)
		{
			const auto otherHouseExt = HouseExt::ExtMap.Find(pOtherOwner);
			if (otherHouseExt->LastAttackedBuildingCoords.X > 0)
				return true;
		}
	}

	// 2. Backup factories & Service Depots: Allowed in late game for security
	if (Unsorted::CurrentFrame > 9900)
	{
		const auto pTechTree = TechTreeTypeClass::GetAnySuitable(pOwner);
		if (pTechTree != nullptr)
		{
			const bool isWF = pBuildingType->Factory == AbstractType::UnitType && !pBuildingType->Naval;
			const bool isBarracks = pBuildingType->Factory == AbstractType::InfantryType;

			bool isDepot = false;
			for (const auto pType : pTechTree->BuildServiceDepot)
			{
				if (pBuildingType == pType)
				{
					isDepot = true;
					break;
				}
			}

			bool isTech = false;
			for (const auto pType : pTechTree->BuildTech)
			{
				if (pBuildingType == pType)
				{
					isTech = true;
					break;
				}
			}

			if (isWF || isBarracks || isDepot || isTech)
				return true;
		}
	}

	// 3. Crawler/Expansion: Allowed if we are actively expanding, the allied building is on the way, AND it is a Power Plant (crawling structure)
	const bool isPowerPlant = TechTreeTypeClass::TotalBuildPower.count(pBuildingType) > 0 || TechTreeTypeClass::TotalBuildAdvancedPower.count(pBuildingType) > 0;
	if (isPowerPlant && houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		const BuildingClass* pOurConYard = pOwner->ConYards.Count > 0 ? pOwner->ConYards[0] : nullptr;
		const CellStruct conyardCell = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pOwner->Base_Center();
		const double conyardDistToTarget = conyardCell.DistanceFrom(houseExt->NextExpansionPointLocation);
		const double allyDistToTarget = pAlliedBuilding->GetMapCoords().DistanceFrom(houseExt->NextExpansionPointLocation);

		if (allyDistToTarget < conyardDistToTarget)
			return true;
	}

	return false;
}

RectangleStruct BuildingExt::Get_Base_Rect(HouseClass* pHouse, int adjacency, int width, int height, BuildingTypeClass* pBuildingType)
{
	int x = INT_MAX;
	int y = INT_MAX;
	int right = INT_MIN;
	int bottom = INT_MIN;

	for (const auto building : BuildingClass::Array)
	{
		if (!building->IsAlive || building->InLimbo)
		{
			continue;
		}

		const bool isOwner = building->Owner == pHouse;
		const bool isEligibleAlly = building->Owner != nullptr && pHouse->IsAlliedWith(building->Owner) && building->Type->EligibileForAllyBuilding &&
			CanAIBuildOffThisAllyBuilding(pHouse, pBuildingType, building);

		if (isOwner || isEligibleAlly)
		{
			const CellStruct buildingCell = building->GetMapCoords();
			if (buildingCell.X < x)
				x = buildingCell.X;

			if (buildingCell.Y < y)
				y = buildingCell.Y;

			const int buildingRight = buildingCell.X + building->Type->GetFoundationWidth() - 1;
			if (buildingRight > right)
				right = buildingRight;

			const int buildingBottom = buildingCell.Y + building->Type->GetFoundationHeight(false) - 1;
			if (buildingBottom > bottom)
				bottom = buildingBottom;
		}
	}

	x -= adjacency;
	x -= width;
	y -= adjacency;
	y -= height;
	right += adjacency + width;
	bottom += adjacency + height;

	return RectangleStruct { x, y, right - x, bottom - y };
}

/**
 *  Checks whether a cell should be evaluated for AI building placement.
 */
bool BuildingExt::Should_Evaluate_Cell_For_Placement(CellStruct cell, BuildingClass* pBuilding, int adjacencyBonus)
{
	if (pBuilding == nullptr || pBuilding->Owner == nullptr)
		return false;

	const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
	bool isCellUnsafe = false;
	for (auto it = houseExt->UnsafePlacementZones.begin(); it != houseExt->UnsafePlacementZones.end(); )
	{
		if (Unsorted::CurrentFrame > it->ExpiryFrame)
			it = houseExt->UnsafePlacementZones.erase(it);
		else
		{
			if (cell.DistanceFromSquared(it->Coords) < 100.0) // 10-cell radius
			{
				isCellUnsafe = true;
				break;
			}
			++it;
		}
	}

	if (isCellUnsafe)
	{
		if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
			return false; // Strictly enforce unsafe/congested zones during expansion crawling

		const BuildingClass* pOurConYard = pBuilding->Owner->ConYards.Count > 0 ? pBuilding->Owner->ConYards[0] : nullptr;
		if (pOurConYard == nullptr || cell.DistanceFromSquared(pOurConYard->GetMapCoords()) >= 400.0)
			return false;
	}

	bool result = false;
	const bool isNaval = pBuilding->Type->Naval;

	for (size_t i = 0; i < ExtData::AdjacencyAnchorCount; i++)
	{
		const BuildingClass* pOtherBuilding = ExtData::AdjacencyAnchors[i];
		int adjacency = pBuilding->Type->Adjacent + adjacencyBonus;

		if (isNaval && pOtherBuilding->Type->ConstructionYard)
			adjacency = RulesClass::Instance->AINavalYardAdjacency + adjacencyBonus;

		const int otherW = pOtherBuilding->Type->GetFoundationWidth();
		const int otherH = pOtherBuilding->Type->GetFoundationHeight(false);
		const CellStruct origin = pOtherBuilding->GetMapCoords();

		const int newW = pBuilding->Type->GetFoundationWidth();
		const int newH = pBuilding->Type->GetFoundationHeight(false);

		// Fast Bounding Box check:
		const int dx = (cell.X > origin.X + otherW - 1) ? (cell.X - (origin.X + otherW - 1)) 
		             : ((cell.X + newW - 1 < origin.X) ? (origin.X - (cell.X + newW - 1)) : 0);
		if (dx > adjacency)
			continue;

		const int dy = (cell.Y > origin.Y + otherH - 1) ? (cell.Y - (origin.Y + otherH - 1)) 
		             : ((cell.Y + newH - 1 < origin.Y) ? (origin.Y - (cell.Y + newH - 1)) : 0);
		if (dy > adjacency)
			continue;

		if (!result)
		{
			bool pass = false;
			CellStruct const* occupy = pOtherBuilding->GetFoundationData(true);
			while (occupy->X != REFRESH_EOL && occupy->Y != REFRESH_EOL)
			{
				const CellStruct sum = origin + *occupy;
				CellStruct const* newOccupy = pBuilding->GetFoundationData(true);
				while (newOccupy->X != REFRESH_EOL && newOccupy->Y != REFRESH_EOL)
				{
					const CellStruct newSum = cell + *newOccupy;

					int xDiff = std::abs(newSum.X - sum.X);
					int yDiff = std::abs(newSum.Y - sum.Y);

					if (xDiff <= adjacency && yDiff <= adjacency)
					{
						pass = true;
						break;
					}

					newOccupy++;
				}

				if (pass)
					break;

				occupy++;
			}

			if (pass)
				result = true;
		}
	}

	return result;
}

bool BuildingExt::Should_Evaluate_Cell_For_Placement(CellStruct cell, BuildingTypeClass* pBuildingType, HouseClass* pOwner, int adjacencyBonus)
{
	if (pOwner == nullptr)
		return false;

	const auto houseExt = HouseExt::ExtMap.Find(pOwner);
	bool isCellUnsafe = false;
	for (auto it = houseExt->UnsafePlacementZones.begin(); it != houseExt->UnsafePlacementZones.end(); )
	{
		if (Unsorted::CurrentFrame > it->ExpiryFrame)
			it = houseExt->UnsafePlacementZones.erase(it);
		else
		{
			if (cell.DistanceFromSquared(it->Coords) < 100.0) // 10-cell radius
			{
				isCellUnsafe = true;
				break;
			}
			++it;
		}
	}

	if (isCellUnsafe)
	{
		if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
			return false; // Strictly enforce unsafe/congested zones during expansion crawling

		const BuildingClass* pOurConYard = pOwner->ConYards.Count > 0 ? pOwner->ConYards[0] : nullptr;
		if (pOurConYard == nullptr || cell.DistanceFromSquared(pOurConYard->GetMapCoords()) >= 400.0)
			return false;
	}

	bool result = false;
	const bool isNaval = pBuildingType->Naval;

	for (size_t i = 0; i < ExtData::AdjacencyAnchorCount; i++)
	{
		const BuildingClass* pOtherBuilding = ExtData::AdjacencyAnchors[i];
		int adjacency = pBuildingType->Adjacent + adjacencyBonus;

		if (isNaval && pOtherBuilding->Type->ConstructionYard)
			adjacency = RulesClass::Instance->AINavalYardAdjacency + adjacencyBonus;

		const int otherW = pOtherBuilding->Type->GetFoundationWidth();
		const int otherH = pOtherBuilding->Type->GetFoundationHeight(false);
		const CellStruct origin = pOtherBuilding->GetMapCoords();

		const int newW = pBuildingType->GetFoundationWidth();
		const int newH = pBuildingType->GetFoundationHeight(false);

		// Fast Bounding Box check:
		const int dx = (cell.X > origin.X + otherW - 1) ? (cell.X - (origin.X + otherW - 1)) 
		             : ((cell.X + newW - 1 < origin.X) ? (origin.X - (cell.X + newW - 1)) : 0);
		if (dx > adjacency)
			continue;

		const int dy = (cell.Y > origin.Y + otherH - 1) ? (cell.Y - (origin.Y + otherH - 1)) 
		             : ((cell.Y + newH - 1 < origin.Y) ? (origin.Y - (cell.Y + newH - 1)) : 0);
		if (dy > adjacency)
			continue;

		if (!result)
		{
			bool pass = false;
			CellStruct const* occupy = pOtherBuilding->GetFoundationData(true);
			while (occupy->X != REFRESH_EOL && occupy->Y != REFRESH_EOL)
			{
				const CellStruct sum = origin + *occupy;
				CellStruct const* newOccupy = pBuildingType->GetFoundationData(true);
				while (newOccupy->X != REFRESH_EOL && newOccupy->Y != REFRESH_EOL)
				{
					const CellStruct newSum = cell + *newOccupy;

					int xDiff = std::abs(newSum.X - sum.X);
					int yDiff = std::abs(newSum.Y - sum.Y);

					if (xDiff <= adjacency && yDiff <= adjacency)
					{
						pass = true;
						break;
					}

					newOccupy++;
				}

				if (pass)
					break;

				occupy++;
			}

			if (pass)
				result = true;
		}
	}

	return result;
}

/**
 *  Implements an extra check for terrain passability around the building.
 *  This is to make the AI less likely to get stuck.
 */
int inline BuildingExt::Modify_Rating_By_Terrain_Passability(CellStruct cell, BuildingClass* pBuilding, int originalValue)
{
	int value = originalValue;

	const CellStruct cellAboveCoords = cell + CellStruct(-1, -1);
	const CellStruct cellBelowCoords = cell + CellStruct(pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false));
	bool passableAbove = true;
	bool passableBelow = true;

	const SpeedType speed = pBuilding->Type->SpeedType == SpeedType::Float ? SpeedType::Float : SpeedType::Foot;

	if (MapClass::Instance.CoordinatesLegal(cellAboveCoords))
	{
		CellClass* cellAbove = MapClass::Instance.GetCellAt(cellAboveCoords);
		if (cellAbove && !cellAbove->IsClearToMove(speed, true, true, 0, MovementZone::Normal, 0, false))
		{
			passableAbove = false;
		}
	}

	if (MapClass::Instance.CoordinatesLegal(cellBelowCoords))
	{
		CellClass* cellBelow = MapClass::Instance.GetCellAt(cellBelowCoords);
		if (cellBelow && !cellBelow->IsClearToMove(speed, true, true, 0, MovementZone::Normal, 0, false))
		{
			passableBelow = false;
		}
	}

	// If both above and below are impassable, apply a higher rating penalty rather than completely discarding the cell.
	if (!passableAbove && !passableBelow)
	{
		value *= 3;
	}

	// Do the same processing for left and right (east and west, considering in-game rendering iow. NOT logical in-game compass)
	const CellStruct cellEastCoords = cell + CellStruct(pBuilding->Type->GetFoundationWidth(), -1);
	const CellStruct cellWestCoords = cell + CellStruct(-1, pBuilding->Type->GetFoundationHeight(false));
	bool passableEast = true;
	bool passableWest = true;

	if (MapClass::Instance.CoordinatesLegal(cellEastCoords))
	{
		CellClass* cell_above = MapClass::Instance.GetCellAt(cellEastCoords);
		if (cell_above && !cell_above->IsClearToMove(speed, true, true, 0, MovementZone::Normal, 0, false))
		{
			passableEast = false;
		}
	}

	if (MapClass::Instance.CoordinatesLegal(cellWestCoords))
	{
		CellClass* cell_below = MapClass::Instance.GetCellAt(cellWestCoords);
		if (cell_below && !cell_below->IsClearToMove(speed, true, true, 0, MovementZone::Normal, 0, false))
		{
			passableWest = false;
		}
	}

	// If both east and west are impassable, apply a higher rating penalty.
	if (!passableEast && !passableWest)
	{
		value *= 3;
	}

	// Individual stuck positions just result in a worse rating.
	if (!passableAbove) value = (value * 4) / 3;
	if (!passableBelow) value = (value * 4) / 3;
	if (!passableEast) value = (value * 4) / 3;
	if (!passableWest) value = (value * 4) / 3;

	return value;
}

/**
 *  Evaluates a rectangle from the map with a value generator
 *  function and finds the best cell for placing down a building.
 *  The best cell is one that has the LOWEST value and that allows legal
 *  building placement.
 */
CellStruct BuildingExt::Find_Best_Building_Placement_Cell(RectangleStruct baseArea, BuildingClass* pBuilding, int (*valueGenerator)(CellStruct, BuildingClass*), int adjacencyBonus)
{
	struct RatedCell {
		CellStruct cell;
		int rating;
	};
	std::vector<RatedCell> bestCells;
	int lowestRating = INT_MAX;
	CellStruct bestCell = CellStruct(0, 0);
	
	// Check the resolution of the scan. If our base area is huge, we can't check as precisely
	// or we'll cause into performance issues.
	// Cap resolution to pBuilding->Type->Adjacent so we never skip valid adjacent placement slots.
	const int resCells = 2000;
	const int areaSize = baseArea.Width * baseArea.Height;
	const int maxResolution = std::max(1, pBuilding->Type->Adjacent);
	const int resolution = std::min(maxResolution, 1 + (areaSize / resCells));

	for (int y = baseArea.Y; y < baseArea.Y + baseArea.Height; y += resolution)
	{
		for (int x = baseArea.X; x < baseArea.X + baseArea.Width; x += resolution)
		{
			CellStruct cell = CellStruct(x, y);

			// Skip cells that are outside of the visible map area.
			if (!MapClass::Instance.CoordinatesLegal(cell))
				continue;

			// Skip cells where we couldn't legally place the building on.
			// TODO: Manually check the cells? Currently our own units also block placement.
			if (!pBuilding->Type->CanPlaceHere(&cell, pBuilding->Owner))
				continue;

			// Check whether this cell is fine by proximity rules.
			if (!Should_Evaluate_Cell_For_Placement(cell, pBuilding, adjacencyBonus))
				continue;

			// Prevent placing buildings on or directly adjacent to Tiberium trees
			if (OverlapsTiberiumTreeZone(cell, pBuilding->Type))
			{
				continue;
			}

			// Get value for the cell.
			int value = valueGenerator(cell, pBuilding);

			// Adjust for terrain passability (lessen the chance for the dumb AI to get stuck).
			value = Modify_Rating_By_Terrain_Passability(cell, pBuilding, value);

			// Enforce spacing between base defenses (prevent placing them touching).
			if (pBuilding->Type->IsBaseDefense)
			{
				bool tooCloseToDefense = false;
				for (const auto pOtherBuilding : BuildingClass::Array)
				{
					if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding->Type->IsBaseDefense && pOtherBuilding != pBuilding)
					{
						double dist = cell.DistanceFrom(pOtherBuilding->GetMapCoords());
						if (dist < 4.0) // Require at least 4 cells spacing between base defenses to avoid clustering
						{
							tooCloseToDefense = true;
							break;
						}
					}
				}
				if (tooCloseToDefense)
				{
					value += 500000; // Add a strong rating penalty (lowest is best) to prevent clustering
				}
			}

			// Enforce a minimum of 1-cell margin spacing around all normal structures (excluding defenses)
			if (!pBuilding->Type->IsBaseDefense)
			{
				bool isAdjacentToAny = false;
				const int x1 = cell.X - 1;
				const int y1 = cell.Y - 1;
				const int w1 = pBuilding->Type->GetFoundationWidth() + 2;
				const int h1 = pBuilding->Type->GetFoundationHeight(false) + 2;

				for (const auto pOtherBuilding : BuildingClass::Array)
				{
					if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding != pBuilding)
					{
						// Exclude base defenses from blocking normal building margin (so we can place defenses near normal buildings if needed)
						if (!pOtherBuilding->Type->IsBaseDefense)
						{
							const int x2 = pOtherBuilding->GetMapCoords().X;
							const int y2 = pOtherBuilding->GetMapCoords().Y;
							const int w2 = pOtherBuilding->Type->GetFoundationWidth();
							const int h2 = pOtherBuilding->Type->GetFoundationHeight(false);

							if ((x1 < x2 + w2) && (x1 + w1 > x2) && (y1 < y2 + h2) && (y1 + h1 > y2))
							{
								isAdjacentToAny = true;
								break;
							}
						}
					}
				}
				if (isAdjacentToAny)
				{
					value += 100000; // Add a strong rating penalty to enforce the 1-cell margin
				}
			}

			// Enforce spacing between factories and refineries to prevent unit exit traffic jams and harvester bottlenecks.
			const bool isFactoryOrRefinery = pBuilding->Type->Factory != AbstractType::None || pBuilding->Type->Refinery || pBuilding->Type->ResourceDestination;
			if (isFactoryOrRefinery)
			{
				bool tooClose = false;
				const int x1 = cell.X - 1;
				const int y1 = cell.Y - 1;
				const int w1 = pBuilding->Type->GetFoundationWidth() + 2;
				const int h1 = pBuilding->Type->GetFoundationHeight(false) + 2;

				for (const auto pOtherBuilding : BuildingClass::Array)
				{
					if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding != pBuilding)
					{
						const bool otherIsFactoryOrRefinery = pOtherBuilding->Type->Factory != AbstractType::None || pOtherBuilding->Type->Refinery || pOtherBuilding->Type->ResourceDestination;
						if (otherIsFactoryOrRefinery)
						{
							const int x2 = pOtherBuilding->GetMapCoords().X;
							const int y2 = pOtherBuilding->GetMapCoords().Y;
							const int w2 = pOtherBuilding->Type->GetFoundationWidth();
							const int h2 = pOtherBuilding->Type->GetFoundationHeight(false);

							if ((x1 < x2 + w2) && (x1 + w1 > x2) && (y1 < y2 + h2) && (y1 + h1 > y2))
							{
								tooClose = true;
								break;
							}
						}
					}
				}
				if (tooClose)
					value += 5000000; // Add a high rating penalty so spacing is strongly preferred
			}


			// Support for AIInnerBase and dispersion of special structures
			if (IsAIInnerBase(pBuilding->Type))
			{
				// Reward being close to the center of the base / ConYard
				const CellStruct center = pBuilding->Owner->Base_Center();
				value += static_cast<int>(cell.DistanceFrom(center) * 100);

				// Penalize being too close to other AIInnerBase buildings to promote dispersion
				bool tooCloseToInnerBase = false;
				double closestInnerBaseDist = 999.0;
				for (const auto pOtherBuilding : BuildingClass::Array)
				{
					if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding->Owner == pBuilding->Owner && pOtherBuilding != pBuilding)
					{
						if (IsAIInnerBase(pOtherBuilding->Type))
						{
							double dist = cell.DistanceFrom(pOtherBuilding->GetMapCoords());
							if (dist < closestInnerBaseDist)
							{
								closestInnerBaseDist = dist;
							}
							if (dist < 3.0)
							{
								tooCloseToInnerBase = true;
							}
						}
					}
				}

				if (tooCloseToInnerBase)
				{
					value += 25000;
				}
				else if (closestInnerBaseDist < 8.0)
				{
					value += static_cast<int>((8.0 - closestInnerBaseDist) * 2000);
				}
			}

			// Dispersion penalty for area-support radius buildings (Gap Generators, Inhibitors, Radar Jammers, etc.)
			// Reward proximity to the Construction Yard first. If the ConYard is missing, reward closeness to
			// production structures (Barracks/War Factories). Fall back to the base center only if neither is present.
			// Unlike AIInnerBase, these buildings carry NO penalty for being placed on the frontline.
			const SupportRadiusType supportRadiusType = GetSupportRadiusType(pBuilding->Type);
			if (supportRadiusType != SupportRadiusType::None)
			{
				// --- Proximity reward: ConYard -> production buildings -> base center ---
				double closestConYardDist = -1.0;
				double closestFactoryDist = -1.0;

				for (const auto pOtherBuilding : BuildingClass::Array)
				{
					if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding->Owner == pBuilding->Owner)
					{
						if (pOtherBuilding->Type->ConstructionYard)
						{
							double dist = cell.DistanceFrom(pOtherBuilding->GetMapCoords());
							if (closestConYardDist < 0.0 || dist < closestConYardDist)
								closestConYardDist = dist;
						}
						else if (pOtherBuilding->Type->Factory == AbstractType::UnitType || pOtherBuilding->Type->Factory == AbstractType::InfantryType)
						{
							double dist = cell.DistanceFrom(pOtherBuilding->GetMapCoords());
							if (closestFactoryDist < 0.0 || dist < closestFactoryDist)
								closestFactoryDist = dist;
						}
					}
				}

				if (closestConYardDist >= 0.0)
				{
					// Reward proximity to the Construction Yard
					value += static_cast<int>(closestConYardDist * 80);
				}
				else if (closestFactoryDist >= 0.0)
				{
					// Reward proximity to Factory/Barracks
					value += static_cast<int>(closestFactoryDist * 80);
				}
				else
				{
					// Fallback: no ConYard nor factories yet — reward proximity to the base center
					const CellStruct center = pBuilding->Owner->Base_Center();
					value += static_cast<int>(cell.DistanceFrom(center) * 80);
				}				// --- Dispersion penalty: avoid clustering with other buildings of the same radius type ---
				bool tooCloseToSameType = false;
				double closestSameTypeDist = 999.0;
				for (const auto pOtherBuilding : BuildingClass::Array)
				{
					if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding->Owner == pBuilding->Owner && pOtherBuilding != pBuilding)
					{
						if (GetSupportRadiusType(pOtherBuilding->Type) == supportRadiusType || _stricmp(GetGroupAsID(pOtherBuilding->Type), GetGroupAsID(pBuilding->Type)) == 0)
						{
							double dist = cell.DistanceFrom(pOtherBuilding->GetMapCoords());
							if (dist < closestSameTypeDist)
								closestSameTypeDist = dist;

							const int otherRadius = GetSupportRadius(pOtherBuilding->Type);
							const int otherSeparation = otherRadius > 1 ? static_cast<int>(otherRadius * 1.8) : 3;
							if (dist < otherSeparation)
								tooCloseToSameType = true;
						}
					}
				}

				const int buildingRadius = GetSupportRadius(pBuilding->Type);
				const int targetSeparation = buildingRadius > 1 ? static_cast<int>(buildingRadius * 1.8) : 8;

				if (tooCloseToSameType)
				{
					value += 500000;
				}
				else if (closestSameTypeDist < targetSeparation)
				{
					value += static_cast<int>((targetSeparation - closestSameTypeDist) * 1500);
				}
			}

			// Check whether this is one of the best placement cells so far.
			if (value < lowestRating)
			{
				lowestRating = value;
				bestCells.clear();
				bestCells.push_back({cell, value});
			}
			else if (value == lowestRating)
			{
				bestCells.push_back({cell, value});
			}
		}
	}

	if (!bestCells.empty())
	{
		const int randIndex = ScenarioClass::Instance->Random.RandomRanged(0, static_cast<int>(bestCells.size()) - 1);
		bestCell = bestCells[randIndex].cell;
	}

	// If no valid cell satisfied all criteria and bestCell is still (0,0), perform a fallback pass over baseArea.
	// This pass still respects adjacency so the building is never placed disconnected from the base.
	// If truly no adjacent cell is available, return (0,0) so the caller can handle it gracefully.
	if (bestCell.X == 0 && bestCell.Y == 0)
	{
		for (int y = baseArea.Y; y < baseArea.Y + baseArea.Height; ++y)
		{
			for (int x = baseArea.X; x < baseArea.X + baseArea.Width; ++x)
			{
				CellStruct cell = CellStruct(x, y);
				if (MapClass::Instance.CoordinatesLegal(cell)
					&& pBuilding->Type->CanPlaceHere(&cell, pBuilding->Owner)
					&& Should_Evaluate_Cell_For_Placement(cell, pBuilding, adjacencyBonus))
				{
					return cell;
				}
			}
		}
	}

	return bestCell;
}

int inline BuildingExt::Modify_Rating_By_Allied_Building_Proximity(CellStruct cell, BuildingClass* pBuilding, int originalValue)
{
	const int value = originalValue * 1000;

	const CellStruct centerCell = cell + CellStruct(pBuilding->Type->GetFoundationWidth() / 2, pBuilding->Type->GetFoundationHeight(false) / 2);

	double closest_distance_sq = std::numeric_limits<double>::max();

	for (size_t i = 0; i < ExtData::OurBuildingCount; i++)
	{
		const BuildingClass* otherBuilding = ExtData::OurBuildings[i];

		CellStruct other_center_cell = GeneralUtils::CellFromCoordinates(otherBuilding->GetCenterCoords());
		const double distSq = centerCell.DistanceFromSquared(other_center_cell);
		if (distSq < closest_distance_sq)
			closest_distance_sq = distSq;
	}

	const double closest_distance = std::sqrt(closest_distance_sq);

	int penalty = 0;
	const HouseClass* pOwner = pBuilding->Owner;
	if (pOwner != nullptr)
	{
		const auto houseExt = HouseExt::ExtMap.Find(pOwner);
		if (houseExt != nullptr && houseExt->PrimaryTechTreeType != nullptr && houseExt->PrimaryTechTreeType->IsNavalMode)
		{
			if (pBuilding->Type->Naval)
			{
				for (size_t i = 0; i < ExtData::OurBuildingCount; i++)
				{
					const BuildingClass* otherBuilding = ExtData::OurBuildings[i];
					if (otherBuilding->Type->Naval)
					{
						CellStruct other_center_cell = GeneralUtils::CellFromCoordinates(otherBuilding->GetCenterCoords());
						const double dist = centerCell.DistanceFrom(other_center_cell);
						if (dist < 5.0)
						{
							penalty = 500000;
							break;
						}
					}
				}
			}
		}
	}

	if (pBuilding->Type->IsBaseDefense)
	{
		for (size_t i = 0; i < ExtData::OurBuildingCount; i++)
		{
			const BuildingClass* otherBuilding = ExtData::OurBuildings[i];
			if (otherBuilding->Type->IsBaseDefense)
			{
				CellStruct other_center_cell = GeneralUtils::CellFromCoordinates(otherBuilding->GetCenterCoords());
				const double dist = centerCell.DistanceFrom(other_center_cell);

				double size1 = (static_cast<double>(pBuilding->Type->GetFoundationWidth()) + static_cast<double>(pBuilding->Type->GetFoundationHeight(false))) / 2.0;
				double size2 = (static_cast<double>(otherBuilding->Type->GetFoundationWidth()) + static_cast<double>(otherBuilding->Type->GetFoundationHeight(false))) / 2.0;
				double minAllowedDist = (size1 + size2) / 2.0 + 0.5;

				if (dist < minAllowedDist)
				{
					penalty = 500000;
					break;
				}
			}
		}
	}

	// The closer the building is to an existing building, the worse the placement position is.
	// In other words, being closer INCREASES the value (as lower is better).
	return value + penalty + (1000 - static_cast<int>(closest_distance));
}

int BuildingExt::Refinery_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	double value = 0;

	// If we have nowhere to expand, then just try placing it somewhere central, hopefully it's safe there.
	if (houseExt->NextExpansionPointLocation.X <= 0 || houseExt->NextExpansionPointLocation.Y <= 0)
	{
		CellStruct conyardCell;
		if (pOwner->ConYards.Count > 0 && pOwner->ConYards[0] != nullptr)
			conyardCell = GeneralUtils::CellFromCoordinates(pOwner->ConYards[0]->GetCenterCoords());
		else
			conyardCell = pOwner->Base_Center();
		const double baseCenterDist = cell.DistanceFrom(pOwner->Base_Center());
		const double conyardDist = cell.DistanceFrom(conyardCell);
		value = (baseCenterDist + conyardDist) / 2.0;
	}
	else
	{
		// For refinery placement, we allow a flexible range up to 9.0 cells from the expansion point
		// to give it room to maneuver and find a buildable flat spot.
		const double dist = cell.DistanceFrom(houseExt->NextExpansionPointLocation);
		if (dist <= 9.0)
		{
			value = dist;
		}
		else
		{
			value = dist * 100.0;
		}
	}

	// Take proximity into nearby buildings into account.
	// We do this to avoid traffic congestion in tight spaces in the AI's base.
	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(value));
}

/**
 *  Calculates the best refinery placement location.
 */
RectangleStruct BuildingExt::GetRefinerySearchRect(HouseClass* pHouse, BuildingTypeClass* pRefineryType)
{
	const auto houseExt = HouseExt::ExtMap.Find(pHouse);
	const int adjacency = pRefineryType->Adjacent + 1;
	RectangleStruct baseArea = Get_Base_Rect(pHouse, adjacency, pRefineryType->GetFoundationWidth(), pRefineryType->GetFoundationHeight(false), pRefineryType);

	if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		// Find the closest base normal building to the expansion target
		const BuildingClass* pClosestPivot = nullptr;
		double closestDist = std::numeric_limits<double>::max();

		for (const auto pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && IsAIBaseNormal(pBld->Type))
			{
				const double dist = pBld->GetMapCoords().DistanceFrom(houseExt->NextExpansionPointLocation);
				if (dist < closestDist)
				{
					closestDist = dist;
					pClosestPivot = pBld;
				}
			}
		}

		if (pClosestPivot != nullptr)
		{
			int xMin = INT_MAX, yMin = INT_MAX, xMax = INT_MIN, yMax = INT_MIN;
			bool foundAny = false;
			const CellStruct pivotCoords = pClosestPivot->GetMapCoords();

			for (const auto pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && IsAIBaseNormal(pBld->Type))
				{
					const double distToPivot = pBld->GetMapCoords().DistanceFrom(pivotCoords);
					if (distToPivot <= 9.0) // 9-cell sweep radius around the main pivot
					{
						const CellStruct bldCell = pBld->GetMapCoords();
						const int bldW = pBld->Type->GetFoundationWidth();
						const int bldH = pBld->Type->GetFoundationHeight(false);
						
						const int refineryW = pRefineryType->GetFoundationWidth();
						const int refineryH = pRefineryType->GetFoundationHeight(false);
						const int adjRange = pRefineryType->Adjacent + 1;

						xMin = std::min(xMin, static_cast<int>(bldCell.X - adjRange - refineryW + 1));
						yMin = std::min(yMin, static_cast<int>(bldCell.Y - adjRange - refineryH + 1));
						xMax = std::max(xMax, static_cast<int>(bldCell.X + bldW + adjRange - 1));
						yMax = std::max(yMax, static_cast<int>(bldCell.Y + bldH + adjRange - 1));
						foundAny = true;
					}
				}
			}

			if (foundAny)
			{
				baseArea = RectangleStruct { xMin, yMin, xMax - xMin + 1, yMax - yMin + 1 };
			}
		}
	}
	return baseArea;
}

CellStruct BuildingExt::Get_Best_Refinery_Placement_Position(BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		const CellStruct expansionTarget = houseExt->NextExpansionPointLocation;

		// 1. Find the closest base normal building to the expansion target (the main pivot)
		const BuildingClass* pClosestPivot = nullptr;
		double closestDist = std::numeric_limits<double>::max();

		for (const auto pBld : pOwner->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && IsAIBaseNormal(pBld->Type))
			{
				const double dist = pBld->GetMapCoords().DistanceFrom(expansionTarget);
				if (dist < closestDist)
				{
					closestDist = dist;
					pClosestPivot = pBld;
				}
			}
		}

		if (pClosestPivot != nullptr)
		{
			const CellStruct pivotCoords = pClosestPivot->GetMapCoords();

			// 2. Gather all other pivots within 9.0 cells of the main pivot
			std::vector<const BuildingClass*> pivotGroup;
			for (const auto pBld : pOwner->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && IsAIBaseNormal(pBld->Type))
				{
					const double distToPivot = pBld->GetMapCoords().DistanceFrom(pivotCoords);
					if (distToPivot <= 9.0)
					{
						pivotGroup.push_back(pBld);
					}
				}
			}

			// 3. Sort pivotGroup by their distance to the expansion target (closest first)
			std::sort(pivotGroup.begin(), pivotGroup.end(), [&expansionTarget](const BuildingClass* a, const BuildingClass* b) {
				return a->GetMapCoords().DistanceFrom(expansionTarget) < b->GetMapCoords().DistanceFrom(expansionTarget);
			});

			// 4. Try to place the refinery around each pivot sequentially
			for (const auto pPivot : pivotGroup)
			{
				const CellStruct pc = pPivot->GetMapCoords();
				const int pivotW = pPivot->Type->GetFoundationWidth();
				const int pivotH = pPivot->Type->GetFoundationHeight(false);

				const int refineryW = pBuilding->Type->GetFoundationWidth();
				const int refineryH = pBuilding->Type->GetFoundationHeight(false);
				const int adjRange = pBuilding->Type->Adjacent + 1;

				const int xMin = pc.X - adjRange - refineryW + 1;
				const int xMax = pc.X + pivotW + adjRange - 1;
				const int yMin = pc.Y - adjRange - refineryH + 1;
				const int yMax = pc.Y + pivotH + adjRange - 1;

				RectangleStruct searchArea { xMin, yMin, xMax - xMin + 1, yMax - yMin + 1 };

				const CellStruct placementCell = Find_Best_Building_Placement_Cell(searchArea, pBuilding, Refinery_Placement_Cell_Value, 0);
				if (placementCell.X > 0 && placementCell.Y > 0)
				{
					const BuildingClass* pOurConYard = pOwner->ConYards.Count > 0 ? pOwner->ConYards[0] : nullptr;
					const CellStruct conyardCell = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pOwner->Base_Center();

					const double conyardDist = placementCell.DistanceFrom(conyardCell);
					const double targetFromConyardDist = expansionTarget.DistanceFrom(conyardCell);

					if (targetFromConyardDist > 25.0 && conyardDist < 20.0)
					{
						Debug::Log("AdvAI: Refinery placement around pivot refused! Target (%d,%d) is far (%.1f cells from base), but placement (%d,%d) is inside main base (%.1f cells from base).\n",
							expansionTarget.X, expansionTarget.Y, targetFromConyardDist,
							placementCell.X, placementCell.Y, conyardDist);
						continue; // Try the next pivot
					}

					return placementCell;
				}
			}

			Debug::Log("AdvAI: Refinery placement failed. Searched around all %d pivots in the expansion zone and found no valid cell.\n",
				static_cast<int>(pivotGroup.size()));
			return CellStruct(0, 0);
		}
	}

	const int adjacency = pBuilding->Type->Adjacent + 1;
	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false), pBuilding->Type);
	
	const CellStruct placementCell = Find_Best_Building_Placement_Cell(baseArea, pBuilding, Refinery_Placement_Cell_Value, 0);

	if (placementCell.X > 0 && placementCell.Y > 0 && houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		const BuildingClass* pOurConYard = pOwner->ConYards.Count > 0 ? pOwner->ConYards[0] : nullptr;
		const CellStruct conyardCell = pOurConYard != nullptr ? pOurConYard->GetMapCoords() : pOwner->Base_Center();

		const double conyardDist = placementCell.DistanceFrom(conyardCell);
		const double targetFromConyardDist = houseExt->NextExpansionPointLocation.DistanceFrom(conyardCell);

		if (targetFromConyardDist > 25.0 && conyardDist < 20.0)
		{
			Debug::Log("AdvAI: Refinery fallback placement refused! Target (%d,%d) is far (%.1f cells from base), but placement (%d,%d) is inside main base (%.1f cells from base).\n",
				houseExt->NextExpansionPointLocation.X, houseExt->NextExpansionPointLocation.Y, targetFromConyardDist,
				placementCell.X, placementCell.Y, conyardDist);
			return CellStruct(0, 0);
		}
	}

	return placementCell;
}

int BuildingExt::ServiceDepot_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	HouseClass* pOwner = pBuilding->Owner;
	const auto pTechTree = TechTreeTypeClass::GetAnySuitable(pOwner);

	if (pTechTree != nullptr)
	{
		for (const auto pBld : pOwner->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld != pBuilding)
			{
				bool isOtherDepot = false;
				for (const auto pType : pTechTree->BuildServiceDepot)
				{
					if (pBld->Type == pType)
					{
						isOtherDepot = true;
						break;
					}
				}
				if (isOtherDepot)
				{
					const double distToOtherDepot = cell.DistanceFrom(pBld->GetMapCoords());
					if (distToOtherDepot < 25.0)
					{
						return 5000000;
					}
				}
			}
		}
	}

	double nearestTargetDist = std::numeric_limits<double>::max();
	for (const auto pBld : pOwner->Buildings)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo)
		{
			const bool isWF = pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval;
			const bool isRefinery = pBld->Type->Refinery || pBld->Type->ResourceDestination;
			if (isWF || isRefinery)
			{
				const double dist = cell.DistanceFrom(pBld->GetMapCoords());
				if (dist < nearestTargetDist)
				{
					nearestTargetDist = dist;
				}
			}
		}
	}

	double value = 0;
	if (nearestTargetDist < std::numeric_limits<double>::max())
	{
		if (nearestTargetDist < 3.0)
		{
			value = (3.0 - nearestTargetDist) * 1000.0;
		}
		else if (nearestTargetDist > 8.0)
		{
			value = (nearestTargetDist - 8.0) * 100.0;
		}
	}
	else
	{
		value = cell.DistanceFrom(pOwner->Base_Center());
	}

	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(value));
}

CellStruct BuildingExt::Get_Best_ServiceDepot_Placement_Position(BuildingClass* pBuilding)
{
	HouseClass* pOwner = pBuilding->Owner;
	const int adjacency = pBuilding->Type->Adjacent + 1;
	const RectangleStruct baseArea = Get_Base_Rect(pOwner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false), pBuilding->Type);

	const CellStruct placementCell = Find_Best_Building_Placement_Cell(baseArea, pBuilding, ServiceDepot_Placement_Cell_Value, 0);
	bool refused = false;

	if (placementCell.X > 0 && placementCell.Y > 0)
	{
		if (ServiceDepot_Placement_Cell_Value(placementCell, pBuilding) >= 5000000)
		{
			refused = true;
		}
	}
	else
	{
		refused = true;
	}

	if (refused)
	{
		Debug::Log("AdvAI: Service Depot placement refused! No placement cell available that is at least 25 cells away from existing depots.\n");
		const auto houseExt = HouseExt::ExtMap.Find(pOwner);
		houseExt->LastServiceDepotPlacementFailedFrame = Unsorted::CurrentFrame;

		// Force the building factory to cancel the pending Service Depot to avoid blocking the queue!
		FactoryClass* buildingFactory = pOwner->GetPrimaryFactory(AbstractType::BuildingType, false, BuildCat::DontCare);
		if (buildingFactory != nullptr)
		{
			buildingFactory->AbandonProduction();
		}
		return CellStruct(0, 0);
	}

	return placementCell;
}

int BuildingExt::Near_Base_Center_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	CellStruct conyardCell;
	if (pOwner->ConYards.Count > 0 && pOwner->ConYards[0] != nullptr)
		conyardCell = GeneralUtils::CellFromCoordinates(pOwner->ConYards[0]->GetCenterCoords());
	else
		conyardCell = pOwner->Base_Center();
	const double baseCenterDist = cell.DistanceFrom(pOwner->Base_Center());
	const double conyardDist = cell.DistanceFrom(conyardCell);
	const double balancedDist = (baseCenterDist + conyardDist) / 2.0;

	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(balancedDist));
}

int BuildingExt::Near_Base_Center_Defense_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* owner = pBuilding->Owner;
	CellStruct conyardCell;
	if (owner->ConYards.Count > 0 && owner->ConYards[0] != nullptr)
		conyardCell = GeneralUtils::CellFromCoordinates(owner->ConYards[0]->GetCenterCoords());
	else
		conyardCell = owner->Base_Center();
	const double baseCenterDist = cell.DistanceFrom(owner->Base_Center());
	const double conyardDist = cell.DistanceFrom(conyardCell);
	const double balancedDist = (baseCenterDist + conyardDist) / 2.0;

	const int enemyDistance = Get_Distance_To_Primary_Enemy(cell, pBuilding->Owner);
	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(balancedDist * 100) + enemyDistance);
}

int BuildingExt::Near_Enemy_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const HouseClass* enemy = nullptr;

	if (pOwner->EnemyHouseIndex >= 0 && pOwner->EnemyHouseIndex < HouseClass::Array.Count)
	{
		enemy = HouseClass::Array[pOwner->EnemyHouseIndex];
	}

	// If we have no enemy, then place it as close to the center of the map as possible.
	// Most commonly we are on the edge of a map, so if we place towards the center,
	// it doesn't go terribly wrong.
	if (enemy == nullptr)
	{
		const Point2D mapCenter { MapClass::Instance.VisibleRect.X + MapClass::Instance.VisibleRect.Width / 2,
			MapClass::Instance.VisibleRect.Y + MapClass::Instance.VisibleRect.Height / 2 };
		const CellStruct mapCenterCell = CellStruct(static_cast<short>(mapCenter.X), static_cast<short>(mapCenter.Y));
		return static_cast<int>(cell.DistanceFrom(mapCenterCell));
	}

	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(cell.DistanceFrom(enemy->Base_Center())));
}


int BuildingExt::Near_Refinery_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const BuildingClass* pRefinery = nullptr;
	for (size_t i = 0; i < ExtData::OurBuildingCount; i++)
	{
		if (ExtData::OurBuildings[i]->Type->Refinery)
		{
			pRefinery = ExtData::OurBuildings[i];
			break;
		}
	}

	CellStruct refineryCell;
	if (pRefinery != nullptr)
	{
		refineryCell = GeneralUtils::CellFromCoordinates(pRefinery->GetCenterCoords());
	}
	else
	{
		// Fallback
		const Point2D mapCenter { MapClass::Instance.VisibleRect.X + MapClass::Instance.VisibleRect.Width / 2,
			MapClass::Instance.VisibleRect.Y + MapClass::Instance.VisibleRect.Height / 2 };
		const CellStruct mapCenterCell = CellStruct(static_cast<short>(mapCenter.X), static_cast<short>(mapCenter.Y));
		refineryCell = mapCenterCell;
	}

	// Secondarily, consider distance to primary enemy.
	HouseClass* pOwner = pBuilding->Owner;
	const int enemyDistance = Get_Distance_To_Primary_Enemy(cell, pOwner);

	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(cell.DistanceFrom(refineryCell) * 100) + enemyDistance);
}

int BuildingExt::Near_ConYard_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding)
{
	CellStruct conyardCell;
	if (pBuilding->Owner->ConYards.Count > 0)
		conyardCell = GeneralUtils::CellFromCoordinates(pBuilding->Owner->ConYards[0]->GetCenterCoords());
	else
	{
		// Fallback
		const Point2D mapCenter { MapClass::Instance.VisibleRect.X + MapClass::Instance.VisibleRect.Width / 2,
			MapClass::Instance.VisibleRect.Y + MapClass::Instance.VisibleRect.Height / 2 };
		const CellStruct mapCenterCell = CellStruct(static_cast<short>(mapCenter.X), static_cast<short>(mapCenter.Y));
		conyardCell = mapCenterCell;
	}

	// Secondarily, consider distance to primary enemy.
	HouseClass* owner = pBuilding->Owner;
	const int enemyDistance = Get_Distance_To_Primary_Enemy(cell, owner);

	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(cell.DistanceFrom(conyardCell) * 100) + enemyDistance);
}

int BuildingExt::Far_From_Enemy_Placement_Position_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const HouseClass* pEnemy = nullptr;

	if (pOwner->EnemyHouseIndex >= 0 && pOwner->EnemyHouseIndex < HouseClass::Array.Count)
	{
		pEnemy = HouseClass::Array[pOwner->EnemyHouseIndex];
	}

	// If we have no enemy, then just place it near the base center (balanced).
	if (pEnemy == nullptr)
	{
		CellStruct conyardCell;
		if (pOwner->ConYards.Count > 0 && pOwner->ConYards[0] != nullptr)
			conyardCell = GeneralUtils::CellFromCoordinates(pOwner->ConYards[0]->GetCenterCoords());
		else
			conyardCell = pOwner->Base_Center();
		const double baseCenterDist = cell.DistanceFrom(pOwner->Base_Center());
		const double conyardDist = cell.DistanceFrom(conyardCell);
		const double balancedDist = (baseCenterDist + conyardDist) / 2.0;
		return static_cast<int>(balancedDist);
	}

	return SHRT_MAX - static_cast<int>(cell.DistanceFrom(pEnemy->Base_Center()));
}

CellStruct BuildingExt::Get_Best_SuperWeapon_Building_Placement_Position(BuildingClass* pBuilding)
{
	const int adjacency = pBuilding->Type->Adjacent + 1;
	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false), pBuilding->Type);
	return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Far_From_Enemy_Placement_Position_Value);
}

int BuildingExt::Towards_Expansion_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	// If we have nowhere to expand, then just try placing it somewhere that's far from our base (balanced).
	if (houseExt->NextExpansionPointLocation.X <= 0 || houseExt->NextExpansionPointLocation.Y <= 0)
	{
		CellStruct conyardCell;
		if (pOwner->ConYards.Count > 0 && pOwner->ConYards[0] != nullptr)
			conyardCell = GeneralUtils::CellFromCoordinates(pOwner->ConYards[0]->GetCenterCoords());
		else
			conyardCell = pOwner->Base_Center();
		const double baseCenterDist = cell.DistanceFrom(pOwner->Base_Center());
		const double conyardDist = cell.DistanceFrom(conyardCell);
		const double balancedDist = (baseCenterDist + conyardDist) / 2.0;
		return static_cast<int>(balancedDist);
	}

	HouseClass* pEnemy = nullptr;

	if (pOwner->EnemyHouseIndex >= 0 && pOwner->EnemyHouseIndex < HouseClass::Array.Count)
	{
		pEnemy = HouseClass::Array[pOwner->EnemyHouseIndex];
	}

	int enemyDistance = 0;
	if (pEnemy != nullptr && pEnemy->ConYards.Count > 0)
	{
		enemyDistance = static_cast<int>(cell.DistanceFrom(pEnemy->ConYards[0]->GetMapCoords()));
	}

	// Otherwise, we can basically make the value equal to the distance
	// that the building has to our next expansion point waypoint.
	// Also, secondarily take distance into enemy into account.
	const CellStruct currentWaypoint = houseExt->GetCrawlingWaypoint(houseExt->NextExpansionPointLocation);
	const double dist = cell.DistanceFrom(currentWaypoint);
	int distanceValue = static_cast<int>(dist * 100);

	return distanceValue + enemyDistance;
}

CellStruct BuildingExt::Get_Best_Expansion_Placement_Position_Helper(HouseClass* pOwner, BuildingTypeClass* pBuildingType, BuildingClass* pBuilding)
{
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);
	Debug::Log("AdvAI ExpansionPlacement: House %d: Evaluating placement for %s. Target: (%d,%d). ShouldBuildRefinery: %s\n",
		pOwner->ArrayIndex, pBuildingType->ID,
		houseExt->NextExpansionPointLocation.X, houseExt->NextExpansionPointLocation.Y,
		houseExt->ShouldBuildRefinery ? "YES" : "NO");

	const int buildingW = pBuildingType->GetFoundationWidth();
	const int buildingH = pBuildingType->GetFoundationHeight(false);
	// +1 allows one cell of adjacency leniency to hop over small gaps.
	const int adjRange = pBuildingType->Adjacent + 1;

	const CellStruct& expansionTarget = houseExt->NextExpansionPointLocation;
	const bool hasExpansionTarget = expansionTarget.X > 0 && expansionTarget.Y > 0;

	if (hasExpansionTarget)
	{
		// For each owned building (anchor), scan cells in its adjacency ring and
		// pick the cell that is closest to the Tiberium expansion target.
		// Using CanPlaceHere (engine check) instead of the coarse bounding-box scan
		// with resolution sampling, which could skip the best cells entirely.
		CellStruct bestCell = CellStruct(0, 0);
		double bestDist = std::numeric_limits<double>::max();

		// Use A* waypoint dynamic crawling
		const CellStruct currentWaypoint = houseExt->GetCrawlingWaypoint(expansionTarget);

		for (size_t i = 0; i < ExtData::OurBuildingCount; i++)
		{
			const BuildingClass* pAnchor = ExtData::OurBuildings[i];
			if (!IsAIBaseNormal(pAnchor->Type))
				continue;

			const CellStruct anchorCell = pAnchor->GetMapCoords();
			const int anchorW = pAnchor->Type->GetFoundationWidth();
			const int anchorH = pAnchor->Type->GetFoundationHeight(false);

			// Scan a rectangle around this anchor building large enough to cover
			// all valid adjacent positions for the new building.
			const int xMin = anchorCell.X - adjRange - buildingW + 1;
			const int xMax = anchorCell.X + anchorW + adjRange - 1;
			const int yMin = anchorCell.Y - adjRange - buildingH + 1;
			const int yMax = anchorCell.Y + anchorH + adjRange - 1;

			for (int y = yMin; y <= yMax; y++)
			{
				for (int x = xMin; x <= xMax; x++)
				{
					CellStruct cell(static_cast<short>(x), static_cast<short>(y));

					if (!MapClass::Instance.CoordinatesLegal(cell))
						continue;

					// CanPlaceHere is the engine's authoritative check:
					// it validates terrain suitability, adjacency to existing structures,
					// occupied cells, and all other placement rules.
					if (!pBuildingType->CanPlaceHere(&cell, pOwner))
						continue;

					if (OverlapsTiberiumTreeZone(cell, pBuildingType))
						continue;

					// Prevent placing the expansion building if it would cause congestion (touching same type or 2+ buildings)
					bool cellIsCongested = false;
					int touchingCount = 0;
					const int b1X = cell.X;
					const int b1Y = cell.Y;
					const int b1W = pBuildingType->GetFoundationWidth();
					const int b1H = pBuildingType->GetFoundationHeight(false);

					for (const auto pOtherBuilding : BuildingClass::Array)
					{
						if (pOtherBuilding && pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding != pBuilding)
						{
							if (pOtherBuilding->Owner == pOwner || pOwner->IsAlliedWith(pOtherBuilding->Owner))
							{
								const int b2X = pOtherBuilding->GetMapCoords().X;
								const int b2Y = pOtherBuilding->GetMapCoords().Y;
								const int b2W = pOtherBuilding->Type->GetFoundationWidth();
								const int b2H = pOtherBuilding->Type->GetFoundationHeight(false);

								// Check if touching (including 1-cell margin)
								if ((b1X - 1 <= b2X + b2W - 1) && (b1X + b1W >= b2X) &&
									(b1Y - 1 <= b2Y + b2H - 1) && (b1Y + b1H >= b2Y))
								{
									if (pBuildingType->IsBaseDefense && pOtherBuilding->Type->IsBaseDefense)
									{
										cellIsCongested = true;
										break;
									}

									if (pOtherBuilding->Type == pBuildingType)
									{
										cellIsCongested = true;
										break;
									}
									touchingCount++;
									if (touchingCount >= 2)
									{
										cellIsCongested = true;
										break;
									}
								}
							}
						}
					}

					if (cellIsCongested)
						continue;

					// Check if this cell is close to a recently destroyed building (unsafe zone)
					bool isUnsafe = false;
					for (auto it = houseExt->UnsafePlacementZones.begin(); it != houseExt->UnsafePlacementZones.end(); )
					{
						if (Unsorted::CurrentFrame > it->ExpiryFrame)
							it = houseExt->UnsafePlacementZones.erase(it);
						else
						{
							if (cell.DistanceFromSquared(it->Coords) < 100.0) // 10-cell radius
							{
								isUnsafe = true;
								break;
							}
							++it;
						}
					}
					if (isUnsafe)
					{
						if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
							continue; // Strictly enforce unsafe/congested zones during expansion crawling

						const BuildingClass* pOurConYard = pOwner->ConYards.Count > 0 ? pOwner->ConYards[0] : nullptr;
						if (pOurConYard == nullptr || cell.DistanceFromSquared(pOurConYard->GetMapCoords()) >= 400.0)
							continue;
					}

					// Enforce a minimum separation of 10 cells between refineries during expansion placement
					if (pBuildingType->ResourceDestination)
					{
						bool tooCloseToRefinery = false;
						for (const auto pOtherBuilding : BuildingClass::Array)
						{
							if (pOtherBuilding && pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding->Type->ResourceDestination && pOtherBuilding != pBuilding)
							{
								if (pOtherBuilding->Owner == pOwner)
								{
									double distToOtherRefinery = cell.DistanceFrom(pOtherBuilding->GetMapCoords());
									if (distToOtherRefinery < 10.0)
									{
										tooCloseToRefinery = true;
										break;
									}
								}
							}
						}
						if (tooCloseToRefinery)
							continue;
					}

					const double dist = cell.DistanceFrom(currentWaypoint);

					if (dist < bestDist)
					{
						bestDist = dist;
						bestCell = cell;
					}
				}
			}
		}

		if (bestCell.X <= 0 && bestCell.Y <= 0)
		{
			Debug::Log("AdvAI ExpansionPlacement: House %d: No valid buildable cell found for %s near any anchor building towards waypoint (%d,%d)\n",
				pOwner->ArrayIndex, pBuildingType->ID, currentWaypoint.X, currentWaypoint.Y);
		}

		if (bestCell.X > 0 || bestCell.Y > 0)
		{
			// Check if this new cell gets us closer to the waypoint than
			// any of our existing buildings. If not, we are as close as we can get.
			if (!houseExt->ShouldBuildRefinery)
			{
				double nearestExistingDistSq = std::numeric_limits<double>::max();
				for (size_t i = 0; i < ExtData::OurBuildingCount; i++)
				{
					const auto pBld = ExtData::OurBuildings[i];
					if (pBld && IsAIBaseNormal(pBld->Type))
					{
						const double dSq = currentWaypoint.DistanceFromSquared(pBld->GetMapCoords());
						if (dSq < nearestExistingDistSq)
							nearestExistingDistSq = dSq;
					}
				}

				const double bestDistSq = bestDist * bestDist;
				const bool isFinalTarget = (currentWaypoint == expansionTarget);
				if (bestDist < 22.0 && isFinalTarget)
				{
					if (bestDistSq >= nearestExistingDistSq || nearestExistingDistSq < 64.0)
					{
						houseExt->ShouldBuildRefinery = true;
						Debug::Log("AdvAI: House %d: Switched ShouldBuildRefinery = YES. BestDist: %.1f, NearestExistingDist: %.1f\n",
							pOwner->ArrayIndex, bestDist, sqrt(nearestExistingDistSq));
					}
				}
				else
				{
					if (bestDistSq >= nearestExistingDistSq)
					{
						Debug::Log("AdvAI: House %d crawler cannot get closer than existing buildings (Best: %.1f, Existing: %.1f). Blocked halfway! Aborting placement.\n",
							pOwner->ArrayIndex, bestDist, sqrt(nearestExistingDistSq));
						return CellStruct::Empty;
					}
				}
			}

			if (pBuilding)
			{
				houseExt->ExpansionPlacementFailures = 0;

				Debug::Log("AdvAI ExpansionPlacement: House %d placing %s at (%d,%d), dist to target (%d,%d) = %.1f cells (waypoint: (%d,%d))%s\n",
					pOwner->ArrayIndex, pBuildingType->ID,
					bestCell.X, bestCell.Y,
					expansionTarget.X, expansionTarget.Y,
					bestCell.DistanceFrom(expansionTarget),
					currentWaypoint.X, currentWaypoint.Y,
					houseExt->ShouldBuildRefinery ? " [REFINERY NEXT]" : "");
			}

			return bestCell;
		}

		if (pBuilding)
		{
			houseExt->ExpansionPlacementFailures++;
			if (houseExt->ExpansionPlacementFailures >= 3)
			{
				Debug::Log("AdvAI ExpansionPlacement: House %d: failed to crawl towards target (%d,%d) 3 times. Abandoning expansion target.\n",
					pOwner->ArrayIndex, expansionTarget.X, expansionTarget.Y);

				// Blacklist this target to prevent endless loop crawls if we cannot place any buildings towards it
				HouseExt::AdvAI_Add_Failed_Expansion_Point(pOwner, expansionTarget);

				Mark_Expansion_As_Done(pOwner);
				houseExt->ExpansionPlacementFailures = 0;
				houseExt->ShouldBuildRefinery = false;
			}
			else
			{
				Debug::Log("AdvAI ExpansionPlacement: House %d: no valid adjacent cell found for %s toward target (%d,%d). Failure count: %d. Falling back.\n",
					pOwner->ArrayIndex, pBuildingType->ID, expansionTarget.X, expansionTarget.Y, houseExt->ExpansionPlacementFailures);
			}
		}

		return CellStruct::Empty; // Abort and do not fall back to base if we were actively expanding!
	}

	if (pBuilding == nullptr)
		return CellStruct::Empty;

	// Fallback: no expansion target set, or couldn't find any adjacent valid cell.
	// Use the old bounding-box scan to find a reasonable placement.
	const int adjacency = adjRange + 1;
	const RectangleStruct baseArea = Get_Base_Rect(pOwner, adjacency, buildingW, buildingH, pBuildingType);

	CellStruct bestCell = Find_Best_Building_Placement_Cell(baseArea, pBuilding, Towards_Expansion_Placement_Cell_Value, 0);

	// Retry with adjacency bonus to allow hopping over small terrain gaps.
	const CellStruct altBestCell = Find_Best_Building_Placement_Cell(baseArea, pBuilding, Towards_Expansion_Placement_Cell_Value, 1);
	if (bestCell.DistanceFrom(altBestCell) > 1 && CellStruct::Empty != altBestCell)
		bestCell = altBestCell;

	return bestCell;
}

CellStruct BuildingExt::Get_Best_Expansion_Placement_Position(BuildingClass* pBuilding)
{
	const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
	if (pBuilding->Type->ResourceDestination && houseExt->NextRefineryPlacementLocation.X > 0 && houseExt->NextRefineryPlacementLocation.Y > 0)
	{
		const CellStruct savedTarget = houseExt->NextExpansionPointLocation;
		houseExt->NextExpansionPointLocation = houseExt->NextRefineryPlacementLocation;
		
		CellStruct result = Get_Best_Expansion_Placement_Position_Helper(pBuilding->Owner, pBuilding->Type, pBuilding);
		
		houseExt->NextExpansionPointLocation = savedTarget;
		return result;
	}
	return Get_Best_Expansion_Placement_Position_Helper(pBuilding->Owner, pBuilding->Type, pBuilding);
}


int BuildingExt::Barracks_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	// A barracks is best built close to the opponent.
	const HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	double expandDistance = 0;
	// If we are expanding, consider distance to expansion location as barracks are great for expanding.
	if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		expandDistance = cell.DistanceFrom(houseExt->NextExpansionPointLocation);
		expandDistance *= 3; // Give expansion distance more weight than distance to enemy
	}

	const HouseClass* pEnemy = nullptr;

	if (pOwner->EnemyHouseIndex >= 0 && pOwner->EnemyHouseIndex < HouseClass::Array.Count)
	{
		pEnemy = HouseClass::Array[pOwner->EnemyHouseIndex];
	}

	if (pEnemy != nullptr)
	{
		return static_cast<int>(cell.DistanceFrom(pEnemy->Base_Center()) + expandDistance);
	}

	// If we do not have an opponent, then just consider expansion.
	if (expandDistance > 0)
	{
		return static_cast<int>(expandDistance);
	}

	// If we do not have an opponent AND do not expand, just place it somewhere on our base outskirts (balanced).
	CellStruct conyardCell;
	if (pOwner->ConYards.Count > 0 && pOwner->ConYards[0] != nullptr)
	{
		conyardCell = GeneralUtils::CellFromCoordinates(pOwner->ConYards[0]->GetCenterCoords());
	}
	else
	{
		conyardCell = pOwner->Base_Center();
	}
	const double baseCenterDist = cell.DistanceFrom(pOwner->Base_Center());
	const double conyardDist = cell.DistanceFrom(conyardCell);
	const double balancedDist = (baseCenterDist + conyardDist) / 2.0;

	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, SHRT_MAX - static_cast<int>(balancedDist));
}

int BuildingExt::NavalYard_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	CellStruct conyardCell;
	if (pOwner->ConYards.Count > 0 && pOwner->ConYards[0] != nullptr)
		conyardCell = GeneralUtils::CellFromCoordinates(pOwner->ConYards[0]->GetCenterCoords());
	else
		conyardCell = pOwner->Base_Center();

	const CellStruct centerCell = cell + CellStruct(pBuilding->Type->GetFoundationWidth() / 2, pBuilding->Type->GetFoundationHeight(false) / 2);
	double closestDistSq = std::numeric_limits<double>::max();

	for (size_t i = 0; i < ExtData::OurBuildingCount; i++)
	{
		const BuildingClass* otherBuilding = ExtData::OurBuildings[i];
		CellStruct otherCenter = GeneralUtils::CellFromCoordinates(otherBuilding->GetCenterCoords());
		double dSq = centerCell.DistanceFromSquared(otherCenter);
		if (dSq < closestDistSq)
			closestDistSq = dSq;
	}

	double closestDist = std::sqrt(closestDistSq);
	double conyardDist = cell.DistanceFrom(conyardCell);

	// Favor cells that are close to our shoreline buildings (closestDist) and reasonably close to our ConYard (conyardDist)
	return static_cast<int>(conyardDist * 10.0) + static_cast<int>(closestDist * 100.0);
}

int BuildingExt::WarFactory_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	// If we are expanding (Tiberium or aggressive enemy crawl), place the factory near the front target
	if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		double value = cell.DistanceFrom(houseExt->NextExpansionPointLocation);
		return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(value));
	}

	return Near_Base_Center_Placement_Position_Value(cell, pBuilding);
}

int BuildingExt::Helipad_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	// If the building is NOT marked as an inner-base structure, and we are expanding, place it near the front target
	if (!IsAIInnerBase(pBuilding->Type) && houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		double value = cell.DistanceFrom(houseExt->NextExpansionPointLocation);
		return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(value));
	}

	return Near_Base_Center_Placement_Position_Value(cell, pBuilding);
}

/**
 *  Calculates the best factory placement location.
 */
CellStruct BuildingExt::Get_Best_Factory_Placement_Position(BuildingClass* pBuilding)
{
	const bool isNaval = pBuilding->Type->Factory == AbstractType::UnitType && pBuilding->Type->Naval;

	const int adjacency = isNaval ? RulesClass::Instance->AINavalYardAdjacency : pBuilding->Type->Adjacent;

	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false), pBuilding->Type);

	if (pBuilding->Type->Factory == AbstractType::InfantryType)
	{
		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Barracks_Placement_Cell_Value);
	}

	if (pBuilding->Type->Factory == AbstractType::UnitType)
	{
		if (isNaval)
		{
			return Find_Best_Building_Placement_Cell(baseArea, pBuilding, NavalYard_Placement_Cell_Value);
		}

		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, WarFactory_Placement_Cell_Value);
	}

	if (pBuilding->Type->Factory == AbstractType::AircraftType)
	{
		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Helipad_Placement_Cell_Value);
	}

	return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_Base_Center_Placement_Position_Value);
}

CellStruct BuildingExt::ExtData::AttackCell;
CellStruct BuildingExt::ExtData::AttackSourceCell;

int BuildingExt::Near_AttackCell_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	int distance = static_cast<int>(cell.DistanceFrom(ExtData::AttackCell));
	int randomOffset = ScenarioClass::Instance->Random.RandomRanged(-2, 2);
	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, distance + randomOffset);
}

int BuildingExt::Directional_Defense_Placement_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	const CellStruct B = ExtData::AttackCell;        // Building that was attacked
	const CellStruct A = ExtData::AttackSourceCell;  // Enemy attacker

	const double dx = static_cast<double>(A.X - B.X);
	const double dy = static_cast<double>(A.Y - B.Y);
	const double L2 = dx * dx + dy * dy;

	if (L2 < 9.0) // Fallback if attacker and building are too close (less than 3 cells)
	{
		// Fallback to simple distance if coordinates are invalid or identical
		int distance = static_cast<int>(cell.DistanceFrom(A));
		int randomOffset = ScenarioClass::Instance->Random.RandomRanged(-2, 2);
		return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, distance + randomOffset);
	}

	const double cx = static_cast<double>(cell.X - B.X);
	const double cy = static_cast<double>(cell.Y - B.Y);

	// Calculate perpendicular distance to the line B -> A
	// distPerp = |cx * dy - cy * dx| / sqrt(L2)
	const double distPerp = std::abs(cx * dy - cy * dx) / std::sqrt(L2);

	// Calculate distance from candidate cell to enemy attacker A
	const double distToAttacker = cell.DistanceFrom(A);

	// Check projection factor (t) to ensure we are building in the direction of the enemy, not behind our building
	const double dot = cx * dx + cy * dy;
	const double t = dot / L2;

	double rating = 2.0 * distPerp + distToAttacker;
	if (t < 0.0)
	{
		rating += 100.0; // Apply a heavy penalty if cell is in the opposite direction of attack
	}

	const int randomOffset = ScenarioClass::Instance->Random.RandomRanged(-2, 2);
	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(rating) + randomOffset);
}

CellStruct BuildingExt::Get_Best_Defense_Placement_Position(BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	ExtData::AttackCell = CellStruct(0, 0);
	ExtData::AttackSourceCell = CellStruct(0, 0);

	const int adjacency = pBuilding->Type->Adjacent;
	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false), pBuilding->Type);

	if (houseExt->ShouldPlaceDefenseAtBlockedEdge)
	{
		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Towards_Expansion_Placement_Cell_Value);
	}

	int paranoiaDuration = TICKS_PER_MINUTE + (30 * TICKS_PER_SECOND);

	CellStruct targetBuilding(0, 0);
	CellStruct targetAttacker(0, 0);

	// 1. Check Frontline Threat (from crawler placement)
	if (houseExt->FrontlineThreatCoords.X > 0 && houseExt->FrontlineThreatActiveFrames > Unsorted::CurrentFrame && houseExt->FrontlineThreatNeedsDefenses > 0)
	{
		targetAttacker = houseExt->FrontlineThreatCoords;
		targetBuilding = houseExt->FrontlineThreatBuildingCoords;
	}
	// 2. Check Self Attacked
	else if (pOwner->LATime > 0 && pOwner->LATime + paranoiaDuration + 1800 > Unsorted::CurrentFrame)
	{
		if (houseExt->LastAttackerCoords.X > 0)
			targetAttacker = houseExt->LastAttackerCoords;
		if (houseExt->LastAttackedBuildingCoords.X > 0)
			targetBuilding = houseExt->LastAttackedBuildingCoords;
	}
	// 3. Check Ally Attacked
	else
	{
		for (const auto pOtherOwner : HouseClass::Array)
		{
			if (pOtherOwner != pOwner && pOwner->IsAlliedWith(pOtherOwner))
			{
				int otherParanoia = TICKS_PER_MINUTE + (30 * TICKS_PER_SECOND);

				if (pOtherOwner->LATime > 0 && pOtherOwner->LATime + otherParanoia + 1800 > Unsorted::CurrentFrame)
				{
					const auto otherHouseExt = HouseExt::ExtMap.Find(pOtherOwner);
					if (otherHouseExt->LastAttackerCoords.X > 0)
						targetAttacker = otherHouseExt->LastAttackerCoords;
					break;
				}
			}
		}
	}

	// 4. Setup AttackCells and select appropriate valuation function
	bool hasDirectionalTargets = false;
	if (targetAttacker.X > 0 && targetBuilding.X > 0)
	{
		ExtData::AttackCell = targetBuilding;
		ExtData::AttackSourceCell = targetAttacker;
		hasDirectionalTargets = true;
	}
	else if (targetAttacker.X > 0)
	{
		ExtData::AttackCell = targetAttacker;
	}
	else if (targetBuilding.X > 0)
	{
		ExtData::AttackCell = targetBuilding;
	}

	// If we didn't find any threat/attacker above, check for undefended expansion refinery
	if (ExtData::AttackCell.X <= 0 || ExtData::AttackCell.Y <= 0)
	{
		const BuildingClass* pOurConYard = pOwner->ConYards.Count > 0 ? pOwner->ConYards[0] : nullptr;
		if (pOurConYard != nullptr)
		{
			for (const auto pBld : pOwner->Buildings)
			{
				if (pBld && pBld->Type && pBld->Type->Refinery)
				{
					if (pBld->GetMapCoords().DistanceFromSquared(pOurConYard->GetMapCoords()) >= 400.0)
					{
						bool isProtected = false;
						for (const auto pOther : pOwner->Buildings)
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
							ExtData::AttackCell = pBld->GetMapCoords();
							break;
						}
					}
				}
			}
		}
	}

	// Call Find_Best_Building_Placement_Cell with the chosen method
	if (ExtData::AttackCell.X > 0 && ExtData::AttackCell.Y > 0)
	{
		if (hasDirectionalTargets)
		{
			return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Directional_Defense_Placement_Cell_Value);
		}
		else
		{
			return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_AttackCell_Cell_Value);
		}
	}

	// Special behaviour if we are under danger of getting rushed.
	// Defend our ConYard and refinery.
	if (houseExt->IsUnderStartRushThreat)
	{
		if (pOwner->ActiveBuildingTypes.GetItemCount(pBuilding->Type->ArrayIndex) < 3)
			return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_ConYard_Placement_Position_Value);

		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_Refinery_Placement_Position_Value);
	}

	// If we are expanding, then it's likely we should build defenses towards the expansion node.
	// However, only so if we are not under immediate threat.
	const bool percentChance75 = ScenarioClass::Instance->Random.RandomRanged(0, 99) < 75;
	if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0 && percentChance75)
		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Towards_Expansion_Placement_Cell_Value);

	const HouseClass* pEnemy = nullptr;
	if (pOwner->EnemyHouseIndex >= 0 && pOwner->EnemyHouseIndex < HouseClass::Array.Count)
		pEnemy = HouseClass::Array[pOwner->EnemyHouseIndex];

	// Place some defenses to the backline.
	const bool percentChance20 = ScenarioClass::Instance->Random.RandomRanged(0, 99) < 20;
	if (percentChance20)
		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_ConYard_Placement_Position_Value);

	// If we have no designed enemy, look for one.
	if (pEnemy == nullptr)
		pEnemy = Find_Closest_Opponent(pOwner);

	// Place some defenses around the center of our base to defend against cheese and flank attacks.
	const bool percentChance30 = ScenarioClass::Instance->Random.RandomRanged(0, 99) < 20;
	if (pEnemy == nullptr || percentChance30)
		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_Base_Center_Defense_Placement_Position_Value);

	return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_Enemy_Placement_Position_Value);
}

CellStruct BuildingExt::Get_Best_Sensor_Placement_Position(BuildingClass* pBuilding)
{
	const int adjacency = pBuilding->Type->Adjacent;
	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false), pBuilding->Type);
	return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_Base_Center_Placement_Position_Value);
}

CellStruct BuildingExt::Get_Best_Placement_Position(BuildingClass* pBuilding)
{
	ExtData::OurBuildingCount = 0;
	if (pBuilding != nullptr && pBuilding->Owner != nullptr)
	{
		for (const auto pOtherBuilding : BuildingClass::Array)
		{
			if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding->Owner == pBuilding->Owner && !pOtherBuilding->Type->InvisibleInGame)
			{
				ExtData::OurBuildings[ExtData::OurBuildingCount++] = pOtherBuilding;
				if (ExtData::OurBuildingCount >= std::size(ExtData::OurBuildings))
				{
					break;
				}
			}
		}
	}

	if (pBuilding->Type->ResourceDestination)
	{
		const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
		if ((houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0) ||
			(houseExt->NextRefineryPlacementLocation.X > 0 && houseExt->NextRefineryPlacementLocation.Y > 0))
		{
			return Get_Best_Expansion_Placement_Position(pBuilding);
		}
		return Get_Best_Refinery_Placement_Position(pBuilding);
	}

	if (GetSupportRadiusType(pBuilding->Type) != SupportRadiusType::None || TechTreeTypeClass::TotalBuildSupport.contains(pBuilding->Type))
	{
		return Get_Best_Support_Placement_Position(pBuilding);
	}

	if (BuildingTypeExt::HasDisableableSuperWeapons(pBuilding->Type))
	{
		return Get_Best_SuperWeapon_Building_Placement_Position(pBuilding);
	}

	if (pBuilding->Type->Factory != AbstractType::None)
	{
		return Get_Best_Factory_Placement_Position(pBuilding);
	}

	if (pBuilding->Type->GetWeapon(0u, false).WeaponType != nullptr)
	{
		return Get_Best_Defense_Placement_Position(pBuilding);
	}

	const auto pTechTree = TechTreeTypeClass::GetAnySuitable(pBuilding->Owner);
	if (pTechTree != nullptr)
	{
		for (const auto pType : pTechTree->BuildServiceDepot)
		{
			if (pBuilding->Type == pType)
			{
				return Get_Best_ServiceDepot_Placement_Position(pBuilding);
			}
		}
	}

	if (pBuilding->Type->SensorArray)
	{
		return Get_Best_Sensor_Placement_Position(pBuilding);
	}

	return Get_Best_Expansion_Placement_Position(pBuilding);
}

void BuildingExt::PopulateAdjacencyAnchors(HouseClass* pOwner, BuildingTypeClass* pBuildingType)
{
	const bool isNaval = pBuildingType->Naval;

	ExtData::OurBuildingCount = 0;
	ExtData::AdjacencyAnchorCount = 0;

	for (const auto pOtherBuilding : BuildingClass::Array)
	{
		if (!pOtherBuilding->IsAlive ||
			pOtherBuilding->InLimbo ||
			pOtherBuilding->Type->InvisibleInGame)
			continue;

		if (pOtherBuilding->Owner == pOwner)
		{
			if (ExtData::OurBuildingCount < std::size(ExtData::OurBuildings))
			{
				ExtData::OurBuildings[ExtData::OurBuildingCount] = pOtherBuilding;
				ExtData::OurBuildingCount++;
			}
		}

		bool isValidAnchor = false;
		if (pOtherBuilding->Owner == pOwner)
			isValidAnchor = true;
		else if (pOtherBuilding->Owner != nullptr && pOwner->IsAlliedWith(pOtherBuilding->Owner) && pOtherBuilding->Type->EligibileForAllyBuilding)
		{
			if (CanAIBuildOffThisAllyBuilding(pOwner, pBuildingType, pOtherBuilding))
				isValidAnchor = true;
		}

		if (isValidAnchor)
		{
			bool include = false;
			if (isNaval)
				include = pOtherBuilding->Type->ConstructionYard || pOtherBuilding->Type->Naval || IsAIBaseNormal(pOtherBuilding->Type);
			else
				include = pOtherBuilding->Type->ConstructionYard || IsAIBaseNormal(pOtherBuilding->Type);

			if (include && ExtData::AdjacencyAnchorCount < std::size(ExtData::AdjacencyAnchors))
			{
				ExtData::AdjacencyAnchors[ExtData::AdjacencyAnchorCount] = pOtherBuilding;
				ExtData::AdjacencyAnchorCount++;
			}
		}

		if (ExtData::OurBuildingCount >= std::size(ExtData::OurBuildings) && ExtData::AdjacencyAnchorCount >= std::size(ExtData::AdjacencyAnchors))
			break;
	}
}

static bool HasWeapons(TechnoClass* pTechno)
{
	if (pTechno != nullptr)
	{
		for (int i = 0; i < TechnoTypeClass::MaxWeapons; i++)
		{
			if (const auto pWeapon = pTechno->GetWeapon(i))
			{
				if (pWeapon->WeaponType != nullptr)
				{
					return true;
				}
			}
		}
	}
	return false;
}

static bool HasEnemyThreatsNear(CellStruct cell, HouseClass* pOwner, double radius)
{
	const double radiusSq = radius * radius;

	for (const auto pFoot : FootClass::Array)
	{
		if (pFoot && pFoot->IsAlive && !pFoot->InLimbo && pFoot->Owner != pOwner && !pOwner->IsAlliedWith(pFoot->Owner))
		{
			// If it's a neutral unit, only consider it a threat if it has weapons (prevents triggering on ambient animals like cows/sheep)
			if (pFoot->Owner->IsNeutral() && !HasWeapons(pFoot))
			{
				continue;
			}

			if (cell.DistanceFromSquared(pFoot->GetMapCoords()) <= radiusSq)
				return true;
		}
	}

	for (const auto pBld : BuildingClass::Array)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner != pOwner && !pOwner->IsAlliedWith(pBld->Owner))
		{
			// If it's a neutral building, only consider it a threat if it has weapons (civilian defenses, sentinels, traps)
			if (pBld->Owner->IsNeutral() && !HasWeapons(pBld))
			{
				continue;
			}

			if (cell.DistanceFromSquared(pBld->GetMapCoords()) <= radiusSq)
				return true;
		}
	}

	return false;
}

int BuildingExt::Exit_Object_Custom_Position(BuildingClass* pBuilding)
{
	PopulateAdjacencyAnchors(pBuilding->Owner, pBuilding->Type);

	const CellStruct placementCell = Get_Best_Placement_Position(pBuilding);

	const auto houseExtLog = HouseExt::ExtMap.Find(pBuilding->Owner);
	if (houseExtLog != nullptr)
	{
		const CellStruct expansionPoint = houseExtLog->NextExpansionPointLocation;
		if (expansionPoint.X <= 0 || expansionPoint.Y <= 0)
		{
			Debug::Log("[Phobos] AdvAI Placement: House %d is PASSIVE (no active expansion, placing %s at (%d,%d) near main base).\n",
				pBuilding->Owner->ArrayIndex, pBuilding->Type->ID, placementCell.X, placementCell.Y);
		}
		else
		{
			bool isCombat = false;
			if (pBuilding->Owner->EnemyHouseIndex >= 0 && pBuilding->Owner->EnemyHouseIndex < HouseClass::Array.Count)
			{
				const HouseClass* pEnemy = HouseClass::Array[pBuilding->Owner->EnemyHouseIndex];
				if (pEnemy != nullptr)
				{
					if (expansionPoint.DistanceFromSquared(pEnemy->Base_Center()) < 100.0)
					{
						isCombat = true;
					}
					else
					{
						for (const auto pBld : pEnemy->Buildings)
						{
							if (pBld && pBld->IsAlive && !pBld->InLimbo && expansionPoint.DistanceFromSquared(pBld->GetMapCoords()) < 100.0)
							{
								isCombat = true;
								break;
							}
						}
					}
				}
			}

			double dist = placementCell.DistanceFrom(expansionPoint);
			if (isCombat)
			{
				Debug::Log("[Phobos] AdvAI Placement: House %d is in COMBAT CRAWLING mode towards target (%d,%d) (placing %s at (%d,%d), distance to target: %.1f cells).\n",
					pBuilding->Owner->ArrayIndex, expansionPoint.X, expansionPoint.Y, pBuilding->Type->ID, placementCell.X, placementCell.Y, dist);
			}
			else
			{
				Debug::Log("[Phobos] AdvAI Placement: House %d is in RESOURCE EXPANSION mode towards Tiberium at (%d,%d) (placing %s at (%d,%d), distance to target: %.1f cells).\n",
					pBuilding->Owner->ArrayIndex, expansionPoint.X, expansionPoint.Y, pBuilding->Type->ID, placementCell.X, placementCell.Y, dist);
			}
		}
	}

	// If we couldn't find any place for the building, refund it.
	if (placementCell.X <= 0 || placementCell.Y <= 0)
	{
		const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
		if (TechTreeTypeClass::TotalBuildSupport.contains(pBuilding->Type))
		{
			const char* groupID = GetGroupAsID(pBuilding->Type);
			int failures = ++houseExt->GroupConsecutiveFailures[groupID];
			// 30 seconds increments: 450 frames * failures
			int cooldownFrames = 450 * failures;
			// Capped at 10 minutes (9000 frames)
			cooldownFrames = std::min(9000, cooldownFrames);

			houseExt->GroupPlacementCooldowns[groupID] = Unsorted::CurrentFrame + cooldownFrames;
			Debug::Log("AdvAI: BuildSupport %s (Group: %s) failed to place (Failures: %d). Group cooldown set for %ds.\n",
				pBuilding->Type->ID, groupID, failures, cooldownFrames / 15);
		}
		else
		{
			houseExt->PlacementFailedCooldowns[pBuilding->Type] = Unsorted::CurrentFrame + 450; // 30 second cooldown at 15 FPS
			Debug::Log("AdvAI: AI %d failed to place building %s. Putting on placement cooldown for 30s.\n", pBuilding->Owner->ArrayIndex, pBuilding->Type->ID);
		}
		if (pBuilding->Type->ResourceDestination)
		{
			if (houseExt->NextRefineryPlacementLocation.X > 0 && houseExt->NextRefineryPlacementLocation.Y > 0)
			{
				HouseExt::AdvAI_Add_Failed_Expansion_Point(pBuilding->Owner, houseExt->NextRefineryPlacementLocation);
				houseExt->NextRefineryPlacementLocation = CellStruct(0, 0);
			}
		}
		return 0;
	}

	const int result = Try_Place(pBuilding, placementCell);

	if (pBuilding->Type->ResourceDestination)
	{
		if (result != 2)
		{
			const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
			houseExt->NextRefineryPlacementLocation = CellStruct(0, 0);
		}
	}

	if (result == 2)
	{
		if (TechTreeTypeClass::TotalBuildSupport.contains(pBuilding->Type))
		{
			const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
			const char* groupID = GetGroupAsID(pBuilding->Type);
			houseExt->GroupConsecutiveFailures[groupID] = 0;
		}

		if (HasEnemyThreatsNear(placementCell, pBuilding->Owner, 8.0))
		{
			pBuilding->Owner->LATime = Unsorted::CurrentFrame;
			const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
			houseExt->LastAttackedBuildingCoords = placementCell;
			Debug::Log("AdvAI: Placed %s at (%d,%d) near enemy threats! Triggering instant paranoia alert.\n", pBuilding->Type->ID, placementCell.X, placementCell.Y);
		}
	}

	return result;
}

static CellStruct Find_Best_Support_Placement(HouseClass* pHouse, BuildingTypeClass* pBuildingType, BuildingClass* pBuilding)
{
	if (pHouse == nullptr || pBuildingType == nullptr)
		return CellStruct::Empty;

	std::vector<BuildingClass*> existingSupports;
	std::vector<BuildingClass*> baseBuildings;

	for (const auto pOtherBuilding : pHouse->Buildings)
	{
		if (pOtherBuilding && pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo)
		{
			if (IsSameSupportNetwork(pOtherBuilding->Type, pBuildingType))
			{
				existingSupports.push_back(pOtherBuilding);
			}

			const bool isBaseBuilding = pOtherBuilding->Type->ConstructionYard ||
				pOtherBuilding->Type->Factory != AbstractType::None ||
				pOtherBuilding->Type->Refinery ||
				pOtherBuilding->Type->Radar ||
				pOtherBuilding->Type->Helipad ||
				(pOtherBuilding->Type->TechLevel > 0 && !pOtherBuilding->Type->IsBaseDefense && GetSupportRadiusType(pOtherBuilding->Type) == SupportRadiusType::None);

			if (isBaseBuilding)
			{
				baseBuildings.push_back(pOtherBuilding);
			}
		}
	}

	const SupportRadiusType supportType = GetSupportRadiusType(pBuildingType);
	const int radius = GetSupportRadius(pBuildingType);
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

	std::vector<BuildingClass*> pivots;

	if (supportType == SupportRadiusType::None)
	{
		// Non-radius support buildings: pivots are all base buildings,
		// sorted with ConYard and Factories prioritized.
		pivots = baseBuildings;
		std::stable_sort(pivots.begin(), pivots.end(), [](const BuildingClass* a, const BuildingClass* b) {
			if (a->Type->ConstructionYard != b->Type->ConstructionYard)
				return a->Type->ConstructionYard;
			if ((a->Type->Factory != AbstractType::None) != (b->Type->Factory != AbstractType::None))
				return a->Type->Factory != AbstractType::None;
			return false;
		});
	}
	else
	{
		// Radius-based support buildings: identify uncovered base buildings.
		std::vector<BuildingClass*> uncoveredBuildings;
		for (const auto pBaseBld : baseBuildings)
		{
			bool covered = false;
			for (const auto pExist : existingSupports)
			{
				if (pExist != pBuilding && pBaseBld->GetMapCoords().DistanceFrom(pExist->GetMapCoords()) <= coverageDistance)
				{
					covered = true;
					break;
				}
			}

			if (!covered)
			{
				uncoveredBuildings.push_back(pBaseBld);
			}
		}

		if (uncoveredBuildings.empty())
		{
			// All base structures are covered!
			return CellStruct::Empty;
		}

		// Sort uncovered buildings by distance to ConYard or base center
		CellStruct coreCell;
		if (pHouse->ConYards.Count > 0 && pHouse->ConYards[0] != nullptr)
			coreCell = GeneralUtils::CellFromCoordinates(pHouse->ConYards[0]->GetCenterCoords());
		else
			coreCell = pHouse->Base_Center();

		pivots = uncoveredBuildings;
		std::sort(pivots.begin(), pivots.end(), [coreCell](const BuildingClass* a, const BuildingClass* b) {
			return a->GetMapCoords().DistanceFromSquared(coreCell) < b->GetMapCoords().DistanceFromSquared(coreCell);
		});
	}

	const int buildingW = pBuildingType->GetFoundationWidth();
	const int buildingH = pBuildingType->GetFoundationHeight(false);
	const int adjRange = pBuildingType->Adjacent + 1;

	// Loop through pivots to find a valid adjacent placement cell
	for (const auto pPivot : pivots)
	{
		const CellStruct pivotCell = pPivot->GetMapCoords();
		const int pivotW = pPivot->Type->GetFoundationWidth();
		const int pivotH = pPivot->Type->GetFoundationHeight(false);

		const int xMin = pivotCell.X - adjRange - buildingW + 1;
		const int xMax = pivotCell.X + pivotW + adjRange - 1;
		const int yMin = pivotCell.Y - adjRange - buildingH + 1;
		const int yMax = pivotCell.Y + pivotH + adjRange - 1;

		CellStruct bestCellForPivot = CellStruct::Empty;
		int bestRatingForPivot = std::numeric_limits<int>::max();

		for (int y = yMin; y <= yMax; y++)
		{
			for (int x = xMin; x <= xMax; x++)
			{
				CellStruct cell(static_cast<short>(x), static_cast<short>(y));

				if (!MapClass::Instance.CoordinatesLegal(cell))
					continue;

				if (!pBuildingType->CanPlaceHere(&cell, pHouse))
					continue;

				if (BuildingExt::OverlapsTiberiumTreeZone(cell, pBuildingType))
					continue;

				// Congestion check with other base structures
				bool cellIsCongested = false;
				int touchingCount = 0;
				const int b1X = cell.X;
				const int b1Y = cell.Y;
				const int b1W = buildingW;
				const int b1H = buildingH;

				for (const auto pOtherBuilding : pHouse->Buildings)
				{
					if (pOtherBuilding && pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding != pBuilding)
					{
						if (pOtherBuilding->Type->InvisibleInGame)
							continue;

						const int b2X = pOtherBuilding->GetMapCoords().X;
						const int b2Y = pOtherBuilding->GetMapCoords().Y;
						const int b2W = pOtherBuilding->Type->GetFoundationWidth();
						const int b2H = pOtherBuilding->Type->GetFoundationHeight(false);

						// Fast distance check to skip far-away buildings
						const double dx = static_cast<double>(b1X - b2X);
						const double dy = static_cast<double>(b1Y - b2Y);
						if ((dx * dx + dy * dy) > 64.0)
							continue;

						if ((b1X - 1 <= b2X + b2W - 1) && (b1X + b1W >= b2X) &&
							(b1Y - 1 <= b2Y + b2H - 1) && (b1Y + b1H >= b2Y))
						{
							if (pBuildingType->IsBaseDefense && pOtherBuilding->Type->IsBaseDefense)
							{
								cellIsCongested = true;
								break;
							}

							if (pOtherBuilding->Type == pBuildingType)
							{
								cellIsCongested = true;
								break;
							}
							touchingCount++;
							if (touchingCount >= 2)
							{
								cellIsCongested = true;
								break;
							}
						}
					}
				}

				if (cellIsCongested)
					continue;

				// Unsafe placement zone check
				const auto houseExt = HouseExt::ExtMap.Find(pHouse);
				bool isUnsafe = false;
				for (auto it = houseExt->UnsafePlacementZones.begin(); it != houseExt->UnsafePlacementZones.end(); )
				{
					if (Unsorted::CurrentFrame > it->ExpiryFrame)
						it = houseExt->UnsafePlacementZones.erase(it);
					else
					{
						if (cell.DistanceFromSquared(it->Coords) < 100.0)
						{
							isUnsafe = true;
							break;
						}
						++it;
					}
				}
				if (isUnsafe)
					continue;

				// Separation rule check (radius-based support buildings only)
				if (supportType != SupportRadiusType::None)
				{
					bool tooCloseToSameType = false;
					for (const auto pExist : existingSupports)
					{
						if (pExist != pBuilding && cell.DistanceFrom(pExist->GetMapCoords()) < targetSeparation)
						{
							tooCloseToSameType = true;
							break;
						}
					}
					if (tooCloseToSameType)
						continue;
				}

				// Rating: closer to the pivot building
				int rating = static_cast<int>(cell.DistanceFrom(pivotCell) * 10);

				if (rating < bestRatingForPivot)
				{
					bestRatingForPivot = rating;
					bestCellForPivot = cell;
				}
			}
		}

		if (bestCellForPivot.X > 0 && bestCellForPivot.Y > 0)
		{
			return bestCellForPivot;
		}
	}

	return CellStruct::Empty;
}

bool BuildingExt::AdvAI_Is_Support_Placement_Feasible(HouseClass* pHouse, BuildingTypeClass* pBuildingType)
{
	CellStruct cell = Find_Best_Support_Placement(pHouse, pBuildingType, nullptr);
	return cell.X > 0 && cell.Y > 0;
}

CellStruct BuildingExt::Get_Best_Support_Placement_Position(BuildingClass* pBuilding)
{
	if (pBuilding == nullptr || pBuilding->Owner == nullptr)
		return CellStruct::Empty;
	return Find_Best_Support_Placement(pBuilding->Owner, pBuilding->Type, pBuilding);
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
