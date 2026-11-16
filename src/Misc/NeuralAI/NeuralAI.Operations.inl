// NeuralAI.Operations.inl: Transport operations manager.
// Part of NeuralAI.cpp (included in order; one translation unit).
//
// A client (a wave, the economy) asks to take units to an address on another landmass, and their
// mission waits meanwhile. The operation is a removal company apart from the client that booked
// the move (user): it gathers the units behind a beach of their land, brings the transports it
// needs (idle ones first, what it lacks is demand for the factories), loads them, crosses along a
// safe lane, sets them down on the land of the address (another beach of it when the first is
// dangerous, the least bad one when all are), hands them back to their client and goes back for
// those left behind. Its units and transports take orders from it alone: the rest of the AI leaves
// them out, and an order found on one of them is logged and overridden (last word and log, user).

namespace
{
	// Defined in NeuralAI.Micro.inl and NeuralAI.Scouting.inl.
	bool MoveTransportToLoadingCell(HouseClass* pHouse, UnitClass* pTransport, const CellStruct& from, int landZone, bool passengersWaiting);
	bool IsActiveScout(HouseClass* pHouse, DWORD uid);

	// How a transport of an operation carries: passengers aboard on the ground (a hovercraft, an
	// amphibious or burrowing APC) or flying (a jumpjet or flying transport, which comes down on a
	// free cell off the beach to load and unload), or one vehicle under a carryall aircraft.
	enum class OpCarrier : uint8_t
	{
		None,
		Surface,
		Flyer,
		Carryall
	};

	OpCarrier GetOpCarrier(const FootClass* pFoot)
	{
		if (!pFoot)
			return OpCarrier::None;

		if (auto const pAir = abstract_cast<const AircraftClass*>(pFoot))
			return (pAir->Type && pAir->Type->Carryall) ? OpCarrier::Carryall : OpCarrier::None;

		auto const pUnit = abstract_cast<const UnitClass*>(pFoot);

		// An IFV changes with its gunner and is no removal van.
		if (!pUnit || !pUnit->Type || !IsAmphibiousOrAirTransport(pUnit->Type) || pUnit->Type->Gunner
			|| HasNoManualEnter(pUnit->Type) || HasNoManualUnload(pUnit->Type))
		{
			return OpCarrier::None;
		}

		return (pUnit->Type->JumpJet || pUnit->Type->MovementZone == MovementZone::Fly) ? OpCarrier::Flyer : OpCarrier::Surface;
	}

	const char* OpClientName(NeuralOpClient client)
	{
		switch (client)
		{
		case NeuralOpClient::Wave:
			return "wave";
		case NeuralOpClient::Plan:
			return "plan";
		case NeuralOpClient::Team:
			return "team";
		case NeuralOpClient::Economy:
			return "economy";
		case NeuralOpClient::Strike:
			return "strike";
		default:
			return "?";
		}
	}

	const char* OpLegName(NeuralOpLeg leg)
	{
		switch (leg)
		{
		case NeuralOpLeg::ToStaging:
			return "to staging";
		case NeuralOpLeg::Parked:
			return "parked";
		case NeuralOpLeg::Crossing:
			return "crossing";
		case NeuralOpLeg::Returning:
			return "returning";
		default:
			return "?";
		}
	}

	// The ground zone a unit stands on (MovementZone::Normal). One on a cell of no ground zone (a
	// shore, a cliff foot, a factory exit) stands for the ground within 2 cells of it. -1: none.
	int OpFootGroundZone(const FootClass* pFoot)
	{
		const CellStruct at = pFoot->GetMapCoords();
		int zone = MapClass::Instance.GetMovementZoneType(at, MovementZone::Normal, pFoot->OnBridge);

		for (int r = 1; r <= 2 && !IsGroundZone(zone); ++r)
		{
			for (int dy = -r; dy <= r && !IsGroundZone(zone); ++dy)
			{
				for (int dx = -r; dx <= r && !IsGroundZone(zone); ++dx)
				{
					if (std::abs(dx) != r && std::abs(dy) != r)
						continue;

					const CellStruct beside { static_cast<short>(at.X + dx), static_cast<short>(at.Y + dy) };
					const int nearZone = MapClass::Instance.GetMovementZoneType(beside, MovementZone::Normal, false);

					if (IsGroundZone(nearZone))
						zone = nearZone;
				}
			}
		}

		return IsGroundZone(zone) ? zone : -1;
	}

