#include "Body.h"
#include "Ext/House/Body.h"
#include "Ext/TechnoType/Body.h"

static bool IsAIBaseNormal(const BuildingTypeClass* pType)
{
	const auto pExt = BuildingTypeExt::ExtMap.Find(pType);
	return pExt->AIBaseNormal.Get(pExt->BaseNormal);
}

static bool IsAIInnerBase(const BuildingTypeClass* pType)
{
	const auto pExt = BuildingTypeExt::ExtMap.Find(pType);
	if (pExt->AIInnerBase.isset())
	{
		return pExt->AIInnerBase.Get();
	}
	return pType->CloakGenerator;
}

// Categorizes a building by which kind of area-support radius it provides.
// Buildings that share the same category should be spatially dispersed.
enum class SupportRadiusType { None, Cloak, Gap, Inhibitor, RadarJam };

static SupportRadiusType GetSupportRadiusType(const BuildingTypeClass* pType)
{
	// CloakRadiusInCells is a BYTE in BuildingTypeClass
	if (pType->CloakRadiusInCells > 0)
		return SupportRadiusType::Cloak;

	// GapRadiusInCells / SuperGapRadiusInCells are chars in TechnoTypeClass (inherited)
	if (pType->GapRadiusInCells != 0 || pType->SuperGapRadiusInCells != 0)
		return SupportRadiusType::Gap;

	// InhibitorRange and RadarJamRadius live in TechnoTypeExt
	const auto pTechnoTypeExt = TechnoTypeExt::ExtMap.Find(pType);
	if (pTechnoTypeExt->InhibitorRange.isset() && pTechnoTypeExt->InhibitorRange.Get() > 0)
		return SupportRadiusType::Inhibitor;

	if (pTechnoTypeExt->RadarJamRadius.Get() > 0)
		return SupportRadiusType::RadarJam;

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

		int closeEnough = 7;

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

		// Check if we placed a refinery.
		// If yes, check if we were expanding. If yes, the expanding is done.
		// If not, but we're close to an expansion field, then flag us to build a refinery as our next building.
		if (pBuilding->Type->ResourceDestination)
		{
			if (houseExt->NextExpansionPointLocation.X != 0 && houseExt->NextExpansionPointLocation.Y != 0)
			{
				const auto buildingExt = ExtMap.Find(pBuilding);
				buildingExt->AssignedExpansionPoint = houseExt->NextExpansionPointLocation;
			}

			Mark_Expansion_As_Done(owner);
			houseExt->ShouldBuildRefinery = false;
		}
		else if (houseExt->NextExpansionPointLocation.X > 0 &&
			houseExt->NextExpansionPointLocation.Y > 0 &&
			GeneralUtils::CellFromCoordinates(pBuilding->GetCenterCoords()).DistanceFrom(houseExt->NextExpansionPointLocation) < closeEnough)
		{
			houseExt->ShouldBuildRefinery = true;
		}

		return 2;
	}

	return 0;
}

/**
 *  Fetches a house's base area as a rectangle.
 *  We can use this as a rough zone for placing new buildings.
 */
