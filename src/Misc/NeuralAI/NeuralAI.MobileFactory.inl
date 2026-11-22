// NeuralAI.MobileFactory.inl: Base deployers, vehicles that unfold into a base building other than a
// yard (Nanocores, mobile factories, outposts), and the one placement they share with the yard: the
// expert decides where it wants a building by its role and the building then comes from the yard or
// from a deployer sent to that spot, which needs no adjacency. A mobile factory, and any reversible
// building, is one whose building packs back into the same vehicle (UndeploysInto): only those move
// again. Told apart from the rules only, never by name.
// Part of NeuralAI.cpp (included in order; one translation unit).

// NeuralAI::IsCellClearedOfExitCorridors outside the class (NeuralAI.Placement.inl).
static bool CellClearedOfExitCorridors(HouseClass* pHouse, BuildingTypeClass* pType, CellStruct candidateCell);

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

	bool IsSensorLookoutType(const UnitTypeClass* pType);

	// A vehicle that unfolds into a base building other than a yard. Only DeploysInto gives a building
	// (Convert.Deploy and Universal Deploy turn a unit into another unit). Not a slave miner (its
	// harvest unfolds it), an MCV (its own expansion logic), a mobile sensor array (the lookouts of the
	// scouting move and unfold it: as a base deployer it unfolded back at its base site each time they
	// packed it up for a lookout, 20 times in 13 matches of stamp 06a1e8c0e) nor a unit that unfolds to
	// fight (a siege gun, an armed vehicle that becomes a defense).
	bool IsBaseDeployerUnit(const UnitTypeClass* pType)
	{
		if (!pType || !pType->DeploysInto || pType->DeploysInto->ConstructionYard || pType->Harvester || pType->ResourceGatherer
			|| IsSensorLookoutType(pType))
		{
			return false;
		}

		auto const pBld = pType->DeploysInto;
		auto const pWeaponSlot = const_cast<UnitTypeClass*>(pType)->GetWeapon(0);
		const bool armed = pWeaponSlot && pWeaponSlot->WeaponType;

		return IsTrueBaseStructureTarget(pBld) && (!pBld->IsBaseDefense || !armed)
			&& !IsDeployableSiegeUnitType(const_cast<UnitTypeClass*>(pType));
	}

	// The role of a deployed building, from the same flags the building pick reads.
	enum class DeployerRole : int
	{
		Support,
		Refinery,
		Factory,
		Defense,
		Power,
		Tech
	};

	DeployerRole DeployedBuildingRole(const BuildingTypeClass* pBld)
	{
		if (pBld->Refinery || pBld->ResourceDestination || pBld->ResourceGatherer)
			return DeployerRole::Refinery;

		if (pBld->Factory != AbstractType::None)
			return DeployerRole::Factory;

		if (pBld->IsBaseDefense)
			return DeployerRole::Defense;

		if (pBld->PowerBonus > pBld->PowerDrain)
			return DeployerRole::Power;

		auto const pBldExt = BuildingTypeExt::Fetch(const_cast<BuildingTypeClass*>(pBld));

		if (pBld->Radar || pBld->SpySat || pBld->SuperWeapon != -1 || pBld->SuperWeapon2 != -1 || (pBldExt && !pBldExt->SuperWeapons.empty()))
			return DeployerRole::Tech;

		return DeployerRole::Support;
	}

	// A base building that packs back into the very vehicle it unfolds from (a mobile factory, the Iron
	// Fist, a yard): it may flee and move again. Siege guns and slave miners have rules of their own.
	bool IsReversibleBaseBuilding(const BuildingTypeClass* pBld)
	{
		return pBld && pBld->UndeploysInto && pBld->UndeploysInto->DeploysInto == pBld && !pBld->ResourceGatherer
			&& pBld->Enslaves == nullptr && !IsDeployedSiegeBuildingType(const_cast<BuildingTypeClass*>(pBld))
			&& IsTrueBaseStructureTarget(pBld);
	}

	// The legal deploy stand nearest originCell, within 28 rings, on requiredZone (-1: any): the building
	// can stand there (CanCreateHere and every foundation cell), the deployer can stand on the cell, our
	// exit corridors stay open and no other deployer or transport of ours is on the foundation. pAvoid:
	// sites tried before; a candidate within 6 cells of one is passed over.
	CellStruct FindDeployFoundationNear(HouseClass* pHouse, BuildingTypeClass* pBldType, const CellStruct& originCell, int requiredZone,
		UnitClass* pDeployerUnit, const std::vector<CellStruct>* pAvoid = nullptr)
	{
		if (!pHouse || !pBldType || originCell == CellStruct::Empty)
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

					const CellStruct cand { static_cast<short>(originCell.X + dx), static_cast<short>(originCell.Y + dy) };
					const CellStruct topLeftCell = cand + deployOffset;

					if (!MapClass::Instance.CoordinatesLegal(cand) || !MapClass::Instance.CoordinatesLegal(topLeftCell))
						continue;

					if (requiredZone >= 0 && MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false) != requiredZone)
						continue;

					if (pAvoid && std::any_of(pAvoid->begin(), pAvoid->end(), [&cand](const CellStruct& avoid)
						{ return std::max(std::abs(avoid.X - cand.X), std::abs(avoid.Y - cand.Y)) <= 6; }))
					{
						continue;
					}

					if (pDeployerUnit && pDeployerUnit->Type && cand != curUnitCell)
					{
						auto const pStandCell = MapClass::Instance.TryGetCellAt(cand);

						if (!pStandCell
							|| !pStandCell->IsClearToMove(pDeployerUnit->Type->SpeedType, false, false, -1, pDeployerUnit->Type->MovementZone, -1, false))
						{
							continue;
						}
					}

					if (!CellClearedOfExitCorridors(pHouse, pBldType, topLeftCell))
						continue;

					UnitExt::Deployer = pDeployerUnit;
					bool canExist = pBldType->CanCreateHere(topLeftCell, pHouse);

					if (canExist)
					{
						for (auto pFoundCell = pBldType->GetFoundationData(true); *pFoundCell != CellStruct { 0x7FFF, 0x7FFF }; ++pFoundCell)
						{
							const CellStruct fc = topLeftCell + *pFoundCell;
							auto const pFC = MapClass::Instance.TryGetCellAt(fc);

							if (!pFC || !pFC->CanThisExistHere(pBldType->SpeedType, pBldType, pHouse))
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
										&& (pOtherU->Type->DeploysInto != nullptr || IsAmphibiousOrAirTransport(pOtherU->Type)))
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
	}

	// TEMP diagnostic (deployer orders, trips and pack-ups): DeployerLogLinesMost lines a match.
	bool DeployerLogAllowed(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);

		if (macro.DeployerLogLines >= DeployerLogLinesMost)
			return false;

		++macro.DeployerLogLines;
		return true;
	}

	const char* PlacementGoalKindName(NeuralPlacementGoalKind kind)
	{
		switch (kind)
		{
		case NeuralPlacementGoalKind::OreField:
			return "ore field";
		case NeuralPlacementGoalKind::IslandHold:
			return "island hold";
		case NeuralPlacementGoalKind::AttackedBuilding:
			return "attacked building";
		case NeuralPlacementGoalKind::KeyAsset:
			return "key asset";
		case NeuralPlacementGoalKind::RouteSpot:
			return "route spot";
		case NeuralPlacementGoalKind::DefenseSector:
			return "defense sector";
		case NeuralPlacementGoalKind::Front:
			return "front";
		case NeuralPlacementGoalKind::AirSector:
			return "air sector";
		case NeuralPlacementGoalKind::Hub:
			return "hub";
		default:
			return "base";
		}
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

	// Combat value of the armed enemy units this house knows of within radius (IsCellKnownToAI: in
	// sight now, or on explored ground out of enemy gaps when the match has no fog of war).
	int VisibleEnemyThreatNear(HouseClass* pHouse, const CellStruct& cell, int radius)
	{
		int threat = 0;

		ForEachTechnoNear(cell, radius, [&](TechnoClass* pTech) -> bool
		{
			if (!pTech->Owner || pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral()
				|| pTech->WhatAmI() == AbstractType::Building || !IsCellKnownToAI(pHouse, pTech->GetMapCoords()))
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
			if (auto const pStone = FindOwnStandingBuildingByID(pHouse, id))
				stones.push_back(pStone->GetMapCoords());
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

	// Where would this house put this building, for a deployer of pDeployerType (pUnit when one asks,
	// nullptr when the building pick does)? The factory spot of a land factory (FindMobileFactorySpot),
	// else the role goal of the base placement search (NeuralPlacementGoal, by the expert in charge: the
	// free field of a refinery; the island held, the building attacked, the key asset, the approach, the
	// sector or the front of a defense; the hub missing a factory), else the cell of that search. A
	// refinery with no free field left on our land takes the free field of another landmass
	// (IslandExpansionField) when the deployer can be ferried there. A defense sent to the front stands
	// at most DeployerFrontCells past our forward building, short of the known enemy defenses.
	// outKind: why. Empty when the house has no ground of its own left.
	CellStruct FindDeployerGoal(HouseClass* pHouse, BuildingTypeClass* pBld, UnitTypeClass* pDeployerType, UnitClass* pUnit,
		NeuralPlacementGoalKind& outKind)
	{
		outKind = NeuralPlacementGoalKind::None;

		if (!pHouse || !pBld)
			return CellStruct::Empty;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const DeployerRole role = DeployedBuildingRole(pBld);
		const CellStruct home = GetHouseHomeCell(pHouse);
		const CellStruct from = pUnit ? pUnit->GetMapCoords() : home;
		const int homeZone = home != CellStruct::Empty ? MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false) : -1;
		const int fromZone = pUnit ? ObjectZone(pUnit, MovementZone::Normal) : homeZone;
		CellStruct goal = CellStruct::Empty;
		CellStruct anchor = CellStruct::Empty;
		NeuralPlacementGoalKind kind = NeuralPlacementGoalKind::None;

		if (role == DeployerRole::Factory && IsLandFactoryBuilding(pBld) && from != CellStruct::Empty)
		{
			const MobileFactorySpot spot = FindMobileFactorySpot(pHouse, pBld, nullptr, from, fromZone, pUnit ? pUnit->UniqueID : 0xFFFFFFFFu);

			if (spot.Reason != MobileFactoryReasonNone)
			{
				goal = spot.Cell;
				kind = NeuralPlacementGoalKind::Hub;
			}
		}

		// Run first: the search notes the goal it chose.
		const CellStruct yardCell = CachedTetrisBuildCell(pHouse, pBld);

		if (goal == CellStruct::Empty)
		{
			auto const itGoal = macro.PlacementGoals.find(pBld->ArrayIndex);

			if (itGoal != macro.PlacementGoals.end() && itGoal->second.Kind != NeuralPlacementGoalKind::None
				&& now >= itGoal->second.Frame && now - itGoal->second.Frame <= PlacementGoalFrames)
			{
				goal = itGoal->second.Cell;
				anchor = itGoal->second.Anchor;
				kind = itGoal->second.Kind;
			}
		}

		if (role == DeployerRole::Refinery && kind != NeuralPlacementGoalKind::OreField && macro.IslandExpansionField != CellStruct::Empty
			&& pDeployerType && CanHouseFerryTypeAcrossWater(pHouse, pDeployerType))
		{
			goal = macro.IslandExpansionField;
			anchor = CellStruct::Empty;
			kind = NeuralPlacementGoalKind::OreField;
		}

		if (kind == NeuralPlacementGoalKind::Front && anchor != CellStruct::Empty)
		{
			const int anchorZone = MapClass::Instance.GetMovementZoneType(anchor, MovementZone::Normal, false);
			const double dist = anchor.DistanceFrom(goal);
			CellStruct front = anchor;

			for (int step = 2; step <= DeployerFrontCells && step < dist; step += 2)
			{
				const CellStruct c { static_cast<short>(anchor.X + static_cast<int>((goal.X - anchor.X) * step / dist)),
					static_cast<short>(anchor.Y + static_cast<int>((goal.Y - anchor.Y) * step / dist)) };

				if (!MapClass::Instance.CoordinatesLegal(c) || MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != anchorZone
					|| IsCellInKnownEnemyDefenseReach(pHouse, c, 2))
				{
					break;
				}

				front = c;
			}

			goal = front;
		}

		if (goal == CellStruct::Empty && yardCell != CellStruct::Empty)
		{
			const CellStruct standOffset = (pBld->GetFoundationWidth() > 2 || pBld->GetFoundationHeight(false) > 2)
				? CellStruct { 1, 1 }
				: CellStruct { 0, 0 };
			goal = yardCell + standOffset;
		}

		if (goal == CellStruct::Empty)
			goal = home;

		outKind = goal != CellStruct::Empty ? kind : NeuralPlacementGoalKind::None;

		return goal;
	}

	// The site of a deployer for that goal: the legal deploy stand nearest it on its ground (our home
	// ground for a goal on no ground), no adjacency asked (FindDeployFoundationNear); for a building on
	// water the nearest one the deployer reaches by its own moves.
	CellStruct FindDeployerSiteNear(HouseClass* pHouse, BuildingTypeClass* pBld, UnitClass* pUnit, const CellStruct& goal,
		const std::vector<CellStruct>* pAvoid)
	{
		if (!pHouse || !pBld || goal == CellStruct::Empty)
			return CellStruct::Empty;

		if (IsWaterBuildingType(pBld))
		{
			// A water site only where the deployer gets by itself.
			const int ownMoveZone = pUnit ? ObjectZone(pUnit, pUnit->Type->MovementZone) : -1;
			std::vector<CellStruct> avoid = pAvoid ? *pAvoid : std::vector<CellStruct> {};

			for (int search = 0; search < DeployerWaterSiteTries; ++search)
			{
				const CellStruct cell = FindDeployFoundationNear(pHouse, pBld, goal, -1, pUnit, &avoid);

				if (cell == CellStruct::Empty)
					break;

				if (!pUnit || MapClass::Instance.GetMovementZoneType(cell, pUnit->Type->MovementZone, false) == ownMoveZone)
					return cell;

				avoid.push_back(cell);
			}

			return CellStruct::Empty;
		}

		const int goalZone = MapClass::Instance.GetMovementZoneType(goal, MovementZone::Normal, false);
		const CellStruct home = GetHouseHomeCell(pHouse);
		const int homeZone = home != CellStruct::Empty ? MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false) : -1;

		return FindDeployFoundationNear(pHouse, pBld, goal, IsGroundZone(goalZone) ? goalZone : homeZone, pUnit, pAvoid);
	}

	// What a deployer risks on its way from `from` to `site`: the value of the known armed enemies near
	// the site and halfway there, and DeployerTripRiskDefense when a known enemy defense reaches the site.
	int DeployerTripRisk(HouseClass* pHouse, const CellStruct& from, const CellStruct& site)
	{
		if (!pHouse || from == CellStruct::Empty || site == CellStruct::Empty)
			return 0;

		const CellStruct mid { static_cast<short>((from.X + site.X) / 2), static_cast<short>((from.Y + site.Y) / 2) };
		int risk = VisibleEnemyThreatNear(pHouse, site, 10) + VisibleEnemyThreatNear(pHouse, mid, 10);

		if (IsCellInKnownEnemyDefenseReach(pHouse, site, 2))
			risk += DeployerTripRiskDefense;

		return risk;
	}

	// The way of a trip with enemies on it, weighed (user, 2026-11-21: the escort is the AI's choice):
	// behind the next wave against alone, by the trip lesson of the house (DeployerTripWeight), the risk
	// and the trained escort slot (scores[60]). Never a fixed rule.
	bool DeployerTripFollowsWave(HouseClass* pHouse, int risk, int escortSlot)
	{
		if (!pHouse || risk <= 0)
			return false;

		auto const& weight = GetMacroState(pHouse).DeployerTripWeight;

		return weight[1] + risk / 4 + escortSlot / 4 > weight[0] + DeployerEscortMargin;
	}

	// A weighed trip ended: the way it took gains when the building stands, loses when the deployer
	// was lost on the way (the rule of the plan weights).
	void NoteDeployerTripEnd(HouseClass* pHouse, bool behindWave, bool deployed)
	{
		if (!pHouse)
			return;

		auto& weight = GetMacroState(pHouse).DeployerTripWeight[behindWave ? 1 : 0];
		weight = std::clamp(weight + (deployed ? DeployerTripGain : -DeployerTripLoss), DeployerTripWeightMin, DeployerTripWeightMax);
	}

	// Is this reversible building about to fall (the criteria of the last yard, ReversibleFlee*)?
	// Attacked, an enemy unit we see within ReversibleFleeEnemyLeptons, and its health under
	// ReversibleFleeHealth or falling from healthThen, framesSince ago, so fast it is gone within
	// ReversibleFleeForecastFrames.
	bool IsReversibleBuildingFalling(HouseClass* pHouse, BuildingClass* pBld, double healthThen, int framesSince)
	{
		if (!pHouse || !pBld || !pBld->HasBeenAttacked)
			return false;

		const double health = pBld->GetHealthPercentage();
		bool falling = health < ReversibleFleeHealth;

		if (!falling && framesSince > 0 && healthThen > health)
		{
			const double lossPerFrame = (healthThen - health) / framesSince;
			falling = health < lossPerFrame * ReversibleFleeForecastFrames;
		}

		if (!falling)
			return false;

		bool enemyClose = false;

		ForEachTechnoNear(pBld->GetMapCoords(), ReversibleFleeEnemyLeptons / Unsorted::LeptonsPerCell + 2, [&](TechnoClass* pTech) -> bool
		{
			if (!pTech->Owner || pTech->Owner == pHouse || pHouse->IsAlliedWith(pTech->Owner) || pTech->Owner->IsNeutral()
				|| pTech->WhatAmI() == AbstractType::Building || !IsCellVisibleToAI(pHouse, pTech->GetCell()))
			{
				return true;
			}

			if (pBld->DistanceFrom(pTech) <= ReversibleFleeEnemyLeptons)
				enemyClose = true;

			return !enemyClose;
		});

		return enemyClose;
	}

	// The yard's anti-loop, for every reversible building: none packs up again within
	// ReversibleRepackCells of the last pack-up of the house for ReversibleRepackFrames.
	bool ReversiblePackAllowed(HouseClass* pHouse, const CellStruct& here)
	{
		auto const& macro = GetMacroState(pHouse);

		return macro.LastReversiblePackCell == CellStruct::Empty || Unsorted::CurrentFrame < macro.LastReversiblePackFrame
			|| Unsorted::CurrentFrame - macro.LastReversiblePackFrame >= ReversibleRepackFrames
			|| here.DistanceFrom(macro.LastReversiblePackCell) > ReversibleRepackCells;
	}

	void NoteReversiblePack(HouseClass* pHouse, const CellStruct& here)
	{
		auto& macro = GetMacroState(pHouse);
		macro.LastReversiblePackCell = here;
		macro.LastReversiblePackFrame = Unsorted::CurrentFrame;
	}

} // namespace