	// The ground an address is reached from: the zone of its cell (a bridge cell: the deck's), or for
	// an address on no ground (a ship off the shore, a cell with no zone) the ground next to it
	// within FerryTargetGroundCells, the unit's own when it touches it. ground: the cell to land by.
	// -1: none.
	int OpTargetGroundZone(const CellStruct& target, int footZone, CellStruct& ground)
	{
		ground = target;
		int zone = MapClass::Instance.GetMovementZoneType(target, MovementZone::Normal, false);

		if (!IsGroundZone(zone))
		{
			auto const pCell = MapClass::Instance.TryGetCellAt(target);

			if (pCell && (pCell->ContainsBridge() || pCell->ContainsBridgeEx()))
				zone = MapClass::Instance.GetMovementZoneType(target, MovementZone::Normal, true);
		}

		if (IsGroundZone(zone))
			return zone;

		int nearest = -1;

		for (int r = 1; r <= FerryTargetGroundCells; ++r)
		{
			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::abs(dx) != r && std::abs(dy) != r)
						continue;

					const CellStruct beside { static_cast<short>(target.X + dx), static_cast<short>(target.Y + dy) };
					const int nearZone = MapClass::Instance.GetMovementZoneType(beside, MovementZone::Normal, false);

					if (!IsGroundZone(nearZone))
						continue;

					if (nearZone == footZone)
					{
						ground = target;
						return footZone;
					}

					if (nearest < 0)
					{
						nearest = nearZone;
						ground = beside;
					}
				}
			}
		}

		return nearest;
	}

	// Can this transport carry this unit at all, room aside? A passenger transport takes what fits
	// its SizeLimit; a carryall aircraft a vehicle it may sling.
	bool OpCarrierTakes(FootClass* pCarrier, OpCarrier carrier, FootClass* pMember)
	{
		auto const pMemberType = pMember->GetTechnoType();

		if (carrier == OpCarrier::Carryall)
		{
			auto const pVehicle = abstract_cast<UnitClass*>(pMember);

			return pVehicle && !pMember->InLimbo && CanCarryallLiftUnit(static_cast<AircraftClass*>(pCarrier), pVehicle);
		}

		auto const pCarrierType = pCarrier->GetTechnoType();
		const int size = std::max(1, static_cast<int>(pMemberType->Size));

		return size <= static_cast<int>(pCarrierType->SizeLimit) && size <= pCarrierType->Passengers;
	}

	// An idle transport an operation may take: ours, sound, empty, a removal van of the kinds above,
	// and in nobody else's work (a plan, the old ferry loader, a repair or scout trip, a sneak
	// strike, an airlift, a factory or repair bay it is linked to).
	bool IsTransportFreeForOp(HouseClass* pHouse, FootClass* pFoot, OpCarrier carrier)
	{
		if (carrier == OpCarrier::None || pFoot->Owner != pHouse || pFoot->InLimbo || !pFoot->IsAlive || pFoot->Health <= 0
			|| pFoot->IsCrashing || pFoot->IsSinking || pFoot->GetHealthPercentage() < OpTransportMinHealth
			|| pFoot->Passengers.NumPassengers > 0 || pFoot->IsMindControlled() || pFoot->Deactivated || pFoot->IsUnderEMP()
			|| pFoot->HasAnyLink())
		{
			return false;
		}

		const DWORD uid = pFoot->UniqueID;

		if (IsInTransportOp(pHouse, uid) || IsPlanMember(pHouse, uid) || IsFerryLoading(pHouse, uid) || IsFerryParkedToLoad(pHouse, uid)
			|| IsOnRepairTrip(pHouse, uid) || IsActiveScout(pHouse, uid) || IsSneakStrikeActive(pHouse, uid))
		{
			return false;
		}

		if (carrier == OpCarrier::Carryall)
		{
			return pFoot->CurrentMission != Mission::Capture && pFoot->CurrentMission != Mission::Unload
				&& !FindPendingPickupCargo(pHouse, pFoot);
		}

		auto const pCarryState = UnitExt::Fetch(static_cast<UnitClass*>(pFoot));

		return !(pCarryState && pCarryState->CarryallCargo);
	}

	// A free land cell of a zone for a flying transport to come down on, the nearest to center from
	// firstRing cells out (fixed order), off the beach (a jumpjet does not touch down on one: over
	// a beach it flies on to the next cell and tries again), two cells or more from every unit in
	// keepClear, other than the one tried last.
	CellClass* PickOpFlyerCell(const CellStruct& center, int zone, int firstRing, const CellStruct& tried,
		const std::vector<CellStruct>& keepClear)
	{
		for (int ring = firstRing; ring <= BoardingLandingRingCells; ++ring)
		{
			for (int dy = -ring; dy <= ring; ++dy)
			{
				for (int dx = -ring; dx <= ring; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != ring)
						continue;

					const CellStruct c { static_cast<short>(center.X + dx), static_cast<short>(center.Y + dy) };
					auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

					if (!pCell || c == tried || pCell->LandType == LandType::Water || pCell->LandType == LandType::Beach
						|| pCell->GetBuilding() || pCell->FirstObject
						|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != zone
						|| !pCell->IsClearToMove(SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false))
					{
						continue;
					}

					const bool clear = std::none_of(keepClear.begin(), keepClear.end(),
						[&c](const CellStruct& at) { return at.DistanceFrom(c) < 2.0; });

					if (clear)
						return pCell;
				}
			}
		}

		return nullptr;
	}

	// Where a transport of a load sets it down: the nearest cell to around within 6 cells on the
	// land of the address that the transport can stand on (a flyer: off the beach and free), other
	// than tried, OpLandingSpreadCells or more from where the other crossing transports of the
	// operation set theirs down.
	CellClass* PickOpLandingCell(const NeuralTransportOperation& op, FootClass* pTransport, OpCarrier carrier, const CellStruct& around,
		const CellStruct& tried)
	{
		auto const pType = pTransport->GetTechnoType();
		const bool flies = carrier == OpCarrier::Flyer || carrier == OpCarrier::Carryall;
		std::vector<CellStruct> taken;

		for (auto const& other : op.Transports)
		{
			if (other.UniqueID != pTransport->UniqueID && other.Leg == NeuralOpLeg::Crossing && other.Landing != CellStruct::Empty)
				taken.push_back(other.Landing);
		}

		for (int ring = 0; ring <= 6; ++ring)
		{
			for (int dy = -ring; dy <= ring; ++dy)
			{
				for (int dx = -ring; dx <= ring; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != ring)
						continue;

					const CellStruct c { static_cast<short>(around.X + dx), static_cast<short>(around.Y + dy) };
					auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

					if (!pCell || c == tried || pCell->LandType == LandType::Water || pCell->GetBuilding()
						|| (flies && (pCell->LandType == LandType::Beach || pCell->FirstObject))
						|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != op.TargetZone)
					{
						continue;
					}

					if (!flies && !pCell->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, false))
						continue;

					const bool spread = std::none_of(taken.begin(), taken.end(),
						[&c](const CellStruct& at) { return at.DistanceFrom(c) < OpLandingSpreadCells; });

					if (spread)
						return pCell;
				}
			}
		}

		return nullptr;
	}

	NeuralTransportOperation* FindTransportOp(HouseClass* pHouse, int id)
	{
		for (auto& op : GetMacroState(pHouse).TransportOps)
		{
			if (op.Id == id && !op.Over)
				return &op;
		}

		return nullptr;
	}

	// A client asks to take a unit to an address on other ground. True when an operation has it
	// (a new one, or one of the same client between the same two lands that is still under way);
	// false when it needs none (it walks there or crosses on its own), cannot have one (no ground
	// at either end, no transport of ours or that we can build makes that crossing) or gave up on it
	// lately (OpRetryFrames).
	bool RequestTransportOperation(HouseClass* pHouse, NeuralOpClient client, FootClass* pFoot, const CellStruct& target)
	{
		if (!TransportOpsEnabled || !pHouse || !pFoot || pFoot->InLimbo || !pFoot->IsAlive || target == CellStruct::Empty)
			return false;

		auto& macro = GetMacroState(pHouse);
		const DWORD uid = pFoot->UniqueID;

		if (macro.TransportOpOf.contains(uid))
			return true;

		const int now = Unsorted::CurrentFrame;

		if (auto const itRetry = macro.TransportOpRetryUntil.find(uid); itRetry != macro.TransportOpRetryUntil.end())
		{
			if (now >= itRetry->second || now < itRetry->second - OpRetryFrames)
				macro.TransportOpRetryUntil.erase(itRetry);
			else
				return false;
		}

		// A plan's member stays with its plan (plans start only when every member walks there).
		auto const pType = pFoot->GetTechnoType();

		if (!pType || CrossesWaterAlone(pType) || GetOpCarrier(pFoot) != OpCarrier::None || pFoot->Transporter
			|| IsPlanMember(pHouse, uid))
		{
			return false;
		}

		// The unit's own movement may join the two cells where a vehicle's does not (a soldier on stairs).
		const int footZone = OpFootGroundZone(pFoot);
		CellStruct ground = target;
		const int targetZone = OpTargetGroundZone(target, footZone, ground);

		if (!IsGroundZone(footZone) || !IsGroundZone(targetZone) || footZone == targetZone || IsSameGroundForFoot(pFoot, target))
			return false;

		auto const pLanding = FindSafeIslandLandingCell(pHouse, ground, targetZone, false);
		const CellStruct footCoords = pFoot->GetMapCoords();

		if (!pLanding || !CanHouseFerryTypeAcrossWater(pHouse, pType, &footCoords, &pLanding->MapCoords))
			return false;

		NeuralTransportOperation* pOp = nullptr;

		for (auto& op : macro.TransportOps)
		{
			if (!op.Over && op.Client == client && op.OriginZone == footZone && op.TargetZone == targetZone)
			{
				pOp = &op;
				break;
			}
		}

		if (!pOp)
		{
			auto& op = macro.TransportOps.emplace_back();
			op.Id = macro.NextTransportOpId++;
			op.Client = client;
			op.OriginZone = footZone;
			op.TargetZone = targetZone;
			op.Target = ground;
			op.CreatedFrame = now;
			op.LastTransportFrame = now;
			pOp = &op;

			Debug::Log("[NeuralAI] House %d transport op %d opened for a %s: %s %u at (%d,%d) zone %d -> (%d,%d) zone %d, frame %d.\n",
				pHouse->ArrayIndex, op.Id, OpClientName(client), pType->ID, uid, footCoords.X, footCoords.Y, footZone, ground.X, ground.Y,
				targetZone, now);
		}
		else
		{
			pOp->Joined++;
		}

		NeuralOpMember member {};
		member.UniqueID = uid;
		member.Size = std::max(1, static_cast<int>(pType->Size));
		member.Target = ground;
		pOp->Members.push_back(member);
		macro.TransportOpOf[uid] = pOp->Id;

		// Its mission waits: no target and no way of its own (the order its client gave before is
		// dropped), until the operation hands it back.
		if (pFoot->MegaMission == Mission::AttackMove)
			pFoot->ClearMegaMissionData();

		pFoot->SetTarget(nullptr);
		pFoot->SetDestination(nullptr, true);
		pFoot->ForceMission(Mission::Guard);

		return true;
	}

	// Gives a member back to its client: a wave's unit goes on to its address over its own ground now
	// (an attack-move, its goal kept as the wave's), the others wait where they stand for their
	// client's next order.
	void HandBackOpMember(HouseClass* pHouse, const NeuralTransportOperation& op, const NeuralOpMember& member, FootClass* pFoot)
	{
		if (pFoot->CurrentMission == Mission::Sleep)
			pFoot->ForceMission(Mission::Guard);

		// Its post is where it stands now, not the gathering spot on the land it left.
		pFoot->SetArchiveTarget(pFoot->GetCell());

		if (op.Client == NeuralOpClient::Wave)
		{
			if (auto const pCell = MapClass::Instance.TryGetCellAt(member.Target))
			{
				OrderFootToAttackMove(pFoot, pCell);
				GetMacroState(pHouse).WaveGoals[member.UniqueID] = { member.Target, op.TargetZone, Unsorted::CurrentFrame };
				return;
			}
		}

		pFoot->SetDestination(nullptr, true);
		pFoot->QueueMission(Mission::Guard, false);
	}

	void UpdateTransportOperation(HouseClass* pHouse, NeuralTransportOperation& op, std::unordered_map<DWORD, FootClass*>& foots,
		bool logNow)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const int houseIndex = pHouse->ArrayIndex;

		auto const find = [&foots](DWORD uid) -> FootClass*
		{
			auto const it = foots.find(uid);

			return it != foots.end() ? it->second : nullptr;
		};

		auto const recordOf = [&op](DWORD uid) -> NeuralOpTransport*
		{
			for (auto& transport : op.Transports)
			{
				if (transport.UniqueID == uid)
					return &transport;
			}

			return nullptr;
		};

		auto const wake = [](FootClass* pFoot)
		{
			if (pFoot->CurrentMission == Mission::Sleep)
				pFoot->ForceMission(Mission::Guard);
		};

		// Ends the operation: the members still waiting go back to their client (after a failure they
		// may not ask again for OpRetryFrames), the transports are let go.
		auto const endOp = [&](const char* reason, bool failed)
		{
			int aboard = 0;

			for (auto const& member : op.Members)
			{
				macro.TransportOpOf.erase(member.UniqueID);
				auto const pFoot = find(member.UniqueID);

				if (!pFoot)
					continue;

				if (pFoot->InLimbo)
				{
					aboard++;
					continue;
				}

				wake(pFoot);
				pFoot->SetArchiveTarget(pFoot->GetCell());
				pFoot->SetDestination(nullptr, true);
				pFoot->QueueMission(Mission::Guard, false);

				if (failed)
					macro.TransportOpRetryUntil[member.UniqueID] = now + OpRetryFrames;
			}

			for (auto const& transport : op.Transports)
			{
				macro.TransportOpOf.erase(transport.UniqueID);

				if (auto const pTransport = find(transport.UniqueID))
					wake(pTransport);
			}

			Debug::Log("[NeuralAI] House %d transport op %d (%s) ends: %s after %d frames; %d landed, %d lost, %d released, %d given back "
				"(%d still aboard), %d joined, %d trips (%d failed), %d transports taken (%d lost), %d interferences, frame %d.\n",
				houseIndex, op.Id, OpClientName(op.Client), reason, now - op.CreatedFrame, op.Landed, op.Lost, op.Released,
				static_cast<int>(op.Members.size()) - aboard, aboard, op.Joined, op.Trips, op.FailedTrips, op.TransportsTaken,
				op.TransportsLost, op.Interferences, now);

			op.Members.clear();
			op.Transports.clear();
			op.Over = true;
			op.EndReason = reason;
		};

		// An order the operation did not give, found on one of its units: counted, logged at most once
		// per OpInterferenceLogFrames (who interferes is to be found and fixed), and overridden by the
		// operation's own order below (user: last word and log).
		auto const checkInterference = [&](FootClass* pFoot, const CellStruct& orderCell, bool transport)
		{
			const Mission mission = pFoot->CurrentMission;
			auto const pGoing = abstract_cast<CellClass*>(pFoot->Destination);
			const char* what = nullptr;

			if (pFoot->MegaMission == Mission::AttackMove)
				what = "attack-move";
			else if (mission == Mission::Hunt || mission == Mission::Patrol || mission == Mission::Sabotage
				|| mission == Mission::Harvest || mission == Mission::Retreat || (mission == Mission::Enter && transport)
				|| (mission == Mission::Capture && GetOpCarrier(pFoot) != OpCarrier::Carryall))
			{
				what = "mission";
			}
			else if (pFoot->Destination && !pGoing)
				what = "object destination";
			else if (pGoing && orderCell != CellStruct::Empty && pGoing->MapCoords.DistanceFrom(orderCell) > 4.0
				&& mission != Mission::Unload)
			{
				what = "move";
			}

			if (!what)
				return;

			op.Interferences++;

			if (op.InterferenceLogFrame >= 0 && now - op.InterferenceLogFrame < OpInterferenceLogFrames && now >= op.InterferenceLogFrame)
				return;

			op.InterferenceLogFrame = now;
			Debug::Log("[NeuralAI] House %d transport op %d interference (%s): %s %u at (%d,%d) on mission %d %d frames old (mega %d), "
				"destination %s (%d,%d), ordered to (%d,%d), frame %d.\n", houseIndex, op.Id, what, pFoot->GetTechnoType()->ID,
				pFoot->UniqueID, pFoot->GetMapCoords().X, pFoot->GetMapCoords().Y, static_cast<int>(mission),
				now - pFoot->CurrentMissionStartTime, static_cast<int>(pFoot->MegaMission),
				pFoot->Destination ? (pGoing ? "cell" : "object") : "none", pGoing ? pGoing->MapCoords.X : -1,
				pGoing ? pGoing->MapCoords.Y : -1, orderCell.X, orderCell.Y, now);

			// The order is dropped before the operation's own is given.
			if (pFoot->MegaMission == Mission::AttackMove)
				pFoot->ClearMegaMissionData();

			pFoot->SetDestination(nullptr, true);
			pFoot->ForceMission(Mission::Guard);
		};

		// The cell it is sent to is its post too: at the end of a move the engine puts a unit of the AI
		// on Area Guard, which walks it back to its old post (match #10380: 64 such moves of the 8
		// members of one wave while they waited by their beach).
		auto const orderTo = [&](FootClass* pFoot, CellStruct& orderCell, CellClass* pCell, double keepWithin)
		{
			if (!pCell)
				return;

			wake(pFoot);
			pFoot->SetArchiveTarget(pCell);
			IssueMoveOrder(pFoot, pCell, keepWithin);
			orderCell = pCell->MapCoords;
		};

		// 1. Transports destroyed or taken from us leave; the members bound for one wait for another.
		std::erase_if(op.Transports, [&](const NeuralOpTransport& transport)
			{
				if (find(transport.UniqueID))
					return false;

				macro.TransportOpOf.erase(transport.UniqueID);
				op.TransportsLost++;

				// Lost on its way across: the next landings keep away from where it went down.
				if (transport.Leg == NeuralOpLeg::Crossing && transport.ProgressCell != CellStruct::Empty)
					macro.BloodyBeaches.emplace_back(transport.ProgressCell, now);

				for (auto& member : op.Members)
				{
					if (member.Transport == transport.UniqueID && !member.Aboard)
						member.Transport = 0;
				}

				return true;
			});

		// Who is aboard which transport of the operation (carryall cargo is a passenger too).
		std::unordered_map<DWORD, DWORD> aboardOf;

		for (auto const& transport : op.Transports)
		{
			auto const pTransport = find(transport.UniqueID);

			for (auto pPassenger = pTransport->Passengers.GetFirstPassenger(); pPassenger;
				pPassenger = abstract_cast<FootClass*>(pPassenger->NextObject))
			{
				aboardOf[pPassenger->UniqueID] = transport.UniqueID;
			}
		}

		// 2. Members: the lost go, those aboard are counted, those on the land of their address (or that
		// can walk there now) go back to their client, the others wait on their land.
		int waitingSize = 0;
		int aboardCount = 0;

		std::erase_if(op.Members, [&](NeuralOpMember& member)
			{
				auto const pFoot = find(member.UniqueID);

				if (!pFoot)
				{
					op.Lost++;
					macro.TransportOpOf.erase(member.UniqueID);
					return true;
				}

				if (auto const itAboard = aboardOf.find(member.UniqueID); itAboard != aboardOf.end())
				{
					member.Aboard = true;
					member.Transport = itAboard->second;
					aboardCount++;
					return false;
				}

				if (pFoot->InLimbo)
				{
					// In another transport, a building, a factory: no longer the operation's to carry.
					Debug::Log("[NeuralAI] House %d transport op %d lets %s %u go: %s, frame %d.\n", houseIndex, op.Id,
						pFoot->GetTechnoType()->ID, member.UniqueID, pFoot->Transporter ? "aboard another transport" : "in limbo", now);
					op.Released++;
					op.Interferences++;
					macro.TransportOpOf.erase(member.UniqueID);
					return true;
				}

				// On the land of its address, or its own movement takes it there now. FootReachesAlone is not
				// asked: it lets a unit on a shore cell (no ground zone) pass for one that gets anywhere.
				const bool wasAboard = member.Aboard;
				member.Aboard = false;
				const int zone = OpFootGroundZone(pFoot);

				if (zone == op.TargetZone || IsSameGroundForFoot(pFoot, member.Target))
				{
					if (zone == op.TargetZone)
						op.Landed++;
					else
						op.Released++;

					HandBackOpMember(pHouse, op, member, pFoot);
					macro.TransportOpOf.erase(member.UniqueID);
					return true;
				}

				// On a third land (set down short, swept off): the operation cannot take it from there.
				if (zone != op.OriginZone)
				{
					Debug::Log("[NeuralAI] House %d transport op %d lets %s %u go: on zone %d at (%d,%d)%s, frame %d.\n", houseIndex, op.Id,
						pFoot->GetTechnoType()->ID, member.UniqueID, zone, pFoot->GetMapCoords().X, pFoot->GetMapCoords().Y,
						wasAboard ? " after a set-down" : "", now);
					op.Released++;
					wake(pFoot);
					pFoot->SetDestination(nullptr, true);
					pFoot->QueueMission(Mission::Guard, false);
					macro.TransportOpOf.erase(member.UniqueID);
					return true;
				}

				// Set down again on its own land (a load taken back): it waits for a seat anew.
				if (wasAboard)
					member.Transport = 0;

				waitingSize += member.Size;
				macro.OpWaitingSizeBySize[std::clamp(member.Size, 1, 15)] += member.Size;
				return false;
			});

		if (op.Members.empty())
		{
			endOp(op.Landed > 0 ? "all delivered" : "nobody left", op.Landed == 0);
			return;
		}

		// The cells of the members waiting on the map (flying transports come down clear of them).
		std::vector<CellStruct> memberCells;

		for (auto const& member : op.Members)
		{
			if (!member.Aboard)
				memberCells.push_back(find(member.UniqueID)->GetMapCoords());
		}

		if (op.Landing == CellStruct::Empty)
		{
			auto const pLanding = FindSafeIslandLandingCell(pHouse, op.Target, op.TargetZone, false);
			op.Landing = pLanding ? pLanding->MapCoords : op.Target;
		}

		// 3. The staging cell: where the members stand, on the outskirts of the base or expansion they
		// are at (FindBoardingCell), not a coast far off: the transports come to them. Only with no such
		// cell, the coast of their land that no known enemy covers. The members wait next to it, up to
		// 3 cells back towards them, off the cells the transports come to.
		if (op.Staging == CellStruct::Empty && !memberCells.empty())
		{
			int sumX = 0;
			int sumY = 0;

			for (auto const& at : memberCells)
			{
				sumX += at.X;
				sumY += at.Y;
			}

			const int count = static_cast<int>(memberCells.size());
			const CellStruct mean { static_cast<short>(sumX / count), static_cast<short>(sumY / count) };
			// The member nearest to their mean: members spread over a base and an expansion board at one
			// of the two, not halfway.
			CellStruct centre = memberCells.front();

			for (auto const& at : memberCells)
			{
				if (at.DistanceFrom(mean) < centre.DistanceFrom(mean))
					centre = at;
			}

			int clearance = 9999;
			const char* rule = "outskirts";
			CellClass* pStaging = FindBoardingCell(pHouse, centre, op.OriginZone, op.Landing, nullptr, &clearance);

			if (!pStaging)
			{
				rule = "coast fallback";
				pStaging = FindSafeIslandLandingCell(pHouse, centre, op.OriginZone, false);
			}

			op.Staging = pStaging ? pStaging->MapCoords : centre;
			op.Gather = op.Staging;
			const double span = op.Staging.DistanceFrom(centre);

			for (int step = std::min(3, static_cast<int>(span)); step >= 1; --step)
			{
				const CellStruct spot { static_cast<short>(op.Staging.X + (centre.X - op.Staging.X) * step / std::max(1.0, span)),
					static_cast<short>(op.Staging.Y + (centre.Y - op.Staging.Y) * step / std::max(1.0, span)) };
				auto const pSpot = MapClass::Instance.TryGetCellAt(spot);
				bool besideBuilding = false;

				for (int dy = -1; dy <= 1 && !besideBuilding; ++dy)
				{
					for (int dx = -1; dx <= 1 && !besideBuilding; ++dx)
					{
						const CellStruct next { static_cast<short>(spot.X + dx), static_cast<short>(spot.Y + dy) };
						auto const pNext = MapClass::Instance.TryGetCellAt(next);
						besideBuilding = pNext && pNext->GetBuilding();
					}
				}

				if (pSpot && pSpot->LandType != LandType::Water && !besideBuilding
					&& MapClass::Instance.GetMovementZoneType(spot, MovementZone::Normal, false) == op.OriginZone)
				{
					op.Gather = spot;
					break;
				}
			}

			// TEMP diagnostic: how far the members walk to board, and how far from our buildings.
			double farthest = 0.0;

			for (auto const& at : memberCells)
				farthest = std::max(farthest, at.DistanceFrom(op.Staging));

			Debug::Log("[NeuralAI] House %d transport op %d staging DIAG: (%d,%d) by %s, %d cells from the members at (%d,%d) (%d waiting, "
				"farthest %d), nearest building %d, gathering at (%d,%d), landing (%d,%d), frame %d.\n",
				houseIndex, op.Id, op.Staging.X, op.Staging.Y, rule, static_cast<int>(op.Staging.DistanceFrom(centre)), centre.X, centre.Y,
				count, static_cast<int>(farthest), clearance, op.Gather.X, op.Gather.Y, op.Landing.X, op.Landing.Y, now);
		}

		auto const pStagingCell = MapClass::Instance.TryGetCellAt(op.Staging);
		auto const pGatherCell = MapClass::Instance.TryGetCellAt(op.Gather);

		// 4. Seats: the members without one take one on a transport at the staging beach (on its way or
		// parked), the largest first and each on the transport with the most room left. A member bound
		// for a transport that sailed waits for the next.
		auto const pendingOf = [&op](DWORD transportId, int& count, int& size)
		{
			count = 0;
			size = 0;

			for (auto const& member : op.Members)
			{
				if (member.Transport == transportId && !member.Aboard)
				{
					count++;
					size += member.Size;
				}
			}
		};

		std::vector<NeuralOpMember*> unseated;

		for (auto& member : op.Members)
		{
			if (member.Aboard)
				continue;

			if (member.Transport)
			{
				auto const pRecord = recordOf(member.Transport);

				if (!pRecord || pRecord->Leg == NeuralOpLeg::Crossing || pRecord->Leg == NeuralOpLeg::Returning)
					member.Transport = 0;
			}

			if (!member.Transport)
				unseated.push_back(&member);
		}

		std::stable_sort(unseated.begin(), unseated.end(), [](const NeuralOpMember* a, const NeuralOpMember* b) { return a->Size > b->Size; });

		auto const seatOn = [&](NeuralOpMember* pMember, FootClass* pTransport, OpCarrier carrier) -> bool
		{
			auto const pFoot = find(pMember->UniqueID);

			if (!pFoot || !OpCarrierTakes(pTransport, carrier, pFoot))
				return false;

			int count = 0;
			int size = 0;
			pendingOf(pTransport->UniqueID, count, size);

			if (carrier == OpCarrier::Carryall)
				return count == 0 && pTransport->Passengers.NumPassengers == 0;

			return CanPassengerFitInTransport(pTransport, pFoot->GetTechnoType(), count, size);
		};

		for (auto const pMember : unseated)
		{
			NeuralOpTransport* pBest = nullptr;
			int bestRoom = -1;

			for (auto& transport : op.Transports)
			{
				if (transport.Leg != NeuralOpLeg::ToStaging && transport.Leg != NeuralOpLeg::Parked)
					continue;

				auto const pTransport = find(transport.UniqueID);
				const OpCarrier carrier = GetOpCarrier(pTransport);

				if (!seatOn(pMember, pTransport, carrier))
					continue;

				int count = 0;
				int size = 0;
				pendingOf(transport.UniqueID, count, size);
				const int room = carrier == OpCarrier::Carryall ? 1
					: pTransport->GetTechnoType()->Passengers - pTransport->Passengers.GetTotalSize() - size;

				if (room > bestRoom)
				{
					bestRoom = room;
					pBest = &transport;
				}
			}

			if (pBest)
				pMember->Transport = pBest->UniqueID;
		}

		std::erase_if(unseated, [](const NeuralOpMember* pMember) { return pMember->Transport != 0; });

		// Still without a seat: idle transports join, the nearest to the staging beach first, each one
		// that has a seat for a member left (user: idle transports first, the factories build the rest).
		if (!unseated.empty() && pStagingCell)
		{
			struct OpCandidate
			{
				FootClass* Foot;
				OpCarrier Carrier;
				double Distance;
			};

			std::vector<OpCandidate> candidates;

			auto const consider = [&](FootClass* pFoot)
			{
				if (!pFoot || pFoot->Owner != pHouse)
					return;

				const OpCarrier carrier = GetOpCarrier(pFoot);

				if (!IsTransportFreeForOp(pHouse, pFoot, carrier))
					return;

				// It has to make the crossing: from where it stands to the staging beach, and on to the landing.
				auto const pType = pFoot->GetTechnoType();

				if (!CanTransportTypeLink(pType, pFoot->GetMapCoords(), op.Staging) || !CanTransportTypeLink(pType, op.Staging, op.Landing))
					return;

				candidates.push_back({ pFoot, carrier, pFoot->GetMapCoords().DistanceFrom(op.Staging) });
			};

			for (auto const pUnit : UnitClass::Array)
				consider(pUnit);

			for (auto const pAir : AircraftClass::Array)
				consider(pAir);

			std::stable_sort(candidates.begin(), candidates.end(),
				[](const OpCandidate& a, const OpCandidate& b) { return a.Distance < b.Distance; });

			for (auto const& candidate : candidates)
			{
				if (unseated.empty())
					break;

				bool seated = false;

				for (auto const pMember : unseated)
				{
					if (!seatOn(pMember, candidate.Foot, candidate.Carrier))
						continue;

					pMember->Transport = candidate.Foot->UniqueID;
					seated = true;

					if (candidate.Carrier == OpCarrier::Carryall)
						break;
				}

				if (!seated)
					continue;

				std::erase_if(unseated, [](const NeuralOpMember* pMember) { return pMember->Transport != 0; });

				NeuralOpTransport transport {};
				transport.UniqueID = candidate.Foot->UniqueID;
				transport.Leg = NeuralOpLeg::ToStaging;
				transport.LegFrame = now;
				op.Transports.push_back(transport);
				op.TransportsTaken++;
				macro.TransportOpOf[candidate.Foot->UniqueID] = op.Id;
				foots[candidate.Foot->UniqueID] = candidate.Foot;

				// An old trip record of the ferry loader is stale on an empty transport.
				ClearFerryTrip(candidate.Foot->UniqueID);
				candidate.Foot->SetTarget(nullptr);
				wake(candidate.Foot);

				Debug::Log("[NeuralAI] House %d transport op %d takes %s %u at (%d,%d), %d cells from the staging beach (%d,%d), frame %d.\n",
					houseIndex, op.Id, candidate.Foot->GetTechnoType()->ID, candidate.Foot->UniqueID, candidate.Foot->GetMapCoords().X,
					candidate.Foot->GetMapCoords().Y, static_cast<int>(candidate.Distance), op.Staging.X, op.Staging.Y, now);
			}
		}

		// No transport for long: the members go back to their client and may ask again later.
		if (!op.Transports.empty())
		{
			op.LastTransportFrame = now;
		}
		else if (now - op.LastTransportFrame >= OpNoTransportFrames || now < op.LastTransportFrame)
		{
			endOp("no transport came", true);
			return;
		}

		// 5. Members waiting on the map: aboard the parked transport they are bound for (TakeAboardNow,
		// once next to it at rest), walking up to a cell next to it, or waiting at the gathering spot.
		for (auto& member : op.Members)
		{
			if (member.Aboard)
				continue;

			auto const pFoot = find(member.UniqueID);
			checkInterference(pFoot, member.OrderCell, false);
			auto const pRecord = member.Transport ? recordOf(member.Transport) : nullptr;
			auto const pTransport = pRecord ? find(pRecord->UniqueID) : nullptr;
			const OpCarrier carrier = pTransport ? GetOpCarrier(pTransport) : OpCarrier::None;

			if (pTransport && pRecord->Leg == NeuralOpLeg::Parked && carrier != OpCarrier::Carryall)
			{
				if (TakeAboardNow(pTransport, pFoot))
				{
					member.Aboard = true;
					op.LastBoardFrame = now;
					aboardCount++;
					continue;
				}

				const bool beside = std::abs(pFoot->Location.X - pTransport->Location.X) < BoardingReachLeptons
					&& std::abs(pFoot->Location.Y - pTransport->Location.Y) < BoardingReachLeptons;

				// An order to enter makes the transport set out for the unit instead of waiting for it.
				if (pFoot->CurrentMission == Mission::Enter || pFoot->QueuedMission == Mission::Enter)
				{
					if (pFoot->ContainsLink(pTransport))
						pFoot->SendCommand(RadioCommand::NotifyUnlink, pTransport);

					pFoot->SetDestination(nullptr, true);
					pFoot->ClearNavigationList();
					pFoot->ForceMission(Mission::Guard);
				}
				else if (!beside)
				{
					auto const pCell = BoardingCellNextTo(pTransport, pFoot);
					orderTo(pFoot, member.OrderCell, pCell ? pCell : pGatherCell, pCell ? 0.5 : OpGatherCells);
				}

				continue;
			}

			// A carryall coming for it: it holds still where it stands once the carryall is near.
			if (pTransport && carrier == OpCarrier::Carryall && pTransport->DistanceFrom(pFoot) <= 10 * Unsorted::LeptonsPerCell)
			{
				if (pFoot->Destination || pFoot->CurrentMission == Mission::Move)
				{
					pFoot->SetDestination(nullptr, true);
					pFoot->QueueMission(Mission::Guard, false);
				}

				member.OrderCell = CellStruct::Empty;
				continue;
			}

			if (pGatherCell && pFoot->GetMapCoords().DistanceFrom(op.Gather) > OpGatherCells)
				orderTo(pFoot, member.OrderCell, pGatherCell, OpGatherCells - 1.0);
		}

		// 6. The load sails when every member bound for a transport of the beach is aboard, or
		// OpBoardingCapFrames after the first transport parked (user: all loaded, with a time cap).
		// Carryalls go as soon as their vehicle hangs under them.
		bool anyLoadedParked = false;
		bool allAboard = true;

		for (auto const& transport : op.Transports)
		{
			if (transport.Leg != NeuralOpLeg::Parked)
				continue;

			// A transport still parked after the last load sailed starts the wait of the next one.
			if (op.BoardingSince < 0)
				op.BoardingSince = now;

			if (find(transport.UniqueID)->Passengers.NumPassengers > 0)
				anyLoadedParked = true;
		}

		for (auto const& member : op.Members)
		{
			if (member.Aboard || !member.Transport)
				continue;

			auto const pRecord = recordOf(member.Transport);

			if (pRecord && (pRecord->Leg == NeuralOpLeg::Parked || pRecord->Leg == NeuralOpLeg::ToStaging)
				&& GetOpCarrier(find(pRecord->UniqueID)) != OpCarrier::Carryall)
			{
				allAboard = false;
			}
		}

		auto const depart = [&](NeuralOpTransport& transport, FootClass* pTransport, OpCarrier carrier)
		{
			transport.Leg = NeuralOpLeg::Crossing;
			transport.LegFrame = now;
			transport.Tries = 0;
			transport.ParkFrame = -1;
			transport.ParkCell = CellStruct::Empty;
			auto const pLanding = PickOpLandingCell(op, pTransport, carrier, op.Landing, CellStruct::Empty);
			transport.Landing = pLanding ? pLanding->MapCoords : op.Landing;
			transport.Waypoint = carrier == OpCarrier::Surface
				? FindSafeFerryWaypoint(pHouse, pTransport->GetMapCoords(), transport.Landing, pTransport->GetTechnoType())
				: CellStruct::Empty;
			transport.ProgressCell = pTransport->GetMapCoords();
			transport.ProgressFrame = now;
			wake(pTransport);

			Debug::Log("[NeuralAI] House %d transport op %d: %s %u sails from (%d,%d) with %d aboard (size %d/%d) for (%d,%d)%s, frame %d.\n",
				houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, pTransport->GetMapCoords().X,
				pTransport->GetMapCoords().Y, pTransport->Passengers.NumPassengers, pTransport->Passengers.GetTotalSize(),
				pTransport->GetTechnoType()->Passengers, transport.Landing.X, transport.Landing.Y,
				transport.Waypoint != CellStruct::Empty ? " by a detour" : "", now);
		};

		const bool capReached = op.BoardingSince >= 0 && (now - op.BoardingSince >= OpBoardingCapFrames || now < op.BoardingSince);

		if (anyLoadedParked && (allAboard || capReached))
		{
			for (auto& transport : op.Transports)
			{
				auto const pTransport = find(transport.UniqueID);

				if (transport.Leg == NeuralOpLeg::Parked && pTransport->Passengers.NumPassengers > 0)
					depart(transport, pTransport, GetOpCarrier(pTransport));
			}

			for (auto& member : op.Members)
			{
				if (member.Aboard || !member.Transport)
					continue;

				auto const pRecord = recordOf(member.Transport);

				if (!pRecord || pRecord->Leg == NeuralOpLeg::Crossing)
					member.Transport = 0;
			}

			op.Trips++;
			op.BoardingSince = -1;
		}
		else if (op.BoardingSince >= 0 && now - op.BoardingSince >= 2 * OpBoardingCapFrames && op.LastBoardFrame < op.BoardingSince
			&& aboardCount == 0)
		{
			// TEMP diagnostic: why nobody went aboard a transport parked this long.
			for (auto const& member : op.Members)
			{
				auto const pRecord = member.Transport ? recordOf(member.Transport) : nullptr;

				if (!pRecord || pRecord->Leg != NeuralOpLeg::Parked)
					continue;

				auto const pFoot = find(member.UniqueID);
				auto const pTransport = find(pRecord->UniqueID);
				auto const pRefusal = BoardingRefusal(pTransport, pFoot);
				Debug::Log("[NeuralAI] House %d transport op %d boarding DIAG: %s %u %d leptons from %s %u (mission %d, height %d, moving %d, "
					"destination %d), its mission %d, refusal: %s, frame %d.\n", houseIndex, op.Id, pFoot->GetTechnoType()->ID, pFoot->UniqueID,
					pFoot->DistanceFrom(pTransport), pTransport->GetTechnoType()->ID, pTransport->UniqueID,
					static_cast<int>(pTransport->CurrentMission), pTransport->GetHeight(), pTransport->Locomotor->Is_Moving() ? 1 : 0,
					pTransport->Destination ? 1 : 0, static_cast<int>(pFoot->CurrentMission), pRefusal ? pRefusal : "none", now);
				break;
			}

			endOp("nobody boarded", true);
			return;
		}

		if (op.FailedTrips >= OpMaxFailedTrips && std::none_of(op.Transports.begin(), op.Transports.end(),
			[](const NeuralOpTransport& transport) { return transport.Leg == NeuralOpLeg::Crossing || transport.Leg == NeuralOpLeg::Returning; }))
		{
			endOp("crossings failed", true);
			return;
		}

		// 7. Transports.
		auto const hasSeated = [&op](DWORD transportId)
		{
			return std::any_of(op.Members.begin(), op.Members.end(),
				[transportId](const NeuralOpMember& member) { return member.Transport == transportId && !member.Aboard; });
		};

		std::vector<DWORD> released;

		// Its load is off: back to the staging beach for the members still waiting (user: they go back
		// for more), else it is let go.
		auto const afterUnload = [&](NeuralOpTransport& transport)
		{
			if (waitingSize > 0)
			{
				transport.Leg = NeuralOpLeg::ToStaging;
				transport.LegFrame = now;
				transport.ParkCell = CellStruct::Empty;
				transport.ParkFrame = -1;
				transport.Tries = 0;
				transport.Waypoint = CellStruct::Empty;
			}
			else
			{
				released.push_back(transport.UniqueID);
			}
		};

		// Sets the load down at the landing: on the ground at once (PutAshoreNow; the unload mission
		// waits for a hovercraft to report no movement, which it does not while it floats); flying, it is
		// told to unload, which brings it down. Still loaded OpUnloadRetryFrames later, it tries another
		// cell.
		auto const unloadAt = [&](NeuralOpTransport& transport, FootClass* pTransport, OpCarrier carrier, bool aloft)
		{
			const int aboardBefore = pTransport->Passengers.NumPassengers;

			if (!aloft && carrier != OpCarrier::Carryall)
			{
				const int out = PutAshoreNow(pTransport);

				if (out > 0)
				{
					Debug::Log("[NeuralAI] House %d transport op %d: %s %u sets %d of %d down at (%d,%d), %d frames after sailing, frame %d.\n",
						houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, out, aboardBefore,
						pTransport->GetMapCoords().X, pTransport->GetMapCoords().Y, now - transport.LegFrame, now);
					transport.ParkFrame = now;
					return;
				}
			}

			auto const pLandingCell = MapClass::Instance.TryGetCellAt(transport.Landing);

			if (transport.ParkFrame < 0)
				transport.ParkFrame = now;

			if (now - transport.ParkFrame >= OpUnloadRetryFrames)
			{
				transport.ParkFrame = now;
				transport.Tries++;
				auto const pNext = carrier == OpCarrier::Surface
					? PickOpLandingCell(op, pTransport, carrier, transport.Landing, transport.Landing)
					: PickOpFlyerCell(transport.Landing, op.TargetZone, 1, transport.Landing, {});

				if (pNext)
				{
					Debug::Log("[NeuralAI] House %d transport op %d: %s %u could not set down at (%d,%d), tries (%d,%d), frame %d.\n",
						houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, transport.Landing.X, transport.Landing.Y,
						pNext->MapCoords.X, pNext->MapCoords.Y, now);
					transport.Landing = pNext->MapCoords;
					wake(pTransport);
					pTransport->SetDestination(pNext, true);
					pTransport->QueueMission(carrier == OpCarrier::Surface ? Mission::Move : Mission::Unload, true);
					transport.OrderCell = pNext->MapCoords;
				}

				return;
			}

			if (pTransport->CurrentMission != Mission::Unload && pLandingCell)
			{
				wake(pTransport);
				pTransport->SetDestination(pLandingCell, true);
				pTransport->QueueMission(Mission::Unload, true);
				transport.OrderCell = transport.Landing;
			}
		};

		for (auto& transport : op.Transports)
		{
			auto const pTransport = find(transport.UniqueID);
			const OpCarrier carrier = GetOpCarrier(pTransport);
			auto const pUnit = abstract_cast<UnitClass*>(pTransport);
			const bool aloft = carrier == OpCarrier::Carryall ? pTransport->IsInAir() : IsFerryAloft(pTransport);
			const int loaded = pTransport->Passengers.NumPassengers;
			auto const pAt = pTransport->GetCell();
			const bool onLand = pAt && pAt->LandType != LandType::Water;
			const bool onTheMove = pTransport->Destination || pTransport->CurrentMission == Mission::Move
				|| pTransport->QueuedMission == Mission::Move;

			if (transport.Leg != NeuralOpLeg::Parked)
				checkInterference(pTransport, transport.OrderCell, true);

			// No weapons: its job is carrying (user). A carryall's target is the vehicle it lifts.
			if (carrier != OpCarrier::Carryall && pTransport->Target)
				pTransport->SetTarget(nullptr);

			// Badly hit on land: it lets its load out at once (the members on their own land wait for
			// another seat, those on the land of their address are delivered) and leaves the operation.
			// Kept, it took its load aboard again at every tick and set it down again (33 times in a row,
			// match #10391).
			if (carrier != OpCarrier::Carryall && !aloft && onLand && pTransport->GetHealthPercentage() < OpDyingTransportHealth)
			{
				const int out = loaded > 0 ? PutAshoreNow(pTransport) : 0;

				if (pTransport->Passengers.NumPassengers == 0)
				{
					Debug::Log("[NeuralAI] House %d transport op %d: %s %u badly hit, sets %d down at (%d,%d) and leaves, frame %d.\n",
						houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, out, pTransport->GetMapCoords().X,
						pTransport->GetMapCoords().Y, now);
					released.push_back(transport.UniqueID);
					continue;
				}
			}

			switch (transport.Leg)
			{
			case NeuralOpLeg::ToStaging:
			{
				if (carrier == OpCarrier::Carryall)
				{
					// It fetches its vehicle where it waits; once the vehicle hangs under it, it goes.
					if (loaded > 0)
					{
						depart(transport, pTransport, carrier);
						break;
					}

					NeuralOpMember* pCargo = nullptr;

					for (auto& member : op.Members)
					{
						if (member.Transport == transport.UniqueID && !member.Aboard)
						{
							pCargo = &member;
							break;
						}
					}

					auto const pVehicle = pCargo ? abstract_cast<UnitClass*>(find(pCargo->UniqueID)) : nullptr;

					if (!pVehicle)
					{
						released.push_back(transport.UniqueID);
						break;
					}

					if (pTransport->CurrentMission != Mission::Capture || pTransport->Target != pVehicle)
						AdvanceRoutedPickup(pHouse, pTransport, pVehicle, false);

					transport.OrderCell = CellStruct::Empty;
					break;
				}

				if (loaded == 0 && !hasSeated(transport.UniqueID) && waitingSize == 0)
				{
					released.push_back(transport.UniqueID);
					break;
				}

				const double toStaging = pTransport->GetMapCoords().DistanceFrom(op.Staging);

				if (toStaging > OpParkCells)
				{
					auto const pGoal = carrier == OpCarrier::Flyer ? GetSafeAirHop(pHouse, pTransport, pStagingCell) : pStagingCell;
					orderTo(pTransport, transport.OrderCell, pGoal, 2.0);
					break;
				}

				auto const park = [&]()
				{
					transport.Leg = NeuralOpLeg::Parked;
					transport.LegFrame = now;
					pTransport->SetDestination(nullptr, true);
					pTransport->ForceMission(Mission::Sleep);
					transport.OrderCell = CellStruct::Empty;

					if (op.BoardingSince < 0)
						op.BoardingSince = now;
				};

				if (carrier == OpCarrier::Flyer)
				{
					// Down with no move left to make: parked. Else it is sent to a free cell off the beach two
					// cells from every member; hanging over it FerryLandingFrames, it tries the next ring.
					if (!aloft && !onTheMove)
					{
						park();
						break;
					}

					const bool hanging = transport.ParkCell != CellStruct::Empty && transport.ParkFrame >= 0
						&& now - transport.ParkFrame >= FerryLandingFrames;

					if (transport.ParkCell == CellStruct::Empty || hanging)
					{
						const int ring = hanging ? std::min(transport.Tries + 2, FerryLandingMaxRing) : 1;

						if (auto const pCell = PickOpFlyerCell(op.Staging, op.OriginZone, ring, transport.ParkCell, memberCells))
						{
							if (hanging)
								transport.Tries++;

							transport.ParkCell = pCell->MapCoords;
							transport.ParkFrame = now;
							wake(pTransport);
							pTransport->SetArchiveTarget(nullptr);
							pTransport->SetDestination(pCell, true);
							pTransport->QueueMission(Mission::Move, true);
							transport.OrderCell = pCell->MapCoords;
						}
					}
					else if (auto const pGoing = abstract_cast<CellClass*>(pTransport->Destination); !pGoing && aloft)
					{
						if (auto const pPark = MapClass::Instance.TryGetCellAt(transport.ParkCell))
						{
							pTransport->SetDestination(pPark, true);
							pTransport->QueueMission(Mission::Move, true);
							transport.OrderCell = transport.ParkCell;
						}
					}

					break;
				}

				// On the ground: on to the nearest cell of the origin land its passengers can step next to,
				// then it stops there. Not on water: there nobody steps in.
				if (pUnit && MoveTransportToLoadingCell(pHouse, pUnit, pTransport->GetMapCoords(), op.OriginZone, true))
				{
					transport.OrderCell = pTransport->Destination ? CellClass::Coord2Cell(pTransport->Destination->GetCoords()) : CellStruct::Empty;
					break;
				}

				if (onLand && TransportLandZone(pTransport->GetMapCoords(), pTransport->OnBridge) == op.OriginZone)
					park();
				else
					orderTo(pTransport, transport.OrderCell, pStagingCell, 0.5);

				break;
			}
			case NeuralOpLeg::Parked:
			{
				// Took off or was pushed away: it parks anew.
				if ((carrier == OpCarrier::Flyer && aloft) || pTransport->GetMapCoords().DistanceFrom(op.Staging) > OpParkCells + 6.0)
				{
					transport.Leg = NeuralOpLeg::ToStaging;
					transport.LegFrame = now;
					transport.ParkCell = CellStruct::Empty;
					break;
				}

				// Empty with nobody bound for it: let go.
				if (loaded == 0 && !hasSeated(transport.UniqueID))
				{
					released.push_back(transport.UniqueID);
					break;
				}

				// Moved by someone else: stopped again. It sleeps (on Guard a transport with a gun takes off
				// or drives at whatever comes into range).
				if (onTheMove)
				{
					checkInterference(pTransport, transport.OrderCell, true);
					pTransport->SetDestination(nullptr, true);
				}

				if (pTransport->CurrentMission != Mission::Sleep)
					pTransport->ForceMission(Mission::Sleep);

				if (!aloft && pTransport->Locomotor->Is_Moving() && IsFootHeldStill(pHouse, pTransport, BoardingSettleFrames))
					pTransport->Locomotor->Stop_Moving();

				break;
			}
			case NeuralOpLeg::Crossing:
			{
				if (loaded == 0)
				{
					afterUnload(transport);
					break;
				}

				auto const pLandingCell = MapClass::Instance.TryGetCellAt(transport.Landing);
				const double toLanding = pTransport->GetMapCoords().DistanceFrom(transport.Landing);
				const double unloadCells = carrier == OpCarrier::Carryall ? 3.0 : OpUnloadCells;

				if (toLanding <= unloadCells && (onLand || aloft))
				{
					if (carrier == OpCarrier::Carryall)
					{
						if (pTransport->CurrentMission != Mission::Unload && pLandingCell)
						{
							pTransport->SetDestination(pLandingCell, true);
							pTransport->QueueMission(Mission::Unload, true);
							transport.OrderCell = transport.Landing;
						}
					}
					else
					{
						unloadAt(transport, pTransport, carrier, aloft);
					}

					break;
				}

				if (now - transport.LegFrame >= OpCrossCapFrames || now < transport.LegFrame)
				{
					Debug::Log("[NeuralAI] House %d transport op %d: %s %u still crossing after %d frames, %d cells from (%d,%d): takes its "
						"load back, frame %d.\n", houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID,
						now - transport.LegFrame, static_cast<int>(toLanding), transport.Landing.X, transport.Landing.Y, now);
					transport.Leg = NeuralOpLeg::Returning;
					transport.LegFrame = now;
					transport.ParkFrame = -1;
					op.FailedTrips++;
					break;
				}

				// Progress watchdog: stuck OpStuckFrames, it tries another landing; after OpMaxLandingTries it
				// sets its load down on the land it stands by, else takes it back.
				if (transport.ProgressCell == CellStruct::Empty || pTransport->GetMapCoords().DistanceFrom(transport.ProgressCell) >= OpStuckCells)
				{
					transport.ProgressCell = pTransport->GetMapCoords();
					transport.ProgressFrame = now;
				}
				else if (now - transport.ProgressFrame >= OpStuckFrames || now < transport.ProgressFrame)
				{
					transport.ProgressFrame = now;
					transport.Tries++;

					if (transport.Tries > OpMaxLandingTries)
					{
						if (!aloft && onLand && carrier != OpCarrier::Carryall && PutAshoreNow(pTransport) > 0)
						{
							Debug::Log("[NeuralAI] House %d transport op %d: %s %u stuck, sets its load down at (%d,%d), frame %d.\n",
								houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, pTransport->GetMapCoords().X,
								pTransport->GetMapCoords().Y, now);
						}
						else
						{
							transport.Leg = NeuralOpLeg::Returning;
							transport.LegFrame = now;
							transport.ParkFrame = -1;
							op.FailedTrips++;
						}

						break;
					}

					if (auto const pNext = PickOpLandingCell(op, pTransport, carrier, op.Landing, transport.Landing))
					{
						Debug::Log("[NeuralAI] House %d transport op %d: %s %u stuck at (%d,%d) on its way to (%d,%d), tries (%d,%d), frame %d.\n",
							houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, pTransport->GetMapCoords().X,
							pTransport->GetMapCoords().Y, transport.Landing.X, transport.Landing.Y, pNext->MapCoords.X, pNext->MapCoords.Y, now);
						transport.Landing = pNext->MapCoords;
						transport.Waypoint = CellStruct::Empty;
					}
				}

				// Along the safe lane: the detour first, then straight in; a flyer hops around the anti-air.
				if (transport.Waypoint != CellStruct::Empty && pTransport->GetMapCoords().DistanceFrom(transport.Waypoint) <= 4.0)
					transport.Waypoint = CellStruct::Empty;

				auto const pLanding = MapClass::Instance.TryGetCellAt(transport.Landing);
				CellClass* pGoal = pLanding;

				if (carrier != OpCarrier::Surface)
					pGoal = GetSafeAirHop(pHouse, pTransport, pLanding);
				else if (transport.Waypoint != CellStruct::Empty)
					pGoal = MapClass::Instance.TryGetCellAt(transport.Waypoint);

				orderTo(pTransport, transport.OrderCell, pGoal, 1.0);
				break;
			}
			case NeuralOpLeg::Returning:
			{
				// A load it could not set down goes back to the staging beach and gets out there.
				if (loaded == 0)
				{
					afterUnload(transport);
					break;
				}

				if (pTransport->GetMapCoords().DistanceFrom(op.Staging) <= OpParkCells && (onLand || aloft))
				{
					if (!aloft && carrier != OpCarrier::Carryall && PutAshoreNow(pTransport) > 0)
						break;

					if (pTransport->CurrentMission != Mission::Unload && pStagingCell)
					{
						wake(pTransport);
						pTransport->SetDestination(pStagingCell, true);
						pTransport->QueueMission(Mission::Unload, true);
						transport.OrderCell = op.Staging;
					}

					break;
				}

				auto const pGoal = carrier == OpCarrier::Surface ? pStagingCell : GetSafeAirHop(pHouse, pTransport, pStagingCell);
				orderTo(pTransport, transport.OrderCell, pGoal, 1.0);
				break;
			}
			default:
				break;
			}
		}

		for (const DWORD uid : released)
		{
			if (auto const pTransport = find(uid))
				wake(pTransport);

			macro.TransportOpOf.erase(uid);

			for (auto& member : op.Members)
			{
				if (member.Transport == uid && !member.Aboard)
					member.Transport = 0;
			}

			std::erase_if(op.Transports, [uid](const NeuralOpTransport& transport) { return transport.UniqueID == uid; });
		}

		if (!logNow)
			return;

		int waiting = 0;
		int seated = 0;

		for (auto const& member : op.Members)
		{
			if (!member.Aboard)
			{
				waiting++;

				if (member.Transport)
					seated++;
			}
		}

		int legs[4] = {};

		for (auto const& transport : op.Transports)
			legs[std::clamp(static_cast<int>(transport.Leg), 0, 3)]++;

		Debug::Log("[NeuralAI] House %d transport op %d (%s) zone %d -> %d: %d waiting (%d with a seat, size %d), %d aboard, %d landed, "
			"%d lost; transports %d to staging, %d parked, %d crossing, %d returning; %d trips, staging (%d,%d), landing (%d,%d), "
			"%d interferences, %d frames old, frame %d.\n", houseIndex, op.Id, OpClientName(op.Client), op.OriginZone, op.TargetZone,
			waiting, seated, waitingSize, aboardCount, op.Landed, op.Lost, legs[0], legs[1], legs[2], legs[3], op.Trips, op.Staging.X,
			op.Staging.Y, op.Landing.X, op.Landing.Y, op.Interferences, now - op.CreatedFrame, now);
	}

	// Runs the house's transport operations once per AI tick, after every other part of the tick
	// has given its orders (the operations have the last word) and before production reads what the
	// waiting members ask for (OpWaitingSizeBySize).
	void UpdateTransportOperations(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		macro.OpWaitingSizeBySize.fill(0);
		macro.OpWaitingFrame = now;

		if (macro.TransportOps.empty())
		{
			macro.OpWatch.clear();
			return;
		}

		// The units the operations hold, by UniqueID, those aboard a transport too (looked up by key
		// only: the operations act in their own fixed order).
		std::unordered_map<DWORD, FootClass*> foots;

		auto const collect = [&](FootClass* pFoot)
		{
			if (pFoot && pFoot->Owner == pHouse && pFoot->IsAlive && pFoot->Health > 0 && macro.TransportOpOf.contains(pFoot->UniqueID))
				foots[pFoot->UniqueID] = pFoot;
		};

		for (auto const pUnit : UnitClass::Array)
			collect(pUnit);

		for (auto const pInf : InfantryClass::Array)
			collect(pInf);

		for (auto const pAir : AircraftClass::Array)
			collect(pAir);

		const bool logNow = macro.TransportOpLogFrame < 0 || now - macro.TransportOpLogFrame >= OpLogFrames || now < macro.TransportOpLogFrame;

		if (logNow)
			macro.TransportOpLogFrame = now;

		for (size_t i = 0; i < macro.TransportOps.size(); ++i)
		{
			if (!macro.TransportOps[i].Over)
				UpdateTransportOperation(pHouse, macro.TransportOps[i], foots, logNow);
		}

		std::erase_if(macro.TransportOps, [](const NeuralTransportOperation& op) { return op.Over; });

		// TEMP trace: the units as the operations leave them, for TraceTransportOpUnits (16 at most).
		macro.OpWatch.clear();

		for (auto const& op : macro.TransportOps)
		{
			for (auto const& member : op.Members)
			{
				auto const it = foots.find(member.UniqueID);

				if (!member.Aboard && it != foots.end() && !it->second->InLimbo && macro.OpWatch.size() < 16)
					macro.OpWatch.push_back({ member.UniqueID, it->second, it->second->CurrentMission, it->second->MegaMission, it->second->Destination });
			}

			for (auto const& transport : op.Transports)
			{
				auto const it = foots.find(transport.UniqueID);

				if (it != foots.end() && macro.OpWatch.size() < 16)
					macro.OpWatch.push_back({ transport.UniqueID, it->second, it->second->CurrentMission, it->second->MegaMission, it->second->Destination });
			}
		}

		// Retry marks past their time go.
		if (macro.TransportOpRetryUntil.size() > 256)
			std::erase_if(macro.TransportOpRetryUntil, [now](const auto& entry) { return now >= entry.second; });
	}
}