RectangleStruct BuildingExt::Get_Base_Rect(HouseClass* pHouse, int adjacency, int width, int height)
{
	int x = INT_MAX;
	int y = INT_MAX;
	int right = INT_MIN;
	int bottom = INT_MIN;

	for (const auto building : BuildingClass::Array)
	{
		if (!building->IsAlive || building->InLimbo || building->Owner != pHouse)
		{
			continue;
		}

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
			if (cell.DistanceFromSquared(it->Coords) < 64.0)
			{
				isCellUnsafe = true;
				break;
			}
			++it;
		}
	}

	if (isCellUnsafe)
	{
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
			if (cell.DistanceFromSquared(it->Coords) < 64.0)
			{
				isCellUnsafe = true;
				break;
			}
			++it;
		}
	}

	if (isCellUnsafe)
	{
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
	int lowestRating = INT_MAX;
	CellStruct bestCell = CellStruct(0, 0);
	
	// Check the resolution of the scan. If our base area is huge, we can't check as precisely
	// or we'll cause into performance issues.
	const int resCells = 2000;
	const int areaSize = baseArea.Width * baseArea.Height;
	const int resolution = 1 + (areaSize / resCells);

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
						if (dist < 2.0) // Less than 2 cells means touching or adjacent (0 or 1 empty cells between them)
						{
							tooCloseToDefense = true;
							break;
						}
					}
				}
				if (tooCloseToDefense)
				{
					value += 10000; // Add a significant rating penalty (lowest is best)
				}
			}

			// Enforce spacing between factories to prevent unit exit traffic jams.
			if (pBuilding->Type->Factory != AbstractType::None)
			{
				bool tooCloseToFactory = false;
				const int x1 = cell.X - 1;
				const int y1 = cell.Y - 1;
				const int w1 = pBuilding->Type->GetFoundationWidth() + 2;
				const int h1 = pBuilding->Type->GetFoundationHeight(false) + 2;

				for (const auto pOtherBuilding : BuildingClass::Array)
				{
					if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding->Type->Factory != AbstractType::None && pOtherBuilding != pBuilding)
					{
						const int x2 = pOtherBuilding->GetMapCoords().X;
						const int y2 = pOtherBuilding->GetMapCoords().Y;
						const int w2 = pOtherBuilding->Type->GetFoundationWidth();
						const int h2 = pOtherBuilding->Type->GetFoundationHeight(false);

						if ((x1 < x2 + w2) && (x1 + w1 > x2) && (y1 < y2 + h2) && (y1 + h1 > y2))
						{
							tooCloseToFactory = true;
							break;
						}
					}
				}
				if (tooCloseToFactory)
					value += 5000; // Add a moderate rating penalty so spacing is preferred if terrain/space allows
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
				}

				// --- Dispersion penalty: avoid clustering with other buildings of the same radius type ---
				bool tooCloseToSameType = false;
				double closestSameTypeDist = 999.0;
				for (const auto pOtherBuilding : BuildingClass::Array)
				{
					if (pOtherBuilding->IsAlive && !pOtherBuilding->InLimbo && pOtherBuilding->Owner == pBuilding->Owner && pOtherBuilding != pBuilding)
					{
						if (GetSupportRadiusType(pOtherBuilding->Type) == supportRadiusType)
						{
							double dist = cell.DistanceFrom(pOtherBuilding->GetMapCoords());
							if (dist < closestSameTypeDist)
								closestSameTypeDist = dist;
							if (dist < 3.0)
								tooCloseToSameType = true;
						}
					}
				}

				if (tooCloseToSameType)
				{
					value += 20000;
				}
				else if (closestSameTypeDist < 8.0)
				{
					value += static_cast<int>((8.0 - closestSameTypeDist) * 1500);
				}
			}

			// Check whether this is the best placement cell so far.
			if (value < lowestRating)
			{
				lowestRating = value;
				bestCell = cell;
			}
		}
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

	// The closer the building is to an existing building, the worse the placement position is.
	// In other words, being closer INCREASES the value (as lower is better).
	return value + (1000 - static_cast<int>(closest_distance));
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
		// For refinery placement, we can basically make the value equal to the distance
		// that the refinery has to our next expansion point.
		value = cell.DistanceFrom(houseExt->NextExpansionPointLocation);
	}

	// Take proximity into nearby buildings into account.
	// We do this to avoid traffic congestion in tight spaces in the AI's base.
	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(value));
}

/**
 *  Calculates the best refinery placement location.
 */
CellStruct BuildingExt::Get_Best_Refinery_Placement_Position(BuildingClass* pBuilding)
{
	const int adjacency = pBuilding->Type->Adjacent + 1;
	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false));
	return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Refinery_Placement_Cell_Value, 0);
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
	for (int i = 0; i < ExtData::OurBuildingCount; i++)
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
	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false));
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
		return SHRT_MAX - static_cast<int>(balancedDist);
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
	// that the building has to our next expansion point.
	// Also, secondarily take distance into enemy into account.
	return static_cast<int>(cell.DistanceFrom(houseExt->NextExpansionPointLocation) * 100) + enemyDistance;
}

