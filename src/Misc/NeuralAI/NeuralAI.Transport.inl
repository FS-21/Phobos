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
			// Passengers is irrelevant here: a carryall lifts one vehicle even when it is unset.
			if (!isCarryallFlag || !isLiftableVehicle || !pCarryType)
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
