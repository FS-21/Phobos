// NeuralAI.Tactics.inl: Watchdogs (air losses, stuck units, idle harvesters, offensive stones) and match tactics.
// Part of NeuralAI.cpp (included in order; one translation unit).

namespace
{
	// Air danger memory: every AI tick, our flying units in the air are remembered; one that
	// vanished since the last tick (not alive any more, not just loaded or landed) away from our
	// base was shot down, so its last cell is an anti-air danger spot for 8100 frames. Air lanes
	// (FindSafeFerryWaypoint) avoid those spots even when the shooter is no longer visible.
	void UpdateAirLossMemory(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		std::unordered_set<DWORD> alive;
		std::vector<NeuralAirLossTrack> next;

		for (auto const pTech : TechnoClass::Array)
		{
			if (!pTech || pTech->Owner != pHouse || !pTech->IsAlive)
				continue;

			auto const pFoot = abstract_cast<FootClass*>(pTech);

			if (!pFoot)
				continue;

			alive.insert(pFoot->UniqueID);

			if (pFoot->InLimbo || !pFoot->IsInAir())
				continue;

			bool isCarrier = false;

			if (auto const pAir = abstract_cast<AircraftClass*>(pFoot))
			{
				if (!pAir->Type || pAir->Type->Spawned)
					continue;

				isCarrier = pAir->Type->Carryall || pAir->Type->Passengers > 0;
			}
			else if (auto const pUnit = abstract_cast<UnitClass*>(pFoot))
			{
				auto const pTypeExt = pUnit->Type ? TechnoTypeExt::Fetch(pUnit->Type) : nullptr;
				isCarrier = (pTypeExt && pTypeExt->Carryall) || (pUnit->Type && IsAmphibiousOrAirTransport(pUnit->Type));
			}

			next.push_back({ pFoot->UniqueID, pFoot->GetMapCoords(), isCarrier });
		}

		for (auto const& track : macro.AirLossTracks)
		{
			if (alive.contains(track.ID) || track.LastCoords == CellStruct::Empty)
				continue;

			bool nearHome = false;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->GetMapCoords().DistanceFrom(track.LastCoords) <= 10.0)
				{
					nearHome = true;
					break;
				}
			}

			if (nearHome)
				continue;

			macro.AirDangerSpots.emplace_back(track.LastCoords, Unsorted::CurrentFrame);

