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
		case NeuralOpClient::Shuttle:
			return "shuttle";
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

		if (IsInTransportOp(pHouse, uid) || IsPlanMember(pHouse, uid) || IsFerryParkedToLoad(pHouse, uid)
			|| IsOnRepairTrip(pHouse, uid) || IsActiveScout(pHouse, uid) || IsSneakStrikeActive(pHouse, uid))
		{
			return false;
		}

		// A carryall on its way to a vehicle (a move sent to it) is lifting it.
		if (carrier == OpCarrier::Carryall)
		{
			return pFoot->CurrentMission != Mission::Unload && !abstract_cast<UnitClass*>(pFoot->Destination)
				&& !FindPendingPickupCargo(pHouse, pFoot);
		}

		auto const pCarryState = UnitExt::Fetch(static_cast<UnitClass*>(pFoot));

		return !(pCarryState && pCarryState->CarryallCargo);
	}

	// TEMP diagnostic (rule consolidation, 2026-11-19): the first check that turns a transport of ours
	// down for an operation, in the order of IsTransportFreeForOp and then of the crossing its
	// candidates must make. It changes nothing: the pending pickup of a carryall is looked up, not
	// pruned (the candidate search already pruned it this tick).
	const char* DescribeOpTransportRefusal(HouseClass* pHouse, FootClass* pFoot, OpCarrier carrier, const CellStruct& staging,
		const CellStruct& landing)
	{
		if (pFoot->InLimbo)
			return "in limbo";

		if (!pFoot->IsAlive || pFoot->Health <= 0 || pFoot->IsCrashing || pFoot->IsSinking)
			return "dying";

		if (pFoot->GetHealthPercentage() < OpTransportMinHealth)
			return "damaged";

		if (pFoot->Passengers.NumPassengers > 0)
			return "loaded";

		if (pFoot->IsMindControlled() || pFoot->Deactivated || pFoot->IsUnderEMP())
			return "disabled";

		if (pFoot->HasAnyLink())
			return "linked";

		const DWORD uid = pFoot->UniqueID;

		if (IsInTransportOp(pHouse, uid))
			return "in an operation";

		if (IsPlanMember(pHouse, uid))
			return "in a plan";

		if (IsFerryParkedToLoad(pHouse, uid))
			return "parked by the ferry loader";

		if (IsOnRepairTrip(pHouse, uid))
			return "on a repair trip";

		if (IsActiveScout(pHouse, uid))
			return "scouting";

		if (IsSneakStrikeActive(pHouse, uid))
			return "on a sneak strike";

		if (carrier == OpCarrier::Carryall)
		{
			if (pFoot->CurrentMission == Mission::Unload || abstract_cast<UnitClass*>(pFoot->Destination) || AirPickups.contains(uid))
				return "lifting a vehicle";
		}
		else
		{
			auto const pCarryState = UnitExt::Fetch(static_cast<UnitClass*>(pFoot));

			if (pCarryState && pCarryState->CarryallCargo)
				return "slinging a vehicle";
		}

		auto const pType = pFoot->GetTechnoType();

		if (!CanTransportTypeLink(pType, pFoot->GetMapCoords(), staging))
			return "cannot come to the staging beach";

		if (!CanTransportTypeLink(pType, staging, landing))
			return "cannot make the crossing";

		return "free";
	}

	// TEMP diagnostic (rule consolidation, 2026-11-19): an operation that no transport takes names
	// each transport of ours (OpRefusalLogTransports at most) with the check that turns it down, and
	// whether CanHouseCarryNow still counts one that comes (then the operation waits
	// OpNoTransportFrames). OpRefusalLogLines lines a match.
	void LogOpTransportRefusals(HouseClass* pHouse, const NeuralTransportOperation& op, int unseated, int largest, bool reachable)
	{
		static int refusalLogSerial = -1;
		static int refusalLogCount = 0;

		if (refusalLogSerial != NeuralMatchSerial)
		{
			refusalLogSerial = NeuralMatchSerial;
			refusalLogCount = 0;
		}

		if (refusalLogCount >= OpRefusalLogLines)
			return;

		++refusalLogCount;
		char list[1536] = "";
		size_t used = 0;
		int owned = 0;
		int listed = 0;

		auto const note = [&](FootClass* pFoot)
		{
			if (!pFoot || pFoot->Owner != pHouse)
				return;

			const OpCarrier carrier = GetOpCarrier(pFoot);

			if (carrier == OpCarrier::None)
				return;

			owned++;

			if (listed >= OpRefusalLogTransports || used + 1 >= sizeof(list))
				return;

			auto const coords = pFoot->GetMapCoords();
			const int written = std::snprintf(list + used, sizeof(list) - used, "%s%s %u at (%d,%d) %d%% %s", listed ? "; " : "",
				pFoot->GetTechnoType()->ID, pFoot->UniqueID, coords.X, coords.Y, static_cast<int>(pFoot->GetHealthPercentage() * 100.0),
				DescribeOpTransportRefusal(pHouse, pFoot, carrier, op.Staging, op.Landing));

			if (written > 0)
				used = std::min(sizeof(list) - 1, used + static_cast<size_t>(written));

			listed++;
		};

		for (auto const pUnit : UnitClass::Array)
			note(pUnit);

		for (auto const pAir : AircraftClass::Array)
			note(pAir);

		Debug::Log("[NeuralAI] House %d transport op %d takes none of our %d transports, staging (%d,%d), landing (%d,%d), %d unseated, "
			"largest %d, one counted as coming %d, frame %d: %s.\n",
			pHouse->ArrayIndex, op.Id, owned, op.Staging.X, op.Staging.Y, op.Landing.X, op.Landing.Y, unseated, largest, reachable ? 1 : 0,
			Unsorted::CurrentFrame, owned ? list : "none");
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

	// A transport that flies: a jumpjet or flying transport, a carryall aircraft.
	bool OpTypeFlies(const TechnoTypeClass* pType)
	{
		return pType && (pType->WhatAmI() == AbstractType::AircraftType || pType->JumpJet || pType->MovementZone == MovementZone::Fly);
	}

	// What can hit a transport of an operation on its way (PlanOpRoute), as the loss log names it: a
	// known enemy defense, any other known enemy building, an enemy unit in sight or one seen lately,
	// a place where we lost such a transport. Its reach in cells and its weight per cell inside it.
	enum class OpThreatKind : uint8_t
	{
		Defense,
		Building,
		Unit,
		Sighting,
		Loss
	};

	struct OpThreat
	{
		CellStruct Cell;
		int Reach;
		int Weight;
		OpThreatKind Kind;
		const TechnoTypeClass* Type;
	};

	const char* OpThreatKindName(OpThreatKind kind)
	{
		switch (kind)
		{
		case OpThreatKind::Defense:
			return "defense";
		case OpThreatKind::Building:
			return "building";
		case OpThreatKind::Unit:
			return "unit in sight";
		case OpThreatKind::Sighting:
			return "unit seen lately";
		case OpThreatKind::Loss:
			return "loss spot";
		default:
			return "?";
		}
	}

	// How far in cells the weapons of a type hit a transport in the air or on the ground (0: none of
	// them hits it), OpRouteMaxReachCells at most. takes: one of those weapons takes the transport
	// (mind control).
	int OpThreatReach(const TechnoTypeClass* pType, bool airborne, bool& takes)
	{
		int reach = 0;
		takes = false;

		for (size_t i = 0; i < 2; ++i)
		{
			auto const pWeapon = pType->GetWeapon(i, false).WeaponType;

			if (!pWeapon || pWeapon->Damage <= 0)
				continue;

			auto const pProjectile = pWeapon->Projectile;

			if (pProjectile ? (airborne ? !pProjectile->AA : !pProjectile->AG) : airborne)
				continue;

			reach = std::max(reach, pWeapon->Range / Unsorted::LeptonsPerCell);

			if (pWeapon->Warhead && pWeapon->Warhead->MindControl)
				takes = true;
		}

		return std::min(reach, OpRouteMaxReachCells);
	}

	// Every threat a crossing in the air or on the ground keeps away from, as the house knows them.
	void CollectOpThreats(HouseClass* pHouse, bool airborne, std::vector<OpThreat>& threats)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		threats.clear();

		auto const isEnemy = [pHouse](HouseClass* pOwner)
		{
			return pOwner && pOwner != pHouse && !pHouse->IsAlliedWith(pOwner) && !pOwner->IsNeutral() && !pOwner->IsObserver()
				&& !pOwner->Defeated;
		};

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Type->Insignificant || pBld->Type->InvisibleInGame
				|| !isEnemy(pBld->Owner) || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			bool takes = false;
			const int reach = OpThreatReach(pBld->Type, airborne, takes);

			if (reach > 0)
			{
				threats.push_back({ pBld->GetMapCoords(), reach + OpRouteDefenseMarginCells, OpRouteDefenseWeight * (takes ? OpRouteTakeFactor : 1),
					OpThreatKind::Defense, pBld->Type });
			}
			else
				threats.push_back({ pBld->GetMapCoords(), OpRouteBaseCells, 1, OpThreatKind::Building, pBld->Type });
		}

		// Enemy units in sight. Aircraft count against flying transports only: where one is now says
		// little about where it strikes a ship. A cloaked unit is not seen.
		std::unordered_set<DWORD> inSight;
		std::unordered_set<DWORD> alive;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || pTechno->InLimbo || !pTechno->IsAlive || pTechno->WhatAmI() == AbstractType::Building || !isEnemy(pTechno->Owner))
				continue;

			alive.insert(pTechno->UniqueID);
			auto const pCell = pTechno->GetCell();

			if ((!airborne && pTechno->WhatAmI() == AbstractType::Aircraft) || !pCell || pTechno->CloakState == CloakState::Cloaked
				|| !IsCellVisibleToAI(pHouse, pCell))
			{
				continue;
			}

			inSight.insert(pTechno->UniqueID);
			auto const pType = pTechno->GetTechnoType();
			bool takes = false;
			const int reach = pType ? OpThreatReach(pType, airborne, takes) : 0;

			if (reach > 0)
			{
				threats.push_back({ pCell->MapCoords, reach + OpRouteUnitMarginCells, OpRouteUnitWeight * (takes ? OpRouteTakeFactor : 1),
					OpThreatKind::Unit, pType });
			}
		}

		// Those out of sight, where they were last seen, fading with the time since (the order of the
		// sightings does not matter: the danger is a sum).
		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			const int age = now - sighting.LastSeenFrame;

			if (!sighting.Type || sighting.LastCoords == CellStruct::Empty || age < 0 || age > OpRouteSightingFrames
				|| inSight.contains(uid) || !alive.contains(uid) || (!airborne && sighting.Category == EnemyForceCategory::Air))
			{
				continue;
			}

			bool takes = false;
			const int reach = OpThreatReach(sighting.Type, airborne, takes);

			if (reach > 0)
			{
				const int weight = std::max(1, OpRouteSightingWeight * (OpRouteSightingFrames - age) / OpRouteSightingFrames)
					* (takes ? OpRouteTakeFactor : 1);
				threats.push_back({ sighting.LastCoords, reach + OpRouteSightingMarginCells, weight, OpThreatKind::Sighting, sighting.Type });
			}
		}

		// Where we lost transports of this kind; for flying ones also where our flyers were shot down
		// (UpdateAirLossMemory), for the others the beaches where loaded ferries went down.
		for (auto const& spot : macro.OpLossSpots)
		{
			const int age = now - spot.Frame;

			if (spot.Airborne == airborne && age >= 0 && age < OpRouteLossFrames)
				threats.push_back({ spot.Cell, OpRouteLossCells, OpRouteLossWeight, OpThreatKind::Loss, nullptr });
		}

		if (airborne)
		{
			for (auto const& [spot, frame] : macro.AirDangerSpots)
			{
				const int age = now - frame;

				if (age >= 0 && age < 8100)
					threats.push_back({ spot, 7, 3 - age / 2700, OpThreatKind::Loss, nullptr });
			}
		}
		else
		{
			for (auto const& [cell, frame] : macro.BloodyBeaches)
			{
				const int age = now - frame;

				if (age >= 0 && age < OpRouteLossFrames)
					threats.push_back({ cell, OpRouteLossCells, OpRouteLossWeight / 2, OpThreatKind::Loss, nullptr });
			}
		}
	}

	// The danger of every block of the map (OpRouteBlockCells cells a side) for a crossing in the air
	// or on the ground, and the threats it was built from.
	struct OpDangerMap
	{
		int Left { 0 };
		int Top { 0 };
		int Width { 1 };
		int Height { 1 };
		std::vector<int> Danger;
		std::vector<OpThreat> Threats;

		int IndexOf(const CellStruct& cell) const
		{
			const int bx = std::clamp((cell.X - this->Left) / OpRouteBlockCells, 0, this->Width - 1);
			const int by = std::clamp((cell.Y - this->Top) / OpRouteBlockCells, 0, this->Height - 1);

			return by * this->Width + bx;
		}

		CellStruct CenterOf(int index) const
		{
			return { static_cast<short>(this->Left + (index % this->Width) * OpRouteBlockCells + OpRouteBlockCells / 2),
				static_cast<short>(this->Top + (index / this->Width) * OpRouteBlockCells + OpRouteBlockCells / 2) };
		}
	};

	// Built once per frame, house and kind of crossing.
	const OpDangerMap& GetOpDangerMap(HouseClass* pHouse, bool airborne)
	{
		static std::array<OpDangerMap, 2> maps;
		static std::array<int, 2> builtFrame { -1, -1 };
		static std::array<int, 2> builtHouse { -1, -1 };
		static std::array<int, 2> builtSerial { -1, -1 };
		const size_t kind = airborne ? 1 : 0;
		auto& map = maps[kind];

		if (builtFrame[kind] == Unsorted::CurrentFrame && builtHouse[kind] == pHouse->ArrayIndex && builtSerial[kind] == NeuralMatchSerial)
			return map;

		builtFrame[kind] = Unsorted::CurrentFrame;
		builtHouse[kind] = pHouse->ArrayIndex;
		builtSerial[kind] = NeuralMatchSerial;

		LTRBStruct bounds = MapClass::Instance.MapCoordBounds;

		if (bounds.Right <= bounds.Left || bounds.Bottom <= bounds.Top)
			bounds = LTRBStruct { 0, 0, 511, 511 };

		map.Left = std::max(0, bounds.Left);
		map.Top = std::max(0, bounds.Top);
		map.Width = std::max(1, (bounds.Right - map.Left) / OpRouteBlockCells + 1);
		map.Height = std::max(1, (bounds.Bottom - map.Top) / OpRouteBlockCells + 1);
		map.Danger.assign(static_cast<size_t>(map.Width * map.Height), 0);
		CollectOpThreats(pHouse, airborne, map.Threats);

		for (auto const& threat : map.Threats)
		{
			const int span = threat.Reach / OpRouteBlockCells + 1;
			const int cx = (threat.Cell.X - map.Left) / OpRouteBlockCells;
			const int cy = (threat.Cell.Y - map.Top) / OpRouteBlockCells;

			for (int by = std::max(0, cy - span); by <= std::min(map.Height - 1, cy + span); ++by)
			{
				for (int bx = std::max(0, cx - span); bx <= std::min(map.Width - 1, cx + span); ++bx)
				{
					const int index = by * map.Width + bx;
					const int distance = static_cast<int>(map.CenterOf(index).DistanceFrom(threat.Cell));

					if (distance < threat.Reach)
						map.Danger[static_cast<size_t>(index)] += (threat.Reach - distance) * threat.Weight;
				}
			}
		}

		return map;
	}

	// The threat nearest to a cell among those whose reach covers it, else the nearest of all (the
	// loss log); nullptr with none.
	const OpThreat* NearestOpThreat(const OpDangerMap& map, const CellStruct& cell, double& distance)
	{
		const OpThreat* pNearest = nullptr;
		bool covers = false;
		distance = 0.0;

		for (auto const& threat : map.Threats)
		{
			const double d = threat.Cell.DistanceFrom(cell);
			const bool inReach = d < threat.Reach;

			if (!pNearest || (inReach && !covers) || (inReach == covers && d < distance))
			{
				pNearest = &threat;
				covers = inReach;
				distance = d;
			}
		}

		return pNearest;
	}

	// Walks the straight line between two blocks of the map, block by block (the first one left out):
	// visit(previous, block, stepX, stepY) for each step; false when visit says so.
	template <typename Visit>
	bool WalkOpLine(const OpDangerMap& map, int from, int to, Visit&& visit)
	{
		int x = from % map.Width;
		int y = from / map.Width;
		const int tx = to % map.Width;
		const int ty = to / map.Width;
		const int dx = std::abs(tx - x);
		const int dy = std::abs(ty - y);
		const int sx = x < tx ? 1 : -1;
		const int sy = y < ty ? 1 : -1;
		int error = dx - dy;

		while (x != tx || y != ty)
		{
			const int previous = y * map.Width + x;
			const int doubled = 2 * error;
			int stepX = 0;
			int stepY = 0;

			if (doubled > -dy)
			{
				error -= dy;
				x += sx;
				stepX = sx;
			}

			if (doubled < dx)
			{
				error += dx;
				y += sy;
				stepY = sy;
			}

			if (!visit(previous, y * map.Width + x, stepX, stepY))
				return false;
		}

		return true;
	}

	// The cost (and danger) of going straight from one cell to another over the danger map: a step
	// across a block costs 10 (14 diagonally) times OpRouteBaseCost plus the block's danger.
	int OpLineCost(const OpDangerMap& map, const CellStruct& from, const CellStruct& to, int& danger)
	{
		int cost = 0;
		danger = 0;

		WalkOpLine(map, map.IndexOf(from), map.IndexOf(to), [&](int, int block, int stepX, int stepY)
			{
				cost += (stepX && stepY ? 14 : 10) * (OpRouteBaseCost + map.Danger[static_cast<size_t>(block)]);
				danger += map.Danger[static_cast<size_t>(block)];

				return true;
			});

		return cost;
	}

	// The cost of a route still ahead: from the cell through the waypoints to the landing, straight
	// between them.
	int OpRouteCost(const OpDangerMap& map, const CellStruct& from, const std::vector<CellStruct>& waypoints, const CellStruct& landing)
	{
		int cost = 0;
		int danger = 0;
		CellStruct at = from;

		for (auto const& waypoint : waypoints)
		{
			cost += OpLineCost(map, at, waypoint, danger);
			at = waypoint;
		}

		return cost + OpLineCost(map, at, landing, danger);
	}

	// The way a transport of an operation takes to one of its landings (PlanOpRoute): the landing, the
	// waypoints before it, the danger of the blocks it crosses, the cost of the way, that cost with the
	// landing's own share (its danger, the walk it adds), and that total for every landing asked about
	// (INT_MAX: out of reach).
	struct OpRoutePlan
	{
		CellStruct Landing { CellStruct::Empty };
		std::vector<CellStruct> Waypoints;
		int Danger { 0 };
		int Cost { 0 };
		int Total { 0 };
		std::vector<int> Totals;
	};

	// The safest way for a transport of an operation from where it stands to one of the landings (user:
	// a new route of safe waypoints so that more of them survive, keeping away from the defenses and
	// from the enemy units wherever possible): the cheapest over the danger map of its kind, each
	// landing adding its own danger (that of the ground too for the passengers of a flying transport)
	// and extraWalk cells of walk for the members. A ground transport keeps to the cells of its own
	// movement zone, a flying one goes anywhere. A carryall and a burrowing transport go straight: the
	// engine's carryall sets its vehicle down at the end of every move (AircraftClass::
	// Do_MISSION_MOVE_Carryall), and a burrower crosses under the ground. False when no landing can be
	// reached. pFrom: where the way starts when not where the transport stands (the vehicle a carryall
	// is to lift).
	bool PlanOpRoute(HouseClass* pHouse, FootClass* pTransport, OpCarrier carrier, const std::vector<CellStruct>& landings,
		const std::vector<int>& extraWalk, OpRoutePlan& plan, const CellStruct* pFrom = nullptr)
	{
		plan = OpRoutePlan {};

		if (landings.empty() || landings.size() != extraWalk.size())
			return false;

		auto const pType = pTransport->GetTechnoType();
		const bool airborne = carrier != OpCarrier::Surface;
		auto const pUnitType = abstract_cast<UnitTypeClass*>(pType);
		const bool underground = pUnitType && IsUndergroundTransportType(pUnitType);
		auto const& map = GetOpDangerMap(pHouse, airborne);
		auto const pGroundMap = airborne ? &GetOpDangerMap(pHouse, false) : nullptr;
		const CellStruct from = pFrom ? *pFrom : pTransport->GetMapCoords();
		const int walkCell = 10 * OpRouteBaseCost / OpRouteBlockCells;
		const int landingFactor = airborne ? OpLandingFlyerDangerFactor : OpLandingDangerFactor;
		plan.Totals.assign(landings.size(), INT_MAX);

		auto const landingShare = [&](size_t i)
		{
			int share = 10 * map.Danger[static_cast<size_t>(map.IndexOf(landings[i]))] * landingFactor + std::max(0, extraWalk[i]) * walkCell;

			if (pGroundMap)
				share += 10 * pGroundMap->Danger[static_cast<size_t>(pGroundMap->IndexOf(landings[i]))] * OpLandingDangerFactor;

			return share;
		};

		auto const take = [&](size_t i, int cost, int danger)
		{
			const int total = cost + landingShare(i);
			plan.Totals[i] = total;

			if (plan.Landing == CellStruct::Empty || total < plan.Total)
			{
				plan.Landing = landings[i];
				plan.Cost = cost;
				plan.Danger = danger;
				plan.Total = total;
			}
		};

		if (carrier == OpCarrier::Carryall || underground)
		{
			for (size_t i = 0; i < landings.size(); ++i)
			{
				// Under the ground a burrower meets no danger on its way.
				int danger = 0;
				const int cost = underground ? static_cast<int>(from.DistanceFrom(landings[i])) * walkCell : OpLineCost(map, from, landings[i], danger);
				take(i, cost, underground ? 0 : danger);
			}

			return true;
		}

		const MovementZone movementZone = pType->MovementZone;
		const int zone = airborne ? -1 : MapClass::Instance.GetMovementZoneType(from, movementZone, pTransport->OnBridge);
		const size_t blocks = static_cast<size_t>(map.Width * map.Height);
		constexpr int Last = OpRouteBlockCells - 1;

		// Which cells of a block the transport can be on (bit 3 * row + column), worked out when first
		// asked for (-1: not yet).
		std::vector<int> cellsOf(blocks, -1);

		auto const cellOf = [&map](int block, int column, int row) -> CellStruct
		{
			return { static_cast<short>(map.Left + (block % map.Width) * OpRouteBlockCells + column),
				static_cast<short>(map.Top + (block / map.Width) * OpRouteBlockCells + row) };
		};

		auto const bit = [](int column, int row) { return 1 << (row * OpRouteBlockCells + column); };

		auto const maskOf = [&](int block) -> int
		{
			int& mask = cellsOf[static_cast<size_t>(block)];

			if (mask >= 0)
				return mask;

			mask = 0;

			for (int row = 0; row < OpRouteBlockCells; ++row)
			{
				for (int column = 0; column < OpRouteBlockCells; ++column)
				{
					const CellStruct c = cellOf(block, column, row);

					if (MapClass::Instance.CoordinatesLegal(c) && (airborne || MapClass::Instance.GetMovementZoneType(c, movementZone, false) == zone))
						mask |= bit(column, row);
				}
			}

			return mask;
		};

		// Two neighbouring blocks join when a usable cell of one touches a usable cell of the other.
		auto const joined = [&](int a, int b, int stepX, int stepY) -> bool
		{
			const int maskA = maskOf(a);
			const int maskB = maskOf(b);

			if (!maskA || !maskB)
				return false;

			if (stepX && stepY)
			{
				const int column = stepX > 0 ? Last : 0;
				const int row = stepY > 0 ? Last : 0;

				return (maskA & bit(column, row)) && (maskB & bit(Last - column, Last - row));
			}

			for (int k = 0; k < OpRouteBlockCells; ++k)
			{
				for (int j = std::max(0, k - 1); j <= std::min(Last, k + 1); ++j)
				{
					const bool touches = stepX
						? (maskA & bit(stepX > 0 ? Last : 0, k)) && (maskB & bit(stepX > 0 ? 0 : Last, j))
						: (maskA & bit(k, stepY > 0 ? Last : 0)) && (maskB & bit(j, stepY > 0 ? 0 : Last));

					if (touches)
						return true;
				}
			}

			return false;
		};

		// Dijkstra over the blocks from the transport's own, until every landing's block is settled.
		std::vector<int> cost(blocks, INT_MAX);
		std::vector<int> came(blocks, -1);
		std::vector<uint8_t> goal(blocks, 0);
		int goals = 0;

		for (auto const& landing : landings)
		{
			auto& isGoal = goal[static_cast<size_t>(map.IndexOf(landing))];

			if (!isGoal)
			{
				isGoal = 1;
				goals++;
			}
		}

		using OpenBlock = std::pair<int, int>;
		std::vector<OpenBlock> open;
		auto const later = [](const OpenBlock& a, const OpenBlock& b) { return a > b; };
		const int start = map.IndexOf(from);
		cost[static_cast<size_t>(start)] = 0;
		open.push_back({ 0, start });

		while (!open.empty() && goals > 0)
		{
			std::pop_heap(open.begin(), open.end(), later);
			const OpenBlock top = open.back();
			open.pop_back();
			const int block = top.second;

			if (top.first > cost[static_cast<size_t>(block)])
				continue;

			if (goal[static_cast<size_t>(block)] == 1)
			{
				goal[static_cast<size_t>(block)] = 2;
				goals--;
			}

			const int bx = block % map.Width;
			const int by = block / map.Width;

			for (int stepY = -1; stepY <= 1; ++stepY)
			{
				for (int stepX = -1; stepX <= 1; ++stepX)
				{
					const int nx = bx + stepX;
					const int ny = by + stepY;

					if ((!stepX && !stepY) || nx < 0 || ny < 0 || nx >= map.Width || ny >= map.Height)
						continue;

					const int next = ny * map.Width + nx;

					if (!joined(block, next, stepX, stepY))
						continue;

					const int reached = top.first + (stepX && stepY ? 14 : 10) * (OpRouteBaseCost + map.Danger[static_cast<size_t>(next)]);

					if (reached < cost[static_cast<size_t>(next)])
					{
						cost[static_cast<size_t>(next)] = reached;
						came[static_cast<size_t>(next)] = block;
						open.push_back({ reached, next });
						std::push_heap(open.begin(), open.end(), later);
					}
				}
			}
		}

		for (size_t i = 0; i < landings.size(); ++i)
		{
			const int reached = cost[static_cast<size_t>(map.IndexOf(landings[i]))];

			if (reached != INT_MAX)
				take(i, reached, 0);
		}

		if (plan.Landing == CellStruct::Empty)
			return false;

		// The path back from the landing's block, and the danger it takes up to each of its blocks.
		std::vector<int> path;

		for (int block = map.IndexOf(plan.Landing); block >= 0; block = came[static_cast<size_t>(block)])
			path.push_back(block);

		std::reverse(path.begin(), path.end());
		std::vector<int> dangerTo(path.size(), 0);

		for (size_t i = 1; i < path.size(); ++i)
			dangerTo[i] = dangerTo[i - 1] + map.Danger[static_cast<size_t>(path[i])];

		plan.Danger = dangerTo.back();

		// A cell of a block the transport can be on, the middle one first.
		auto const pointOf = [&](int block) -> CellStruct
		{
			const int mask = maskOf(block);
			constexpr int Middle = OpRouteBlockCells / 2;

			if (mask & bit(Middle, Middle))
				return cellOf(block, Middle, Middle);

			for (int row = 0; row < OpRouteBlockCells; ++row)
			{
				for (int column = 0; column < OpRouteBlockCells; ++column)
				{
					if (mask & bit(column, row))
						return cellOf(block, column, row);
				}
			}

			return map.CenterOf(block);
		};

		// The waypoints: from each one on, the farthest block of the path a straight line reaches over
		// usable blocks at no more danger than the path takes there.
		size_t anchor = 0;

		while (anchor + 1 < path.size())
		{
			size_t next = anchor + 1;

			for (size_t j = std::min(path.size() - 1, anchor + OpRouteSmoothBlocks); j > anchor + 1; --j)
			{
				int lineDanger = 0;
				const bool clear = WalkOpLine(map, path[anchor], path[j], [&](int previous, int block, int stepX, int stepY)
					{
						lineDanger += map.Danger[static_cast<size_t>(block)];

						return joined(previous, block, stepX, stepY);
					});

				if (clear && lineDanger <= dangerTo[j] - dangerTo[anchor])
				{
					next = j;
					break;
				}
			}

			anchor = next;

			if (anchor + 1 < path.size())
			{
				const CellStruct waypoint = pointOf(path[anchor]);

				if (waypoint.DistanceFrom(from) > OpRouteReachCells && static_cast<int>(plan.Waypoints.size()) < OpRouteMaxWaypoints)
					plan.Waypoints.push_back(waypoint);
			}
		}

		return true;
	}

	// The landings a load may sail for: first, the operation's landing, and other cells of the target's
	// land up to OpLandingCandidateCells from it (OpLandingCandidates in all, OpLandingSpacingCells
	// apart, the nearest to the target first), each with the walk it adds to the target against the
	// operation's landing. A ground transport lands by the shore on a cell it can stand on, a flying one
	// on a free cell off the beach.
	void CollectOpLandingCandidates(const NeuralTransportOperation& op, FootClass* pTransport, OpCarrier carrier, const CellStruct& first,
		std::vector<CellStruct>& candidates, std::vector<int>& extraWalk)
	{
		auto const pType = pTransport->GetTechnoType();
		const bool flies = carrier != OpCarrier::Surface;
		const int transportZone = flies ? -1
			: MapClass::Instance.GetMovementZoneType(pTransport->GetMapCoords(), pType->MovementZone, pTransport->OnBridge);
		std::vector<std::pair<double, CellStruct>> found;
		// A back door lands by the building it is for, not anywhere around it.
		const int radius = op.Client == NeuralOpClient::Strike ? BackdoorLandingCandidateCells : OpLandingCandidateCells;

		for (int dy = -radius; dy <= radius; dy += 2)
		{
			for (int dx = -radius; dx <= radius; dx += 2)
			{
				const CellStruct c { static_cast<short>(op.Landing.X + dx), static_cast<short>(op.Landing.Y + dy) };
				auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

				if (!pCell || pCell->LandType == LandType::Water || pCell->GetBuilding()
					|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != op.TargetZone)
				{
					continue;
				}

				if (flies)
				{
					if (pCell->LandType == LandType::Beach || pCell->FirstObject
						|| !pCell->IsClearToMove(SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false))
					{
						continue;
					}
				}
				else
				{
					if (MapClass::Instance.GetMovementZoneType(c, pType->MovementZone, false) != transportZone
						|| !pCell->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, false))
					{
						continue;
					}

					bool shore = false;

					for (int sy = -2; sy <= 2 && !shore; ++sy)
					{
						for (int sx = -2; sx <= 2 && !shore; ++sx)
						{
							auto const pNear = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(c.X + sx), static_cast<short>(c.Y + sy) });
							shore = pNear && pNear->LandType == LandType::Water;
						}
					}

					if (!shore)
						continue;
				}

				found.push_back({ c.DistanceFrom(op.Target), c });
			}
		}

		std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		candidates.assign(1, first);

		if (first != op.Landing)
			candidates.push_back(op.Landing);

		for (auto const& [distance, c] : found)
		{
			if (static_cast<int>(candidates.size()) >= OpLandingCandidates)
				break;

			const bool apart = std::none_of(candidates.begin(), candidates.end(),
				[&c](const CellStruct& kept) { return kept.DistanceFrom(c) < OpLandingSpacingCells; });

			if (apart)
				candidates.push_back(c);
		}

		const double firstWalk = op.Landing.DistanceFrom(op.Target);
		extraWalk.clear();

		for (auto const& c : candidates)
			extraWalk.push_back(std::max(0, static_cast<int>(c.DistanceFrom(op.Target) - firstWalk)));
	}

	// The landing a load sails for (user: the landing chosen together with the route): the
	// operation's landing or another of its candidates (CollectOpLandingCandidates), the one PlanOpRoute
	// finds cheapest for the first transport that sails. danger: that of the route to it (0 when none
	// could be planned).
	CellStruct ChooseOpTripLanding(HouseClass* pHouse, NeuralTransportOperation& op, FootClass* pTransport, OpCarrier carrier, int& danger)
	{
		danger = 0;
		auto const pType = pTransport->GetTechnoType();
		std::vector<CellStruct> candidates;
		std::vector<int> extraWalk;
		CollectOpLandingCandidates(op, pTransport, carrier, op.Landing, candidates, extraWalk);
		const double firstWalk = op.Landing.DistanceFrom(op.Target);
		OpRoutePlan plan;

		if (!PlanOpRoute(pHouse, pTransport, carrier, candidates, extraWalk, plan))
			return op.Landing;

		danger = plan.Danger;

		if (plan.Landing != op.Landing)
		{
			op.LandingMoves++;
			Debug::Log("[NeuralAI] House %d transport op %d: the load of %s %u lands at (%d,%d) instead of (%d,%d), cost %d against %s, "
				"%d cells more walk, %d candidates, frame %d.\n", pHouse->ArrayIndex, op.Id, pType->ID, pTransport->UniqueID, plan.Landing.X,
				plan.Landing.Y, op.Landing.X, op.Landing.Y, plan.Total, plan.Totals.front() == INT_MAX ? "no way"
					: std::to_string(plan.Totals.front()).c_str(), std::max(0, static_cast<int>(plan.Landing.DistanceFrom(op.Target) - firstWalk)),
				static_cast<int>(candidates.size()), Unsorted::CurrentFrame);
		}

		return plan.Landing;
	}

	// The danger of the straight way a carryall flies with its vehicle, from the vehicle to the
	// cheapest of the operation's landing candidates (0 when there is none).
	int OpCarryallWayDanger(HouseClass* pHouse, const NeuralTransportOperation& op, FootClass* pCarryall, const CellStruct& vehicleCell)
	{
		std::vector<CellStruct> candidates;
		std::vector<int> extraWalk;
		CollectOpLandingCandidates(op, pCarryall, OpCarrier::Carryall, op.Landing, candidates, extraWalk);
		OpRoutePlan plan;

		return PlanOpRoute(pHouse, pCarryall, OpCarrier::Carryall, candidates, extraWalk, plan, &vehicleCell) ? plan.Danger : 0;
	}

	// The danger at a landing for a load set down there: on the ground, and for a flying transport, which
	// comes down there, in the air too.
	int OpLandingDanger(HouseClass* pHouse, const CellStruct& landing, OpCarrier carrier)
	{
		auto const& ground = GetOpDangerMap(pHouse, false);
		int danger = ground.Danger[static_cast<size_t>(ground.IndexOf(landing))];

		if (carrier != OpCarrier::Surface)
		{
			auto const& air = GetOpDangerMap(pHouse, true);
			danger += air.Danger[static_cast<size_t>(air.IndexOf(landing))];
		}

		return danger;
	}

	// The berth of a transport of an operation (OpBerthRingCells): the nearest cell around the staging
	// cell, OpBerthSpacingCells or more from the berths of the operation's other transports, that it
	// can stand on by the land of the members (a flyer: a free cell of that land off the beach). wait:
	// where its passengers wait for it, OpBerthWaitCells farther out from the staging cell on their land
	// (the berth itself when that is no such land). Empty when there is none.
	CellStruct PickOpBerth(const NeuralTransportOperation& op, FootClass* pTransport, OpCarrier carrier, CellStruct& wait)
	{
		auto const pType = pTransport->GetTechnoType();
		const bool flies = carrier != OpCarrier::Surface;
		const int transportZone = flies ? -1
			: MapClass::Instance.GetMovementZoneType(pTransport->GetMapCoords(), pType->MovementZone, pTransport->OnBridge);
		std::vector<CellStruct> taken;
		wait = CellStruct::Empty;

		for (auto const& other : op.Transports)
		{
			if (other.UniqueID != pTransport->UniqueID && other.Berth != CellStruct::Empty)
				taken.push_back(other.Berth);
		}

		auto const originLand = [&op](const CellStruct& c)
		{
			auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

			return pCell && pCell->LandType != LandType::Water && !pCell->GetBuilding()
				&& MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) == op.OriginZone;
		};

		for (int ring = 0; ring <= OpBerthRingCells; ++ring)
		{
			for (int dy = -ring; dy <= ring; ++dy)
			{
				for (int dx = -ring; dx <= ring; ++dx)
				{
					if (std::max(std::abs(dx), std::abs(dy)) != ring)
						continue;

					const CellStruct c { static_cast<short>(op.Staging.X + dx), static_cast<short>(op.Staging.Y + dy) };
					auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

					if (!pCell || pCell->GetBuilding()
						|| std::any_of(taken.begin(), taken.end(), [&c](const CellStruct& at) { return at.DistanceFrom(c) < OpBerthSpacingCells; }))
					{
						continue;
					}

					if (flies)
					{
						if (!originLand(c) || pCell->LandType == LandType::Beach
							|| !pCell->IsClearToMove(SpeedType::Track, false, false, -1, MovementZone::Normal, -1, false))
						{
							continue;
						}
					}
					else
					{
						if (MapClass::Instance.GetMovementZoneType(c, pType->MovementZone, false) != transportZone
							|| !pCell->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, false))
						{
							continue;
						}

						bool byLand = false;

						for (int sy = -1; sy <= 1 && !byLand; ++sy)
						{
							for (int sx = -1; sx <= 1 && !byLand; ++sx)
								byLand = originLand(CellStruct { static_cast<short>(c.X + sx), static_cast<short>(c.Y + sy) });
						}

						if (!byLand)
							continue;
					}

					const double length = ring > 0 ? std::sqrt(static_cast<double>(dx * dx + dy * dy)) : 1.0;
					const double outX = ring > 0 ? dx / length : 1.0;
					const double outY = ring > 0 ? dy / length : 0.0;
					const CellStruct out { static_cast<short>(c.X + std::lround(outX * OpBerthWaitCells)),
						static_cast<short>(c.Y + std::lround(outY * OpBerthWaitCells)) };
					wait = originLand(out) ? out : c;

					return c;
				}
			}
		}

		return CellStruct::Empty;
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

	// The key of a crossing in CrossingBlockedUntil: the client and the two lands.
	long long CrossingBlockKey(NeuralOpClient client, int originZone, int targetZone)
	{
		return (static_cast<long long>(client) << 42) | (static_cast<long long>(originZone & 0x1FFFFF) << 21)
			| static_cast<long long>(targetZone & 0x1FFFFF);
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

		// A crossing whose last operation ended with no transport stays shut OpNoTransportRetryFrames.
		if (auto const itBlock = macro.CrossingBlockedUntil.find(CrossingBlockKey(client, footZone, targetZone));
			itBlock != macro.CrossingBlockedUntil.end())
		{
			if (now < itBlock->second && itBlock->second - now <= OpNoTransportRetryFrames)
				return false;

			macro.CrossingBlockedUntil.erase(itBlock);
		}

		auto const pLanding = FindSafeIslandLandingCell(pHouse, ground, targetZone, false);
		const CellStruct footCoords = pFoot->GetMapCoords();

		// A transport that comes now: one the house could build only after more prerequisites left the
		// engineers waiting OpNoTransportFrames for a crossing nothing it had could make.
		if (!pLanding)
			return false;

		if (!CanHouseFerryTypeAcrossWater(pHouse, pType, &footCoords, &pLanding->MapCoords, true))
		{
			// The production builds what the transport that would make it lacks.
			NoteCrossingNeed(pHouse, footCoords, pLanding->MapCoords, std::max(1, static_cast<int>(pType->Size)));
			return false;
		}

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

	// A back door (UpdateBackdoor) asks to take a unit to a landing of its choice by the building target
	// on the far side of an enemy base, on the unit's own ground or another: an operation of the Strike
	// client to that landing (a new one, or the one going there). False when the unit cannot be taken.
	bool RequestStrikeTransport(HouseClass* pHouse, FootClass* pFoot, const CellStruct& landing, const CellStruct& target)
	{
		if (!TransportOpsEnabled || !pHouse || !pFoot || pFoot->InLimbo || !pFoot->IsAlive || landing == CellStruct::Empty
			|| target == CellStruct::Empty)
		{
			return false;
		}

		auto& macro = GetMacroState(pHouse);
		const DWORD uid = pFoot->UniqueID;
		auto const pType = pFoot->GetTechnoType();

		if (macro.TransportOpOf.contains(uid) || !pType || CrossesWaterAlone(pType) || GetOpCarrier(pFoot) != OpCarrier::None
			|| pFoot->Transporter || IsPlanMember(pHouse, uid))
		{
			return false;
		}

		const int footZone = OpFootGroundZone(pFoot);
		const int landingZone = MapClass::Instance.GetMovementZoneType(landing, MovementZone::Normal, false);

		if (!IsGroundZone(footZone) || !IsGroundZone(landingZone))
			return false;

		const int now = Unsorted::CurrentFrame;
		NeuralTransportOperation* pOp = nullptr;

		for (auto& op : macro.TransportOps)
		{
			if (!op.Over && op.Client == NeuralOpClient::Strike && op.OriginZone == footZone && op.Landing == landing)
			{
				pOp = &op;
				break;
			}
		}

		if (!pOp)
		{
			auto& op = macro.TransportOps.emplace_back();
			op.Id = macro.NextTransportOpId++;
			op.Client = NeuralOpClient::Strike;
			op.OriginZone = footZone;
			op.TargetZone = landingZone;
			op.Target = target;
			op.Landing = landing;
			op.CreatedFrame = now;
			op.LastTransportFrame = now;
			pOp = &op;

			Debug::Log("[NeuralAI] House %d transport op %d opened for a strike: %s %u at (%d,%d) zone %d -> landing (%d,%d) zone %d by "
				"(%d,%d), frame %d.\n", pHouse->ArrayIndex, op.Id, pType->ID, uid, pFoot->GetMapCoords().X, pFoot->GetMapCoords().Y, footZone,
				landing.X, landing.Y, landingZone, target.X, target.Y, now);
		}
		else
		{
			pOp->Joined++;
		}

		NeuralOpMember member {};
		member.UniqueID = uid;
		member.Size = std::max(1, static_cast<int>(pType->Size));
		member.Target = target;
		pOp->Members.push_back(member);
		macro.TransportOpOf[uid] = pOp->Id;

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

		// A back door's member attacks the building it was carried to; kept as a wave goal, the wave
		// dispatch does not send it elsewhere.
		if (op.Client == NeuralOpClient::Wave || op.Client == NeuralOpClient::Strike)
		{
			if (auto const pCell = MapClass::Instance.TryGetCellAt(member.Target))
			{
				OrderFootToAttackMove(pFoot, pCell);
				GetMacroState(pHouse).WaveGoals[member.UniqueID] = { member.Target, op.TargetZone, Unsorted::CurrentFrame };
				return;
			}
		}

		pFoot->SetDestination(nullptr, true);

		// Engineer claims (EngineerClaim*): an engineer set down with its claimed building no longer to
		// take walks EngineerLandingStepCells inland, towards its old goal, off the landing (user: they
		// stood at the landing with nothing to do); its client gives it its next capture from there.
		if (op.Client == NeuralOpClient::Economy && pFoot->WhatAmI() == AbstractType::Infantry
			&& EngineerClaimWanted(pHouse, member.UniqueID) == 0)
		{
			auto& macro = GetMacroState(pHouse);
			macro.EngineerCaptureTargets.erase(member.UniqueID);
			const CellStruct at = pFoot->GetMapCoords();
			const int dx = member.Target.X - at.X;
			const int dy = member.Target.Y - at.Y;
			const int steps = std::max(std::abs(dx), std::abs(dy));

			if (member.Target != CellStruct::Empty && steps >= 2)
			{
				const int step = std::min(steps, EngineerLandingStepCells);
				const CellStruct to { static_cast<short>(at.X + dx * step / steps), static_cast<short>(at.Y + dy * step / steps) };
				auto const pType = pFoot->GetTechnoType();
				auto const pCell = MapClass::Instance.CoordinatesLegal(to) ? MapClass::Instance.TryGetCellAt(to) : nullptr;

				if (pCell && pType && !pCell->GetBuilding()
					&& MapClass::Instance.GetMovementZoneType(to, pType->MovementZone, false)
						== MapClass::Instance.GetMovementZoneType(at, pType->MovementZone, pFoot->OnBridge)
					&& IssueMoveOrder(pFoot, pCell, 1.5))
				{
					return;
				}
			}
		}

		pFoot->QueueMission(Mission::Guard, false);
	}

	// A cell where a member waiting for its seat holds the way of everything else that crosses there: a
	// bridge or tube cell, a passage mouth or a narrow passage (OpHoldMoveRing).
	bool IsOpWaitInTheWay(const CellStruct& cell)
	{
		auto const pCell = MapClass::Instance.CoordinatesLegal(cell) ? MapClass::Instance.TryGetCellAt(cell) : nullptr;

		return !pCell || IsBridgeOrTubeCell(pCell) || IsPassageMouthCell(cell) || IsNarrowPassage(cell);
	}

	// The wait cell used instead of `hold` when that one is in the way: the nearest cell to it within
	// OpHoldMoveRing on the land the members leave from, ground with no building, out of the way and
	// between bandMin and bandMax cells from the staging cell; kept for the operation (HoldMoves).
	// Empty when there is none.
	CellStruct FindOpHoldOutOfTheWay(NeuralTransportOperation& op, const CellStruct& hold, double bandMin, double bandMax, int houseIndex)
	{
		for (auto const& [from, to] : op.HoldMoves)
		{
			if (from == hold)
				return to;
		}

		CellStruct found = CellStruct::Empty;

		for (int r = 1; r <= OpHoldMoveRing && found == CellStruct::Empty; ++r)
		{
			double bestDistance = 1.0e9;

			for (int dy = -r; dy <= r; ++dy)
			{
				for (int dx = -r; dx <= r; ++dx)
				{
					if (std::abs(dx) != r && std::abs(dy) != r)
						continue;

					const CellStruct c { static_cast<short>(hold.X + dx), static_cast<short>(hold.Y + dy) };
					auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

					if (!pCell || pCell->LandType == LandType::Water || pCell->GetBuilding()
						|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != op.OriginZone)
					{
						continue;
					}

					const double fromStaging = c.DistanceFrom(op.Staging);
					const double toHold = c.DistanceFrom(hold);

					if (fromStaging < bandMin || fromStaging > bandMax || toHold >= bestDistance || IsOpWaitInTheWay(c))
						continue;

					bestDistance = toHold;
					found = c;
				}
			}
		}

		if (op.HoldMoves.size() >= OpHoldMovesMost)
			op.HoldMoves.clear();

		op.HoldMoves.push_back({ hold, found });
		CountDecision(NeuralDecision::OpHoldMoved, houseIndex);
		// TEMP diagnostic: each wait cell moved out of the way and where to.
		Debug::Log("[NeuralAI] House %d transport op %d: wait cell (%d,%d) is in the way (bridge %d, mouth %d, narrow %d), moved to (%d,%d), "
			"staging (%d,%d), frame %d.\n", houseIndex, op.Id, hold.X, hold.Y,
			IsBridgeOrTubeCell(MapClass::Instance.TryGetCellAt(hold)) ? 1 : 0, IsPassageMouthCell(hold) ? 1 : 0, IsNarrowPassage(hold) ? 1 : 0,
			found.X, found.Y, op.Staging.X, op.Staging.Y, Unsorted::CurrentFrame);

		return found;
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

		const bool shuttle = op.Client == NeuralOpClient::Shuttle;

		// Ends the operation: the members still waiting go back to their client (after a failure they
		// may not ask again for OpRetryFrames), the transports are let go.
		auto const endOp = [&](const char* reason, bool failed)
		{
			if (shuttle)
			{
				macro.ShuttleCreditsTotal += op.ShuttleCredits;
				macro.ShuttleTransportsLost += op.TransportsLost;

				if (failed || op.Lost > 0 || op.TransportsLost >= ShuttleMaxLostTransports)
					macro.ShuttleRetryUntil = now + ShuttleRetryFrames;

				Debug::Log("[NeuralAI] House %d shuttle op %d ends: %s after %d frames; %d rounds home with %d credits of ore, %d transports "
					"lost (house totals %d credits, %d transports lost), frame %d.\n", houseIndex, op.Id, reason, now - op.CreatedFrame, op.Rounds,
					op.ShuttleCredits, op.TransportsLost, macro.ShuttleCreditsTotal, macro.ShuttleTransportsLost, now);
			}

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

				// A shuttle's harvester goes on with its work: it mines, or unloads its load.
				pFoot->QueueMission(shuttle ? Mission::Harvest : Mission::Guard, false);

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
				"(%d still aboard), %d joined, %d trips (%d failed), %d transports taken (%d lost), %d interferences, %d route changes of %d "
				"replans, %d loads landed elsewhere, %d loads held for danger, %d landings changed under way, frame %d.\n", houseIndex, op.Id,
				OpClientName(op.Client), reason, now - op.CreatedFrame, op.Landed, op.Lost, op.Released, static_cast<int>(op.Members.size()) - aboard,
				aboard, op.Joined, op.Trips, op.FailedTrips, op.TransportsTaken, op.TransportsLost, op.Interferences, op.RouteChanges, op.Replans,
				op.LandingMoves, op.DangerHolds, op.LandingSwitches, now);

			op.Members.clear();
			op.Transports.clear();
			op.Over = true;
			op.EndReason = reason;
		};

		// Ends an operation that got no transport: its back door, or its crossing (client and the two
		// lands), opens again only OpNoTransportRetryFrames later.
		auto const endWithNoTransport = [&](const char* reason)
		{
			if (op.Client == NeuralOpClient::Strike)
				macro.BackdoorRetryUntil = now + OpNoTransportRetryFrames;
			else if (!shuttle)
				macro.CrossingBlockedUntil[CrossingBlockKey(op.Client, op.OriginZone, op.TargetZone)] = now + OpNoTransportRetryFrames;

			endOp(reason, true);
		};

		// An order the operation did not give, found on one of its units: counted, logged at most once
		// per OpInterferenceLogFrames (who interferes is to be found and fixed), and overridden by the
		// operation's own order below (user: last word and log). pOwnObject: an object the operation
		// sent it to (the vehicle a carryall lifts).
		auto const checkInterference = [&](FootClass* pFoot, const CellStruct& orderCell, bool transport, AbstractClass* pOwnObject)
		{
			const Mission mission = pFoot->CurrentMission;
			auto const pGoing = abstract_cast<CellClass*>(pFoot->Destination);
			const char* what = nullptr;

			if (pFoot->MegaMission == Mission::AttackMove)
				what = "attack-move";
			else if (mission == Mission::Hunt || mission == Mission::Patrol || mission == Mission::Sabotage
				|| mission == Mission::Harvest || mission == Mission::Retreat || (mission == Mission::Enter && transport)
				|| mission == Mission::Capture)
			{
				what = "mission";
			}
			else if (pFoot->Destination && !pGoing && pFoot->Destination != pOwnObject)
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
		// A loss is logged with where and how it happened (the routes are measured by it), and the next
		// routes keep away from where it went down, the next landings too when it went down crossing.
		std::erase_if(op.Transports, [&](const NeuralOpTransport& transport)
			{
				if (find(transport.UniqueID))
					return false;

				macro.TransportOpOf.erase(transport.UniqueID);
				op.TransportsLost++;

				if (transport.LastCell != CellStruct::Empty)
				{
					int aboard = 0;

					for (auto const& member : op.Members)
					{
						if (member.Transport == transport.UniqueID && member.Aboard)
							aboard++;
					}

					const bool airborne = OpTypeFlies(transport.Type);
					double threatCells = 0.0;
					auto const pThreat = NearestOpThreat(GetOpDangerMap(pHouse, airborne), transport.LastCell, threatCells);
					const CellStruct goal = transport.Leg == NeuralOpLeg::Crossing ? transport.Landing : op.Staging;
					const int strength = transport.Type ? std::max(1, transport.Type->Strength) : 1;

					// Taken from us (mind control, a capture) or destroyed: one taken still stands, under another
					// house (a transport counted lost came back later in another operation, match #10484).
					char fate[48] = "destroyed";

					for (auto const pFoot : FootClass::Array)
					{
						if (pFoot && pFoot->UniqueID == transport.UniqueID && pFoot->IsAlive && pFoot->Health > 0 && pFoot->Owner
							&& pFoot->Owner != pHouse)
						{
							std::snprintf(fate, sizeof(fate), "taken by house %d", pFoot->Owner->ArrayIndex);
							break;
						}
					}

					Debug::Log("[NeuralAI] House %d transport op %d: %s %u lost %s at (%d,%d), %d cells from (%d,%d), %d frames into the leg, "
						"%d aboard, health %d%% at the last tick, %d waypoints left (danger %d planned, %d straight), nearest threat %s %s %d cells "
						"off (reach %d), %s, frame %d.\n", houseIndex, op.Id, transport.Type ? transport.Type->ID : "?", transport.UniqueID,
						OpLegName(transport.Leg), transport.LastCell.X, transport.LastCell.Y, static_cast<int>(transport.LastCell.DistanceFrom(goal)),
						goal.X, goal.Y, now - transport.LegFrame, aboard, transport.LastHealth * 100 / strength,
						static_cast<int>(transport.Route.size()), transport.RouteDanger, transport.StraightDanger,
						pThreat ? OpThreatKindName(pThreat->Kind) : "none", (pThreat && pThreat->Type) ? pThreat->Type->ID : "-",
						static_cast<int>(threatCells), pThreat ? pThreat->Reach : 0, fate, now);

					macro.OpLossSpots.push_back({ transport.LastCell, now, airborne });

					if (macro.OpLossSpots.size() > static_cast<size_t>(OpLossSpotsKept))
						macro.OpLossSpots.erase(macro.OpLossSpots.begin());

					if (transport.Leg == NeuralOpLeg::Crossing)
						macro.BloodyBeaches.emplace_back(transport.LastCell, now);
				}

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

		// A shuttle whose field got a refinery of ours is done: its harvesters unload there.
		if (shuttle)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && (pBld->Type->Refinery || pBld->Type->ResourceDestination)
					&& ObjectZone(pBld, MovementZone::Normal) == op.FieldZone)
				{
					endOp("a refinery of ours on the field", false);
					return;
				}
			}
		}

		// Sends a harvester of a shuttle to its work: on the field it mines the nearest ore of that land,
		// at home it unloads at the nearest refinery (a full harvester told to harvest docks first).
		auto const startShuttleWork = [&](NeuralOpMember& member, FootClass* pFoot, NeuralShuttleWork work)
		{
			member.Work = work;
			member.WorkFrame = now;
			member.LoadSeen = pFoot->Tiberium.GetTotalAmount();
			member.LoadFrame = now;
			member.OrderCell = CellStruct::Empty;
			member.Transport = 0;
			op.Landed++;
			wake(pFoot);
			pFoot->SetArchiveTarget(pFoot->GetCell());
			CellClass* pOre = nullptr;

			if (work == NeuralShuttleWork::Harvesting)
			{
				pOre = FindFreeOreFieldCell(pHouse, pFoot->GetMapCoords(), op.FieldZone, 0, true);

				if (!pOre)
					pOre = MapClass::Instance.TryGetCellAt(op.Field);
			}
			else
			{
				const int credits = pFoot->Tiberium.GetTotalValue();
				op.ShuttleCredits += credits;
				op.Rounds++;
				Debug::Log("[NeuralAI] House %d shuttle op %d: %s %u home at (%d,%d) with %d credits of ore (round %d, %d credits in all), frame %d.\n",
					houseIndex, op.Id, pFoot->GetTechnoType()->ID, member.UniqueID, pFoot->GetMapCoords().X, pFoot->GetMapCoords().Y, credits,
					op.Rounds, op.ShuttleCredits, now);
			}

			pFoot->SetDestination(pOre, true);
			pFoot->QueueMission(Mission::Harvest, false);
		};

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

				// Aboard, its progress starts anew once it is set down: a load set down on its own land by a
				// stuck transport had all 8 members let go 15 frames later, on where they stood before they
				// boarded 8400 frames earlier.
				if (auto const itAboard = aboardOf.find(member.UniqueID); itAboard != aboardOf.end())
				{
					member.Aboard = true;
					member.Transport = itAboard->second;
					member.ProgressCell = CellStruct::Empty;
					aboardCount++;
					return false;
				}

				// A harvester of a shuttle at work is left to it until its load is in (on the field) or out
				// (at home); it waits for no ride meanwhile. Done once the loads go the other way, it waits
				// for its ride like any member: full, its load no longer growing or its time up on the field;
				// empty and out of the refinery, or its time up at home.
				if (member.Work != NeuralShuttleWork::None)
				{
					const bool harvesting = member.Work == NeuralShuttleWork::Harvesting;
					const bool docked = !harvesting && pFoot->InLimbo && !pFoot->Transporter;

					if (!docked && (pFoot->InLimbo || OpFootGroundZone(pFoot) != (harvesting ? op.FieldZone : op.HomeZone)))
					{
						Debug::Log("[NeuralAI] House %d shuttle op %d lets %s %u go: %s, frame %d.\n", houseIndex, op.Id,
							pFoot->GetTechnoType()->ID, member.UniqueID, pFoot->InLimbo ? "in limbo" : "off the land of its work", now);
						op.Released++;
						macro.TransportOpOf.erase(member.UniqueID);
						return true;
					}

					const float load = pFoot->Tiberium.GetTotalAmount();

					if (load > member.LoadSeen + 0.01f)
					{
						member.LoadSeen = load;
						member.LoadFrame = now;
					}

					const int workFrames = now - member.WorkFrame;
					bool done = false;

					if (harvesting)
					{
						const float storage = static_cast<float>(std::max(1, pFoot->GetTechnoType()->Storage));
						done = op.Homeward && (load >= storage - 0.5f || now - member.LoadFrame >= ShuttleIdleFrames
							|| workFrames >= ShuttleHarvestCapFrames || workFrames < 0);
					}
					else
					{
						const bool atRefinery = docked || pFoot->CurrentMission == Mission::Enter || pFoot->CurrentMission == Mission::Unload
							|| pFoot->HasAnyLink();
						done = !op.Homeward && !docked && ((load < 0.5f && !atRefinery) || workFrames >= ShuttleUnloadCapFrames || workFrames < 0);

						// Still loaded and doing something else: it is told to unload again.
						if (!done && load >= 0.5f && !atRefinery && pFoot->CurrentMission != Mission::Harvest)
						{
							wake(pFoot);
							pFoot->SetDestination(nullptr, true);
							pFoot->QueueMission(Mission::Harvest, false);
						}
					}

					if (!done)
						return false;

					// It stops its work (its harvest mission is no interference): the operation's orders follow.
					member.Work = NeuralShuttleWork::None;
					member.Target = harvesting ? op.Home : op.Field;
					member.OrderCell = CellStruct::Empty;

					if (!pFoot->InLimbo)
					{
						pFoot->SetDestination(nullptr, true);
						pFoot->ForceMission(Mission::Guard);
					}
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

				// A harvester of a shuttle set down on the field mines it, one set down at home unloads.
				if (shuttle && zone == op.TargetZone)
				{
					startShuttleWork(member, pFoot, op.TargetZone == op.FieldZone ? NeuralShuttleWork::Harvesting : NeuralShuttleWork::Unloading);
					return false;
				}

				// A back door's member stands on that ground from the start: it is delivered once a transport sets
				// it down by the landing, on the far side of the base under attack.
				const bool delivered = op.Client == NeuralOpClient::Strike
					? wasAboard && pFoot->GetMapCoords().DistanceFrom(op.Landing) <= BackdoorDeliverCells
					: zone == op.TargetZone || IsSameGroundForFoot(pFoot, member.Target);

				if (delivered)
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

				// A shuttle's harvester that gets no nearer to the transport waiting for it (shut in, its way
				// blocked) is let go: the transport would wait for it until the operation gave up.
				if (shuttle)
				{
					const CellStruct at = pFoot->GetMapCoords();

					if (op.BoardingSince < 0 || member.ProgressCell == CellStruct::Empty || at.DistanceFrom(member.ProgressCell) >= 2.0)
					{
						member.ProgressCell = at;
						member.ProgressFrame = now;
					}
					else if (now - member.ProgressFrame >= ShuttleStuckFrames || now < member.ProgressFrame)
					{
						Debug::Log("[NeuralAI] House %d shuttle op %d lets %s %u go: no nearer to its ride at (%d,%d) in %d frames, frame %d.\n",
							houseIndex, op.Id, pFoot->GetTechnoType()->ID, member.UniqueID, at.X, at.Y, now - member.ProgressFrame, now);
						macro.ShuttleSkipUntil[member.UniqueID] = now + ShuttleStuckRetryFrames;
						op.Released++;
						macro.TransportOpOf.erase(member.UniqueID);
						wake(pFoot);
						pFoot->SetDestination(nullptr, true);
						pFoot->QueueMission(Mission::Harvest, false);
						return true;
					}
				}
				else
				{
					// Any other member that gets no OpMemberStuckCells nearer to the cell the operation sent it to in
					// OpMemberStuckFrames, while still farther from it than OpMemberStuckFarCells (shut in, its way
					// blocked, the engine's Area Guard walking it elsewhere), is let go and may ask again after
					// OpRetryFrames: the operation would wait for it until it gave up.
					const CellStruct at = pFoot->GetMapCoords();
					const double toOrder = member.OrderCell != CellStruct::Empty ? at.DistanceFrom(member.OrderCell) : 0.0;

					if (toOrder <= OpMemberStuckFarCells || member.ProgressCell == CellStruct::Empty
						|| toOrder + OpMemberStuckCells <= member.ProgressCell.DistanceFrom(member.OrderCell))
					{
						member.ProgressCell = at;
						member.ProgressFrame = now;
					}
					else if (now - member.ProgressFrame >= OpMemberStuckFrames || now < member.ProgressFrame)
					{
						Debug::Log("[NeuralAI] House %d transport op %d lets %s %u go: no nearer to (%d,%d) in %d frames, at (%d,%d) %d cells from it, "
							"frame %d.\n", houseIndex, op.Id, pFoot->GetTechnoType()->ID, member.UniqueID, member.OrderCell.X, member.OrderCell.Y,
							now - member.ProgressFrame, at.X, at.Y, static_cast<int>(toOrder), now);
						macro.TransportOpRetryUntil[member.UniqueID] = now + OpRetryFrames;
						op.Released++;
						macro.TransportOpOf.erase(member.UniqueID);
						wake(pFoot);
						pFoot->SetDestination(nullptr, true);
						pFoot->QueueMission(Mission::Guard, false);
						return true;
					}
				}

				// Engineer claims (EngineerClaim*): an engineer whose claimed building is no longer to take
				// (gone, ours or an ally's, no longer capturable) or that an engineer ahead of it goes for
				// stays home: let go before it boards.
				if (op.Client == NeuralOpClient::Economy && pFoot->WhatAmI() == AbstractType::Infantry
					&& EngineerClaimWanted(pHouse, member.UniqueID) == 0)
				{
					Debug::Log("[NeuralAI] House %d transport op %d lets %s %u go: its capture is no longer to make, frame %d.\n", houseIndex,
						op.Id, pFoot->GetTechnoType()->ID, member.UniqueID, now);
					macro.EngineerCaptureTargets.erase(member.UniqueID);
					op.Released++;
					macro.TransportOpOf.erase(member.UniqueID);
					wake(pFoot);
					pFoot->SetDestination(nullptr, true);
					pFoot->QueueMission(Mission::Guard, false);
					return true;
				}

				waitingSize += member.Size;

				// A shuttle rides only on idle transports: its harvesters ask the factories for none.
				if (!shuttle)
					macro.OpWaitingSizeBySize[std::clamp(member.Size, 1, 15)] += member.Size;

				return false;
			});

		if (op.Members.empty())
		{
			endOp(op.Landed > 0 ? "all delivered" : "nobody left", op.Landed == 0);
			return;
		}

		// A shuttle turns about once every harvester is at work on the other side: the loads go the
		// other way, from a staging cell next to the harvesters (FindBoardingCell: out of the reach of
		// the known enemy defenses), where its transports wait for them like drivers. Before the loads
		// go out again the shuttle checks it is still wanted: the home ore still short, ore left on the
		// field and no losses past ShuttleMaxLostTransports.
		if (shuttle)
		{
			const NeuralShuttleWork turnOn = op.Homeward ? NeuralShuttleWork::Unloading : NeuralShuttleWork::Harvesting;
			const bool turnAbout = std::all_of(op.Members.begin(), op.Members.end(),
				[turnOn](const NeuralOpMember& member) { return member.Work == turnOn; });

			if (turnAbout)
			{
				if (op.Homeward)
				{
					const char* reason = nullptr;
					CellClass* const pOre = FindFreeOreFieldCell(pHouse, op.Field, op.FieldZone, 0, true);

					// The island's MCV (or an engineer) waiting for a ride has the transports first.
					const bool economyWaits = std::any_of(macro.TransportOps.begin(), macro.TransportOps.end(),
						[](const NeuralTransportOperation& other)
						{
							return !other.Over && other.Client == NeuralOpClient::Economy && other.Transports.empty()
								&& std::any_of(other.Members.begin(), other.Members.end(), [](const NeuralOpMember& member) { return !member.Aboard; });
						});

					if (op.TransportsLost >= ShuttleMaxLostTransports)
						reason = "too many transports lost";
					else if (economyWaits)
						reason = "an economy operation waits for a transport";
					else if (macro.IslandHomeOre >= 2 * ShuttleHomeOre)
						reason = "ore at home again";
					else if (!pOre)
						reason = "no free ore left on the field's land";

					if (reason)
					{
						endOp(reason, false);
						return;
					}

					op.Field = pOre->MapCoords;
				}
				else
				{
					bool homeRefinery = false;

					for (auto const pBld : pHouse->Buildings)
					{
						if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && (pBld->Type->Refinery || pBld->Type->ResourceDestination)
							&& ObjectZone(pBld, MovementZone::Normal) == op.HomeZone)
						{
							homeRefinery = true;
							break;
						}
					}

					if (!homeRefinery)
					{
						endOp("no refinery of ours at home", false);
						return;
					}
				}

				op.Homeward = !op.Homeward;
				op.PhaseFrame = now;
				std::swap(op.OriginZone, op.TargetZone);
				op.Target = op.Homeward ? op.Home : op.Field;
				op.Landing = CellStruct::Empty;
				op.Staging = CellStruct::Empty;
				op.Gather = CellStruct::Empty;
				op.TripLanding = CellStruct::Empty;
				op.TripLandingFrame = -1;
				op.BoardingSince = -1;
				op.DangerWaitSince = -1;
				op.DangerCheckFrame = -1;

				for (auto& member : op.Members)
				{
					member.Target = op.Target;
					member.Transport = 0;
				}

				for (auto& transport : op.Transports)
				{
					transport.Leg = NeuralOpLeg::ToStaging;
					transport.LegFrame = now;
					transport.ParkCell = CellStruct::Empty;
					transport.ParkFrame = -1;
					transport.Tries = 0;
					transport.Route.clear();
					transport.RouteFrame = -1;
					transport.Berth = CellStruct::Empty;
					transport.BerthWait = CellStruct::Empty;
				}

				Debug::Log("[NeuralAI] House %d shuttle op %d turns about: %d harvesters %s, the loads go from zone %d to (%d,%d) zone %d, "
					"%d transports wait for them, frame %d.\n", houseIndex, op.Id, static_cast<int>(op.Members.size()),
					op.Homeward ? "mine the field" : "unload at home", op.OriginZone, op.Target.X, op.Target.Y, op.TargetZone,
					static_cast<int>(op.Transports.size()), now);
			}
		}

		// The cells of the members waiting on the map (flying transports come down clear of them).
		std::vector<CellStruct> memberCells;

		for (auto const& member : op.Members)
		{
			auto const pFoot = find(member.UniqueID);

			if (!member.Aboard && !pFoot->InLimbo)
				memberCells.push_back(pFoot->GetMapCoords());
		}

		// A shuttle's harvesters at work on the side the loads leave from: they sail together once done
		// (those at work on the other side wait there for the loads to turn about).
		const NeuralShuttleWork originWork = op.Homeward ? NeuralShuttleWork::Harvesting : NeuralShuttleWork::Unloading;
		const bool shuttleAtWork = shuttle && std::any_of(op.Members.begin(), op.Members.end(),
			[originWork](const NeuralOpMember& member) { return member.Work == originWork; });

		// A shuttle's loads bound for the field land by its ore.
		if (op.Landing == CellStruct::Empty)
		{
			auto const pLanding = FindSafeIslandLandingCell(pHouse, op.Target, op.TargetZone, shuttle && !op.Homeward);
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

			CellClass* pStaging = FindBoardingCell(pHouse, centre, op.OriginZone, op.Landing, nullptr);

			if (!pStaging)
			{
				pStaging = FindSafeIslandLandingCell(pHouse, centre, op.OriginZone, false);

				// A coast no transport of the house sails from to the landing: the members walked up to a
				// hundred cells to it and waited OpNoTransportFrames there. They stay with their client.
				if (pStaging && !CanBoardingCarriersLink(pHouse, pStaging->MapCoords, op.Landing))
				{
					Debug::Log("[NeuralAI] House %d transport op %d: no transport of the house crosses from zone %d near (%d,%d) to the "
						"landing (%d,%d), frame %d.\n", houseIndex, op.Id, op.OriginZone, centre.X, centre.Y, op.Landing.X, op.Landing.Y, now);
					endOp("no crossing", true);
					return;
				}
			}

			op.Staging = pStaging ? pStaging->MapCoords : centre;
			op.Gather = op.Staging;
			op.HoldMoves.clear();
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
					&& MapClass::Instance.GetMovementZoneType(spot, MovementZone::Normal, false) == op.OriginZone
					&& !IsOpWaitInTheWay(spot))
				{
					op.Gather = spot;
					break;
				}
			}

			// The gathering spot is a wait cell too: never on a bridge, a passage mouth or in a narrow passage.
			if (op.Gather == op.Staging && IsOpWaitInTheWay(op.Staging))
			{
				const CellStruct moved = FindOpHoldOutOfTheWay(op, op.Staging, 1.0, OpBerthRingCells, houseIndex);

				if (moved != CellStruct::Empty)
					op.Gather = moved;
			}
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
			if (member.Aboard || member.Work != NeuralShuttleWork::None)
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
		// The danger of the crossing for its kind counts as more distance (user: the kind of transport by
		// the danger: a burrower under a sea of warships, a ship under heavy anti-air).
		if (!unseated.empty() && pStagingCell)
		{
			struct OpCandidate
			{
				FootClass* Foot;
				OpCarrier Carrier;
				double Distance;
				double Score;
			};

			std::vector<OpCandidate> candidates;
			std::array<int, 2> crossingDanger { -1, -1 };

			auto const dangerFor = [&](FootClass* pFoot, OpCarrier carrier) -> int
			{
				auto const pUnitType = abstract_cast<UnitTypeClass*>(pFoot->GetTechnoType());

				if (pUnitType && IsUndergroundTransportType(pUnitType))
					return 0;

				const size_t kind = carrier == OpCarrier::Surface ? 0 : 1;

				if (crossingDanger[kind] < 0)
					OpLineCost(GetOpDangerMap(pHouse, kind == 1), op.Staging, op.Landing, crossingDanger[kind]);

				return crossingDanger[kind];
			};

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

				const double distance = pFoot->GetMapCoords().DistanceFrom(op.Staging);
				candidates.push_back({ pFoot, carrier, distance, distance + static_cast<double>(dangerFor(pFoot, carrier)) / OpKindDangerPerCell });
			};

			for (auto const pUnit : UnitClass::Array)
				consider(pUnit);

			for (auto const pAir : AircraftClass::Array)
				consider(pAir);

			// None of ours makes this crossing: the production builds one that does, or what it lacks (a
			// shuttle rides only on idle transports). When the house has no transport, owned or buildable
			// now, with a seat for a member that comes to the staging cell (CanHouseCarryNow), the operation
			// ends after OpNoReachFrames instead of waiting OpNoTransportFrames for it.
			if (candidates.empty() && op.Transports.empty() && !shuttle)
			{
				int largest = 1;
				bool reachable = false;
				std::vector<const TechnoTypeClass*> asked;

				for (auto const pMember : unseated)
				{
					largest = std::max(largest, pMember->Size);
					auto const pMemberFoot = find(pMember->UniqueID);
					auto const pMemberType = pMemberFoot ? pMemberFoot->GetTechnoType() : nullptr;

					if (reachable || !pMemberType || std::find(asked.begin(), asked.end(), pMemberType) != asked.end())
						continue;

					asked.push_back(pMemberType);
					reachable = CanHouseCarryNow(pHouse, pMemberType, op.Staging, op.Landing);
				}

				NoteCrossingNeed(pHouse, op.Staging, op.Landing, largest);

				// TEMP diagnostic (rule consolidation, 2026-11-19): why each transport of ours is turned down.
				if (IsAITickWindow(OpRefusalLogFrames, 0))
					LogOpTransportRefusals(pHouse, op, static_cast<int>(unseated.size()), largest, reachable);

				if (reachable)
				{
					op.NoReachSince = -1;
				}
				else if (op.NoReachSince < 0 || now < op.NoReachSince)
				{
					op.NoReachSince = now;
				}
				else if (now - op.NoReachSince >= OpNoReachFrames)
				{
					CountDecision(NeuralDecision::OpNoTransportReach, houseIndex);
					endWithNoTransport("no transport can reach");
					return;
				}
			}
			else
			{
				op.NoReachSince = -1;
			}

			std::stable_sort(candidates.begin(), candidates.end(),
				[](const OpCandidate& a, const OpCandidate& b) { return a.Score < b.Score; });

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
			endWithNoTransport("no transport came");
			return;
		}

		// 5. Members waiting on the map: aboard the parked transport they are bound for (TakeAboardNow,
		// once next to it at rest), walking up to a cell next to it, or waiting at the gathering spot.
		for (auto& member : op.Members)
		{
			if (member.Aboard || member.Work != NeuralShuttleWork::None)
				continue;

			auto const pFoot = find(member.UniqueID);
			checkInterference(pFoot, member.OrderCell, false, nullptr);
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

			// Bound for a transport still on its way: it waits by that transport's berth, in the small group
			// of its own load, off the transport's way in.
			if (pRecord && pRecord->BerthWait != CellStruct::Empty && carrier != OpCarrier::Carryall)
			{
				if (pFoot->GetMapCoords().DistanceFrom(pRecord->BerthWait) > OpGatherCells)
					orderTo(pFoot, member.OrderCell, MapClass::Instance.TryGetCellAt(pRecord->BerthWait), OpGatherCells - 1.0);

				continue;
			}

			// Without a seat: it waits out of the berths' way, OpBerthRingCells + OpBerthWaitCells from the
			// staging cell on the side it comes from (each on its own side, not all on one cell), until a
			// transport has a seat for it; with no land there, at the gathering spot. Never on a bridge, a
			// passage mouth or in a narrow passage (IsOpWaitInTheWay): such a wait cell moves to the nearest
			// one out of the way in the band (FindOpHoldOutOfTheWay).
			const CellStruct at = pFoot->GetMapCoords();
			const double fromStaging = at.DistanceFrom(op.Staging);
			constexpr double HoldCells = OpBerthRingCells + OpBerthWaitCells;
			constexpr double HoldBandMin = HoldCells - 2.0;
			constexpr double HoldBandMax = 2.0 * HoldCells;

			// Waiting where it should: it is not stuck short of the cell it was last sent to.
			if (fromStaging >= HoldBandMin && fromStaging <= HoldBandMax && !IsOpWaitInTheWay(at))
			{
				member.ProgressCell = at;
				member.ProgressFrame = now;
				continue;
			}

			const double towardX = fromStaging >= 1.0 ? (at.X - op.Staging.X) / fromStaging : 1.0;
			const double towardY = fromStaging >= 1.0 ? (at.Y - op.Staging.Y) / fromStaging : 0.0;
			const CellStruct hold { static_cast<short>(op.Staging.X + std::lround(towardX * HoldCells)),
				static_cast<short>(op.Staging.Y + std::lround(towardY * HoldCells)) };
			auto pHold = MapClass::Instance.CoordinatesLegal(hold) ? MapClass::Instance.TryGetCellAt(hold) : nullptr;

			if (pHold && pHold->LandType != LandType::Water && !pHold->GetBuilding()
				&& MapClass::Instance.GetMovementZoneType(hold, MovementZone::Normal, false) == op.OriginZone && IsOpWaitInTheWay(hold))
			{
				const CellStruct moved = FindOpHoldOutOfTheWay(op, hold, HoldBandMin, HoldBandMax, houseIndex);
				pHold = moved != CellStruct::Empty ? MapClass::Instance.TryGetCellAt(moved) : nullptr;
			}

			if (pHold && pHold->LandType != LandType::Water && !pHold->GetBuilding()
				&& MapClass::Instance.GetMovementZoneType(pHold->MapCoords, MovementZone::Normal, false) == op.OriginZone)
			{
				orderTo(pFoot, member.OrderCell, pHold, OpGatherCells - 1.0);
			}
			else if (pGatherCell && pFoot->GetMapCoords().DistanceFrom(op.Gather) > OpGatherCells)
			{
				orderTo(pFoot, member.OrderCell, pGatherCell, OpGatherCells - 1.0);
			}
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

			// A transport still parked after the last load sailed starts the wait of the next one (a
			// shuttle's, once its harvesters are done with their work).
			if (op.BoardingSince < 0 && !shuttleAtWork)
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

		// Plans the transport's way to goal (PlanOpRoute). A fresh plan always replaces the route; a plan
		// made again during the trip does so only when it costs OpRouteKeepPercent or less of what is
		// left of the old one over today's danger (a route that swings to and fro reaches nowhere).
		// True when the route changed.
		auto const planRoute = [&](NeuralOpTransport& transport, FootClass* pTransport, OpCarrier carrier, const CellStruct& goal, bool fresh) -> bool
		{
			transport.RouteFrame = now;
			transport.RouteHealth = pTransport->Health;
			const std::vector<CellStruct> goals { goal };
			const std::vector<int> noWalk { 0 };
			OpRoutePlan plan;

			if (!PlanOpRoute(pHouse, pTransport, carrier, goals, noWalk, plan))
			{
				if (fresh)
					transport.Route.clear();

				return false;
			}

			auto const& map = GetOpDangerMap(pHouse, carrier != OpCarrier::Surface);
			const CellStruct from = pTransport->GetMapCoords();

			if (!fresh && static_cast<long long>(plan.Cost) * 100
				> static_cast<long long>(OpRouteCost(map, from, transport.Route, goal)) * OpRouteKeepPercent)
			{
				return false;
			}

			const bool changed = plan.Waypoints != transport.Route;
			transport.Route = plan.Waypoints;
			transport.RouteDanger = plan.Danger;
			OpLineCost(map, from, goal, transport.StraightDanger);

			return changed;
		};

		// Follows the transport's safe route to goal (planRoute), planned again every OpRouteReplanFrames,
		// every OpRouteDangerReplanFrames while it is inside the reach of a threat (user: the threats that
		// move, a warship seen a moment ago turning up on the route), and when it loses OpRouteHitHealth of
		// its strength (OpRouteHitReplanFrames apart at least): its waypoints in turn, then goal itself.
		auto const followRoute = [&](NeuralOpTransport& transport, FootClass* pTransport, OpCarrier carrier, const CellStruct& goal,
			double keepWithin)
		{
			auto const& map = GetOpDangerMap(pHouse, carrier != OpCarrier::Surface);
			const bool inDanger = map.Danger[static_cast<size_t>(map.IndexOf(pTransport->GetMapCoords()))] > 0;
			const int replanFrames = inDanger ? OpRouteDangerReplanFrames : OpRouteReplanFrames;
			const int strength = std::max(1, pTransport->GetTechnoType()->Strength);
			const int sincePlan = now - transport.RouteFrame;
			const bool hit = transport.RouteFrame >= 0 && transport.RouteHealth - pTransport->Health >= static_cast<int>(OpRouteHitHealth * strength);

			if (transport.RouteFrame < 0 || sincePlan < 0 || sincePlan >= replanFrames || (hit && sincePlan >= OpRouteHitReplanFrames))
			{
				op.Replans++;

				if (planRoute(transport, pTransport, carrier, goal, transport.RouteFrame < 0))
				{
					op.RouteChanges++;

					if (hit)
					{
						Debug::Log("[NeuralAI] House %d transport op %d: %s %u hit at (%d,%d) %s, health %d%%, takes another route of %d waypoints "
							"(danger %d against %d straight), frame %d.\n", houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID,
							pTransport->GetMapCoords().X, pTransport->GetMapCoords().Y, OpLegName(transport.Leg), pTransport->Health * 100 / strength,
							static_cast<int>(transport.Route.size()), transport.RouteDanger, transport.StraightDanger, now);
					}
				}
			}

			while (!transport.Route.empty() && pTransport->GetMapCoords().DistanceFrom(transport.Route.front()) <= OpRouteReachCells)
				transport.Route.erase(transport.Route.begin());

			auto const pGoal = MapClass::Instance.TryGetCellAt(transport.Route.empty() ? goal : transport.Route.front());
			orderTo(pTransport, transport.OrderCell, pGoal, keepWithin);
		};

		// The load sails: the landing of the whole load is chosen with the route by its first transport
		// that sails this tick (ChooseOpTripLanding), and each transport sets down on its own cell around
		// it along its own route.
		auto const depart = [&](NeuralOpTransport& transport, FootClass* pTransport, OpCarrier carrier)
		{
			transport.Leg = NeuralOpLeg::Crossing;
			transport.LegFrame = now;
			transport.Tries = 0;
			transport.ParkFrame = -1;
			transport.ParkCell = CellStruct::Empty;
			transport.Carried = true;

			if (op.TripLandingFrame != now)
			{
				op.TripLanding = ChooseOpTripLanding(pHouse, op, pTransport, carrier, op.TripDanger);
				op.TripLandingFrame = now;
			}

			auto const pLanding = PickOpLandingCell(op, pTransport, carrier, op.TripLanding, CellStruct::Empty);
			transport.Landing = pLanding ? pLanding->MapCoords : op.TripLanding;
			transport.LandingDanger = OpLandingDanger(pHouse, transport.Landing, carrier);
			planRoute(transport, pTransport, carrier, transport.Landing, true);
			transport.ProgressCell = pTransport->GetMapCoords();
			transport.ProgressFrame = now;
			wake(pTransport);

			char way[96] = "";

			if (!transport.Route.empty())
			{
				std::snprintf(way, sizeof(way), " by a route of %d waypoints (danger %d against %d straight)",
					static_cast<int>(transport.Route.size()), transport.RouteDanger, transport.StraightDanger);
			}
			else if (transport.StraightDanger > 0)
			{
				std::snprintf(way, sizeof(way), " straight in (danger %d)", transport.StraightDanger);
			}

			Debug::Log("[NeuralAI] House %d transport op %d: %s %u sails from (%d,%d) with %d aboard (size %d/%d) for (%d,%d)%s, frame %d.\n",
				houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, pTransport->GetMapCoords().X,
				pTransport->GetMapCoords().Y, pTransport->Passengers.NumPassengers, pTransport->Passengers.GetTotalSize(),
				pTransport->GetTechnoType()->Passengers, transport.Landing.X, transport.Landing.Y, way, now);
		};

		const bool capReached = op.BoardingSince >= 0 && (now - op.BoardingSince >= OpBoardingCapFrames || now < op.BoardingSince);
		bool sailNow = anyLoadedParked && (allAboard || capReached) && !shuttleAtWork;

		// Too dangerous even along the safest route (user: the load waits on its beach, with a cap): it
		// waits for the danger to pass (moving enemies go, sightings fade), planned again every
		// OpDangerCheckFrames, OpDangerWaitFrames at most.
		if (sailNow)
		{
			if (op.DangerCheckFrame < 0 || now - op.DangerCheckFrame >= OpDangerCheckFrames || now < op.DangerCheckFrame)
			{
				op.DangerCheckFrame = now;

				for (auto const& transport : op.Transports)
				{
					auto const pTransport = find(transport.UniqueID);

					if (transport.Leg == NeuralOpLeg::Parked && pTransport->Passengers.NumPassengers > 0)
					{
						op.TripLanding = ChooseOpTripLanding(pHouse, op, pTransport, GetOpCarrier(pTransport), op.TripDanger);
						op.TripLandingFrame = now;
						break;
					}
				}
			}

			if (op.TripDanger > OpSailDangerLimit)
			{
				if (op.DangerWaitSince < 0 || now < op.DangerWaitSince)
				{
					op.DangerWaitSince = now;
					op.DangerHolds++;
					Debug::Log("[NeuralAI] House %d transport op %d: the load waits on its beach, its safest route has a danger of %d (limit %d), "
						"frame %d.\n", houseIndex, op.Id, op.TripDanger, OpSailDangerLimit, now);
				}

				// Still above OpAbortDangerLimit at the end of the wait, along the safest route to the cheapest
				// landing of the target's land: a die says whether the load sails (user: 50% it goes on the
				// adventure, 50% it aborts, and troops and transports stay idle for others to take). Not while
				// another load of the operation is crossing: its transports must bring it over.
				const bool crossing = std::any_of(op.Transports.begin(), op.Transports.end(),
					[](const NeuralOpTransport& transport) { return transport.Leg == NeuralOpLeg::Crossing; });

				if (now - op.DangerWaitSince < OpDangerWaitFrames)
				{
					sailNow = false;
				}
				else if (op.TripDanger > OpAbortDangerLimit && !crossing && ScenarioClass::Instance->Random.RandomRanged(0, 99) < OpAbortPercent)
				{
					// Unloading to load again is time lost (user): another operation of the same client from the
					// same land, bound for other ground and with members waiting for a seat, takes the loaded
					// transports with their loads. With none, the loads are set down.
					NeuralTransportOperation* pOther = nullptr;

					for (auto& other : macro.TransportOps)
					{
						if (&other == &op || other.Over || other.Client != op.Client || other.OriginZone != op.OriginZone
							|| other.TargetZone == op.TargetZone)
						{
							continue;
						}

						if (std::any_of(other.Members.begin(), other.Members.end(),
							[](const NeuralOpMember& member) { return !member.Aboard && !member.Transport; }))
						{
							pOther = &other;
							break;
						}
					}

					std::vector<DWORD> handed;
					int handedMembers = 0;
					int out = 0;

					for (auto const& transport : op.Transports)
					{
						auto const pTransport = find(transport.UniqueID);

						if (transport.Leg != NeuralOpLeg::Parked || pTransport->Passengers.NumPassengers <= 0)
							continue;

						if (!pOther)
						{
							out += PutAshoreNow(pTransport);
							continue;
						}

						NeuralOpTransport record {};
						record.UniqueID = transport.UniqueID;
						record.Type = transport.Type;
						record.LegFrame = now;
						pOther->Transports.push_back(record);
						pOther->TransportsTaken++;
						macro.TransportOpOf[transport.UniqueID] = pOther->Id;
						handed.push_back(transport.UniqueID);

						for (auto const& member : op.Members)
						{
							if (!member.Aboard || member.Transport != transport.UniqueID)
								continue;

							NeuralOpMember moved = member;
							moved.Target = pOther->Target;
							moved.OrderCell = CellStruct::Empty;
							moved.ProgressCell = CellStruct::Empty;
							pOther->Members.push_back(moved);
							pOther->Joined++;
							macro.TransportOpOf[member.UniqueID] = pOther->Id;
							handedMembers++;
						}
					}

					auto const wasHanded = [&handed](DWORD uid) { return std::find(handed.begin(), handed.end(), uid) != handed.end(); };
					std::erase_if(op.Members, [&wasHanded](const NeuralOpMember& member) { return member.Aboard && wasHanded(member.Transport); });
					std::erase_if(op.Transports, [&wasHanded](const NeuralOpTransport& transport) { return wasHanded(transport.UniqueID); });

					Debug::Log("[NeuralAI] House %d transport op %d: the load aborts after waiting %d frames, its route still has a danger of %d "
						"(abort above %d, the die said no): %d transports with %d aboard go to op %d, %d set down, frame %d.\n", houseIndex, op.Id,
						now - op.DangerWaitSince, op.TripDanger, OpAbortDangerLimit, static_cast<int>(handed.size()), handedMembers,
						pOther ? pOther->Id : -1, out, now);
					endOp("too dangerous", true);
					return;
				}
				else
				{
					Debug::Log("[NeuralAI] House %d transport op %d: the load sails after waiting %d frames, its route still has a danger of %d, "
						"frame %d.\n", houseIndex, op.Id, now - op.DangerWaitSince, op.TripDanger, now);
				}
			}
			else if (op.DangerWaitSince >= 0)
			{
				Debug::Log("[NeuralAI] House %d transport op %d: the danger passed after %d frames (route danger %d), the load sails, frame %d.\n",
					houseIndex, op.Id, now - op.DangerWaitSince, op.TripDanger, now);
			}
		}

		if (sailNow)
		{
			op.DangerWaitSince = -1;
			op.DangerCheckFrame = -1;

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
		// for more), else it is let go. A shuttle's stays where it set its load down until the loads
		// turn about, then waits next to its harvesters.
		auto const afterUnload = [&](NeuralOpTransport& transport)
		{
			if (shuttle && waitingSize == 0)
				return;

			if (waitingSize > 0)
			{
				transport.Leg = NeuralOpLeg::ToStaging;
				transport.LegFrame = now;
				transport.ParkCell = CellStruct::Empty;
				transport.ParkFrame = -1;
				transport.Tries = 0;
				transport.Route.clear();
				transport.RouteFrame = -1;
			}
			else
			{
				released.push_back(transport.UniqueID);
			}
		};

		// Sets the load down at once on the ground below (PutAshoreNow); the load counted for the
		// set-downs the engine makes follows.
		auto const putAshore = [&](NeuralOpTransport& transport, FootClass* pTransport) -> int
		{
			const int out = PutAshoreNow(pTransport);
			transport.LastLoaded = pTransport->Passengers.NumPassengers;

			return out;
		};

		// Sets the load down at the landing: on the ground at once (PutAshoreNow; the unload mission
		// waits for a hovercraft to report no movement, which it does not while it floats); flying, it is
		// told to unload, which brings it down; a carryall is sent to the cell, where the engine sets its
		// vehicle down at the end of the move (told to unload, an aircraft that is no dropship flies off
		// to the map's reinforcement waypoint, AircraftClass::Do_MISSION_UNLOAD). Still loaded
		// OpUnloadRetryFrames later, it tries another cell.
		auto const unloadAt = [&](NeuralOpTransport& transport, FootClass* pTransport, OpCarrier carrier, bool aloft)
		{
			const int aboardBefore = pTransport->Passengers.NumPassengers;
			// How it sets down where it stands, and how it goes on to another cell.
			const Mission setDown = carrier == OpCarrier::Carryall ? Mission::Move : Mission::Unload;
			const Mission moveOn = carrier == OpCarrier::Flyer ? Mission::Unload : Mission::Move;

			if (!aloft && carrier != OpCarrier::Carryall)
			{
				const int out = putAshore(transport, pTransport);

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
					pTransport->QueueMission(moveOn, true);
					transport.OrderCell = pNext->MapCoords;
				}

				return;
			}

			if (pTransport->CurrentMission != setDown && pLandingCell)
			{
				wake(pTransport);
				pTransport->SetDestination(pLandingCell, true);
				pTransport->QueueMission(setDown, true);
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

			// The vehicle a carryall goes to lift (the operation sends it to the vehicle itself).
			FootClass* pCargo = nullptr;

			if (carrier == OpCarrier::Carryall)
			{
				for (auto const& member : op.Members)
				{
					if (member.Transport == transport.UniqueID && !member.Aboard)
					{
						pCargo = find(member.UniqueID);
						break;
					}
				}
			}

			// Part of its load set down by the engine since the last tick: a flying transport's unload
			// mission, a carryall at the end of its move.
			if ((transport.Leg == NeuralOpLeg::Crossing || transport.Leg == NeuralOpLeg::Returning) && loaded < transport.LastLoaded)
			{
				Debug::Log("[NeuralAI] House %d transport op %d: %s %u sets %d of %d down at (%d,%d), %d frames after sailing, by the engine, "
					"frame %d.\n", houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, transport.LastLoaded - loaded,
					transport.LastLoaded, pTransport->GetMapCoords().X, pTransport->GetMapCoords().Y, now - transport.LegFrame, now);
			}

			transport.Type = pTransport->GetTechnoType();
			transport.LastCell = pTransport->GetMapCoords();
			transport.LastHealth = pTransport->Health;
			transport.LastLoaded = loaded;

			if (transport.Leg != NeuralOpLeg::Parked)
				checkInterference(pTransport, transport.OrderCell, true, pCargo);

			// No weapons: its job is carrying (user). A carryall has no target either: it lifts its vehicle
			// on a move sent to it.
			if (pTransport->Target)
				pTransport->SetTarget(nullptr);

			// Badly hit on land: it lets its load out at once (the members on their own land wait for
			// another seat, those on the land of their address are delivered) and leaves the operation.
			// Kept, it took its load aboard again at every tick and set it down again (33 times in a row,
			// match #10391).
			if (carrier != OpCarrier::Carryall && !aloft && onLand && pTransport->GetHealthPercentage() < OpDyingTransportHealth)
			{
				const int out = loaded > 0 ? putAshore(transport, pTransport) : 0;

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
						if (transport.HoldSince >= 0)
						{
							Debug::Log("[NeuralAI] House %d transport op %d: %s %u lifts its vehicle after waiting %d frames, its straight way still has a "
								"danger of %d, frame %d.\n", houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID,
								now - transport.HoldSince, transport.HoldDanger, now);
						}

						transport.HoldSince = -1;
						transport.HoldCheckFrame = -1;
						transport.HoldRolled = false;
						depart(transport, pTransport, carrier);
						break;
					}

					auto const pVehicle = abstract_cast<UnitClass*>(pCargo);

					// A shuttle's carryall with no harvester to lift yet waits by the staging cell.
					if (!pVehicle && shuttle && transport.Carried)
					{
						if (op.Staging != CellStruct::Empty && pTransport->GetMapCoords().DistanceFrom(op.Staging) > OpParkCells)
							followRoute(transport, pTransport, carrier, op.Staging, 2.0);

						break;
					}

					if (!pVehicle)
					{
						released.push_back(transport.UniqueID);
						break;
					}

					// Its straight way from the vehicle is too dangerous (it follows no waypoints, and it never parks
					// with a load to wait on the beach): the vehicle stays on the ground and the carryall waits by
					// the staging cell until the danger passes, OpDangerWaitFrames at most, measured again every
					// OpDangerCheckFrames.
					if (transport.HoldCheckFrame < 0 || now - transport.HoldCheckFrame >= OpDangerCheckFrames || now < transport.HoldCheckFrame)
					{
						transport.HoldCheckFrame = now;
						transport.HoldDanger = OpCarryallWayDanger(pHouse, op, pTransport, pVehicle->GetMapCoords());
					}

					if (transport.HoldDanger > OpSailDangerLimit)
					{
						if (transport.HoldSince < 0 || now < transport.HoldSince)
						{
							transport.HoldSince = now;
							op.DangerHolds++;
							Debug::Log("[NeuralAI] House %d transport op %d: %s %u leaves %s %u on the ground and waits, its straight way has a danger "
								"of %d (limit %d), frame %d.\n", houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID,
								pVehicle->GetTechnoType()->ID, pVehicle->UniqueID, transport.HoldDanger, OpSailDangerLimit, now);
						}

						if (now - transport.HoldSince < OpDangerWaitFrames)
						{
							if (pTransport->Destination == pVehicle)
								pTransport->SetDestination(nullptr, true);

							if (op.Staging != CellStruct::Empty && pTransport->GetMapCoords().DistanceFrom(op.Staging) > OpParkCells)
								followRoute(transport, pTransport, carrier, op.Staging, 2.0);

							break;
						}

						// Still above OpAbortDangerLimit at the end of the wait: a die, thrown once, says whether it
						// lifts (user: 50% it goes on the adventure, 50% it aborts, and the vehicle and the carryall
						// stay idle for others to take; the vehicle may not ask for a crossing for OpRetryFrames).
						if (!transport.HoldRolled && transport.HoldDanger > OpAbortDangerLimit)
						{
							transport.HoldRolled = true;

							if (ScenarioClass::Instance->Random.RandomRanged(0, 99) < OpAbortPercent)
							{
								Debug::Log("[NeuralAI] House %d transport op %d: %s %u aborts after waiting %d frames, its straight way still has a danger "
									"of %d (abort above %d, the die said no); %s %u and the carryall are let go, frame %d.\n", houseIndex, op.Id,
									pTransport->GetTechnoType()->ID, pTransport->UniqueID, now - transport.HoldSince, transport.HoldDanger, OpAbortDangerLimit,
									pVehicle->GetTechnoType()->ID, pVehicle->UniqueID, now);
								const DWORD vehicleId = pVehicle->UniqueID;
								std::erase_if(op.Members, [vehicleId](const NeuralOpMember& member) { return member.UniqueID == vehicleId; });
								macro.TransportOpOf.erase(vehicleId);
								macro.TransportOpRetryUntil[vehicleId] = now + OpRetryFrames;
								op.Released++;
								wake(pVehicle);
								pVehicle->SetDestination(nullptr, true);
								pVehicle->QueueMission(Mission::Guard, false);

								if (pTransport->Destination == pVehicle)
									pTransport->SetDestination(nullptr, true);

								released.push_back(transport.UniqueID);
								break;
							}
						}
					}
					else if (transport.HoldSince >= 0)
					{
						Debug::Log("[NeuralAI] House %d transport op %d: the danger passed after %d frames (straight way danger %d), %s %u goes for its "
							"vehicle, frame %d.\n", houseIndex, op.Id, now - transport.HoldSince, transport.HoldDanger, pTransport->GetTechnoType()->ID,
							pTransport->UniqueID, now);
						transport.HoldSince = -1;
					}

					if (pTransport->Destination != pVehicle || pTransport->CurrentMission != Mission::Move)
						AdvanceRoutedPickup(pHouse, pTransport, pVehicle);

					transport.OrderCell = CellStruct::Empty;
					break;
				}

				// A shuttle keeps the transports that carried its harvesters: their drivers.
				if (loaded == 0 && !hasSeated(transport.UniqueID) && waitingSize == 0 && !(shuttle && transport.Carried))
				{
					released.push_back(transport.UniqueID);
					break;
				}

				// No staging cell yet (a shuttle's harvesters all inside a refinery): it waits where it is.
				if (op.Staging == CellStruct::Empty)
					break;

				// Its own berth around the staging cell, picked once (the staging cell itself when there is
				// none: its passengers then wait at the gathering spot).
				if (transport.Berth == CellStruct::Empty)
				{
					transport.Berth = PickOpBerth(op, pTransport, carrier, transport.BerthWait);

					if (transport.Berth == CellStruct::Empty)
					{
						transport.Berth = op.Staging;
						transport.BerthWait = CellStruct::Empty;
					}
				}

				auto const pBerthCell = MapClass::Instance.TryGetCellAt(transport.Berth);
				const double toStaging = pTransport->GetMapCoords().DistanceFrom(transport.Berth);

				// On its way along a safe route too: in the first matches of the routes 4 of the 7 hovercraft
				// lost died empty on their way back to the staging cell, by warships seen a moment before.
				if (toStaging > OpParkCells)
				{
					followRoute(transport, pTransport, carrier, transport.Berth, 2.0);
					break;
				}

				auto const park = [&]()
				{
					transport.Leg = NeuralOpLeg::Parked;
					transport.LegFrame = now;
					pTransport->SetDestination(nullptr, true);
					pTransport->ForceMission(Mission::Sleep);
					transport.OrderCell = CellStruct::Empty;

					if (op.BoardingSince < 0 && !shuttleAtWork)
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

						if (auto const pCell = PickOpFlyerCell(transport.Berth, op.OriginZone, ring, transport.ParkCell, memberCells))
						{
							if (hanging)
								transport.Tries++;

							transport.ParkCell = pCell->MapCoords;
							transport.ParkFrame = now;
							wake(pTransport);
							pTransport->SetArchiveTarget(nullptr);
							pTransport->SetDestination(pCell, true);
							pTransport->QueueMission(Mission::Move, true);
							NoteOrderCaller(pTransport, "move");
							transport.OrderCell = pCell->MapCoords;
						}
					}
					else if (auto const pGoing = abstract_cast<CellClass*>(pTransport->Destination); !pGoing && aloft)
					{
						if (auto const pPark = MapClass::Instance.TryGetCellAt(transport.ParkCell))
						{
							pTransport->SetDestination(pPark, true);
							pTransport->QueueMission(Mission::Move, true);
							NoteOrderCaller(pTransport, "move");
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
					orderTo(pTransport, transport.OrderCell, pBerthCell, 0.5);

				break;
			}
			case NeuralOpLeg::Parked:
			{
				// Took off or was pushed away: it parks anew.
				const CellStruct berth = transport.Berth != CellStruct::Empty ? transport.Berth : op.Staging;

				if ((carrier == OpCarrier::Flyer && aloft) || pTransport->GetMapCoords().DistanceFrom(berth) > OpParkCells + 6.0)
				{
					transport.Leg = NeuralOpLeg::ToStaging;
					transport.LegFrame = now;
					transport.ParkCell = CellStruct::Empty;
					break;
				}

				// Empty with nobody bound for it: let go (a shuttle's driver waits for its harvesters).
				if (loaded == 0 && !hasSeated(transport.UniqueID) && !(shuttle && transport.Carried))
				{
					released.push_back(transport.UniqueID);
					break;
				}

				// Moved by someone else: stopped again. It sleeps (on Guard a transport with a gun takes off
				// or drives at whatever comes into range).
				if (onTheMove)
				{
					checkInterference(pTransport, transport.OrderCell, true, nullptr);
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

				const double toLanding = pTransport->GetMapCoords().DistanceFrom(transport.Landing);
				const double unloadCells = carrier == OpCarrier::Carryall ? 3.0 : OpUnloadCells;

				if (toLanding <= unloadCells && (onLand || aloft))
				{
					unloadAt(transport, pTransport, carrier, aloft);
					break;
				}

				// Turned back: home along a route planned anew.
				auto const turnBack = [&]()
				{
					transport.Leg = NeuralOpLeg::Returning;
					transport.LegFrame = now;
					transport.ParkFrame = -1;
					transport.Route.clear();
					transport.RouteFrame = -1;
					op.FailedTrips++;
				};

				if (now - transport.LegFrame >= OpCrossCapFrames || now < transport.LegFrame)
				{
					Debug::Log("[NeuralAI] House %d transport op %d: %s %u still crossing after %d frames, %d cells from (%d,%d): takes its "
						"load back, frame %d.\n", houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID,
						now - transport.LegFrame, static_cast<int>(toLanding), transport.Landing.X, transport.Landing.Y, now);
					turnBack();
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
						if (!aloft && onLand && carrier != OpCarrier::Carryall && putAshore(transport, pTransport) > 0)
						{
							Debug::Log("[NeuralAI] House %d transport op %d: %s %u stuck, sets its load down at (%d,%d), frame %d.\n",
								houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, pTransport->GetMapCoords().X,
								pTransport->GetMapCoords().Y, now);
						}
						else
						{
							turnBack();
						}

						break;
					}

					if (auto const pNext = PickOpLandingCell(op, pTransport, carrier, transport.Landing, transport.Landing))
					{
						Debug::Log("[NeuralAI] House %d transport op %d: %s %u stuck at (%d,%d) on its way to (%d,%d), tries (%d,%d), frame %d.\n",
							houseIndex, op.Id, pTransport->GetTechnoType()->ID, pTransport->UniqueID, pTransport->GetMapCoords().X,
							pTransport->GetMapCoords().Y, transport.Landing.X, transport.Landing.Y, pNext->MapCoords.X, pNext->MapCoords.Y, now);
						transport.Landing = pNext->MapCoords;
						transport.Route.clear();
						transport.RouteFrame = -1;
					}
				}

				// Enemies gathered at its landing since it sailed (user: the threats that move): the load takes
				// another landing when one costs OpLandingSwitchPercent or less of it, once per
				// OpLandingSwitchFrames for the operation, and the transports of the load bound for the old
				// one go along.
				if (toLanding >= OpLandingSwitchMinCells
					&& (op.LandingSwitchFrame < 0 || now - op.LandingSwitchFrame >= OpLandingSwitchFrames || now < op.LandingSwitchFrame))
				{
					const int landingDanger = OpLandingDanger(pHouse, transport.Landing, carrier);

					if (landingDanger - transport.LandingDanger >= OpLandingSwitchDanger)
					{
						op.LandingSwitchFrame = now;
						std::vector<CellStruct> candidates;
						std::vector<int> extraWalk;
						CollectOpLandingCandidates(op, pTransport, carrier, transport.Landing, candidates, extraWalk);
						OpRoutePlan plan;

						if (PlanOpRoute(pHouse, pTransport, carrier, candidates, extraWalk, plan) && plan.Landing != transport.Landing
							&& (plan.Totals.front() == INT_MAX
								|| static_cast<long long>(plan.Total) * 100 <= static_cast<long long>(plan.Totals.front()) * OpLandingSwitchPercent))
						{
							const CellStruct old = transport.Landing;
							int moved = 0;
							op.TripLanding = plan.Landing;
							op.LandingSwitches++;

							for (auto& other : op.Transports)
							{
								auto const pOther = find(other.UniqueID);

								if (other.Leg != NeuralOpLeg::Crossing || !pOther || other.Landing.DistanceFrom(old) > 2.0 * OpLandingSpacingCells)
									continue;

								const OpCarrier otherCarrier = GetOpCarrier(pOther);
								auto const pCell = PickOpLandingCell(op, pOther, otherCarrier, plan.Landing, CellStruct::Empty);
								other.Landing = pCell ? pCell->MapCoords : plan.Landing;
								other.LandingDanger = OpLandingDanger(pHouse, other.Landing, otherCarrier);
								other.Route.clear();
								other.RouteFrame = -1;
								other.ParkFrame = -1;
								other.Tries = 0;
								moved++;
							}

							Debug::Log("[NeuralAI] House %d transport op %d: enemies gathered at the landing (%d,%d) (danger %d, %d when it sailed): %d "
								"transports of the load land at (%d,%d) instead, cost %d against %s, frame %d.\n", houseIndex, op.Id, old.X, old.Y,
								landingDanger, transport.LandingDanger, moved, plan.Landing.X, plan.Landing.Y, plan.Total,
								plan.Totals.front() == INT_MAX ? "no way" : std::to_string(plan.Totals.front()).c_str(), now);
						}
					}
				}

				// Along the safe route to the landing. A carryall keeps its move unless the cell moves farther
				// than the engine's own choice of where to come down near it.
				followRoute(transport, pTransport, carrier, transport.Landing, carrier == OpCarrier::Carryall ? 4.0 : 1.0);
				break;
			}
			case NeuralOpLeg::Returning:
			{
				// A load it could not set down goes back to the staging beach along a safe route too, and
				// gets out there.
				if (loaded == 0)
				{
					afterUnload(transport);
					break;
				}

				if (pTransport->GetMapCoords().DistanceFrom(op.Staging) <= OpParkCells && (onLand || aloft))
				{
					if (!aloft && carrier != OpCarrier::Carryall && putAshore(transport, pTransport) > 0)
						break;

					const Mission setDown = carrier == OpCarrier::Carryall ? Mission::Move : Mission::Unload;

					if (pTransport->CurrentMission != setDown && pStagingCell)
					{
						wake(pTransport);
						pTransport->SetDestination(pStagingCell, true);
						pTransport->QueueMission(setDown, true);
						transport.OrderCell = op.Staging;
					}

					break;
				}

				followRoute(transport, pTransport, carrier, op.Staging, carrier == OpCarrier::Carryall ? 4.0 : 1.0);
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

	// The harvester shuttle client (user): once the ore of the lands we hold is nearly gone
	// (UpdateIslandOreOutlook) and a free field lies across water, on a land with no refinery of ours
	// and no construction yard of ours to place one, idle harvesters ride the idle transports there,
	// mine it and are brought back full, round after round (a bridge too while the island's MCV is on
	// its way: the shuttle ends once a refinery of ours stands on the field). Never chrono miners, which
	// warp back to their refinery on their own, nor miners that deploy (the slave miners): those are
	// handled apart. One shuttle per house, checked every ShuttleCheckFrames.
	void UpdateHarvesterShuttles(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (macro.ShuttleCheckFrame >= 0 && now - macro.ShuttleCheckFrame < ShuttleCheckFrames && now >= macro.ShuttleCheckFrame)
			return;

		macro.ShuttleCheckFrame = now;

		if (macro.ShuttleSkipUntil.size() > 64)
			std::erase_if(macro.ShuttleSkipUntil, [now](const auto& entry) { return now >= entry.second; });

		if (macro.ShuttleRetryUntil >= 0 && now < macro.ShuttleRetryUntil && macro.ShuttleRetryUntil - now <= ShuttleRetryFrames)
			return;

		if (macro.IslandOutlookFrame < 0 || macro.IslandExpansionField == CellStruct::Empty || macro.IslandHomeOre >= ShuttleHomeOre)
			return;

		if (std::any_of(macro.TransportOps.begin(), macro.TransportOps.end(),
			[](const NeuralTransportOperation& op) { return !op.Over && op.Client == NeuralOpClient::Shuttle; }))
		{
			return;
		}

		const CellStruct field = macro.IslandExpansionField;
		const int fieldZone = MapClass::Instance.GetMovementZoneType(field, MovementZone::Normal, false);

		if (!IsGroundZone(fieldZone))
			return;

		// Our refineries and their lands. A refinery or construction yard of ours on the field's land
		// leaves it to the base there.
		std::vector<std::pair<int, CellStruct>> refineries;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			const bool refinery = pBld->Type->Refinery || pBld->Type->ResourceDestination;

			if (!refinery && !pBld->Type->ConstructionYard)
				continue;

			const int zone = ObjectZone(pBld, MovementZone::Normal);

			if (zone == fieldZone)
				return;

			if (refinery)
				refineries.emplace_back(zone, pBld->GetMapCoords());
		}

		// Idle harvesters of ours on a land with a refinery of ours: empty, out of the refinery and
		// doing nothing, or looking in vain for ore. The land of the first one found is the shuttle's home.
		std::vector<UnitClass*> harvesters;
		int homeZone = -1;

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || pUnit->Health <= 0 || !pUnit->Type)
				continue;

			auto const pType = pUnit->Type;

			if (!pType->Harvester || pType->Weeder || pType->Teleporter || pType->DeploysInto || CrossesWaterAlone(pType)
				|| pUnit->Transporter || IsInTransportOp(pHouse, pUnit->UniqueID) || IsPlanMember(pHouse, pUnit->UniqueID)
				|| pUnit->IsMindControlled() || pUnit->Deactivated || pUnit->IsUnderEMP() || pUnit->GetHealthPercentage() < OpTransportMinHealth)
			{
				continue;
			}

			// One that could not get to its ride lately stays.
			if (auto const itSkip = macro.ShuttleSkipUntil.find(pUnit->UniqueID); itSkip != macro.ShuttleSkipUntil.end()
				&& now < itSkip->second && itSkip->second - now <= ShuttleStuckRetryFrames)
			{
				continue;
			}

			const Mission mission = pUnit->CurrentMission;
			const bool idle = mission == Mission::Guard || mission == Mission::Area_Guard || mission == Mission::Sleep
				|| mission == Mission::None || (mission == Mission::Harvest && !pUnit->Locomotor->Is_Moving());

			if (!idle || pUnit->HasAnyLink() || pUnit->Tiberium.GetTotalAmount() >= 0.5f)
				continue;

			const int zone = OpFootGroundZone(pUnit);

			if (zone < 0 || (homeZone >= 0 && zone != homeZone)
				|| std::none_of(refineries.begin(), refineries.end(), [zone](const std::pair<int, CellStruct>& refinery) { return refinery.first == zone; }))
			{
				continue;
			}

			homeZone = zone;
			harvesters.push_back(pUnit);
		}

		if (harvesters.empty())
			return;

		auto const pLanding = FindSafeIslandLandingCell(pHouse, field, fieldZone, true);

		if (!pLanding)
			return;

		// The rides: idle transports of ours that take a harvester and make the crossing from its land
		// to the field (a carryall slings one, a passenger transport takes as many as fit).
		auto const pSample = harvesters.front();
		const CellStruct sampleCell = pSample->GetMapCoords();
		const int size = std::max(1, static_cast<int>(pSample->GetTechnoType()->Size));
		int rides = 0;
		int seats = 0;

		auto const considerRide = [&](FootClass* pFoot)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo)
				return;

			const OpCarrier carrier = GetOpCarrier(pFoot);

			if (carrier == OpCarrier::None || !OpCarrierTakes(pFoot, carrier, pSample))
				return;

			if (!IsTransportFreeForOp(pHouse, pFoot, carrier))
				return;

			auto const pType = pFoot->GetTechnoType();

			if (!CanTransportTypeLink(pType, pFoot->GetMapCoords(), sampleCell) || !CanTransportTypeLink(pType, sampleCell, pLanding->MapCoords))
				return;

			rides++;
			seats += carrier == OpCarrier::Carryall ? 1 : pType->Passengers / size;
		};

		for (auto const pUnit : UnitClass::Array)
			considerRide(pUnit);

		for (auto const pAir : AircraftClass::Array)
			considerRide(pAir);

		if (seats <= 0)
		{
			if (macro.ShuttleNoRideLogFrame < 0 || now - macro.ShuttleNoRideLogFrame >= 4500 || now < macro.ShuttleNoRideLogFrame)
			{
				macro.ShuttleNoRideLogFrame = now;
				Debug::Log("[NeuralAI] House %d shuttle: %d idle harvesters on zone %d, no idle transport takes one to the field at (%d,%d) "
					"zone %d (home ore %d); %d credits, frame %d.\n", pHouse->ArrayIndex, static_cast<int>(harvesters.size()), homeZone, field.X,
					field.Y, fieldZone, macro.IslandHomeOre, pHouse->Available_Money(), now);
			}

			return;
		}

		// The harvesters nearest to the field go (fixed order among equals); the loads come home to the
		// refinery of their land nearest to the first of them.
		std::stable_sort(harvesters.begin(), harvesters.end(),
			[&field](UnitClass* a, UnitClass* b) { return a->GetMapCoords().DistanceFrom(field) < b->GetMapCoords().DistanceFrom(field); });

		CellStruct home = CellStruct::Empty;

		for (auto const& [zone, cell] : refineries)
		{
			if (zone == homeZone && (home == CellStruct::Empty
				|| cell.DistanceFrom(harvesters.front()->GetMapCoords()) < home.DistanceFrom(harvesters.front()->GetMapCoords())))
			{
				home = cell;
			}
		}

		const int count = std::min({ seats, ShuttleMaxHarvesters, static_cast<int>(harvesters.size()) });
		int opId = 0;
		int taken = 0;

		for (int i = 0; i < count; ++i)
		{
			if (!RequestTransportOperation(pHouse, NeuralOpClient::Shuttle, harvesters[i], field))
				continue;

			auto const itOp = macro.TransportOpOf.find(harvesters[i]->UniqueID);
			auto const pOp = itOp != macro.TransportOpOf.end() ? FindTransportOp(pHouse, itOp->second) : nullptr;

			if (!pOp)
				continue;

			taken++;
			opId = pOp->Id;

			if (pOp->FieldZone < 0)
			{
				pOp->Field = pOp->Target;
				pOp->FieldZone = pOp->TargetZone;
				pOp->Home = home;
				pOp->HomeZone = pOp->OriginZone;
				pOp->PhaseFrame = now;
			}
		}

		if (taken > 0)
		{
			Debug::Log("[NeuralAI] House %d shuttle op %d opened: %d of %d idle harvesters of zone %d, %d idle transports with %d seats, to the "
				"field at (%d,%d) zone %d, home refinery (%d,%d); home ore %d, %d credits, frame %d.\n", pHouse->ArrayIndex, opId, taken,
				static_cast<int>(harvesters.size()), homeZone, rides, seats, field.X, field.Y, fieldZone, home.X, home.Y, macro.IslandHomeOre,
				pHouse->Available_Money(), now);
		}
	}

	// Does the house own a transport its operations can use (a ship, a flyer or a carryall)?
	bool HouseOwnsOpTransport(HouseClass* pHouse)
	{
		for (auto const pFoot : FootClass::Array)
		{
			if (pFoot && pFoot->Owner == pHouse && pFoot->IsAlive && !pFoot->InLimbo && GetOpCarrier(pFoot) != OpCarrier::None)
				return true;
		}

		return false;
	}

	// The landing by a cell: a free land cell of its ground on the shore within BackdoorLandingCells,
	// the one with the least danger for a load set down there.
	CellStruct FindShoreLandingBy(HouseClass* pHouse, const CellStruct& bc, int zone, int& danger)
	{
		CellStruct best = CellStruct::Empty;
		danger = INT_MAX;

		for (int dy = -BackdoorLandingCells; dy <= BackdoorLandingCells; dy += 2)
		{
			for (int dx = -BackdoorLandingCells; dx <= BackdoorLandingCells; dx += 2)
			{
				const CellStruct c { static_cast<short>(bc.X + dx), static_cast<short>(bc.Y + dy) };
				auto const pCell = MapClass::Instance.CoordinatesLegal(c) ? MapClass::Instance.TryGetCellAt(c) : nullptr;

				if (!pCell || pCell->LandType == LandType::Water || pCell->GetBuilding()
					|| MapClass::Instance.GetMovementZoneType(c, MovementZone::Normal, false) != zone)
				{
					continue;
				}

				bool shore = false;

				for (int sy = -2; sy <= 2 && !shore; ++sy)
				{
					for (int sx = -2; sx <= 2 && !shore; ++sx)
					{
						auto const pNear = MapClass::Instance.TryGetCellAt(CellStruct { static_cast<short>(c.X + sx), static_cast<short>(c.Y + sy) });
						shore = pNear && pNear->LandType == LandType::Water;
					}
				}

				if (!shore)
					continue;

				const int here = OpLandingDanger(pHouse, c, OpCarrier::Surface);

				if (here < danger)
				{
					danger = here;
					best = c;
				}
			}
		}

		return best;
	}

	// Back door (user, an idea of the old aimd.ini): while a wave of the house hits an enemy base on the
	// house's own ground through defenses, a small team of idle units at home rides a transport operation
	// around that base and lands by a valuable building on its far side, which it attacks (constants
	// Backdoor*). The transports keep to the safe routes and wait, or abort, on danger like any load.
	void UpdateBackdoor(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (macro.BackdoorCheckFrame >= 0 && now - macro.BackdoorCheckFrame < BackdoorCheckFrames && now >= macro.BackdoorCheckFrame)
			return;

		macro.BackdoorCheckFrame = now;

		// TEMP diagnostic: where the checks stop (no back door in 16 matches of its first builds), summed
		// per house and logged every 9000 frames.
		enum BackdoorStop { Waiting, NoFront, OtherGround, FewDefenses, NoVictim, NoTarget, SmallTeam, NoCarrier, Started, StopCount };
		static std::array<std::array<int, StopCount>, 8> stops {};
		static std::array<int, 8> stopsLogFrame {};
		static int stopsMatch = -1;

		if (stopsMatch != NeuralMatchSerial)
		{
			stopsMatch = NeuralMatchSerial;
			stops = {};
			stopsLogFrame.fill(0);
		}

		auto& houseStops = stops[static_cast<size_t>(pHouse->ArrayIndex) & 7];
		auto const stop = [&](BackdoorStop why)
		{
			houseStops[why]++;
			auto& logFrame = stopsLogFrame[static_cast<size_t>(pHouse->ArrayIndex) & 7];

			if (now - logFrame >= 9000 || now < logFrame)
			{
				logFrame = now;
				Debug::Log("[NeuralAI] House %d back door DIAG: checks stopped at waiting %d, no front %d, front on other ground %d, few defenses %d, "
					"no victim %d, no target with a safe landing %d, small team %d, no transport for the team %d, started %d, frame %d.\n",
					pHouse->ArrayIndex, houseStops[Waiting], houseStops[NoFront], houseStops[OtherGround], houseStops[FewDefenses],
					houseStops[NoVictim], houseStops[NoTarget], houseStops[SmallTeam], houseStops[NoCarrier], houseStops[Started], now);
			}
		};

		if (macro.BackdoorRetryUntil >= 0 && now < macro.BackdoorRetryUntil
			&& macro.BackdoorRetryUntil - now <= std::max(BackdoorRetryFrames, OpNoTransportRetryFrames))
		{
			stop(Waiting);
			return;
		}

		if (std::any_of(macro.TransportOps.begin(), macro.TransportOps.end(),
			[](const NeuralTransportOperation& op) { return !op.Over && op.Client == NeuralOpClient::Strike; }))
		{
			stop(Waiting);
			return;
		}

		// The front: the target of the house's latest wave, on the house's own ground, with defenses.
		const CellStruct front = macro.LastWaveTargetCell;
		const CellStruct home = pHouse->GetBaseCenter();

		if (front == CellStruct::Empty || home == CellStruct::Empty || now - macro.LastWaveDispatchFrame > BackdoorFrontFrames)
		{
			stop(NoFront);
			return;
		}

		const int homeZone = MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false);

		if (!IsGroundZone(homeZone) || MapClass::Instance.GetMovementZoneType(front, MovementZone::Normal, false) != homeZone)
		{
			stop(OtherGround);
			return;
		}

		const int frontDefenses = CountKnownEnemyDefensesNear(pHouse, front, BackdoorFrontCells, false);

		if (frontDefenses < BackdoorMinDefenses)
		{
			stop(FewDefenses);
			return;
		}

		auto const isEnemy = [pHouse](HouseClass* pOwner)
		{
			return pOwner && pOwner != pHouse && !pHouse->IsAlliedWith(pOwner) && !pOwner->IsNeutral() && !pOwner->IsObserver()
				&& !pOwner->Defeated;
		};

		// Whose base the wave hits: the owner of the known enemy building nearest to its target.
		HouseClass* pVictim = nullptr;
		double nearest = 0.0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !isEnemy(pBld->Owner) || !IsEnemyBuildingKnown(pHouse, pBld))
				continue;

			const double distance = pBld->GetMapCoords().DistanceFrom(front);

			if (!pVictim || distance < nearest)
			{
				pVictim = pBld->Owner;
				nearest = distance;
			}
		}

		if (!pVictim)
		{
			stop(NoVictim);
			return;
		}

		// The landing by a building: a free land cell of its ground on the shore, the one with the least
		// danger for a load set down there.
		auto const findLanding = [&](const CellStruct& bc, int zone, int& danger) -> CellStruct
		{
			return FindShoreLandingBy(pHouse, bc, zone, danger);
		};

		// The building: known, of that house, worth a strike (a factory, a construction yard, a power plant,
		// a refinery, a superweapon), far enough from the wave's target, with a safe landing by it.
		BuildingClass* pTarget = nullptr;
		CellStruct landing = CellStruct::Empty;
		int landingDanger = 0;
		int bestScore = INT_MIN;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || pBld->Owner != pVictim || !IsEnemyBuildingKnown(pHouse, pBld))
				continue;

			auto const pType = pBld->Type;

			if (pType->Factory == AbstractType::None && !pType->ConstructionYard && pType->PowerBonus <= 0 && !pType->Refinery
				&& pType->SuperWeapon == -1)
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			const double gap = bc.DistanceFrom(front);
			const int zone = MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge);

			if (gap < BackdoorMinGapCells || !IsGroundZone(zone))
				continue;

			const int guards = CountKnownEnemyDefensesNear(pHouse, bc, BackdoorGuardCells, false);
			const int score = static_cast<int>(gap) * 10 - guards * 100;

			if (score <= bestScore)
				continue;

			int danger = 0;
			const CellStruct here = findLanding(bc, zone, danger);

			if (here == CellStruct::Empty || danger > OpSailDangerLimit)
				continue;

			pTarget = pBld;
			landing = here;
			landingDanger = danger;
			bestScore = score;
		}

		if (!pTarget)
		{
			stop(NoTarget);
			return;
		}

		// The team: idle armed ground units of the house at home, the nearest to its base first, each one
		// with a seat on a transport the house owns or can build now that comes to it and links it to the
		// landing (CanHouseCarryNow, asked once per type; user, 2026-11-18: 13 of 14 back doors never
		// sailed, their vehicles waited 9000 frames for air transports that seat infantry only, or for a
		// hovercraft that could not leave the water).
		std::vector<std::pair<double, FootClass*>> idle;
		std::vector<std::pair<const TechnoTypeClass*, bool>> carriable;
		int noCarrier = 0;

		auto const canCarry = [&](const TechnoTypeClass* pType, const CellStruct& at)
		{
			for (auto const& [pKnown, answer] : carriable)
			{
				if (pKnown == pType)
					return answer;
			}

			const bool answer = CanHouseCarryNow(pHouse, pType, at, landing);
			carriable.push_back({ pType, answer });

			return answer;
		};

		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || pFoot->Owner != pHouse || pFoot->InLimbo || !pFoot->IsAlive || pFoot->Transporter)
				continue;

			const AbstractType kind = pFoot->WhatAmI();

			if (kind != AbstractType::Unit && kind != AbstractType::Infantry)
				continue;

			auto const pType = pFoot->GetTechnoType();
			auto const pWeapon = pType ? pType->GetWeapon(0, false).WeaponType : nullptr;

			if (!pWeapon || pWeapon->Damage <= 0 || CrossesWaterAlone(pType) || GetOpCarrier(pFoot) != OpCarrier::None)
				continue;

			if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
			{
				if (pUnitType->Harvester || pUnitType->DeploysInto)
					continue;
			}

			if (auto const pInfType = abstract_cast<InfantryTypeClass*>(pType))
			{
				if (pInfType->Engineer)
					continue;
			}

			const Mission mission = pFoot->CurrentMission;

			if ((mission != Mission::Guard && mission != Mission::Area_Guard && mission != Mission::Sleep) || pFoot->Target
				|| pFoot->Destination || IsPlanMember(pHouse, pFoot->UniqueID) || IsInTransportOp(pHouse, pFoot->UniqueID)
				|| macro.TankGuard.contains(pFoot->UniqueID))
			{
				continue;
			}

			const CellStruct at = pFoot->GetMapCoords();
			const double fromHome = at.DistanceFrom(home);

			if (fromHome > BackdoorHomeCells || OpFootGroundZone(pFoot) != homeZone)
				continue;

			if (!canCarry(pType, at))
			{
				noCarrier++;
				continue;
			}

			idle.push_back({ fromHome, pFoot });
		}

		if (static_cast<int>(idle.size()) < BackdoorMinTeam)
		{
			if (static_cast<int>(idle.size()) + noCarrier >= BackdoorMinTeam)
			{
				CountDecision(NeuralDecision::BackdoorNoCarrier, pHouse->ArrayIndex);
				stop(NoCarrier);
			}
			else
			{
				stop(SmallTeam);
			}

			return;
		}

		std::stable_sort(idle.begin(), idle.end(), [](const auto& a, const auto& b)
			{
				return a.first < b.first || (a.first == b.first && a.second->UniqueID < b.second->UniqueID);
			});

		int taken = 0;

		for (auto const& [distance, pFoot] : idle)
		{
			if (taken >= BackdoorTeam)
				break;

			if (RequestStrikeTransport(pHouse, pFoot, landing, pTarget->GetMapCoords()))
				taken++;
		}

		if (taken == 0)
		{
			stop(SmallTeam);
			return;
		}

		stop(Started);
		macro.BackdoorRetryUntil = now + BackdoorRetryFrames;
		Debug::Log("[NeuralAI] House %d back door: %d units to %s at (%d,%d) of house %d, landing (%d,%d) danger %d, %d cells from the wave's "
			"target (%d,%d) with %d defenses, frame %d.\n", pHouse->ArrayIndex, taken, pTarget->Type->ID, pTarget->GetMapCoords().X,
			pTarget->GetMapCoords().Y, pVictim->ArrayIndex, landing.X, landing.Y, landingDanger,
			static_cast<int>(pTarget->GetMapCoords().DistanceFrom(front)), front.X, front.Y, frontDefenses, now);
	}

	// Runs the house's transport operations once per AI tick, after every other part of the tick
	// has given its orders (the operations have the last word) and before production reads what the
	// waiting members ask for (OpWaitingSizeBySize). The harvester shuttle client books its rides first.
	void UpdateTransportOperations(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (TransportOpsEnabled)
		{
			UpdateHarvesterShuttles(pHouse);
			UpdateBackdoor(pHouse);
		}

		macro.OpWaitingSizeBySize.fill(0);
		macro.OpWaitingFrame = now;
		std::erase_if(macro.OpLossSpots, [now](const NeuralOpLossSpot& spot) { return now < spot.Frame || now - spot.Frame >= OpRouteLossFrames; });

		if (macro.TransportOps.empty())
			return;

		// The operations' own orders pass the order helpers, which turn their units down for the rest of
		// the AI tick (IsHeldByTransportOp).
		struct OpOrdersOpen
		{
			OpOrdersOpen()
			{
				TransportOpOrdersOpen = true;
			}

			~OpOrdersOpen()
			{
				TransportOpOrdersOpen = false;
			}
		} const ordersOpen;

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

		// The danger of the crossings that wait for transports, straight from the staging cell to the
		// landing, on the ground and in the air: the kind of transport the operations take and the
		// factories build follows it.
		if (macro.OpKindDangerFrame < 0 || now - macro.OpKindDangerFrame >= OpKindDangerFrames || now < macro.OpKindDangerFrame)
		{
			macro.OpKindDanger.fill(0);
			macro.OpKindDangerFrame = now;

			for (auto const& op : macro.TransportOps)
			{
				const bool waiting = op.Staging != CellStruct::Empty && op.Landing != CellStruct::Empty
					&& std::any_of(op.Members.begin(), op.Members.end(), [](const NeuralOpMember& member) { return !member.Aboard; });

				if (!waiting)
					continue;

				for (size_t kind = 0; kind < 2; ++kind)
				{
					int danger = 0;
					OpLineCost(GetOpDangerMap(pHouse, kind == 1), op.Staging, op.Landing, danger);
					macro.OpKindDanger[kind] = std::max(macro.OpKindDanger[kind], danger);
				}
			}
		}

		// Retry marks past their time go.
		if (macro.TransportOpRetryUntil.size() > 256)
			std::erase_if(macro.TransportOpRetryUntil, [now](const auto& entry) { return now >= entry.second; });
	}

	// How much the danger of the crossings waiting for transports (OpKindDanger) lowers the factories'
	// score of a transport type: none for a burrowing one, OpKindDangerPenalty per unit of the danger on
	// the ground or in the air, OpKindDangerMaxPenalty at most; nothing once the measure is old.
	int OpCrossingDangerPenalty(HouseClass* pHouse, const TechnoTypeClass* pType)
	{
		auto const& macro = GetMacroState(pHouse);
		const int age = Unsorted::CurrentFrame - macro.OpKindDangerFrame;

		if (!pType || macro.OpKindDangerFrame < 0 || age < 0 || age > 4 * OpKindDangerFrames)
			return 0;

		auto const pUnitType = abstract_cast<const UnitTypeClass*>(pType);

		if (pUnitType && IsUndergroundTransportType(pUnitType))
			return 0;

		return std::min(OpKindDangerMaxPenalty, macro.OpKindDanger[OpTypeFlies(pType) ? 1 : 0] * OpKindDangerPenalty);
	}
}
