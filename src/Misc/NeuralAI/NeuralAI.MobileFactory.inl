// NeuralAI.MobileFactory.inl: Mobile factories, vehicles that unfold into a war factory, barracks or
// airfield and can pack up again. Told apart from the rules only (DeploysInto / UndeploysInto and
// Factory), never by name.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	constexpr int MobileFactoryReasonNone = 0;
	// One of our hubs (ConYard or refinery) has no factory of that kind near it: an outpost, an
	// island hub, or a base that lost its factory.
	constexpr int MobileFactoryReasonOutpost = 1;
	// The front (newest offensive stone) is far from every factory of that kind.
	constexpr int MobileFactoryReasonFront = 2;

	// A place left under fire is avoided for this long, within this many cells.
	constexpr int MobileFactoryAvoidFrames = 4500;
	constexpr double MobileFactoryAvoidRadius = 14.0;

	struct MobileFactorySpot
	{
		CellStruct Cell { CellStruct::Empty };
		int Reason { MobileFactoryReasonNone };
	};

	// A land factory building that packs back into the very vehicle it unfolds from.
	bool IsMobileFactoryBuilding(const BuildingTypeClass* pType)
	{
		if (!pType || !pType->UndeploysInto || pType->ConstructionYard || pType->Naval)
			return false;

		if (pType->Factory != AbstractType::UnitType && pType->Factory != AbstractType::InfantryType
			&& pType->Factory != AbstractType::AircraftType)
		{
			return false;
		}

		return pType->UndeploysInto->DeploysInto == pType;
	}

	bool IsMobileFactoryUnit(const UnitTypeClass* pType)
	{
		return pType && !pType->Naval && IsMobileFactoryBuilding(pType->DeploysInto);
	}

	// Is this cell within the ground reach (+ margin cells) of a known enemy defense? Fog-fair:
	// only enemy structures this house has seen.
	bool IsCellInKnownEnemyDefenseReach(HouseClass* pHouse, const CellStruct& cell, int marginCells)
	{
		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (!info.IsDefense)
				continue;

			const int range = GroundRangeOf(info.Building->Type);

			if (range > 0 && info.Coords.DistanceFrom(cell) * Unsorted::LeptonsPerCell
				<= range + marginCells * Unsorted::LeptonsPerCell)
			{
				return true;
			}
		}

		return false;
	}

	// Where should a mobile factory of this type stand, seen from `from` (on movement zone
	// fromZone)? pSelf is the deployed factory asking (it does not cover its own spot), or nullptr.
	// An outpost lacking that kind of factory wins over the front: there it is the only producer,
	// at the front it only saves the drive. Never a spot inside known enemy defense reach or one
	// we were just driven out of. The hub at preferCell (the current goal) is favoured, so a slow
	// vehicle does not swap between two outposts as its distance to each changes on the way.
	MobileFactorySpot FindMobileFactorySpot(HouseClass* pHouse, const BuildingTypeClass* pFactoryType,
		const BuildingClass* pSelf, const CellStruct& from, int fromZone, const CellStruct& preferCell = CellStruct::Empty)
	{
		MobileFactorySpot spot {};

		if (!pHouse || !pFactoryType || from == CellStruct::Empty)
			return spot;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const bool canCross = CanHouseFerryTypeAcrossWater(pHouse, pFactoryType->UndeploysInto);
		const bool avoidActive = macro.MobileFactoryAvoidCell != CellStruct::Empty
			&& now - macro.MobileFactoryAvoidFrame < MobileFactoryAvoidFrames;

		struct OwnSite
		{
			CellStruct Cell;
			int Zone;
			bool IsYard;
		};

		std::vector<OwnSite> sameKind;
		std::vector<OwnSite> hubs;
		std::vector<CellStruct> defenses;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld == pSelf || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
				continue;

			auto const pType = pBld->Type;
			const CellStruct bc = pBld->GetMapCoords();

			if (pType->Factory == pFactoryType->Factory && !pType->Naval)
				sameKind.push_back({ bc, MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge), false });

			if (pType->ConstructionYard || pType->Refinery || pType->ResourceDestination)
			{
				hubs.push_back({ bc, MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge),
					pType->ConstructionYard });
			}

			if (pType->IsBaseDefense && !pType->Wall)
				defenses.push_back(bc);
		}

		auto reachable = [&](int zone)
		{
			return zone < 0 || fromZone < 0 || zone == fromZone || canCross;
		};

		auto coveredBySameKind = [&](const CellStruct& cell, int zone, double radius)
		{
			for (auto const& site : sameKind)
			{
				if ((zone < 0 || site.Zone < 0 || site.Zone == zone) && site.Cell.DistanceFrom(cell) <= radius)
					return true;
			}

			return false;
		};

		auto isSafe = [&](const CellStruct& cell)
		{
			if (avoidActive && cell.DistanceFrom(macro.MobileFactoryAvoidCell) <= MobileFactoryAvoidRadius)
				return false;

			return !IsCellInKnownEnemyDefenseReach(pHouse, cell, 4);
		};

		// Outpost: the closest hub with no factory of that kind within 24 cells. A ConYard is worth
		// 20 cells of extra drive over a refinery (it can rebuild and defend around the factory).
		double bestOutpost = 1e9;

		for (auto const& hub : hubs)
		{
			if (!reachable(hub.Zone) || coveredBySameKind(hub.Cell, hub.Zone, 24.0) || !isSafe(hub.Cell))
				continue;

			const bool isPreferred = preferCell != CellStruct::Empty && hub.Cell.DistanceFrom(preferCell) <= 3.0;
			const double cost = hub.Cell.DistanceFrom(from) + (hub.IsYard ? 0.0 : 20.0) - (isPreferred ? 30.0 : 0.0);

			if (cost < bestOutpost)
			{
				bestOutpost = cost;
				spot.Cell = hub.Cell;
				spot.Reason = MobileFactoryReasonOutpost;
			}
		}

		if (spot.Reason != MobileFactoryReasonNone)
			return spot;

		// Front: the newest offensive stone, 40+ cells away from every factory of that kind. The
		// factory stands behind it: behind our defenses there when we have some, stepping back
		// toward the factories (or `from`) until it is out of known enemy defense reach.
		const CellStruct stone = GetNewestOffensiveStoneCell(pHouse);

		if (stone == CellStruct::Empty)
			return spot;

		const int frontZone = MapClass::Instance.GetMovementZoneType(stone, MovementZone::Normal, false);

		if (!reachable(frontZone) || coveredBySameKind(stone, frontZone, 40.0))
			return spot;

		int sumX = 0;
		int sumY = 0;
		int count = 0;

		for (auto const& def : defenses)
		{
			if (def.DistanceFrom(stone) <= 14.0)
			{
				sumX += def.X;
				sumY += def.Y;
				count++;
			}
		}

		const CellStruct front = count > 0
			? CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) }
			: stone;

		CellStruct anchor = from;
		double anchorDist = 1e9;

		for (auto const& site : sameKind)
		{
			const double d = site.Cell.DistanceFrom(front);

			if ((frontZone < 0 || site.Zone == frontZone) && d < anchorDist)
			{
				anchorDist = d;
				anchor = site.Cell;
			}
		}

		const int dist = static_cast<int>(front.DistanceFrom(anchor));

		if (dist <= 0)
			return spot;

		for (int back = 6; back <= std::min(24, dist); back += 3)
		{
			const CellStruct c { static_cast<short>(front.X + ((anchor.X - front.X) * back) / dist),
				static_cast<short>(front.Y + ((anchor.Y - front.Y) * back) / dist) };

			if (!MapClass::Instance.CoordinatesLegal(c))
				continue;

			if (frontZone >= 0 && MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != frontZone)
				continue;

			if (isSafe(c))
			{
				spot.Cell = c;
				spot.Reason = MobileFactoryReasonFront;
				break;
			}
		}

		return spot;
	}

	// Should this mobile factory vehicle be built now? One per factory kind at a time (packed or
	// deployed), and only when it has a spot worth the trip; otherwise it would just unfold next
	// to the factory that made it. Answers are kept 150 frames per house and type.
	bool HouseWantsMobileFactory(HouseClass* pHouse, const UnitTypeClass* pUnitType)
	{
		if (!pHouse || !IsMobileFactoryUnit(pUnitType))
			return false;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		auto const cached = macro.MobileFactoryWanted.find(pUnitType->ArrayIndex);

		if (cached != macro.MobileFactoryWanted.end() && now >= cached->second.first && now - cached->second.first < 150)
			return cached->second.second;

		auto const kind = pUnitType->DeploysInto->Factory;
		bool wanted = true;

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && IsMobileFactoryUnit(pUnit->Type)
				&& pUnit->Type->DeploysInto->Factory == kind)
			{
				wanted = false;
				break;
			}
		}

		CellStruct from = pHouse->BaseCenter != CellStruct::Empty ? pHouse->BaseCenter : pHouse->BaseSpawnCell;
		int fromZone = MapClass::Instance.GetMovementZoneType(from, MovementZone::Normal, false);

		for (auto const pBld : pHouse->Buildings)
		{
			if (!wanted || !pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
				continue;

			if (IsMobileFactoryBuilding(pBld->Type) && pBld->Type->Factory == kind)
			{
				wanted = false;
			}
			else if (pBld->Type->Factory == AbstractType::UnitType && !pBld->Type->Naval && pBld->IsPrimaryFactory)
			{
				from = pBld->GetMapCoords();
				fromZone = ObjectZone(pBld, MovementZone::Normal);
			}
		}

		if (wanted)
			wanted = FindMobileFactorySpot(pHouse, pUnitType->DeploysInto, nullptr, from, fromZone).Reason != MobileFactoryReasonNone;

		macro.MobileFactoryWanted[pUnitType->ArrayIndex] = { now, wanted };

		return wanted;
	}

} // namespace