CellStruct BuildingExt::Get_Best_Expansion_Placement_Position(BuildingClass* pBuilding)
{
	HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	const int buildingW = pBuilding->Type->GetFoundationWidth();
	const int buildingH = pBuilding->Type->GetFoundationHeight(false);
	// +1 allows one cell of adjacency leniency to hop over small gaps.
	const int adjRange = pBuilding->Type->Adjacent + 1;

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
					if (!pBuilding->Type->CanPlaceHere(&cell, pOwner))
						continue;

					// Check if this cell is close to a recently destroyed building (unsafe zone)
					bool isUnsafe = false;
					for (auto it = houseExt->UnsafePlacementZones.begin(); it != houseExt->UnsafePlacementZones.end(); )
					{
						if (Unsorted::CurrentFrame > it->ExpiryFrame)
							it = houseExt->UnsafePlacementZones.erase(it);
						else
						{
							if (cell.DistanceFromSquared(it->Coords) < 64.0)
							{
								isUnsafe = true;
								break;
							}
							++it;
						}
					}
					if (isUnsafe)
					{
						const BuildingClass* pOurConYard = pOwner->ConYards.Count > 0 ? pOwner->ConYards[0] : nullptr;
						if (pOurConYard == nullptr || cell.DistanceFromSquared(pOurConYard->GetMapCoords()) >= 400.0)
							continue;
					}

					const double dist = cell.DistanceFrom(expansionTarget);
					if (dist < bestDist)
					{
						bestDist = dist;
						bestCell = cell;
					}
				}
			}
		}

		if (bestCell.X > 0 || bestCell.Y > 0)
		{
			// Check if this new cell gets us closer to the expansion point than
			// any of our existing buildings. If not, we are as close as we can
			// get and should build the refinery next.
			if (!houseExt->ShouldBuildRefinery)
			{
				double nearestExistingDistSq = std::numeric_limits<double>::max();
				for (size_t i = 0; i < ExtData::OurBuildingCount; i++)
				{
					const double dSq = expansionTarget.DistanceFromSquared(ExtData::OurBuildings[i]->GetMapCoords());
					if (dSq < nearestExistingDistSq)
						nearestExistingDistSq = dSq;
				}

				const double bestDistSq = bestDist * bestDist;
				if (bestDistSq >= nearestExistingDistSq || nearestExistingDistSq < 225.0)
					houseExt->ShouldBuildRefinery = true;
			}

			houseExt->ExpansionPlacementFailures = 0;

			Debug::Log("AdvAI ExpansionPlacement: House %d placing %s at (%d,%d), dist to target (%d,%d) = %.1f cells%s\n",
				pOwner->ArrayIndex, pBuilding->Type->ID,
				bestCell.X, bestCell.Y,
				expansionTarget.X, expansionTarget.Y,
				bestDist,
				houseExt->ShouldBuildRefinery ? " [REFINERY NEXT]" : "");

			return bestCell;
		}

		houseExt->ExpansionPlacementFailures++;
		if (houseExt->ExpansionPlacementFailures >= 3)
		{
			Debug::Log("AdvAI ExpansionPlacement: House %d: failed to crawl towards target (%d,%d) 3 times. Abandoning expansion target.\n",
				pOwner->ArrayIndex, expansionTarget.X, expansionTarget.Y);
			Mark_Expansion_As_Done(pOwner);
			houseExt->ExpansionPlacementFailures = 0;
			houseExt->ShouldBuildRefinery = false;
		}
		else
		{
			Debug::Log("AdvAI ExpansionPlacement: House %d: no valid adjacent cell found for %s toward target (%d,%d). Failure count: %d. Falling back.\n",
				pOwner->ArrayIndex, pBuilding->Type->ID, expansionTarget.X, expansionTarget.Y, houseExt->ExpansionPlacementFailures);
		}
	}

	// Fallback: no expansion target set, or couldn't find any adjacent valid cell.
	// Use the old bounding-box scan to find a reasonable placement.
	const int adjacency = adjRange + 1;
	const RectangleStruct baseArea = Get_Base_Rect(pOwner, adjacency, buildingW, buildingH);

	CellStruct bestCell = Find_Best_Building_Placement_Cell(baseArea, pBuilding, Towards_Expansion_Placement_Cell_Value, 0);

	// Retry with adjacency bonus to allow hopping over small terrain gaps.
	const CellStruct altBestCell = Find_Best_Building_Placement_Cell(baseArea, pBuilding, Towards_Expansion_Placement_Cell_Value, 1);
	if (bestCell.DistanceFrom(altBestCell) > 1 && CellStruct::Empty != altBestCell)
		bestCell = altBestCell;

	return bestCell;
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

	// If we are expanding (Tiberium or aggressive enemy crawl), place the helipad near the front target
	if (houseExt->NextExpansionPointLocation.X > 0 && houseExt->NextExpansionPointLocation.Y > 0)
	{
		double value = cell.DistanceFrom(houseExt->NextExpansionPointLocation);
		return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(value));
	}

	return Far_From_Enemy_Placement_Position_Value(cell, pBuilding);
}

/**
 *  Calculates the best factory placement location.
 */
CellStruct BuildingExt::Get_Best_Factory_Placement_Position(BuildingClass* pBuilding)
{
	const bool isNaval = pBuilding->Type->Factory == AbstractType::UnitType && pBuilding->Type->Naval;

	const int adjacency = isNaval ? RulesClass::Instance->AINavalYardAdjacency : pBuilding->Type->Adjacent;

	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false));

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

int BuildingExt::Near_AttackCell_Cell_Value(CellStruct cell, BuildingClass* pBuilding)
{
	return Modify_Rating_By_Allied_Building_Proximity(cell, pBuilding, static_cast<int>(cell.DistanceFrom(ExtData::AttackCell)));
}

