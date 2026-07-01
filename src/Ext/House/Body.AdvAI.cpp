#include "Body.h"
#include <Ext/BuildingType/Body.h>
#include <Ext/Building/Body.h>

#include <AircraftClass.h>
#include <functional>
#include <GameOptionsClass.h>
#include <OverlayClass.h>
#include <TerrainClass.h>

#define TICKS_PER_SECOND    15
#define TICKS_PER_MINUTE    (TICKS_PER_SECOND * 60)
#define TICKS_PER_HOUR      (TICKS_PER_MINUTE * 60)

std::vector<BuildingTypeClass*> HouseExt::BaseDefenses;
bool HouseExt::BaseDefensesInitialized;

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

bool HouseExt::AdvAI_House_Search_For_Next_Expansion_Point(HouseClass* pHouse)
{
	const auto ext = ExtMap.Find(pHouse);

	if (ext->NextExpansionPointLocation.X != 0 && ext->NextExpansionPointLocation.Y != 0)
	{
		return false;
	}

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

	// Scan through terrain objects that spawn Tiberium (Tiberium Trees) AND map cells containing
	// Tiberium/Ore overlays. Pick the one that is closest to any of our own structures and does not
	// have a refinery near it yet. This allows the AI to expand on any map, whether it uses Tiberium trees or not.
	std::vector<CellStruct> candidates;

	// 1. Scan TerrainClass for Tiberium trees
	for (const auto pTerrain : TerrainClass::Array)
	{
		if (pTerrain->IsAlive && !pTerrain->InLimbo && pTerrain->Type->SpawnsTiberium)
		{
			candidates.push_back(pTerrain->GetMapCoords());
		}
	}

	// 2. Scan Map for cells containing Tiberium or Ore overlays
	const auto& visibleRect = MapClass::Instance.VisibleRect;
	// Safety check: ensure map rect is initialized and valid to prevent out of bounds or infinite loops
	if (visibleRect.Width > 0 && visibleRect.Height > 0 && visibleRect.Width <= 512 && visibleRect.Height <= 512)
	{
		const int scanStep = 4; // Scan every 4th cell to be fast and cover the map fields safely
		for (int y = visibleRect.Y; y < visibleRect.Y + visibleRect.Height; y += scanStep)
		{
			for (int x = visibleRect.X; x < visibleRect.X + visibleRect.Width; x += scanStep)
			{
				CellStruct cellCoords = CellStruct(static_cast<short>(x), static_cast<short>(y));
				if (!MapClass::Instance.CoordinatesLegal(cellCoords))
					continue;

				const CellClass* cell = MapClass::Instance.GetCellAt(cellCoords);
				if (cell && cell->OverlayTypeIndex != -1)
				{
					int tibType = OverlayClass::GetTiberiumType(cell->OverlayTypeIndex);
					if (tibType >= 0)
					{
						candidates.push_back(cellCoords);
					}
				}
			}
		}
	}

	double nearestDistance = std::numeric_limits<double>::max();
	CellStruct target = CellStruct();
	const BuildingClass* pBestNearestBuilding = nullptr;

	int totalNodesChecked = 0;
	int occupiedNodes = 0;

	for (const auto& candidateCell : candidates)
	{
		totalNodesChecked++;

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

		bool found = false;
		for (const auto pBuilding : BuildingClass::Array)
		{
			if (!pBuilding->IsAlive || pBuilding->InLimbo || !pBuilding->Type->ResourceDestination)
			{
				continue;
			}

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
			if (dist < 15)
			{
				found = true;
				break;
			}
		}

		if (found)
		{
			occupiedNodes++;
			continue; // Someone is already occupying this Tiberium cell/field
		}

		// Find the distance from our nearest owned structure to this candidate cell.
		double distanceFromNearestOwnedStructure = std::numeric_limits<double>::max();
		const BuildingClass* pNearestBuilding = nullptr;
		for (const auto pBuilding : BuildingClass::Array)
		{
			if (!pBuilding->IsAlive || pBuilding->InLimbo || pBuilding->Owner != pHouse)
				continue;

			const double dist = pBuilding->GetMapCoords().DistanceFrom(candidateCell);
			if (dist < distanceFromNearestOwnedStructure)
			{
				distanceFromNearestOwnedStructure = dist;
				pNearestBuilding = pBuilding;
			}
		}

		if (distanceFromNearestOwnedStructure < nearestDistance)
		{
			nearestDistance = distanceFromNearestOwnedStructure;
			target = candidateCell;
			pBestNearestBuilding = pNearestBuilding;
			Debug::Log("AdvAI ExpansionSearch: House %d: Tiberium cell at (%d,%d) is free. Nearest structure: %s at dist %.1f cells. New best target.\n",
				pHouse->ArrayIndex, candidateCell.X, candidateCell.Y,
				pNearestBuilding ? pNearestBuilding->Type->ID : "None",
				distanceFromNearestOwnedStructure);
		}
	}

	Debug::Log("AdvAI ExpansionSearch: House %d: Checked %d Tiberium nodes, %d occupied/blocked. Target: (%d,%d).\n",
		pHouse->ArrayIndex, totalNodesChecked, occupiedNodes, target.X, target.Y);

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
							if (distSq < nearestEnemyDistSq && pOurBld->IsInSameZoneAs(pBld))
							{
								nearestEnemyDistSq = distSq;
								enemyTarget = pBld->GetMapCoords();
							}
						}
					}
				}
			}

			// If the enemy base is within crawling range (e.g. 80 cells, squared is 6400)
			if (nearestEnemyDistSq <= 6400.0 && enemyTarget.X > 0 && enemyTarget.Y > 0)
			{
				ext->NextExpansionPointLocation = enemyTarget;
				Debug::Log("AdvAI ExpansionSearch: House %d: All Tiberium fields taken. Crawling towards enemy base at (%d,%d) at dist %.1f cells.\n",
					pHouse->ArrayIndex, enemyTarget.X, enemyTarget.Y, std::sqrt(nearestEnemyDistSq));
				return true;
			}
		}

		return false;
	}

	ext->NextExpansionPointLocation = target;

	return true;
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
				if (pOurRefinery != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s because it has reached an expansion point\n", pOurRefinery->Name);
					return pOurRefinery;
				}
				else
				{
					Debug::Log("AdvAI: House %d reached expansion point, but refinery is physically unbuildable here. Reverting ShouldBuildRefinery to crawl further.\n", pHouse->ArrayIndex);
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

		if (!isUnderThreat && Unsorted::CurrentFrame > 5000 && (pHouse->PowerOutput - pHouse->PowerDrain < 100 || hasUnpoweredBuildings))
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
		size_t maxBarracksLimit = 8;
		if (pHouse->AIDifficulty == AIDifficulty::Easy)
			maxBarracksLimit = 6;
		else if (pHouse->AIDifficulty == AIDifficulty::Hard)
			maxBarracksLimit = 12;

		if (optimalBarracksCount > maxBarracksLimit)
			optimalBarracksCount = maxBarracksLimit;

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
		if (pHouse->AIDifficulty == AIDifficulty::Normal)
			paranoiaDuration = 2 * TICKS_PER_MINUTE;
		else if (pHouse->AIDifficulty == AIDifficulty::Hard)
			paranoiaDuration = 3 * TICKS_PER_MINUTE;

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
				int powerSurplus = pHouse->PowerOutput - pHouse->PowerDrain;
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
				if (antiAirDeficiency > 0 && ourAntiAirDefense != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s because it is paranoid/protecting and faces airborne threats. Deficiency: %d, EnemyAircraftVal: %d\n",
						ourAntiAirDefense->Name, antiAirDeficiency, enemyAircraftValue);
					return ourAntiAirDefense;
				}

				const bool hasInfDeficiency = antiInfDeficiency > 0;
				const bool hasVehDeficiency = antiVehicleDeficiency > 0;

				if (hasInfDeficiency && hasVehDeficiency)
				{
					if (antiInfDeficiency > antiVehicleDeficiency && ourAntiInfantryDefense != nullptr)
					{
						Debug::Log("AdvAI: Making AI build %s (anti-inf more urgent, paranoid/protecting). InfDef: %d > VehDef: %d\n",
							ourAntiInfantryDefense->Name, antiInfDeficiency, antiVehicleDeficiency);
						return ourAntiInfantryDefense;
					}
					else if (antiVehicleDeficiency > antiInfDeficiency && ourAntiVehicleDefense != nullptr)
					{
						Debug::Log("AdvAI: Making AI build %s (anti-vehicle more urgent, paranoid/protecting). VehDef: %d > InfDef: %d\n",
							ourAntiVehicleDefense->Name, antiVehicleDeficiency, antiInfDeficiency);
						return ourAntiVehicleDefense;
					}
					else
					{
						const bool pickVehicle = (ScenarioClass::Instance->Random.RandomRanged(0, 1) == 0);
						if (pickVehicle && ourAntiVehicleDefense != nullptr)
						{
							Debug::Log("AdvAI: Making AI build %s (equal deficiency, random pick: vehicle, paranoid/protecting).\n", ourAntiVehicleDefense->Name);
							return ourAntiVehicleDefense;
						}
						else if (ourAntiInfantryDefense != nullptr)
						{
							Debug::Log("AdvAI: Making AI build %s (equal deficiency, random pick: infantry, paranoid/protecting).\n", ourAntiInfantryDefense->Name);
							return ourAntiInfantryDefense;
						}
					}
				}
				else if (hasInfDeficiency && ourAntiInfantryDefense != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s because only anti-inf is deficient (paranoid/protecting). InfDef: %d\n",
						ourAntiInfantryDefense->Name, antiInfDeficiency);
					return ourAntiInfantryDefense;
				}
				else if (hasVehDeficiency && ourAntiVehicleDefense != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s because only anti-vehicle is deficient (paranoid/protecting). VehDef: %d\n",
						ourAntiVehicleDefense->Name, antiVehicleDeficiency);
					return ourAntiVehicleDefense;
				}
			}
		}

		// If we are under threat of an immediate early-game rush, then skip the WF and refinery minimums.
		// Instead build defenses or tech up so we can get AA ASAP.
		if (!isUnderThreat || (antiInfDeficiency <= 0 && antiAirDeficiency <= 0))
		{
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
			size_t maxWFLimit = 8;
			if (pHouse->AIDifficulty == AIDifficulty::Easy)
				maxWFLimit = 6;
			else if (pHouse->AIDifficulty == AIDifficulty::Hard)
				maxWFLimit = 12;

			if (optimalWeaponsCount > maxWFLimit)
				optimalWeaponsCount = maxWFLimit;

			const BuildingTypeClass* pWeaponsFactoryToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildWeapons, 1, optimalWeaponsCount);

			if (pWeaponsFactoryToBuild != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because it does not have enough Weapons Factories. Wanted: %d (Competitor max: %d)\n",
					pWeaponsFactoryToBuild->Name, optimalWeaponsCount, maxWFOwnedByOther);

				return pWeaponsFactoryToBuild;
			}

			// Find the maximum number of Naval Yards owned by any other house
			int maxNavalYardsOwnedByOther = 0;
			for (const auto pOtherHouse : HouseClass::Array)
			{
				if (pOtherHouse == pHouse || pHouse->IsAlliedWith(pOtherHouse))
					continue;

				int otherNavalYardCount = 0;
				for (const auto pBuilding : BuildingClass::Array)
				{
					if (pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Owner == pOtherHouse)
					{
						if (pBuilding->Type->Factory == AbstractType::UnitType && pBuilding->Type->Naval)
						{
							otherNavalYardCount++;
						}
					}
				}

				if (otherNavalYardCount > maxNavalYardsOwnedByOther)
				{
					maxNavalYardsOwnedByOther = otherNavalYardCount;
				}
			}

			size_t optimalNavalYardCount = 1;
			if (refineryCount >= 6)
				optimalNavalYardCount = 3;
			else if (refineryCount >= 3)
				optimalNavalYardCount = 2;

			if (!hasTechCenter)
				optimalNavalYardCount = 1;

			// Scale up to match competitors if they build more
			if (static_cast<size_t>(maxNavalYardsOwnedByOther) > optimalNavalYardCount)
			{
				optimalNavalYardCount = static_cast<size_t>(maxNavalYardsOwnedByOther);
			}

			// Apply difficulty-based safety cap for Naval Yards (Easy: 3, Normal: 5, Hard: 8)
			size_t maxNavalYardLimit = 5;
			if (pHouse->AIDifficulty == AIDifficulty::Easy)
				maxNavalYardLimit = 3;
			else if (pHouse->AIDifficulty == AIDifficulty::Hard)
				maxNavalYardLimit = 8;

			if (optimalNavalYardCount > maxNavalYardLimit)
				optimalNavalYardCount = maxNavalYardLimit;

			const BuildingTypeClass* pNavalYardToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildNavalYard, 1, optimalNavalYardCount);
			if (pNavalYardToBuild != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because it does not have enough Naval Yards. Wanted: %d (Competitor max: %d)\n",
					pNavalYardToBuild->Name, optimalNavalYardCount, maxNavalYardsOwnedByOther);

				return pNavalYardToBuild;
			}

			// If we have too few refineries, build enough to match the minimum.
			// Because this is not for expanding but an emergency situation,
			// cancel any potential expanding.
			int minRefineryCount = RulesExt::Global()->AdvancedAIMinimumRefineryCount;
			if (!hasTechCenter)
				minRefineryCount = std::min(minRefineryCount, 2);

			pRefineryToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildRefinery, 1, minRefineryCount, slaveMinerCount);
			if (pRefineryToBuild != nullptr)
			{
				houseExt->NextExpansionPointLocation = CellStruct(0, 0);
				Debug::Log("AdvAI: Making AI build %s because it only has too few refineries\n", pRefineryToBuild->Name);
				return pRefineryToBuild;
			}
		}

		// Probabilistic roll: 70% chance to build defense when paranoid (threat/attack), 50% chance in normal state.
		const int rollChance = isParanoid ? 70 : 50;
		const bool shouldBuildDefenseThisCycle = (ScenarioClass::Instance->Random.RandomRanged(0, 99) < rollChance);

		if (shouldBuildDefenseThisCycle)
		{
			// Prioritize Anti-Air defense FIRST if there is airborne threat deficiency
			if (antiAirDeficiency > 0 && ourAntiAirDefense != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because it faces airborne threats. Deficiency: %d, EnemyAircraftVal: %d\n",
					ourAntiAirDefense->Name, antiAirDeficiency, enemyAircraftValue);

				return ourAntiAirDefense;
			}

			const bool hasInfDeficiency = antiInfDeficiency > 0;
			const bool hasVehDeficiency = antiVehicleDeficiency > 0;

			if (hasInfDeficiency && hasVehDeficiency)
			{
				if (antiInfDeficiency > antiVehicleDeficiency && ourAntiInfantryDefense != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s (anti-inf more urgent). InfDef: %d > VehDef: %d\n",
						ourAntiInfantryDefense->Name, antiInfDeficiency, antiVehicleDeficiency);
					return ourAntiInfantryDefense;
				}
				else if (antiVehicleDeficiency > antiInfDeficiency && ourAntiVehicleDefense != nullptr)
				{
					Debug::Log("AdvAI: Making AI build %s (anti-vehicle more urgent). VehDef: %d > InfDef: %d\n",
						ourAntiVehicleDefense->Name, antiVehicleDeficiency, antiInfDeficiency);
					return ourAntiVehicleDefense;
				}
				else // Tied deficiency
				{
					const bool pickVehicle = (ScenarioClass::Instance->Random.RandomRanged(0, 1) == 0);
					if (pickVehicle && ourAntiVehicleDefense != nullptr)
					{
						Debug::Log("AdvAI: Making AI build %s (equal deficiency, random pick: vehicle).\n", ourAntiVehicleDefense->Name);
						return ourAntiVehicleDefense;
					}
					else if (ourAntiInfantryDefense != nullptr)
					{
						Debug::Log("AdvAI: Making AI build %s (equal deficiency, random pick: infantry).\n", ourAntiInfantryDefense->Name);
						return ourAntiInfantryDefense;
					}
				}
			}
			else if (hasInfDeficiency && ourAntiInfantryDefense != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because only anti-inf is deficient. InfDef: %d\n",
					ourAntiInfantryDefense->Name, antiInfDeficiency);
				return ourAntiInfantryDefense;
			}
			else if (hasVehDeficiency && ourAntiVehicleDefense != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because only anti-vehicle is deficient. VehDef: %d\n",
					ourAntiVehicleDefense->Name, antiVehicleDeficiency);
				return ourAntiVehicleDefense;
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
			const int ourHelipadCount = pHouse->ActiveBuildingTypes.GetItemCount(pHelipadType->ArrayIndex);
			const int currentDocks = ourHelipadCount * docksPerHelipad;

			size_t optimalHelipadCount = 1;

			// Count all airport-bound aircraft currently owned by this house
			int ownedAirportBoundAircraft = 0;
			for (const auto pAircraft : AircraftClass::Array)
			{
				if (pAircraft->Owner == pHouse && pAircraft->IsAlive && !pAircraft->InLimbo)
				{
					if (pAircraft->Type->AirportBound)
					{
						ownedAirportBoundAircraft++;
					}
				}
			}

			// If we already have helipads, we only build more if all current docks are occupied
			if (ourHelipadCount > 0)
				optimalHelipadCount = (ownedAirportBoundAircraft >= currentDocks) ? ourHelipadCount + 1 : ourHelipadCount;

			// Enforce difficulty-based safety cap of docks (Easy: 8 docks, Normal: 12 docks, Hard: 16 docks)
			int maxDocks = 16;
			if (pHouse->AIDifficulty == AIDifficulty::Easy)
				maxDocks = 8;
			else if (pHouse->AIDifficulty == AIDifficulty::Normal)
				maxDocks = 12;

			const size_t maxHelipadCount = (maxDocks + docksPerHelipad - 1) / docksPerHelipad;
			if (optimalHelipadCount > maxHelipadCount)
				optimalHelipadCount = maxHelipadCount;

			const BuildingTypeClass* pHelipadToBuild = AdvAI_BuildAtLeastNOfSideAndMInTotal(pHouse, pPrimaryTechTree, TechTreeTypeClass::BuildType::BuildHelipad, 1, optimalHelipadCount);
			if (pHelipadToBuild != nullptr)
			{
				Debug::Log("AdvAI: Making AI build %s because it has no free aircraft docks (Docks: %d, Aircraft: %d, Wanted helipads: %d)\n",
					pHelipadToBuild->Name, currentDocks, ownedAirportBoundAircraft, optimalHelipadCount);

				return pHelipadToBuild;
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
			Debug::Log("AdvAI: Making AI build %s because the AI is expanding.\n",
				pOurPowerPlant->Name);
			return pOurPowerPlant;
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
	const BuildingTypeClass* buildChoice = AdvAI_Evaluate_Get_Best_Building(pHouse);

	if (buildChoice == nullptr)
	{
		return nullptr;
	}

	if (buildChoice->PowerDrain > 0 && !buildChoice->ResourceDestination && !buildChoice->ConstructionYard)
	{
		const int expectedSurplus = (pHouse->PowerOutput - pHouse->PowerDrain) - buildChoice->PowerDrain;
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
		if (pHouse->AIDifficulty == AIDifficulty::Normal)
			paranoiaDuration = 2 * TICKS_PER_MINUTE;
		else if (pHouse->AIDifficulty == AIDifficulty::Hard)
			paranoiaDuration = 3 * TICKS_PER_MINUTE;

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

							// 1. Keep at least 2 defenses within 15 cells of the Construction Yard,
							// and NEVER sell any defenses that are outside the main base (>= 20 cells from ConYard).
							if (pOurConYard != nullptr)
							{
								double distToConYardSq = pBld->GetMapCoords().DistanceFromSquared(pOurConYard->GetMapCoords());
								if (distToConYardSq >= 400.0)
								{
									safeToSell = false;
								}
								else if (distToConYardSq < 225.0)
								{
									int defensesNearConYard = 0;
									for (const auto pOther : pHouse->Buildings)
									{
										if (pOther && pOther->IsAlive && !pOther->InLimbo && TechTreeTypeClass::TotalBuildDefense.contains(pOther->Type))
										{
											if (pOther->GetMapCoords().DistanceFromSquared(pOurConYard->GetMapCoords()) < 225.0)
												defensesNearConYard++;
										}
									}
									if (defensesNearConYard <= 2)
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

							if (safeToSell)
							{
								double distSq = pBld->GetMapCoords().DistanceFromSquared(center);
								if (distSq < closestDistSq)
								{
									closestDistSq = distSq;
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
	if (pHouse->EnemyHouseIndex != -1)
	{
		pEnemy = HouseClass::FindByCountryIndex(pHouse->EnemyHouseIndex);
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

	// If we have nowhere to expand towards, check for a new location to expand to.
	if (houseExt->NextExpansionPointLocation.X <= 0 || houseExt->NextExpansionPointLocation.Y <= 0)
	{
		AdvAI_House_Search_For_Next_Expansion_Point(pHouse);
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
	if (!RulesExt::Global()->IsUseAdvancedAI)
	{
		return;
	}

	// If we have more than 1 ConYard without Rules allowing it, sell some of them off
	// to avoid the "Extreme AI" syndrome.
	if (pHouse->ConYards.Count > 1 && !RulesExt::Global()->IsAdvancedAIMultiConYard)
	{
		AdvAI_Sell_Extra_ConYards(pHouse);
	}

	// If we have no enemy, then pick one.
	if (pHouse->EnemyHouseIndex == -1)
	{
		pHouse->Unknown_Timer_5640.Start(0);
	}

	const auto houseExt = ExtMap.Find(pHouse);

	// Do some economy upkeep to keep the AI running.

	if (Unsorted::CurrentFrame > houseExt->LastExcessRefineryCheckFrame + 500)
	{
		houseExt->LastExcessRefineryCheckFrame = Unsorted::CurrentFrame;
		AdvAI_Economy_Upkeep(pHouse);
	}

	if (Unsorted::CurrentFrame > houseExt->LastSleepingHarvesterCheckFrame + 1000)
	{
		houseExt->LastSleepingHarvesterCheckFrame = Unsorted::CurrentFrame;
		AdvAI_Awaken_Sleeping_Harvesters(pHouse);
	}

	if (Unsorted::CurrentFrame > houseExt->LastPrimaryFactoryCheckFrame + 800)
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

	const HouseClass* pEnemy = nullptr;
	if (pHouse->EnemyHouseIndex >= 0 && pHouse->EnemyHouseIndex < HouseClass::Array.Count)
	{
		pEnemy = HouseClass::Array[pHouse->EnemyHouseIndex];
	}

	if (!pEnemy || pEnemy->Buildings.Count == 0)
		return;

	CellStruct targetCoords = pEnemy->Base_Center();

	auto updatePrimary = [pHouse, targetCoords](AbstractType type) {
		BuildingClass* pBestFactory = nullptr;
		double bestDistanceSq = std::numeric_limits<double>::max();

		for (const auto pBuilding : pHouse->Buildings)
		{
			if (pBuilding && pBuilding->IsAlive && !pBuilding->InLimbo && pBuilding->Type->Factory == type)
			{
				double distSq = pBuilding->GetMapCoords().DistanceFromSquared(targetCoords);
				if (distSq < bestDistanceSq)
				{
					bestDistanceSq = distSq;
					pBestFactory = pBuilding;
				}
			}
		}

		if (pBestFactory != nullptr)
		{
			for (const auto pBuilding : pHouse->Buildings)
			{
				if (pBuilding && pBuilding->Type->Factory == type)
				{
					pBuilding->IsPrimaryFactory = (pBuilding == pBestFactory);
				}
			}

			if (pBestFactory->Factory != nullptr)
			{
				pHouse->SetPrimaryFactory(pBestFactory->Factory, type, pBestFactory->Type->Naval, BuildCat::DontCare);
			}
		}
	};

	updatePrimary(AbstractType::InfantryType);
	updatePrimary(AbstractType::UnitType);
}
