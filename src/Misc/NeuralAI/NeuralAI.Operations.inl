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

		// A carryall on its way to a vehicle (a move sent to it) is lifting it.
		if (carrier == OpCarrier::Carryall)
		{
			return pFoot->CurrentMission != Mission::Unload && !abstract_cast<UnitClass*>(pFoot->Destination)
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
	// them hits it), OpRouteMaxReachCells at most.
	int OpThreatReach(const TechnoTypeClass* pType, bool airborne)
	{
		int reach = 0;

		for (size_t i = 0; i < 2; ++i)
		{
			auto const pWeapon = pType->GetWeapon(i, false).WeaponType;

			if (!pWeapon || pWeapon->Damage <= 0)
				continue;

			auto const pProjectile = pWeapon->Projectile;

			if (pProjectile ? (airborne ? !pProjectile->AA : !pProjectile->AG) : airborne)
				continue;

			reach = std::max(reach, pWeapon->Range / Unsorted::LeptonsPerCell);
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

			const int reach = OpThreatReach(pBld->Type, airborne);

			if (reach > 0)
				threats.push_back({ pBld->GetMapCoords(), reach + OpRouteDefenseMarginCells, OpRouteDefenseWeight, OpThreatKind::Defense, pBld->Type });
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
			const int reach = pType ? OpThreatReach(pType, airborne) : 0;

			if (reach > 0)
				threats.push_back({ pCell->MapCoords, reach + OpRouteUnitMarginCells, OpRouteUnitWeight, OpThreatKind::Unit, pType });
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

			const int reach = OpThreatReach(sighting.Type, airborne);

			if (reach > 0)
			{
				const int weight = std::max(1, OpRouteSightingWeight * (OpRouteSightingFrames - age) / OpRouteSightingFrames);
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
	// reached.
	bool PlanOpRoute(HouseClass* pHouse, FootClass* pTransport, OpCarrier carrier, const std::vector<CellStruct>& landings,
		const std::vector<int>& extraWalk, OpRoutePlan& plan)
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
		const CellStruct from = pTransport->GetMapCoords();
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

		for (int dy = -OpLandingCandidateCells; dy <= OpLandingCandidateCells; dy += 2)
		{
			for (int dx = -OpLandingCandidateCells; dx <= OpLandingCandidateCells; dx += 2)
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

		// A seat the old ferry loader held for it goes: that transport would call it to its door.
		macro.FerryReserved.erase(uid);
		macro.FerryWaitSince.erase(uid);

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

			// None of ours makes this crossing: the production builds one that does, or what it lacks.
			if (candidates.empty() && op.Transports.empty())
			{
				int largest = 1;

				for (auto const pMember : unseated)
					largest = std::max(largest, pMember->Size);

				NoteCrossingNeed(pHouse, op.Staging, op.Landing, largest);
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
			// transport has a seat for it; with no land there, at the gathering spot.
			const CellStruct at = pFoot->GetMapCoords();
			const double fromStaging = at.DistanceFrom(op.Staging);
			constexpr double HoldCells = OpBerthRingCells + OpBerthWaitCells;

			if (fromStaging >= HoldCells - 2.0 && fromStaging <= 2.0 * HoldCells)
				continue;

			const double towardX = fromStaging >= 1.0 ? (at.X - op.Staging.X) / fromStaging : 1.0;
			const double towardY = fromStaging >= 1.0 ? (at.Y - op.Staging.Y) / fromStaging : 0.0;
			const CellStruct hold { static_cast<short>(op.Staging.X + std::lround(towardX * HoldCells)),
				static_cast<short>(op.Staging.Y + std::lround(towardY * HoldCells)) };
			auto const pHold = MapClass::Instance.CoordinatesLegal(hold) ? MapClass::Instance.TryGetCellAt(hold) : nullptr;

			if (pHold && pHold->LandType != LandType::Water && !pHold->GetBuilding()
				&& MapClass::Instance.GetMovementZoneType(hold, MovementZone::Normal, false) == op.OriginZone)
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
		bool sailNow = anyLoadedParked && (allAboard || capReached);

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

				if (now - op.DangerWaitSince < OpDangerWaitFrames)
				{
					sailNow = false;
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
						depart(transport, pTransport, carrier);
						break;
					}

					auto const pVehicle = abstract_cast<UnitClass*>(pCargo);

					if (!pVehicle)
					{
						released.push_back(transport.UniqueID);
						break;
					}

					if (pTransport->Destination != pVehicle || pTransport->CurrentMission != Mission::Move)
						AdvanceRoutedPickup(pHouse, pTransport, pVehicle);

					transport.OrderCell = CellStruct::Empty;
					break;
				}

				if (loaded == 0 && !hasSeated(transport.UniqueID) && waitingSize == 0)
				{
					released.push_back(transport.UniqueID);
					break;
				}

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

	// Runs the house's transport operations once per AI tick, after every other part of the tick
	// has given its orders (the operations have the last word) and before production reads what the
	// waiting members ask for (OpWaitingSizeBySize).
	void UpdateTransportOperations(HouseClass* pHouse)
	{
		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		macro.OpWaitingSizeBySize.fill(0);
		macro.OpWaitingFrame = now;
		std::erase_if(macro.OpLossSpots, [now](const NeuralOpLossSpot& spot) { return now < spot.Frame || now - spot.Frame >= OpRouteLossFrames; });

		if (macro.TransportOps.empty())
		{
			macro.OpWatch.clear();
			return;
		}

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
