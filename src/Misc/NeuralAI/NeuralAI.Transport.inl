// NeuralAI.Transport.inl: Transport capability and move order helpers.
// Part of NeuralAI.cpp (included in order; one translation unit).

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

	// An amphibious/air/subterranean ground transport doubles as a land strike APC (engineer
	// or C4 insertion) whenever no water crossing needs it: it has no ferry trip in progress
	// and its house has no fresh ferry demand. Naval transports never do land strikes.
	bool IsTransportFreeForLandStrike(HouseClass* pHouse, UnitClass* pTrans)
	{
		if (!pHouse || !pTrans || !pTrans->Type)
			return false;

		if (!IsAmphibiousOrAirTransport(pTrans->Type))
			return true;

		if (pTrans->Type->Naval || FindFerryTrip(pTrans->UniqueID))
			return false;

		auto const& macro = GetMacroState(pHouse);

		if (macro.FerryDemandFrame >= 0 && Unsorted::CurrentFrame - macro.FerryDemandFrame <= 600)
		{
			for (int size = 1; size <= 15; ++size)
			{
				if (macro.FerryDemandSizeBySize[size] > 0)
					return false;
			}
		}

		return true;
	}

	// Type-level carryall rule, the one UnitExt::CarryallCanLift applies to a unit carrier: a vehicle
	// (not organic, naval or NonVehicle) whose type allows lifting (Carryall.Allowed), not listed in the
	// carrier's Carryall.IgnoreTypes, and within its size limit (Carryall.SizeLimit, and SizeLimit for
	// an aircraft carryall) unless the carrier lists it in Carryall.AllowTypes. Carryall.ExclusiveWithPassengers
	// depends on the carrier's passengers right now: the engine check covers it for unit carriers.
	bool CarryallTypeCanLiftType(const TechnoTypeClass* pCarryType, const TechnoTypeClass* pCargoType)
	{
		auto const pCargo = abstract_cast<const UnitTypeClass*>(pCargoType);

		if (!pCarryType || !pCargo || pCargo->Organic || pCargo->Naval || pCargo->NonVehicle)
			return false;

		auto const pCargoExt = TechnoTypeExt::Fetch(pCargo);
		auto const pCarryExt = TechnoTypeExt::Fetch(pCarryType);

		if (pCargoExt && !pCargoExt->Carryall_Allowed.Get())
			return false;

		if (pCarryExt && pCarryExt->Carryall_IgnoreTypes.Contains(const_cast<UnitTypeClass*>(pCargo)))
			return false;

		if (pCarryExt && pCarryExt->Carryall_AllowTypes.Contains(const_cast<UnitTypeClass*>(pCargo)))
			return true;

		const int cargoSize = static_cast<int>(pCargo->TechnoTypeClass::Size);

		if (pCarryType->WhatAmI() == AbstractType::AircraftType && pCarryType->SizeLimit > 0
			&& cargoSize > static_cast<int>(pCarryType->SizeLimit))
		{
			return false;
		}

		return !(pCarryExt && pCarryExt->Carryall_SizeLimit.isset() && cargoSize > pCarryExt->Carryall_SizeLimit.Get());
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
		if (!pCarryType || !pUnitType || !pCarryType->Carryall)
			return false;

		// A carryall lifts one vehicle; most carryall types leave Passengers unset (0), which
		// must not read as "no capacity" (that silently disabled every AI airlift).
		if (pCarryall->Passengers.NumPassengers >= std::max(1, static_cast<int>(pCarryType->Passengers)) || pUnit->IsOnCarryall
			|| pUnit->IsIronCurtained())
		{
			return false;
		}

		// Troops aboard and a slung vehicle exclude each other on this carrier.
		if (pCarryall->Passengers.NumPassengers > 0)
		{
			auto const pCarryTypeExt = TechnoTypeExt::Fetch(pCarryType);

			if (pCarryTypeExt && pCarryTypeExt->Carryall_ExclusiveWithPassengers)
				return false;
		}

		if (!CarryallTypeCanLiftType(pCarryType, pUnitType))
			return false;

		return !pUnit->Transporter;
	}

	// Can a transport of this type carry something between these two cells? One that flies or
	// burrows goes anywhere. One that rides land and water needs both ends within the ground and
	// water it covers itself (one zone of its own movement), so a hovercraft is not asked to get
	// past a cliff that only a flyer or a carryall crosses. Ships keep their own shore logic.
	bool CanTransportTypeLink(const TechnoTypeClass* pTransType, const CellStruct& from, const CellStruct& to)
	{
		if (!pTransType)
			return false;

		const MovementZone zone = pTransType->MovementZone;

		if (zone != MovementZone::Amphibious && zone != MovementZone::AmphibiousCrusher && zone != MovementZone::AmphibiousDestroyer)
			return true;

		if (pTransType->JumpJet || pTransType->ConsideredAircraft)
			return true;

		const int fromZone = MapClass::Instance.GetMovementZoneType(from, zone, false);
		const int toZone = MapClass::Instance.GetMovementZoneType(to, zone, false);

		return fromZone >= 0 && fromZone == toZone;
	}

	// With both cells given, only transports that can make that very crossing count
	// (CanTransportTypeLink): the unit's cell and the landing on the ground it wants to reach.
	bool CanHouseFerryTypeAcrossWater(HouseClass* pHouse, const TechnoTypeClass* pFootType, const CellStruct* pFrom = nullptr,
		const CellStruct* pTo = nullptr)
	{
		if (!pHouse || !pFootType)
			return false;

		auto const links = [pFrom, pTo](const TechnoTypeClass* pTransType) -> bool
		{
			return !pFrom || !pTo || CanTransportTypeLink(pTransType, *pFrom, *pTo);
		};

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
			// Passengers is irrelevant here: a carryall lifts one vehicle even when it is unset.
			return isCarryallFlag && isLiftableVehicle && CarryallTypeCanLiftType(pCarryType, pFootType);
		};

		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU->Owner != pHouse || pU->InLimbo || !pU->IsAlive || !pU->Type)
				continue;

			auto const pUT = pU->Type;
			if (IsAmphibiousOrAirTransport(pUT) && static_cast<int>(pUT->SizeLimit) >= passSize
				&& pUT->Passengers >= passSize && links(pUT))
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
				&& pUT->Passengers >= passSize && links(pUT);
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

	// Issues a Move only when it actually changes the order. Every new destination makes
	// the engine re-path and resets its blockage escalation (FootClass::Assign_Destination:
	// IsToPathAroundBlockage / BlockagePathDelay), so units jammed behind others never learn
	// to path around them; and moving infantry stop and re-path on ANY destination, even
	// the same one (InfantryClass::Assign_Destination, 0x51ABB7..0x51ABCC: Stop_Driver
	// unless in Attack with the same target). Vehicles ignore an identical destination
	// (UnitClass::Assign_Destination, 0x741A80). keepWithinCells adds hysteresis: a freshly
	// recomputed cell close to the one the unit is already heading to (e.g. a landing
	// beach re-scored by occupancy) keeps the current order instead of zig-zagging.
	// Ships (water-only movement): a destination outside their water body, e.g. a land cell on the
	// enemy's coast, can never be reached; the pathfinder gives up and the ship freezes in Guard.
	// Returns the nearest cell of the ship's own zone within 12 cells of the target (nullptr when
	// the target is fine, the unit is not water-only, or nothing is found).
	CellClass* NearestReachableNavalCell(FootClass* pFoot, const CellStruct& target)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		if (!pType || (pType->MovementZone != MovementZone::Water && pType->MovementZone != MovementZone::WaterBeach))
			return nullptr;

		const int ownZone = ObjectZone(pFoot, pType->MovementZone);

		if (ownZone < 0 || MapClass::Instance.GetMovementZoneType(target, pType->MovementZone, false) == ownZone)
			return nullptr;

		// Up to 32 cells: a wave target deep inland (12 was too short) left ships with a land cell
		// they could never reach, so their attack-move stalled and was sent again in a loop.
		for (int r = 1; r <= 32; ++r)
		{
			CellClass* pBest = nullptr;
			int bestDist = INT_MAX;

			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::abs(dx) != r && std::abs(dy) != r)
						continue;

					const CellStruct c { static_cast<short>(target.X + dx), static_cast<short>(target.Y + dy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(c);

					if (!pCell || MapClass::Instance.GetMovementZoneType(c, pType->MovementZone, false) != ownZone)
						continue;

					const int d = dx * dx + dy * dy;

					if (d < bestDist)
					{
						bestDist = d;
						pBest = pCell;
					}
				}
			}

			if (pBest)
				return pBest;
		}

		return nullptr;
	}

	// A ship (water-only movement) cannot shell a target farther from the nearest water of its own
	// sea than its weapon reaches. An attack order on such a target leaves it standing: it can
	// neither get closer nor fire, the stuck watchdog drops the order and its owner logic gives it
	// again (destroyers sent at buildings 9 to 21 cells inland).
	bool IsOutOfNavalReach(FootClass* pFoot, const CellStruct& target, int rangeLeptons)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		if (!pType || (pType->MovementZone != MovementZone::Water && pType->MovementZone != MovementZone::WaterBeach))
			return false;

		const int ownZone = ObjectZone(pFoot, pType->MovementZone);

		if (ownZone < 0 || MapClass::Instance.GetMovementZoneType(target, pType->MovementZone, false) == ownZone)
			return false;

		auto const pShore = NearestReachableNavalCell(pFoot, target);

		return !pShore || pShore->MapCoords.DistanceFrom(target) * Unsorted::LeptonsPerCell > rangeLeptons;
	}

	// A flyer (jumpjet, aircraft) only finishes a move once it can settle on its cell: several sent
	// to the same cell kept hovering there in Mission::Move and got the order again and again. When
	// another unit of ours already has that cell as destination or stands on it, the flyer gets the
	// nearest cell within 5 that is free of both (ring by ring, fixed order, so a flyer keeps the cell
	// it already has).
	CellClass* SpreadFlyerDestination(FootClass* pFoot, CellClass* pCell)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		if (!pType || !pCell || !pFoot->Owner
			|| !(pType->JumpJet || pType->MovementZone == MovementZone::Fly || pFoot->WhatAmI() == AbstractType::Aircraft))
		{
			return pCell;
		}

		std::vector<CellStruct> claimed;

		auto const claim = [&](FootClass* pOther)
		{
			if (!pOther || pOther == pFoot || pOther->Owner != pFoot->Owner || !pOther->IsAlive || pOther->InLimbo)
				return;

			if (auto const pOtherDest = abstract_cast<CellClass*>(pOther->Destination))
				claimed.push_back(pOtherDest->MapCoords);
			else
				claimed.push_back(pOther->GetMapCoords());
		};

		const CellStruct target = pCell->MapCoords;

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->GetMapCoords().DistanceFrom(target) <= 8.0)
				claim(pUnit);
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->GetMapCoords().DistanceFrom(target) <= 8.0)
				claim(pInf);
		}

		for (auto const pAir : AircraftClass::Array)
		{
			if (pAir && pAir->GetMapCoords().DistanceFrom(target) <= 8.0)
				claim(pAir);
		}

		auto const isClaimed = [&](const CellStruct& c)
		{
			return std::find(claimed.begin(), claimed.end(), c) != claimed.end();
		};

		if (!isClaimed(target))
			return pCell;

		for (int r = 1; r <= 5; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::abs(dx) != r && std::abs(dy) != r)
						continue;

					const CellStruct c { static_cast<short>(target.X + dx), static_cast<short>(target.Y + dy) };
					auto const pCand = MapClass::Instance.TryGetCellAt(c);

					if (!pCand || isClaimed(c) || pCand->GetBuilding()
						|| !pCand->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, false))
					{
						continue;
					}

					return pCand;
				}
			}
		}

		return pCell;
	}

	// Coarse sea route for a ship: breadth-first search over the cells of its movement zone on a
	// 3-cell grid (8 directions, fixed order), from the ship to target (a cell of its zone). Returns
	// the first cell of the route at least `hop` cells from the ship, or nullptr when the target is
	// within reach of one order (hop + 5) or no route is found (6000 grid nodes at most). A long
	// naval order then goes in hops the engine path finder can manage.
	CellClass* NavalRouteHop(FootClass* pFoot, const CellStruct& target, int hop)
	{
		const NeuralSlowStep slowStep { "NavalRouteHop" };

		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		if (!pType || (pType->MovementZone != MovementZone::Water && pType->MovementZone != MovementZone::WaterBeach))
			return nullptr;

		const CellStruct from = pFoot->GetMapCoords();

		if (from.DistanceFrom(target) <= hop + 5.0)
			return nullptr;

		const MovementZone mz = pType->MovementZone;
		const int zone = ObjectZone(pFoot, mz);

		if (zone < 0 || MapClass::Instance.GetMovementZoneType(target, mz, false) != zone)
			return nullptr;

		constexpr int Step = 3;
		constexpr int DirX[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
		constexpr int DirY[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
		auto const key = [](const CellStruct& c) { return (static_cast<uint32_t>(static_cast<uint16_t>(c.X)) << 16) | static_cast<uint16_t>(c.Y); };

		std::unordered_map<uint32_t, CellStruct> parent;
		std::vector<CellStruct> queue { from };
		parent.emplace(key(from), from);
		CellStruct reached = CellStruct::Empty;

		for (size_t head = 0; head < queue.size() && queue.size() < 6000; ++head)
		{
			const CellStruct node = queue[head];

			if (node.DistanceFrom(target) <= Step + 0.5)
			{
				reached = node;
				break;
			}

			for (int d = 0; d < 8; ++d)
			{
				const CellStruct next { static_cast<short>(node.X + DirX[d] * Step), static_cast<short>(node.Y + DirY[d] * Step) };

				if (!MapClass::Instance.CoordinatesLegal(next) || parent.contains(key(next))
					|| MapClass::Instance.GetMovementZoneType(next, mz, false) != zone)
				{
					continue;
				}

				parent.emplace(key(next), node);
				queue.push_back(next);
			}
		}

		if (reached == CellStruct::Empty)
			return nullptr;

		std::vector<CellStruct> route;

		for (CellStruct c = reached; c != from; c = parent[key(c)])
			route.push_back(c);

		for (auto it = route.rbegin(); it != route.rend(); ++it)
		{
			if (from.DistanceFrom(*it) >= hop)
				return MapClass::Instance.TryGetCellAt(*it);
		}

		return nullptr;
	}

	// Coarse land route for a ground unit: breadth-first search over the cells of its movement zone,
	// one cell at a time (a cliff line must never be stepped over; a diagonal step needs both of its
	// side cells in the zone), 8 directions in a fixed order, 40000 cells at most. The route is cached
	// per movement zone, start and target (4-cell grid) for 900 frames. Returns the route cell `hop`
	// steps ahead of the unit, or nullptr when the target is within hop + 10 cells, out of the zone,
	// or no route is found. Match #6194 (Polar Knot): both bases sit on plateaus whose cliff tiles
	// are missing from the theater; they load as flat cells, the zone map links their one-level steps
	// and the engine path finder fails inside a corridor over them ("Regular findpath failure"), so
	// whole waves stood in their own base. Legs along a route with the real step rule get out.
	// With avoidResources the route keeps off resource cells (past the first cells around the unit,
	// so one standing in a field can leave it) and is given for any target more than hop + 2 away.
	// Why the last GroundRouteHop call gave no leg (diagnostics of units that stay held).
	enum class NeuralRouteAnswer : int
	{
		Given = 0,
		Excluded, // flyer, jumpjet, teleporter, burrower
		Near, // the target is within the leg plus its margin
		NoZone, // the unit stands on no zone of its own
		GoalOffZone, // no cell of its zone at or around the target
		Budget, // the house used its searches of this tick and has no route kept
		NoRoute, // the search found none
		ShortLeg, // the leg would end next to the unit
		Count
	};

	NeuralRouteAnswer LastGroundRouteAnswer = NeuralRouteAnswer::Given;
	// Places the last GroundRouteHop call searched, -1 when it searched none (a route kept, or no search).
	int LastGroundRouteSearched = -1;

	bool IsNarrowPassage(const CellStruct& center);

	// With pAvoidPassage the route keeps off the narrow cells (IsNarrowPassage) within JamAvoidCells
	// of that spot, past the first cells around the unit, and is given for any target more than
	// hop + 2 away: the way around a passage that stays blocked.
	CellClass* GroundRouteHop(FootClass* pFoot, const CellStruct& target, int hop, bool avoidResources = false,
		const CellStruct* pAvoidPassage = nullptr)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;
		LastGroundRouteAnswer = NeuralRouteAnswer::Excluded;
		LastGroundRouteSearched = -1;

		if (!pType || pType->JumpJet || pType->Teleporter || pType->MovementZone == MovementZone::Fly
			|| pType->MovementZone == MovementZone::Subterrannean || pFoot->IsInAir())
		{
			return nullptr;
		}

		// Ships too (match #7061, Fludded Recources: a naval yard built across a narrow channel; the
		// zone map still links both sides, so the engine kept sending 15 ships into the blocked
		// channel although open sea led around the island). Water is flat: no step rule.
		const bool naval = pType->Naval || pType->MovementZone == MovementZone::Water
			|| pType->MovementZone == MovementZone::WaterBeach;
		const CellStruct from = pFoot->GetMapCoords();
		CellStruct goal = target;

		LastGroundRouteAnswer = NeuralRouteAnswer::Near;

		if (from.DistanceFrom(goal) <= hop + ((avoidResources || pAvoidPassage) ? 2.0 : 10.0))
			return nullptr;

		const MovementZone mz = pType->MovementZone;
		const int zone = ObjectZone(pFoot, mz);
		LastGroundRouteAnswer = NeuralRouteAnswer::NoZone;

		if (zone <= 1)
			return nullptr;

		// A target off the unit's zone (a ship sent at a land cell, a cell next to a building that is
		// water or cliff) stands for the nearest cell of the zone around it: within 8 cells for a
		// ship, RouteGoalSnapCells for the rest.
		if (MapClass::Instance.GetMovementZoneType(goal, mz, false) != zone)
		{
			bool found = false;
			const int reach = naval ? 8 : RouteGoalSnapCells;

			for (int r = 1; r <= reach && !found; ++r)
			{
				for (int dy = -r; dy <= r && !found; ++dy)
				{
					for (int dx = -r; dx <= r && !found; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != r)
							continue;

						const CellStruct c { static_cast<short>(target.X + dx), static_cast<short>(target.Y + dy) };

						if (MapClass::Instance.CoordinatesLegal(c) && MapClass::Instance.GetMovementZoneType(c, mz, false) == zone)
						{
							goal = c;
							found = true;
						}
					}
				}
			}
		}

		LastGroundRouteAnswer = NeuralRouteAnswer::GoalOffZone;

		if (MapClass::Instance.GetMovementZoneType(goal, mz, false) != zone)
			return nullptr;

		// A goal on a resource cell has no way to it that keeps off them.
		if (avoidResources)
		{
			auto const pGoalCell = MapClass::Instance.TryGetCellAt(goal);

			if (!pGoalCell || pGoalCell->GetContainedTiberiumIndex() >= 0)
				return nullptr;
		}

		struct GroundRoute
		{
			int MatchSerial;
			int Frame;
			std::vector<CellStruct> Cells; // start -> target
			std::vector<uint8_t> OnDeck; // per route cell: on the deck of a high bridge
		};

		static std::unordered_map<uint64_t, GroundRoute> routes;
		const int now = Unsorted::CurrentFrame;
		// Units around one start share a route: a wider square for the way around a field, whose legs
		// are short and start from the route cell nearest to each unit.
		const int startGrid = avoidResources ? 8 : 4;
		uint64_t routeKey = (static_cast<uint64_t>(static_cast<uint16_t>(zone)) << 48)
			| (static_cast<uint64_t>(static_cast<uint8_t>(mz)) << 40)
			| (static_cast<uint64_t>(static_cast<uint16_t>(from.X / startGrid)) << 30)
			| (static_cast<uint64_t>(static_cast<uint16_t>(from.Y / startGrid)) << 20)
			| (static_cast<uint64_t>(static_cast<uint16_t>(goal.X / 4)) << 10) | static_cast<uint64_t>(static_cast<uint16_t>(goal.Y / 4))
			| (avoidResources ? (1ull << 63) : 0ull);

		// A way around a passage is a route of its own, per spot avoided.
		if (pAvoidPassage)
		{
			routeKey ^= (1ull << 62);
			routeKey ^= ((static_cast<uint64_t>(static_cast<uint16_t>(pAvoidPassage->X)) << 16) | static_cast<uint16_t>(pAvoidPassage->Y))
				* 0x9E3779B97F4A7C15ull;
		}

		if (routes.size() > 256)
		{
			std::erase_if(routes, [now](const auto& entry)
				{ return entry.second.MatchSerial != NeuralMatchSerial || now < entry.second.Frame || now - entry.second.Frame > 900; });
		}

		auto it = routes.find(routeKey);

		bool search = it == routes.end() || it->second.MatchSerial != NeuralMatchSerial || now < it->second.Frame
			|| now - it->second.Frame > 900;

		// A search floods thousands of cells: a house starts a few per tick. Past that an old route of
		// this match serves once more, and a unit with none waits for the next tick (a wave spread over
		// many start cells cost 30 ms of one tick).
		if (search)
		{
			static int searchFrame = -1;
			static std::array<int, 32> searches {};

			if (searchFrame != now)
			{
				searchFrame = now;
				searches.fill(0);
			}

			const int owner = pFoot->Owner ? pFoot->Owner->ArrayIndex : -1;

			if (owner >= 0 && owner < 32 && searches[owner]++ >= GroundRouteSearchesPerTick)
			{
				LastGroundRouteAnswer = NeuralRouteAnswer::Budget;

				if (it == routes.end() || it->second.MatchSerial != NeuralMatchSerial)
					return nullptr;

				search = false;
			}
		}

		if (search)
		{
			const NeuralSlowStep slowStep { pAvoidPassage ? "route search around a passage"
				: (avoidResources ? "route search around resources" : "route search") };
			constexpr int DirX[8] = { 1, 0, -1, 0, 1, -1, -1, 1 };
			constexpr int DirY[8] = { 0, 1, 0, -1, 1, 1, -1, -1 };
			// Per cell tables of one search instead of hash maps (a search of 12000 cells took 10 to 30 ms
			// with them): the cell each one was reached from, and whether the unit can stand on it.
			// A cell has two places to stand on: its ground, and the deck of a high bridge over it. A
			// search over the ground alone never leaves an island joined to the rest by high bridges
			// (match #8533, No Rest for the Wicked, 16 of them: no route at all for either army).
			constexpr int GridSide = 512;
			constexpr size_t GridCells = static_cast<size_t>(GridSide) * GridSide;
			static std::vector<uint32_t> cameFrom(GridCells * 2);
			static std::vector<uint8_t> standable(GridCells * 2);
			std::fill(cameFrom.begin(), cameFrom.end(), 0u);
			std::fill(standable.begin(), standable.end(), static_cast<uint8_t>(0));

			struct RouteNode
			{
				CellStruct Cell;
				bool Deck;
			};

			auto const onGrid = [](const CellStruct& c) { return c.X >= 0 && c.Y >= 0 && c.X < GridSide && c.Y < GridSide; };
			auto const slot = [](const RouteNode& n)
			{ return (n.Deck ? GridCells : 0) + static_cast<size_t>(n.Cell.Y) * GridSide + static_cast<size_t>(n.Cell.X); };

			if (!onGrid(from))
				return nullptr;

			// A place the unit can stand on. Ground: in its zone and clear for its speed type (other units
			// ignored). Deck: a standing high bridge whose deck is in its zone (ships stay on the water).
			// Buildings found on ground of the zone are kept: when no route is found they are its walls.
			std::vector<BuildingClass*> walls;

			auto const canStand = [&](const RouteNode& n)
			{
				if (!onGrid(n.Cell))
					return false;

				uint8_t& known = standable[slot(n)];

				if (known == 0)
				{
					auto const pCell = MapClass::Instance.CoordinatesLegal(n.Cell) ? MapClass::Instance.TryGetCellAt(n.Cell) : nullptr;
					bool clear = false;

					// Off the passage to go around: any cell far from the spot, next to the unit or not narrow.
					auto const offPassage = [&]()
					{
						return !pAvoidPassage || n.Cell.DistanceFrom(*pAvoidPassage) > JamAvoidCells || n.Cell.DistanceFrom(from) <= 2.5
							|| !IsNarrowPassage(n.Cell);
					};

					if (pCell && n.Deck)
					{
						clear = !naval && pCell->ContainsBridge() && MapClass::Instance.GetMovementZoneType(n.Cell, mz, true) == zone
							&& offPassage();
					}
					else if (pCell && MapClass::Instance.GetMovementZoneType(n.Cell, mz, false) == zone)
					{
						clear = pCell->IsClearToMove(pType->SpeedType, true, true, -1, mz, -1, false)
							&& (!avoidResources || pCell->GetContainedTiberiumIndex() < 0 || n.Cell.DistanceFrom(from) <= 2.5)
							&& offPassage();

						if (!clear && walls.size() < RouteWallsKept)
						{
							auto const pWall = pCell->GetBuilding();

							if (pWall && std::find(walls.begin(), walls.end(), pWall) == walls.end())
								walls.push_back(pWall);
						}
					}

					known = clear ? 1 : 2;
				}

				return known == 1;
			};

			// One step between two such places, as the engine path finder allows it (FootClass::Can_Reach):
			// the same height level, or one level apart with the lower one a ground slope (SlopeIndex != 0;
			// a slope cell rises from its own level to the next one). A deck stands CellClass::BridgeLevels
			// over its cell, level with the ground at both of its ends: that is where a unit gets on and
			// off it, and ground under a bridge never links to the deck above.
			// The zone map links any two cells less than 2 levels apart, so a flat step of one level
			// shares the zone although no unit can climb it (Polar Knot: cliff and ramp tiles of tile sets
			// missing from the theater load as flat clear cells).
			auto const canStep = [naval](const RouteNode& a, const RouteNode& b)
			{
				if (naval)
					return true;

				auto const pA = MapClass::Instance.TryGetCellAt(a.Cell);
				auto const pB = MapClass::Instance.TryGetCellAt(b.Cell);

				if (!pA || !pB)
					return false;

				const int levelA = pA->Level + (a.Deck ? CellClass::BridgeLevels : 0);
				const int levelB = pB->Level + (b.Deck ? CellClass::BridgeLevels : 0);

				if (levelA == levelB)
					return true;

				if (std::abs(levelA - levelB) != 1)
					return false;

				const RouteNode& lower = levelA < levelB ? a : b;

				return !lower.Deck && (levelA < levelB ? pA : pB)->SlopeIndex != 0;
			};

			// A side cell of a diagonal step lets it through on either of its two places.
			auto const sideOpen = [&](const RouteNode& node, const CellStruct& side, const RouteNode& next)
			{
				for (int deck = 0; deck < 2; ++deck)
				{
					const RouteNode via { side, deck != 0 };

					if (canStand(via) && canStep(node, via) && canStep(via, next))
						return true;
				}

				return false;
			};

			const RouteNode start { from, !naval && pFoot->OnBridge };
			std::vector<RouteNode> queue { start };
			queue.reserve(16384);
			cameFrom[slot(start)] = static_cast<uint32_t>(slot(start)) + 1;
			bool found = false;
			RouteNode reached { CellStruct::Empty, false };

			const size_t maxNodes = avoidResources ? ResourceDetourMaxNodes : 40000;

			for (size_t head = 0; head < queue.size() && queue.size() < maxNodes; ++head)
			{
				const RouteNode node = queue[head];

				if (node.Cell.DistanceFrom(goal) <= 1.5)
				{
					reached = node;
					found = true;
					break;
				}

				for (int d = 0; d < 8; ++d)
				{
					const CellStruct nextCell { static_cast<short>(node.Cell.X + DirX[d]), static_cast<short>(node.Cell.Y + DirY[d]) };

					for (int deck = 0; deck < 2; ++deck)
					{
						const RouteNode next { nextCell, deck != 0 };

						if (!canStand(next) || cameFrom[slot(next)] != 0 || !canStep(node, next))
							continue;

						if (d >= 4 && (!sideOpen(node, CellStruct { nextCell.X, node.Cell.Y }, next)
							|| !sideOpen(node, CellStruct { node.Cell.X, nextCell.Y }, next)))
						{
							continue;
						}

						cameFrom[slot(next)] = static_cast<uint32_t>(slot(node)) + 1;
						queue.push_back(next);
					}
				}
			}

			GroundRoute route { NeuralMatchSerial, now, {}, {} };
			LastGroundRouteSearched = static_cast<int>(queue.size());

			if (found)
			{
				for (RouteNode n = reached; n.Cell != from || n.Deck != start.Deck;)
				{
					route.Cells.push_back(n.Cell);
					route.OnDeck.push_back(n.Deck ? 1 : 0);
					const size_t back = static_cast<size_t>(cameFrom[slot(n)]) - 1;
					const size_t cell = back % GridCells;
					n = RouteNode { CellStruct { static_cast<short>(cell % GridSide), static_cast<short>(cell / GridSide) }, back >= GridCells };
				}

				std::reverse(route.Cells.begin(), route.Cells.end());
				std::reverse(route.OnDeck.begin(), route.OnDeck.end());
			}
			else if (!avoidResources && !pAvoidPassage && queue.size() < maxNodes)
			{
				// The whole ground the unit can walk was searched and the target, in its zone, is not on
				// it: buildings close the way (the zone map does not know them). Logged a few times per
				// match with the buildings the search ran into that stand nearest to the target.
				static int wallLogSerial = -1;
				static int wallLogCount = 0;

				if (wallLogSerial != NeuralMatchSerial)
				{
					wallLogSerial = NeuralMatchSerial;
					wallLogCount = 0;
				}

				if (wallLogCount < RouteWallLogsPerMatch)
				{
					++wallLogCount;
					std::string wallText;

					std::sort(walls.begin(), walls.end(), [&goal](BuildingClass* a, BuildingClass* b)
					{
						const double distA = a->GetMapCoords().DistanceFrom(goal);
						const double distB = b->GetMapCoords().DistanceFrom(goal);

						return distA != distB ? distA < distB : a->UniqueID < b->UniqueID;
					});

					if (walls.size() > RouteWallsLogged)
						walls.resize(RouteWallsLogged);

					for (auto const pWall : walls)
					{
						const CellStruct wc = pWall->GetMapCoords();
						char item[64];
						std::snprintf(item, sizeof(item), " %s h%d (%d,%d)", pWall->Type->ID, pWall->Owner ? pWall->Owner->ArrayIndex : -1, wc.X, wc.Y);
						wallText += item;
					}

					Debug::Log("[NeuralAI] Ground route closed: %s of house %d (%d,%d)->(%d,%d) mz %d zone %d, searched %d, frame %d, buildings on its edge:%s\n",
						pType->ID, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1, from.X, from.Y, goal.X, goal.Y, static_cast<int>(mz), zone,
						static_cast<int>(queue.size()), now, wallText.empty() ? " none" : wallText.c_str());
				}
			}

			// Diagnostics, the first 4 routes of a match: land types and heights along the route.
			static int routeLogSerial = -1;
			static int routeLogCount = 0;

			if (routeLogSerial != NeuralMatchSerial)
			{
				routeLogSerial = NeuralMatchSerial;
				routeLogCount = 0;
			}

			if (routeLogCount < 4)
			{
				++routeLogCount;
				int landCounts[16] = {};
				int minLevel = 999;
				int maxLevel = -999;
				int maxStep = 0;
				int prevLevel = INT_MIN;

				for (auto const& c : route.Cells)
				{
					if (auto const pC = MapClass::Instance.TryGetCellAt(c))
					{
						landCounts[std::clamp(static_cast<int>(pC->LandType), 0, 15)]++;
						minLevel = std::min(minLevel, static_cast<int>(pC->Level));
						maxLevel = std::max(maxLevel, static_cast<int>(pC->Level));

						if (prevLevel != INT_MIN)
							maxStep = std::max(maxStep, std::abs(static_cast<int>(pC->Level) - prevLevel));

						prevLevel = pC->Level;
					}
				}

				char landText[160] = "";
				size_t landLen = 0;

				for (int lt = 0; lt < 16; ++lt)
				{
					if (landCounts[lt] > 0 && landLen < sizeof(landText) - 16)
						landLen += std::snprintf(landText + landLen, sizeof(landText) - landLen, "L%d:%d ", lt, landCounts[lt]);
				}

				Debug::Log("[NeuralAI] Ground route %s (%d,%d)->(%d,%d) mz %d zone %d: %d cells (%d on bridges), land %s levels %d..%d max step %d, searched %d\n",
					pType->ID, from.X, from.Y, target.X, target.Y, static_cast<int>(mz), zone, static_cast<int>(route.Cells.size()),
					static_cast<int>(std::count(route.OnDeck.begin(), route.OnDeck.end(), static_cast<uint8_t>(1))),
					landText, minLevel, maxLevel, maxStep, static_cast<int>(queue.size()));

				// The ground of the first 40 route cells (first route of the match only): 'r' route, '#'
				// building, '~' water, '^' not clear for this unit, '/' a slope, 'u' a unit, digits the height level.
				if (routeLogCount == 1)
				{
					// Without a route, the ground around the start.
					const int around = route.Cells.empty() ? 24 : 0;
					int x0 = from.X - around, x1 = from.X + around, y0 = from.Y - around, y1 = from.Y + around;

					for (size_t i = 0; i < route.Cells.size() && i < 40; ++i)
					{
						x0 = std::min<int>(x0, route.Cells[i].X);
						x1 = std::max<int>(x1, route.Cells[i].X);
						y0 = std::min<int>(y0, route.Cells[i].Y);
						y1 = std::max<int>(y1, route.Cells[i].Y);
					}

					x0 -= 4; x1 = std::min(x1 + 4, x0 + 59); y0 -= 4; y1 = std::min(y1 + 4, y0 + 49);

					for (int y = y0; y <= y1; ++y)
					{
						char row[64] = {};

						for (int x = x0; x <= x1; ++x)
						{
							const CellStruct c { static_cast<short>(x), static_cast<short>(y) };
							auto const pC = MapClass::Instance.TryGetCellAt(c);
							char ch = ' ';

							if (pC)
							{
								const bool onRoute = std::find(route.Cells.begin(), route.Cells.begin() + std::min<size_t>(40, route.Cells.size()), c)
									!= route.Cells.begin() + std::min<size_t>(40, route.Cells.size());

								if (pC->GetBuilding())
									ch = '#';
								else if (pC->LandType == LandType::Water)
									ch = '~';
								else if (!pC->IsClearToMove(pType->SpeedType, true, true, -1, mz, -1, false))
									ch = '^';
								else if (onRoute)
									ch = 'r';
								else if (pC->SlopeIndex != 0)
									ch = '/';
								else if (pC->FirstObject)
									ch = 'u';
								else
									ch = static_cast<char>('0' + std::clamp(static_cast<int>(pC->Level), 0, 9));
							}

							row[x - x0] = ch;
						}

						Debug::Log("[NeuralAI]   route y %3d x%3d %s\n", y, x0, row);
					}
				}
			}

			it = routes.insert_or_assign(routeKey, std::move(route)).first;
		}

		// The route cell `hop` steps ahead of the route cell nearest to this unit (units of one group
		// share a cached route from a start up to 4 cells away). Counted in steps, not in straight
		// distance: a leg is then a stretch of the route itself, which the engine path finder walks
		// even where its zone map links cells no unit can step between (Polar Knot: a 25-cell
		// straight leg around a plateau edge still failed).
		auto const& cells = it->second.Cells;
		LastGroundRouteAnswer = NeuralRouteAnswer::NoRoute;

		if (cells.empty())
			return nullptr;

		size_t nearest = 0;
		double nearestDist = from.DistanceFrom(cells[0]);

		for (size_t i = 1; i < cells.size(); ++i)
		{
			const double dist = from.DistanceFrom(cells[i]);

			if (dist < nearestDist)
			{
				nearestDist = dist;
				nearest = i;
			}
		}

		size_t next = std::min(cells.size() - 1, nearest + static_cast<size_t>(std::max(1, hop)));

		// A leg never ends on a bridge or at its mouth, where the units of the wave would stop in each
		// other's way: it runs on to the ground past the far end.
		auto const& onDeck = it->second.OnDeck;

		if (next < onDeck.size() && onDeck[next])
		{
			while (next + 1 < cells.size() && onDeck[next])
				++next;

			next = std::min(cells.size() - 1, next + RouteLegPastBridgeCells);
		}

		LastGroundRouteAnswer = NeuralRouteAnswer::ShortLeg;

		if (from.DistanceFrom(cells[next]) < 3.0)
			return nullptr;

		LastGroundRouteAnswer = NeuralRouteAnswer::Given;

		return MapClass::Instance.TryGetCellAt(cells[next]);
	}

	bool IssueMoveOrder(FootClass* pFoot, AbstractClass* pDest, double keepWithinCells = 1.5)
	{
		if (!pFoot || !pDest || pFoot->InLimbo || !pFoot->IsAlive)
			return false;

		if (auto const pDestAsCell = abstract_cast<CellClass*>(pDest))
		{
			if (auto const pReach = NearestReachableNavalCell(pFoot, pDestAsCell->MapCoords))
				pDest = pReach;
			else
				pDest = SpreadFlyerDestination(pFoot, pDestAsCell);
		}

		const bool isMoving = pFoot->CurrentMission == Mission::Move || pFoot->QueuedMission == Mission::Move;

		if (isMoving && pFoot->Destination)
		{
			if (pFoot->Destination == pDest)
			{
				CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
				return false;
			}

			if (keepWithinCells > 0.0)
			{
				auto const pOldCell = abstract_cast<CellClass*>(pFoot->Destination);
				auto const pNewCell = abstract_cast<CellClass*>(pDest);

				if (pOldCell && pNewCell && pOldCell->MapCoords.DistanceFrom(pNewCell->MapCoords) <= keepWithinCells)
				{
					CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
					return false;
				}
			}
		}

		// Already standing there: the engine clears the destination on arrival, so without this a
		// group holding a point got the same move order every AI tick.
		if (!isMoving && keepWithinCells > 0.0)
		{
			auto const pDestCell = abstract_cast<CellClass*>(pDest);

			if (pDestCell && pFoot->GetMapCoords().DistanceFrom(pDestCell->MapCoords) <= keepWithinCells)
			{
				CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
				return false;
			}
		}

		pFoot->SetDestination(pDest, true);
		pFoot->QueueMission(Mission::Move, true);
		CountDecision(NeuralDecision::MoveOrderIssued, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);

		return true;
	}

	CellClass* GetSafeAirHop(HouseClass* pHouse, FootClass* pCarrier, CellClass* pGoal);

	// Sends an idle, empty carryall with no airlift job back home (it would otherwise hover
	// wherever it dropped its last cargo, e.g. over an expansion island or the front): next to
	// the nearest own repair station, else construction yard, else factory. Does nothing when
	// it is already within 8 cells of any of them. Returns true when an order was issued.
	CellClass* FindCarrierHomeCell(HouseClass* pHouse, FootClass* pCarrier, int* pNearestHomeDist = nullptr)
	{
		if (!pHouse || !pCarrier)
			return nullptr;

		BuildingClass* pBest = nullptr;
		int bestRank = 3;
		int bestDist = INT_MAX;
		int nearestHomeDist = INT_MAX;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type)
				continue;

			const int rank = pBld->Type->UnitRepair ? 0
				: (pBld->Type->ConstructionYard ? 1 : (pBld->Type->Factory != AbstractType::None ? 2 : 3));

			if (rank >= 3)
				continue;

			const int dist = pCarrier->DistanceFrom(pBld);
			nearestHomeDist = std::min(nearestHomeDist, dist);

			if (rank < bestRank || (rank == bestRank && dist < bestDist))
			{
				bestRank = rank;
				bestDist = dist;
				pBest = pBld;
			}
		}

		if (pNearestHomeDist)
			*pNearestHomeDist = nearestHomeDist;

		if (!pBest)
			return nullptr;

		// Park just south of the footprint, never on top of the building itself.
		const CellStruct bldCell = pBest->GetMapCoords();
		const CellStruct parkCoords { bldCell.X, static_cast<short>(bldCell.Y + pBest->Type->GetFoundationHeight(false) + 1) };
		CellClass* pPark = MapClass::Instance.TryGetCellAt(parkCoords);

		return pPark ? pPark : pBest->GetCell();
	}

	bool ReturnIdleCarryallToBase(HouseClass* pHouse, FootClass* pCarrier)
	{
		int nearestHomeDist = INT_MAX;
		CellClass* const pPark = FindCarrierHomeCell(pHouse, pCarrier, &nearestHomeDist);

		if (!pPark || nearestHomeDist <= 8 * Unsorted::LeptonsPerCell)
			return false;

		return IssueMoveOrder(pCarrier, GetSafeAirHop(pHouse, pCarrier, pPark), 3.0);
	}

} // namespace
