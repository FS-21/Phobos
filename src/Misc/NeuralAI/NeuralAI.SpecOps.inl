// NeuralAI.SpecOps.inl: Expert 4 (Special Operations): fog-fair memory of neutral tech
// buildings, capture / spy / bridge opportunities that feed the expert selector, escorted
// engineer captures and the bookkeeping of engineers and spies sent out.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// What the map starts with (its capturable buildings, the civilian buildings soldiers can
	// occupy) is known to every house the way its ore fields are: players see the map before the
	// match. It is taken as known only this early, while it still is as the map has it; what became
	// of each building later is learned by seeing it.
	constexpr int MapKnowledgeFrames = 900;

	// A capturable building this house knows (from the map or seen), as it looked the last time it
	// was in sight.
	struct NeuralKnownTech
	{
		DWORD ID { 0 };
		CellStruct Cell { CellStruct::Empty };
		int Value { 0 };
		bool Cash { false };
		// Owner index when last seen; -1 while it was neutral.
		int OwnerIndex { -1 };
		// Movement zones (Normal) of the cells around the foundation, where an engineer enters from.
		std::array<int, 4> Zones { -1, -1, -1, -1 };
		int SeenFrame { 0 };
		int Failures { 0 };
		int BlockedUntilFrame { 0 };
		// Walled in as seen from home ground at the last scan: only an Expert 4 op with a wall
		// breaker takes it.
		bool Walled { false };
	};

	// One engineer on a capture, with its escort.
	struct NeuralSpecOpsCapture
	{
		DWORD EngineerID { 0 };
		DWORD TargetID { 0 };
		CellStruct TargetCell { CellStruct::Empty };
		int StartFrame { 0 };
		bool Dispatched { false };
		int DispatchFrame { 0 };
		std::vector<DWORD> Escorts {};
		// Walled-in tech: the wall piece to knock down first (Empty: none or already open), our
		// ground next to it, its movement zone (Normal), and the units shooting it.
		CellStruct BreachCell { CellStruct::Empty };
		CellStruct BreachApproach { CellStruct::Empty };
		int BreachZone { -1 };
		int BreachFrame { 0 };
		int BreachPieces { 0 };
		bool BreachReinforced { false };
		// A breach made from another land than the engineer's: the engineer's land (-1: same land),
		// from where engineer and breakers go over by transport, and whether a breaker stands on the
		// land of the wall yet (the breach's time runs from then).
		int BreachFerryZone { -1 };
		bool BreachLanded { false };
		std::vector<DWORD> Breachers {};
	};

	// One spy sent to infiltrate a building.
	struct NeuralSpecOpsSpy
	{
		DWORD SpyID { 0 };
		DWORD TargetID { 0 };
		CellStruct TargetCell { CellStruct::Empty };
		int TargetOwner { -1 };
		int LastDistance { 9999 };
		// Distance (cells) from the target's map cell at which a vanished spy counts as gone in.
		int DoorDistance { 3 };
		int StartFrame { 0 };
		// House whose colors the spy wore on its last tick in sight (-1: none of an enemy).
		int DisguiseOwner { -1 };
		CellStruct Waypoint { CellStruct::Empty };
		int WaypointFrame { -1000 };
	};

	// One shot of the disguise kit is enough: a spy fires again only for the colors of another
	// house (its target changed) or after losing the ones it had, or when the shot never landed.
	struct NeuralSpyKitShot
	{
		int House { -1 };
		int Frame { 0 };
		bool Landed { false };
	};

	// A demolition truck or a terrorist on its way to blow up a target without a transport.
	struct NeuralSneakStrike
	{
		DWORD Target { 0 };
		int StartFrame { 0 };
		bool Charging { false };
		bool WaitingForRide { false };
		CellStruct LastCell { CellStruct::Empty };
		int LastMoveFrame { 0 };
		DWORD SkipTarget { 0 };
		int SkipUntilFrame { 0 };
	};

	// A barracks built next to a tech so that engineers walk to it (UpdateCaptureBarracks): where it stood,
	// its land and the tech it was built for. Kept after its loss: an engineer it trained may still
	// capture.
	struct NeuralCaptureBarracks
	{
		DWORD ID { 0 };
		CellStruct Cell { CellStruct::Empty };
		int Zone { -1 };
		CellStruct TechCell { CellStruct::Empty };
	};

	// A land where placement found no cell for a capture barracks: how often, and until when no site
	// there asks for one.
	struct NeuralCaptureNoRoom
	{
		int Zone { -1 };
		int Count { 0 };
		int UntilFrame { -1 };
	};

	struct NeuralSpecOpsState
	{
		int MatchSerial { -1 };
		int LastScanFrame { -100000 };
		// Sorted by ID (binary search, deterministic order).
		std::vector<NeuralKnownTech> Techs {};
		// The capturable buildings the map starts with were taken into the memory.
		bool MapTechsKnown { false };
		std::vector<NeuralSpecOpsCapture> Captures {};
		std::vector<NeuralSpecOpsSpy> Spies {};
		// Opportunity summary of the last scan: the engineer type (Engineer=yes: bridges and captures)
		// and the type that captures, an engineer or else one that captures by its weapon.
		InfantryTypeClass* EngineerType { nullptr };
		InfantryTypeClass* CaptureType { nullptr };
		InfantryTypeClass* SpyType { nullptr };
		bool SpyAvailable { false };
		int KnownNeutralTechs { 0 };
		int KnownNeutralCash { 0 };
		int SafeTechValue { 0 };
		int ContestedTechValue { 0 };
		int SpyTargets { 0 };
		bool BridgeRepairTask { false };
		bool BridgeDemolitionTask { false };
		int Opportunity { 0 };
		// Results this match (logs and tests).
		int CaptureCount { 0 };
		int InfiltrationCount { 0 };
		// Captures of each engineer by its weapon, which outlives them: UniqueID -> count (key access only).
		std::unordered_map<DWORD, int> WeaponCaptures {};
		// Frames of the last spies killed on their way (oldest first, 4 at most).
		std::vector<int> SpyLossFrames {};
		// Spy UniqueID -> its last disguise kit shot (key access only).
		std::unordered_map<DWORD, NeuralSpyKitShot> KitShots {};
		// Suicide raiders sneaking to a target on foot or on their own wheels: UniqueID -> strike
		// (key access only; orders go out in UnitClass / InfantryClass array order).
		std::unordered_map<DWORD, NeuralSneakStrike> SneakStrikes {};
		int LastSneakFrame { -1 };
		// Capture barracks (UpdateCaptureBarracks): the tech the next barracks goes next to, on a land
		// with buildings of ours and no barracks of ours (ID 0: none), and whether it is urgent.
		DWORD CaptureSiteTechID { 0 };
		CellStruct CaptureSiteCell { CellStruct::Empty };
		int CaptureSiteZone { -1 };
		bool CaptureSiteUrgent { false };
		std::vector<NeuralCaptureNoRoom> CaptureNoRoom {};
		// A barracks was placed for a site on this land (-1: none): the first barracks of ours standing
		// there is that site's capture barracks.
		int CapturePlacedZone { -1 };
		CellStruct CapturePlacedTechCell { CellStruct::Empty };
		int CapturePlacedFrame { -1 };
		std::vector<NeuralCaptureBarracks> CaptureBarracks {};
		// The barracks of ours the next engineer is trained at (Empty: the usual choice), and its tech.
		CellStruct CaptureEngineerCell { CellStruct::Empty };
		DWORD CaptureEngineerTechID { 0 };
		// The last engineer in training logged as trained there.
		DWORD CaptureRoutedObjectID { 0 };
		// Results this match (logs): capture barracks standing, engineers trained for the land of
		// their tech, captures on the land of a capture barracks.
		int CaptureBarracksBuilt { 0 };
		int CaptureEngineersRouted { 0 };
		int CaptureBarracksCaptures { 0 };
		int CaptureLogLines { 0 };
	};

	std::array<NeuralSpecOpsState, 32> SpecOpsStates {};

	NeuralSpecOpsState& GetSpecOpsState(const HouseClass* pHouse)
	{
		auto& state = SpecOpsStates[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (state.MatchSerial != NeuralMatchSerial)
		{
			state = NeuralSpecOpsState {};
			state.MatchSerial = NeuralMatchSerial;
		}

		return state;
	}

	NeuralKnownTech* FindKnownTech(NeuralSpecOpsState& state, DWORD id)
	{
		auto const it = std::lower_bound(state.Techs.begin(), state.Techs.end(), id,
			[](const NeuralKnownTech& tech, DWORD key) { return tech.ID < key; });

		return (it != state.Techs.end() && it->ID == id) ? &*it : nullptr;
	}

	// What owning this tech building is worth, from its rules only: income first, then what it
	// produces, heals or reveals.
	int SpecOpsTechValue(const BuildingTypeClass* pType)
	{
		if (!pType)
			return 0;

		int value = 1;

		if (pType->ProduceCashAmount > 0 || pType->ProduceCashStartup > 0)
			value += 3;

		if (pType->Factory != AbstractType::None)
			value += 2;

		if (pType->Hospital || pType->Armory || pType->UnitRepair || pType->Radar || pType->SpySat)
			value += 2;

		if (pType->PowerBonus > pType->PowerDrain)
			value += 1;

		if (pType->Cost >= 1000)
			value += 1;

		return std::min(value, 6);
	}

	// Can a ground unit standing in this movement zone (Normal) walk up to the building? The cells
	// under a building often carry another zone than the ground around it (map tech placed at map
	// start), so the zone of the building's own cell says nothing; the cells around it do.
	bool IsBuildingEnterableFromZone(const BuildingClass* pBld, int zone, MovementZone movementZone)
	{
		if (!pBld || !pBld->Type || zone < 0)
			return false;

		const CellStruct cell = pBld->GetMapCoords();
		const int width = pBld->Type->GetFoundationWidth();
		const int height = pBld->Type->GetFoundationHeight(false);

		for (int dy = -2; dy <= height + 1; ++dy)
		{
			for (int dx = -2; dx <= width + 1; ++dx)
			{
				if (dx >= 0 && dx < width && dy >= 0 && dy < height)
					continue;

				const CellStruct around { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };

				if (MapClass::Instance.CoordinatesLegal(around)
					&& MapClass::Instance.GetMovementZoneType(around, movementZone, false) == zone)
				{
					return true;
				}
			}
		}

		return false;
	}

	// The ground a building stands on: the zone of its cell, or, when that cell is on no ground (the
	// cells of a building may carry another zone), the first ground zone around its foundation. -1
	// with no ground around it.
	int BuildingGroundZone(const BuildingClass* pBld)
	{
		if (!pBld || !pBld->Type)
			return -1;

		const CellStruct cell = pBld->GetMapCoords();
		const int ownZone = MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, pBld->OnBridge);

		if (IsGroundZone(ownZone))
			return ownZone;

		const int width = pBld->Type->GetFoundationWidth();
		const int height = pBld->Type->GetFoundationHeight(false);

		for (int dy = -1; dy <= height; ++dy)
		{
			for (int dx = -1; dx <= width; ++dx)
			{
				if (dx >= 0 && dx < width && dy >= 0 && dy < height)
					continue;

				const CellStruct around { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };

				if (!MapClass::Instance.CoordinatesLegal(around))
					continue;

				const int zone = MapClass::Instance.GetMovementZoneType(around, MovementZone::Normal, false);

				if (IsGroundZone(zone))
					return zone;
			}
		}

		return -1;
	}

	// The wall overlay standing on this cell, or nullptr.
	OverlayTypeClass* GetWallOverlayAt(const CellStruct& cell)
	{
		auto const pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell || pCell->OverlayTypeIndex < 0 || pCell->OverlayTypeIndex >= OverlayTypeClass::Array.Count)
			return nullptr;

		auto const pOverlay = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex);

		return (pOverlay && pOverlay->Wall) ? pOverlay : nullptr;
	}

	// The free ground a unit from another land is set down on to reach a building: the nearest cell
	// around the foundation, within three cells, on ground and free of walls and buildings (fixed
	// order). The zone of the building's own cells is no such ground when a wall ring closes them off:
	// the zone map then has a pocket under the building alone. -1 with none.
	int BuildingApproachGround(const BuildingClass* pBld, CellStruct& outCell)
	{
		outCell = CellStruct::Empty;

		if (!pBld || !pBld->Type)
			return -1;

		const CellStruct origin = pBld->GetMapCoords();
		const int width = pBld->Type->GetFoundationWidth();
		const int height = pBld->Type->GetFoundationHeight(false);

		for (int ring = 1; ring <= 3; ++ring)
		{
			for (int dy = -ring; dy < height + ring; ++dy)
			{
				for (int dx = -ring; dx < width + ring; ++dx)
				{
					if (dx > -ring && dx < width + ring - 1 && dy > -ring && dy < height + ring - 1)
						continue;

					const CellStruct around { static_cast<short>(origin.X + dx), static_cast<short>(origin.Y + dy) };
					auto const pCell = MapClass::Instance.CoordinatesLegal(around) ? MapClass::Instance.TryGetCellAt(around) : nullptr;

					if (!pCell || pCell->GetBuilding() || GetWallOverlayAt(around))
						continue;

					const int zone = MapClass::Instance.GetMovementZoneType(around, MovementZone::Normal, false);

					if (IsGroundZone(zone))
					{
						outCell = around;
						return zone;
					}
				}
			}
		}

		return -1;
	}

	// A breach into a walled-in tech: the wall piece to knock down, the free cell of our ground
	// next to it and that ground's movement zone (Normal; GetTechWallStateFrom fills it).
	struct NeuralTechBreach
	{
		CellStruct Wall { CellStruct::Empty };
		CellStruct Approach { CellStruct::Empty };
		int Zone { -1 };
	};

	enum class NeuralTechWall
	{
		Open,
		Breachable,
		Sealed
	};

	// Is this tech walled in for a ground unit in this movement zone (Normal)? Walled in: no free
	// cell next to the foundation is in the zone, a wall stands within two cells of it and free
	// ground of the zone lies just beyond. The wider ring of IsBuildingEnterableFromZone reads such
	// a building as open (map tech is often fenced in on the cells right around it), and the
	// engineer then stands at the fence until its op times out. Breachable: a single wall piece
	// stands orthogonally between our ground and the inside (a diagonal gap may not let a soldier
	// through); outBreach gets the one nearest to `from`. Sealed: thicker walls, or only pieces
	// that are Immune or belong to an ally.
	NeuralTechWall GetTechWallState(HouseClass* pHouse, const BuildingClass* pBld, int zone, const CellStruct& from,
		NeuralTechBreach& outBreach)
	{
		outBreach = NeuralTechBreach {};

		if (!pBld || !pBld->Type || zone <= 1)
			return NeuralTechWall::Open;

		const CellStruct origin = pBld->GetMapCoords();
		const int width = pBld->Type->GetFoundationWidth();
		const int height = pBld->Type->GetFoundationHeight(false);

		// Cells out from the foundation along each axis (0 when within its columns or rows).
		auto axisRings = [&](const CellStruct& cell, int& outX, int& outY)
		{
			const int dx = cell.X - origin.X;
			const int dy = cell.Y - origin.Y;
			outX = dx < 0 ? -dx : std::max(0, dx - width + 1);
			outY = dy < 0 ? -dy : std::max(0, dy - height + 1);
		};

		auto ringOf = [&](const CellStruct& cell)
		{
			int rx = 0;
			int ry = 0;
			axisRings(cell, rx, ry);

			return std::max(rx, ry);
		};

		auto zoneOf = [](const CellStruct& cell)
		{
			return MapClass::Instance.GetMovementZoneType(cell, MovementZone::Normal, false);
		};

		bool wallNear = false;
		bool groundBeyond = false;

		for (int dy = -3; dy <= height + 2; ++dy)
		{
			for (int dx = -3; dx <= width + 2; ++dx)
			{
				const CellStruct cell { static_cast<short>(origin.X + dx), static_cast<short>(origin.Y + dy) };
				const int ring = ringOf(cell);

				if (ring == 0 || !MapClass::Instance.CoordinatesLegal(cell))
					continue;

				if (GetWallOverlayAt(cell))
				{
					wallNear = wallNear || ring <= 2;

					continue;
				}

				if (zoneOf(cell) != zone)
					continue;

				if (ring == 1)
					return NeuralTechWall::Open;

				groundBeyond = true;
			}
		}

		if (!wallNear || !groundBeyond)
			return NeuralTechWall::Open;

		static constexpr std::array<std::pair<short, short>, 4> Sides { { { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 } } };
		int bestDistSq = INT_MAX;

		for (int dy = -2; dy <= height + 1; ++dy)
		{
			for (int dx = -2; dx <= width + 1; ++dx)
			{
				const CellStruct cell { static_cast<short>(origin.X + dx), static_cast<short>(origin.Y + dy) };
				int rx = 0;
				int ry = 0;
				axisRings(cell, rx, ry);

				if (rx == 0 && ry == 0)
					continue;

				auto const pWall = GetWallOverlayAt(cell);

				if (!pWall || pWall->Immune)
					continue;

				auto const pCell = MapClass::Instance.TryGetCellAt(cell);
				auto const pWallOwner = (pCell->WallOwnerIndex >= 0 && pCell->WallOwnerIndex < HouseClass::Array.Count)
					? HouseClass::Array.GetItem(pCell->WallOwnerIndex) : nullptr;

				if (pWallOwner && pWallOwner != pHouse && pHouse->IsAlliedWith(pWallOwner))
					continue;

				// A piece right beside the foundation (not on its corner) opens the door by itself;
				// one further out needs free inside ground behind it.
				bool inside = std::max(rx, ry) == 1 && std::min(rx, ry) == 0;
				CellStruct approach = CellStruct::Empty;

				for (auto const& [sx, sy] : Sides)
				{
					const CellStruct side { static_cast<short>(cell.X + sx), static_cast<short>(cell.Y + sy) };

					if (!MapClass::Instance.CoordinatesLegal(side) || ringOf(side) == 0 || GetWallOverlayAt(side))
						continue;

					const int sideZone = zoneOf(side);

					if (sideZone == zone)
					{
						if (approach == CellStruct::Empty)
							approach = side;
					}
					else if (sideZone > 1 && ringOf(side) == 1)
					{
						inside = true;
					}
				}

				if (!inside || approach == CellStruct::Empty)
					continue;

				const int distSq = (cell.X - from.X) * (cell.X - from.X) + (cell.Y - from.Y) * (cell.Y - from.Y);

				if (distSq < bestDistSq)
				{
					bestDistSq = distSq;
					outBreach.Wall = cell;
					outBreach.Approach = approach;
				}
			}
		}

		return bestDistSq < INT_MAX ? NeuralTechWall::Breachable : NeuralTechWall::Sealed;
	}

	// GetTechWallState for a unit standing in zone, judged from the ground the unit would enter
	// from. A tech on that ground is judged from it. A tech on another land reads open from here
	// (no wall stands between it and our ground), so it is judged from the free ground around it,
	// the land a transport sets the unit down on: open from any of those lands is open. In match
	// #21545 an engineer was ferried to a lab ringed by sandbags that read open from home.
	// outBreach.Zone: the ground the way in is judged from.
	NeuralTechWall GetTechWallStateFrom(HouseClass* pHouse, const BuildingClass* pBld, int zone, const CellStruct& from,
		NeuralTechBreach& outBreach)
	{
		const NeuralTechWall own = GetTechWallState(pHouse, pBld, zone, from, outBreach);
		outBreach.Zone = zone;

		if (own != NeuralTechWall::Open || !pBld || !pBld->Type || IsBuildingEnterableFromZone(pBld, zone))
			return own;

		const CellStruct origin = pBld->GetMapCoords();
		const int width = pBld->Type->GetFoundationWidth();
		const int height = pBld->Type->GetFoundationHeight(false);
		std::array<int, 4> lands { -1, -1, -1, -1 };
		size_t landCount = 0;
		NeuralTechWall result = NeuralTechWall::Open;

		for (int ring = 1; ring <= 3 && landCount < lands.size(); ++ring)
		{
			for (int dy = -ring; dy < height + ring && landCount < lands.size(); ++dy)
			{
				for (int dx = -ring; dx < width + ring && landCount < lands.size(); ++dx)
				{
					if (dx > -ring && dx < width + ring - 1 && dy > -ring && dy < height + ring - 1)
						continue;

					const CellStruct around { static_cast<short>(origin.X + dx), static_cast<short>(origin.Y + dy) };
					auto const pCell = MapClass::Instance.CoordinatesLegal(around) ? MapClass::Instance.TryGetCellAt(around) : nullptr;

					if (!pCell || pCell->GetBuilding() || GetWallOverlayAt(around))
						continue;

					const int land = MapClass::Instance.GetMovementZoneType(around, MovementZone::Normal, false);

					if (!IsGroundZone(land) || std::find(lands.begin(), lands.begin() + landCount, land) != lands.begin() + landCount)
						continue;

					lands[landCount++] = land;
					NeuralTechBreach breach;
					const NeuralTechWall wall = GetTechWallState(pHouse, pBld, land, from, breach);

					if (wall == NeuralTechWall::Open)
					{
						outBreach = NeuralTechBreach {};
						outBreach.Zone = land;

						return NeuralTechWall::Open;
					}

					if (wall == NeuralTechWall::Breachable && result != NeuralTechWall::Breachable)
					{
						result = NeuralTechWall::Breachable;
						outBreach = breach;
						outBreach.Zone = land;
					}
					else if (wall == NeuralTechWall::Sealed && result == NeuralTechWall::Open)
					{
						result = NeuralTechWall::Sealed;
					}
				}
			}
		}

		return result;
	}

	// Can this vehicle crush the wall piece by driving over it? Crusher= against a Crushable= wall.
	bool CanCrushWall(FootClass* pFoot, const OverlayTypeClass* pWall)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;

		return pWall && pWall->Crushable && pType && pType->Crusher && pFoot->WhatAmI() == AbstractType::Unit;
	}

	// Can this unit knock the wall piece down by its fire? Its primary weapon must hit the ground
	// with a warhead that hurts walls as the engine rules it: Wall=, or Wood= on a wood wall.
	bool CanShootWall(FootClass* pFoot, const OverlayTypeClass* pWall)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;
		auto const pSlot = pType ? pType->GetWeapon(0) : nullptr;
		auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

		if (!pWall || !pWeapon || !pWeapon->Warhead || !pWeapon->Projectile || !pWeapon->Projectile->AG || pWeapon->Damage <= 0)
			return false;

		auto const pWarhead = pWeapon->Warhead;

		return pWarhead->Wall || pWarhead->WallAbsoluteDestroyer || (pWarhead->Wood && pWall->Armor == Armor::Wood);
	}

	// Can this unit knock the wall piece down: by its fire, or a vehicle by crushing it.
	bool CanBreachWall(FootClass* pFoot, const OverlayTypeClass* pWall)
	{
		return CanShootWall(pFoot, pWall) || CanCrushWall(pFoot, pWall);
	}

	// Breach lines are capped per match so a long training match keeps them readable.
	bool CanLogSpecOpsBreach()
	{
		static int logSerial = -1;
		static int logCount = 0;

		if (logSerial != NeuralMatchSerial)
		{
			logSerial = NeuralMatchSerial;
			logCount = 0;
		}

		return logCount++ < 30;
	}

	// Has this house seen the building as neutral the last time it was in sight?
	bool IsKnownNeutralTech(HouseClass* pHouse, const BuildingClass* pBld)
	{
		if (!pHouse || !pBld)
			return false;

		auto& state = GetSpecOpsState(pHouse);
		auto const pTech = FindKnownTech(state, pBld->UniqueID);

		return pTech && pTech->OwnerIndex < 0;
	}

	// Has this house seen the tech, the last time it was in sight, neutral or in the hands of a house it
	// is not allied with? Those are the techs Expert 4 goes for (PickSpecOpsCaptureTarget).
	bool IsKnownCapturableTech(HouseClass* pHouse, const BuildingClass* pBld)
	{
		if (!pHouse || !pBld)
			return false;

		auto const pTech = FindKnownTech(GetSpecOpsState(pHouse), pBld->UniqueID);

		if (!pTech || pTech->OwnerIndex == pHouse->ArrayIndex || pTech->OwnerIndex >= HouseClass::Array.Count)
			return false;

		if (pTech->OwnerIndex < 0)
			return true;

		auto const pOwner = HouseClass::Array.GetItem(pTech->OwnerIndex);

		return pOwner && !pHouse->IsAlliedWith(pOwner);
	}

	// An enemy building nothing guards: out of the known reach of enemy defenses and no armed enemy
	// seen near it. A lone engineer takes such a one; at a guarded one it dies on the way.
	bool IsEnemyBuildingUnguarded(HouseClass* pHouse, const BuildingClass* pBld)
	{
		const CellStruct cell = pBld->GetMapCoords();

		return !IsCellInKnownEnemyDefenseReach(pHouse, cell, CaptureUnguardedDefenseMargin)
			&& VisibleEnemyThreatNear(pHouse, cell, CaptureUnguardedThreatCells) <= 0;
	}

	// An enemy's expansion: a paying tech, a refinery or a building to build around (BaseNormal) with
	// no other known building of its owner within ExpansionDenialBaseCells. Taking it denies its owner
	// that field (user, 2026-11-20: on island maps an enemy's derricks are its expansion fields, and
	// taking them corners it).
	bool IsKnownEnemyExpansion(HouseClass* pHouse, const BuildingClass* pBld)
	{
		auto const pType = pBld->Type;

		if (!pBld->Owner || pBld->Owner->IsNeutral()
			|| !(pType->ProduceCashAmount > 0 || pType->ProduceCashStartup > 0 || pType->Refinery || BuildingTypeExt::IsAIBaseNormal(pType)))
		{
			return false;
		}

		const CellStruct cell = pBld->GetMapCoords();

		for (auto const pOther : pBld->Owner->Buildings)
		{
			if (pOther && pOther != pBld && pOther->IsAlive && !pOther->InLimbo && pOther->Type && !pOther->Type->Insignificant
				&& pOther->GetMapCoords().DistanceFrom(cell) <= ExpansionDenialBaseCells && IsEnemyBuildingKnown(pHouse, pOther))
			{
				return false;
			}
		}

		return true;
	}

	// TEMP diagnostic (captures of enemy buildings, user 2026-11-20): every CaptureLedgerFrames, each
	// capturable building that changed hands since the last look, with its old and new owner (-1:
	// neutral), CaptureLedgerLogLines a match. Read only.
	void NoteCaptureLedger()
	{
		static int serial = -1;
		static int lastFrame = -1;
		static int logCount = 0;
		static std::unordered_map<DWORD, int> owners;
		const int now = Unsorted::CurrentFrame;

		if (serial != NeuralMatchSerial)
		{
			serial = NeuralMatchSerial;
			lastFrame = -1;
			logCount = 0;
			owners.clear();
		}

		if (lastFrame >= 0 && now >= lastFrame && now - lastFrame < CaptureLedgerFrames)
			return;

		lastFrame = now;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Type->Capturable || !pBld->Owner)
				continue;

			const int owner = pBld->Owner->IsNeutral() ? -1 : pBld->Owner->ArrayIndex;
			auto const [it, inserted] = owners.try_emplace(pBld->UniqueID, owner);

			if (inserted || it->second == owner)
				continue;

			if (logCount++ < CaptureLedgerLogLines)
			{
				Debug::Log("[NeuralAI] capture ledger (TEMP) frame %d: %s %u at (%d,%d) from house %d to house %d.\n", now, pBld->Type->ID,
					pBld->UniqueID, pBld->GetMapCoords().X, pBld->GetMapCoords().Y, it->second, owner);
			}

			it->second = owner;
		}
	}

	// Coalition (NeuralAI.Urban.inl): an allied engineer walks into it or an allied Neural AI
	// has picked it, so we leave it to them.
	bool IsTechClaimedByAlly(HouseClass* pHouse, DWORD buildingID);

	bool IsSpecOpsCaptureTarget(HouseClass* pHouse, DWORD targetID, DWORD exceptEngineer)
	{
		for (auto const& capture : GetSpecOpsState(pHouse).Captures)
		{
			if (capture.TargetID == targetID && capture.EngineerID != exceptEngineer)
				return true;
		}

		return false;
	}

	// Whether an engineer walking to its claimed building has not come EngineerClaimStepCells closer to it
	// for EngineerClaimStallFrames (EngineerClaimStall*). A claim made after its last step starts again.
	bool IsEngineerClaimStalled(HouseClass* pHouse, const InfantryClass* pEngineer, const BuildingClass* pBld, int claimFrame)
	{
		const int now = Unsorted::CurrentFrame;
		const int distance = static_cast<int>(pEngineer->GetMapCoords().DistanceFrom(pBld->GetMapCoords()));
		auto& progress = GetMacroState(pHouse).EngineerClaimProgress;
		auto const it = progress.find(pEngineer->UniqueID);

		if (it == progress.end() || claimFrame > it->second.second || distance <= it->second.first - EngineerClaimStepCells)
		{
			if (progress.size() > 64)
				std::erase_if(progress, [now](const auto& entry) { return now - entry.second.second > 4 * EngineerClaimStallFrames; });

			progress[pEngineer->UniqueID] = { distance, now };
			return false;
		}

		return now - it->second.second > EngineerClaimStallFrames;
	}

	// Engineer claims (EngineerClaim*): does an engineer of ours other than engineerUid go for this
	// building? In a transport operation, walking to it or attacking it, or sent at it within
	// EngineerClaimFrames. With aheadOnly only the ones ahead of engineerUid count: walking or attacking
	// ones, ones not in a transport operation, ones aboard, and ones waiting with a lower UniqueID.
	bool IsCaptureClaimedByOther(HouseClass* pHouse, const BuildingClass* pBld, DWORD engineerUid, bool aheadOnly)
	{
		if (!pHouse || !pBld)
			return false;

		const int now = Unsorted::CurrentFrame;

		for (auto const& [uid, claim] : GetMacroState(pHouse).EngineerCaptureTargets)
		{
			if (uid == engineerUid || claim.first != pBld->UniqueID)
				continue;

			auto const pEngineer = abstract_cast<InfantryClass*>(FindOwnFootByID(pHouse, uid));

			if (!pEngineer)
				continue;

			const bool inOp = IsInTransportOp(pHouse, uid);
			const bool onIt = !pEngineer->InLimbo && (pEngineer->Destination == pBld || pEngineer->Target == pBld);

			if (!inOp && !onIt && now - claim.second > EngineerClaimFrames)
				continue;

			if (!inOp && onIt && IsEngineerClaimStalled(pHouse, pEngineer, pBld, claim.second))
				continue;

			if (!aheadOnly || onIt || !inOp || pEngineer->InLimbo || uid < engineerUid)
				return true;
		}

		return false;
	}

	// The claim of an engineer of ours (EngineerCaptureTargets): -1 none; 1 its building still stands,
	// is capturable, no house's of ours or an ally's, and no engineer ahead of it goes for it; 0 not.
	int EngineerClaimWanted(HouseClass* pHouse, DWORD engineerUid)
	{
		auto const& claims = GetMacroState(pHouse).EngineerCaptureTargets;
		auto const it = claims.find(engineerUid);

		if (it == claims.end())
			return -1;

		BuildingClass* pBld = nullptr;

		for (auto const pCandidate : BuildingClass::Array)
		{
			if (pCandidate && pCandidate->UniqueID == it->second.first)
			{
				pBld = pCandidate;
				break;
			}
		}

		const bool wanted = pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->Capturable && pBld->Owner
			&& pBld->Owner != pHouse && !pHouse->IsAlliedWith(pBld->Owner) && !IsCaptureClaimedByOther(pHouse, pBld, engineerUid, true);

		return wanted ? 1 : 0;
	}

	// May any engineer of ours walk to this neutral building now? Known as neutral, not walled in
	// (an engineer alone would wait at the wall), not blocked by recent failures, not taken by an
	// Expert 4 capture nor claimed by an ally, and out of known enemy defense reach.
	bool IsSpecOpsTechAvailable(HouseClass* pHouse, const BuildingClass* pBld, DWORD engineerID)
	{
		if (!pHouse || !pBld)
			return false;

		auto& state = GetSpecOpsState(pHouse);
		auto const pTech = FindKnownTech(state, pBld->UniqueID);

		return pTech && pTech->OwnerIndex < 0 && !pTech->Walled && Unsorted::CurrentFrame >= pTech->BlockedUntilFrame
			&& !IsSpecOpsCaptureTarget(pHouse, pTech->ID, engineerID) && !IsTechClaimedByAlly(pHouse, pTech->ID)
			&& !IsCellInKnownEnemyDefenseReach(pHouse, pTech->Cell, 2);
	}

	// Was this tech walled in at the last scan (GetTechWallStateFrom, from the land an engineer gets
	// in from)? Those are the SpecOps breach's: an engineer alone would wait at the wall.
	bool IsKnownTechWalled(HouseClass* pHouse, const BuildingClass* pBld)
	{
		auto const pTech = (pHouse && pBld) ? FindKnownTech(GetSpecOpsState(pHouse), pBld->UniqueID) : nullptr;

		return pTech && pTech->Walled;
	}

	bool CanEngineerReachTech(HouseClass* pHouse, const NeuralKnownTech& tech, int engineerZone, const InfantryTypeClass* pEngineerType)
	{
		for (const int zone : tech.Zones)
		{
			if (zone >= 0 && zone == engineerZone)
				return true;
		}

		return pEngineerType && CanHouseFerryTypeAcrossWater(pHouse, pEngineerType);
	}

	// Refresh the tech memory from what this house knows now and log captures. A tech out of its
	// knowledge (shroud, enemy gap, fog) keeps the owner it had when last seen.
	void UpdateSpecOpsTechMemory(HouseClass* pHouse, NeuralSpecOpsState& state)
	{
		const int now = Unsorted::CurrentFrame;
		NoteCaptureLedger();
		std::vector<DWORD> knownNowIDs;

		// The capturable buildings a map starts with are map knowledge, like its ore fields: a house
		// knows where they stand from the first frames, neutral as the map has them. What became of
		// each one since (captured, destroyed) it learns only by seeing it.
		const bool mapKnowledge = !state.MapTechsKnown && now <= MapKnowledgeFrames;
		state.MapTechsKnown = true;

		// Insignificant is no filter here: many maps' oil derricks are Insignificant tech buildings.
		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || !pBld->Type->Capturable
				|| pBld->Type->InvisibleInGame || pBld->Type->BridgeRepairHut)
			{
				continue;
			}

			const CellStruct cell = pBld->GetMapCoords();

			const bool knownNow = pBld->Owner == pHouse || IsCellKnownToAI(pHouse, cell);

			if (!knownNow && !(mapKnowledge && pBld->Owner->IsNeutral()))
				continue;

			if (knownNow)
				knownNowIDs.push_back(pBld->UniqueID);
			const int ownerIndex = pBld->Owner->IsNeutral() ? -1 : pBld->Owner->ArrayIndex;
			NeuralKnownTech* pTech = FindKnownTech(state, pBld->UniqueID);

			if (!pTech)
			{
				// Only buildings first seen neutral are tech; capturable enemy structures are the
				// engineer assault plan's business.
				if (ownerIndex >= 0)
					continue;

				NeuralKnownTech tech;
				tech.ID = pBld->UniqueID;
				tech.Cell = cell;
				tech.Value = SpecOpsTechValue(pBld->Type);
				tech.Cash = pBld->Type->ProduceCashAmount > 0 || pBld->Type->ProduceCashStartup > 0;

				const int width = pBld->Type->GetFoundationWidth();
				const int height = pBld->Type->GetFoundationHeight(false);
				size_t zoneCount = 0;

				for (int dy = -2; dy <= height + 1 && zoneCount < tech.Zones.size(); ++dy)
				{
					for (int dx = -2; dx <= width + 1 && zoneCount < tech.Zones.size(); ++dx)
					{
						if (dx >= 0 && dx < width && dy >= 0 && dy < height)
							continue;

						const CellStruct around { static_cast<short>(cell.X + dx), static_cast<short>(cell.Y + dy) };

						if (!MapClass::Instance.CoordinatesLegal(around))
							continue;

						const int zone = MapClass::Instance.GetMovementZoneType(around, MovementZone::Normal, false);

						if (zone <= 1 || std::find(tech.Zones.begin(), tech.Zones.begin() + zoneCount, zone) != tech.Zones.begin() + zoneCount)
							continue;

						tech.Zones[zoneCount++] = zone;
					}
				}

				auto const it = std::lower_bound(state.Techs.begin(), state.Techs.end(), tech.ID,
					[](const NeuralKnownTech& known, DWORD key) { return known.ID < key; });
				pTech = &*state.Techs.insert(it, tech);
			}

			if (pTech->OwnerIndex != ownerIndex)
			{
				if (ownerIndex == pHouse->ArrayIndex)
				{
					state.CaptureCount++;
					Debug::Log("[NeuralAI] House %d SpecOps: captured %s at (%d,%d), frame %d (captures %d).\n",
						pHouse->ArrayIndex, pBld->Type->ID, cell.X, cell.Y, now, state.CaptureCount);

					// An engineer by its weapon outlives its capture: it lets go of the building, now ours, so
					// that it is idle for the next capture (a soldier keeps a target it may no longer fire at).
					for (auto const pInf : InfantryClass::Array)
					{
						if (!pInf || pInf->Owner != pHouse || !pInf->IsAlive || pInf->Target != pBld || !CapturesByWeapon(pInf->Type))
							continue;

						const int taken = ++state.WeaponCaptures[pInf->UniqueID];
						pInf->SetTarget(nullptr);
						pInf->QueueMission(Mission::Guard, true);
						Debug::Log("[NeuralAI] House %d SpecOps: %s %u took %s at (%d,%d) by its weapon, its capture %d, frame %d.\n",
							pHouse->ArrayIndex, pInf->Type->ID, pInf->UniqueID, pBld->Type->ID, cell.X, cell.Y, taken, now);
					}

					// A capture on the land of a barracks built for its techs (UpdateCaptureBarracks), standing
					// or lost since.
					for (auto const& entry : state.CaptureBarracks)
					{
						if (std::find(pTech->Zones.begin(), pTech->Zones.end(), entry.Zone) == pTech->Zones.end())
							continue;

						auto const pBarracks = FindBuildingByID(entry.ID);
						const bool standing = pBarracks && pBarracks->IsAlive && pBarracks->Owner == pHouse;
						state.CaptureBarracksCaptures++;
						Debug::Log("[NeuralAI] House %d capture barracks: captured %s at (%d,%d) on land %d, the capture barracks at (%d,%d)%s %d cells away, "
							"frame %d (captures there %d).\n", pHouse->ArrayIndex, pBld->Type->ID, cell.X, cell.Y, entry.Zone, entry.Cell.X,
							entry.Cell.Y, standing ? "" : " (lost)", static_cast<int>(entry.Cell.DistanceFrom(cell)), now, state.CaptureBarracksCaptures);
						break;
					}

					// TEMP diagnostic: what a captured secret lab unlocks and whether the house may build it.
					if (pBld->Type->SecretLab)
					{
						auto const pBoon = pBld->SecretProduction;
						Debug::Log("[NeuralAI] House %d SecretLab DIAG: %s unlocks %s, prerequisites met %d, its tech level %d, ours %d, frame %d.\n",
							pHouse->ArrayIndex, pBld->Type->ID, pBoon ? pBoon->ID : "nothing",
							(pBoon && HouseExt::PrerequisitesMet(pHouse, pBoon)) ? 1 : 0, pBoon ? pBoon->TechLevel : 0, pHouse->TechLevel, now);
					}
				}

				pTech->OwnerIndex = ownerIndex;
			}

			pTech->SeenFrame = now;
		}

		// A remembered tech whose cell is known but that is gone was destroyed.
		std::sort(knownNowIDs.begin(), knownNowIDs.end());
		state.Techs.erase(std::remove_if(state.Techs.begin(), state.Techs.end(),
			[&](const NeuralKnownTech& tech)
			{
				return IsCellKnownToAI(pHouse, tech.Cell) && !std::binary_search(knownNowIDs.begin(), knownNowIDs.end(), tech.ID);
			}), state.Techs.end());
	}

	int CountKnownDisguiseDetectorBuildings(HouseClass* pHouse, const CellStruct& cell, int marginCells);
	int CountKnownSpyCounterBuildings(HouseClass* pHouse, const InfantryTypeClass* pSpyType, const CellStruct& cell, int marginCells);

	// Enemy buildings this house knows and would gain something from spying now, filtered only by
	// static counter buildings that actually detect this house's spy type (DetectDisguise for
	// disguised spies, SensorArray / Sensors for cloaked Nod spies).
	int CountSpecOpsSpyTargets(HouseClass* pHouse, const InfantryTypeClass* pSpyType)
	{
		int targets = 0;
		auto const& knownEntries = GetKnownEnemyBuildingCache(pHouse).Entries;

		for (auto const& info : knownEntries)
		{
			auto const pType = info.Building->Type;

			if (!pType->Spyable || CountKnownSpyCounterBuildings(pHouse, pSpyType, info.Building->GetMapCoords(), 1) > 0)
				continue;

			auto const pOwner = info.Building->Owner;
			const int eSide = pOwner ? pOwner->SideIndex : -1;
			const bool needStolenTech = pType->TechLevel >= 6
				&& ((eSide == 0 && !pHouse->Side0TechInfiltrated)
					|| (eSide == 1 && !pHouse->Side1TechInfiltrated)
					|| (eSide == 2 && !pHouse->Side2TechInfiltrated));
			bool blackoutWorth = false;

			if (pType->PowerBonus > pType->PowerDrain && pOwner && pOwner->PowerBlackoutTimer.GetTimeLeft() <= 150)
			{
				for (auto const& defInfo : knownEntries)
				{
					if (defInfo.IsDefense && defInfo.Building->Owner == pOwner
						&& (defInfo.Building->Type->Powered || defInfo.Building->Type->PoweredSpecial))
					{
						blackoutWorth = true;
						break;
					}
				}
			}

			auto const pSpyBldExt = BuildingTypeExt::Fetch(pType);
			const bool worth = (pSpyBldExt && pSpyBldExt->SpyEffect_AIWorth >= 0) ? pSpyBldExt->SpyEffect_AIWorth >= 100
				: (pType->Factory == AbstractType::UnitType && !pType->Naval && !pHouse->WarFactoryInfiltrated)
				|| (pType->Factory == AbstractType::InfantryType && !pHouse->BarracksInfiltrated)
				|| ((pType->Radar || pType->SpySat) && !pHouse->SpySatActive && !pOwner->RadarVisibleTo.Contains(pHouse->ArrayIndex))
				|| pType->SuperWeapon != -1
				|| needStolenTech
				|| blackoutWorth;

			if (worth && ++targets >= 3)
				break;
		}

		return targets;
	}

	// The enemy house whose colors a spy wears, or nullptr. A new spy shows the default disguise of
	// its own house, which fools nobody; copying a soldier takes that soldier's house.
	HouseClass* SpyDisguiseHouse(const InfantryClass* pSpy)
	{
		if (!pSpy || !pSpy->Disguised || !pSpy->Owner)
			return nullptr;

		auto const pAs = pSpy->DisguisedAsHouse;

		if (!pAs || pAs == pSpy->Owner || pSpy->Owner->IsAlliedWith(pAs))
			return nullptr;

		return pAs;
	}

	// A disguise only fools the copied house and its allies: in a free for all, the soldiers of a
	// third house still shoot a spy dressed as somebody else.
	bool IsSpyDisguisedFor(const InfantryClass* pSpy, HouseClass* pVictim)
	{
		auto const pAs = SpyDisguiseHouse(pSpy);

		return pAs && pVictim && (pAs == pVictim || pVictim->IsAlliedWith(pAs));
	}

	// A soldier of the victim's side (the victim or an ally of it, never ours) that we know of,
	// nearest to the spy first (UniqueID on ties): a player can only copy a soldier shown on the map.
	InfantryClass* FindSpyDisguiseSource(HouseClass* pHouse, InfantryClass* pSpy, HouseClass* pVictim)
	{
		InfantryClass* pBest = nullptr;
		int bestDist = INT_MAX;

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || pInf->InLimbo || !pInf->IsAlive || !pInf->Owner || !pInf->Type || pInf->Type->Doggie
				|| pInf->Type->Slaved || pInf->IsDisguised())
			{
				continue;
			}

			if ((pInf->Owner != pVictim && !pVictim->IsAlliedWith(pInf->Owner)) || pHouse->IsAlliedWith(pInf->Owner)
				|| pInf->Owner->IsNeutral() || !IsCellKnownToAI(pHouse, pInf->GetMapCoords()))
			{
				continue;
			}

			const int dist = pSpy->DistanceFrom(pInf);

			if (dist < bestDist || (dist == bestDist && pInf->UniqueID < pBest->UniqueID))
			{
				bestDist = dist;
				pBest = pInf;
			}
		}

		return pBest;
	}

	// The spy carries a disguise kit: a weapon whose warhead copies the target (MakesDisguise=).
	bool HasDisguiseKit(InfantryTypeClass* pType)
	{
		for (int i = 0; pType && i < 2; ++i)
		{
			auto const pSlot = pType->GetWeapon(i);
			auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

			if (pWeapon && pWeapon->Warhead && pWeapon->Warhead->MakesDisguise)
				return true;
		}

		return false;
	}

	// Notes that the last kit shot of a spy gave it the colors it went for.
	void UpdateSpyKitShot(HouseClass* pHouse, InfantryClass* pSpy)
	{
		auto& kitShots = GetSpecOpsState(pHouse).KitShots;
		auto const it = kitShots.find(pSpy->UniqueID);

		if (it == kitShots.end() || it->second.Landed || it->second.House < 0 || it->second.House >= HouseClass::Array.Count)
			return;

		if (IsSpyDisguisedFor(pSpy, HouseClass::Array.GetItem(it->second.House)))
			it->second.Landed = true;
	}

	// Copies a soldier's colors: at once when the engine allows a direct disguise, otherwise the
	// way a player does it, firing the disguise kit (a MakesDisguise= warhead, e.g. MakeupKit with
	// an endless range and FireOnce) at that soldier. One shot is enough; another one only for the
	// colors of another house, after the colors were lost (the spy was found out), or when the shot
	// did not land within 450 frames (the soldier died or walked off). True while the copy is done
	// or under way.
	bool CopySoldierDisguise(HouseClass* pHouse, InfantryClass* pSpy, InfantryClass* pSoldier)
	{
		if (!pSpy || !pSoldier || !pSoldier->Owner)
			return false;

		if (pSpy->CanDisguiseAs(pSoldier))
		{
			pSpy->DisguiseAs(pSoldier);
			return true;
		}

		if (pSpy->Target == pSoldier)
			return true;

		if (!HasDisguiseKit(pSpy->Type))
			return false;

		UpdateSpyKitShot(pHouse, pSpy);

		auto& shot = GetSpecOpsState(pHouse).KitShots[pSpy->UniqueID];
		const int want = pSoldier->Owner->ArrayIndex;
		const bool missed = !shot.Landed
			&& (Unsorted::CurrentFrame - shot.Frame > 450 || Unsorted::CurrentFrame < shot.Frame);

		if (shot.House == want && !shot.Landed && !missed)
			return false;

		const char* const reason = shot.House < 0 ? "first" : shot.House != want ? "new house"
			: shot.Landed ? "colors lost" : "missed";
		shot.House = want;
		shot.Frame = Unsorted::CurrentFrame;
		shot.Landed = false;
		pSpy->SetTarget(pSoldier);
		pSpy->QueueMission(Mission::Attack, false);
		Debug::Log("[NeuralAI] House %d spy %s %u fires its disguise kit at %s of house %d (%s shot), frame %d.\n",
			pHouse->ArrayIndex, pSpy->Type->ID, pSpy->UniqueID, pSoldier->Type->ID, want, reason, Unsorted::CurrentFrame);

		return true;
	}

	// Puts on the victim's colors as soon as one of its soldiers is known. True when the spy
	// fools the victim now or is copying one of its soldiers.
	bool DisguiseSpyFor(HouseClass* pHouse, InfantryClass* pSpy, HouseClass* pVictim)
	{
		if (!pHouse || !pSpy || !pVictim || !pSpy->Type || !pSpy->Type->CanDisguise || pSpy->InLimbo)
			return false;

		if (IsSpyDisguisedFor(pSpy, pVictim))
		{
			UpdateSpyKitShot(pHouse, pSpy);
			return true;
		}

		auto const pSource = FindSpyDisguiseSource(pHouse, pSpy, pVictim);

		return pSource && CopySoldierDisguise(pHouse, pSpy, pSource);
	}

	// The enemy house a spy of ours is going to: its building target, or the one SpecOps noted.
	HouseClass* GetSpyVictim(HouseClass* pHouse, InfantryClass* pSpy)
	{
		auto const pGoal = abstract_cast<BuildingClass*>(pSpy->Destination ? pSpy->Destination : pSpy->ArchiveTarget);

		if (pGoal && pGoal->IsAlive && pGoal->Owner && !pGoal->Owner->IsNeutral() && !pHouse->IsAlliedWith(pGoal->Owner))
			return pGoal->Owner;

		for (auto const& spy : GetSpecOpsState(pHouse).Spies)
		{
			if (spy.SpyID == pSpy->UniqueID && spy.TargetOwner >= 0 && spy.TargetOwner < HouseClass::Array.Count)
			{
				auto const pOwner = HouseClass::Array.GetItem(spy.TargetOwner);

				return (pOwner && !pOwner->Defeated && !pHouse->IsAlliedWith(pOwner)) ? pOwner : nullptr;
			}
		}

		return nullptr;
	}

	// How far a techno sees through disguises, in cells (0: it does not). DetectDisguiseRange= stamps
	// cells for its house; only ground-capable (AG) weapons extend a detector's own shooting reach,
	// never anti-air-only SAM launchers (whose 16-cell AA range previously marked whole bases watched).
	int DisguiseDetectionReach(TechnoTypeClass* pType)
	{
		auto const pInfType = abstract_cast<InfantryTypeClass*>(pType);

		if (!pType || !(pType->DetectDisguise || (pInfType && pInfType->Doggie)))
			return 0;

		int reach = std::max(0, pType->DetectDisguiseRange);

		for (int i = 0; i < 2; ++i)
		{
			auto const pSlot = pType->GetWeapon(i);
			auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

			if (!pWeapon || pWeapon->Range <= 0 || pWeapon->Damage <= 0)
				continue;

			if (pWeapon->Projectile && !pWeapon->Projectile->AG)
				continue;

			if (reach <= 0 || (pInfType && pInfType->Doggie) || pType->WhatAmI() == AbstractType::BuildingType)
				reach = std::max(reach, pWeapon->Range / Unsorted::LeptonsPerCell);
		}

		return reach > 0 ? reach : std::max(1, pType->Sight);
	}

	// How far a techno reveals cloaked units (Nod Chameleon Spy, stealth raiders), in cells (0: none):
	// CloakRadiusInCells on SensorArray buildings, and SensorsSight (or Sight when 0) on Sensors=yes.
	int CloakDetectionReach(TechnoTypeClass* pType)
	{
		if (!pType)
			return 0;

		auto const pBldType = abstract_cast<BuildingTypeClass*>(pType);
		int reach = 0;

		if (pBldType && pBldType->SensorArray)
			reach = std::max<int>(reach, pBldType->CloakRadiusInCells);

		if (pType->Sensors || (pBldType && pBldType->SensorArray && reach <= 0))
			reach = std::max(reach, pType->SensorsSight > 0 ? pType->SensorsSight : std::max(1, pType->Sight));

		return reach;
	}

	// A detector that takes a spy over (mind control: Yuri clones, the Psychic Tower) is worse than
	// one that kills it: the enemy gets the spy.
	bool IsMindControlDetector(TechnoTypeClass* pType)
	{
		for (int i = 0; pType && i < 2; ++i)
		{
			auto const pSlot = pType->GetWeapon(i);
			auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

			if (pWeapon && pWeapon->Warhead && pWeapon->Warhead->MindControl)
				return true;
		}

		return false;
	}

	// Known enemy counter that reveals disguises (Reach) and/or cloak (CloakReach).
	struct NeuralDisguiseDetector
	{
		CellStruct Cell;
		int Reach;
		int CloakReach;
		int OwnerIndex;
		bool Building;
		bool MindControl;
	};

	struct NeuralDisguiseDetectorCache
	{
		int Frame { -1 };
		std::vector<NeuralDisguiseDetector> Detectors {};
	};

	std::array<NeuralDisguiseDetectorCache, 32> DisguiseDetectorCaches {};

	const std::vector<NeuralDisguiseDetector>& GetKnownDisguiseDetectors(HouseClass* pHouse)
	{
		auto& cache = DisguiseDetectorCaches[std::clamp(pHouse ? pHouse->ArrayIndex : 0, 0, 31)];

		if (cache.Frame == Unsorted::CurrentFrame || !pHouse)
			return cache.Detectors;

		cache.Frame = Unsorted::CurrentFrame;
		cache.Detectors.clear();

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			auto const pType = info.Building->Type;
			const int disgReach = DisguiseDetectionReach(pType);
			const int cloakReach = CloakDetectionReach(pType);

			if (disgReach > 0 || cloakReach > 0)
			{
				const int ownerIdx = info.Building->Owner ? info.Building->Owner->ArrayIndex : -1;
				cache.Detectors.push_back({ info.Building->GetMapCoords(), disgReach, cloakReach, ownerIdx, true, IsMindControlDetector(pType) });
			}
		}

		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || pFoot->InLimbo || !pFoot->IsAlive || !pFoot->Owner || pHouse->IsAlliedWith(pFoot->Owner)
				|| pFoot->Owner->IsNeutral())
			{
				continue;
			}

			auto const pType = pFoot->GetTechnoType();
			const int disgReach = DisguiseDetectionReach(pType);
			const int cloakReach = CloakDetectionReach(pType);

			if ((disgReach > 0 || cloakReach > 0) && IsCellKnownToAI(pHouse, pFoot->GetMapCoords()))
				cache.Detectors.push_back({ pFoot->GetMapCoords(), disgReach, cloakReach, pFoot->Owner->ArrayIndex, false, IsMindControlDetector(pType) });
		}

		return cache.Detectors;
	}

	// Detection radius of this counter against a given spy type: CloakReach for cloaked spies (Nod
	// Chameleon Spy), DisguiseReach for disguised spies (Allied Spy), min of both when a unit has both.
	int SpyCounterReachFor(const NeuralDisguiseDetector& detector, const InfantryTypeClass* pSpyType)
	{
		if (!pSpyType)
			return detector.Reach;

		const bool cloaks = pSpyType->Cloakable;
		const bool disguises = pSpyType->CanDisguise;

		if (cloaks && !disguises)
			return detector.CloakReach;

		if (disguises && !cloaks)
			return detector.Reach;

		if (cloaks && disguises)
			return std::min(detector.CloakReach, detector.Reach);

		return std::max(detector.Reach, detector.CloakReach);
	}

	// Known enemy buildings that see through disguises covering a cell, plus marginCells.
	int CountKnownDisguiseDetectorBuildings(HouseClass* pHouse, const CellStruct& cell, int marginCells)
	{
		int count = 0;

		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			if (detector.Building && detector.Reach > 0 && detector.Cell.DistanceFrom(cell) <= static_cast<double>(detector.Reach + marginCells))
				count++;
		}

		return count;
	}

	// Known enemy static buildings that actually reveal pSpyType around cell (DetectDisguise buildings
	// for Allied Spy; SensorArray / Sensors buildings for Nod Chameleon Spy).
	int CountKnownSpyCounterBuildings(HouseClass* pHouse, const InfantryTypeClass* pSpyType, const CellStruct& cell, int marginCells)
	{
		int count = 0;

		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			const int reach = SpyCounterReachFor(detector, pSpyType);

			if (detector.Building && reach > 0 && detector.Cell.DistanceFrom(cell) <= static_cast<double>(reach + marginCells))
				count++;
		}

		return count;
	}

	bool IsCellWatchedByDisguiseDetector(HouseClass* pHouse, const CellStruct& cell, int marginCells)
	{
		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			if (detector.Reach > 0 && detector.Cell.DistanceFrom(cell) <= static_cast<double>(detector.Reach + marginCells))
				return true;
		}

		return false;
	}

	bool IsCellWatchedForSpy(HouseClass* pHouse, const InfantryClass* pSpy, const CellStruct& cell, int marginCells)
	{
		auto const pSpyType = pSpy ? pSpy->Type : nullptr;

		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			const int reach = SpyCounterReachFor(detector, pSpyType);

			if (reach > 0 && detector.Cell.DistanceFrom(cell) <= static_cast<double>(reach + marginCells))
				return true;
		}

		return false;
	}

	// Counter danger around a target building's door specifically for pSpy.
	int SpyCounterPenaltyAt(HouseClass* pHouse, const InfantryClass* pSpy, const CellStruct& cell)
	{
		auto const pSpyType = pSpy ? pSpy->Type : nullptr;
		int penalty = 0;

		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			const int reach = SpyCounterReachFor(detector, pSpyType);

			if (reach > 0 && detector.Cell.DistanceFrom(cell) <= static_cast<double>(reach + 1))
				penalty += (detector.Building ? 70 : (reach <= 2 ? 25 : 45)) * (detector.MindControl ? 2 : 1);
		}

		return penalty;
	}

	// Safe counter-avoiding waypoint for a spy walking from fromCell to toCell: ONLY counters that
	// detect THIS spy (DetectDisguise / Doggie for disguised Allied spies; Sensors / SensorArray for
	// cloaked Nod spies) plus armed enemies of houses not fooled when neither cloaked nor disguised
	// count as threats. Non-detector turrets and tanks that the spy's disguise or cloak fools are
	// completely ignored so the spy only detours around real counters.
	CellStruct FindSafeSpyWaypoint(HouseClass* pHouse, const InfantryClass* pSpy, const CellStruct& fromCell,
		const CellStruct& toCell, HouseClass* pVictim, int* outBestExposure = nullptr)
	{
		if (outBestExposure)
			*outBestExposure = 0;

		if (!pHouse || !pSpy || !pSpy->Type || fromCell == CellStruct::Empty || toCell == CellStruct::Empty)
			return CellStruct::Empty;

		auto const pSpyType = pSpy->Type;
		const bool spyCloaks = pSpyType->Cloakable;

		struct SpyThreat
		{
			int X;
			int Y;
			int Radius;
			int Weight;
		};

		std::vector<SpyThreat> threats;

		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			const int reach = SpyCounterReachFor(detector, pSpyType);

			if (reach <= 0)
				continue;

			if (detector.Cell.DistanceFrom(toCell) <= 2.0)
				continue;

			const int weight = (detector.MindControl ? 2 : 1) * (detector.Building ? 7 : (reach <= 2 ? 3 : 6));
			threats.push_back({ detector.Cell.X, detector.Cell.Y, reach + 2, weight });
		}

		if (!spyCloaks)
		{
			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				if (!info.IsDefense || !info.Building->Owner)
					continue;

				if (IsSpyDisguisedFor(pSpy, info.Building->Owner)
					|| (pVictim && pSpyType->CanDisguise && (info.Building->Owner == pVictim || info.Building->Owner->IsAlliedWith(pVictim))
						&& FindSpyDisguiseSource(pHouse, const_cast<InfantryClass*>(pSpy), pVictim)))
				{
					continue;
				}

				const int gr = GroundRangeOf(info.Building->Type);

				if (gr > 0)
					threats.push_back({ info.Coords.X, info.Coords.Y, gr + 2, 4 });
			}

			for (auto const pFoot : FootClass::Array)
			{
				if (!pFoot || pFoot->InLimbo || !pFoot->IsAlive || !pFoot->Owner || pHouse->IsAlliedWith(pFoot->Owner)
					|| pFoot->Owner->IsNeutral() || IsSpyDisguisedFor(pSpy, pFoot->Owner))
				{
					continue;
				}

				if (pVictim && pSpyType->CanDisguise && (pFoot->Owner == pVictim || pFoot->Owner->IsAlliedWith(pVictim))
					&& FindSpyDisguiseSource(pHouse, const_cast<InfantryClass*>(pSpy), pVictim))
				{
					continue;
				}

				if (!IsCellVisibleToAI(pHouse, pFoot->GetCell()))
					continue;

				const int gr = GroundRangeOf(pFoot->GetTechnoType());

				if (gr > 0)
				{
					const CellStruct fc = pFoot->GetMapCoords();
					threats.push_back({ fc.X, fc.Y, gr + 2, 3 });
				}
			}
		}

		if (threats.empty())
			return CellStruct::Empty;

		auto segmentExposure = [&threats](const CellStruct& a, const CellStruct& b) -> int
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

		const int directExposure = segmentExposure(fromCell, toCell);

		if (directExposure <= 0)
			return CellStruct::Empty;

		const double laneDX = static_cast<double>(toCell.X - fromCell.X);
		const double laneDY = static_cast<double>(toCell.Y - fromCell.Y);
		const double laneLength = std::sqrt(laneDX * laneDX + laneDY * laneDY);

		if (laneLength < 4.5)
		{
			if (outBestExposure)
				*outBestExposure = directExposure;

			return CellStruct::Empty;
		}

		const double normalX = -laneDY / laneLength;
		const double normalY = laneDX / laneLength;
		constexpr double LaneFractions[] = { 0.25, 0.5, 0.75 };
		constexpr int LaneOffsets[] = { 6, -6, 12, -12, 20, -20, 30, -30 };

		CellStruct bestWaypoint = CellStruct::Empty;
		int bestExposure = directExposure;
		int bestTotalCost = directExposure;

		for (const double fraction : LaneFractions)
		{
			for (const int offset : LaneOffsets)
			{
				const CellStruct candidate {
					static_cast<short>(fromCell.X + laneDX * fraction + normalX * offset),
					static_cast<short>(fromCell.Y + laneDY * fraction + normalY * offset)
				};

				auto const pCandCell = MapClass::Instance.TryGetCellAt(candidate);

				if (!pCandCell || !MapClass::Instance.CoordinatesLegal(candidate) || pCandCell->LandType == LandType::Water)
					continue;

				if (!pCandCell->IsClearToMove(pSpyType->SpeedType, true, true, -1, pSpyType->MovementZone, -1, false)
					|| !AreZonesConnected(fromCell, candidate, pSpyType->MovementZone))
				{
					continue;
				}

				const int exposure = segmentExposure(fromCell, candidate) + segmentExposure(candidate, toCell);
				const int detourLength = static_cast<int>(fromCell.DistanceFrom(candidate) + candidate.DistanceFrom(toCell));
				const int totalCost = exposure + std::max(0, detourLength - static_cast<int>(laneLength)) / 5;

				if (totalCost < bestTotalCost)
				{
					bestTotalCost = totalCost;
					bestExposure = exposure;
					bestWaypoint = candidate;
				}
			}
		}

		if (outBestExposure)
			*outBestExposure = bestExposure;

		if (bestWaypoint != CellStruct::Empty && (bestExposure == 0 || bestTotalCost * 10 < directExposure * 8))
			return bestWaypoint;

		if (outBestExposure)
			*outBestExposure = directExposure;

		return CellStruct::Empty;
	}

	int SpyCounterRoutePenalty(HouseClass* pHouse, const InfantryClass* pSpy, const CellStruct& from, const CellStruct& to, HouseClass* pVictim)
	{
		int residualExposure = 0;
		FindSafeSpyWaypoint(pHouse, pSpy, from, to, pVictim, &residualExposure);

		return std::min(220, residualExposure * 2);
	}

	// Combat value of the house's own armed units within radius of a cell.
	int OwnArmedValueNear(HouseClass* pHouse, const CellStruct& cell, int radius)
	{
		int value = 0;

		ForEachTechnoNear(cell, radius, [&](TechnoClass* pTech) -> bool
		{
			if (pTech->Owner != pHouse || pTech->WhatAmI() == AbstractType::Building)
				return true;

			auto const pType = pTech->GetTechnoType();

			if (pType && GroundRangeOf(pType) > 0)
				value += GetCombatValueOfType(pType, false);

			return true;
		});

		return value;
	}

	// A failed capture blocks that tech for a while (longer after each failure) and its neighbours
	// within 6 cells as long, since whatever stopped the engineer is likely still around them.
	void FailSpecOpsCapture(NeuralSpecOpsState& state, DWORD targetID, int baseBlockFrames)
	{
		auto const pTech = FindKnownTech(state, targetID);

		if (!pTech)
			return;

		pTech->Failures++;
		const int until = Unsorted::CurrentFrame + baseBlockFrames * std::min(pTech->Failures, 3);
		const CellStruct cell = pTech->Cell;

		for (auto& tech : state.Techs)
		{
			if (tech.Cell.DistanceFrom(cell) <= 6.0)
			{
				tech.BlockedUntilFrame = std::max(tech.BlockedUntilFrame, until);

				if (&tech != pTech)
					tech.Failures = std::max(tech.Failures, 1);
			}
		}
	}

	// The ground danger map of the crossings (NeuralAI.Operations.inl).
	struct OpDangerMap;
	const OpDangerMap& GetOpDangerMap(HouseClass* pHouse, bool airborne);
	int OpLineCost(const OpDangerMap& map, const CellStruct& from, const CellStruct& to, int& danger);

	bool CanLogCaptureBarracks(NeuralSpecOpsState& state)
	{
		return state.CaptureLogLines++ < CaptureBarracksLogLines;
	}

	// Capture barracks (CaptureCrossingDangerLimit), on each tech memory scan: the barracks placed for a
	// site once it stands, the site of the next one, and the barracks of ours the next engineer is
	// trained at. Fog-fair: the techs as this house last saw them, our own buildings and engineers.
	void UpdateCaptureBarracks(HouseClass* pHouse, NeuralSpecOpsState& state)
	{
		const int now = Unsorted::CurrentFrame;
		// Our barracks and the buildings we can build next to, each with its land.
		std::vector<std::pair<CellStruct, int>> barracks;
		std::vector<std::pair<CellStruct, int>> anchors;

		// A barracks placed for a site that never stood (lost while it went up) is forgotten.
		if (state.CapturePlacedZone >= 0 && now - state.CapturePlacedFrame > CaptureBarracksNoRoomFrames)
			state.CapturePlacedZone = -1;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->Type->InvisibleInGame
				|| pBld->CurrentMission == Mission::Selling || pBld->QueuedMission == Mission::Selling)
			{
				continue;
			}

			const int zone = BuildingGroundZone(pBld);

			if (!IsGroundZone(zone))
				continue;

			const CellStruct cell = pBld->GetMapCoords();

			if (pBld->Type->Factory == AbstractType::InfantryType && !pBld->Type->Naval)
			{
				barracks.emplace_back(cell, zone);

				// The barracks placed for a site stands: it is the capture barracks of that land.
				if (zone == state.CapturePlacedZone)
				{
					state.CaptureBarracks.push_back({ pBld->UniqueID, cell, zone, state.CapturePlacedTechCell });
					state.CaptureBarracksBuilt++;
					state.CapturePlacedZone = -1;
					Debug::Log("[NeuralAI] House %d capture barracks: %s stands at (%d,%d) on land %d, %d cells from the tech at (%d,%d), frame %d "
						"(capture barracks %d).\n", pHouse->ArrayIndex, pBld->Type->ID, cell.X, cell.Y, zone,
						static_cast<int>(cell.DistanceFrom(state.CapturePlacedTechCell)), state.CapturePlacedTechCell.X,
						state.CapturePlacedTechCell.Y, now, state.CaptureBarracksBuilt);
				}
			}

			if (pBld->Type->BaseNormal || pBld->Type->ConstructionYard || BuildingTypeExt::IsAIBaseNormal(pBld->Type))
				anchors.emplace_back(cell, zone);
		}

		// The lands an engineer of ours stands on, and the buildings our engineers are on their way into.
		std::vector<int> engineerZones;
		std::vector<const AbstractClass*> engineerGoals;

		for (auto const pInf : InfantryClass::Array)
		{
			if (!pInf || pInf->Owner != pHouse || !pInf->IsAlive || !IsCaptureInfantryType(pInf->Type))
				continue;

			if (pInf->Destination)
				engineerGoals.push_back(pInf->Destination);

			if (pInf->InLimbo)
				continue;

			const int zone = ObjectZone(pInf, MovementZone::Normal);

			if (IsGroundZone(zone) && std::find(engineerZones.begin(), engineerZones.end(), zone) == engineerZones.end())
				engineerZones.push_back(zone);
		}

		// Nearest entry of a list on a land to a cell (INT_MAX: none there).
		auto const nearestOn = [](const std::vector<std::pair<CellStruct, int>>& list, int zone, const CellStruct& cell, CellStruct* pAt)
		{
			int best = INT_MAX;

			for (auto const& [at, atZone] : list)
			{
				const int distance = static_cast<int>(at.DistanceFrom(cell));

				if (atZone == zone && distance < best)
				{
					best = distance;

					if (pAt)
						*pAt = at;
				}
			}

			return best;
		};

		auto const pExt = HouseExt::Fetch(pHouse);
		const int expert = pExt ? pExt->NeuralAI_ActiveExpert : -1;
		const OpDangerMap* pDangerMap = nullptr;

		const NeuralKnownTech* pSite = nullptr;
		BuildingClass* pSiteBld = nullptr;
		int siteZone = -1;
		int siteScore = 0;
		bool siteUrgent = false;
		int siteDanger = 0;
		int siteWalk = 0;

		const NeuralKnownTech* pGoal = nullptr;
		CellStruct goalCell = CellStruct::Empty;
		int goalScore = 0;

		for (auto const& tech : state.Techs)
		{
			if (!state.CaptureType || barracks.empty())
				break;

			if (tech.OwnerIndex == pHouse->ArrayIndex)
				continue;

			auto const pOwner = (tech.OwnerIndex >= 0 && tech.OwnerIndex < HouseClass::Array.Count)
				? HouseClass::Array.GetItem(tech.OwnerIndex)
				: nullptr;
			const bool enemyHeld = tech.OwnerIndex >= 0;

			if ((enemyHeld && (!pOwner || pHouse->IsAlliedWith(pOwner))) || (!enemyHeld && IsTechClaimedByAlly(pHouse, tech.ID))
				|| IsCellInKnownEnemyDefenseReach(pHouse, tech.Cell, 2))
			{
				continue;
			}

			auto const pTechBld = FindBuildingByID(tech.ID);

			if (!pTechBld || !pTechBld->IsAlive || pTechBld->InLimbo || !pTechBld->Type)
				continue;

			const int worth = tech.Value * ((enemyHeld && tech.Cash) ? 2 : 1);
			int barracksZone = -1;
			CellStruct barracksCell = CellStruct::Empty;
			int barracksDist = INT_MAX;

			for (const int zone : tech.Zones)
			{
				CellStruct at = CellStruct::Empty;
				const int distance = IsGroundZone(zone) ? nearestOn(barracks, zone, tech.Cell, &at) : INT_MAX;

				if (distance < barracksDist)
				{
					barracksDist = distance;
					barracksZone = zone;
					barracksCell = at;
				}
			}

			if (barracksZone < 0)
			{
				// No barracks of ours on its land: a site where we can build next to our buildings there,
				// unless that land had no room for one lately.
				int walk = INT_MAX;
				int walkZone = -1;

				for (const int zone : tech.Zones)
				{
					const bool noRoom = std::any_of(state.CaptureNoRoom.begin(), state.CaptureNoRoom.end(),
						[zone, now](const NeuralCaptureNoRoom& entry) { return entry.Zone == zone && now < entry.UntilFrame; });
					const int distance = (IsGroundZone(zone) && !noRoom) ? nearestOn(anchors, zone, tech.Cell, nullptr) : INT_MAX;

					if (distance < walk)
					{
						walk = distance;
						walkZone = zone;
					}
				}

				if (walkZone < 0)
					continue;

				// The straight crossing from our nearest barracks, all of them on other lands.
				int danger = 0;

				if (tech.Failures == 0)
				{
					CellStruct from = barracks.front().first;

					for (auto const& [at, atZone] : barracks)
					{
						if (at.DistanceFrom(tech.Cell) < from.DistanceFrom(tech.Cell))
							from = at;
					}

					if (!pDangerMap)
						pDangerMap = &GetOpDangerMap(pHouse, false);

					OpLineCost(*pDangerMap, from, tech.Cell, danger);
				}

				const bool urgent = tech.Failures > 0 || danger > CaptureCrossingDangerLimit;
				const int score = worth * 1000 / (walk + 12);

				if (!pSite || (urgent && !siteUrgent) || (urgent == siteUrgent && score > siteScore))
				{
					pSite = &tech;
					pSiteBld = pTechBld;
					siteZone = walkZone;
					siteScore = score;
					siteUrgent = urgent;
					siteDanger = danger;
					siteWalk = walk;
				}

				continue;
			}

			// A barracks of ours on its land: the next engineer may be trained there for it, when the
			// engineer can take it (an enemy-held tech only Expert 4 goes for, or the resource hoarder for
			// its cash), no engineer of ours stands on that land or goes into it, and it is not blocked
			// by recent failures.
			const bool takenByUs = std::find(engineerGoals.begin(), engineerGoals.end(), static_cast<const AbstractClass*>(pTechBld))
				!= engineerGoals.end() || IsSpecOpsCaptureTarget(pHouse, tech.ID, 0);
			const bool covered = std::any_of(tech.Zones.begin(), tech.Zones.end(), [&engineerZones](int zone)
				{
					return IsGroundZone(zone) && std::find(engineerZones.begin(), engineerZones.end(), zone) != engineerZones.end();
				});
			const bool takable = !enemyHeld || expert == 4 || (expert == 6 && tech.Cash);

			if (takenByUs || covered || !takable || tech.Walled || now < tech.BlockedUntilFrame)
				continue;

			const int score = worth * 1000 / (barracksDist + 12);

			if (!pGoal || score > goalScore)
			{
				pGoal = &tech;
				goalCell = barracksCell;
				goalScore = score;
			}
		}

		const DWORD previousSite = state.CaptureSiteTechID;
		state.CaptureSiteTechID = pSite ? pSite->ID : 0;
		state.CaptureSiteCell = pSite ? pSite->Cell : CellStruct::Empty;
		state.CaptureSiteZone = pSite ? siteZone : -1;
		state.CaptureSiteUrgent = pSite && siteUrgent;

		if (pSite && pSite->ID != previousSite && CanLogCaptureBarracks(state))
		{
			Debug::Log("[NeuralAI] House %d capture barracks: site %s at (%d,%d) on land %d (owner %d), %s (failures %d, crossing danger %d), "
				"our nearest building there %d cells away, frame %d.\n", pHouse->ArrayIndex, pSiteBld->Type->ID, pSite->Cell.X, pSite->Cell.Y,
				siteZone, pSite->OwnerIndex, siteUrgent ? "urgent" : "not urgent", pSite->Failures, siteDanger, siteWalk, now);
		}

		state.CaptureEngineerCell = pGoal ? goalCell : CellStruct::Empty;
		state.CaptureEngineerTechID = pGoal ? pGoal->ID : 0;
	}

	// A capture site waits for its barracks.
	bool IsCaptureSiteWaiting(HouseClass* pHouse)
	{
		return pHouse && GetSpecOpsState(pHouse).CaptureSiteTechID != 0;
	}

	// A barracks of this type goes to the capture site now (placement).
	bool CaptureBarracksWants(HouseClass* pHouse, const BuildingTypeClass* pType)
	{
		return pType && pType->Factory == AbstractType::InfantryType && !pType->Naval && IsCaptureSiteWaiting(pHouse);
	}

	bool IsCaptureSiteUrgent(HouseClass* pHouse)
	{
		return GetSpecOpsState(pHouse).CaptureSiteUrgent;
	}

	// Where placement put a barracks the capture site wanted (Empty: no cell on the site's land, which
	// then waits CaptureBarracksNoRoomFrames, longer each time up to CaptureBarracksNoRoomSteps times as
	// long). The site is cleared at once: the next pick asks for no second barracks there, and a barracks
	// with no room there goes where any other would.
	void NoteCaptureBarracksPlacement(HouseClass* pHouse, const BuildingTypeClass* pType, const CellStruct& cell)
	{
		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (cell == CellStruct::Empty)
		{
			auto it = std::find_if(state.CaptureNoRoom.begin(), state.CaptureNoRoom.end(),
				[&state](const NeuralCaptureNoRoom& entry) { return entry.Zone == state.CaptureSiteZone; });

			if (it == state.CaptureNoRoom.end())
				it = state.CaptureNoRoom.insert(state.CaptureNoRoom.end(), NeuralCaptureNoRoom { state.CaptureSiteZone, 0, -1 });

			it->Count++;
			it->UntilFrame = now + CaptureBarracksNoRoomFrames * std::min(it->Count, CaptureBarracksNoRoomSteps);

			if (CanLogCaptureBarracks(state))
			{
				Debug::Log("[NeuralAI] House %d capture barracks: no room for %s on land %d near the tech at (%d,%d) (time %d), frame %d.\n",
					pHouse->ArrayIndex, pType->ID, state.CaptureSiteZone, state.CaptureSiteCell.X, state.CaptureSiteCell.Y, it->Count, now);
			}
		}
		else
		{
			state.CapturePlacedZone = state.CaptureSiteZone;
			state.CapturePlacedTechCell = state.CaptureSiteCell;
			state.CapturePlacedFrame = now;
			Debug::Log("[NeuralAI] House %d capture barracks: %s placed at (%d,%d), %d cells from the tech at (%d,%d) on land %d, %s, frame %d.\n",
				pHouse->ArrayIndex, pType->ID, cell.X, cell.Y, static_cast<int>(cell.DistanceFrom(state.CaptureSiteCell)), state.CaptureSiteCell.X,
				state.CaptureSiteCell.Y, state.CaptureSiteZone, state.CaptureSiteUrgent ? "urgent" : "not urgent", now);
		}

		state.CaptureSiteTechID = 0;
		state.CaptureSiteCell = CellStruct::Empty;
		state.CaptureSiteZone = -1;
		state.CaptureSiteUrgent = false;
	}

	// The barracks of ours the engineer in training comes out of (Empty: the usual choice).
	CellStruct CaptureEngineerGoal(HouseClass* pHouse)
	{
		return GetSpecOpsState(pHouse).CaptureEngineerCell;
	}

	// An engineer in training was given the barracks on its tech's land (logged once per engineer).
	void NoteCaptureEngineerRouted(HouseClass* pHouse, const TechnoClass* pEngineer, const BuildingClass* pBarracks)
	{
		auto& state = GetSpecOpsState(pHouse);

		if (!pEngineer || !pBarracks || pEngineer->UniqueID == state.CaptureRoutedObjectID)
			return;

		state.CaptureRoutedObjectID = pEngineer->UniqueID;
		state.CaptureEngineersRouted++;
		auto const pTechBld = FindBuildingByID(state.CaptureEngineerTechID);
		const CellStruct barracksCell = pBarracks->GetMapCoords();
		const bool captureBarracks = std::any_of(state.CaptureBarracks.begin(), state.CaptureBarracks.end(),
			[pBarracks](const NeuralCaptureBarracks& entry) { return entry.ID == pBarracks->UniqueID; });

		if (CanLogCaptureBarracks(state))
		{
			Debug::Log("[NeuralAI] House %d capture barracks: engineer %u trained at %s (%d,%d)%s for %s at (%d,%d), %d cells away, frame %d "
				"(engineers %d).\n", pHouse->ArrayIndex, pEngineer->UniqueID, pBarracks->Type->ID, barracksCell.X, barracksCell.Y,
				captureBarracks ? ", a capture barracks," : "", pTechBld && pTechBld->Type ? pTechBld->Type->ID : "?",
				pTechBld ? pTechBld->GetMapCoords().X : -1, pTechBld ? pTechBld->GetMapCoords().Y : -1,
				pTechBld ? static_cast<int>(barracksCell.DistanceFrom(pTechBld->GetMapCoords())) : -1, Unsorted::CurrentFrame,
				state.CaptureEngineersRouted);
		}
	}

	bool UpdateSpecOpsBreach(HouseClass* pHouse, NeuralSpecOpsState& state, NeuralSpecOpsCapture& capture, BuildingClass* pTarget, int now);
	void ReleaseSpecOpsBreachers(HouseClass* pHouse, const NeuralSpecOpsCapture& capture);
	bool RequestTransportOperation(HouseClass* pHouse, NeuralOpClient client, FootClass* pFoot, const CellStruct& target);

	// Per AI tick: tech memory (every 150 frames), opportunity summary, captures and spies upkeep.
	void UpdateSpecOps(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const CellStruct home = GetHouseHomeCell(pHouse);

		if (now - state.LastScanFrame >= 150 || now < state.LastScanFrame)
		{
			state.LastScanFrame = now;
			UpdateSpecOpsTechMemory(pHouse, state);

			state.EngineerType = nullptr;
			state.CaptureType = nullptr;
			state.SpyType = nullptr;
			state.SpyAvailable = false;
			InfantryTypeClass* pWeaponCaptureType = nullptr;

			for (auto const pType : InfantryTypeClass::Array)
			{
				if (!pType || (!IsCaptureInfantryType(pType) && !IsSpyInfantryType(pType)))
					continue;

				const bool usable = pHouse->CountOwnedNow(pType) > 0 || CanBuildUnderHumanRules(pHouse, pType);

				if (!usable)
					continue;

				if (pType->Engineer && !state.EngineerType)
					state.EngineerType = pType;
				else if (CapturesByWeapon(pType) && !pWeaponCaptureType)
					pWeaponCaptureType = pType;
				else if (IsSpyInfantryType(pType))
				{
					state.SpyAvailable = true;

					if (!state.SpyType)
						state.SpyType = pType;
				}
			}

			state.CaptureType = state.EngineerType ? state.EngineerType : pWeaponCaptureType;

			const int homeZone = (home != CellStruct::Empty)
				? MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false)
				: -1;

			state.KnownNeutralTechs = 0;
			state.KnownNeutralCash = 0;
			state.SafeTechValue = 0;
			state.ContestedTechValue = 0;

			// Worth of each reachable tech out of known defense reach, full up to 40 cells from home and
			// fading beyond; contested ones (enemies in sight near them) at half. Only the three best
			// count: a map with dozens of derricks has no more engineer work at a time than one with three.
			std::array<int, 3> bestWorth {};

			for (auto& tech : state.Techs)
			{
				if (tech.OwnerIndex >= 0)
					continue;

				state.KnownNeutralTechs++;
				state.KnownNeutralCash += tech.Cash ? 1 : 0;

				// A walled-in tech counts when a single piece of its wall opens it: a wall breaker goes
				// with the engineer (by transport too when the tech stands on another land).
				NeuralTechBreach breach;
				auto const pTechBld = FindBuildingByID(tech.ID);
				const NeuralTechWall wall = GetTechWallStateFrom(pHouse, pTechBld, homeZone, home, breach);
				const bool wasWalled = tech.Walled;
				tech.Walled = wall != NeuralTechWall::Open;

				if (tech.Walled != wasWalled && pTechBld && CanLogSpecOpsBreach())
				{
					if (tech.Walled)
					{
						Debug::Log("[NeuralAI] House %d SpecOps: breach: %s at (%d,%d) walled in, %s (wall piece (%d,%d)), frame %d.\n",
							pHouse->ArrayIndex, pTechBld->Type->ID, tech.Cell.X, tech.Cell.Y,
							wall == NeuralTechWall::Breachable ? "breachable" : "sealed", breach.Wall.X, breach.Wall.Y, now);
					}
					else
					{
						Debug::Log("[NeuralAI] House %d SpecOps: breach: %s at (%d,%d) no longer walled in, frame %d.\n",
							pHouse->ArrayIndex, pTechBld->Type->ID, tech.Cell.X, tech.Cell.Y, now);
					}
				}
				const bool reachable = wall == NeuralTechWall::Open
					? CanEngineerReachTech(pHouse, tech, homeZone, state.CaptureType)
					: wall == NeuralTechWall::Breachable
						&& (breach.Zone == homeZone || (state.CaptureType && CanHouseFerryTypeAcrossWater(pHouse, state.CaptureType)));

				if (now < tech.BlockedUntilFrame || !reachable || IsCellInKnownEnemyDefenseReach(pHouse, tech.Cell, 2))
					continue;

				const bool contested = VisibleEnemyThreatNear(pHouse, tech.Cell, 7) > 0;
				const int dist = (home != CellStruct::Empty) ? static_cast<int>(home.DistanceFrom(tech.Cell)) : 40;
				int worth = tech.Value * 30 * 40 / std::max(40, dist);

				if (contested)
				{
					worth /= 2;
					state.ContestedTechValue += tech.Value;
				}
				else
				{
					state.SafeTechValue += tech.Value;
				}

				auto const pWorst = std::min_element(bestWorth.begin(), bestWorth.end());

				if (worth > *pWorst)
					*pWorst = worth;
			}

			// Two spies killed on the way within 9000 frames: the way in is watched, ask for no more.
			const bool spiesWasted = state.SpyLossFrames.size() >= 2
				&& now - state.SpyLossFrames[state.SpyLossFrames.size() - 2] < 9000;
			state.SpyTargets = (state.SpyAvailable && !spiesWasted) ? CountSpecOpsSpyTargets(pHouse, state.SpyType) : 0;

			auto const& bridges = GetBridgeState(pHouse);
			state.BridgeRepairTask = state.EngineerType && (bridges.RouteSpan >= 0 || bridges.FleetBlockSpan >= 0);
			state.BridgeDemolitionTask = AreBridgesDestroyable()
				&& (bridges.AmbushSpan >= 0 || bridges.SeaLaneSpan >= 0 || bridges.MoatSpan >= 0);

			// Selector prior of Expert 4: real work for it on this map right now. Techs only count
			// with an engineer we own or can train; ops in flight keep it in charge until they end.
			int opportunity = 0;

			if (state.CaptureType)
				opportunity += std::min(560, bestWorth[0] + bestWorth[1] + bestWorth[2]);

			opportunity += std::min(180, state.SpyTargets * 60);
			opportunity += state.BridgeRepairTask ? 260 : 0;
			opportunity += state.BridgeDemolitionTask ? 200 : 0;
			opportunity += state.Captures.empty() ? 0 : 160;
			state.Opportunity = std::min(780, opportunity);

			UpdateCaptureBarracks(pHouse, state);
		}

		// Captures upkeep.
		for (size_t i = 0; i < state.Captures.size();)
		{
			auto& capture = state.Captures[i];
			auto const pEngineer = FindOwnFootByID(pHouse, capture.EngineerID);
			auto const pTarget = FindBuildingByID(capture.TargetID);
			// A breach first takes the walk of the wall breakers and their shots.
			const int captureTimeout = capture.BreachFerryZone >= 0 ? 18000 : (capture.BreachFrame > 0 ? 10800 : 7200);
			bool done = false;

			if (pTarget && pTarget->Owner == pHouse)
			{
				done = true;
			}
			else if (!pEngineer)
			{
				// Killed on the way (or entered a building that changed hands meanwhile).
				FailSpecOpsCapture(state, capture.TargetID, 2700);
				Debug::Log("[NeuralAI] House %d SpecOps: engineer %u lost on the way to (%d,%d), frame %d.\n",
					pHouse->ArrayIndex, capture.EngineerID, capture.TargetCell.X, capture.TargetCell.Y, now);
				done = true;
			}
			// A tech an enemy took stays the target (the picker takes those too): closing on it each tick
			// stopped the engineer at once and the picker sent it again, every 15 to 30 frames (match
			// #10769, Crystal Whiplash).
			else if (!pTarget || !IsKnownCapturableTech(pHouse, pTarget) || now - capture.StartFrame > captureTimeout)
			{
				if (pTarget && now - capture.StartFrame > captureTimeout)
					FailSpecOpsCapture(state, capture.TargetID, 1800);

				// An engineer a transport operation carries waits for the operation to give it back.
				if ((pEngineer->Destination == pTarget || pEngineer->CurrentMission == Mission::Enter)
					&& !IsInTransportOp(pHouse, pEngineer->UniqueID))
				{
					pEngineer->SetDestination(nullptr, true);
					pEngineer->QueueMission(Mission::Guard, true);
				}

				done = true;
			}
			else if (capture.Dispatched && !pEngineer->InLimbo && home != CellStruct::Empty)
			{
				// On the way: turn back when the visible enemies around the engineer outweigh our
				// armed units there, unless it is already at the door.
				const CellStruct engCell = pEngineer->GetMapCoords();

				if (engCell.DistanceFrom(capture.TargetCell) > 5.0 && now - capture.DispatchFrame >= 150)
				{
					const int threat = VisibleEnemyThreatNear(pHouse, engCell, 5);

					if (threat > 0 && threat > OwnArmedValueNear(pHouse, engCell, 6))
					{
						FailSpecOpsCapture(state, capture.TargetID, 1800);

						if (auto const pHomeCell = MapClass::Instance.TryGetCellAt(home))
							IssueMoveOrder(pEngineer, pHomeCell, 4.0);

						Debug::Log("[NeuralAI] House %d SpecOps: engineer %u turns back from (%d,%d) (threat %d), frame %d.\n",
							pHouse->ArrayIndex, capture.EngineerID, capture.TargetCell.X, capture.TargetCell.Y, threat, now);
						done = true;
					}
				}
			}

			if (!done && pEngineer && pTarget && capture.BreachCell != CellStruct::Empty)
				done = !UpdateSpecOpsBreach(pHouse, state, capture, pTarget, now);

			if (done)
			{
				ReleaseSpecOpsBreachers(pHouse, capture);
				state.Captures.erase(state.Captures.begin() + static_cast<std::ptrdiff_t>(i));
			}
			else
			{
				++i;
			}
		}

		// Spies upkeep: a spy that vanished at the door went in.
		for (size_t i = 0; i < state.Spies.size();)
		{
			auto& spy = state.Spies[i];
			auto const pSpy = FindOwnFootByID(pHouse, spy.SpyID);
			bool done = false;

			if (!pSpy)
			{
				if (spy.LastDistance <= spy.DoorDistance)
				{
					state.InfiltrationCount++;
					Debug::Log("[NeuralAI] House %d SpecOps: spy %u infiltrated the building at (%d,%d) of house %d disguised as house %d, frame %d (infiltrations %d).\n",
						pHouse->ArrayIndex, spy.SpyID, spy.TargetCell.X, spy.TargetCell.Y, spy.TargetOwner, spy.DisguiseOwner, now,
						state.InfiltrationCount);
				}
				else
				{
					state.SpyLossFrames.push_back(now);

					if (state.SpyLossFrames.size() > 4)
						state.SpyLossFrames.erase(state.SpyLossFrames.begin());

					Debug::Log("[NeuralAI] House %d SpecOps: spy %u lost %d cells from (%d,%d) of house %d disguised as house %d, frame %d.\n",
						pHouse->ArrayIndex, spy.SpyID, spy.LastDistance, spy.TargetCell.X, spy.TargetCell.Y, spy.TargetOwner,
						spy.DisguiseOwner, now);
				}

				done = true;
			}
			else if (now - spy.StartFrame > 9000)
			{
				done = true;
			}
			else if (!pSpy->InLimbo)
			{
				spy.LastDistance = static_cast<int>(pSpy->GetMapCoords().DistanceFrom(spy.TargetCell));
				auto const pDisguiseHouse = SpyDisguiseHouse(abstract_cast<InfantryClass*>(pSpy));
				spy.DisguiseOwner = pDisguiseHouse ? pDisguiseHouse->ArrayIndex : -1;
			}

			if (done)
				state.Spies.erase(state.Spies.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}
	}

	// The engineer is on a SpecOps capture (Plans leave it there).
	bool IsSpecOpsEngineerBusy(HouseClass* pHouse, DWORD engineerID)
	{
		for (auto const& capture : GetSpecOpsState(pHouse).Captures)
		{
			if (capture.EngineerID == engineerID)
				return true;
		}

		return false;
	}

	int SpecOpsOpportunity(HouseClass* pHouse)
	{
		return GetSpecOpsState(pHouse).Opportunity;
	}

	void GetSpecOpsWork(HouseClass* pHouse, int& outTechValue, int& outSpyTargets, bool& outBridgeRepair, bool& outBridgeDemolition)
	{
		auto const& state = GetSpecOpsState(pHouse);
		outTechValue = state.CaptureType ? state.SafeTechValue + state.ContestedTechValue : 0;
		outSpyTargets = state.SpyTargets;
		outBridgeRepair = state.BridgeRepairTask;
		outBridgeDemolition = state.BridgeDemolitionTask;
	}

	void NoteSpecOpsSpy(HouseClass* pHouse, FootClass* pSpy, BuildingClass* pTarget)
	{
		if (!pHouse || !pSpy || !pTarget)
			return;

		auto& state = GetSpecOpsState(pHouse);

		for (auto& spy : state.Spies)
		{
			if (spy.SpyID == pSpy->UniqueID)
			{
				if (spy.TargetID != pTarget->UniqueID)
				{
					spy.TargetID = pTarget->UniqueID;
					spy.TargetCell = pTarget->GetMapCoords();
					spy.TargetOwner = pTarget->Owner ? pTarget->Owner->ArrayIndex : -1;
					spy.DoorDistance = 2 + std::max<int>(pTarget->Type->GetFoundationWidth(), pTarget->Type->GetFoundationHeight(false));
					spy.StartFrame = Unsorted::CurrentFrame;
					spy.Waypoint = CellStruct::Empty;
					spy.WaypointFrame = -1000;
				}

				return;
			}
		}

		NeuralSpecOpsSpy spy;
		spy.SpyID = pSpy->UniqueID;
		spy.TargetID = pTarget->UniqueID;
		spy.TargetCell = pTarget->GetMapCoords();
		spy.TargetOwner = pTarget->Owner ? pTarget->Owner->ArrayIndex : -1;
		spy.DoorDistance = 2 + (pTarget->Type ? std::max<int>(pTarget->Type->GetFoundationWidth(), pTarget->Type->GetFoundationHeight(false)) : 1);
		spy.LastDistance = static_cast<int>(pSpy->GetMapCoords().DistanceFrom(spy.TargetCell));
		spy.StartFrame = Unsorted::CurrentFrame;
		auto const pDisguiseHouse = SpyDisguiseHouse(abstract_cast<InfantryClass*>(pSpy));
		spy.DisguiseOwner = pDisguiseHouse ? pDisguiseHouse->ArrayIndex : -1;
		state.Spies.push_back(spy);
		Debug::Log("[NeuralAI] House %d SpecOps: spy %u (%s) -> %s of house %d at (%d,%d) disguised as house %d, frame %d.\n",
			pHouse->ArrayIndex, spy.SpyID, pSpy->GetTechnoType() ? pSpy->GetTechnoType()->ID : "?",
			pTarget->Type ? pTarget->Type->ID : "?", spy.TargetOwner, spy.TargetCell.X, spy.TargetCell.Y, spy.DisguiseOwner,
			spy.StartFrame);
	}

	BuildingClass* GetSpecOpsSpyTargetBuilding(HouseClass* pHouse, const InfantryClass* pSpy)
	{
		if (!pHouse || !pSpy)
			return nullptr;

		if (auto const pGoal = abstract_cast<BuildingClass*>(pSpy->Destination ? pSpy->Destination : pSpy->ArchiveTarget))
		{
			if (pGoal->IsAlive && !pGoal->InLimbo && pGoal->Owner && !pGoal->Owner->IsNeutral() && !pHouse->IsAlliedWith(pGoal->Owner))
				return pGoal;
		}

		for (auto const& spy : GetSpecOpsState(pHouse).Spies)
		{
			if (spy.SpyID == pSpy->UniqueID)
			{
				auto const pBld = FindBuildingByID(spy.TargetID);

				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Owner && !pBld->Owner->IsNeutral() && !pHouse->IsAlliedWith(pBld->Owner))
					return pBld;
			}
		}

		return nullptr;
	}

	void DropSpecOpsSpyTarget(HouseClass* pHouse, DWORD spyID)
	{
		auto& spies = GetSpecOpsState(pHouse).Spies;
		std::erase_if(spies, [spyID](const NeuralSpecOpsSpy& s) { return s.SpyID == spyID; });
	}

	template <typename DispatchFn>
	bool AdvanceRoutedSpyInfiltration(HouseClass* pHouse, InfantryClass* pSpy, BuildingClass* pTarget, DispatchFn&& dispatchEnter)
	{
		if (!pHouse || !pSpy || !pSpy->Type || pSpy->InLimbo || !pTarget || !pTarget->IsAlive || pTarget->InLimbo)
			return false;

		NoteSpecOpsSpy(pHouse, pSpy, pTarget);

		const int spyZone = ObjectZone(pSpy, MovementZone::Normal);
		const int bldZone = BuildingGroundZone(pTarget);

		if (spyZone > 1 && bldZone > 1 && spyZone != bldZone)
			return dispatchEnter(pSpy, pTarget, Mission::Enter, false);

		const CellStruct from = pSpy->GetMapCoords();
		const CellStruct goal = pTarget->GetMapCoords();
		const int now = Unsorted::CurrentFrame;
		NeuralSpecOpsSpy* pEntry = nullptr;

		for (auto& s : GetSpecOpsState(pHouse).Spies)
		{
			if (s.SpyID == pSpy->UniqueID)
			{
				pEntry = &s;
				break;
			}
		}

		if (from.DistanceFrom(goal) > 4.5 && pEntry)
		{
			const bool reachedWp = pEntry->Waypoint != CellStruct::Empty && from.DistanceFrom(pEntry->Waypoint) <= 3.0;
			const bool watchedWp = pEntry->Waypoint != CellStruct::Empty && IsCellWatchedForSpy(pHouse, pSpy, pEntry->Waypoint, 1);

			if (reachedWp)
				pEntry->Waypoint = CellStruct::Empty;

			if (pEntry->Waypoint == CellStruct::Empty || watchedWp || now - pEntry->WaypointFrame >= 90 || now < pEntry->WaypointFrame)
			{
				const CellStruct prevWp = pEntry->Waypoint;
				pEntry->Waypoint = FindSafeSpyWaypoint(pHouse, pSpy, from, goal, pTarget->Owner);
				pEntry->WaypointFrame = now;

				if (pEntry->Waypoint != CellStruct::Empty && pEntry->Waypoint != prevWp)
					CountDecision(NeuralDecision::SafeLaneDetour, pHouse->ArrayIndex);
			}

			if (pEntry->Waypoint != CellStruct::Empty)
			{
				if (auto const pWpCell = MapClass::Instance.TryGetCellAt(pEntry->Waypoint))
				{
					pSpy->SetTarget(nullptr);
					IssueMoveOrder(pSpy, pWpCell, 2.0);
					return true;
				}
			}
		}
		else if (pEntry)
		{
			pEntry->Waypoint = CellStruct::Empty;
		}

		if (pSpy->CurrentMission == Mission::Enter && (pSpy->Destination == pTarget || pSpy->ArchiveTarget == pTarget))
			return true;

		return dispatchEnter(pSpy, pTarget, Mission::Enter, false);
	}

	bool AdvancePlanSpyInfiltration(HouseClass* pHouse, InfantryClass* pSpy, BuildingClass* pTarget)
	{
		return AdvanceRoutedSpyInfiltration(pHouse, pSpy, pTarget,
			[](FootClass* pFoot, AbstractClass* pBld, Mission, bool)
			{
				if (auto const pTargetBld = abstract_cast<BuildingClass*>(pBld))
					return OrderFootToEnter(pFoot, pTargetBld);

				return false;
			});
	}

	// May this engineer take a tech another engineer of ours already goes for? Only when it stands
	// on the tech's own ground and the other one does not, or is more than twice as far: the
	// engineer on the spot captures, the far one picks something else. (An engineer landed next to
	// two oil derricks was sent across the water because two engineers that never arrived had them.)
	bool IsBetterPlacedForTech(HouseClass* pHouse, const NeuralKnownTech& tech, FootClass* pEngineer, int engineerZone)
	{
		if (engineerZone < 0 || std::find(tech.Zones.begin(), tech.Zones.end(), engineerZone) == tech.Zones.end())
			return false;

		for (auto const& capture : GetSpecOpsState(pHouse).Captures)
		{
			if (capture.TargetID != tech.ID || capture.EngineerID == pEngineer->UniqueID)
				continue;

			auto const pOther = FindOwnFootByID(pHouse, capture.EngineerID);

			// Riding a transport: it is on its way and keeps the tech.
			if (!pOther || pOther->InLimbo)
				return false;

			const int otherZone = ObjectZone(pOther, MovementZone::Normal);
			const bool otherOnTheGround = std::find(tech.Zones.begin(), tech.Zones.end(), otherZone) != tech.Zones.end();
			const double ownDist = pEngineer->GetMapCoords().DistanceFrom(tech.Cell);
			const double otherDist = pOther->GetMapCoords().DistanceFrom(tech.Cell);

			if (otherOnTheGround && otherDist <= 2.0 * ownDist + 10.0)
				return false;
		}

		return true;
	}

	// Best known tech for this engineer: worth over distance, reachable, out of known enemy defense
	// reach, not blocked by recent failures nor taken by another engineer. Neutral techs, and techs
	// an enemy took (worth double when they pay: we gain the cash and the enemy loses it). A tech
	// that lets us build around it (BaseNormal) on another landmass than ours counts 2 more: it
	// opens an expansion without an MCV. outContested: enemies were seen near it (an escort should
	// go first). outBreach: the wall piece to knock down first when it is walled in (a tech whose
	// walls no single breach opens is skipped), on our ground or across the water. walledOnly: only
	// walled-in techs, which the micro's own capture leaves alone (an engineer alone would wait at
	// the wall).
	BuildingClass* PickSpecOpsCaptureTarget(HouseClass* pHouse, FootClass* pEngineer, bool& outContested, NeuralTechBreach& outBreach,
		bool walledOnly)
	{
		outContested = false;
		outBreach = NeuralTechBreach {};
		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const int engineerZone = ObjectZone(pEngineer, MovementZone::Normal);
		const bool ferryBlocked = IsFerryBlocked(pHouse, pEngineer->UniqueID);
		const CellStruct from = pEngineer->GetMapCoords();
		BuildingClass* pBest = nullptr;
		int bestScore = 0;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Type->Capturable || !pBld->Owner
				|| pBld->Owner == pHouse || (!pBld->Owner->IsNeutral() && pHouse->IsAlliedWith(pBld->Owner)))
			{
				continue;
			}

			auto const pTech = FindKnownTech(state, pBld->UniqueID);
			const bool enemyHeld = pTech && pTech->OwnerIndex >= 0 && pTech->OwnerIndex != pHouse->ArrayIndex
				&& !pBld->Owner->IsNeutral();

			if (pTech && pTech->OwnerIndex < 0 && IsTechClaimedByAlly(pHouse, pTech->ID))
			{
				CountDecision(NeuralDecision::AllyTechYield, pHouse->ArrayIndex);
				continue;
			}

			if (!pTech || (pTech->OwnerIndex >= 0 && !enemyHeld) || now < pTech->BlockedUntilFrame
				|| (IsSpecOpsCaptureTarget(pHouse, pTech->ID, pEngineer->UniqueID)
					&& !IsBetterPlacedForTech(pHouse, *pTech, pEngineer, engineerZone))
				|| IsCellInKnownEnemyDefenseReach(pHouse, pTech->Cell, 2))
			{
				continue;
			}

			// No ferry for this engineer for a while (it gave up waiting for one): only what it can
			// walk to.
			if (ferryBlocked && std::find(pTech->Zones.begin(), pTech->Zones.end(), engineerZone) == pTech->Zones.end())
				continue;

			if (walledOnly && !pTech->Walled)
				continue;

			NeuralTechBreach breach;
			const NeuralTechWall wall = GetTechWallStateFrom(pHouse, pBld, engineerZone, from, breach);

			if (walledOnly && wall != NeuralTechWall::Breachable)
				continue;

			// A breach from another land: the engineer and its wall breakers go by transport.
			if (wall == NeuralTechWall::Breachable && breach.Zone != engineerZone
				&& (ferryBlocked || !CanHouseFerryTypeAcrossWater(pHouse, pEngineer->GetTechnoType())))
			{
				continue;
			}

			// Walled in as seen from home but open as seen from here: this engineer stands on
			// other ground and would only reach the outside of the wall (by ferry), then wait there.
			if (wall == NeuralTechWall::Open && pTech->Walled)
			{
				static int skipLogSerial = -1;
				static int skipLogCount = 0;

				if (skipLogSerial != NeuralMatchSerial)
				{
					skipLogSerial = NeuralMatchSerial;
					skipLogCount = 0;
				}

				if (skipLogCount++ < 5)
				{
					const CellStruct home = GetHouseHomeCell(pHouse);
					Debug::Log("[NeuralAI] House %d SpecOps: breach: engineer %u at (%d,%d) zone %d skips walled-in %s at (%d,%d) (home zone %d, tech zones %d %d %d %d), frame %d.\n",
						pHouse->ArrayIndex, pEngineer->UniqueID, from.X, from.Y, engineerZone, pBld->Type->ID, pTech->Cell.X, pTech->Cell.Y,
						home != CellStruct::Empty ? MapClass::Instance.GetMovementZoneType(home, MovementZone::Normal, false) : -1,
						pTech->Zones[0], pTech->Zones[1], pTech->Zones[2], pTech->Zones[3], now);
				}

				continue;
			}

			if (wall == NeuralTechWall::Sealed
				|| (wall == NeuralTechWall::Open && !CanEngineerReachTech(pHouse, *pTech, engineerZone,
					abstract_cast<InfantryClass*>(pEngineer) ? abstract_cast<InfantryClass*>(pEngineer)->Type : nullptr)))
			{
				continue;
			}

			const int threat = VisibleEnemyThreatNear(pHouse, pTech->Cell, 7);
			// After a loss there (or next to it) the next engineer goes with an escort.
			const bool contested = threat > 0 || pTech->Failures > 0;

			// A tech held by a visible force much bigger than an escort could answer is not worth a walk.
			if (threat > 2400)
				continue;

			const int dist = static_cast<int>(from.DistanceFrom(pTech->Cell));
			const bool opensExpansion = pBld->Type->BaseNormal && engineerZone >= 0
				&& std::find(pTech->Zones.begin(), pTech->Zones.end(), engineerZone) == pTech->Zones.end();
			const int worth = (pTech->Value + (opensExpansion ? 2 : 0)) * ((enemyHeld && pTech->Cash) ? 2 : 1);
			const int score = worth * 1000 / (dist + 12) - (contested ? 15 : 0)
				- (wall == NeuralTechWall::Breachable ? 10 : 0);

			if (score > bestScore)
			{
				bestScore = score;
				pBest = pBld;
				outContested = contested;
				outBreach = breach;
			}
		}

		return pBest;
	}

	// Idle armed ground units and soldiers of ours near a tech, nearest first (UniqueID on ties),
	// not in a plan nor escorting or breaching for another capture (nor for pPending, the capture
	// being set up). Specialists (engineers, spies, thieves) and harvesters never escort. With
	// pBreachWall, only units that can knock that wall down count, from a little further away
	// (without them the engineer cannot get in at all): those standing in breachZone (Normal), then
	// those in ferryZone a transport of ours can take there, then the ones that crush it rather
	// than shoot it.
	void CollectSpecOpsEscorts(HouseClass* pHouse, const CellStruct& cell, int maxCount, std::vector<FootClass*>& out,
		const NeuralSpecOpsCapture* pPending = nullptr, const OverlayTypeClass* pBreachWall = nullptr, int breachZone = -1,
		int ferryZone = -1)
	{
		out.clear();
		auto const& state = GetSpecOpsState(pHouse);
		std::vector<std::pair<int, FootClass*>> candidates;

		auto isTaken = [&](const NeuralSpecOpsCapture& capture, DWORD id)
		{
			return std::find(capture.Escorts.begin(), capture.Escorts.end(), id) != capture.Escorts.end()
				|| std::find(capture.Breachers.begin(), capture.Breachers.end(), id) != capture.Breachers.end();
		};

		auto consider = [&](FootClass* pFoot)
		{
			// Guarding, or just repositioning (a move with no target, out of any plan, healthy):
			// units entering, rescuing or retreating to repair keep their job.
			const bool repositioning = pFoot->CurrentMission == Mission::Move && pFoot->GetHealthPercentage() >= 0.6;

			if (pFoot->Target || IsPlanMember(pHouse, pFoot->UniqueID)
				|| (pFoot->CurrentMission != Mission::Guard && pFoot->CurrentMission != Mission::Area_Guard && !repositioning))
			{
				return;
			}

			int rank = 0;

			if (pBreachWall)
			{
				if (!CanBreachWall(pFoot, pBreachWall))
					return;

				const int footZone = ObjectZone(pFoot, MovementZone::Normal);

				if (footZone != breachZone)
				{
					if (ferryZone < 0 || footZone != ferryZone || IsInTransportOp(pHouse, pFoot->UniqueID)
						|| !CanHouseFerryTypeAcrossWater(pHouse, pFoot->GetTechnoType()))
					{
						return;
					}

					rank += 1000;
				}

				if (!CanShootWall(pFoot, pBreachWall))
					rank += 500;
			}

			if (pPending && isTaken(*pPending, pFoot->UniqueID))
				return;

			for (auto const& capture : state.Captures)
			{
				if (isTaken(capture, pFoot->UniqueID))
					return;
			}

			const int dist = static_cast<int>(pFoot->GetMapCoords().DistanceFrom(cell));

			if (dist <= (pBreachWall ? 90 : 45))
				candidates.emplace_back(dist + rank, pFoot);
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type && !pUnit->Type->Naval
				&& !pUnit->Type->Harvester && !pUnit->Type->ResourceGatherer && !pUnit->Type->DeploysInto
				&& GroundRangeOf(pUnit->Type) > 0)
			{
				consider(pUnit);
			}
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->Owner == pHouse && pInf->IsAlive && !pInf->InLimbo && pInf->Type && !pInf->Type->Engineer
				&& !IsSpyInfantryType(pInf->Type) && !pInf->Type->Agent && !pInf->Type->Thief && !pInf->Type->VehicleThief
				&& !pInf->Type->Slaved && GroundRangeOf(pInf->Type) > 0)
			{
				consider(pInf);
			}
		}

		std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b)
		{
			return a.first != b.first ? a.first < b.first : a.second->UniqueID < b.second->UniqueID;
		});

		for (auto const& [dist, pFoot] : candidates)
		{
			if (static_cast<int>(out.size()) >= maxCount)
				break;

			out.push_back(pFoot);
		}
	}

	// Breakers still shooting the cell of a wall piece that is gone, or of an op that ended, would
	// keep firing at bare ground: they go back to guard.
	void ReleaseSpecOpsBreachers(HouseClass* pHouse, const NeuralSpecOpsCapture& capture)
	{
		if (capture.BreachCell == CellStruct::Empty)
			return;

		auto const pWallCell = MapClass::Instance.TryGetCellAt(capture.BreachCell);

		for (const DWORD id : capture.Breachers)
		{
			auto const pFoot = FindOwnFootByID(pHouse, id);

			if (pFoot && pWallCell && pFoot->Target == pWallCell)
			{
				pFoot->SetTarget(nullptr);
				pFoot->QueueMission(Mission::Guard, true);
			}
		}
	}

	// Keeps the breach into a walled-in tech going, every AI tick (the engineer may be walking and
	// out of the micro's reach): the breakers shoot the wall piece and, once it is down, either the
	// way in is open and the op goes on as any other or the next piece is taken. The engine
	// recomputes the movement zones when a wall piece falls. Returns false when the breach failed.
	bool UpdateSpecOpsBreach(HouseClass* pHouse, NeuralSpecOpsState& state, NeuralSpecOpsCapture& capture, BuildingClass* pTarget, int now)
	{
		auto fail = [&](const char* reason)
		{
			FailSpecOpsCapture(state, capture.TargetID, 1800);

			if (CanLogSpecOpsBreach())
			{
				Debug::Log("[NeuralAI] House %d SpecOps: breach into %s at (%d,%d) failed (%s) after %d frames, %d wall pieces down, frame %d.\n",
					pHouse->ArrayIndex, pTarget->Type->ID, capture.TargetCell.X, capture.TargetCell.Y, reason, now - capture.BreachFrame,
					capture.BreachPieces, now);
			}

			return false;
		};

		const OverlayTypeClass* pWall = GetWallOverlayAt(capture.BreachCell);

		if (!pWall)
		{
			ReleaseSpecOpsBreachers(pHouse, capture);
			capture.BreachPieces++;
			NeuralTechBreach next;
			const NeuralTechWall wall = GetTechWallState(pHouse, pTarget, capture.BreachZone, capture.BreachApproach, next);

			if (wall == NeuralTechWall::Open)
			{
				if (CanLogSpecOpsBreach())
				{
					Debug::Log("[NeuralAI] House %d SpecOps: breach into %s at (%d,%d) open, wall piece (%d,%d) down after %d frames (%d pieces), frame %d.\n",
						pHouse->ArrayIndex, pTarget->Type->ID, capture.TargetCell.X, capture.TargetCell.Y, capture.BreachCell.X,
						capture.BreachCell.Y, now - capture.BreachFrame, capture.BreachPieces, now);
				}

				capture.BreachCell = CellStruct::Empty;

				return true;
			}

			if (wall == NeuralTechWall::Sealed)
				return fail("sealed");

			if (capture.BreachPieces >= 4)
				return fail("too many pieces");

			capture.BreachCell = next.Wall;
			capture.BreachApproach = next.Approach;
			pWall = GetWallOverlayAt(next.Wall);
		}

		// Across the water the breach's time runs from the first breaker on the land of the wall; the
		// crossing itself has 9000 frames.
		if (capture.BreachFerryZone >= 0 && !capture.BreachLanded)
		{
			for (const DWORD id : capture.Breachers)
			{
				auto const pFoot = FindOwnFootByID(pHouse, id);

				if (pFoot && !pFoot->InLimbo && ObjectZone(pFoot, MovementZone::Normal) == capture.BreachZone)
				{
					capture.BreachLanded = true;
					capture.BreachFrame = now;

					if (CanLogSpecOpsBreach())
					{
						Debug::Log("[NeuralAI] House %d SpecOps: breach into %s at (%d,%d): wall breaker %s %u stands by the wall piece's land, "
							"frame %d.\n", pHouse->ArrayIndex, pTarget->Type->ID, capture.TargetCell.X, capture.TargetCell.Y,
							pFoot->GetTechnoType()->ID, pFoot->UniqueID, now);
					}

					break;
				}
			}

			if (!capture.BreachLanded && now - capture.BreachFrame > 9000)
				return fail("no wall breaker landed");
		}
		else if (now - capture.BreachFrame > 3600)
		{
			return fail("too long");
		}

		auto const pWallCell = MapClass::Instance.TryGetCellAt(capture.BreachCell);
		int breakers = 0;
		int ferryRefused = 0;

		for (const DWORD id : capture.Breachers)
		{
			auto const pFoot = FindOwnFootByID(pHouse, id);

			if (!pFoot || !CanBreachWall(pFoot, pWall))
				continue;

			// Waiting for its transport or aboard: on its way.
			if (IsInTransportOp(pHouse, id))
			{
				breakers++;
				continue;
			}

			if (pFoot->InLimbo)
				continue;

			// Still on the engineer's land: a transport takes it to our side of the wall.
			if (capture.BreachFerryZone >= 0 && ObjectZone(pFoot, MovementZone::Normal) != capture.BreachZone)
			{
				if (RequestTransportOperation(pHouse, NeuralOpClient::Economy, pFoot, capture.BreachApproach))
					breakers++;
				else
					ferryRefused++;

				continue;
			}

			breakers++;

			// A breaker fighting enemies finishes that first; one that dropped the order gets it back.
			if (pFoot->Target == pWallCell || (pFoot->Target && pFoot->Target->WhatAmI() != AbstractType::Cell))
				continue;

			if (CanShootWall(pFoot, pWall))
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetTarget(pWallCell);
				pFoot->QueueMission(Mission::Attack, true);
			}
			else if (pFoot->Destination != pWallCell)
			{
				// A crusher drives over the piece.
				pFoot->ClearMegaMissionData();
				pFoot->SetDestination(pWallCell, true);
				pFoot->QueueMission(Mission::Move, true);
				NoteOrderCaller(pFoot, "breach");
			}
		}

		// The breakers on the engineer's land got no transport: the next ones would get none either.
		if (breakers == 0 && ferryRefused > 0)
			return fail("no transport for the wall breakers");

		if (breakers == 0)
		{
			std::vector<FootClass*> fresh;
			CollectSpecOpsEscorts(pHouse, capture.BreachCell, 2, fresh, nullptr, pWall, capture.BreachZone, capture.BreachFerryZone);

			if (fresh.empty())
				return fail("no wall breaker left");

			capture.Breachers.clear();

			for (auto const pFoot : fresh)
				capture.Breachers.push_back(pFoot->UniqueID);
		}
		else if (!capture.BreachReinforced && now - capture.BreachFrame > 1500)
		{
			// Still standing well into the breach (a breaker that cannot get a shot on it): one
			// more breaker joins, once.
			capture.BreachReinforced = true;
			std::vector<FootClass*> fresh;
			CollectSpecOpsEscorts(pHouse, capture.BreachCell, 1, fresh, nullptr, pWall, capture.BreachZone, capture.BreachFerryZone);

			for (auto const pFoot : fresh)
				capture.Breachers.push_back(pFoot->UniqueID);
		}

		return true;
	}

	// Expert 4 engineer: take (or keep) a capture op, send the escort first when enemies were
	// seen near the tech and walk in once the way is clear. Returns true when the engineer got
	// its job for this tick (wait or go). dispatch is the micro's order helper (ferries included).
	template <typename DispatchFn>
	bool RunSpecOpsEngineer(HouseClass* pHouse, FootClass* pEngineer, bool expertActive, DispatchFn&& dispatch)
	{
		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;
		NeuralSpecOpsCapture* pCapture = nullptr;

		for (auto& capture : state.Captures)
		{
			if (capture.EngineerID == pEngineer->UniqueID)
			{
				pCapture = &capture;
				break;
			}
		}

		BuildingClass* pTarget = pCapture ? FindBuildingByID(pCapture->TargetID) : nullptr;

		// An engineer that gave up waiting for a ferry cannot get to a tech across the water: that
		// capture ends and it picks a tech it can walk to, instead of standing there with it.
		if (pCapture && IsFerryBlocked(pHouse, pEngineer->UniqueID))
		{
			auto const pTech = FindKnownTech(state, pCapture->TargetID);
			const int engineerZone = ObjectZone(pEngineer, MovementZone::Normal);

			if (!pTech || std::find(pTech->Zones.begin(), pTech->Zones.end(), engineerZone) == pTech->Zones.end())
			{
				Debug::Log("[NeuralAI] House %d SpecOps: engineer %u without a ferry drops the tech at (%d,%d), frame %d.\n",
					pHouse->ArrayIndex, pEngineer->UniqueID, pCapture->TargetCell.X, pCapture->TargetCell.Y, now);
				state.Captures.erase(state.Captures.begin() + (pCapture - state.Captures.data()));
				pCapture = nullptr;
				pTarget = nullptr;
			}
		}

		// Without Expert 4 only walled-in techs: the micro's own capture leaves those alone.
		if (!pCapture)
		{
			bool contested = false;
			NeuralTechBreach breach;
			pTarget = PickSpecOpsCaptureTarget(pHouse, pEngineer, contested, breach, !expertActive);

			if (!pTarget)
				return false;

			// The tech of an engineer worse placed for it goes to this one.
			for (size_t i = state.Captures.size(); i-- > 0;)
			{
				if (state.Captures[i].TargetID != pTarget->UniqueID)
					continue;

				Debug::Log("[NeuralAI] House %d SpecOps: engineer %u on the spot takes %s at (%d,%d) from engineer %u, frame %d.\n",
					pHouse->ArrayIndex, pEngineer->UniqueID, pTarget->Type->ID, pTarget->GetMapCoords().X, pTarget->GetMapCoords().Y,
					state.Captures[i].EngineerID, now);
				state.Captures.erase(state.Captures.begin() + static_cast<std::ptrdiff_t>(i));
			}

			NeuralSpecOpsCapture capture;
			capture.EngineerID = pEngineer->UniqueID;
			capture.TargetID = pTarget->UniqueID;
			capture.TargetCell = pTarget->GetMapCoords();
			capture.StartFrame = now;

			if (breach.Wall != CellStruct::Empty)
			{
				// A walled-in tech needs a wall breaker of ours on the outer side of the wall, or one
				// on the engineer's land a transport takes there with it (user, 2026-11-28: a wall
				// breaker goes along and opens the way); without one that tech waits and another one is
				// picked next tick.
				const int engineerZone = ObjectZone(pEngineer, MovementZone::Normal);
				const int zone = breach.Zone >= 0 ? breach.Zone : engineerZone;
				std::vector<FootClass*> breachers;
				CollectSpecOpsEscorts(pHouse, breach.Wall, 2, breachers, nullptr, GetWallOverlayAt(breach.Wall), zone,
					zone != engineerZone ? engineerZone : -1);

				if (breachers.empty())
				{
					if (auto const pTech = FindKnownTech(state, capture.TargetID))
						pTech->BlockedUntilFrame = std::max(pTech->BlockedUntilFrame, now + 900);

					if (CanLogSpecOpsBreach())
					{
						Debug::Log("[NeuralAI] House %d SpecOps: breach into %s at (%d,%d) waits, no wall breaker near the wall piece (%d,%d), frame %d.\n",
							pHouse->ArrayIndex, pTarget->Type->ID, capture.TargetCell.X, capture.TargetCell.Y, breach.Wall.X, breach.Wall.Y, now);
					}

					return false;
				}

				capture.BreachCell = breach.Wall;
				capture.BreachApproach = breach.Approach;
				capture.BreachZone = zone;
				capture.BreachFrame = now;
				capture.BreachFerryZone = zone != engineerZone ? engineerZone : -1;

				for (auto const pBreacher : breachers)
					capture.Breachers.push_back(pBreacher->UniqueID);
			}

			if (contested)
			{
				std::vector<FootClass*> escorts;
				CollectSpecOpsEscorts(pHouse, capture.TargetCell, 3, escorts, &capture);

				// No engineer walks alone where enemies were seen or engineers were lost: with
				// nobody free to escort it, that tech waits and another one is picked next tick.
				if (escorts.empty())
				{
					if (auto const pTech = FindKnownTech(state, capture.TargetID))
						pTech->BlockedUntilFrame = std::max(pTech->BlockedUntilFrame, now + 900);

					return false;
				}

				auto const pEscortGoal = MapClass::Instance.TryGetCellAt(capture.TargetCell);

				for (auto const pEscort : escorts)
				{
					capture.Escorts.push_back(pEscort->UniqueID);

					if (pEscortGoal)
						dispatch(pEscort, pEscortGoal, Mission::AttackMove, false);
				}
			}

			state.Captures.push_back(capture);
			pCapture = &state.Captures.back();
			Debug::Log("[NeuralAI] House %d SpecOps: engineer %u -> %s at (%d,%d)%s, %d escorts, frame %d.\n",
				pHouse->ArrayIndex, capture.EngineerID, pTarget->Type->ID, capture.TargetCell.X, capture.TargetCell.Y,
				contested ? " (contested)" : "", static_cast<int>(capture.Escorts.size()), now);

			if (capture.BreachCell != CellStruct::Empty && CanLogSpecOpsBreach())
			{
				auto const pBreacher = FindOwnFootByID(pHouse, capture.Breachers[0]);
				Debug::Log("[NeuralAI] House %d SpecOps: breach into %s at (%d,%d) by %d wall breakers (%s), wall piece (%d,%d), engineer %u, frame %d.\n",
					pHouse->ArrayIndex, pTarget->Type->ID, capture.TargetCell.X, capture.TargetCell.Y, static_cast<int>(capture.Breachers.size()),
					pBreacher && pBreacher->GetTechnoType() ? pBreacher->GetTechnoType()->ID : "?", capture.BreachCell.X, capture.BreachCell.Y,
					capture.EngineerID, now);
			}
		}
		else if (!pTarget || pCapture->Dispatched)
		{
			// Upkeep closes ops on lost targets; a dispatched engineer back on guard tries again.
			if (!pTarget)
				return false;

			pCapture->Dispatched = false;
		}

		// Walled in: once a breaker is close to the wall piece, the engineer waits a few cells back
		// from it (out of the splash of the shots); the upkeep clears the breach when the way in
		// is open and the engineer then walks in as on any other capture.
		if (pCapture->BreachCell != CellStruct::Empty)
		{
			// Across the water: the engineer goes over to our side of the wall by transport, with its
			// breakers when they board on the same beach.
			if (pCapture->BreachFerryZone >= 0 && !pEngineer->InLimbo
				&& ObjectZone(pEngineer, MovementZone::Normal) != pCapture->BreachZone)
			{
				auto const pApproachCell = MapClass::Instance.TryGetCellAt(pCapture->BreachApproach);

				if (!pApproachCell || !dispatch(pEngineer, pApproachCell, Mission::Move, false))
				{
					FailSpecOpsCapture(state, pCapture->TargetID, 1800);

					if (CanLogSpecOpsBreach())
					{
						Debug::Log("[NeuralAI] House %d SpecOps: breach into %s at (%d,%d) failed (no transport for engineer %u), frame %d.\n",
							pHouse->ArrayIndex, pTarget->Type->ID, pCapture->TargetCell.X, pCapture->TargetCell.Y, pEngineer->UniqueID, now);
					}

					ReleaseSpecOpsBreachers(pHouse, *pCapture);
					state.Captures.erase(state.Captures.begin() + (pCapture - state.Captures.data()));

					return false;
				}

				return true;
			}

			bool breakerNear = false;

			for (const DWORD id : pCapture->Breachers)
			{
				auto const pBreacher = FindOwnFootByID(pHouse, id);

				if (pBreacher && !pBreacher->InLimbo && pBreacher->GetMapCoords().DistanceFrom(pCapture->BreachCell) <= 10.0)
					breakerNear = true;
			}

			if (breakerNear)
			{
				const CellStruct wallCell = pCapture->BreachCell;
				const CellStruct approach = pCapture->BreachApproach;
				const CellStruct back { static_cast<short>(approach.X + 2 * (approach.X - wallCell.X)),
					static_cast<short>(approach.Y + 2 * (approach.Y - wallCell.Y)) };
				const bool backFree = MapClass::Instance.CoordinatesLegal(back) && !GetWallOverlayAt(back)
					&& MapClass::Instance.GetMovementZoneType(back, MovementZone::Normal, false) == pCapture->BreachZone;
				const CellStruct stand = backFree ? back : approach;

				if (pEngineer->GetMapCoords().DistanceFrom(stand) > 2.0)
				{
					if (auto const pStandCell = MapClass::Instance.TryGetCellAt(stand))
						IssueMoveOrder(pEngineer, pStandCell, 2.0);
				}
			}

			return true;
		}

		auto const pTargetCell = MapClass::Instance.TryGetCellAt(pCapture->TargetCell);
		int escortsAlive = 0;
		bool escortArrived = false;

		for (const DWORD id : pCapture->Escorts)
		{
			auto const pEscort = FindOwnFootByID(pHouse, id);

			if (!pEscort)
				continue;

			escortsAlive++;

			if (pEscort->GetMapCoords().DistanceFrom(pCapture->TargetCell) <= 6.0)
				escortArrived = true;
			else if (pTargetCell && !pEscort->Target && (pEscort->CurrentMission == Mission::Guard || pEscort->CurrentMission == Mission::Area_Guard))
				dispatch(pEscort, pTargetCell, Mission::AttackMove, false);
		}

		const int threat = VisibleEnemyThreatNear(pHouse, pCapture->TargetCell, 7);
		const bool waitForEscort = escortsAlive > 0 && !escortArrived && now - pCapture->StartFrame < 1800;
		const bool go = (threat == 0 && !waitForEscort) || escortArrived
			|| (escortsAlive == 0 && now - pCapture->StartFrame >= 900 && threat < 600);

		if (go)
		{
			pCapture->Dispatched = dispatch(pEngineer, pTarget, Mission::Enter, false);
			pCapture->DispatchFrame = now;

			if (!pCapture->Dispatched)
				FailSpecOpsCapture(state, pCapture->TargetID, 1800);
		}
		else if (escortsAlive == 0 && now - pCapture->StartFrame >= 900)
		{
			// No escort and too many enemies there: try another tech later.
			FailSpecOpsCapture(state, pCapture->TargetID, 1800);
			state.Captures.erase(state.Captures.begin() + (pCapture - state.Captures.data()));

			return false;
		}

		return true;
	}

	// The blast a suicide raider sets off: the Phobos FeedbackWeapon= that blows up the firer,
	// a Suicide= primary weapon, or the DeathWeapon= of a vehicle that Explodes=.
	WeaponTypeClass* GetSuicideBlastWeapon(TechnoTypeClass* pType)
	{
		auto const pSlot = pType ? pType->GetWeapon(0) : nullptr;
		auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

		if (pWeapon)
		{
			if (auto const pWeaponExt = WeaponTypeExt::TryFetch(pWeapon); pWeaponExt && IsAreaWeapon(pWeaponExt->FeedbackWeapon.Get()))
				return pWeaponExt->FeedbackWeapon.Get();

			if (pWeapon->Suicide)
				return pWeapon;
		}

		if (pType && pType->Explodes && IsAreaWeapon(pType->DeathWeapon))
			return pType->DeathWeapon;

		return pWeapon;
	}

	bool RequestTransportOperation(HouseClass* pHouse, NeuralOpClient client, FootClass* pFoot, const CellStruct& target);

	// A fast flying or subterranean raid transport of ours that can carry a raider of this size:
	// the raider waits briefly for AirborneFront instead of walking; surface hovercraft never delay land strikes.
	bool HasRaidTransportFor(HouseClass* pHouse, double size)
	{
		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->WhatAmI() == AbstractType::Infantry)
				continue;

			auto const pType = pFoot->GetTechnoType();

			if (!pType || pType->Passengers <= 0 || size > pType->SizeLimit)
				continue;

			auto const pUnitType = abstract_cast<UnitTypeClass*>(pType);
			const bool flies = pFoot->WhatAmI() == AbstractType::Aircraft || pType->JumpJet || pType->ConsideredAircraft
				|| pType->MovementZone == MovementZone::Fly;

			if (flies || IsUndergroundTransportType(pUnitType))
				return true;
		}

		return false;
	}

	struct SneakAssignedTarget
	{
		DWORD RaiderID { 0 };
		DWORD TargetID { 0 };
		CellStruct Coords { CellStruct::Empty };
		bool WideBlast { false };
	};

	bool IsWideSuicideRaider(TechnoTypeClass* pType)
	{
		auto const pBlast = GetSuicideBlastWeapon(pType);

		if (!pBlast)
			return false;

		return pBlast->RadLevel >= 100 || (pBlast->Warhead && pBlast->Warhead->CellSpread >= 5.0f);
	}

	// Where a suicide raider (DTRUCK, TERROR) deals the highest net blast + radiation damage:
	// evaluates both known enemy building clusters and dense visible enemy ground-force clusters
	// within the warhead's CellSpread, spreads active raiders across distinct clusters, commits
	// strongly to nearby targets when already inside an enemy base, and ignores AA-only defenses.
	TechnoClass* PickSneakStrikeTarget(HouseClass* pHouse, FootClass* pRaider,
		const std::vector<SneakAssignedTarget>& assigned, DWORD skipTargetId = 0, bool crossIsland = false)
	{
		auto const pType = pRaider->GetTechnoType();
		auto const pBlast = GetSuicideBlastWeapon(pType);
		const double rawSpread = (pBlast && pBlast->Warhead && pBlast->Warhead->CellSpread > 0.0)
			? static_cast<double>(pBlast->Warhead->CellSpread)
			: 2.0;
		const double spread = std::clamp(rawSpread, 2.0, 9.0) + 1.0;
		const bool radBlast = pBlast && pBlast->RadLevel >= 100;
		const bool wideBlast = rawSpread >= 5.0 || radBlast;
		const DWORD selfId = pRaider->UniqueID;
		const CellStruct from = pRaider->GetMapCoords();
		auto const& entries = GetKnownEnemyBuildingCache(pHouse).Entries;

		struct VisibleGroundUnit
		{
			FootClass* Foot;
			CellStruct Coords;
			int Worth;
			bool Friendly;
		};

		std::vector<VisibleGroundUnit> groundUnits;
		groundUnits.reserve(96);

		auto collectFoot = [&](FootClass* pF)
		{
			if (!pF || pF == pRaider || pF->InLimbo || !pF->IsAlive || !pF->Owner || pF->Owner->IsNeutral() || pF->IsInAir())
				return;

			auto const pFT = pF->GetTechnoType();

			if (!pFT || pFT->Naval)
				return;

			const bool friendly = pF->Owner == pHouse || pHouse->IsAlliedWith(pF->Owner);

			if (!friendly && (!pF->GetCell() || !IsCellVisibleToAI(pHouse, pF->GetCell())))
				return;

			const int w = std::max(3, pFT->Cost / (radBlast ? 45 : 65));
			groundUnits.push_back({ pF, pF->GetMapCoords(), w, friendly });
		};

		for (auto const pU : UnitClass::Array)
			collectFoot(pU);

		for (auto const pI : InfantryClass::Array)
			collectFoot(pI);

		auto friendlyBuildingNear = [&](const CellStruct& center)
		{
			for (auto const pOwnB : pHouse->Buildings)
			{
				if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->GetMapCoords().DistanceFrom(center) <= spread + 1.0)
					return true;
			}

			return false;
		};

		auto groundDefensesNear = [&](const CellStruct& center, const BuildingClass* pSkip)
		{
			int defs = 0;

			for (auto const& other : entries)
			{
				if (!other.IsDefense || other.Building == pSkip || !other.Building || !other.Building->Type)
					continue;

				if (GroundRangeOf(other.Building->Type) <= 0)
					continue;

				if (other.Coords.DistanceFrom(center) <= 8.0)
					defs++;
			}

			return defs;
		};

		auto assignedOverlapPenalty = [&](const CellStruct& center, DWORD candidateId)
		{
			int sameTarget = 0;
			int wideOverlap = 0;
			int closeSmall = 0;

			for (auto const& a : assigned)
			{
				if (a.RaiderID == selfId || a.TargetID == 0)
					continue;

				if (a.TargetID == candidateId)
					sameTarget++;

				const double d = a.Coords.DistanceFrom(center);

				if (a.WideBlast && d <= spread)
					wideOverlap++;
				else if (!a.WideBlast && d <= 4.0)
					closeSmall++;
			}

			if (wideBlast)
				return sameTarget * 260 + wideOverlap * 180 + closeSmall * 45;

			return wideOverlap * 160 + std::max(0, sameTarget - 1) * 90 + std::max(0, closeSmall - 2) * 55;
		};

		TechnoClass* pBest = nullptr;
		int bestScore = INT_MIN;

		for (auto const& info : entries)
		{
			auto const pBld = info.Building;

			if (!pBld || pBld->UniqueID == skipTargetId || !pBld->IsAlive || pBld->InLimbo || !pBld->Owner
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Type->Insignificant || pBld->Type->InvisibleInGame || pBld->Type->Wall)
			{
				continue;
			}

			const bool sameLand = AreZonesConnected(from, info.Coords, pType->MovementZone);

			if (sameLand == crossIsland)
				continue;

			if (friendlyBuildingNear(info.Coords))
				continue;

			int worth = 0;

			for (auto const& other : entries)
			{
				if (!other.Building || !other.Building->Type || other.Coords.DistanceFrom(info.Coords) > spread)
					continue;

				auto const pOT = other.Building->Type;
				worth += std::max(4, pOT->Cost / 50);

				if (pOT->ConstructionYard || pOT->BuildCat == BuildCat::Tech)
					worth += 24;
				else if (pOT->WeaponsFactory || pOT->Factory != AbstractType::None || pOT->Refinery)
					worth += 16;
			}

			int friendlyLoss = 0;

			for (auto const& gu : groundUnits)
			{
				if (gu.Coords.DistanceFrom(info.Coords) <= spread)
				{
					if (gu.Friendly)
						friendlyLoss += gu.Worth * 3;
					else
						worth += gu.Worth;
				}
			}

			if (worth <= friendlyLoss || worth < 8)
				continue;

			const double dist = from.DistanceFrom(info.Coords);
			const int proximityBonus = dist <= 36.0 ? static_cast<int>(36.0 - dist) * 4 : 0;
			const int defenses = groundDefensesNear(info.Coords, pBld);
			const int overlapPenalty = assignedOverlapPenalty(info.Coords, pBld->UniqueID);
			const int score = (worth - friendlyLoss) + proximityBonus - defenses * 8 - static_cast<int>(dist) / 5 - overlapPenalty;

			if (score > bestScore || (score == bestScore && pBest && pBld->UniqueID < pBest->UniqueID))
			{
				bestScore = score;
				pBest = pBld;
			}
		}

		// Also evaluate dense visible enemy ground-force clusters (e.g. massed enemy armor/infantry):
		if (!crossIsland)
		{
			for (auto const& anchor : groundUnits)
			{
				if (anchor.Friendly || anchor.Foot->UniqueID == skipTargetId
					|| !AreZonesConnected(from, anchor.Coords, pType->MovementZone))
				{
					continue;
				}

				if (friendlyBuildingNear(anchor.Coords))
					continue;

				int enemyClusterWorth = 0;
				int enemyCount = 0;
				int friendlyLoss = 0;

				for (auto const& gu : groundUnits)
				{
					if (gu.Coords.DistanceFrom(anchor.Coords) > spread)
						continue;

					if (gu.Friendly)
					{
						friendlyLoss += gu.Worth * 3;
					}
					else
					{
						enemyClusterWorth += gu.Worth;
						enemyCount++;
					}
				}

				if (enemyCount < 3 || enemyClusterWorth < 36 || enemyClusterWorth <= friendlyLoss * 2)
					continue;

				const double dist = from.DistanceFrom(anchor.Coords);
				const int proximityBonus = dist <= 36.0 ? static_cast<int>(36.0 - dist) * 4 : 0;
				const int defenses = groundDefensesNear(anchor.Coords, nullptr);
				const int overlapPenalty = assignedOverlapPenalty(anchor.Coords, anchor.Foot->UniqueID);
				const int score = (enemyClusterWorth - friendlyLoss) + proximityBonus - defenses * 8 - static_cast<int>(dist) / 5 - overlapPenalty;

				if (score > bestScore || (score == bestScore && pBest && anchor.Foot->UniqueID < pBest->UniqueID))
				{
					bestScore = score;
					pBest = anchor.Foot;
				}
			}
		}

		return pBest;
	}

	// A suicide raider of ours is sneaking to (or charging) a target, or briefly holding at home for a raid transport:
	// the micro leaves it alone.
	bool IsSneakStrikeActive(HouseClass* pHouse, DWORD id)
	{
		auto const& strikes = GetSpecOpsState(pHouse).SneakStrikes;
		auto const it = strikes.find(id);

		return it != strikes.end() && (it->second.Target != 0 || it->second.WaitingForRide);
	}

	// Suicide raiders without a ride (demolition trucks, terrorists): fragile and only useful once
	// they blow up among buildings or massed enemy forces, so they never join regular army waves.
	void UpdateSneakStrikes(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - state.LastSneakFrame < 60 && now >= state.LastSneakFrame)
			return;

		state.LastSneakFrame = now;
		const NeuralOrderPriorityScope orderPriority { NeuralOrderPriority::Operation };
		auto& strikes = state.SneakStrikes;
		std::unordered_set<DWORD> seen;

		auto findTarget = [pHouse](DWORD id) -> TechnoClass*
		{
			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				if (info.Building && info.Building->UniqueID == id)
					return (info.Building->IsAlive && !info.Building->InLimbo) ? info.Building : nullptr;
			}

			for (auto const pU : UnitClass::Array)
			{
				if (pU && pU->UniqueID == id && pU->IsAlive && !pU->InLimbo && pU->Owner && !pHouse->IsAlliedWith(pU->Owner)
					&& pU->GetCell() && IsCellVisibleToAI(pHouse, pU->GetCell()))
				{
					return pU;
				}
			}

			for (auto const pI : InfantryClass::Array)
			{
				if (pI && pI->UniqueID == id && pI->IsAlive && !pI->InLimbo && pI->Owner && !pHouse->IsAlliedWith(pI->Owner)
					&& pI->GetCell() && IsCellVisibleToAI(pHouse, pI->GetCell()))
				{
					return pI;
				}
			}

			return nullptr;
		};

		std::vector<SneakAssignedTarget> assigned;
		assigned.reserve(strikes.size() + 8);

		auto syncAssigned = [&](DWORD raiderId, DWORD targetId, const CellStruct& coords, bool wide)
		{
			for (auto& a : assigned)
			{
				if (a.RaiderID == raiderId)
				{
					a.TargetID = targetId;
					a.Coords = coords;
					a.WideBlast = wide;
					return;
				}
			}

			if (targetId != 0)
				assigned.push_back({ raiderId, targetId, coords, wide });
		};

		for (auto const& [raiderId, st] : strikes)
		{
			if (st.Target == 0)
				continue;

			if (auto const pT = findTarget(st.Target))
			{
				bool wide = true;

				for (auto const pI : InfantryClass::Array)
				{
					if (pI && pI->UniqueID == raiderId)
					{
						wide = IsWideSuicideRaider(pI->Type);
						break;
					}
				}

				assigned.push_back({ raiderId, st.Target, pT->GetMapCoords(), wide });
			}
		}

		auto handle = [&](FootClass* pFoot)
		{
			const DWORD id = pFoot->UniqueID;
			auto const pType = pFoot->GetTechnoType();
			const double size = pType->Size;
			const bool wideBlast = IsWideSuicideRaider(pType);
			seen.insert(id);
			auto it = strikes.find(id);

			// A plan's member and a unit of a transport operation take their orders from them alone.
			if (IsPlanMember(pHouse, id) || IsInTransportOp(pHouse, id) || IsLeavingItsFactory(pFoot))
			{
				if (it != strikes.end())
					strikes.erase(it);

				syncAssigned(id, 0, CellStruct::Empty, wideBlast);
				return;
			}

			TechnoClass* pTarget = nullptr;
			const CellStruct curCell = pFoot->GetMapCoords();

			// Validate existing target first so a destroyed or blocked target immediately re-picks in this same tick
			// without leaving a 60-frame Target == 0 window where Micro would pull the raider back to home base.
			if (it != strikes.end() && it->second.Target != 0)
			{
				auto& strike = it->second;
				pTarget = findTarget(strike.Target);

				if (strike.LastCell == CellStruct::Empty || curCell.DistanceFrom(strike.LastCell) >= 2.0)
				{
					strike.LastCell = curCell;
					strike.LastMoveFrame = now;
				}

				if (!pTarget || pHouse->IsAlliedWith(pTarget->Owner) || now - strike.StartFrame > 5400 || now < strike.StartFrame)
				{
					strike.Target = 0;
					strike.Charging = false;
					strike.WaitingForRide = false;
					strike.StartFrame = now - 900;
					pTarget = nullptr;
					syncAssigned(id, 0, CellStruct::Empty, wideBlast);
				}
				else if (now - strike.LastMoveFrame >= 240 && now >= strike.LastMoveFrame)
				{
					const double stuckDist = curCell.DistanceFrom(pTarget->GetMapCoords());

					if (!strike.Charging && stuckDist <= 32.0)
					{
						strike.Charging = true;
						strike.LastMoveFrame = now;
					}
					else
					{
						strike.SkipTarget = strike.Target;
						strike.SkipUntilFrame = now + 1800;
						strike.Target = 0;
						strike.Charging = false;
						strike.WaitingForRide = false;
						strike.StartFrame = now - 900;
						pTarget = nullptr;
						syncAssigned(id, 0, CellStruct::Empty, wideBlast);
					}
				}
			}

			if (it == strikes.end() || it->second.Target == 0)
			{
				auto const pCurTargetTech = abstract_cast<TechnoClass*>(pFoot->Target);
				const bool hasValidEnemyAttackTarget = pCurTargetTech && pCurTargetTech->IsAlive && !pCurTargetTech->InLimbo
					&& pCurTargetTech->Owner && pCurTargetTech->Owner != pHouse && !pHouse->IsAlliedWith(pCurTargetTech->Owner);
				const bool readyForStrike = !hasValidEnemyAttackTarget
					&& (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard
						|| pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::Enter
						|| pFoot->CurrentMission == Mission::Attack || pFoot->MindControlledBy != nullptr);

				if (!readyForStrike)
					return;

				if (it == strikes.end())
					it = strikes.emplace(id, NeuralSneakStrike { 0, now, false, false, curCell, now, 0, 0 }).first;

				const bool threatRightHere = VisibleEnemyThreatNear(pHouse, curCell, 8) > 0;
				const bool waitForRide = !threatRightHere && pFoot->MindControlledBy == nullptr
					&& now - it->second.StartFrame < 450 && now >= it->second.StartFrame;

				if (waitForRide && HasRaidTransportFor(pHouse, size))
				{
					it->second.WaitingForRide = true;
					return;
				}

				it->second.WaitingForRide = false;
				const DWORD skipId = (now < it->second.SkipUntilFrame) ? it->second.SkipTarget : 0;
				pTarget = PickSneakStrikeTarget(pHouse, pFoot, assigned, skipId, false);

				if (!pTarget && skipId != 0)
					pTarget = PickSneakStrikeTarget(pHouse, pFoot, assigned, 0, false);

				if (!pTarget)
				{
					if (auto const pIslandTarget = PickSneakStrikeTarget(pHouse, pFoot, assigned, 0, true))
					{
						if (RequestTransportOperation(pHouse, NeuralOpClient::Wave, pFoot, pIslandTarget->GetMapCoords()))
							return;
					}

					// Never leave an unassigned demolition truck parked next to our own structures where a stray
					// hit would detonate inside our base: move it out to a safe perimeter cell 12..16 cells away.
					if (wideBlast && pFoot->CurrentMission != Mission::Move)
					{
						bool nearOwnBld = false;

						for (auto const pOwnB : pHouse->Buildings)
						{
							if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->GetMapCoords().DistanceFrom(curCell) <= 10.0)
							{
								nearOwnBld = true;
								break;
							}
						}

						if (nearOwnBld)
						{
							const int homeZone = ObjectZone(pFoot, pType->MovementZone);

							for (int ring = 12; ring <= 16; ring += 2)
							{
								bool movedOut = false;

								for (int dy = -ring; dy <= ring && !movedOut; dy += 4)
								{
									for (int dx = -ring; dx <= ring; dx += 4)
									{
										if (std::max(std::abs(dx), std::abs(dy)) < ring)
											continue;

										const CellStruct c { static_cast<short>(curCell.X + dx), static_cast<short>(curCell.Y + dy) };
										auto const pParkCell = MapClass::Instance.TryGetCellAt(c);

										if (!pParkCell || !pParkCell->IsClearToMove(pType->SpeedType, false, false, -1, pType->MovementZone, -1, false)
											|| MapClass::Instance.GetMovementZoneType(c, pType->MovementZone, false) != homeZone)
										{
											continue;
										}

										bool clearOfOwnBlds = true;

										for (auto const pOwnB : pHouse->Buildings)
										{
											if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->GetMapCoords().DistanceFrom(c) <= 10.0)
											{
												clearOfOwnBlds = false;
												break;
											}
										}

										if (clearOfOwnBlds && !IsCellInKnownEnemyDefenseReach(pHouse, c, 2))
										{
											IssueMoveOrder(pFoot, pParkCell, 2.0);
											movedOut = true;
											break;
										}
									}
								}

								if (movedOut)
									break;
							}
						}
					}

					return;
				}

				if (pFoot->CurrentMission == Mission::Enter)
				{
					pFoot->SetDestination(nullptr, true);
					pFoot->ForceMission(Mission::Guard);
				}

				it->second.Target = pTarget->UniqueID;
				it->second.StartFrame = now;
				it->second.Charging = threatRightHere;
				it->second.WaitingForRide = false;
				it->second.LastCell = curCell;
				it->second.LastMoveFrame = now;
				const CellStruct cell = pTarget->GetMapCoords();
				syncAssigned(id, pTarget->UniqueID, cell, wideBlast);
				Debug::Log("[NeuralAI] House %d SneakStrike: %s %u sneaks to %s of house %d at (%d,%d), %d cells, frame %d.\n",
					pHouse->ArrayIndex, pType->ID, id, pTarget->GetTechnoType()->ID, pTarget->Owner->ArrayIndex,
					cell.X, cell.Y, static_cast<int>(curCell.DistanceFrom(cell)), now);
			}

			auto& strike = it->second;

			if (!pTarget)
				return;

			syncAssigned(id, pTarget->UniqueID, pTarget->GetMapCoords(), wideBlast);

			if (strike.Charging)
			{
				if (pFoot->Target != pTarget || pFoot->CurrentMission != Mission::Attack)
				{
					pFoot->SetDestination(nullptr, true);
					pFoot->SetTarget(pTarget);
					pFoot->QueueMission(Mission::Attack, false);
				}

				return;
			}

			const CellStruct cell = pTarget->GetMapCoords();
			const double dist = curCell.DistanceFrom(cell);
			const bool pressed = pFoot->HasBeenAttacked || VisibleEnemyThreatNear(pHouse, curCell, 5) > 0;
			CellClass* const pGoal = pTarget->GetCell();

			if (dist <= 12.0 || (pressed && dist <= 18.0))
			{
				strike.Charging = true;
				pFoot->SetDestination(nullptr, true);
				pFoot->SetTarget(pTarget);
				pFoot->QueueMission(Mission::Attack, false);
				Debug::Log("[NeuralAI] House %d SneakStrike: %s %u charges %s of house %d from %d cells, frame %d.\n",
					pHouse->ArrayIndex, pType->ID, id, pTarget->GetTechnoType()->ID, pTarget->Owner->ArrayIndex,
					static_cast<int>(dist), now);
			}
			else
			{
				// Toward the target, around known defenses and armed enemies in sight while a detour
				// is clearly safer (recomputed every 450 frames).
				CellClass* const pHop = GetSafeAirHop(pHouse, pFoot, pGoal);
				IssueMoveOrder(pFoot, pHop ? pHop : pGoal, 3.0);
			}
		};

		for (auto const pUnit : UnitClass::Array)
		{
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && IsVehicleRaiderType(pUnit->Type))
				handle(pUnit);
		}

		for (auto const pInf : InfantryClass::Array)
		{
			if (pInf && pInf->Owner == pHouse && pInf->IsAlive && !pInf->InLimbo && IsSuicideInfantryType(pInf->Type))
				handle(pInf);
		}

		std::erase_if(strikes, [&seen](const auto& entry) { return !seen.contains(entry.first); });
	}
}
