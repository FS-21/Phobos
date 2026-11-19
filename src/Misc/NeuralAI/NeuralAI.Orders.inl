// NeuralAI.Orders.inl: Repair, enter and attack-move orders, boarding, island landing and ferry waypoints.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	int CountStationTrips(const HouseClass* pHouse, DWORD stationID, DWORD excludeID);

	// Picks the repair station (UnitRepair=yes) this unit should use: only stations it can
	// actually reach (naval units at naval stations, ground units at land stations,
	// amphibious/hover units at either), balancing load by adding the current queue (our
	// units already entering or docked there, or on a repair trip to it) to the distance. A
	// station with RepairStationQueueUnits trips is full: none when every one is.
	BuildingClass* FindBestRepairStation(HouseClass* pHouse, FootClass* pFoot, bool airlifted = false)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		// Repairs cost credits: none while broke (UpdateEconomyCrisis).
		if (!pHouse || !pType || GetMacroState(pHouse).BrokeHoldActive)
			return nullptr;

		const CellStruct fc = pFoot->GetMapCoords();
		const bool footNaval = pType->Naval;
		const bool footAmphibious = pType->SpeedType == SpeedType::Hover || pType->SpeedType == SpeedType::Amphibious
			|| pType->MovementZone == MovementZone::Amphibious || pType->MovementZone == MovementZone::AmphibiousDestroyer
			|| pType->MovementZone == MovementZone::AmphibiousCrusher;
		BuildingClass* pBest = nullptr;
		double bestCost = 1e9;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->UnitRepair
				|| pBld->CurrentMission == Mission::Selling)
			{
				continue;
			}

			const bool stationNaval = pBld->Type->Naval;

			if ((footNaval && !stationNaval && !footAmphibious) || (!footNaval && stationNaval && !footAmphibious))
				continue;

			// Reachable when some cell around the foundation is in the unit's movement zone.
			const CellStruct bc = pBld->GetMapCoords();
			const int w = pBld->Type->GetFoundationWidth();
			const int h = pBld->Type->GetFoundationHeight(false);
			bool reachable = false;

			for (int dy = -1; dy <= h && !reachable; ++dy)
			{
				for (int dx = -1; dx <= w && !reachable; ++dx)
				{
					if (dx >= 0 && dx < w && dy >= 0 && dy < h)
						continue;

					const CellStruct edge { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };

					if (AreZonesConnected(fc, edge, pType->MovementZone))
						reachable = true;
				}
			}

			// An airlifted unit (carried by a carryall) does not need a ground path.
			if (!reachable && !airlifted)
				continue;

			int queue = 0;

			for (auto const pTech : TechnoClass::Array)
			{
				auto const pOther = abstract_cast<FootClass*>(pTech);

				if (pOther && pOther != pFoot && pOther->Owner == pHouse && pOther->IsAlive && !pOther->InLimbo
					&& (pOther->Destination == pBld || pOther->ArchiveTarget == pBld || pOther->ContainsLink(pBld))
					&& (pOther->CurrentMission == Mission::Enter || pOther->ContainsLink(pBld)))
				{
					queue++;
				}
			}

			// A unit waiting at a busy station is on a move to a cell next to it, which the count above
			// misses; our repair trips to it count them.
			queue = std::max(queue, CountStationTrips(pHouse, pBld->UniqueID, pFoot->UniqueID));

			if (queue >= RepairStationQueueUnits)
				continue;

			const double cost = fc.DistanceFrom(bc) + queue * 8.0;

			if (cost < bestCost)
			{
				bestCost = cost;
				pBest = pBld;
			}
		}

		return pBest;
	}

	// True while a transport is heading to, waiting at or docked in a repair station.
	bool IsTransportUnderRepair(FootClass* pTrans)
	{
		if (!pTrans)
			return false;

		for (AbstractClass* const pRef : { pTrans->Destination, pTrans->ArchiveTarget, static_cast<AbstractClass*>(pTrans->GetNthLink(0)) })
		{
			if (auto const pBld = abstract_cast<BuildingClass*>(pRef))
			{
				if (pBld->Type && pBld->Type->UnitRepair && (pTrans->CurrentMission == Mission::Enter || pTrans->ContainsLink(pBld)))
					return true;
			}
		}

		return false;
	}

	void OrderFootToEnter(FootClass* pFoot, TechnoClass* pEnterTarget)
	{
		if (!pFoot || !pEnterTarget || pFoot->InLimbo || !pFoot->IsAlive || pEnterTarget->InLimbo
			|| !pEnterTarget->IsAlive || IsHeldByTransportOp(pFoot))
		{
			return;
		}

		if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
		{
			if (pFoot->ContainsLink(pEnterTarget) || pFoot->ArchiveTarget == pEnterTarget
				|| pFoot->Destination == pEnterTarget)
			{
				return;
			}

			// When FootClass::Mission_Enter is driving a large vehicle (e.g. Slave
			// Miner / MCV) onto the boarding cell next to pEnterTarget, the YR engine
			// sets pFoot->Destination to that adjacent CellClass*. Do NOT reset its
			// navigation list or spam SetDestination!
			if (pFoot->Destination != nullptr)
			{
				const CellStruct dc = CellClass::Coord2Cell(pFoot->Destination->GetCoords());
				if (dc.DistanceFrom(pEnterTarget->GetMapCoords()) <= 3.5)
					return;
			}
		}

		// Match EventClass::Execute (MEGA_MISSION, 0x004C73B9-0x004C747C) exact call
		// order: UnitClass::Assign_Destination (0x00741C54) and
		// InfantryClass::Assign_Destination (0x0051AC2A) ONLY establish radio contact
		// (RequestLink + RequestLoading) or ArchiveTarget when CurrentMission ==
		// Mission::Enter or QueuedMission == Mission::Enter BEFORE SetDestination!
		pFoot->ClearMegaMissionData();

		if (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
			|| pFoot->CurrentMission == Mission::None || pFoot->CurrentMission == Mission::Sleep)
		{
			pFoot->ForceMission(Mission::Enter);
		}
		else
		{
			pFoot->QueueMission(Mission::Enter, false);
		}

		pFoot->SetArchiveTarget(nullptr);
		pFoot->ClearNavigationList();
		pFoot->SetTarget(nullptr);
		pFoot->SetDestination(pEnterTarget, true);

		if (pFoot->ReadyToNextMission())
			pFoot->NextMission();
	}

	// The zones a transport compares when a passenger asks to load (UnitClass::Receive_Message):
	// those of the last cell each of them occupied, read for the passenger's movement zone. With
	// different zones it sets out for the passenger instead of taking it in, and the passenger
	// waits linked to it.
	struct FerryDoorZones
	{
		int Transport { -1 };
		int Boarder { -1 };

		bool Match() const
		{
			return this->Transport == this->Boarder;
		}
	};

	FerryDoorZones GetFerryDoorZones(const FootClass* pTransport, const FootClass* pBoarder)
	{
		const MovementZone movementZone = pBoarder->GetTechnoType()->MovementZone;

		return { MapClass::Instance.GetMovementZoneType(pTransport->LastMapCoords, movementZone, false),
			MapClass::Instance.GetMovementZoneType(pBoarder->LastMapCoords, movementZone, false) };
	}

	// A load larger than this never runs dry.
	constexpr int AirEndlessLoadShots = 100;

	// A load that never runs dry: no load at all (Ammo=-1), or one so large it never ends (a mod
	// gives its gunships Ammo=2147483645: a sortie damage of shots x damage overflowed, and such an
	// aircraft never had its full load again after its first shot).
	bool HasEndlessAirLoad(const AircraftTypeClass* pType)
	{
		return pType->Ammo < 0 || pType->Ammo > AirEndlessLoadShots;
	}

	// Shots an air sortie counts: the load of an aircraft that rearms, 4 for an endless one.
	int AirSortieShots(const AircraftTypeClass* pType)
	{
		return HasEndlessAirLoad(pType) ? 4 : std::max(1, pType->Ammo);
	}

	// An aircraft on the ground, linked to a dock that has power, with less than its full load:
	// its dock is reloading it.
	bool IsAircraftReloadingOnDock(AircraftClass* pAir)
	{
		if (!pAir || HasEndlessAirLoad(pAir->Type) || pAir->Ammo >= pAir->Type->Ammo || pAir->IsInAir())
			return false;

		auto const pDock = abstract_cast<BuildingClass*>(pAir->GetNthLink(0));

		return pDock && pDock->IsPowerOnline();
	}

	// An attack order the aircraft keeps all the way to its target. With the extended aircraft
	// missions an aircraft that has a guard point (ArchiveTarget, set by the air patrol) drops its
	// target as soon as it is 1.1 times its guard range from that point and flies back to it
	// (AircraftClass_Update_UnlandableDamage), so an aircraft taken off a patrol by a plan or a
	// strike never got farther than about 40 cells from its patrol stop: a Blackout of three Chaos
	// Bombers circled 39 cells from home and 171 from the power plant until its 3600 frames ran out,
	// an Air Strike stopped 41 cells from its target (matches #9571, #9608). The guard point goes;
	// the air patrol gives a new one once the aircraft is idle again.
	void OrderAircraftAttack(AircraftClass* pAir, AbstractClass* pTarget)
	{
		if (!pAir || !pTarget)
			return;

		if (pAir->ArchiveTarget && AircraftTypeExt::Fetch(pAir->Type)->ExtendedAircraftMissions.Get(RulesExt::Global()->ExtendedAircraftMissions))
			pAir->SetArchiveTarget(nullptr);

		pAir->SetTarget(pTarget);
		pAir->QueueMission(Mission::Attack, true);
	}

	// The same for any plan member: aircraft through OrderAircraftAttack, the rest as before.
	void OrderFootAttack(FootClass* pFoot, AbstractClass* pTarget)
	{
		if (auto const pAir = abstract_cast<AircraftClass*>(pFoot))
		{
			OrderAircraftAttack(pAir, pTarget);
			return;
		}

		pFoot->SetTarget(pTarget);
		pFoot->QueueMission(Mission::Attack, true);
	}

	// A transport that flies and is off the ground.
	bool IsFerryAloft(const FootClass* pTransport)
	{
		auto const pType = pTransport->GetTechnoType();

		return (pType->JumpJet || pType->MovementZone == MovementZone::Fly) && pTransport->GetHeight() > 0;
	}

	// The engine records the cell a unit occupies when it reaches the cell's centre
	// (FootClass::Per_Cell_Process), so one halted short of the centre, or a jumpjet that landed,
	// still counts as standing on the cell before: water for a transport stopped at the shore.
	// Runs that step for a unit at rest on the ground; true when its record was behind and caught up.
	bool RegisterStoppedFootCell(FootClass* pFoot)
	{
		if (pFoot->LastMapCoords == pFoot->GetMapCoords() || pFoot->Destination || pFoot->Locomotor->Is_Moving()
			|| IsFerryAloft(pFoot))
		{
			return false;
		}

		pFoot->UpdatePosition(PCPType::End);

		return pFoot->LastMapCoords == pFoot->GetMapCoords();
	}

	// Why a passenger cannot go aboard a transport right now (nullptr: it can): it stands within
	// BoardingReachLeptons of it, the transport is at rest on the ground, and nothing the engine
	// refuses when a passenger asks to enter (0x73758A) holds.
	const char* BoardingRefusal(FootClass* pTransport, FootClass* pPassenger)
	{
		if (std::abs(pPassenger->Location.X - pTransport->Location.X) >= BoardingReachLeptons
			|| std::abs(pPassenger->Location.Y - pTransport->Location.Y) >= BoardingReachLeptons
			|| std::abs(pPassenger->Location.Z - pTransport->Location.Z) >= Unsorted::CellHeight)
		{
			return "out of reach";
		}

		// The transport at rest: on the ground, off a Move or Unload mission, with no destination.
		// What its locomotor says is not asked: a hovercraft reports movement for as long as it
		// floats, and three LCRF stood 1500 frames with their soldiers next to them.
		if (IsFerryAloft(pTransport))
			return "transport in the air";

		if (pTransport->Destination || pTransport->CurrentMission == Mission::Move || pTransport->QueuedMission == Mission::Move)
			return "transport on the move";

		if (pTransport->CurrentMission == Mission::Unload)
			return "transport unloading";

		if (pTransport->IsBeingWarpedOut() || pTransport->OnBridge)
			return "transport warped or on a bridge";

		auto const pPassengerInfantry = abstract_cast<InfantryClass*>(pPassenger);
		auto const pControl = pPassenger->CaptureManager;

		if (pPassenger->Deactivated || pPassenger->IsUnderEMP() || pPassenger->IsMindControlled() || pPassenger->ParasiteEatingMe
			|| (pControl && pControl->IsControllingSomething()) || (pPassengerInfantry && pPassengerInfantry->IsDeployed()))
		{
			return "passenger not free to enter";
		}

		auto const pTransportCell = pTransport->GetCell();

		if (pTransportCell && pTransportCell->LandType == LandType::Water
			&& !TechnoTypeExt::Fetch(pTransport->GetTechnoType())->AmphibiousEnter.Get(RulesExt::Global()->AmphibiousEnter))
		{
			return "transport on water";
		}

		return CanPassengerFitInTransport(pTransport, pPassenger->GetTechnoType()) ? nullptr : "no room";
	}

	// Takes aboard, at once, a passenger with no BoardingRefusal: the steps the engine runs at the
	// end of an Enter move (0x51A21B for infantry, 0x73A6D1 for vehicles), in the order the fast
	// enter action of Hooks.Transport.cpp runs them. The request to load over the radio left the
	// soldiers of a quarter to a half of the Airborne Front plans outside a transport they stood
	// next to. True when it went in.
	bool TakeAboardNow(FootClass* pTransport, FootClass* pPassenger)
	{
		if (BoardingRefusal(pTransport, pPassenger))
			return false;

		auto const pTransportType = pTransport->GetTechnoType();

		pPassenger->SetTarget(nullptr);
		pPassenger->SetArchiveTarget(nullptr);
		pPassenger->MissionAccumulateTime = 0;
		pPassenger->GattlingValue = 0;
		pPassenger->CurrentGattlingStage = 0;
		pPassenger->Limbo();
		pPassenger->OnBridge = false;
		pPassenger->NextObject = nullptr;
		pPassenger->SetDestination(nullptr, true);
		pPassenger->QueueUpToEnter = nullptr;
		pPassenger->FrozenStill = true;
		pPassenger->SetSpeedPercentage(0.0);
		pPassenger->Transporter = pTransport;
		pTransport->AddPassenger(pPassenger);

		if (pTransportType->OpenTopped)
			pTransport->EnteredOpenTopped(pPassenger);

		if (pPassenger->GetTechnoType()->OpenTopped)
			pPassenger->SetTargetForPassengers(nullptr);

		pPassenger->Undiscover();
		FootExt::Fetch(pPassenger)->ResetLocomotor = true;

		return true;
	}

	// Where a passenger walks to be taken aboard (TakeAboardNow): the cell next to the transport
	// nearest to it that it can stand on, fixed order among equals; nullptr when none is free. Sent
	// onto the transport itself it asks to enter, and the transport sets out for it instead.
	CellClass* BoardingCellNextTo(FootClass* pTransport, FootClass* pBoarder)
	{
		const CellStruct at = pTransport->GetMapCoords();
		auto const pType = pBoarder->GetTechnoType();
		CellClass* pBest = nullptr;
		int bestDist = INT_MAX;

		for (int dy = -1; dy <= 1; ++dy)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				const CellStruct c { static_cast<short>(at.X + dx), static_cast<short>(at.Y + dy) };
				auto const pCell = (dx != 0 || dy != 0) && MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

				if (!pCell || pCell->GetBuilding() || !pCell->IsClearToMove(pType->SpeedType, true, false, -1, pType->MovementZone, -1, false))
					continue;

				const int dist = pBoarder->DistanceFrom(pCell);

				if (dist < bestDist)
				{
					bestDist = dist;
					pBest = pCell;
				}
			}
		}

		return pBest;
	}

	// Lets the passengers of a transport on the ground out onto the cells next to it, at once: the
	// steps of the engine's unload, which detaches each passenger and puts it on a cell next to
	// the transport that it can enter (UnitClass::Mission_Unload). The mission itself first waits
	// for the locomotor to report no movement, and a hovercraft reports movement for as long as
	// it floats. Returns how many got out; one with no cell to stand on stays aboard with those
	// behind it.
	int PutAshoreNow(FootClass* pTransport)
	{
		if (IsFerryAloft(pTransport) || pTransport->OnBridge)
			return 0;

		const CellStruct at = pTransport->GetMapCoords();
		int out = 0;

		while (auto const pPassenger = pTransport->Passengers.GetFirstPassenger())
		{
			CellClass* pSpot = nullptr;
			CoordStruct where = CoordStruct::Empty;

			for (int direction = 0; direction < 8 && !pSpot; ++direction)
			{
				const CellStruct offset = Unsorted::AdjacentCell[direction];
				const CellStruct c { static_cast<short>(at.X + offset.X), static_cast<short>(at.Y + offset.Y) };
				auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

				if (!pCell || pCell->ContainsBridge())
					continue;

				const Move occupied = pPassenger->IsCellOccupied(pCell, FacingType::None, -1, nullptr, true);

				if (occupied != Move::OK && occupied != Move::MovingBlock)
					continue;

				CoordStruct coords = pCell->GetCoordsWithBridge();

				if (pPassenger->WhatAmI() == AbstractType::Infantry)
				{
					coords = pCell->FindInfantrySubposition(coords, false, false, false);

					if (coords == CoordStruct::Empty)
						continue;

					coords.Z = pCell->GetCoordsWithBridge().Z;
				}

				pSpot = pCell;
				where = coords;
			}

			if (!pSpot)
				break;

			pTransport->RemoveFirstPassenger();
			pPassenger->OnBridge = false;
			++Unsorted::ScenarioInit;
			const bool placed = pPassenger->Unlimbo(where, static_cast<DirType>(0));
			--Unsorted::ScenarioInit;

			if (!placed)
			{
				pTransport->EnterAsPassenger(pPassenger);
				break;
			}

			if (pTransport->GetTechnoType()->OpenTopped)
				pTransport->ExitedOpenTopped(pPassenger);

			pPassenger->Transporter = nullptr;
			pPassenger->LastMapCoords = pSpot->MapCoords;
			pPassenger->ShouldEnterOccupiable = false;
			pPassenger->ShouldGarrisonStructure = false;
			pPassenger->QueueMission(Mission::Guard, false);
			out++;
		}

		return out;
	}

	// TEMP diagnostic: a transport the loader left parked to load (Guard or landing, no destination)
	// that is found with other orders after a part of the AI tick: that part, or the engine between
	// two ticks, moved it. Logged once per departure from the parking; the loader parks it again.
	void TraceParkedFerries(int houseIndex, const char* part)
	{
		auto const pHouse = houseIndex >= 0 && houseIndex < HouseClass::Array.Count ? HouseClass::Array.GetItem(houseIndex) : nullptr;

		if (!pHouse)
			return;

		auto& parked = GetMacroState(pHouse).ParkedFerries;

		if (parked.empty())
			return;

		static int traceLogSerial = -1;
		static int traceLogCount = 0;

		if (traceLogSerial != NeuralMatchSerial)
		{
			traceLogSerial = NeuralMatchSerial;
			traceLogCount = 0;
		}

		for (auto it = parked.begin(); it != parked.end();)
		{
			UnitClass* const pUnit = it->second.Unit;

			if (UnitClass::Array.FindItemIndex(pUnit) < 0 || pUnit->UniqueID != it->first || !pUnit->IsAlive || pUnit->InLimbo)
			{
				it = parked.erase(it);
				continue;
			}

			const bool stillParked = (pUnit->CurrentMission == Mission::Guard || pUnit->CurrentMission == Mission::Unload)
				&& !pUnit->Destination;

			if (stillParked)
			{
				++it;
				continue;
			}

			if (traceLogCount++ < 80)
			{
				auto const pDestCell = abstract_cast<CellClass*>(pUnit->Destination);
				auto const pDestObject = abstract_cast<ObjectClass*>(pUnit->Destination);
				auto const pTargetObject = abstract_cast<ObjectClass*>(pUnit->Target);
				Debug::Log("[NeuralAI] Ferry DIAG house %d frame %d: parked %s %u moved by %s: mission %d queued %d mega %d, dest %s (%d,%d), "
					"target %s, pass %d, at (%d,%d).\n",
					houseIndex, Unsorted::CurrentFrame, pUnit->Type->ID, pUnit->UniqueID, part, static_cast<int>(pUnit->CurrentMission),
					static_cast<int>(pUnit->QueuedMission), static_cast<int>(pUnit->MegaMission),
					pDestCell ? "cell" : (pDestObject && pDestObject->GetType() ? pDestObject->GetType()->ID : "none"),
					pDestCell ? pDestCell->MapCoords.X : -1, pDestCell ? pDestCell->MapCoords.Y : -1,
					pTargetObject && pTargetObject->GetType() ? pTargetObject->GetType()->ID : "none", pUnit->Passengers.NumPassengers,
					pUnit->GetMapCoords().X, pUnit->GetMapCoords().Y);
			}

			it = parked.erase(it);
		}
	}

	bool CanFootDamageBuildings(const FootClass* pFoot)
	{
		if (!pFoot)
			return false;

		const auto pType = pFoot->GetTechnoType();
		if (!pType)
			return false;

		if (pType->Spawns != nullptr)
			return true;

		for (int wIdx = 0; wIdx < 2; ++wIdx)
		{
			const auto pWS = pFoot->GetWeapon(wIdx);
			const auto pWpn = pWS ? pWS->WeaponType : nullptr;
			if (!pWpn)
				continue;

			if (pWpn->Spawner)
				return true;

			if (pWpn->Damage <= 0)
				continue;

			if (pWpn->Projectile && !pWpn->Projectile->AG)
				continue;

			if (pWpn->Damage <= 5)
				continue;

			return true;
		}

		return false;
	}

	// Resource cells on the straight line between two cells, over its first maxCells.
	int ResourceCellsOnLine(const CellStruct& from, const CellStruct& to, int maxCells)
	{
		const int dx = to.X - from.X;
		const int dy = to.Y - from.Y;
		const int span = std::max(std::abs(dx), std::abs(dy));
		const int steps = std::min(span, maxCells);
		int count = 0;

		for (int i = 1; i <= steps; ++i)
		{
			const CellStruct cell { static_cast<short>(from.X + dx * i / span), static_cast<short>(from.Y + dy * i / span) };
			auto const pCell = MapClass::Instance.TryGetCellAt(cell);

			if (pCell && pCell->GetContainedTiberiumIndex() >= 0)
				++count;
		}

		return count;
	}

	// The next leg of a way around the resource fields for a unit their cells hurt, when walking
	// straight to the goal would cost it a good share of its health (in large fields soldiers died
	// before reaching the other side). nullptr when the straight way is cheap or no way keeps off them.
	CellClass* ResourceDetourHop(FootClass* pFoot, const CellStruct& goal)
	{
		const int damage = ResourceCellDamage(pFoot->GetTechnoType());

		if (damage <= 0 || pFoot->IsInAir())
			return nullptr;

		const int cells = ResourceCellsOnLine(pFoot->GetMapCoords(), goal, ResourceLineCells);

		if (cells * damage * 100 < pFoot->Health * ResourceDetourHealthPercent)
			return nullptr;

		return GroundRouteHop(pFoot, goal, ResourceDetourHopSteps, true);
	}

	// The next leg of the way around a narrow passage that stayed blocked (JamRerouteFrames), for a
	// ground unit whose way leads past the spot: within JamRerouteReachCells of it, the goal beyond
	// it and not at the passage itself. A unit standing in the passage goes on through it. nullptr
	// when the passage is not blocked, this unit is of the half that keeps to it, or no way goes
	// around it.
	CellClass* JamRerouteHop(FootClass* pFoot, const CellStruct& goal)
	{
		auto const pHouse = pFoot ? pFoot->Owner : nullptr;
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		if (!pHouse || !pType || pFoot->IsInAir() || pType->Naval || pType->MovementZone == MovementZone::Water
			|| pType->MovementZone == MovementZone::WaterBeach)
		{
			return nullptr;
		}

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now >= macro.JamRerouteUntil || macro.JamRerouteSpot == CellStruct::Empty
			|| (now >= macro.JamRerouteNoWayFrame && now - macro.JamRerouteNoWayFrame < JamRerouteRetryFrames)
			|| (!macro.JamRerouteAll && (pFoot->UniqueID & 1) != 0))
		{
			return nullptr;
		}

		const CellStruct spot = macro.JamRerouteSpot;
		const CellStruct here = pFoot->GetMapCoords();
		const double toSpot = here.DistanceFrom(spot);
		const double toGoal = here.DistanceFrom(goal);

		if (toSpot > JamRerouteReachCells || toSpot >= toGoal || spot.DistanceFrom(goal) >= toGoal || spot.DistanceFrom(goal) <= JamAvoidCells
			|| (toSpot <= JamAvoidCells && IsNarrowPassage(here)))
		{
			return nullptr;
		}

		auto const pHop = GroundRouteHop(pFoot, goal, JamRerouteHopSteps, false, &spot);

		if (!pHop && LastGroundRouteAnswer == NeuralRouteAnswer::NoRoute)
		{
			macro.JamRerouteNoWayFrame = now;
			Debug::Log("[NeuralAI] Frame %d | House #%d no way around the blocked passage at (%d,%d) for %s from (%d,%d) to (%d,%d)\n",
				now, pHouse->ArrayIndex, spot.X, spot.Y, pType->ID, here.X, here.Y, goal.X, goal.Y);
		}

		return pHop;
	}

	// A wounded unit that resource cells heal stops on one near its way before it goes on with its
	// order. True while it stays there (or walks to it): the order is not given.
	bool HoldsToHealOnResources(FootClass* pFoot, const CellStruct& goal)
	{
		auto const pType = pFoot->GetTechnoType();

		if (!pFoot->Owner || !HealsOnResourceCells(pType) || pFoot->IsInAir())
			return false;

		auto& since = GetMacroState(pFoot->Owner).ResourceHealSince;
		const int now = Unsorted::CurrentFrame;
		const double health = pFoot->GetHealthPercentage();
		bool started = false;

		if (since.size() > 512)
		{
			std::erase_if(since, [now](const auto& entry)
				{ return now < entry.second || now - entry.second >= ResourceHealMaxFrames + ResourceHealCooldownFrames; });
		}

		if (auto const it = since.find(pFoot->UniqueID); it != since.end())
		{
			const int elapsed = now - it->second;

			if (elapsed < 0 || elapsed >= ResourceHealMaxFrames + ResourceHealCooldownFrames)
				since.erase(it);
			else if (elapsed >= ResourceHealMaxFrames || health >= ResourceHealUntil)
				return false;
			else
				started = true;
		}

		if (!started && health >= ResourceHealBelow)
			return false;

		// The cell it stands on, else the nearest resource cell of its ground that does not take it
		// away from its goal.
		const CellStruct here = pFoot->GetMapCoords();
		CellClass* pHealCell = MapClass::Instance.TryGetCellAt(here);
		const bool standsOnOne = pHealCell && pHealCell->GetContainedTiberiumIndex() >= 0;

		if (!standsOnOne)
		{
			pHealCell = nullptr;
			const int zone = ObjectZone(pFoot, pType->MovementZone);
			const double hereToGoal = here.DistanceFrom(goal);

			for (int r = 1; r <= ResourceHealSearchCells && !pHealCell; ++r)
			{
				for (int dy = -r; dy <= r && !pHealCell; ++dy)
				{
					for (int dx = -r; dx <= r && !pHealCell; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != r)
							continue;

						const CellStruct cell { static_cast<short>(here.X + dx), static_cast<short>(here.Y + dy) };
						auto const pCell = MapClass::Instance.TryGetCellAt(cell);

						if (pCell && pCell->GetContainedTiberiumIndex() >= 0 && cell.DistanceFrom(goal) <= hereToGoal + 4.0
							&& MapClass::Instance.GetMovementZoneType(cell, pType->MovementZone, false) == zone)
						{
							pHealCell = pCell;
						}
					}
				}
			}
		}

		if (!pHealCell)
		{
			since.erase(pFoot->UniqueID);
			return false;
		}

		if (!started)
		{
			since[pFoot->UniqueID] = now;
			CountDecision(NeuralDecision::ResourceHealStop, pFoot->Owner->ArrayIndex);
		}

		if (standsOnOne)
		{
			if (pFoot->MegaMission == Mission::AttackMove || pFoot->CurrentMission == Mission::Move)
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetDestination(nullptr, true);
				pFoot->QueueMission(Mission::Guard, true);
			}
		}
		else
		{
			pFoot->ClearMegaMissionData();
			IssueMoveOrder(pFoot, pHealCell, 0.5);
		}

		return true;
	}

	// A unit that needs no transport over water or cliffs: amphibious, hovering, flying, burrowing,
	// teleporting, or a ship.
	bool CrossesWaterAlone(const TechnoTypeClass* pType)
	{
		if (!pType)
			return false;

		return pType->MovementZone == MovementZone::Amphibious || pType->MovementZone == MovementZone::AmphibiousDestroyer
			|| pType->MovementZone == MovementZone::AmphibiousCrusher || pType->MovementZone == MovementZone::Fly
			|| pType->MovementZone == MovementZone::Subterrannean || pType->MovementZone == MovementZone::Water
			|| pType->SpeedType == SpeedType::Hover || pType->SpeedType == SpeedType::Winged || pType->JumpJet || pType->Teleporter
			|| pType->Naval;
	}

	// Does the unit get to this cell by itself? Yes when it crosses water alone, when the cell is on
	// its ground by a vehicle's movement rules or by its own (IsSameGroundForFoot), or when the
	// building that stands there can be reached from its ground. A unit on a cell of no ground zone
	// (a factory exit, a bridge ramp) is not told apart, so it counts as able. A ship gets to a land
	// target only as far as it hits it from its own sea (NavalReaches): every ship counted for an
	// inland wave target, so the wave went out short of the units that could get there (match #9846).
	bool FootReachesAlone(const FootClass* pFoot, const CellStruct& cell, const BuildingClass* pBuilding = nullptr)
	{
		if (pFoot && IsWaterBound(pFoot->GetTechnoType()))
			return NavalReaches(pFoot, cell, pBuilding);

		if (!pFoot || CrossesWaterAlone(pFoot->GetTechnoType()))
			return true;

		const int footZone = ObjectZone(pFoot, MovementZone::Normal);

		if (footZone <= 1)
			return true;

		int cellZone = MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false);

		if (cellZone <= 1)
		{
			auto const pCell = MapClass::Instance.TryGetCellAt(cell);

			if (pCell && (pCell->ContainsBridge() || pCell->ContainsBridgeEx()))
				cellZone = MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, true);
		}

		return cellZone == footZone || IsSameGroundForFoot(pFoot, cell) || IsBuildingEnterableFromZone(pBuilding, footZone);
	}

	void OrderFootToAttackMove(FootClass* pFoot, AbstractClass* pDest, const char* callerFile = __builtin_FILE(), int callerLine = __builtin_LINE())
	{
		if (!pFoot || !pDest || pFoot->InLimbo || !pFoot->IsAlive)
			return;

		// A transport parked to load stays where its passengers go; a unit of a transport operation
		// takes its orders from the operation alone; a ship that waits outside a crowded narrow passage
		// stays where it is.
		if ((pFoot->Owner && IsFerryParkedToLoad(pFoot->Owner, pFoot->UniqueID)) || IsHeldByTransportOp(pFoot)
			|| IsWaitingAtNavalPassage(pFoot))
		{
			return;
		}

		// Another code holds the unit for its own goal (order leases).
		if (!MayOrderUnit(pFoot, CellClass::Coord2Cell(pDest->GetCoords()), callerFile, callerLine))
			return;

		AbstractClass* pResolvedDest = pDest;
		CellClass* pDestCell = (pDest->WhatAmI() == AbstractType::Cell)
			? static_cast<CellClass*>(pDest)
			: MapClass::Instance.TryGetCellAt(CellClass::Coord2Cell(pDest->GetCoords()));

		if (pDestCell && HoldsToHealOnResources(pFoot, pDestCell->MapCoords))
			return;

		// Long-range units (ground weapon or drone launcher reaching >= 8 cells) sent against an
		// enemy building attack it directly, like a player's attack order: the engine stops them
		// at their maximum range. An attack-move to the building cell would drive them right
		// into the enemy defenses (e.g. a drone carrier hovering on top of its target).
		{
			BuildingClass* pEnemyBld = abstract_cast<BuildingClass*>(pDest);

			if (!pEnemyBld && pDestCell)
				pEnemyBld = pDestCell->GetBuilding();

			auto const pFootType = pFoot->GetTechnoType();
			int longestGroundRange = 0;

			for (int i = 0; pFootType && i < 2; ++i)
			{
				auto const pWS = pFootType->GetWeapon(i);
				auto const pW = pWS ? pWS->WeaponType : nullptr;

				if (pW && (pW->Spawner
					|| (pW->Damage > 0 && LandTargetingAllows(pFootType, i) && (!pW->Projectile || pW->Projectile->AG))))
					longestGroundRange = std::max(longestGroundRange, pW->Range);
			}

			// Only from near its target, and never for a unit that already stands held with it: an attack
			// order on a building across the map leaves the unit where the engine path finder gives up,
			// still on its Attack mission, so nothing takes it for idle and nothing moves it (match #8533,
			// No Rest for the Wicked: howitzers and V3 launchers standing in the three-cell necks of the
			// bridges, both armies shut in behind them). From afar it travels like the rest of the wave
			// (attack-move, route legs when held).
			const bool nearEnough = pEnemyBld
				&& pFoot->DistanceFrom(pEnemyBld) <= longestGroundRange + LongRangeDirectAttackCells * Unsorted::LeptonsPerCell;
			const bool heldWithIt = pFoot->Owner && IsFootHeldStill(pFoot->Owner, pFoot, LongRangeHeldFrames);

			if (pEnemyBld && pEnemyBld->IsAlive && !pEnemyBld->InLimbo && pEnemyBld->Owner && pFoot->Owner
				&& pEnemyBld->Owner != pFoot->Owner && !pFoot->Owner->IsAlliedWith(pEnemyBld->Owner)
				&& longestGroundRange >= 8 * Unsorted::LeptonsPerCell && nearEnough && !heldWithIt
				&& !IsOutOfNavalReach(pFoot, pEnemyBld->GetMapCoords(), longestGroundRange))
			{
				if (pFoot->Target != pEnemyBld)
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetTarget(pEnemyBld);
					pFoot->QueueMission(Mission::Attack, true);
					CountDecision(NeuralDecision::MoveOrderIssued, pFoot->Owner->ArrayIndex);
					NoteOrderCaller(pFoot, "attackBuilding", callerFile, callerLine);
				}
				else
				{
					CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner->ArrayIndex);
				}

				return;
			}
		}

		if (pDestCell && pDestCell->GetBuilding() != nullptr)
		{
			const CellStruct fc = pFoot->GetMapCoords();
			const CellStruct dc = pDestCell->MapCoords;
			CellClass* pBestAdj = nullptr;
			int bestDist = 999999;
			// A cell the unit can stand on first (its own zone, clear for it): the engine finds no path
			// to water or a cliff next to the building. Any free cell otherwise (a target on another
			// landmass, for the units that cross water).
			auto const pAdjType = pFoot->GetTechnoType();
			const int adjZone = pAdjType ? ObjectZone(pFoot, pAdjType->MovementZone) : -1;

			for (int pass = (adjZone > 1 ? 0 : 1); pass < 2 && !pBestAdj; ++pass)
			{
				for (int r = 1; r <= 4 && !pBestAdj; ++r)
				{
					for (int dy = -r; dy <= r; ++dy)
					{
						for (int dx = -r; dx <= r; ++dx)
						{
							if (std::abs(dx) != r && std::abs(dy) != r)
								continue;

							const CellStruct cc { static_cast<short>(dc.X + dx), static_cast<short>(dc.Y + dy) };
							if (auto const pC = MapClass::Instance.TryGetCellAt(cc))
							{
								if (pC->GetBuilding() != nullptr)
									continue;

								if (pC->LandType == LandType::Rock || pC->LandType == LandType::Wall)
									continue;

								if (pass == 0 && (MapClass::Instance.GetMovementZoneType(cc, pAdjType->MovementZone, false) != adjZone
									|| !pC->IsClearToMove(pAdjType->SpeedType, true, true, -1, pAdjType->MovementZone, -1, false)))
								{
									continue;
								}

								const int d = std::abs(cc.X - fc.X) + std::abs(cc.Y - fc.Y);
								if (d < bestDist)
								{
									bestDist = d;
									pBestAdj = pC;
								}
							}
						}
					}
				}
			}

			if (pBestAdj)
			{
				pDestCell = pBestAdj;
				if (pDest->WhatAmI() == AbstractType::Cell)
					pResolvedDest = pBestAdj;
			}
		}

		// Ships cannot attack-move onto land: the nearest water cell of their own body instead.
		if (pDestCell)
		{
			if (auto const pReach = NearestReachableNavalCell(pFoot, pDestCell->MapCoords))
			{
				pDestCell = pReach;
				pResolvedDest = pReach;
			}
			else if (IsOutOfNavalReach(pFoot, pDestCell->MapCoords, INT_MAX))
			{
				// A target in another body of water with none of ours near it: the ship can never get
				// there (20 ships shut in a lake took 1022 such orders in one match, each dropped by the
				// stuck watchdog 450 frames later and given again).
				CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
				return;
			}

			// A far naval order goes in hops of 25 cells along a sea route (NavalRouteHop); the owner
			// logic sends it again once the ship is there. A far ground order likewise along a land route
			// (GroundRouteHop): the engine path finder gives up on long winding ways.
			auto const stepRouteHop = [&]() -> CellClass*
				{
					// Only as a fallback: a unit that has not left its cell for 300 frames (the engine
					// path finder gave up on the long way), and then for 1200 frames after its last hop
					// so it does not fall back to the long order between legs. Where the engine finds its
					// way the order stays whole: hops everywhere made more path failures on maps it
					// handled (fs_duel).
					static std::unordered_map<DWORD, int> hoppingSince;
					const int nowFrame = Unsorted::CurrentFrame;

					if (hoppingSince.size() > 2048)
						std::erase_if(hoppingSince, [nowFrame](const auto& e) { return nowFrame < e.second || nowFrame - e.second > 1200; });

					auto const itHop = hoppingSince.find(pFoot->UniqueID);
					const bool hopping = itHop != hoppingSince.end() && nowFrame >= itHop->second && nowFrame - itHop->second <= 1200;
					const bool held = pFoot->Owner && IsFootHeldStill(pFoot->Owner, pFoot, 300);

					if (!hopping && !held)
						return nullptr;

					// A route is only given past its leg plus 10 cells, so a goal nearer than that takes
					// shorter legs (ships held in a row 26 cells from their goal got no route at all).
					const double goalDist = pFoot->GetMapCoords().DistanceFrom(pDestCell->MapCoords);
					const int hopSteps = goalDist > 35.0 ? 25 : (goalDist > 22.0 ? 12 : 6);
					auto const pHopCell = GroundRouteHop(pFoot, pDestCell->MapCoords, hopSteps);

					if (pHopCell)
						hoppingSince[pFoot->UniqueID] = nowFrame;

					return pHopCell;
				};

			// A ship that has not left its cell takes the cell by cell route first. The sea route only
			// follows the zone map: it runs through a channel one of our buildings closed, and its legs
			// are measured in a straight line, so one can end behind a headland the engine does not
			// path around (match #8133, Nora Island: nine Sea Scorpions in a row for the whole match).
			auto const pHopType = pFoot->GetTechnoType();
			const bool ship = pHopType && (pHopType->Naval || pHopType->MovementZone == MovementZone::Water
				|| pHopType->MovementZone == MovementZone::WaterBeach);
			// A unit whose way leads through a passage that stayed blocked goes around it; one that
			// resource cells hurt goes around a field that lies across its way.
			CellClass* const pJamHop = JamRerouteHop(pFoot, pDestCell->MapCoords);
			CellClass* const pDetourHop = pJamHop ? pJamHop : ResourceDetourHop(pFoot, pDestCell->MapCoords);
			CellClass* pGroundHop = pDetourHop ? pDetourHop : (ship ? stepRouteHop() : nullptr);
			CellClass* const pSeaHop = pGroundHop ? nullptr : NavalRouteHop(pFoot, pDestCell->MapCoords, 25);

			if (!pGroundHop && !pSeaHop && !ship)
				pGroundHop = stepRouteHop();

			if (pSeaHop)
			{
				pDestCell = pSeaHop;
				pResolvedDest = pSeaHop;
			}
			else if (pGroundHop)
			{
				// Already attack-moving to an earlier hop that is still 6+ cells away and closer to the
				// goal than the unit: it keeps that leg (a new hop every few cells re-paths it each time).
				const CellStruct goal = pDestCell->MapCoords;
				const CellStruct here = pFoot->GetMapCoords();

				// A leg around a field is short and need not be nearer to the goal: it is kept while it is
				// off the resource cells and still ahead. So is a leg around a blocked passage, while it
				// is another cell than the goal and within a leg's length.
				if (auto const pOldHop = abstract_cast<CellClass*>(pFoot->MegaDestination);
					pOldHop && pFoot->MegaMission == Mission::AttackMove
					&& (pJamHop
						? (here.DistanceFrom(pOldHop->MapCoords) > 6.0
							&& here.DistanceFrom(pOldHop->MapCoords) <= JamRerouteHopSteps + 6.0
							&& pOldHop->MapCoords.DistanceFrom(goal) > 3.0
							&& !(pOldHop->MapCoords.DistanceFrom(GetMacroState(pFoot->Owner).JamRerouteSpot) <= JamAvoidCells
								&& IsNarrowPassage(pOldHop->MapCoords)))
						: pDetourHop
						? (here.DistanceFrom(pOldHop->MapCoords) > 3.0
							&& here.DistanceFrom(pOldHop->MapCoords) <= ResourceDetourHopSteps + 4.0
							&& pOldHop->GetContainedTiberiumIndex() < 0)
						: (here.DistanceFrom(pOldHop->MapCoords) > 6.0
							&& pOldHop->MapCoords.DistanceFrom(goal) + 5.0 < here.DistanceFrom(goal))))
				{
					CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
					return;
				}

				pDestCell = pGroundHop;
				pResolvedDest = pGroundHop;
				CountDecision((pDetourHop && !pJamHop) ? NeuralDecision::ResourceDetour : NeuralDecision::GroundRouteHop,
					pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
			}
		}

		// A cell another unit of ours already holds or heads to (a wave's shore cell, its target, a route
		// leg): the nearest free one instead, so the wave does not queue for one cell. A unit already
		// heading to that shared cell takes its own one now (the hysteresis below would keep it).
		CellClass* pSharedCell = nullptr;

		if (pResolvedDest->WhatAmI() == AbstractType::Cell)
		{
			auto const pAsked = static_cast<CellClass*>(pResolvedDest);
			pDestCell = SpreadDestination(pFoot, pAsked);
			pResolvedDest = pDestCell;

			if (pDestCell != pAsked)
				pSharedCell = pAsked;
		}

		// Hysteresis: a unit already attack-moving to (almost) the same place keeps its order.
		// Re-issuing it resets the target, re-paths and restarts the locomotor (ships slow down
		// and their wake animation restarts), so recomputed cells within 3 cells are ignored.
		{
			AbstractClass* pNewMegaDest = (pResolvedDest->WhatAmI() == AbstractType::Cell) ? pResolvedDest : pDestCell;
			auto const pOldMegaCell = abstract_cast<CellClass*>(pFoot->MegaDestination);
			auto const pNewMegaCell = abstract_cast<CellClass*>(pNewMegaDest);
			const bool stillAttackMoving = pFoot->MegaMission == Mission::AttackMove
				&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::Attack
					|| pFoot->HaveAttackMoveTarget);
			const AbstractClass* pNewMegaTarget = (pResolvedDest->WhatAmI() == AbstractType::Cell) ? nullptr : pResolvedDest;

			const bool sameOrder = pFoot->MegaMission == Mission::AttackMove && pOldMegaCell && pNewMegaCell
				&& pFoot->MegaTarget == pNewMegaTarget && pOldMegaCell->MapCoords.DistanceFrom(pNewMegaCell->MapCoords) <= 3.0
				&& pOldMegaCell != pSharedCell;

			if (stillAttackMoving && sameOrder)
			{
				CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
				return;
			}

			// The engine left it waiting with that same order pending (it found no path, or the way is
			// blocked). Given again every AI tick the order makes the engine search the same path every
			// time and the unit shuffle in place (match #8533: 448000 orders for one house, 1.4 million
			// failed path searches), so it is given again only every AttackMoveRetryFrames. Key access only.
			static std::unordered_map<DWORD, int> lastIssued;
			const int nowFrame = Unsorted::CurrentFrame;

			if (lastIssued.size() > 4096)
			{
				std::erase_if(lastIssued, [nowFrame](const auto& entry)
					{ return nowFrame < entry.second || nowFrame - entry.second >= AttackMoveRetryFrames; });
			}

			auto const itIssued = lastIssued.find(pFoot->UniqueID);

			if (sameOrder && itIssued != lastIssued.end() && nowFrame >= itIssued->second
				&& nowFrame - itIssued->second < AttackMoveRetryFrames)
			{
				CountDecision(NeuralDecision::MoveOrderSkipped, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
				return;
			}

			lastIssued[pFoot->UniqueID] = nowFrame;
		}

		CountDecision(NeuralDecision::MoveOrderIssued, pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
		pFoot->SetTarget(nullptr);
		pFoot->SetArchiveTarget(nullptr);
		pFoot->MegaMission = Mission::AttackMove;

		if (pResolvedDest->WhatAmI() == AbstractType::Cell)
		{
			pFoot->MegaDestination = pResolvedDest;
			pFoot->MegaTarget = nullptr;
		}
		else
		{
			pFoot->MegaDestination = pDestCell;
			pFoot->MegaTarget = pResolvedDest;
		}

		pFoot->HaveAttackMoveTarget = false;
		pFoot->SetDestination(pFoot->MegaDestination ? pFoot->MegaDestination : pResolvedDest, true);
		if (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
			|| pFoot->CurrentMission == Mission::None || pFoot->CurrentMission == Mission::Sleep)
		{
			pFoot->ForceMission(Mission::Move);
		}
		else
		{
			pFoot->QueueMission(Mission::Move, true);
		}

		NoteOrderCaller(pFoot, "attackMove", callerFile, callerLine);
	}

	bool HasActiveBoardingPassenger(TechnoClass* pTrans, int transZone)
	{
		if (!pTrans)
			return false;

		if (pTrans->HasAnyLink())
		{
			if (auto const pLinked = pTrans->GetNthLink(0))
			{
				if (pLinked->IsAlive && !pLinked->InLimbo && pLinked->Owner == pTrans->Owner
					&& (pLinked->CurrentMission == Mission::Enter || pLinked->QueuedMission == Mission::Enter))
				{
					return true;
				}
			}
		}

		const CellStruct tc = pTrans->GetMapCoords();

		for (auto const pU : UnitClass::Array)
		{
			if (!pU || pU == pTrans || pU->Owner != pTrans->Owner || pU->InLimbo || !pU->IsAlive)
				continue;

			if (pU->CurrentMission != Mission::Enter && pU->QueuedMission != Mission::Enter)
				continue;

			if (pU->ContainsLink(pTrans) || pU->ArchiveTarget == pTrans || pU->Destination == pTrans)
				return true;

			if (pU->Destination != nullptr && pU->GetMapCoords().DistanceFrom(tc) <= 6.0
				&& CellClass::Coord2Cell(pU->Destination->GetCoords()).DistanceFrom(tc) <= 3.0)
			{
				return true;
			}
		}

		for (auto const pI : InfantryClass::Array)
		{
			if (!pI || pI->Owner != pTrans->Owner || pI->InLimbo || !pI->IsAlive)
				continue;

			if (pI->CurrentMission != Mission::Enter && pI->QueuedMission != Mission::Enter)
				continue;

			if (pI->ContainsLink(pTrans) || pI->ArchiveTarget == pTrans || pI->Destination == pTrans)
				return true;

			if (pI->Destination != nullptr && pI->GetMapCoords().DistanceFrom(tc) <= 6.0
				&& CellClass::Coord2Cell(pI->Destination->GetCoords()).DistanceFrom(tc) <= 3.0)
			{
				return true;
			}
		}

		return false;
	}

	bool ClearStaleEnterOrUnreachableCrossIslandMission(FootClass* pFoot, bool canCrossWaterDirectly)
	{
		if (!pFoot || pFoot->InLimbo || !pFoot->IsAlive)
			return false;

		const auto pFootType = pFoot->GetTechnoType();

		if (!pFootType)
			return false;

		const CellStruct footCoords = pFoot->GetMapCoords();
		const int fz = MapClass::Instance.GetMovementZoneType(footCoords, MovementZone::Normal, pFoot->OnBridge);

		if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
		{
			TechnoClass* pEnterT = pFoot->HasAnyLink()
				? pFoot->GetNthLink(0)
				: abstract_cast<TechnoClass*>(pFoot->ArchiveTarget ? pFoot->ArchiveTarget : pFoot->Destination);
			bool validEnter = false;

			if (pEnterT && pEnterT->IsAlive && !pEnterT->InLimbo)
			{
				const auto pEnterType = pEnterT->GetTechnoType();
				const int ez
					= ObjectZone(pEnterT, MovementZone::Normal);

				if (pEnterT->WhatAmI() == AbstractType::Building)
				{
					if (auto const pEnterBld = static_cast<BuildingClass*>(pEnterT))
					{
						const bool sameLandmass = (canCrossWaterDirectly || fz < 0 || ez < 0 || ez == fz);

						if (pEnterBld->Type && pEnterBld->Type->Bunker)
						{
							validEnter = sameLandmass
								&& (pEnterBld->BunkerLinkedItem == nullptr || pEnterBld->BunkerLinkedItem == pFoot);
						}
						else if (pEnterBld->Type && pEnterBld->Type->CanBeOccupied)
						{
							const int maxOcc = std::clamp(pEnterBld->Type->MaxNumberOccupants, 1, 10);
							validEnter = sameLandmass && pEnterBld->Occupants.Count < maxOcc;
						}
						else
						{
							validEnter = sameLandmass;
						}
					}
				}
				else if (pEnterType && pEnterType->Passengers > 0)
				{
					const bool hasRoom = CanPassengerFitInTransport(pEnterT, pFootType);
					const auto pECell = pEnterT->GetCell();
					const bool reachableShore = canCrossWaterDirectly || (fz >= 0 && ez == fz)
						|| pFoot->ContainsLink(pEnterT)
						|| (pECell && pECell->LandType == LandType::Water && pFoot->DistanceFrom(pEnterT) <= 1280);
					validEnter = hasRoom && reachableShore;
				}
			}

			if (validEnter)
				return false;

			if (pFoot->HasAnyLink())
				pFoot->SendToFirstLink(RadioCommand::NotifyUnlink);

			pFoot->ClearMegaMissionData();
			pFoot->SetArchiveTarget(nullptr);
			pFoot->SetDestination(nullptr, true);
			pFoot->ClearNavigationList();
			pFoot->ForceMission(Mission::Guard);

			return true;
		}

		// A ship sails where these land tests see water and another zone: they stopped every ship on
		// its way to a water cell at every AI tick (the wave loop passes "cannot cross water" for
		// the Water movement zone), so fleets crawled a cell every 50 frames or never left their
		// yard (Naval Strike: 48 of 93 plans out of time 50 to 200 cells from the target).
		const bool sails = pFootType->Naval || pFootType->MovementZone == MovementZone::Water
			|| pFootType->MovementZone == MovementZone::WaterBeach;

		if (!canCrossWaterDirectly && !sails && fz >= 0
			&& (pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::AttackMove
				|| pFoot->MegaMission == Mission::AttackMove || pFoot->CurrentMission == Mission::Hunt
				|| pFoot->CurrentMission == Mission::Attack))
		{
			if (pFoot->Target == nullptr
				&& (pFoot->CurrentMission == Mission::Hunt || pFoot->CurrentMission == Mission::AttackMove
					|| pFoot->MegaMission == Mission::AttackMove))
			{
				bool reachedDest = (pFoot->Destination == nullptr || pFoot->DistanceFrom(pFoot->Destination) <= 384);

				if (!reachedDest && pFoot->Owner)
				{
					bool anyEnemyInZone = false;

					for (auto const pEB : BuildingClass::Array)
					{
						if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type
							|| pEB->Type->Insignificant || pEB->Type->InvisibleInGame)
						{
							continue;
						}

						if (pEB->Owner == pFoot->Owner || pFoot->Owner->IsAlliedWith(pEB->Owner) || !IsEnemyBuildingKnown(pFoot->Owner, pEB)
							|| pEB->Owner->IsNeutral() || pEB->Owner->IsObserver() || pEB->Owner->Defeated)
						{
							continue;
						}

						if (ObjectZone(pEB, MovementZone::Normal)
							== fz)
						{
							anyEnemyInZone = true;
							break;
						}
					}

					if (!anyEnemyInZone)
					{
						for (auto const pEU : UnitClass::Array)
						{
							if (!pEU || pEU->InLimbo || !pEU->IsAlive || !pEU->Owner || !pEU->Type || pEU->Type->Naval)
								continue;

							if (pEU->Owner == pFoot->Owner || pFoot->Owner->IsAlliedWith(pEU->Owner) || !IsCellVisibleToAI(pFoot->Owner, pEU->GetCell())
								|| pEU->Owner->IsNeutral() || pEU->Owner->IsObserver() || pEU->Owner->Defeated)
							{
								continue;
							}

							if (ObjectZone(pEU, MovementZone::Normal)
								== fz)
							{
								anyEnemyInZone = true;
								break;
							}
						}
					}

					if (!anyEnemyInZone)
						reachedDest = true;
				}

				if (reachedDest)
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}

			// Walking to one of our transports is no order into the water, wherever the transport
			// floats: its loader takes it from there.
			auto const pDestTechno = abstract_cast<TechnoClass*>(pFoot->Destination);
			const bool boundForTransport = pDestTechno && pDestTechno->Owner && pFoot->Owner
				&& (pDestTechno->Owner == pFoot->Owner || pFoot->Owner->IsAlliedWith(pDestTechno->Owner))
				&& pDestTechno->GetTechnoType()->Passengers > 0;

			if (pFoot->Destination != nullptr && pFoot->Target == nullptr && !boundForTransport)
			{
				const CellStruct dc = CellClass::Coord2Cell(pFoot->Destination->GetCoords());
				const int dz = MapClass::Instance.GetMovementZoneType(dc, MovementZone::Normal, false);
				auto const pDCell = MapClass::Instance.TryGetCellAt(dc);

				if ((dz >= 0 && dz != fz && !IsSameGroundForFoot(pFoot, dc)) || (pDCell && pDCell->LandType == LandType::Water))
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}

			if (pFoot->Target != nullptr)
			{
				const CellStruct tc = CellClass::Coord2Cell(pFoot->Target->GetCoords());
				const int tz = MapClass::Instance.GetMovementZoneType(tc, MovementZone::Normal, false);
				int wpnRange = 1280;

				if (pFootType->GetWeapon(0) && pFootType->GetWeapon(0)->WeaponType)
					wpnRange = std::max(wpnRange, pFootType->GetWeapon(0)->WeaponType->Range);

				if (tz >= 0 && tz != fz && pFoot->DistanceFrom(pFoot->Target) > (wpnRange + 512))
				{
					pFoot->ClearMegaMissionData();
					pFoot->SetTarget(nullptr);
					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);

					return true;
				}
			}
		}

		return false;
	}

	bool IsTrueBaseStructureTarget(const BuildingTypeClass* pTargetBld)
	{
		if (!pTargetBld)
			return false;

		return (pTargetBld->ConstructionYard || pTargetBld->Refinery || pTargetBld->ResourceGatherer
			|| pTargetBld->ResourceDestination || pTargetBld->Factory != AbstractType::None
			|| pTargetBld->PowerBonus > pTargetBld->PowerDrain || pTargetBld->BaseNormal || pTargetBld->IsBaseDefense
			|| pTargetBld->Radar || pTargetBld->SpySat || pTargetBld->UnitRepair || pTargetBld->SuperWeapon != -1
			|| pTargetBld->Storage > 0 || pTargetBld->Grinding || pTargetBld->Cloning || pTargetBld->OrePurifier
			|| pTargetBld->FactoryPlant || pTargetBld->SecretLab || pTargetBld->Hospital || pTargetBld->Armory
			|| (pTargetBld->TechLevel >= 0 && pTargetBld->TechLevel <= 10));
	}

	CellClass* FindSafeIslandLandingCellUncached(
		HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone, bool preferOre);

	// One search per house, zone, ore preference and square of LandingCacheGridCells around the
	// island reference, kept LandingCacheFrames. The search walks thousands of cells; with a cache
	// of one frame keyed by the exact cell it ran for every enemy building and every unit that
	// asked from its own cell, 3.5 times per AI tick, and took 23% of the whole tick (tick cost
	// diagnostic, 2026-11-13).
	constexpr int LandingCacheGridCells = 8;
	// 150 frames: with 45 the search still ran about once per AI tick, 11% of it.
	constexpr int LandingCacheFrames = 150;

	CellClass* FindSafeIslandLandingCell(
		HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone, bool preferOre)
	{
		struct LandingAnswer
		{
			CellStruct Cell;
			int Frame;
		};

		static int cacheSerial = -1;
		static std::unordered_map<uint64_t, LandingAnswer> cache;
		const int now = Unsorted::CurrentFrame;

		if (cacheSerial != NeuralMatchSerial)
		{
			cacheSerial = NeuralMatchSerial;
			cache.clear();
		}

		if (cache.size() > 1024)
			std::erase_if(cache, [now](const auto& entry) { return now < entry.second.Frame || now - entry.second.Frame >= LandingCacheFrames; });

		const uint64_t key = (static_cast<uint64_t>(static_cast<uint8_t>(pHouse ? pHouse->ArrayIndex : 255)) << 56)
			| (static_cast<uint64_t>(static_cast<uint16_t>(targetIslandZone)) << 40)
			| (static_cast<uint64_t>(static_cast<uint16_t>(islandRefCell.X / LandingCacheGridCells)) << 24)
			| (static_cast<uint64_t>(static_cast<uint16_t>(islandRefCell.Y / LandingCacheGridCells)) << 8) | (preferOre ? 1u : 0u);

		if (auto const it = cache.find(key); it != cache.end() && now >= it->second.Frame && now - it->second.Frame < LandingCacheFrames)
			return it->second.Cell == CellStruct::Empty ? nullptr : MapClass::Instance.TryGetCellAt(it->second.Cell);

		CellClass* const pResult = FindSafeIslandLandingCellUncached(pHouse, islandRefCell, targetIslandZone, preferOre);
		cache.insert_or_assign(key, LandingAnswer { pResult ? pResult->MapCoords : CellStruct::Empty, now });
		return pResult;
	}

	CellClass* FindSafeIslandLandingCellUncached(
		HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone, bool preferOre)
	{
		bool islandHasEnemyPresence = false;
		for (auto const pEB : BuildingClass::Array)
		{
			if (!pEB || pEB->InLimbo || !pEB->IsAlive || !pEB->Owner || !pEB->Type || pEB->Type->Insignificant
				|| pEB->Type->InvisibleInGame || pEB->Type->Naval)
			{
				continue;
			}

			if (pEB->Owner == pHouse || pHouse->IsAlliedWith(pEB->Owner) || pEB->Owner->IsNeutral()
				|| pEB->Owner->IsObserver() || pEB->Owner->Defeated || !IsEnemyBuildingKnown(pHouse, pEB))
			{
				continue;
			}

			const CellStruct ebc = pEB->GetMapCoords();
			if (targetIslandZone >= 0)
			{
				if (MapClass::Instance.GetMovementZoneType(ebc, MovementZone::Normal, pEB->OnBridge) == targetIslandZone)
				{
					islandHasEnemyPresence = true;
					break;
				}
			}
			else if (std::max(std::abs(ebc.X - islandRefCell.X), std::abs(ebc.Y - islandRefCell.Y)) <= 45)
			{
				islandHasEnemyPresence = true;
				break;
			}
		}

		// What a landing is judged by, gathered once: known enemy defenses with their real reach (weapon
		// range + 4 cells, at least 10), known enemy structures (cost, and whether a known defense
		// covers them), enemy units in sight, our transports and where they are going.
		struct LandingDefense { CellStruct Cell; int Reach; };
		struct LandingStructure { CellStruct Cell; int Value; bool Covered; };
		std::vector<LandingDefense> landingDefenses;
		std::vector<LandingStructure> landingStructures;
		std::vector<CellStruct> landingEnemyUnits;
		std::vector<CellStruct> landingOwnTransports;
		std::vector<CellStruct> landingOwnTransportDests;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame || pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner)
				|| pBld->Owner->IsNeutral() || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();

			if (pBld->Type->IsBaseDefense)
			{
				int rangeCells = 0;

				for (int w = 0; w < 2; ++w)
				{
					if (auto const pWeaponStruct = pBld->GetWeapon(w); pWeaponStruct && pWeaponStruct->WeaponType)
						rangeCells = std::max(rangeCells, pWeaponStruct->WeaponType->Range / 256);
				}

				landingDefenses.push_back({ bc, std::max(10, rangeCells + 4) });
			}
			else
			{
				landingStructures.push_back({ bc, std::max(100, pBld->Type->Cost), false });
			}
		}

		for (auto& structure : landingStructures)
		{
			for (auto const& defense : landingDefenses)
			{
				// Covered: inside the defense's own weapon range (its reach less the 4-cell margin).
				if (std::max(std::abs(structure.Cell.X - defense.Cell.X), std::abs(structure.Cell.Y - defense.Cell.Y))
					<= defense.Reach - 3)
				{
					structure.Covered = true;
					break;
				}
			}
		}

		for (auto const pUnitScan : UnitClass::Array)
		{
			if (!pUnitScan || pUnitScan->InLimbo || !pUnitScan->IsAlive || !pUnitScan->Owner)
				continue;

			if (pUnitScan->Owner == pHouse)
			{
				if (pUnitScan->Type && IsAmphibiousOrAirTransport(pUnitScan->Type))
				{
					landingOwnTransports.push_back(pUnitScan->GetMapCoords());

					if (pUnitScan->Destination)
						landingOwnTransportDests.push_back(CellClass::Coord2Cell(pUnitScan->Destination->GetCoords()));
				}

				continue;
			}

			if (pHouse->IsAlliedWith(pUnitScan->Owner) || pUnitScan->Owner->IsNeutral()
				|| !IsCellVisibleToAI(pHouse, pUnitScan->GetCell()))
			{
				continue;
			}

			landingEnemyUnits.push_back(pUnitScan->GetMapCoords());
		}

		// Soldiers count like vehicles, and so does what was seen there a moment ago: a cargo set
		// down next to infantry, or next to units that had just left our sight, is dead in seconds
		// (user, match #8916: a transport disk kept unloading into the defenders).
		for (auto const pInfScan : InfantryClass::Array)
		{
			if (!pInfScan || pInfScan->InLimbo || !pInfScan->IsAlive || !pInfScan->Owner || pInfScan->Owner == pHouse
				|| pHouse->IsAlliedWith(pInfScan->Owner) || pInfScan->Owner->IsNeutral() || !IsCellVisibleToAI(pHouse, pInfScan->GetCell()))
			{
				continue;
			}

			landingEnemyUnits.push_back(pInfScan->GetMapCoords());
		}

		for (auto const& [seenID, sighting] : GetMacroState(pHouse).EnemySightings)
		{
			if (sighting.LastCoords != CellStruct::Empty && Unsorted::CurrentFrame - sighting.LastSeenFrame <= FerryLandingSightingFrames)
				landingEnemyUnits.push_back(sighting.LastCoords);
		}

		CellClass* pBestLanding = nullptr;
		int bestScore = -999999;
		// The whole island: 96 cells around the reference when the island is known (big islands
		// have undefended coasts far from the target), sampled more sparsely far out.
		const int maxR = targetIslandZone >= 0 ? 96 : 64;
		auto const sampleStep = [](int d) { return std::abs(d) <= 32 ? 2 : (std::abs(d) <= 64 ? 3 : 4); };

		for (int dy = -maxR; dy <= maxR; dy += sampleStep(dy))
		{
			for (int dx = -maxR; dx <= maxR; dx += sampleStep(dx))
			{
				const CellStruct cand { static_cast<short>(islandRefCell.X + dx),
					static_cast<short>(islandRefCell.Y + dy) };
				auto const pCell = MapClass::Instance.TryGetCellAt(cand);
				if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock
					|| pCell->GetBuilding())
				{
					continue;
				}

				if (targetIslandZone >= 0)
				{
					const int cz = MapClass::Instance.GetMovementZoneType(cand, MovementZone::Normal, false);
					if (cz != targetIslandZone)
						continue;
				}

				if (!pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false)
					|| !pCell->IsClearToMove(
						SpeedType::Hover, true, true, -1, MovementZone::AmphibiousDestroyer, -1, false))
				{
					continue;
				}

				bool nearCoast = (pCell->LandType == LandType::Beach);
				if (!nearCoast)
				{
					const CellStruct neighbors[8] = { { static_cast<short>(cand.X + 1), cand.Y },
						{ static_cast<short>(cand.X - 1), cand.Y }, { cand.X, static_cast<short>(cand.Y + 1) },
						{ cand.X, static_cast<short>(cand.Y - 1) }, { static_cast<short>(cand.X + 2), cand.Y },
						{ static_cast<short>(cand.X - 2), cand.Y }, { cand.X, static_cast<short>(cand.Y + 2) },
						{ cand.X, static_cast<short>(cand.Y - 2) } };
					for (const auto& nc : neighbors)
					{
						if (auto const pNC = MapClass::Instance.TryGetCellAt(nc))
						{
							if ((pNC->LandType == LandType::Water || pNC->LandType == LandType::Beach)
								&& std::abs(static_cast<int>(pCell->Level) - static_cast<int>(pNC->Level)) <= 1)
							{
								nearCoast = true;
								break;
							}
						}
					}
				}

				if (!nearCoast)
				{
					const CellStruct ring3[4] = { { static_cast<short>(cand.X + 3), cand.Y },
						{ static_cast<short>(cand.X - 3), cand.Y }, { cand.X, static_cast<short>(cand.Y + 3) },
						{ cand.X, static_cast<short>(cand.Y - 3) } };
					for (const auto& nc3 : ring3)
					{
						if (auto const pNC3 = MapClass::Instance.TryGetCellAt(nc3))
						{
							if ((pNC3->LandType == LandType::Water || pNC3->LandType == LandType::Beach)
								&& std::abs(static_cast<int>(pCell->Level) - static_cast<int>(pNC3->Level)) <= 1)
							{
								nearCoast = true;
								break;
							}
						}
					}
				}

				if (!nearCoast)
					continue;

				const int distFromRef = std::abs(dx) + std::abs(dy);
				// On an enemy-occupied island the landing is chosen for the damage it can do: the value of
				// the enemy structures no known defense covers within 6..22 cells (an undefended coast by
				// refineries and power plants), distance to the target only weighing beyond 40 cells.
				// On a friendly / neutral island, close to islandRefCell.
				int score = 900 - distFromRef * 3;

				if (islandHasEnemyPresence)
				{
					int softValue = 0;

					for (auto const& structure : landingStructures)
					{
						const int d = std::max(std::abs(cand.X - structure.Cell.X), std::abs(cand.Y - structure.Cell.Y));

						if (!structure.Covered && d >= 6 && d <= 22)
							softValue += structure.Value;
					}

					score = 900 - std::max(0, distFromRef - 40) * 3 + std::min(2600, softValue / 3);
				}
				if (pCell->FirstObject != nullptr)
				{
					score -= 550; // Prefer an unoccupied beach cell so multiple
								// transports/retries spread out!
				}

				bool allImmediateNeighborsDryLand = (pCell->LandType != LandType::Beach);
				int emptyDryNeighbors = 0;
				for (int ndy = -1; ndy <= 1; ++ndy)
				{
					for (int ndx = -1; ndx <= 1; ++ndx)
					{
						if (ndx == 0 && ndy == 0)
							continue;

						const CellStruct adj { static_cast<short>(cand.X + ndx), static_cast<short>(cand.Y + ndy) };
						auto const pAdj = MapClass::Instance.TryGetCellAt(adj);
						if (!pAdj || pAdj->LandType == LandType::Water || pAdj->GetBuilding() != nullptr
							|| !pAdj->IsClearToMove(
								SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
						{
							allImmediateNeighborsDryLand = false;
						}
						else if (pAdj->FirstObject == nullptr)
						{
							emptyDryNeighbors++;
						}
					}
				}

				if (allImmediateNeighborsDryLand)
					score += 350;

				score += emptyDryNeighbors * 55;
				if (emptyDryNeighbors < 2)
				{
					score -= 700; // Heavily penalize congested/blocked exit cells so
								// transports shift to a clear beach zone!
				}

				if (preferOre && pCell->GetContainedTiberiumValue() > 0)
					score += 180;

				// Avoid beaches where our loaded transports were recently destroyed.
				for (auto const& [bloodyCell, bloodyFrame] : GetMacroState(pHouse).BloodyBeaches)
				{
					const double dBloody = cand.DistanceFrom(bloodyCell);

					if (dBloody <= 10.0)
						score -= 2500 - static_cast<int>(dBloody * 150.0);
				}

				// Threats: a known defense whose real reach covers the cell, a structure a defense covers
				// within 14 cells, any structure within 5 (no room to land), enemy units in sight within 11.
				// Our other transports (and where they are heading) spread the landings.
				int defenseThreat = 0;

				for (auto const& defense : landingDefenses)
				{
					const int d = std::max(std::abs(cand.X - defense.Cell.X), std::abs(cand.Y - defense.Cell.Y));

					if (d <= defense.Reach)
						defenseThreat = std::max(defenseThreat, defense.Reach + 1 - d);
				}

				int minCoveredBldDist = 9999;
				int minAnyBldDist = 9999;

				for (auto const& structure : landingStructures)
				{
					const int d = std::max(std::abs(cand.X - structure.Cell.X), std::abs(cand.Y - structure.Cell.Y));
					minAnyBldDist = std::min(minAnyBldDist, d);

					if (structure.Covered)
						minCoveredBldDist = std::min(minCoveredBldDist, d);
				}

				int minEnemyUnitDist = 9999;

				for (auto const& uc : landingEnemyUnits)
					minEnemyUnitDist = std::min(minEnemyUnitDist, static_cast<int>(std::max(std::abs(cand.X - uc.X), std::abs(cand.Y - uc.Y))));

				for (auto const& tc : landingOwnTransports)
				{
					if (tc == islandRefCell)
						continue;

					const int dTrans = std::max(std::abs(cand.X - tc.X), std::abs(cand.Y - tc.Y));

					if (dTrans <= 3)
						score -= (4 - dTrans) * 450;
				}

				for (auto const& dc : landingOwnTransportDests)
				{
					if (dc == islandRefCell)
						continue;

					const int dDest = std::max(std::abs(cand.X - dc.X), std::abs(cand.Y - dc.Y));

					if (dDest <= 2)
						score -= (3 - dDest) * 350;
				}

				if (defenseThreat > 0)
					score -= 5000 + defenseThreat * 350;
				if (minCoveredBldDist <= 14)
					score -= 3800 + (15 - minCoveredBldDist) * 260;
				if (minAnyBldDist <= 5)
					score -= 1500 + (6 - minAnyBldDist) * 200;
				if (minEnemyUnitDist <= 11)
					score -= 2400 + (12 - minEnemyUnitDist) * 200;

				// Reward completely uncontested beachheads on the island outside enemy range!
				if (islandHasEnemyPresence && defenseThreat == 0 && minCoveredBldDist >= 15 && minEnemyUnitDist >= 12)
				{
					score += 1400;
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

	// The transport types that may come to load units of the house: the one given, else those it
	// owns, else those it can build (surface: the cell must suit them all). crossing: every type that
	// may make the crossing, owned or buildable now (the operation opens on any of them). Flying ones
	// come down anywhere; the others have to stand on the cell.
	void CollectBoardingCarrierTypes(HouseClass* pHouse, const TechnoTypeClass* pCarrierType,
		std::vector<const TechnoTypeClass*>& surface, std::vector<const TechnoTypeClass*>& crossing, bool& anyFlyer)
	{
		anyFlyer = false;
		bool ownsAny = false;

		auto const flies = [](const TechnoTypeClass* pType)
		{
			return pType->WhatAmI() == AbstractType::AircraftType || pType->JumpJet || pType->ConsideredAircraft
				|| pType->MovementZone == MovementZone::Fly;
		};

		auto const add = [&](const TechnoTypeClass* pType, bool owned)
		{
			if (flies(pType))
			{
				anyFlyer = true;
				ownsAny = ownsAny || owned;
				return;
			}

			if (std::find(crossing.begin(), crossing.end(), pType) == crossing.end())
				crossing.push_back(pType);

			if (owned)
			{
				ownsAny = true;

				if (std::find(surface.begin(), surface.end(), pType) == surface.end())
					surface.push_back(pType);
			}
		};

		if (pCarrierType)
		{
			add(pCarrierType, true);
			return;
		}

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && !pUnit->InLimbo && pUnit->IsAlive && pUnit->Type
				&& IsAmphibiousOrAirTransport(pUnit->Type) && !pUnit->Type->Gunner)
			{
				add(pUnit->Type, true);
			}
		}

		for (auto const pAir : AircraftClass::Array)
		{
			if (pAir && pAir->Owner == pHouse && !pAir->InLimbo && pAir->IsAlive && pAir->Type
				&& (pAir->Type->Carryall || pAir->Type->Passengers > 0))
			{
				add(pAir->Type, true);
			}
		}

		for (auto const pType : UnitTypeClass::Array)
		{
			if (pType && pType->TechLevel >= 0 && pType->TechLevel <= pHouse->TechLevel && IsAmphibiousOrAirTransport(pType)
				&& !pType->Gunner && HouseExt::PrerequisitesMet(pHouse, pType))
			{
				add(pType, false);
			}
		}

		for (auto const pType : AircraftTypeClass::Array)
		{
			if (pType && pType->TechLevel >= 0 && pType->TechLevel <= pHouse->TechLevel
				&& (pType->Carryall || pType->Passengers > 0) && HouseExt::PrerequisitesMet(pHouse, pType))
			{
				add(pType, false);
			}
		}

		// Nothing owned: whatever comes is built, so the cell must suit what can be built.
		if (!ownsAny)
			surface = crossing;
	}

	// Can a transport the house owns, or whose prerequisites it has, carry its units from `from` to
	// `landing`: any flying one, or one that makes the crossing (CanHouseCarry; no seat, no gunner
	// transport, as CollectBoardingCarrierTypes gathers them). True with no landing.
	bool CanBoardingCarriersLink(HouseClass* pHouse, const CellStruct& from, const CellStruct& landing)
	{
		if (landing == CellStruct::Empty)
			return true;

		CarryQuery query {};
		query.From = from;
		query.To = landing;
		query.Sources = CarryOwned | CarryPrereqOnly;
		query.Rules = CarryNoGunner | CarryNoSeat | CarryAirHolds | CarryAnyFlyerIsYes | CarryLink;

		return CanHouseCarry(pHouse, query);
	}

	// Where units of a land board a transport (user: "the outskirts of the base are fine, but not
	// that far unless it is one of its expansions"): the nearest cell to them, on that land, out of
	// the base or expansion they stand at (BoardingBaseClearCells from every building of ours), with
	// room around it for them to wait on, that every surface transport type that may come can stand
	// on and one of them (or a flying one) can sail from to the landing, out of the reach of known
	// enemy defenses. Enemies in sight and beaches where our transports died count as more walk.
	// Null when no such cell lies within BoardingSearchCells. pClearance, when given, receives the
	// distance from the cell to our nearest building.
	CellClass* FindBoardingCellUncached(HouseClass* pHouse, const CellStruct& around, int zone, const CellStruct& landing,
		const TechnoTypeClass* pCarrierType, int* pClearance)
	{
		if (!pHouse || around == CellStruct::Empty || !IsGroundZone(zone))
			return nullptr;

		std::vector<const TechnoTypeClass*> surface;
		std::vector<const TechnoTypeClass*> crossing;
		bool anyFlyer = false;
		CollectBoardingCarrierTypes(pHouse, pCarrierType, surface, crossing, anyFlyer);

		const int reach = BoardingSearchCells + 12;
		auto const inReach = [&around, reach](const CellStruct& at)
		{
			return std::abs(at.X - around.X) <= reach && std::abs(at.Y - around.Y) <= reach;
		};

		// Our buildings as foundation rectangles; known enemy defenses with their reach.
		struct Rect { int X0; int Y0; int X1; int Y1; };
		struct Defense { CellStruct Cell; int Reach; };
		std::vector<Rect> ownBuildings;
		std::vector<Defense> defenses;
		std::vector<CellStruct> enemies;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame)
				continue;

			const CellStruct at = pBld->GetMapCoords();

			if (!inReach(at))
				continue;

			if (pBld->Owner == pHouse)
			{
				if (!pBld->Type->Wall && !pBld->Type->Insignificant)
				{
					ownBuildings.push_back({ at.X, at.Y, at.X + std::max(1, static_cast<int>(pBld->Type->GetFoundationWidth())) - 1,
						at.Y + std::max(1, static_cast<int>(pBld->Type->GetFoundationHeight(false))) - 1 });
				}

				continue;
			}

			if (!pBld->Type->IsBaseDefense || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral()
				|| !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			int rangeCells = 0;

			for (int w = 0; w < 2; ++w)
			{
				if (auto const pWeaponStruct = pBld->GetWeapon(w); pWeaponStruct && pWeaponStruct->WeaponType)
					rangeCells = std::max(rangeCells, pWeaponStruct->WeaponType->Range / Unsorted::LeptonsPerCell);
			}

			if (rangeCells > 0)
				defenses.push_back({ at, rangeCells + 2 });
		}

		auto const addEnemy = [&](TechnoClass* pTechno)
		{
			if (pTechno && !pTechno->InLimbo && pTechno->IsAlive && pTechno->Owner && pTechno->Owner != pHouse
				&& !pHouse->IsAlliedWith(pTechno->Owner) && !pTechno->Owner->IsNeutral() && inReach(pTechno->GetMapCoords())
				&& IsCellVisibleToAI(pHouse, pTechno->GetCell()))
			{
				enemies.push_back(pTechno->GetMapCoords());
			}
		};

		for (auto const pUnit : UnitClass::Array)
			addEnemy(pUnit);

		for (auto const pInf : InfantryClass::Array)
			addEnemy(pInf);

		auto const& macro = GetMacroState(pHouse);

		for (auto const& [seenID, sighting] : macro.EnemySightings)
		{
			if (sighting.LastCoords != CellStruct::Empty && Unsorted::CurrentFrame - sighting.LastSeenFrame <= FerryLandingSightingFrames
				&& inReach(sighting.LastCoords))
			{
				enemies.push_back(sighting.LastCoords);
			}
		}

		auto const clearanceOf = [&ownBuildings](const CellStruct& c)
		{
			int best = 9999;

			for (auto const& rect : ownBuildings)
			{
				const int dx = std::max({ 0, rect.X0 - c.X, c.X - rect.X1 });
				const int dy = std::max({ 0, rect.Y0 - c.Y, c.Y - rect.Y1 });
				best = std::min(best, std::max(dx, dy));
			}

			return best;
		};

		auto const footing = [zone](const CellStruct& c) -> CellClass*
		{
			auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

			if (!pCell || pCell->LandType == LandType::Water || pCell->LandType == LandType::Rock || pCell->GetBuilding()
				|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != zone
				|| !pCell->IsClearToMove(SpeedType::Track, true, true, -1, MovementZone::Normal, -1, false))
			{
				return nullptr;
			}

			return pCell;
		};

		auto const search = [&](int minClearance, int minRoom, int& clearanceOut) -> CellClass*
		{
			CellClass* pBest = nullptr;
			double bestScore = 1e9;

			for (int ring = 0; ring <= BoardingSearchCells && ring <= bestScore; ++ring)
			{
				for (int dy = -ring; dy <= ring; ++dy)
				{
					for (int dx = -ring; dx <= ring; ++dx)
					{
						if (std::max(std::abs(dx), std::abs(dy)) != ring)
							continue;

						const CellStruct c { static_cast<short>(around.X + dx), static_cast<short>(around.Y + dy) };
						auto const pCell = footing(c);

						if (!pCell)
							continue;

						const int clearance = clearanceOf(c);

						if (clearance < minClearance)
							continue;

						const double dist = c.DistanceFrom(around);

						if (dist >= bestScore)
							continue;

						bool covered = false;

						for (auto const& defense : defenses)
						{
							if (std::max(std::abs(c.X - defense.Cell.X), std::abs(c.Y - defense.Cell.Y)) <= defense.Reach)
							{
								covered = true;
								break;
							}
						}

						if (covered)
							continue;

						bool standsAll = true;

						for (auto const pType : surface)
						{
							if (!pCell->IsClearToMove(pType->SpeedType, true, true, -1, pType->MovementZone, -1, false))
							{
								standsAll = false;
								break;
							}
						}

						if (!standsAll)
							continue;

						bool sails = anyFlyer || crossing.empty() || landing == CellStruct::Empty;

						for (size_t i = 0; !sails && i < crossing.size(); ++i)
							sails = CanTransportTypeLink(crossing[i], c, landing);

						if (!sails)
							continue;

						int room = 0;

						for (int ry = -2; ry <= 2; ++ry)
						{
							for (int rx = -2; rx <= 2; ++rx)
							{
								if ((rx != 0 || ry != 0) && footing({ static_cast<short>(c.X + rx), static_cast<short>(c.Y + ry) }))
									++room;
							}
						}

						if (room < minRoom)
							continue;

						double score = dist;

						for (auto const& enemy : enemies)
						{
							if (std::max(std::abs(c.X - enemy.X), std::abs(c.Y - enemy.Y)) <= BoardingEnemyCells)
							{
								score += BoardingEnemyPenaltyCells;
								break;
							}
						}

						for (auto const& [bloodyCell, bloodyFrame] : macro.BloodyBeaches)
						{
							if (c.DistanceFrom(bloodyCell) <= BoardingBloodyCells)
							{
								score += BoardingBloodyPenaltyCells;
								break;
							}
						}

						if (landing != CellStruct::Empty)
							score += c.DistanceFrom(landing) * BoardingLandingWeight;

						if (pCell->FirstObject)
							score += 1.0;

						if (score < bestScore)
						{
							bestScore = score;
							pBest = pCell;
							clearanceOut = clearance;
						}
					}
				}
			}

			return pBest;
		};

		int clearance = 9999;
		CellClass* pResult = search(BoardingBaseClearCells, BoardingRoomCells, clearance);

		if (!pResult)
			pResult = search(BoardingBaseClearRelaxedCells, BoardingRoomRelaxedCells, clearance);

		if (pClearance)
			*pClearance = clearance;

		return pResult;
	}

	// One search per house, zone, carrier type, square of BoardingCacheGridCells around the units
	// and square of LandingCacheGridCells around the landing, kept BoardingCacheFrames.
	CellClass* FindBoardingCell(HouseClass* pHouse, const CellStruct& around, int zone, const CellStruct& landing,
		const TechnoTypeClass* pCarrierType, int* pClearance = nullptr)
	{
		struct BoardingAnswer
		{
			CellStruct Cell;
			int Clearance;
			int Frame;
		};

		using BoardingKey = std::tuple<int, int, int, int, int, int, const TechnoTypeClass*>;
		static int cacheSerial = -1;
		static std::map<BoardingKey, BoardingAnswer> cache;
		const int now = Unsorted::CurrentFrame;

		if (cacheSerial != NeuralMatchSerial)
		{
			cacheSerial = NeuralMatchSerial;
			cache.clear();
		}

		if (cache.size() > 512)
			std::erase_if(cache, [now](const auto& entry) { return now < entry.second.Frame || now - entry.second.Frame >= BoardingCacheFrames; });

		const BoardingKey key { pHouse ? pHouse->ArrayIndex : -1, zone, around.X / BoardingCacheGridCells, around.Y / BoardingCacheGridCells,
			landing.X / LandingCacheGridCells, landing.Y / LandingCacheGridCells, pCarrierType };

		if (auto const it = cache.find(key); it != cache.end() && now >= it->second.Frame && now - it->second.Frame < BoardingCacheFrames)
		{
			if (pClearance)
				*pClearance = it->second.Clearance;

			return it->second.Cell == CellStruct::Empty ? nullptr : MapClass::Instance.TryGetCellAt(it->second.Cell);
		}

		int clearance = 9999;
		CellClass* const pResult = FindBoardingCellUncached(pHouse, around, zone, landing, pCarrierType, &clearance);
		cache.insert_or_assign(key, BoardingAnswer { pResult ? pResult->MapCoords : CellStruct::Empty, clearance, now });

		if (pClearance)
			*pClearance = clearance;

		return pResult;
	}

	// Safe ferry routing: when the straight sea lane from a transport to its landing
	// cell crosses an enemy base or the range of its defenses, pick a detour
	// waypoint that clearly lowers the exposure. Returns CellStruct::Empty when the
	// direct lane is safe or when no detour is meaningfully safer (unavoidable).
	CellStruct FindSafeFerryWaypoint(HouseClass* pHouse, const CellStruct& fromCell, const CellStruct& toCell,
		const TechnoTypeClass* pTransType)
	{
		if (!pHouse || !pTransType || fromCell == CellStruct::Empty || toCell == CellStruct::Empty)
			return CellStruct::Empty;

		const bool isAirborneTransport = pTransType->WhatAmI() == AbstractType::AircraftType
			|| pTransType->MovementZone == MovementZone::Fly || pTransType->JumpJet || pTransType->ConsideredAircraft;
		const MovementZone zone = pTransType->MovementZone;
		// A land mover (a demolition truck or a terrorist sneaking in) detours over land and keeps
		// away from every armed enemy in sight, not only from warships.
		const bool isLandMover = !isAirborneTransport && !pTransType->Naval && zone != MovementZone::Water
			&& zone != MovementZone::WaterBeach && zone != MovementZone::Amphibious
			&& zone != MovementZone::AmphibiousCrusher && zone != MovementZone::AmphibiousDestroyer
			&& pTransType->SpeedType != SpeedType::Amphibious;

		struct FerryThreat
		{
			int X;
			int Y;
			int Radius;
			int Weight;
		};

		std::vector<FerryThreat> threats;

		auto weaponThreatRange = [isAirborneTransport](const WeaponTypeClass* pWeapon) -> int
		{
			if (!pWeapon || pWeapon->Damage <= 0)
				return 0;

			if (auto const pProj = pWeapon->Projectile)
			{
				if (isAirborneTransport ? !pProj->AA : !pProj->AG)
					return 0;
			}
			else if (isAirborneTransport)
			{
				return 0;
			}

			return pWeapon->Range / Unsorted::LeptonsPerCell;
		};

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Owner || !pBld->Type || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame)
			{
				continue;
			}

			if (pBld->Owner == pHouse || pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral()
				|| pBld->Owner->IsObserver() || pBld->Owner->Defeated || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			const auto pWS0 = pBld->Type->GetWeapon(0);
			const auto pWS1 = pBld->Type->GetWeapon(1);
			const int weaponRange = std::max(weaponThreatRange(pWS0 ? pWS0->WeaponType : nullptr),
				weaponThreatRange(pWS1 ? pWS1->WeaponType : nullptr));

			if (weaponRange > 0)
				threats.push_back({ bc.X, bc.Y, weaponRange + 2, 4 });
			else
				threats.push_back({ bc.X, bc.Y, 6, 1 }); // The enemy base footprint itself.
		}

		// Mobile threats in sight: warships for surface transports; for flying carriers every
		// anti-air vehicle, infantry and aircraft (flak tracks, rocket troopers, interceptors).
		for (auto const pEnemy : TechnoClass::Array)
		{
			if (!pEnemy || pEnemy->InLimbo || !pEnemy->IsAlive || !pEnemy->Owner
				|| pEnemy->WhatAmI() == AbstractType::Building)
			{
				continue;
			}

			auto const pEnemyType = pEnemy->GetTechnoType();

			if (!pEnemyType || (!isAirborneTransport && !isLandMover && !pEnemyType->Naval))
				continue;

			if (pEnemy->Owner == pHouse || pHouse->IsAlliedWith(pEnemy->Owner) || pEnemy->Owner->IsNeutral()
				|| !IsCellVisibleToAI(pHouse, pEnemy->GetCell()))
			{
				continue;
			}

			const auto pWS0 = pEnemyType->GetWeapon(0);
			const auto pWS1 = isAirborneTransport ? pEnemyType->GetWeapon(1) : nullptr;
			const int weaponRange = std::max(weaponThreatRange(pWS0 ? pWS0->WeaponType : nullptr),
				weaponThreatRange(pWS1 ? pWS1->WeaponType : nullptr));

			if (weaponRange > 0)
			{
				const CellStruct ec = pEnemy->GetMapCoords();
				threats.push_back({ ec.X, ec.Y, weaponRange + (isLandMover ? 3 : 1), 2 });
			}
		}

		// Where our own flyers were shot down recently (UpdateAirLossMemory): the anti-air that
		// did it may be out of sight now. Weight fades 3 -> 1 over 8100 frames.
		if (isAirborneTransport)
		{
			for (auto const& [spot, frame] : GetMacroState(pHouse).AirDangerSpots)
			{
				const int age = Unsorted::CurrentFrame - frame;

				if (age >= 0 && age < 8100)
					threats.push_back({ spot.X, spot.Y, 7, 3 - age / 2700 });
			}
		}

		if (threats.empty())
			return CellStruct::Empty;

		auto segmentThreatCost = [&threats](const CellStruct& a, const CellStruct& b) -> int
		{
			const int dx = b.X - a.X;
			const int dy = b.Y - a.Y;
			const int steps = std::max(1, std::max(std::abs(dx), std::abs(dy)) / 2);
			int cost = 0;

			for (int i = 0; i <= steps; ++i)
			{
				const int px = a.X + dx * i / steps;
				const int py = a.Y + dy * i / steps;

				for (auto const& threat : threats)
				{
					const int dist = std::max(std::abs(px - threat.X), std::abs(py - threat.Y));

					if (dist < threat.Radius)
						cost += (threat.Radius - dist) * threat.Weight;
				}
			}

			return cost;
		};

		const int directCost = segmentThreatCost(fromCell, toCell);

		if (directCost <= 0)
			return CellStruct::Empty;

		const double laneDX = static_cast<double>(toCell.X - fromCell.X);
		const double laneDY = static_cast<double>(toCell.Y - fromCell.Y);
		const double laneLength = std::sqrt(laneDX * laneDX + laneDY * laneDY);

		if (laneLength < 8.0)
			return CellStruct::Empty;

		const double normalX = -laneDY / laneLength;
		const double normalY = laneDX / laneLength;
		constexpr double LaneFractions[] = { 0.35, 0.5, 0.65 };
		constexpr int LaneOffsets[] = { 10, -10, 18, -18, 26, -26, 36, -36 };

		CellStruct bestWaypoint = CellStruct::Empty;
		int bestCost = directCost;

		for (const double fraction : LaneFractions)
		{
			for (const int offset : LaneOffsets)
			{
				const CellStruct candidate {
					static_cast<short>(fromCell.X + laneDX * fraction + normalX * offset),
					static_cast<short>(fromCell.Y + laneDY * fraction + normalY * offset)
				};

				auto const pCandCell = MapClass::Instance.TryGetCellAt(candidate);

				if (!pCandCell || !MapClass::Instance.CoordinatesLegal(candidate))
					continue;

				if (isLandMover)
				{
					if (pCandCell->LandType == LandType::Water
						|| !pCandCell->IsClearToMove(
							pTransType->SpeedType, true, true, -1, pTransType->MovementZone, -1, false)
						|| !AreZonesConnected(fromCell, candidate, pTransType->MovementZone))
					{
						continue;
					}
				}
				else if (!isAirborneTransport)
				{
					// Surface transports detour through open water reachable from here.
					if (pCandCell->LandType != LandType::Water
						|| !pCandCell->IsClearToMove(
							pTransType->SpeedType, true, true, -1, pTransType->MovementZone, -1, false)
						|| !AreZonesConnected(fromCell, candidate, pTransType->MovementZone))
					{
						continue;
					}
				}

				// Exposure dominates; the extra sailing distance only breaks ties.
				const int detourLength = static_cast<int>(fromCell.DistanceFrom(candidate) + candidate.DistanceFrom(toCell));
				const int cost = segmentThreatCost(fromCell, candidate) + segmentThreatCost(candidate, toCell)
					+ std::max(0, detourLength - static_cast<int>(laneLength)) / 4;

				if (cost < bestCost)
				{
					bestCost = cost;
					bestWaypoint = candidate;
				}
			}
		}

		// Only detour when it is clearly safer; otherwise exposure is unavoidable.
		return (bestWaypoint != CellStruct::Empty && bestCost * 10 < directCost * 7) ? bestWaypoint : CellStruct::Empty;
	}

	// TEMP diagnostic (order churn, 2026-11-19; user: units drive halfway to their goal, turn back, then
	// take it up again, "as if the AI were schizophrenic"). At the start of each AI tick it reads where
	// every vehicle and soldier of the house heads (its attack-move goal, else its destination; not
	// harvesters, which the engine drives). A switch is a new goal more than OrderChurnJumpCells from the
	// old one before the unit reached it; a turn is a switch that points back (more than 90 degrees from
	// the old way), a flip a turn within OrderChurnFlipFrames of the last change, a reversal a switch
	// back to (near) the goal before the last one within OrderChurnReverseFrames. Each is booked on the
	// pair of codes that gave the old and the new goal (NoteOrderCaller; none since the last look is
	// "untracked": the engine, or an order given without a note). Every OrderChurnLogFrames a line per
	// house with the totals, and a line for each of its worst pairs.
	constexpr int OrderChurnLogFrames = 9000;
	constexpr int OrderChurnFlipFrames = 450;
	constexpr int OrderChurnReverseFrames = 1800;
	constexpr double OrderChurnJumpCells = 6.0;
	constexpr double OrderChurnBackCells = 4.0;
	constexpr double OrderChurnArriveCells = 3.0;
	constexpr int OrderChurnTopPairs = 6;

	struct NeuralOrderGoal
	{
		CellStruct Goal { CellStruct::Empty };
		CellStruct PrevGoal { CellStruct::Empty };
		const char* File { nullptr };
		int Line { 0 };
		int ChangeFrame { -1 };
		int SeenFrame { -1 };
		bool Reached { false };
		// Where it headed at the end of the last AI tick of its house (SnapshotOrderGoals).
		CellStruct EndGoal { CellStruct::Empty };
		int EndFrame { -1 };
	};

	struct NeuralOrderChurnPair
	{
		int Switches { 0 };
		int Turns { 0 };
		int Flips { 0 };
		int Reversals { 0 };
		const char* TypeId { nullptr };
		DWORD UniqueID { 0 };
	};

	struct NeuralOrderChurn
	{
		std::unordered_map<DWORD, NeuralOrderGoal> Goals;
		std::map<std::string, NeuralOrderChurnPair> Pairs;
		int Switches { 0 };
		int Turns { 0 };
		int Flips { 0 };
		int Reversals { 0 };
		int LogFrame { 0 };
		int Serial { -1 };
	};

	std::array<NeuralOrderChurn, 32> OrderChurn {};

	void UpdateOrderChurn(HouseClass* pHouse)
	{
		if (!NeuralAI::HeadlessMode || !pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& churn = OrderChurn[pHouse->ArrayIndex];
		const int now = Unsorted::CurrentFrame;

		if (churn.Serial != NeuralMatchSerial || now < churn.LogFrame)
		{
			churn = {};
			churn.Serial = NeuralMatchSerial;
			churn.LogFrame = now;
		}

		auto const& callers = LastOrderCaller[pHouse->ArrayIndex];

		auto const look = [&](FootClass* pFoot, TechnoTypeClass* pType)
		{
			if (!pFoot || !pType || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || pType->ResourceGatherer)
				return;

			const CellStruct here = pFoot->GetMapCoords();
			auto& entry = churn.Goals[pFoot->UniqueID];
			const int lastSeen = entry.SeenFrame;
			entry.SeenFrame = now;

			if (entry.Goal != CellStruct::Empty && here.DistanceFrom(entry.Goal) <= OrderChurnArriveCells)
				entry.Reached = true;

			AbstractClass* const pGoalObject = (pFoot->MegaMission == Mission::AttackMove && pFoot->MegaDestination)
				? pFoot->MegaDestination
				: pFoot->Destination;

			// Arrived or stopped: the old goal stays, Reached tells whether the unit got there.
			if (!pGoalObject)
				return;

			const CellStruct goal = CellClass::Coord2Cell(pGoalObject->GetCoords());

			if (entry.Goal != CellStruct::Empty && goal.DistanceFrom(entry.Goal) <= OrderChurnJumpCells)
				return;

			// The goal it had at the end of the last AI tick tells who changed it: "engine" (or a hook
			// outside the AI tick) when it changed after that tick, else the code noted in that tick, else
			// "unnoted" (an order of that tick given without a note); "untracked" without that end look.
			const bool endSeen = entry.EndFrame >= 0 && entry.EndFrame >= lastSeen;
			const bool changedAfterTick = endSeen
				&& (entry.EndGoal == CellStruct::Empty || goal.DistanceFrom(entry.EndGoal) > OrderChurnJumpCells);
			const char* file = endSeen ? "unnoted" : "untracked";
			int line = 0;

			if (changedAfterTick)
			{
				file = "engine";
			}
			else if (auto const itCaller = callers.find(pFoot->UniqueID);
				itCaller != callers.end() && itCaller->second.File && itCaller->second.Frame >= lastSeen)
			{
				file = itCaller->second.File;
				line = itCaller->second.Line;
			}

			if (entry.Goal != CellStruct::Empty && !entry.Reached)
			{
				const int oldDx = entry.Goal.X - here.X;
				const int oldDy = entry.Goal.Y - here.Y;
				const int newDx = goal.X - here.X;
				const int newDy = goal.Y - here.Y;
				const bool turn = oldDx * newDx + oldDy * newDy < 0;
				const bool flip = turn && entry.ChangeFrame >= 0 && now - entry.ChangeFrame <= OrderChurnFlipFrames;
				const bool reversal = entry.PrevGoal != CellStruct::Empty && goal.DistanceFrom(entry.PrevGoal) <= OrderChurnBackCells
					&& entry.ChangeFrame >= 0 && now - entry.ChangeFrame <= OrderChurnReverseFrames;

				char key[128];
				std::snprintf(key, sizeof(key), "%s:%d > %s:%d", entry.File ? entry.File : "untracked", entry.Line, file, line);
				auto& pair = churn.Pairs[key];
				pair.Switches++;
				pair.Turns += turn ? 1 : 0;
				pair.Flips += flip ? 1 : 0;
				pair.Reversals += reversal ? 1 : 0;
				pair.TypeId = pType->ID;
				pair.UniqueID = pFoot->UniqueID;
				churn.Switches++;
				churn.Turns += turn ? 1 : 0;
				churn.Flips += flip ? 1 : 0;
				churn.Reversals += reversal ? 1 : 0;
			}

			entry.PrevGoal = entry.Goal;
			entry.Goal = goal;
			entry.File = file;
			entry.Line = line;
			entry.ChangeFrame = now;
			entry.Reached = here.DistanceFrom(goal) <= OrderChurnArriveCells;
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse)
				look(pUnit, pUnit->Type);
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->Owner == pHouse)
				look(pInf, pInf->Type);
		}

		if (churn.Goals.size() > 2048)
			std::erase_if(churn.Goals, [now](const auto& entry) { return now - entry.second.SeenFrame > 600; });

		if (now - churn.LogFrame < OrderChurnLogFrames)
			return;

		Debug::Log("[NeuralAI] House %d order churn (TEMP): %d switches, %d turns, %d flips, %d reversals in %d frames, %d pairs, frame %d.\n",
			pHouse->ArrayIndex, churn.Switches, churn.Turns, churn.Flips, churn.Reversals, now - churn.LogFrame,
			static_cast<int>(churn.Pairs.size()), now);

		std::vector<std::pair<const std::string*, const NeuralOrderChurnPair*>> worst;
		worst.reserve(churn.Pairs.size());

		for (auto const& [key, pair] : churn.Pairs)
		{
			if (pair.Turns + pair.Reversals > 0)
				worst.emplace_back(&key, &pair);
		}

		std::stable_sort(worst.begin(), worst.end(), [](const auto& a, const auto& b)
		{
			return a.second->Turns + a.second->Reversals > b.second->Turns + b.second->Reversals;
		});

		for (size_t i = 0; i < worst.size() && i < static_cast<size_t>(OrderChurnTopPairs); ++i)
		{
			auto const& pair = *worst[i].second;
			Debug::Log("[NeuralAI] House %d order churn pair (TEMP): %s: %d switches, %d turns, %d flips, %d reversals, last %s %u, frame %d.\n",
				pHouse->ArrayIndex, worst[i].first->c_str(), pair.Switches, pair.Turns, pair.Flips, pair.Reversals,
				pair.TypeId ? pair.TypeId : "?", pair.UniqueID, now);
		}

		// Orders the leases turned down (MayOrderUnit), by holder and refused code.
		auto& refusals = LeaseRefusals[pHouse->ArrayIndex];
		std::vector<std::pair<const std::string*, int>> refused;
		refused.reserve(refusals.Pairs.size());

		for (auto const& [key, count] : refusals.Pairs)
			refused.emplace_back(&key, count);

		std::stable_sort(refused.begin(), refused.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		Debug::Log("[NeuralAI] House %d order leases (TEMP): %d orders turned down, %d pairs, %d leases held, frame %d.\n",
			pHouse->ArrayIndex, refusals.Total, static_cast<int>(refusals.Pairs.size()),
			static_cast<int>(OrderLeasesOf(pHouse->ArrayIndex).size()), now);

		for (size_t i = 0; i < refused.size() && i < static_cast<size_t>(OrderChurnTopPairs); ++i)
		{
			Debug::Log("[NeuralAI] House %d order lease refusal (TEMP): %s: %d, frame %d.\n", pHouse->ArrayIndex,
				refused[i].first->c_str(), refused[i].second, now);
		}

		refusals = {};
		churn.Pairs.clear();
		churn.Switches = 0;
		churn.Turns = 0;
		churn.Flips = 0;
		churn.Reversals = 0;
		churn.LogFrame = now;
	}

	// TEMP diagnostic (order churn): where the units of the house head at the end of its AI tick, so the
	// next look tells the changes the engine made after the tick from the orders of the tick.
	void SnapshotOrderGoals(HouseClass* pHouse)
	{
		if (!NeuralAI::HeadlessMode || !pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& churn = OrderChurn[pHouse->ArrayIndex];

		if (churn.Serial != NeuralMatchSerial)
			return;

		const int now = Unsorted::CurrentFrame;

		for (auto& [uid, entry] : churn.Goals)
		{
			if (entry.SeenFrame != now)
				continue;

			entry.EndFrame = now;
			entry.EndGoal = CellStruct::Empty;
		}

		auto const note = [&](FootClass* pFoot)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo)
				return;

			auto const itEntry = churn.Goals.find(pFoot->UniqueID);

			if (itEntry == churn.Goals.end() || itEntry->second.EndFrame != now)
				return;

			AbstractClass* const pGoalObject = (pFoot->MegaMission == Mission::AttackMove && pFoot->MegaDestination)
				? pFoot->MegaDestination
				: pFoot->Destination;

			if (pGoalObject)
				itEntry->second.EndGoal = CellClass::Coord2Cell(pGoalObject->GetCoords());
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse)
				note(pUnit);
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->Owner == pHouse)
				note(pInf);
		}
	}

	// Takes the end look of SnapshotOrderGoals when the AI tick of the house is over.
	struct NeuralOrderChurnTickEnd
	{
		explicit NeuralOrderChurnTickEnd(HouseClass* pHouse) : House { pHouse } { }
		~NeuralOrderChurnTickEnd() { SnapshotOrderGoals(House); }

		NeuralOrderChurnTickEnd(const NeuralOrderChurnTickEnd&) = delete;
		NeuralOrderChurnTickEnd& operator=(const NeuralOrderChurnTickEnd&) = delete;

		HouseClass* House;
	};

} // namespace