			if (track.IsCarrier)
				CountDecision(NeuralDecision::AirCarrierLost, pHouse->ArrayIndex);
		}

		std::erase_if(macro.AirDangerSpots, [](const auto& spot)
			{ return spot.second > Unsorted::CurrentFrame || Unsorted::CurrentFrame - spot.second >= 8100; });

		if (macro.AirDangerSpots.size() > 24)
		{
			macro.AirDangerSpots.erase(macro.AirDangerSpots.begin(),
				macro.AirDangerSpots.begin() + static_cast<std::ptrdiff_t>(macro.AirDangerSpots.size() - 24));
		}

		macro.AirLossTracks = std::move(next);
	}

	// Stuck order watchdog: a ground unit with a move or attack-move destination that has not
	// left its cell is released (orders cleared, back to Guard) so the AI gives it a new job:
	// after 450 frames when the destination is in another movement zone (unreachable: e.g. the
	// only bridge was destroyed and the pathfinder parks it on the near bank forever), after 1800
	// frames otherwise (a traffic jam that does not clear). Units fighting are never touched.
	void UpdateStuckUnits(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		std::unordered_map<DWORD, NeuralStuckTrack> next;
		const int now = Unsorted::CurrentFrame;

		auto check = [&](FootClass* pFoot, bool skip)
		{
			if (skip || !pFoot || pFoot->Owner != pHouse || !pFoot->IsAlive || pFoot->InLimbo || pFoot->IsInAir())
				return;

			auto const pType = pFoot->GetTechnoType();

			// Ships included: an unreachable naval order (land cell) is dropped like any other.
			if (!pType || pType->JumpJet || pType->Teleporter || pType->MovementZone == MovementZone::Fly)
				return;

			const bool moving = pFoot->CurrentMission == Mission::Move || pFoot->CurrentMission == Mission::AttackMove
				|| pFoot->MegaMission == Mission::AttackMove;
			AbstractClass* const pDest = pFoot->Destination ? pFoot->Destination : pFoot->MegaDestination;

			if (!moving || !pDest)
				return;

			const CellStruct here = pFoot->GetMapCoords();
			const CellStruct destCell = CellClass::Coord2Cell(pDest->GetCoords());

			if (here.DistanceFrom(destCell) <= 2.5)
				return;

			NeuralStuckTrack track { here, now };

			if (auto const it = macro.StuckTracks.find(pFoot->UniqueID);
				it != macro.StuckTracks.end() && it->second.Cell.DistanceFrom(here) <= 1.5)
			{
				track = it->second;
			}

			auto const pTargetTechno = abstract_cast<TechnoClass*>(pFoot->Target);

			if (pFoot->IsFiring || (pTargetTechno && pTargetTechno->IsAlive && !pTargetTechno->InLimbo))
				track.SinceFrame = now;

			const int stuckFrames = now - track.SinceFrame;
			auto const pDestObject = abstract_cast<ObjectClass*>(pDest);
			const int footZone = ObjectZone(pFoot, pType->MovementZone);
			const int destZone = MapClass::Instance.GetMovementZoneType(
				destCell, pType->MovementZone, pDestObject && pDestObject->OnBridge);
			bool unreachable = footZone > 1 && destZone != footZone;

			// A destination on an impassable cell (a building, an obstacle; zone 1) is reachable
			// when a cell next to it (up to 2 away) is in our zone, as for attack-moves onto enemy
			// buildings.
			if (unreachable && destZone <= 1)
			{
				for (int r = 1; r <= 2 && unreachable; ++r)
				{
					for (int dy = -r; dy <= r && unreachable; ++dy)
					{
						for (int dx = -r; dx <= r && unreachable; ++dx)
						{
							if (std::abs(dx) != r && std::abs(dy) != r)
								continue;

							const CellStruct nc { static_cast<short>(destCell.X + dx), static_cast<short>(destCell.Y + dy) };

							if (MapClass::Instance.GetMovementZoneType(nc, pType->MovementZone, false) == footZone)
								unreachable = false;
						}
					}
				}
			}

			// Stalled attack-move: the unit fell back to Guard (typically after a fight) with its
			// attack-move still pending and does not resume it, while the order paths that check
			// "MegaMission != AttackMove" treat it as already on its way. Drop the stale order after
			// 150 frames so its owner logic sends it again.
			const bool stalledAttackMove = (pFoot->CurrentMission == Mission::Guard || pFoot->CurrentMission == Mission::Area_Guard)
				&& pFoot->MegaMission == Mission::AttackMove;

			if ((unreachable && stuckFrames >= 450) || stuckFrames >= 1800 || (stalledAttackMove && stuckFrames >= 150))
			{
				pFoot->ClearMegaMissionData();
				pFoot->SetDestination(nullptr, true);
				pFoot->QueueMission(Mission::Guard, true);
				CountDecision(unreachable ? NeuralDecision::UnreachableOrderDropped
					: (stalledAttackMove ? NeuralDecision::AttackMoveStalled : NeuralDecision::StuckUnitReleased),
					pHouse->ArrayIndex);
				return; // Its next order starts a new track.
			}

			next.emplace(pFoot->UniqueID, track);
		};

		for (auto const pUnit : UnitClass::Array)
			check(pUnit, !pUnit || !pUnit->Type || pUnit->Type->Harvester || pUnit->Type->ResourceGatherer);

		for (auto const pInf : InfantryClass::Array)
			check(pInf, false);

		macro.StuckTracks = std::move(next);
	}

	// Idle harvester dispatcher: an empty harvester on Harvest that has not left its cell for 900
	// frames found no ore within its scan, and vanilla keeps it waiting in place (e.g. on a
	// depleted field). Send it to the nearest worthy ore field (>= 4 ore cells in 5x5) on its own
	// landmass within 96 cells, 8+ cells away and 10+ cells from known enemy structures; on
	// arrival it falls back to Guard and the harvester logic puts it back on Harvest.
	// Chrono miners (Teleporter) and deployable gatherers are left alone: a destination makes a
	// chrono miner loop-teleport, and slave miners have their own routing.
	void UpdateIdleHarvesters(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		std::unordered_map<DWORD, NeuralStuckTrack> next;
		const int now = Unsorted::CurrentFrame;
		std::vector<CellStruct> enemyStructures;
		bool enemyStructuresKnown = false;

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type
				|| !pUnit->Type->Harvester || pUnit->Type->Teleporter || pUnit->Type->DeploysInto || pUnit->IsInAir())
			{
				continue;
			}

			if (pUnit->CurrentMission != Mission::Harvest || pUnit->HasAnyLink()
				|| pUnit->Tiberium.GetTotalAmount() > 0.0f)
			{
				continue;
			}

			const CellStruct here = pUnit->GetMapCoords();
			NeuralStuckTrack track { here, now };

			if (auto const it = macro.IdleHarvesterTracks.find(pUnit->UniqueID);
				it != macro.IdleHarvesterTracks.end() && it->second.Cell.DistanceFrom(here) <= 1.5)
			{
				track = it->second;
			}

			if (now - track.SinceFrame < 900)
			{
				next.emplace(pUnit->UniqueID, track);
				continue;
			}

			if (!enemyStructuresKnown)
			{
				enemyStructuresKnown = true;

				for (auto const pEB : BuildingClass::Array)
				{
					if (pEB && pEB->IsAlive && !pEB->InLimbo && pEB->Owner && pEB->Type && pEB->Owner != pHouse
						&& !pHouse->IsAlliedWith(pEB->Owner) && !pEB->Owner->IsNeutral() && IsEnemyBuildingKnown(pHouse, pEB))
					{
						enemyStructures.push_back(pEB->GetMapCoords());
					}
				}
			}

			const MovementZone mz = pUnit->Type->MovementZone;
			const int unitZone = ObjectZone(pUnit, mz);
			CellClass* pBestOre = nullptr;
			int bestDist = INT_MAX;

			for (int dy = -96; dy <= 96; dy += 3)
			{
				for (int dx = -96; dx <= 96; dx += 3)
				{
					const int d = std::max(std::abs(dx), std::abs(dy));

					if (d < 8 || d >= bestDist)
						continue;

					const CellStruct tc { static_cast<short>(here.X + dx), static_cast<short>(here.Y + dy) };
					auto const pCell = MapClass::Instance.TryGetCellAt(tc);

					if (!pCell || pCell->GetContainedTiberiumValue() <= 0)
						continue;

					if (unitZone >= 0 && MapClass::Instance.GetMovementZoneType(tc, mz, false) != unitZone)
						continue;

					bool nearEnemy = false;

					for (auto const& ec : enemyStructures)
					{
						if (std::max(std::abs(ec.X - tc.X), std::abs(ec.Y - tc.Y)) < 10)
						{
							nearEnemy = true;
							break;
						}
					}

					if (nearEnemy)
						continue;

					int fieldCells = 0;

					for (int fy = -2; fy <= 2; ++fy)
					{
						for (int fx = -2; fx <= 2; ++fx)
						{
							auto const pFC = MapClass::Instance.TryGetCellAt(
								CellStruct { static_cast<short>(tc.X + fx), static_cast<short>(tc.Y + fy) });

							if (pFC && pFC->GetContainedTiberiumValue() > 0)
								fieldCells++;
						}
					}

					if (fieldCells < 4)
						continue;

					bestDist = d;
					pBestOre = pCell;
				}
			}

			if (!pBestOre)
				pBestOre = FindFreeOreFieldCell(pHouse, here, ObjectZone(pUnit, MovementZone::Normal), 8, true);

			if (pBestOre)
			{
				pUnit->SetDestination(pBestOre, true);
				pUnit->QueueMission(Mission::Move, true);
				CountDecision(NeuralDecision::IdleHarvesterSent, pHouse->ArrayIndex);
				continue; // A new track starts when it is back on Harvest.
			}

			// No field in reach: look again in 900 frames rather than scanning every tick.
			track.SinceFrame = now;
			next.emplace(pUnit->UniqueID, track);
		}

		macro.IdleHarvesterTracks = std::move(next);
	}

	// Offensive crawl bookkeeping: confirm the cells chosen for stones toward the enemy once a
	// structure of ours stands there, then count the confirmed stones that are destroyed. The
	// trainer credits slots 38 / 81 with how well forward stones survive.
	void UpdateOffensiveStones(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastStoneCheckFrame < 150)
			return;

		macro.LastStoneCheckFrame = now;

		auto findOwnAt = [pHouse](const CellStruct& cell) -> BuildingClass*
		{
			for (auto const pB : pHouse->Buildings)
			{
				if (pB && pB->IsAlive && !pB->InLimbo && pB->Type && pB->GetMapCoords() == cell)
					return pB;
			}

			return nullptr;
		};

		std::vector<std::pair<CellStruct, int>> stillPending;

		for (auto const& [cell, frame] : macro.OffensiveStoneCells)
		{
			if (auto const pPlaced = findOwnAt(cell))
			{
				macro.OffensiveStones.push_back(pPlaced->UniqueID);
				CountDecision(NeuralDecision::CrawlStoneForward, pHouse->ArrayIndex);
			}
			else if (now - frame <= 1800)
			{
				stillPending.emplace_back(cell, frame);
			}
		}

		macro.OffensiveStoneCells = std::move(stillPending);

		std::vector<DWORD> alive;

		for (const DWORD id : macro.OffensiveStones)
		{
			bool found = false;

			for (auto const pB : pHouse->Buildings)
			{
				if (pB && pB->IsAlive && !pB->InLimbo && pB->UniqueID == id)
				{
					found = true;
					break;
				}
			}

			if (found)
				alive.push_back(id);
			else
				CountDecision(NeuralDecision::CrawlStoneLost, pHouse->ArrayIndex);
		}

		macro.OffensiveStones = std::move(alive);
	}

	// Cell of the newest forward stone of the offensive crawl still standing (the front), or Empty.
	CellStruct GetNewestOffensiveStoneCell(HouseClass* pHouse)
	{
		auto const& stones = GetMacroState(pHouse).OffensiveStones;

		if (stones.empty())
			return CellStruct::Empty;

		for (auto const pOwnB : pHouse->Buildings)
		{
			if (pOwnB && pOwnB->IsAlive && !pOwnB->InLimbo && pOwnB->UniqueID == stones.back())
				return pOwnB->GetMapCoords();
		}

		return CellStruct::Empty;
	}

	// Resource hoarder (Expert 6 at full strength, every house a little): the best ore field we
	// do not mine yet. Value = ore cells + 40 for a spawner tree (a renewable field); risk tier 0 =
	// no known enemy structure within 20 cells, 1 = one is near (only Expert 6 claims those, after
	// every tier 0 field); fields held by a known enemy refinery or ConYard (14 cells) are left to
	// the raid plans. Fog-fair: enemy structures count only when known. Only fields on the
	// landmass of one of our ConYards (the MCV / crawl can reach them).
	void UpdateResourceClaim(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastResourceClaimFrame < 300)
			return;

		macro.LastResourceClaimFrame = now;
		macro.ResourceClaimCell = CellStruct::Empty;
		macro.ResourceClaimTreeCell = CellStruct::Empty;
		macro.ResourceClaimTier = -1;
		macro.ResourceClaimZone = -1;

		auto const pExt = HouseExt::Fetch(pHouse);
		const bool hoarder = pExt && pExt->NeuralAI_ActiveExpert == 6;

		std::vector<CellStruct> ownHubs;
		std::vector<std::pair<CellStruct, int>> ownAnchors; // cell, zone

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			const CellStruct bc = pBld->GetMapCoords();

			if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				ownHubs.push_back(bc);

			if (pBld->Type->ConstructionYard)
				ownAnchors.emplace_back(bc, MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge));
		}

		if (ownAnchors.empty())
			return;

		std::vector<CellStruct> enemyStructures;
		std::vector<CellStruct> enemyHubs;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Type->Insignificant
				|| pBld->Type->InvisibleInGame || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const CellStruct bc = pBld->GetMapCoords();
			enemyStructures.push_back(bc);

			if (pBld->Type->Refinery || pBld->Type->ResourceDestination || pBld->Type->ConstructionYard)
				enemyHubs.push_back(bc);
		}

		auto chebyshev = [](const CellStruct& a, const CellStruct& b)
		{
			return std::max(std::abs(a.X - b.X), std::abs(a.Y - b.Y));
		};

		int bestMetric = INT_MAX;

		for (auto const& field : GetOreFields())
		{
			if (std::any_of(ownHubs.begin(), ownHubs.end(), [&](const CellStruct& h) { return chebyshev(h, field.Cell) <= 14; }))
				continue;

			if (std::any_of(enemyHubs.begin(), enemyHubs.end(), [&](const CellStruct& h) { return chebyshev(h, field.Cell) <= 14; }))
				continue;

			const int tier = std::any_of(enemyStructures.begin(), enemyStructures.end(),
				[&](const CellStruct& e) { return chebyshev(e, field.Cell) <= 20; }) ? 1 : 0;

			if (tier == 1 && !hoarder)
				continue;

			const int fieldZone = MapClass::Instance.GetMovementZoneType(field.Cell, MovementZone::Normal, false);
			int dist = INT_MAX;

			for (auto const& [anchorCell, anchorZone] : ownAnchors)
			{
				if (anchorZone == fieldZone)
					dist = std::min(dist, chebyshev(anchorCell, field.Cell));
			}

			if (dist == INT_MAX)
				continue;

			const int value = field.OreCells + (field.SpawnerTree ? 40 : 0);
			const int metric = tier * 100000 + dist * 3 - value;

			if (metric < bestMetric)
			{
				bestMetric = metric;
				macro.ResourceClaimCell = field.Cell;
				macro.ResourceClaimTier = tier;
				macro.ResourceClaimZone = fieldZone;
			}
		}

		// The field's spawner tree (within 6 cells of its cell): its refinery goes next to it.
		if (macro.ResourceClaimCell != CellStruct::Empty)
		{
			int bestTree = INT_MAX;

			for (int dy = -6; dy <= 6; ++dy)
			{
				for (int dx = -6; dx <= 6; ++dx)
				{
					const CellStruct tc { static_cast<short>(macro.ResourceClaimCell.X + dx), static_cast<short>(macro.ResourceClaimCell.Y + dy) };

					if (auto const pCell = MapClass::Instance.TryGetCellAt(tc))
					{
						if (auto const pTerrain = pCell->GetTerrain(false);
							pTerrain && pTerrain->IsAlive && pTerrain->Type && pTerrain->Type->SpawnsTiberium
							&& std::abs(dx) + std::abs(dy) < bestTree)
						{
							bestTree = std::abs(dx) + std::abs(dy);
							macro.ResourceClaimTreeCell = tc;
						}
					}
				}
			}
		}
	}

	bool CanBuildUnderHumanRules(HouseClass* pHouse, TechnoTypeClass* pType);

	// A real structure a player can place: not a hidden switch, limbo upgrade or wall.
	bool IsPlayerFacingBuilding(BuildingTypeClass* pType)
	{
		return pType && pType->Selectable && !pType->InvisibleInGame && !pType->Insignificant && !pType->Wall
			&& !pType->Gate && pType->TechLevel >= 0 && pType->TechLevel <= 10;
	}

	void CollectPrerequisiteBuildings(int prereq, std::vector<int>& out, int depth);

	// Branch choices (map-wide, computed once per match): building types that exclude another
	// building through Prerequisite.Negative, or are excluded by one. Building one closes the other
	// branch (two tech labs, rival defense lines), so a player picks by taste, not by a fixed rule.
	std::vector<char> BranchChoiceBuildings;
	int BranchChoiceMatchSerial { -1 };
	bool BranchChoiceReady { false };

	bool IsBranchChoiceBuilding(int buildingIdx)
	{
		if (!BranchChoiceReady || BranchChoiceMatchSerial != NeuralMatchSerial
			|| static_cast<int>(BranchChoiceBuildings.size()) != BuildingTypeClass::Array.Count)
		{
			BranchChoiceReady = true;
			BranchChoiceMatchSerial = NeuralMatchSerial;
			BranchChoiceBuildings.assign(static_cast<size_t>(BuildingTypeClass::Array.Count), 0);

			std::vector<int> excluded;

			for (auto const pType : BuildingTypeClass::Array)
			{
				auto const pTypeExt = pType ? TechnoTypeExt::Fetch(pType) : nullptr;

				if (!pTypeExt || pTypeExt->Prerequisite_Negative.empty())
					continue;

				for (const int negative : pTypeExt->Prerequisite_Negative)
				{
					excluded.clear();
					CollectPrerequisiteBuildings(negative, excluded, 0);

					for (const int b : excluded)
					{
						if (b >= 0 && b < BuildingTypeClass::Array.Count)
						{
							BranchChoiceBuildings[static_cast<size_t>(b)] = 1;
							BranchChoiceBuildings[static_cast<size_t>(pType->ArrayIndex)] = 1;
						}
					}
				}
			}
		}

		return buildingIdx >= 0 && buildingIdx < static_cast<int>(BranchChoiceBuildings.size())
			&& BranchChoiceBuildings[static_cast<size_t>(buildingIdx)] != 0;
	}

	// Deterministic per-match taste in [-range, range] for one house and one type (game seed,
	// house, type): the same all match, different between matches, identical on every client.
	int MatchTaste(HouseClass* pHouse, int typeIdx, int range)
	{
		const uint32_t taste = (static_cast<uint32_t>(Game::Seed) * 2654435761u)
			^ (static_cast<uint32_t>(pHouse->ArrayIndex + 1) * 40503u)
			^ (static_cast<uint32_t>(typeIdx + 1) * 2246822519u);
		return static_cast<int>(taste % static_cast<uint32_t>(2 * range + 1)) - range;
	}

	// Building types that satisfy one prerequisite entry: the building itself (index >= 0), or the
	// members of a generic prerequisite group (index < 0: Phobos [GenericPrerequisites] when
	// defined, else the vanilla POWER / FACTORY / BARRACKS / RADAR / TECH / PROC lists).
	void CollectPrerequisiteBuildings(int prereq, std::vector<int>& out, int depth)
	{
		if (prereq >= 0)
		{
			out.push_back(prereq);
			return;
		}

		if (depth > 4)
			return;

		auto const pRulesExt = RulesExt::Global();
		const int group = -prereq;

		if (pRulesExt && group < pRulesExt->GenericPrerequisites.Count && pRulesExt->GenericPrerequisites.GetItem(group).Count > 0)
		{
			for (const int member : pRulesExt->GenericPrerequisites.GetItem(group))
				CollectPrerequisiteBuildings(member, out, depth + 1);

			return;
		}

		auto const pRules = RulesClass::Instance;

		if (!pRules)
			return;

		TypeList<int>* pList = nullptr;

		switch (prereq)
		{
		case -1: pList = &pRules->PrerequisitePower; break;
		case -2: pList = &pRules->PrerequisiteFactory; break;
		case -3: pList = &pRules->PrerequisiteBarracks; break;
		case -4: pList = &pRules->PrerequisiteRadar; break;
		case -5: pList = &pRules->PrerequisiteTech; break;
		case -6: pList = &pRules->PrerequisiteProc; break;
		default: break;
		}

		if (pList)
		{
			for (const int member : *pList)
				out.push_back(member);
		}
	}

	// Tech tree progress like a human player (every 900 frames): for each building type we do not
	// own, how many unit or building types of our tree (not buildable yet) list it, or a generic
	// group it belongs to, among their missing prerequisites. Read from the prerequisite lists, nothing named, so captured ConYards that
	// open foreign trees count the same way. HasBuildableTechUnlock: one of those unlockers can be
	// built right now (its own prerequisites are met), so the tech tree is not done yet.
	void UpdateTechUnlocks(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.TechUnlockFrame < 900 && now >= macro.TechUnlockFrame)
			return;

		macro.TechUnlockFrame = now;
		macro.TechUnlocks.clear();
		macro.HasBuildableTechUnlock = false;

		std::vector<int> satisfiers;

		// Our tree: types available to our country (HouseExt::IsAvailableToHouse: owners, required
		// and forbidden houses, parent country), or owned by the country of a foreign ConYard we hold
		// (a captured one opens its owners' tree), within our tech level. "Not buildable yet" is
		// judged under human rules (CanBuildUnderHumanRules): for an AI house the game's CanBuild
		// skips prerequisites and even ownership, so it calls nearly everything buildable.
		DWORD foreignYardOwners = 0u;

		for (auto const pOwnBld : pHouse->Buildings)
		{
			if (pOwnBld && pOwnBld->IsAlive && pOwnBld->Type && pOwnBld->Type->ConstructionYard
				&& !HouseExt::IsAvailableToHouse(pHouse, pOwnBld->Type))
			{
				foreignYardOwners |= pOwnBld->Type->OwnerFlags;
			}
		}

		auto scanType = [&](TechnoTypeClass* pType)
		{
			if (!pType || pType->TechLevel < 0 || pType->TechLevel > pHouse->TechLevel
				|| !(HouseExt::IsAvailableToHouse(pHouse, pType) || (pType->OwnerFlags & foreignYardOwners))
				|| CanBuildUnderHumanRules(pHouse, pType))
			{
				return;
			}

			std::vector<int> unlockers;

			for (const int prereq : pType->Prerequisite)
			{
				const bool met = prereq >= 0 ? HouseExt::HasBuildingPrerequisite(pHouse, prereq)
					: (RulesExt::Global() && -prereq < RulesExt::Global()->GenericPrerequisites.Count
						&& RulesExt::Global()->GenericPrerequisites.GetItem(-prereq).Count > 0
						? HouseExt::HasGenericPrerequisite(prereq, pHouse)
						: false);

				if (met)
					continue;

				satisfiers.clear();
				CollectPrerequisiteBuildings(prereq, satisfiers, 0);

				// A vanilla group we already own a member of is met as well.
				if (prereq < 0 && std::any_of(satisfiers.begin(), satisfiers.end(),
						[pHouse](int b) { return HouseExt::HasBuildingPrerequisite(pHouse, b); }))
				{
					continue;
				}

				for (const int b : satisfiers)
				{
					if (b >= 0 && b < BuildingTypeClass::Array.Count && IsPlayerFacingBuilding(BuildingTypeClass::Array.GetItem(b)))
						unlockers.push_back(b);
				}
			}

			std::sort(unlockers.begin(), unlockers.end());
			unlockers.erase(std::unique(unlockers.begin(), unlockers.end()), unlockers.end());

			for (const int b : unlockers)
				macro.TechUnlocks[b]++;
		};

		for (auto const pType : InfantryTypeClass::Array)
			scanType(pType);

		for (auto const pType : UnitTypeClass::Array)
			scanType(pType);

		for (auto const pType : AircraftTypeClass::Array)
			scanType(pType);

		for (auto const pType : BuildingTypeClass::Array)
		{
			if (IsPlayerFacingBuilding(pType))
				scanType(pType);
		}

		for (auto const& [buildingIdx, count] : macro.TechUnlocks)
		{
			auto const pBld = BuildingTypeClass::Array.GetItem(buildingIdx);

			if (count > 0 && IsPlayerFacingBuilding(pBld) && pHouse->CountOwnedNow(pBld) == 0
				&& CanBuildUnderHumanRules(pHouse, pBld))
			{
				macro.HasBuildableTechUnlock = true;
				break;
			}
		}
	}

	// Progressive unit tier (every 150 frames): super units come in step by step, as the tech a
	// player can afford grows, even when the tech tree is complete from minute 0. The cap starts at
	// TechLevel 2 and rises one level every 3 / 2 / 1 game minutes (Easy / Normal / Hard), up to the
	// house's tech level (the map's limit), where it ends. It rises sooner when the house has seen an
	// enemy unit of a higher tier (seen tier + 1, fog-fair) or has lost half its peak buildings.
	void UpdateUnitTierCap(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastUnitTierFrame < 150 && now >= macro.LastUnitTierFrame)
			return;

		macro.LastUnitTierFrame = now;

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->Owner || pTechno->Owner == pHouse
				|| pTechno->Owner->IsNeutral() || pHouse->IsAlliedWith(pTechno->Owner)
				|| pTechno->WhatAmI() == AbstractType::Building || !IsCellVisibleToAI(pHouse, pTechno->GetCell()))
			{
				continue;
			}

			if (auto const pType = pTechno->GetTechnoType())
				macro.SeenEnemyUnitTier = std::max(macro.SeenEnemyUnitTier, std::clamp(pType->TechLevel, 0, 10));
		}

		macro.PeakOwnedBuildings = std::max(macro.PeakOwnedBuildings, pHouse->OwnedBuildings);

		const int diffIdx = static_cast<int>(pHouse->GetAIDifficultyIndex());
		const int framesPerTier = 900 * (diffIdx >= 2 ? 3 : (diffIdx == 1 ? 2 : 1));
		int cap = 2 + std::max(0, now) / framesPerTier;

		if (macro.SeenEnemyUnitTier > 0)
			cap = std::max(cap, macro.SeenEnemyUnitTier + 1);

		if (macro.PeakOwnedBuildings >= 8 && pHouse->OwnedBuildings * 2 < macro.PeakOwnedBuildings)
			cap++;

		macro.UnitTierCap = std::min(cap, std::max(0, pHouse->TechLevel));
	}

	// Whether the progressive tier lets this house build this unit now. Economy and utility units are
	// never held back (harvesters, MCVs and other deployers, engineers, unarmed transports, carryalls),
	// and a very expensive unit counts as at least tier 7 whatever its TechLevel says.
	bool IsUnitTierAllowed(HouseClass* pHouse, TechnoTypeClass* pType)
	{
		if (!pHouse || !pType)
			return false;

		auto const& macro = GetMacroState(pHouse);

		if (macro.UnitTierCap >= std::max(0, pHouse->TechLevel))
			return true;

		if (auto const pUnitType = abstract_cast<UnitTypeClass*>(pType))
		{
			if (pUnitType->Harvester || pUnitType->ResourceGatherer || pUnitType->DeploysInto)
				return true;
		}

		if (auto const pInfType = abstract_cast<InfantryTypeClass*>(pType); pInfType && pInfType->Engineer)
			return true;

		auto const pWeapon = pType->GetWeapon(0);
		const bool armed = pWeapon && pWeapon->WeaponType && pWeapon->WeaponType->Damage > 0;

		auto const pAirType = abstract_cast<AircraftTypeClass*>(pType);

		if (!armed && (pType->Passengers > 0 || (pAirType && pAirType->Carryall)))
			return true;

		const int tier = std::max(std::clamp(pType->TechLevel, 0, 10), pType->Cost >= 2000 ? 7 : 0);

		return tier <= macro.UnitTierCap;
	}

	// Field scouting: one fast, cheap idle ground unit at a time visits the nearest free ore field
	// (map-wide cache) not scouted in the last 5400 frames, so an enemy outpost there is known
	// before an MCV is sent (free-field searches skip fields near known enemy structures).
	void UpdateFieldScouting(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastFieldScoutFrame < 300)
			return;

		macro.LastFieldScoutFrame = now;

		// The current scout is still on its way.
		if (macro.FieldScoutUid != 0)
		{
			for (auto const pUnit : UnitClass::Array)
			{
				if (pUnit && pUnit->UniqueID == macro.FieldScoutUid && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Owner == pHouse
					&& pUnit->CurrentMission == Mission::Move && pUnit->Destination)
				{
					return;
				}
			}

			macro.FieldScoutUid = 0;
		}

		macro.ScoutedFields.erase(std::remove_if(macro.ScoutedFields.begin(), macro.ScoutedFields.end(),
			[now](auto const& entry) { return now - entry.second > 5400; }), macro.ScoutedFields.end());

		UnitClass* pScout = nullptr;
		int bestSpeed = 0;

		for (auto const pUnit : UnitClass::Array)
		{
			if (!pUnit || pUnit->Owner != pHouse || !pUnit->IsAlive || pUnit->InLimbo || !pUnit->Type || pUnit->Type->Harvester
				|| pUnit->Type->ResourceGatherer || pUnit->Type->DeploysInto || pUnit->Type->Naval || pUnit->IsInAir()
				|| pUnit->Type->MovementZone == MovementZone::Fly || pUnit->Type->Cost > 1200 || pUnit->Target != nullptr
				|| pUnit->GetHealthPercentage() < 0.6 || macro.PlanMembers.contains(pUnit->UniqueID)
				|| (pUnit->CurrentMission != Mission::Guard && pUnit->CurrentMission != Mission::Area_Guard))
			{
				continue;
			}

			if (pUnit->Type->Speed > bestSpeed)
			{
				bestSpeed = pUnit->Type->Speed;
				pScout = pUnit;
			}
		}

		if (!pScout)
			return;

		const CellStruct from = pScout->GetMapCoords();
		const int zone = ObjectZone(pScout, MovementZone::Normal);
		CellClass* pField = nullptr;
		int bestDist = INT_MAX;

		for (auto const& field : GetOreFields())
		{
			const int dist = std::max(std::abs(field.Cell.X - from.X), std::abs(field.Cell.Y - from.Y));

			if (dist < 20 || dist >= bestDist || MapClass::Instance.GetMovementZoneType(field.Cell, MovementZone::Normal, false) != zone)
				continue;

			const bool scouted = std::any_of(macro.ScoutedFields.begin(), macro.ScoutedFields.end(),
				[&field](auto const& entry) { return entry.first.DistanceFrom(field.Cell) <= 12.0; });

			if (scouted)
				continue;

			// Only fields we do not hold already.
			bool ours = false;

			for (auto const pOwnB : pHouse->Buildings)
			{
				if (pOwnB && pOwnB->IsAlive && pOwnB->Type && pOwnB->GetMapCoords().DistanceFrom(field.Cell) <= 14.0)
				{
					ours = true;
					break;
				}
			}

			if (ours)
				continue;

			if (auto const pCell = MapClass::Instance.TryGetCellAt(field.Cell))
			{
				bestDist = dist;
				pField = pCell;
			}
		}

		if (!pField)
			return;

		macro.ScoutedFields.emplace_back(pField->MapCoords, now);
		macro.FieldScoutUid = pScout->UniqueID;
		IssueMoveOrder(pScout, pField, 3.0);
		CountDecision(NeuralDecision::FieldScoutSent, pHouse->ArrayIndex);
	}

	// Per-tick match tactics feeding the wave logic:
	// 1. our armed ground units that died within 22 cells of the current wave target, by the
	//    side they came from (points 31/42: the next waves approach from another side);
	// 2. a counterattack window when the enemy pressure near home peaked (>= 200) and collapsed
	//    to a quarter (point 45);
	// 3. every 900 frames: continuous pressure when our economy leads (point 53) and the enemy
	//    house to focus, the clearly weakest one (points 39/51). All fog-fair.
	void UpdateMatchTactics(HouseClass* pHouse, const std::array<int16_t, NeuralAI::OutputVectorSize>& scores)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;
		const CellStruct home = GetHouseHomeCell(pHouse);

		{
			std::unordered_set<DWORD> alive;
			std::vector<NeuralGroundLossTrack> next;

			for (auto const pTech : TechnoClass::Array)
			{
				if (!pTech || pTech->Owner != pHouse || !pTech->IsAlive)
					continue;

				const auto what = pTech->WhatAmI();

				if (what != AbstractType::Unit && what != AbstractType::Infantry)
					continue;

				alive.insert(pTech->UniqueID);
				auto const pType = pTech->GetTechnoType();
				auto const pWS = pType ? pType->GetWeapon(0) : nullptr;

				if (pTech->InLimbo || !pType || pType->Naval || !pWS || GetWeaponThreatRate(pWS->WeaponType) <= 0)
					continue;

				next.push_back({ pTech->UniqueID, pTech->GetMapCoords(), GetCombatValueOfType(pType, false) });
			}

			const bool waveRecent = macro.LastWaveTargetCell != CellStruct::Empty && now - macro.LastWaveDispatchFrame <= 3600;

			for (auto const& track : macro.GroundLossTracks)
			{
				if (!waveRecent || alive.contains(track.ID) || track.Coords.DistanceFrom(macro.LastWaveTargetCell) > 22.0)
					continue;

				const int sector = AttackSectorOf(macro.LastWaveTargetCell, track.Coords);

				if (sector >= 0)
					macro.FailedApproachWeight[sector] += track.Value;
			}

			macro.GroundLossTracks = std::move(next);

			if (now - macro.FailedApproachDecayFrame >= 2700 || macro.FailedApproachDecayFrame > now)
			{
				macro.FailedApproachDecayFrame = now;

				for (auto& weight : macro.FailedApproachWeight)
					weight = weight * 8 / 10;
			}
		}

		if (home != CellStruct::Empty)
		{
			int threat = 0;

			for (auto const& [uid, sighting] : macro.EnemySightings)
			{
				if (now - sighting.LastSeenFrame <= 150 && sighting.LastCoords.DistanceFrom(home) <= 22.0)
					threat += sighting.Value;
			}

			if (threat > macro.HomeThreatPeak || now - macro.HomeThreatPeakFrame > 2700 || macro.HomeThreatPeakFrame > now)
			{
				macro.HomeThreatPeak = threat;
				macro.HomeThreatPeakFrame = now;
			}
			else if (macro.HomeThreatPeak >= 200 && threat * 4 <= macro.HomeThreatPeak)
			{
				// Their attack broke on our base: hit back while they are depleted.
				if (SlotGate(scores[37], -200, 37))
				{
					macro.CounterattackUntilFrame = now + 1200;
					CountDecision(NeuralDecision::Counterattack, pHouse->ArrayIndex);
				}

				macro.HomeThreatPeak = threat;
				macro.HomeThreatPeakFrame = now;
			}
		}

		if (now - macro.TacticsRefreshFrame < 900 && macro.TacticsRefreshFrame <= now)
			return;

		macro.TacticsRefreshFrame = now;

		int ownRefineries = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && (pBld->Type->Refinery || pBld->Type->ResourceDestination))
				ownRefineries++;
		}

		std::array<int, 32> enemyStrength {};
		std::array<int, 32> enemyKnownBuildings {};
		std::array<int, 32> enemyKnownRefineries {};

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || pBld->InLimbo || !pBld->IsAlive || !pBld->Type || !pBld->Owner || pBld->Owner == pHouse
				|| pHouse->IsAlliedWith(pBld->Owner) || pBld->Owner->IsNeutral() || pBld->Owner->Defeated
				|| pBld->Owner->IsObserver() || pBld->Type->Insignificant || !IsEnemyBuildingKnown(pHouse, pBld))
			{
				continue;
			}

			const int idx = std::clamp(pBld->Owner->ArrayIndex, 0, 31);
			enemyKnownBuildings[idx]++;
			enemyStrength[idx] += 10 + (pBld->Type->IsBaseDefense ? 25 : 0);

			if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				enemyKnownRefineries[idx]++;
		}

		for (auto const& [uid, sighting] : macro.EnemySightings)
		{
			if (sighting.OwnerIndex >= 0 && sighting.OwnerIndex < 32 && now - sighting.LastSeenFrame <= 2700)
				enemyStrength[sighting.OwnerIndex] += sighting.Value;
		}

		int weakest = -1;
		int weakestStrength = INT_MAX;
		int strongestStrength = 0;
		int enemies = 0;
		int richestEnemyRefineries = 0;

		for (int i = 0; i < 32; ++i)
		{
			if (enemyKnownBuildings[i] <= 0)
				continue;

			enemies++;
			richestEnemyRefineries = std::max(richestEnemyRefineries, enemyKnownRefineries[i]);
			strongestStrength = std::max(strongestStrength, enemyStrength[i]);

			if (enemyStrength[i] < weakestStrength)
			{
				weakestStrength = enemyStrength[i];
				weakest = i;
			}
		}

		macro.PressureMode = pHouse->Available_Money() >= 2500 && pHouse->NumWarFactories >= 1 && ownRefineries >= 2
			&& ownRefineries >= richestEnemyRefineries + 1 && SlotGate(scores[37], 0, 37);

		macro.FocusEnemyIndex = (enemies >= 2 && weakestStrength * 10 <= strongestStrength * 6 && SlotGate(scores[43], -150, 43))
			? weakest
			: -1;
	}

} // namespace
