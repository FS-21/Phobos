// NeuralAI.MobileFactory.inl: Factory deployers, vehicles that unfold into a war factory, barracks or
// airfield. A mobile factory is one whose building packs back into the same vehicle (UndeploysInto):
// only those move again. Told apart from the rules only, never by name.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	constexpr int MobileFactoryReasonNone = 0;
	// Produce where the fight is: behind our towers at the outpost or crawl stone nearest the enemy.
	constexpr int MobileFactoryReasonForward = 1;
	// An island hub of ours, reached by ferry, without that factory kind.
	constexpr int MobileFactoryReasonIsland = 2;
	// The main base lost its factory of that kind: stand in for it until it is rebuilt.
	constexpr int MobileFactoryReasonEmergency = 3;
	// A fresh expansion ConYard still building up: produce its defenders next to it.
	constexpr int MobileFactoryReasonEscort = 4;
	// The main base is overrun or a known enemy superweapon is ready: produce from a quiet flank.
	constexpr int MobileFactoryReasonHidden = 5;

	// A place left under fire is avoided for this long, within this many cells.
	constexpr int MobileFactoryAvoidFrames = 4500;
	constexpr double MobileFactoryAvoidRadius = 14.0;
	// A deployed mobile factory stands at least this long before a planned move, and the house
	// waits this long after any pack-up before the next planned one (fleeing ignores both).
	constexpr int MobileFactoryDwellFrames = 2700;
	constexpr int MobileFactoryPackCooldownFrames = 5400;
	// The front must have moved this far past it before it leapfrogs forward.
	constexpr double MobileFactoryLeapfrogCells = 30.0;

	struct MobileFactorySpot
	{
		CellStruct Cell { CellStruct::Empty };
		int Reason { MobileFactoryReasonNone };
	};

	bool IsLandFactoryBuilding(const BuildingTypeClass* pType)
	{
		return pType && !pType->ConstructionYard && !pType->Naval
			&& (pType->Factory == AbstractType::UnitType || pType->Factory == AbstractType::InfantryType
				|| pType->Factory == AbstractType::AircraftType);
	}

	// A land factory building that packs back into the very vehicle it unfolds from.
	bool IsMobileFactoryBuilding(const BuildingTypeClass* pType)
	{
		return IsLandFactoryBuilding(pType) && pType->UndeploysInto && pType->UndeploysInto->DeploysInto == pType;
	}

	// Any vehicle that unfolds into a land factory, one-way or not.
	bool IsFactoryDeployerUnit(const UnitTypeClass* pType)
	{
		return pType && !pType->Naval && IsLandFactoryBuilding(pType->DeploysInto);
	}

	bool IsMobileFactoryUnit(const UnitTypeClass* pType)
	{
		return IsFactoryDeployerUnit(pType) && IsMobileFactoryBuilding(pType->DeploysInto);
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

	// Combat value of the armed enemy units in our sight right now within radius (fog-fair: a unit
	// standing in fog over ground explored earlier is not seen).
	int VisibleEnemyThreatNear(HouseClass* pHouse, const CellStruct& cell, int radius)
	{
		int threat = 0;

		ForEachTechnoNear(cell, radius, [&](TechnoClass* pTech) -> bool
		{
			if (!pTech->Owner || pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral()
				|| pTech->WhatAmI() == AbstractType::Building || !IsCellInSightOfAI(pHouse, pTech->GetMapCoords()))
			{
				return true;
			}

			auto const pType = pTech->GetTechnoType();

			if (pType && GroundRangeOf(pType) > 0)
				threat += GetCombatValueOfType(pType, false);

			return true;
		});

		return threat;
	}

	// Combat value of our own armed defenses and units within radius.
	int OwnDefenseNear(HouseClass* pHouse, const CellStruct& cell, int radius)
	{
		int defense = 0;

		ForEachTechnoNear(cell, radius, [&](TechnoClass* pTech) -> bool
		{
			if (pTech->Owner != pHouse)
				return true;

			auto const pType = pTech->GetTechnoType();

			if (pType && GroundRangeOf(pType) > 0)
				defense += GetCombatValueOfType(pType, false);

			return true;
		});

		return defense;
	}

	// The building of a known enemy whose offensive superweapon (nuke, storm, dominator) is ready or
	// about to be. Superweapon timers are shown to every player, the building must have been seen.
	const BuildingClass* FindKnownReadyEnemySuperweapon(HouseClass* pHouse)
	{
		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			auto const pBld = info.Building;
			auto const pOwner = pBld->Owner;

			for (const int swIndex : { pBld->Type->SuperWeapon, pBld->Type->SuperWeapon2 })
			{
				if (swIndex < 0 || swIndex >= pOwner->Supers.Count)
					continue;

				auto const pSuper = pOwner->Supers.GetItem(swIndex);

				if (!pSuper || !pSuper->Type || !pSuper->IsPresent || !pSuper->Type->ShowTimer)
					continue;

				auto const kind = pSuper->Type->Type;

				if (kind != SuperWeaponType::Nuke && kind != SuperWeaponType::LightningStorm
					&& kind != SuperWeaponType::PsychicDominator)
				{
					continue;
				}

				if (pSuper->IsReady || pSuper->RechargeTimer.GetTimeLeft() <= 900)
					return pBld;
			}
		}

		return nullptr;
	}

	// Our main base: the ConYard nearest our start cell (kept while it stands, so the main base does
	// not swap between yards as the base center moves), else the base center.
	CellStruct GetMobileFactoryHome(HouseClass* pHouse, int& zone)
	{
		auto& macro = GetMacroState(pHouse);
		zone = -1;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->UniqueID == macro.MobileFactoryHomeID && pBld->IsAlive && !pBld->InLimbo && pBld->Type
				&& pBld->Type->ConstructionYard)
			{
				zone = ObjectZone(pBld, MovementZone::Normal);
				return pBld->GetMapCoords();
			}
		}

		const CellStruct start = pHouse->BaseSpawnCell != CellStruct::Empty ? pHouse->BaseSpawnCell : pHouse->BaseCenter;
		CellStruct home = start;
		double bestDist = 1e9;
		macro.MobileFactoryHomeID = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->ConstructionYard)
				continue;

			const double d = start != CellStruct::Empty ? pBld->GetMapCoords().DistanceFrom(start) : 0.0;

			if (d < bestDist)
			{
				bestDist = d;
				home = pBld->GetMapCoords();
				zone = ObjectZone(pBld, MovementZone::Normal);
				macro.MobileFactoryHomeID = pBld->UniqueID;
			}
		}

		if (zone < 0 && home != CellStruct::Empty)
			zone = MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false);

		return home;
	}

	// Where should a factory deployer of this type stand, seen from `from` (on movement zone
	// fromZone)? pSelf is the deployed factory asking (it does not cover its own spot), or nullptr;
	// selfID is the asking unit or building (spots chosen by other deployers of ours are left to
	// them, the lower UniqueID keeping a shared one). The hub at preferCell (the current goal) is
	// favoured, so a slow vehicle does not swap targets as its distances change on the way.
	// Never a spot inside known enemy defense reach or one we were just driven out of.
	MobileFactorySpot FindMobileFactorySpot(HouseClass* pHouse, const BuildingTypeClass* pFactoryType,
		const BuildingClass* pSelf, const CellStruct& from, int fromZone, DWORD selfID,
		const CellStruct& preferCell = CellStruct::Empty)
	{
		MobileFactorySpot spot {};

		if (!pHouse || !pFactoryType || from == CellStruct::Empty)
			return spot;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const bool canCross = pFactoryType->UndeploysInto && CanHouseFerryTypeAcrossWater(pHouse, pFactoryType->UndeploysInto);
		const bool avoidActive = macro.MobileFactoryAvoidCell != CellStruct::Empty
			&& now - macro.MobileFactoryAvoidFrame < MobileFactoryAvoidFrames;
		int homeZone = -1;
		const CellStruct home = GetMobileFactoryHome(pHouse, homeZone);

		struct OwnSite
		{
			CellStruct Cell;
			int Zone;
			bool IsYard;
		};

		std::vector<OwnSite> sameKind;
		std::vector<OwnSite> hubs;
		std::vector<CellStruct> defenses;
		std::vector<CellStruct> structures;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld == pSelf || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
				continue;

			auto const pType = pBld->Type;
			const CellStruct bc = pBld->GetMapCoords();

			if (pType->Factory == pFactoryType->Factory && !pType->Naval)
				sameKind.push_back({ bc, ObjectZone(pBld, MovementZone::Normal), false });

			if (pType->ConstructionYard || pType->Refinery || pType->ResourceDestination)
				hubs.push_back({ bc, ObjectZone(pBld, MovementZone::Normal), pType->ConstructionYard });

			if (pType->IsBaseDefense && !pType->Wall)
				defenses.push_back(bc);

			if (!pType->Wall && !pType->Insignificant)
				structures.push_back(bc);
		}

		std::vector<CellStruct> stones;

		for (const DWORD id : macro.OffensiveStones)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->UniqueID == id)
				{
					stones.push_back(pBld->GetMapCoords());
					break;
				}
			}
		}

		std::vector<CellStruct> reserved;

		for (auto const& [id, goal] : macro.MobileFactoryGoals)
		{
			if (id < selfID && goal.Cell != CellStruct::Empty && now - goal.PickFrame <= 450)
				reserved.push_back(goal.Cell);
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

		auto isFree = [&](const CellStruct& cell)
		{
			if (avoidActive && cell.DistanceFrom(macro.MobileFactoryAvoidCell) <= MobileFactoryAvoidRadius)
				return false;

			for (auto const& r : reserved)
			{
				if (r.DistanceFrom(cell) <= 6.0)
					return false;
			}

			return !IsCellInKnownEnemyDefenseReach(pHouse, cell, 4);
		};

		auto closestHub = [&](auto&& accept) -> const OwnSite*
		{
			const OwnSite* pBest = nullptr;
			double bestCost = 1e9;

			for (auto const& hub : hubs)
			{
				if (!reachable(hub.Zone) || !isFree(hub.Cell) || !accept(hub))
					continue;

				const bool isPreferred = preferCell != CellStruct::Empty && hub.Cell.DistanceFrom(preferCell) <= 3.0;
				const double cost = hub.Cell.DistanceFrom(from) + (hub.IsYard ? 0.0 : 20.0) - (isPreferred ? 30.0 : 0.0);

				if (cost < bestCost)
				{
					bestCost = cost;
					pBest = &hub;
				}
			}

			return pBest;
		};

		const bool nearHome = home != CellStruct::Empty && from.DistanceFrom(home) <= 30.0;

		// Hidden production: the main base is losing to what we see there, or a known enemy
		// superweapon is ready. Of eight cells 22 cells around the base, the free one farthest from
		// the threat (and from our other factories of that kind, for a superweapon).
		if (nearHome)
		{
			CellStruct threatPoint = CellStruct::Empty;
			bool spreadOut = false;
			const int baseThreat = VisibleEnemyThreatNear(pHouse, home, 20);

			if (baseThreat >= 200 && baseThreat > OwnDefenseNear(pHouse, home, 20))
			{
				int sumX = 0;
				int sumY = 0;
				int count = 0;

				ForEachTechnoNear(home, 20, [&](TechnoClass* pTech) -> bool
				{
					if (pTech->Owner && pTech->Owner != pHouse && !pHouse->IsAlliedWith(pTech->Owner) && !pTech->Owner->IsNeutral()
						&& pTech->WhatAmI() != AbstractType::Building && IsCellVisibleToAI(pHouse, pTech->GetCell()))
					{
						const CellStruct c = pTech->GetMapCoords();
						sumX += c.X;
						sumY += c.Y;
						count++;
					}

					return true;
				});

				if (count > 0)
					threatPoint = CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) };
			}
			else if (auto const pSuperBld = FindKnownReadyEnemySuperweapon(pHouse))
			{
				threatPoint = pSuperBld->GetMapCoords();
				spreadOut = true;
			}

			if (threatPoint != CellStruct::Empty)
			{
				constexpr int dirs[8][2] = { { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 } };
				double bestScore = -1e9;

				for (auto const& dir : dirs)
				{
					const int step = (dir[0] != 0 && dir[1] != 0) ? 16 : 22;
					const CellStruct c { static_cast<short>(home.X + dir[0] * step), static_cast<short>(home.Y + dir[1] * step) };

					if (!MapClass::Instance.CoordinatesLegal(c) || !isFree(c) || VisibleEnemyThreatNear(pHouse, c, 10) > 0)
						continue;

					if (homeZone >= 0 && MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != homeZone)
						continue;

					double score = c.DistanceFrom(threatPoint);

					if (spreadOut)
					{
						double nearestFactory = 30.0;

						for (auto const& site : sameKind)
							nearestFactory = std::min(nearestFactory, site.Cell.DistanceFrom(c));

						score += nearestFactory;
					}

					if (score > bestScore)
					{
						bestScore = score;
						spot.Cell = c;
						spot.Reason = MobileFactoryReasonHidden;
					}
				}

				if (spot.Reason != MobileFactoryReasonNone)
					return spot;
			}
		}

		// Emergency replacement: the main base had a fixed factory of that kind and lost it. A packed
		// vehicle heads there; a deployed one standing in there stays until the fixed one is rebuilt.
		const size_t kindIndex = pFactoryType->Factory == AbstractType::UnitType ? 0u
			: (pFactoryType->Factory == AbstractType::InfantryType ? 1u : 2u);
		bool mainHasFixed = false;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Factory == pFactoryType->Factory
				&& !pBld->Type->Naval && !IsMobileFactoryBuilding(pBld->Type) && pBld->GetMapCoords().DistanceFrom(home) <= 30.0)
			{
				mainHasFixed = true;
				break;
			}
		}

		if (mainHasFixed)
			macro.MobileFactoryMainHad[kindIndex] = true;

		const bool mainLostFactory = home != CellStruct::Empty && macro.MobileFactoryMainHad[kindIndex] && !mainHasFixed;

		if (mainLostFactory && pSelf && from.DistanceFrom(home) <= 30.0)
			return spot;

		if (mainLostFactory && !pSelf && reachable(homeZone) && isFree(home) && !coveredBySameKind(home, homeZone, 30.0))
		{
			spot.Cell = home;
			spot.Reason = MobileFactoryReasonEmergency;
			return spot;
		}

		// Expansion escort: a ConYard away from the main base with no factory of that kind near it
		// and fewer than six structures around it yet.
		auto const pEscort = closestHub([&](const OwnSite& hub)
		{
			if (!hub.IsYard || hub.Cell.DistanceFrom(home) <= 24.0 || coveredBySameKind(hub.Cell, hub.Zone, 24.0))
				return false;

			int around = 0;

			for (auto const& s : structures)
			{
				if (s.DistanceFrom(hub.Cell) <= 14.0)
					around++;
			}

			return around < 6;
		});

		if (pEscort)
		{
			spot.Cell = pEscort->Cell;
			spot.Reason = MobileFactoryReasonEscort;
			return spot;
		}

		// Island hop: a hub on another landmass than the main base, ferried there.
		auto const pIsland = closestHub([&](const OwnSite& hub)
		{
			return homeZone >= 0 && hub.Zone >= 0 && hub.Zone != homeZone && !coveredBySameKind(hub.Cell, hub.Zone, 24.0);
		});

		if (pIsland)
		{
			spot.Cell = pIsland->Cell;
			spot.Reason = MobileFactoryReasonIsland;
			return spot;
		}

		// Forward production: the hub or crawl stone nearest a known enemy structure on its own
		// landmass, when it is 30+ cells nearer the enemy than our closest factory of that kind.
		struct EnemySite
		{
			CellStruct Cell;
			int Zone;
		};

		std::vector<EnemySite> enemies;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			if (!info.Building->Type->Insignificant && !info.Building->Type->InvisibleInGame)
				enemies.push_back({ info.Coords, ObjectZone(info.Building, MovementZone::Normal) });
		}

		auto enemyDistance = [&](const CellStruct& cell, int zone)
		{
			double best = 1e9;

			for (auto const& e : enemies)
			{
				if (zone < 0 || e.Zone < 0 || e.Zone == zone)
					best = std::min(best, e.Cell.DistanceFrom(cell));
			}

			return best;
		};

		CellStruct anchor = CellStruct::Empty;
		int anchorZone = -1;
		double anchorEnemyDist = 1e9;

		auto considerAnchor = [&](const CellStruct& cell, int zone)
		{
			if (!reachable(zone))
				return;

			const double d = enemyDistance(cell, zone);

			if (d < anchorEnemyDist)
			{
				anchorEnemyDist = d;
				anchor = cell;
				anchorZone = zone;
			}
		};

		for (auto const& hub : hubs)
			considerAnchor(hub.Cell, hub.Zone);

		for (auto const& stone : stones)
			considerAnchor(stone, MapClass::Instance.GetMovementZoneType(stone, MovementZone::Normal, false));

		if (anchor == CellStruct::Empty || anchorEnemyDist >= 1e8)
			return spot;

		double factoryEnemyDist = 1e9;
		CellStruct backAnchor = home != CellStruct::Empty ? home : from;
		double backDist = 1e9;

		for (auto const& site : sameKind)
		{
			if (anchorZone >= 0 && site.Zone >= 0 && site.Zone != anchorZone)
				continue;

			factoryEnemyDist = std::min(factoryEnemyDist, enemyDistance(site.Cell, site.Zone));

			const double d = site.Cell.DistanceFrom(anchor);

			if (d < backDist)
			{
				backDist = d;
				backAnchor = site.Cell;
			}
		}

		if (anchorEnemyDist + 30.0 > factoryEnemyDist)
			return spot;

		// Behind our towers: from the centroid of our defenses near the anchor (or the anchor),
		// step back toward our factories until clear of known enemy defense reach.
		int sumX = 0;
		int sumY = 0;
		int count = 0;

		for (auto const& def : defenses)
		{
			if (def.DistanceFrom(anchor) <= 14.0)
			{
				sumX += def.X;
				sumY += def.Y;
				count++;
			}
		}

		const CellStruct front = count > 0
			? CellStruct { static_cast<short>(sumX / count), static_cast<short>(sumY / count) }
			: anchor;
		const int dist = static_cast<int>(front.DistanceFrom(backAnchor));

		if (dist <= 0)
			return spot;

		for (int back = 4; back <= std::min(24, dist); back += 2)
		{
			const CellStruct c { static_cast<short>(front.X + ((backAnchor.X - front.X) * back) / dist),
				static_cast<short>(front.Y + ((backAnchor.Y - front.Y) * back) / dist) };

			if (!MapClass::Instance.CoordinatesLegal(c))
				continue;

			if (anchorZone >= 0 && MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != anchorZone)
				continue;

			if (isFree(c))
			{
				spot.Cell = c;
				spot.Reason = MobileFactoryReasonForward;
				break;
			}
		}

		return spot;
	}

	// Where a fleeing mobile factory goes: the main base when no enemy is seen there, else the
	// nearest reachable hub of ours with no enemy seen near it and out of known defense reach.
	CellStruct FindMobileFactoryRetreat(HouseClass* pHouse, const CellStruct& from, int fromZone, bool canCross)
	{
		int homeZone = -1;
		const CellStruct home = GetMobileFactoryHome(pHouse, homeZone);

		auto isQuiet = [&](const CellStruct& cell)
		{
			return VisibleEnemyThreatNear(pHouse, cell, 12) == 0 && !IsCellInKnownEnemyDefenseReach(pHouse, cell, 4);
		};

		if (home != CellStruct::Empty && home.DistanceFrom(from) > 12.0 && (homeZone == fromZone || canCross) && isQuiet(home))
			return home;

		CellStruct best = CellStruct::Empty;
		double bestDist = 1e9;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type
				|| (!pBld->Type->ConstructionYard && !pBld->Type->Refinery && !pBld->Type->ResourceDestination))
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			const double d = bc.DistanceFrom(from);

			if (d <= 12.0 || d >= bestDist || (ObjectZone(pBld, MovementZone::Normal) != fromZone && !canCross) || !isQuiet(bc))
				continue;

			bestDist = d;
			best = bc;
		}

		return best != CellStruct::Empty ? best : home;
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

		// A vehicle lost before it could unfold (seen packed after the last deploy, gone now): wait
		// before risking another one on the same road.
		if (wanted && macro.MobileFactoryVehicleSeenFrame > macro.MobileFactoryDeployFrame
			&& now - macro.MobileFactoryVehicleSeenFrame < 9000)
		{
			wanted = false;
		}

		// Asked as no unit at all (the highest ID): every spot already chosen by a deployer is taken.
		if (wanted)
		{
			wanted = FindMobileFactorySpot(pHouse, pUnitType->DeploysInto, nullptr, from, fromZone, 0xFFFFFFFFu).Reason
				!= MobileFactoryReasonNone;
		}

		macro.MobileFactoryWanted[pUnitType->ArrayIndex] = { now, wanted };

		return wanted;
	}

} // namespace