CellStruct BuildingExt::Get_Best_Defense_Placement_Position(BuildingClass* pBuilding)
{
	const HouseClass* pOwner = pBuilding->Owner;
	const auto houseExt = HouseExt::ExtMap.Find(pOwner);

	ExtData::AttackCell = CellStruct(0, 0);

	const int adjacency = pBuilding->Type->Adjacent;
	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false));

	int paranoiaDuration = TICKS_PER_MINUTE;
	if (pOwner->AIDifficulty == AIDifficulty::Normal)
		paranoiaDuration = 2 * TICKS_PER_MINUTE;
	else if (pOwner->AIDifficulty == AIDifficulty::Hard)
		paranoiaDuration = 3 * TICKS_PER_MINUTE;

	// If we were attacked recently, place the defense near the last attacked building location.
	if (pOwner->LATime + paranoiaDuration > Unsorted::CurrentFrame && houseExt->LastAttackedBuildingCoords.X > 0)
		ExtData::AttackCell = houseExt->LastAttackedBuildingCoords;

	// If we have an undefended expansion refinery, prioritize placing defenses near it.
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

	if (ExtData::AttackCell.X > 0 && ExtData::AttackCell.Y > 0)
		return Find_Best_Building_Placement_Cell(baseArea, pBuilding, Near_AttackCell_Cell_Value);

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
	const RectangleStruct baseArea = Get_Base_Rect(pBuilding->Owner, adjacency, pBuilding->Type->GetFoundationWidth(), pBuilding->Type->GetFoundationHeight(false));
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
		return Get_Best_Refinery_Placement_Position(pBuilding);
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

	if (pBuilding->Type->SensorArray)
	{
		return Get_Best_Sensor_Placement_Position(pBuilding);
	}

	return Get_Best_Expansion_Placement_Position(pBuilding);
}

void BuildingExt::PopulateAdjacencyAnchors(HouseClass* pOwner, BuildingTypeClass* pBuildingType)
{
	const bool buildOffAlly = SessionClass::IsCampaign() ? RulesClass::Instance->BuildOffAlly : GameModeOptionsClass::Instance.BuildOffAlly;
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
		else if (buildOffAlly && pOwner->IsAlliedWith(pOtherBuilding->Owner) && pOtherBuilding->Type->EligibileForAllyBuilding)
			isValidAnchor = true;

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

static bool HasEnemyThreatsNear(CellStruct cell, HouseClass* pOwner, double radius)
{
	const double radiusSq = radius * radius;

	for (const auto pFoot : FootClass::Array)
	{
		if (pFoot && pFoot->IsAlive && !pFoot->InLimbo && pFoot->Owner != pOwner && !pFoot->Owner->IsNeutral() && !pOwner->IsAlliedWith(pFoot->Owner))
		{
			if (cell.DistanceFromSquared(pFoot->GetMapCoords()) <= radiusSq)
				return true;
		}
	}

	for (const auto pBld : BuildingClass::Array)
	{
		if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner != pOwner && !pBld->Owner->IsNeutral() && !pOwner->IsAlliedWith(pBld->Owner))
		{
			// Only consider structures that actually have weapons (defenses/armed buildings) as threats
			const auto& primary = pBld->Type->GetWeapon(0, false);
			const auto& secondary = pBld->Type->GetWeapon(1, false);

			if (primary.WeaponType != nullptr || secondary.WeaponType != nullptr)
			{
				if (cell.DistanceFromSquared(pBld->GetMapCoords()) <= radiusSq)
					return true;
			}
		}
	}

	return false;
}

int BuildingExt::Exit_Object_Custom_Position(BuildingClass* pBuilding)
{
	PopulateAdjacencyAnchors(pBuilding->Owner, pBuilding->Type);

	const CellStruct placementCell = Get_Best_Placement_Position(pBuilding);

	// If we couldn't find any place for the building, refund it.
	if (placementCell.X <= 0 || placementCell.Y <= 0)
	{
		const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
		houseExt->PlacementFailedCooldowns[pBuilding->Type] = Unsorted::CurrentFrame + 450; // 30 second cooldown at 15 FPS
		Debug::Log("AdvAI: AI %d failed to place building %s. Putting on placement cooldown for 30s.\n", pBuilding->Owner->ArrayIndex, pBuilding->Type->ID);
		return 0;
	}

	const int result = Try_Place(pBuilding, placementCell);

	if (result == 2)
	{
		if (HasEnemyThreatsNear(placementCell, pBuilding->Owner, 7.0))
		{
			pBuilding->Owner->LATime = Unsorted::CurrentFrame;
			const auto houseExt = HouseExt::ExtMap.Find(pBuilding->Owner);
			houseExt->LastAttackedBuildingCoords = placementCell;
			Debug::Log("AdvAI: Placed %s at (%d,%d) near enemy threats! Triggering instant paranoia alert.\n", pBuilding->Type->ID, placementCell.X, placementCell.Y);
		}
	}

	return result;
}
