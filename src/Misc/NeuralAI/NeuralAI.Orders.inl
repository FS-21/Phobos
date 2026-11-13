// NeuralAI.Orders.inl: Repair, enter and attack-move orders, boarding, island landing and ferry waypoints.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Picks the repair station (UnitRepair=yes) this unit should use: only stations it can
	// actually reach (naval units at naval stations, ground units at land stations,
	// amphibious/hover units at either), balancing load by adding the current queue (our
	// units already entering or docked there) to the distance. No more single-depot queues.
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
			|| !pEnterTarget->IsAlive)
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

	// A passenger waiting for its turn to board: true when it is within FerryWaitCells of its
	// transport, where it stops instead of walking on to the door (a crowd on the cells next to
	// the transport keeps the one boarding out). Farther away it only drops the march it was on
	// (an attack-move takes its old destination back over the walk to the transport) and the
	// caller sends it closer.
	bool HoldFerryWaiter(FootClass* pWaiter, FootClass* pTransport)
	{
		if (pWaiter->GetMapCoords().DistanceFrom(pTransport->GetMapCoords()) > FerryWaitCells)
		{
			if (pWaiter->MegaMission != Mission::None)
				pWaiter->ClearMegaMissionData();

			return false;
		}

		if (pWaiter->CurrentMission == Mission::Move && pWaiter->Destination == pTransport)
		{
			pWaiter->SetDestination(nullptr, true);
			pWaiter->ClearNavigationList();
			pWaiter->ForceMission(Mission::Guard);
		}

		return true;
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

	// TEMP diagnostic: what the route legs answer to the units that stay held on their cell with an
	// attack-move to give, per house every HeldRouteLogFrames, and a few of those units in detail
	// (those that got no leg, and those held again within 1200 frames of a leg).
	constexpr int HeldRouteLogFrames = 1800;
	constexpr int HeldRouteSamplesPerMatch = 30;

	void NoteHeldRouteAnswer(FootClass* pFoot, const CellStruct& goal, CellClass* pLeg, bool afterLeg)
	{
		constexpr size_t AnswerCount = static_cast<size_t>(NeuralRouteAnswer::Count);
		static constexpr const char* AnswerNames[AnswerCount] = { "given", "excluded", "near", "no zone", "goal off zone", "budget",
			"no route", "short leg" };
		static int serial = -1;
		static std::array<std::array<int, AnswerCount + 1>, 32> answers {};
		static std::array<int, 32> logFrame {};
		static int samples = 0;

		if (serial != NeuralMatchSerial)
		{
			serial = NeuralMatchSerial;
			samples = 0;
			logFrame.fill(0);

			for (auto& tally : answers)
				tally.fill(0);
		}

		const int house = pFoot->Owner ? pFoot->Owner->ArrayIndex : -1;
		auto const pType = pFoot->GetTechnoType();

		if (house < 0 || house >= 32 || !pType)
			return;

		auto& tally = answers[house];
		const size_t answer = std::min(static_cast<size_t>(LastGroundRouteAnswer), AnswerCount - 1);
		tally[answer]++;

		if (afterLeg)
			tally[AnswerCount]++;

		const int now = Unsorted::CurrentFrame;
		const CellStruct here = pFoot->GetMapCoords();

		if ((afterLeg || !pLeg) && samples < HeldRouteSamplesPerMatch)
		{
			++samples;
			int crowd = 0;

			for (auto const pTechno : TechnoClass::Array)
			{
				if (pTechno && pTechno != pFoot && pTechno->IsAlive && !pTechno->InLimbo && pTechno->WhatAmI() != AbstractType::Building
					&& pTechno->GetMapCoords().DistanceFrom(here) <= 2.5)
				{
					++crowd;
				}
			}

			auto const pGoalCell = MapClass::Instance.TryGetCellAt(goal);
			const MovementZone mz = pType->MovementZone;
			Debug::Log("[NeuralAI] Held unit DIAG house %d frame %d: %s %u at (%d,%d) mz %d zone %d, goal (%d,%d) %d cells away zone %d land %d, "
				"answer %s, leg (%d,%d) free of units %d, held after a leg %d, mission %d mega %d, %d units within 2 cells.\n",
				house, now, pType->ID, pFoot->UniqueID, here.X, here.Y, static_cast<int>(mz), ObjectZone(pFoot, mz), goal.X, goal.Y,
				static_cast<int>(here.DistanceFrom(goal)), MapClass::Instance.GetMovementZoneType(goal, mz, false),
				pGoalCell ? static_cast<int>(pGoalCell->LandType) : -1, AnswerNames[answer],
				pLeg ? pLeg->MapCoords.X : -1, pLeg ? pLeg->MapCoords.Y : -1,
				pLeg ? (pLeg->IsClearToMove(pType->SpeedType, false, false, -1, mz, -1, false) ? 1 : 0) : -1, afterLeg ? 1 : 0,
				static_cast<int>(pFoot->CurrentMission), static_cast<int>(pFoot->MegaMission), crowd);
		}

		if (now - logFrame[house] >= HeldRouteLogFrames || now < logFrame[house])
		{
			logFrame[house] = now;
			Debug::Log("[NeuralAI] House %d held units and route legs: given %d, excluded %d, near %d, no zone %d, goal off zone %d, budget %d, "
				"no route %d, short leg %d; held again after a leg %d, frame %d.\n",
				house, tally[0], tally[1], tally[2], tally[3], tally[4], tally[5], tally[6], tally[7], tally[AnswerCount], now);
			tally.fill(0);
		}
	}

	void OrderFootToAttackMove(FootClass* pFoot, AbstractClass* pDest)
	{
		if (!pFoot || !pDest || pFoot->InLimbo || !pFoot->IsAlive)
			return;

		// A transport parked to load stays where its passengers go.
		if (pFoot->Owner && IsFerryParkedToLoad(pFoot->Owner, pFoot->UniqueID))
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

				if (pW && (pW->Spawner || (pW->Damage > 0 && (!pW->Projectile || pW->Projectile->AG))))
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

					if (held)
						NoteHeldRouteAnswer(pFoot, pDestCell->MapCoords, pHopCell, hopping);

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
			// A unit that resource cells hurt goes around a field that lies across its way.
			CellClass* const pDetourHop = ResourceDetourHop(pFoot, pDestCell->MapCoords);
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
				// off the resource cells and still ahead.
				if (auto const pOldHop = abstract_cast<CellClass*>(pFoot->MegaDestination);
					pOldHop && pFoot->MegaMission == Mission::AttackMove
					&& (pDetourHop
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
				CountDecision(pDetourHop ? NeuralDecision::ResourceDetour : NeuralDecision::GroundRouteHop,
					pFoot->Owner ? pFoot->Owner->ArrayIndex : -1);
			}
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
				&& pFoot->MegaTarget == pNewMegaTarget && pOldMegaCell->MapCoords.DistanceFrom(pNewMegaCell->MapCoords) <= 3.0;

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

		if (!canCrossWaterDirectly && fz >= 0
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

			if (pFoot->Destination != nullptr && pFoot->Target == nullptr)
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

	// One search per house, island reference, zone and ore preference per frame (user's profile:
	// FindSafeIslandLandingCell was the AI's costliest function, called once per soldier of a wave
	// ordered across water with the very same arguments, thousands of cells each).
	CellClass* FindSafeIslandLandingCell(
		HouseClass* pHouse, const CellStruct& islandRefCell, int targetIslandZone, bool preferOre)
	{
		static int cacheFrame = -1;
		static std::unordered_map<uint64_t, CellStruct> cache;

		if (cacheFrame != Unsorted::CurrentFrame)
		{
			cacheFrame = Unsorted::CurrentFrame;
			cache.clear();
		}

		const uint64_t key = (static_cast<uint64_t>(static_cast<uint8_t>(pHouse ? pHouse->ArrayIndex : 255)) << 56)
			| (static_cast<uint64_t>(static_cast<uint16_t>(targetIslandZone)) << 40)
			| (static_cast<uint64_t>(static_cast<uint16_t>(islandRefCell.X)) << 24)
			| (static_cast<uint64_t>(static_cast<uint16_t>(islandRefCell.Y)) << 8) | (preferOre ? 1u : 0u);

		if (auto const it = cache.find(key); it != cache.end())
			return it->second == CellStruct::Empty ? nullptr : MapClass::Instance.TryGetCellAt(it->second);

		CellClass* const pResult = FindSafeIslandLandingCellUncached(pHouse, islandRefCell, targetIslandZone, preferOre);
		cache.emplace(key, pResult ? pResult->MapCoords : CellStruct::Empty);
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

} // namespace
