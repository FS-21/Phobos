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
		// Opportunity summary of the last scan.
		InfantryTypeClass* EngineerType { nullptr };
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
		// Frames of the last spies killed on their way (oldest first, 4 at most).
		std::vector<int> SpyLossFrames {};
		// Spy UniqueID -> its last disguise kit shot (key access only).
		std::unordered_map<DWORD, NeuralSpyKitShot> KitShots {};
		// Suicide raiders sneaking to a target on foot or on their own wheels: UniqueID -> strike
		// (key access only; orders go out in UnitClass / InfantryClass array order).
		std::unordered_map<DWORD, NeuralSneakStrike> SneakStrikes {};
		int LastSneakFrame { -1 };
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

	// The wall overlay standing on this cell, or nullptr.
	OverlayTypeClass* GetWallOverlayAt(const CellStruct& cell)
	{
		auto const pCell = MapClass::Instance.TryGetCellAt(cell);

		if (!pCell || pCell->OverlayTypeIndex < 0 || pCell->OverlayTypeIndex >= OverlayTypeClass::Array.Count)
			return nullptr;

		auto const pOverlay = OverlayTypeClass::Array.GetItem(pCell->OverlayTypeIndex);

		return (pOverlay && pOverlay->Wall) ? pOverlay : nullptr;
	}

	// A breach into a walled-in tech: the wall piece to knock down and the free cell of our ground
	// next to it.
	struct NeuralTechBreach
	{
		CellStruct Wall { CellStruct::Empty };
		CellStruct Approach { CellStruct::Empty };
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

	// Can this unit knock the wall piece down? Its primary weapon must hit the ground with a
	// warhead that hurts walls as the engine rules it: Wall=, or Wood= on a wood wall.
	bool CanBreachWall(FootClass* pFoot, const OverlayTypeClass* pWall)
	{
		auto const pType = pFoot ? pFoot->GetTechnoType() : nullptr;
		auto const pSlot = pType ? pType->GetWeapon(0) : nullptr;
		auto const pWeapon = pSlot ? pSlot->WeaponType : nullptr;

		if (!pWall || !pWeapon || !pWeapon->Warhead || !pWeapon->Projectile || !pWeapon->Projectile->AG || pWeapon->Damage <= 0)
			return false;

		auto const pWarhead = pWeapon->Warhead;

		return pWarhead->Wall || pWarhead->WallAbsoluteDestroyer || (pWarhead->Wood && pWall->Armor == Armor::Wood);
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

	bool CanEngineerReachTech(HouseClass* pHouse, const NeuralKnownTech& tech, int engineerZone, const InfantryTypeClass* pEngineerType)
	{
		for (const int zone : tech.Zones)
		{
			if (zone >= 0 && zone == engineerZone)
				return true;
		}

		return pEngineerType && CanHouseFerryTypeAcrossWater(pHouse, pEngineerType);
	}

	// Refresh the tech memory from what is in sight now and log captures. Fog-fair: a tech in the
	// fog keeps the owner it had when last seen.
	void UpdateSpecOpsTechMemory(HouseClass* pHouse, NeuralSpecOpsState& state)
	{
		const NeuralSlowStep slowStep { "UpdateSpecOpsTechMemory" };

		const int now = Unsorted::CurrentFrame;
		std::vector<DWORD> inSightIDs;

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

			const bool inSight = pBld->Owner == pHouse || IsCellInSightOfAI(pHouse, cell);

			if (!inSight && !(mapKnowledge && pBld->Owner->IsNeutral()))
				continue;

			if (inSight)
				inSightIDs.push_back(pBld->UniqueID);
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

		// A remembered tech whose cell is in sight but that is gone was destroyed.
		std::sort(inSightIDs.begin(), inSightIDs.end());
		state.Techs.erase(std::remove_if(state.Techs.begin(), state.Techs.end(),
			[&](const NeuralKnownTech& tech)
			{
				return IsCellInSightOfAI(pHouse, tech.Cell) && !std::binary_search(inSightIDs.begin(), inSightIDs.end(), tech.ID);
			}), state.Techs.end());
	}

	int CountKnownDisguiseDetectorBuildings(HouseClass* pHouse, const CellStruct& cell, int marginCells);

	// Enemy buildings this house knows and would gain something from spying now.
	int CountSpecOpsSpyTargets(HouseClass* pHouse)
	{
		int targets = 0;

		for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
		{
			auto const pType = info.Building->Type;

			if (!pType->Spyable || CountKnownDisguiseDetectorBuildings(pHouse, info.Building->GetMapCoords(), 1) > 0)
				continue;

			auto const pOwner = info.Building->Owner;
			auto const pSpyBldExt = BuildingTypeExt::Fetch(pType);
			const bool worth = (pSpyBldExt && pSpyBldExt->SpyEffect_AIWorth >= 0) ? pSpyBldExt->SpyEffect_AIWorth >= 100
				: (pType->Factory == AbstractType::UnitType && !pType->Naval && !pHouse->WarFactoryInfiltrated)
				|| (pType->Factory == AbstractType::InfantryType && !pHouse->BarracksInfiltrated)
				|| ((pType->Radar || pType->SpySat) && !pHouse->SpySatActive && !pOwner->RadarVisibleTo.Contains(pHouse->ArrayIndex))
				|| pType->SuperWeapon != -1;

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

	// A soldier of the victim's side (the victim or an ally of it, never ours) in our sight right
	// now, nearest to the spy first (UniqueID on ties): a player can only copy a soldier in sight.
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
				|| pInf->Owner->IsNeutral() || !IsCellInSightOfAI(pHouse, pInf->GetMapCoords()))
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

	// Puts on the victim's colors as soon as one of its soldiers is in sight. True when the spy
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

	// How far a techno sees through disguises, in cells (0: it does not). DetectDisguiseRange= when
	// set (it marks those cells for its whole house); a detector without one still unmasks and
	// shoots a spy within its own weapon reach (Psychic Tower, Guard Tower), else within its sight.
	// Dogs (Doggie=) sniff spies out as well.
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

			if (pWeapon && pWeapon->Range > 0)
				reach = std::max(reach, pWeapon->Range / Unsorted::LeptonsPerCell);
		}

		return reach > 0 ? reach : std::max(1, pType->Sight);
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

	// An enemy that sees through disguises: known building, or unit / soldier in current sight.
	struct NeuralDisguiseDetector
	{
		CellStruct Cell;
		int Reach;
		bool Building;
		bool MindControl;
	};

	struct NeuralDisguiseDetectorCache
	{
		int Frame { -1 };
		std::vector<NeuralDisguiseDetector> Detectors {};
	};

	std::array<NeuralDisguiseDetectorCache, 32> DisguiseDetectorCaches {};

	// The disguise detectors a house knows of this frame: known enemy buildings (their power is not
	// always visible, so all count) in the order of the known building cache, then enemy units and
	// soldiers in current sight in FootClass array order.
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

			if (const int reach = DisguiseDetectionReach(pType))
				cache.Detectors.push_back({ info.Building->GetMapCoords(), reach, true, IsMindControlDetector(pType) });
		}

		for (auto const pFoot : FootClass::Array)
		{
			if (!pFoot || pFoot->InLimbo || !pFoot->IsAlive || !pFoot->Owner || pHouse->IsAlliedWith(pFoot->Owner)
				|| pFoot->Owner->IsNeutral())
			{
				continue;
			}

			auto const pType = pFoot->GetTechnoType();
			const int reach = DisguiseDetectionReach(pType);

			if (reach > 0 && IsCellInSightOfAI(pHouse, pFoot->GetMapCoords()))
				cache.Detectors.push_back({ pFoot->GetMapCoords(), reach, false, IsMindControlDetector(pType) });
		}

		return cache.Detectors;
	}

	// Known enemy buildings that see through disguises covering a cell, plus marginCells.
	int CountKnownDisguiseDetectorBuildings(HouseClass* pHouse, const CellStruct& cell, int marginCells)
	{
		int count = 0;

		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			if (detector.Building && detector.Cell.DistanceFrom(cell) <= static_cast<double>(detector.Reach + marginCells))
				count++;
		}

		return count;
	}

	// A disguise is no use in this cell: a known detector building, or an enemy unit or soldier
	// in sight that sees through disguises (DetectDisguise=, dogs), covers it.
	bool IsCellWatchedByDisguiseDetector(HouseClass* pHouse, const CellStruct& cell, int marginCells)
	{
		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			if (detector.Cell.DistanceFrom(cell) <= static_cast<double>(detector.Reach + marginCells))
				return true;
		}

		return false;
	}

	// Danger for a spy of the detectors around a target building: 60 per building and 35 per unit
	// covering it, twice that for a mind controller.
	int DisguiseDetectorPenaltyAt(HouseClass* pHouse, const CellStruct& cell)
	{
		int penalty = 0;

		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			if (detector.Cell.DistanceFrom(cell) <= static_cast<double>(detector.Reach + 1))
				penalty += (detector.Building ? 60 : 35) * (detector.MindControl ? 2 : 1);
		}

		return penalty;
	}

	// Danger for a spy of the detectors along its straight walk (a cell every 3 on the way, the
	// last 2 cells left to DisguiseDetectorPenaltyAt): 25 per detector crossed, 50 for a mind
	// controller.
	int DisguiseDetectorPenaltyOnWay(HouseClass* pHouse, const CellStruct& from, const CellStruct& to)
	{
		const int dx = to.X - from.X;
		const int dy = to.Y - from.Y;
		const int length = std::max(std::abs(dx), std::abs(dy));

		if (length <= 3)
			return 0;

		int penalty = 0;

		for (auto const& detector : GetKnownDisguiseDetectors(pHouse))
		{
			for (int step = 0; step <= length - 2; step += 3)
			{
				const CellStruct point { static_cast<short>(from.X + dx * step / length), static_cast<short>(from.Y + dy * step / length) };

				if (detector.Cell.DistanceFrom(point) <= static_cast<double>(detector.Reach))
				{
					penalty += detector.MindControl ? 50 : 25;
					break;
				}
			}
		}

		return penalty;
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

	FootClass* FindOwnFootByID(HouseClass* pHouse, DWORD id)
	{
		if (!id)
			return nullptr;

		for (auto const pFoot : FootClass::Array)
		{
			if (pFoot && pFoot->UniqueID == id)
				return (pFoot->Owner == pHouse && pFoot->IsAlive) ? pFoot : nullptr;
		}

		return nullptr;
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

	bool UpdateSpecOpsBreach(HouseClass* pHouse, NeuralSpecOpsState& state, NeuralSpecOpsCapture& capture, BuildingClass* pTarget, int now);
	void ReleaseSpecOpsBreachers(HouseClass* pHouse, const NeuralSpecOpsCapture& capture);

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
			state.SpyAvailable = false;

			for (auto const pType : InfantryTypeClass::Array)
			{
				if (!pType || (!pType->Engineer && !IsSpyInfantryType(pType)))
					continue;

				const bool usable = pHouse->CountOwnedNow(pType) > 0 || CanBuildUnderHumanRules(pHouse, pType);

				if (!usable)
					continue;

				if (pType->Engineer && !state.EngineerType)
					state.EngineerType = pType;
				else if (IsSpyInfantryType(pType))
					state.SpyAvailable = true;
			}

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

				// A walled-in tech counts when a single piece of its wall opens it: Expert 4 sends a
				// wall breaker with the engineer.
				NeuralTechBreach breach;
				auto const pTechBld = FindBuildingByID(tech.ID);
				const NeuralTechWall wall = GetTechWallState(pHouse, pTechBld, homeZone, home, breach);
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
					? CanEngineerReachTech(pHouse, tech, homeZone, state.EngineerType)
					: wall == NeuralTechWall::Breachable;

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
			state.SpyTargets = (state.SpyAvailable && !spiesWasted) ? CountSpecOpsSpyTargets(pHouse) : 0;

			auto const& bridges = GetBridgeState(pHouse);
			state.BridgeRepairTask = state.EngineerType && (bridges.RouteSpan >= 0 || bridges.FleetBlockSpan >= 0);
			state.BridgeDemolitionTask = AreBridgesDestroyable()
				&& (bridges.AmbushSpan >= 0 || bridges.SeaLaneSpan >= 0 || bridges.MoatSpan >= 0);

			// Selector prior of Expert 4: real work for it on this map right now. Techs only count
			// with an engineer we own or can train; ops in flight keep it in charge until they end.
			int opportunity = 0;

			if (state.EngineerType)
				opportunity += std::min(560, bestWorth[0] + bestWorth[1] + bestWorth[2]);

			opportunity += std::min(180, state.SpyTargets * 60);
			opportunity += state.BridgeRepairTask ? 260 : 0;
			opportunity += state.BridgeDemolitionTask ? 200 : 0;
			opportunity += state.Captures.empty() ? 0 : 160;
			state.Opportunity = std::min(780, opportunity);
		}

		// Captures upkeep.
		for (size_t i = 0; i < state.Captures.size();)
		{
			auto& capture = state.Captures[i];
			auto const pEngineer = FindOwnFootByID(pHouse, capture.EngineerID);
			auto const pTarget = FindBuildingByID(capture.TargetID);
			// A breach first takes the walk of the wall breakers and their shots.
			const int captureTimeout = capture.BreachFrame > 0 ? 10800 : 7200;
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
			else if (!pTarget || !IsKnownNeutralTech(pHouse, pTarget) || now - capture.StartFrame > captureTimeout)
			{
				if (pTarget && now - capture.StartFrame > captureTimeout)
					FailSpecOpsCapture(state, capture.TargetID, 1800);

				if (pEngineer->Destination == pTarget || pEngineer->CurrentMission == Mission::Enter)
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
		outTechValue = state.EngineerType ? state.SafeTechValue + state.ContestedTechValue : 0;
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
	// walls no single breach opens is skipped).
	BuildingClass* PickSpecOpsCaptureTarget(HouseClass* pHouse, FootClass* pEngineer, bool& outContested, NeuralTechBreach& outBreach)
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

			NeuralTechBreach breach;
			const NeuralTechWall wall = GetTechWallState(pHouse, pBld, engineerZone, from, breach);

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
	// pBreachWall, only units standing in breachZone (Normal) that can knock that wall down count,
	// from a little further away: without them the engineer cannot get in at all.
	void CollectSpecOpsEscorts(HouseClass* pHouse, const CellStruct& cell, int maxCount, std::vector<FootClass*>& out,
		const NeuralSpecOpsCapture* pPending = nullptr, const OverlayTypeClass* pBreachWall = nullptr, int breachZone = -1)
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

			if (pBreachWall && (!CanBreachWall(pFoot, pBreachWall) || ObjectZone(pFoot, MovementZone::Normal) != breachZone))
				return;

			if (pPending && isTaken(*pPending, pFoot->UniqueID))
				return;

			for (auto const& capture : state.Captures)
			{
				if (isTaken(capture, pFoot->UniqueID))
					return;
			}

			const int dist = static_cast<int>(pFoot->GetMapCoords().DistanceFrom(cell));

			if (dist <= (pBreachWall ? 90 : 45))
				candidates.emplace_back(dist, pFoot);
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

		if (now - capture.BreachFrame > 3600)
			return fail("too long");

		auto const pWallCell = MapClass::Instance.TryGetCellAt(capture.BreachCell);
		int breakers = 0;

		for (const DWORD id : capture.Breachers)
		{
			auto const pFoot = FindOwnFootByID(pHouse, id);

			if (!pFoot || pFoot->InLimbo || !CanBreachWall(pFoot, pWall))
				continue;

			breakers++;

			// A breaker fighting enemies finishes that first; one that dropped the order gets it back.
			if (pFoot->Target == pWallCell || (pFoot->Target && pFoot->Target->WhatAmI() != AbstractType::Cell))
				continue;

			pFoot->ClearMegaMissionData();
			pFoot->SetTarget(pWallCell);
			pFoot->QueueMission(Mission::Attack, true);
		}

		if (breakers == 0)
		{
			std::vector<FootClass*> fresh;
			CollectSpecOpsEscorts(pHouse, capture.BreachCell, 2, fresh, nullptr, pWall, capture.BreachZone);

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
			CollectSpecOpsEscorts(pHouse, capture.BreachCell, 1, fresh, nullptr, pWall, capture.BreachZone);

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

		if (!pCapture)
		{
			if (!expertActive)
				return false;

			bool contested = false;
			NeuralTechBreach breach;
			pTarget = PickSpecOpsCaptureTarget(pHouse, pEngineer, contested, breach);

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
				// A walled-in tech needs a wall breaker of ours on this side of the wall; without
				// one that tech waits and another one is picked next tick.
				const int zone = ObjectZone(pEngineer, MovementZone::Normal);
				std::vector<FootClass*> breachers;
				CollectSpecOpsEscorts(pHouse, breach.Wall, 2, breachers, nullptr, GetWallOverlayAt(breach.Wall), zone);

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

	// A raid transport of ours that can carry a raider of this size (passenger aircraft, flying,
	// burrowing or amphibious transport): the raider waits for an airborne raid instead of walking.
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

			if (flies || IsUndergroundTransportType(pUnitType) || IsAmphibiousGroundTransportType(pUnitType))
				return true;
		}

		return false;
	}

	// Where a suicide raider does the most harm: the known enemy building whose blast area holds the
	// most building value (cost / 100 of every known building within the warhead's CellSpread plus
	// one cell), less 6 per known defense within 7 cells of it and a cell of the walk per 8.
	// Reachable by the raider's movement zone only; UniqueID breaks ties.
	BuildingClass* PickSneakStrikeTarget(HouseClass* pHouse, FootClass* pRaider)
	{
		auto const pType = pRaider->GetTechnoType();
		auto const pBlast = GetSuicideBlastWeapon(pType);
		const double spread = (pBlast && pBlast->Warhead ? pBlast->Warhead->CellSpread : 1.0) + 1.0;
		const CellStruct from = pRaider->GetMapCoords();
		auto const& entries = GetKnownEnemyBuildingCache(pHouse).Entries;
		BuildingClass* pBest = nullptr;
		int bestScore = 0;

		for (auto const& info : entries)
		{
			auto const pBld = info.Building;

			if (!pBld->IsAlive || pBld->InLimbo || !pBld->Owner || pBld->Type->Insignificant || pBld->Type->InvisibleInGame
				|| !AreZonesConnected(from, info.Coords, pType->MovementZone))
			{
				continue;
			}

			int worth = 0;
			int defenses = 0;

			for (auto const& other : entries)
			{
				const double dist = other.Coords.DistanceFrom(info.Coords);

				if (dist <= spread)
					worth += other.Building->Type->Cost / 100;

				if (other.IsDefense && other.Building != pBld && dist <= 7.0)
					defenses++;
			}

			const int score = worth - defenses * 6 - static_cast<int>(from.DistanceFrom(info.Coords)) / 8;

			if (score > bestScore || (score == bestScore && pBest && pBld->UniqueID < pBest->UniqueID))
			{
				bestScore = score;
				pBest = pBld;
			}
		}

		return pBest;
	}

	// A suicide raider of ours is sneaking to (or charging) a target: the micro leaves it alone.
	bool IsSneakStrikeActive(HouseClass* pHouse, DWORD id)
	{
		auto const& strikes = GetSpecOpsState(pHouse).SneakStrikes;
		auto const it = strikes.find(id);

		return it != strikes.end() && it->second.Target != 0;
	}

	// Suicide raiders without a ride (demolition trucks, terrorists): fragile and only useful once
	// they blow up among buildings, so they do not join the army. An idle one with no enemy close
	// waits up to 2700 frames for a raid transport that can carry it (AirborneFront takes it), then
	// sneaks to the best known target along a detour around known defenses and armed enemies in
	// sight (FindSafeFerryWaypoint, land mode) and charges it from 8 cells, or from 14 once shot at
	// or with enemies close in. A strike gives up after 9000 frames; a raid that boards the raider
	// ends it. Every 60 frames.
	void UpdateSneakStrikes(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->ArrayIndex < 0 || pHouse->ArrayIndex >= 32)
			return;

		auto& state = GetSpecOpsState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - state.LastSneakFrame < 60 && now >= state.LastSneakFrame)
			return;

		state.LastSneakFrame = now;
		auto& strikes = state.SneakStrikes;
		std::unordered_set<DWORD> seen;

		auto findTarget = [pHouse](DWORD id) -> BuildingClass*
		{
			for (auto const& info : GetKnownEnemyBuildingCache(pHouse).Entries)
			{
				if (info.Building->UniqueID == id)
					return (info.Building->IsAlive && !info.Building->InLimbo) ? info.Building : nullptr;
			}

			return nullptr;
		};

		auto handle = [&](FootClass* pFoot)
		{
			const DWORD id = pFoot->UniqueID;
			const double size = pFoot->GetTechnoType()->Size;
			seen.insert(id);
			auto it = strikes.find(id);

			if (IsPlanMember(pHouse, id))
			{
				if (it != strikes.end())
					strikes.erase(it);

				return;
			}

			const bool idle = (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard)
				&& !pFoot->Target;

			if (it == strikes.end() || it->second.Target == 0)
			{
				if (!idle)
					return;

				if (it == strikes.end())
					it = strikes.emplace(id, NeuralSneakStrike { 0, now, false }).first;

				if (VisibleEnemyThreatNear(pHouse, pFoot->GetMapCoords(), 8) > 0)
					return;

				const bool waitForRide = now - it->second.StartFrame < 2700 && now >= it->second.StartFrame;

				if (waitForRide && HasRaidTransportFor(pHouse, size))
					return;

				auto const pTarget = PickSneakStrikeTarget(pHouse, pFoot);

				if (!pTarget)
					return;

				it->second = NeuralSneakStrike { pTarget->UniqueID, now, false };
				const CellStruct cell = pTarget->GetMapCoords();
				Debug::Log("[NeuralAI] House %d SneakStrike: %s %u sneaks to %s of house %d at (%d,%d), %d cells, frame %d.\n",
					pHouse->ArrayIndex, pFoot->GetTechnoType()->ID, id, pTarget->Type->ID, pTarget->Owner->ArrayIndex,
					cell.X, cell.Y, static_cast<int>(pFoot->GetMapCoords().DistanceFrom(cell)), now);
			}

			auto& strike = it->second;
			auto const pTarget = findTarget(strike.Target);

			if (!pTarget || pHouse->IsAlliedWith(pTarget->Owner) || now - strike.StartFrame > 9000 || now < strike.StartFrame)
			{
				// Target gone or too long on the way: pick again at once next time it is idle.
				strike = NeuralSneakStrike { 0, now - 2700, false };
				return;
			}

			if (strike.Charging)
			{
				if (pFoot->Target != pTarget)
				{
					pFoot->SetTarget(pTarget);
					pFoot->QueueMission(Mission::Attack, false);
				}

				return;
			}

			const CellStruct cell = pTarget->GetMapCoords();
			const double dist = pFoot->GetMapCoords().DistanceFrom(cell);
			const bool pressed = pFoot->HasBeenAttacked || VisibleEnemyThreatNear(pHouse, pFoot->GetMapCoords(), 4) > 0;
			CellClass* const pGoal = pTarget->GetCell();

			if (dist <= 8.0 || (pressed && dist <= 14.0))
			{
				strike.Charging = true;
				pFoot->SetTarget(pTarget);
				pFoot->QueueMission(Mission::Attack, false);
				Debug::Log("[NeuralAI] House %d SneakStrike: %s %u charges %s of house %d from %d cells, frame %d.\n",
					pHouse->ArrayIndex, pFoot->GetTechnoType()->ID, id, pTarget->Type->ID, pTarget->Owner->ArrayIndex,
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
