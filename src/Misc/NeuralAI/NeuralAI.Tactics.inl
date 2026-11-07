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
	// Aircraft stuck on an attack they never fly are released as well (see below).
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

		// Aircraft on Mission::Attack that neither move nor fire: when the engine finds no fire
		// location for the target it goes back to "return to base" and, still armed and targeting,
		// straight back to picking a location, so the aircraft sits on its pad for the rest of the
		// match, never reloading and never free for other work. Released (target cleared, idle
		// mode: back to the pad and reload) after 450 frames on the ground without moving or a
		// shot, or 1800 frames in the air without a shot. Plan members are left to their plan.
		std::unordered_map<DWORD, NeuralAirStallTrack> nextAir;

		for (auto const pAir : AircraftClass::Array)
		{
			if (!pAir || pAir->Owner != pHouse || !pAir->IsAlive || pAir->InLimbo || !pAir->Type || pAir->Type->Spawned
				|| pAir->CurrentMission != Mission::Attack || !pAir->Target || macro.PlanMembers.contains(pAir->UniqueID))
			{
				continue;
			}

			const CellStruct here = pAir->GetMapCoords();
			NeuralAirStallTrack track { here, now, pAir->Ammo, now };

			if (auto const it = macro.AirStallTracks.find(pAir->UniqueID); it != macro.AirStallTracks.end())
			{
				track = it->second;

				if (track.Cell.DistanceFrom(here) > 1.5)
				{
					track.Cell = here;
					track.SinceFrame = now;
				}

				if (track.Ammo != pAir->Ammo)
				{
					track.Ammo = pAir->Ammo;
					track.AmmoSinceFrame = now;
				}
			}

			if (pAir->IsFiring)
			{
				track.SinceFrame = now;
				track.AmmoSinceFrame = now;
			}

			const bool parked = !pAir->IsInAir() && now - track.SinceFrame >= 450 && now - track.AmmoSinceFrame >= 450;
			const bool idleFlight = pAir->IsInAir() && now - track.AmmoSinceFrame >= 1800;

			if (parked || idleFlight)
			{
				pAir->SetTarget(nullptr);
				pAir->SetDestination(nullptr, true);
				pAir->EnterIdleMode(false, true);
				CountDecision(NeuralDecision::AirAttackStalled, pHouse->ArrayIndex);
				continue; // Its next attack starts a new track.
			}

			nextAir.emplace(pAir->UniqueID, track);
		}

		macro.AirStallTracks = std::move(nextAir);
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
		// Expert 0 in charge moves a harvester stuck on a drained field three times sooner.
		auto const pIdleExt = HouseExt::Fetch(pHouse);
		const int idleWait = (pIdleExt && pIdleExt->NeuralAI_ActiveExpert == 0) ? 300 : 900;
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

			if (now - track.SinceFrame < idleWait)
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
			// Distance plus the enemy route heat the straight way there crosses (40 heat = 1 cell),
			// so a harvester does not cross the hottest enemy routes for a slightly closer field.
			int bestCost = INT_MAX;
			CellClass* pNearestOre = nullptr;
			int nearestDist = INT_MAX;

			for (int dy = -96; dy <= 96; dy += 3)
			{
				for (int dx = -96; dx <= 96; dx += 3)
				{
					const int d = std::max(std::abs(dx), std::abs(dy));

					if (d < 8 || (d >= bestCost && d >= nearestDist))
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

					if (d < nearestDist)
					{
						nearestDist = d;
						pNearestOre = pCell;
					}

					const int cost = d + RouteHeatAlong(pHouse, here, tc) / 40;

					if (cost < bestCost)
					{
						bestCost = cost;
						pBestOre = pCell;
					}
				}
			}

			if (pBestOre && pNearestOre && pBestOre != pNearestOre && pBestOre->MapCoords.DistanceFrom(pNearestOre->MapCoords) > 6.0)
				CountDecision(NeuralDecision::RouteHarvesterDetour, pHouse->ArrayIndex);

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
	// The same pass grades the work there is for Expert 6 (ExpansionOpportunity, the selector
	// prior): explored free fields out of known enemy defense reach, by value over distance, the
	// best three; those beyond the crawl's reach (60 cells from any structure of ours) only count
	// while an expansion MCV can be built. Fields matter less the more we already mine.
	bool IsCellInKnownEnemyDefenseReach(HouseClass* pHouse, const CellStruct& cell, int marginCells);

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
		macro.ExpansionOpportunity = 0;
		macro.ClaimNeedsMCV = false;

		auto const pExt = HouseExt::Fetch(pHouse);
		const bool hoarder = pExt && pExt->NeuralAI_ActiveExpert == 6;

		std::vector<CellStruct> ownHubs;
		std::vector<std::pair<CellStruct, int>> ownAnchors; // cell, zone
		std::vector<CellStruct> ownStructures;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
				continue;

			const CellStruct bc = pBld->GetMapCoords();

			if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				ownHubs.push_back(bc);

			if (pBld->Type->ConstructionYard)
				ownAnchors.emplace_back(bc, MapClass::Instance.GetMovementZoneType(bc, MovementZone::Normal, pBld->OnBridge));

			if (!pBld->Type->Wall && !pBld->Type->Insignificant && !pBld->Type->InvisibleInGame)
				ownStructures.push_back(bc);
		}

		if (ownAnchors.empty())
			return;

		// An expansion MCV is possible: a ground war factory and money for it, below the yard cap the
		// production uses (2 + free fields up to 3, + 1 with a big bank, at most 6).
		const int money = pHouse->Available_Money();
		const bool canSendMCV = pHouse->NumWarFactories > 0 && money >= 2500
			&& pHouse->NumConYards < std::clamp(2 + std::min(3, macro.UncolonizedOreFields) + (money >= 15000 ? 1 : 0), 2, 6);
		std::array<int, 3> bestWorth {};

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
			int crawlDist = INT_MAX;

			for (auto const& sc : ownStructures)
				crawlDist = std::min(crawlDist, chebyshev(sc, field.Cell));

			const bool beyondCrawl = crawlDist > 60;

			if (metric < bestMetric)
			{
				bestMetric = metric;
				macro.ResourceClaimCell = field.Cell;
				macro.ResourceClaimTier = tier;
				macro.ResourceClaimZone = fieldZone;
				macro.ClaimNeedsMCV = beyondCrawl;
			}

			// Work for Expert 6: only what it knows (explored ground) and can take now.
			if (beyondCrawl && !canSendMCV)
				continue;

			auto const pFieldCell = MapClass::Instance.TryGetCellAt(field.Cell);

			if (!pFieldCell || !IsCellVisibleToAI(pHouse, pFieldCell) || IsCellInKnownEnemyDefenseReach(pHouse, field.Cell, 3))
				continue;

			int worth = std::min(120, value) * 2 * 30 / std::max(30, dist);

			if (tier == 1)
				worth /= 2;

			for (auto& slot : bestWorth)
			{
				if (worth > slot)
					std::swap(worth, slot);
			}
		}

		// Each field more is worth less the more we already mine (5% per refinery of ours, 8% with a
		// bank of 30000+ where income stopped being the limit; denied ground still counts, so never
		// below a third): on maps with ore everywhere the three best fields always look perfect.
		int opportunity = bestWorth[0] + bestWorth[1] + bestWorth[2];
		const int perRefinery = money >= 30000 ? 8 : 5;
		opportunity = opportunity * std::max(34, 100 - perRefinery * static_cast<int>(ownHubs.size())) / 100;

		// Nothing to gather with: the money for the first step of a crawl or a refinery is missing.
		if (money < 1500 && pHouse->CountResourceGatherers == 0)
			opportunity /= 2;

		macro.ExpansionOpportunity = std::min(450, opportunity);

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
		macro.TechUnlockTypes.clear();
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
			{
				macro.TechUnlocks[b]++;
				macro.TechUnlockTypes[b].push_back(pType);
			}
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

	int VisibleEnemyThreatNear(HouseClass* pHouse, const CellStruct& cell, int radius);
	bool IsBuildingEnterableFromZone(const BuildingClass* pBld, int zone);
	void OrderFootToEnter(FootClass* pFoot, TechnoClass* pEnterTarget);

	// Expert 0 (Economy & Crisis) bookkeeping and actions, every 150 frames. The peaks of static
	// refineries and harvesters are tracked for every house; EconomyRecoveryActive is set when a raid
	// cost a third of either (production then rebuilds the economy first while Expert 0 runs).
	// Any house, whatever the expert (without income it is dead anyway):
	// - Broke: no income and less than a harvester's price: sell the least useful structure, one per
	//   600 frames: a factory copy, a defense copy far from any known enemy structure, a power plant
	//   copy the power surplus can spare, any defense copy, then a lone support structure that is no
	//   factory. Never a ConYard or a refinery.
	// - Harvesters under fire: a damaged one goes home to a refinery with no enemy in sight.
	// - EconomyTrouble: the selector prior of Expert 0 (what it could fix now; 0 vetoes it).
	// While Expert 0 is in charge it also acts:
	// - Harvesters under fire go home at the first enemy seen near them, damaged or not.
	// - Blackout: with low power, high-drain structures that are neither factories nor defenses go
	//   offline (the existing micro brings them back online once the surplus is >= 150).
	// - Harvester balance: a refinery with 3+ harvesters waiting next to it sends one to the ore of a
	//   refinery of ours that has none (one per 450 frames).
	void UpdateEconomyCrisis(HouseClass* pHouse)
	{
		if (!pHouse || pHouse->Defeated)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastEconomyCrisisFrame < 150 && now >= macro.LastEconomyCrisisFrame)
			return;

		macro.LastEconomyCrisisFrame = now;

		std::vector<BuildingClass*> refineries;
		std::vector<UnitClass*> harvesters;
		int deployedGatherers = 0;
		int cashBuildings = 0;

		for (auto const pBld : pHouse->Buildings)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->CurrentMission == Mission::Selling)
				continue;

			if (pBld->Type->ProduceCashAmount > 0)
				cashBuildings++;

			// A deployed slave miner (ResourceGatherer, Yuri) gathers by itself: counted with the
			// gatherers, not the refineries, so deploying one never looks like a lost harvester.
			if (pBld->Type->ResourceGatherer)
				deployedGatherers++;
			else if (pBld->Type->Refinery || pBld->Type->ResourceDestination)
				refineries.push_back(pBld);
		}

		for (auto const pUnit : UnitClass::Array)
		{
			// Mobile slave miners (they deploy into their refinery) gather too. Not one in limbo: the
			// harvester a factory is building exists already, waiting for the credits it needs.
			if (pUnit && pUnit->Owner == pHouse && pUnit->IsAlive && !pUnit->InLimbo && pUnit->Type
				&& (pUnit->Type->Harvester || pUnit->Type->ResourceGatherer
					|| (pUnit->Type->DeploysInto && (pUnit->Type->DeploysInto->Refinery || pUnit->Type->DeploysInto->ResourceGatherer))))
			{
				harvesters.push_back(pUnit);
			}
		}

		const int refCount = static_cast<int>(refineries.size());
		const int harvCount = static_cast<int>(harvesters.size()) + deployedGatherers;
		macro.PeakRefineries = std::max(macro.PeakRefineries, refCount);
		macro.PeakHarvesters = std::max(macro.PeakHarvesters, harvCount);
		macro.EconomyRecoveryActive = (macro.PeakRefineries >= 2 && refCount * 3 < macro.PeakRefineries * 2)
			|| (macro.PeakHarvesters >= 3 && harvCount * 3 < macro.PeakHarvesters * 2);

		auto const pExt = HouseExt::Fetch(pHouse);

		if (!pExt)
			return;

		const bool expertInCharge = pExt->NeuralAI_ActiveExpert == 0;

		// Harvesters under fire: an armed enemy in our sight within 7 cells. A damaged one (any one
		// while Expert 0 is in charge) goes home to the nearest refinery it can reach with no enemy in
		// sight around it, so the load is not lost and the guards there take over. One order per
		// harvester per 600 frames. Chrono miners are counted but never ordered (an order makes
		// them loop-teleport); slave miners deploy where they stop.
		int underFire = 0;
		{
			std::unordered_map<DWORD, int> keptRetreats;

			for (auto const pHarv : harvesters)
			{
				if (!pHarv->Type->Harvester || pHarv->Type->DeploysInto || pHarv->IsInAir())
					continue;

				const CellStruct hc = pHarv->GetMapCoords();

				if (VisibleEnemyThreatNear(pHouse, hc, 7) <= 0)
					continue;

				underFire++;

				if (pHarv->Type->Teleporter)
					continue;

				auto const lastIt = macro.HarvesterRetreatFrames.find(pHarv->UniqueID);
				const bool recentOrder = lastIt != macro.HarvesterRetreatFrames.end() && now - lastIt->second < 600;

				if (recentOrder)
					keptRetreats.emplace(pHarv->UniqueID, lastIt->second);

				if (recentOrder || (!expertInCharge && pHarv->GetHealthPercentage() >= 0.9)
					|| pHarv->CurrentMission == Mission::Enter || pHarv->CurrentMission == Mission::Unload
					|| pHarv->QueuedMission == Mission::Enter || pHarv->HasAnyLink())
				{
					continue;
				}

				const int harvZone = ObjectZone(pHarv, pHarv->Type->MovementZone);
				BuildingClass* pHome = nullptr;
				BuildingClass* pNearestHome = nullptr;
				// Distance plus the enemy route heat the straight way home crosses (40 heat = 1 cell).
				int bestCost = INT_MAX;
				int nearestDist = INT_MAX;

				for (auto const pRef : refineries)
				{
					if (!pRef->Type->Refinery || !IsBuildingEnterableFromZone(pRef, harvZone))
						continue;

					const int dist = static_cast<int>(pRef->GetMapCoords().DistanceFrom(hc));

					if (dist < 6 || dist > 60 || (dist >= bestCost && dist >= nearestDist)
						|| VisibleEnemyThreatNear(pHouse, pRef->GetMapCoords(), 8) > 0)
					{
						continue;
					}

					if (dist < nearestDist)
					{
						nearestDist = dist;
						pNearestHome = pRef;
					}

					const int cost = dist + RouteHeatAlong(pHouse, hc, pRef->GetMapCoords()) / 40;

					if (cost < bestCost)
					{
						bestCost = cost;
						pHome = pRef;
					}
				}

				if (!pHome)
					continue;

				if (pHome != pNearestHome)
					CountDecision(NeuralDecision::RouteHarvesterDetour, pHouse->ArrayIndex);

				OrderFootToEnter(pHarv, pHome);
				keptRetreats[pHarv->UniqueID] = now;
				CountDecision(NeuralDecision::HarvesterRetreat, pHouse->ArrayIndex);
			}

			macro.HarvesterRetreatFrames = std::move(keptRetreats);
		}

		macro.HarvestersUnderFire = underFire;

		// Refineries no harvester serves while another has 3+ waiting next to it (the balance below
		// has work), and storage nearly full while it limits our income.
		BuildingClass* pCrowded = nullptr;
		BuildingClass* pIdle = nullptr;
		int starvedRefineries = 0;

		if (refCount >= 2)
		{
			int crowdedQueue = 2;

			for (auto const pRef : refineries)
			{
				int queuedNear = 0;
				bool anyAssigned = false;

				for (auto const pHarv : harvesters)
				{
					const int dist = pHarv->DistanceFrom(pRef);

					if (dist <= 6 * 256)
						queuedNear++;

					if (dist <= 14 * 256)
						anyAssigned = true;
				}

				if (queuedNear > crowdedQueue)
				{
					crowdedQueue = queuedNear;
					pCrowded = pRef;
				}

				if (!anyAssigned)
				{
					starvedRefineries++;

					if (!pIdle)
						pIdle = pRef;
				}
			}
		}

		macro.StarvedRefineries = pCrowded ? starvedRefineries : 0;

		// Storage only limits income where a refinery keeps the harvest (Refinery.UseStorage, the
		// same rule a human player has): 85% full means the next unloads will be lost.
		bool storageLimited = false;

		for (auto const pBld : pHouse->Buildings)
		{
			if (pBld->IsOnMap && !pBld->InLimbo && BuildingTypeExt::Fetch(pBld->Type)->Refinery_UseStorage)
			{
				storageLimited = true;
				break;
			}
		}

		bool nearlyFull = storageLimited && pHouse->TotalStorage > 0
			&& pHouse->OwnedTiberium.GetTotalAmount() >= static_cast<float>(pHouse->TotalStorage) * 0.85f;

		// Only trouble Expert 0 can fix: a silo we can build now (a silo may need a refinery type
		// the storage option disables, so a full house can have none).
		if (nearlyFull)
		{
			nearlyFull = std::any_of(BuildingTypeClass::Array.begin(), BuildingTypeClass::Array.end(), [pHouse](BuildingTypeClass* pType)
				{
					return pType && pType->Storage > 0 && !pType->Refinery && !pType->ResourceGatherer && !pType->ResourceDestination
						&& CanBuildUnderHumanRules(pHouse, pType);
				});
		}

		macro.StorageNearlyFull = nearlyFull;

		if (pHouse->HasLowPower())
		{
			if (macro.LowPowerSinceFrame < 0 || macro.LowPowerSinceFrame > now)
				macro.LowPowerSinceFrame = now;
		}
		else
		{
			macro.LowPowerSinceFrame = -1;
		}

		// Blackout.
		if (expertInCharge && pHouse->HasLowPower())
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->Type->TogglePower && pBld->StuffEnabled
					&& !pBld->IsIronCurtained() && !pBld->Type->IsBaseDefense && pBld->Type->Factory == AbstractType::None
					&& !pBld->Type->ConstructionYard && !pBld->Type->Refinery && pBld->Type->PowerDrain >= 100)
				{
					pBld->GoOffline();
				}
			}
		}

		// Broke.
		int cheapestHarvester = INT_MAX;

		for (auto const pType : UnitTypeClass::Array)
		{
			if (pType && (pType->Harvester || pType->ResourceGatherer)
				&& (!pType->DeploysInto || pType->DeploysInto->Refinery || pType->DeploysInto->ResourceGatherer)
				&& CanBuildUnderHumanRules(pHouse, pType))
				cheapestHarvester = std::min(cheapestHarvester, pType->GetActualCost(pHouse));
		}

		const bool harvesterBuildable = cheapestHarvester != INT_MAX;

		// Without a refinery no harvester can be built: the cheapest refinery (with its free
		// harvester) is then what the credits are kept for.
		if (!harvesterBuildable && refCount == 0)
		{
			for (auto const pType : BuildingTypeClass::Array)
			{
				if (pType && (pType->Refinery || pType->ResourceGatherer) && CanBuildUnderHumanRules(pHouse, pType))
					cheapestHarvester = std::min(cheapestHarvester, pType->GetActualCost(pHouse));
			}
		}

		// No income: nothing gathers (harvesters, slave miners deployed or not).
		const bool noIncome = harvCount == 0;

		// Broke hold: while nothing gathers, no cash building pays and a harvester (or, with no
		// refinery left, a refinery) can be built, every production that does not restore income stops
		// spending, so the last credits and what a broke sale returns go to that one item. A refinery
		// waits while a harvester can be built (it costs more for the same income), a power plant
		// goes on under low power. The vehicle in the war factory is cancelled (refunded; the harvester
		// needs that queue), the others are put on hold and resumed once something gathers.
		const bool holdForHarvester = noIncome && cashBuildings == 0 && cheapestHarvester != INT_MAX;
		macro.BrokeHoldActive = holdForHarvester;

		// No repairs while broke: wrench repairs stop, units on a repair station (service depot,
		// shipyard) step off it.
		if (holdForHarvester)
		{
			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type)
					continue;

				if (pBld->IsBeingRepaired)
					pBld->SetRepairState(0);

				if (!pBld->Type->UnitRepair)
					continue;

				const CellStruct exitCell = pBld->GetMapCoords() + CellStruct { static_cast<short>(pBld->Type->GetFoundationWidth() + 2), 0 };

				for (int i = 0; pBld->RadioLinks.IsAllocated && i < pBld->RadioLinks.Capacity; ++i)
				{
					auto const pDocked = abstract_cast<FootClass*>(pBld->GetNthLink(i));

					if (!pDocked || !pDocked->IsAlive || pDocked->Owner != pHouse || pDocked->GetHealthPercentage() >= 1.0)
						continue;

					if (auto const pExitCell = MapClass::Instance.TryGetCellAt(exitCell))
					{
						pDocked->SetDestination(pExitCell, true);
						pDocked->QueueMission(Mission::Move, false);
					}
				}
			}
		}

		for (auto const pFactory : FactoryClass::Array)
		{
			if (!pFactory || pFactory->Owner != pHouse || !pFactory->Object)
				continue;

			auto const pObject = pFactory->Object;
			auto const pType = pObject->GetTechnoType();
			const DWORD objectId = pObject->UniqueID;
			auto const held = std::find(macro.BrokeHeldObjects.begin(), macro.BrokeHeldObjects.end(), objectId);

			if (!holdForHarvester)
			{
				if (held != macro.BrokeHeldObjects.end() && pFactory->IsSuspended)
					pFactory->Unsuspend(true);

				continue;
			}

			if (!pType || pFactory->IsSuspended || pFactory->IsDone() || held != macro.BrokeHeldObjects.end())
				continue;

			auto const pUnitType = abstract_cast<UnitTypeClass*>(pType);
			auto const pBldType = abstract_cast<BuildingTypeClass*>(pType);
			auto const pInfType = abstract_cast<InfantryTypeClass*>(pType);
			const bool restoresIncome = (pUnitType
					&& (pUnitType->Harvester || pUnitType->ResourceGatherer
						|| (pUnitType->DeploysInto
							&& (pUnitType->DeploysInto->Refinery || pUnitType->DeploysInto->ResourceGatherer
								|| (pUnitType->DeploysInto->ConstructionYard && pHouse->ConYards.Count == 0)))))
				|| (pBldType && (((pBldType->Refinery || pBldType->ResourceGatherer) && !harvesterBuildable)
					|| pBldType->ProduceCashAmount > 0
					|| (pBldType->PowerBonus > pBldType->PowerDrain && pHouse->HasLowPower())))
				|| (pInfType && pInfType->Engineer);

			if (restoresIncome)
				continue;

			if (pUnitType && !pUnitType->Naval)
			{
				Debug::Log("[NeuralAI] House %d broke: cancelling %s for a harvester.\n", pHouse->ArrayIndex, pType->ID);
				pFactory->AbandonProduction();
			}
			else
			{
				pFactory->Suspend(true);
				macro.BrokeHeldObjects.push_back(objectId);
			}
		}

		if (!holdForHarvester)
			macro.BrokeHeldObjects.clear();

		if (noIncome && cheapestHarvester != INT_MAX && pHouse->Available_Money() < cheapestHarvester
			&& now - macro.LastBrokeSaleFrame >= 600)
		{
			std::map<int, int> typeCounts;

			for (auto const pBld : pHouse->Buildings)
			{
				if (pBld && pBld->IsAlive && !pBld->InLimbo && pBld->Type && pBld->CurrentMission != Mission::Selling)
					typeCounts[pBld->Type->ArrayIndex]++;
			}

			std::vector<CellStruct> enemyCells;

			for (auto const pEB : BuildingClass::Array)
			{
				if (pEB && pEB->IsAlive && !pEB->InLimbo && pEB->Owner && pEB->Owner != pHouse && !pEB->Owner->IsNeutral()
					&& !pHouse->IsAlliedWith(pEB->Owner) && IsEnemyBuildingKnown(pHouse, pEB))
				{
					enemyCells.push_back(pEB->GetMapCoords());
				}
			}

			BuildingClass* pSell = nullptr;
			int bestTier = INT_MAX;

			for (auto const pBld : pHouse->Buildings)
			{
				if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Type || pBld->CurrentMission == Mission::Selling
					|| pBld->Type->ConstructionYard || pBld->Type->Refinery || pBld->Type->ResourceDestination
					|| pBld->Type->Unsellable || pBld->Type->InvisibleInGame)
				{
					continue;
				}

				const CellStruct bc = pBld->GetMapCoords();
				const bool hasCopy = typeCounts[pBld->Type->ArrayIndex] >= 2;
				const int netPower = pBld->Type->PowerBonus - pBld->Type->PowerDrain;
				int tier = INT_MAX;

				if (netPower > 0)
				{
					if (hasCopy && pHouse->PowerOutput - pHouse->PowerDrain >= netPower)
						tier = 2;
				}
				else if (hasCopy && pBld->Type->Factory != AbstractType::None)
				{
					tier = 0;
				}
				else if (hasCopy && pBld->Type->IsBaseDefense)
				{
					tier = std::none_of(enemyCells.begin(), enemyCells.end(), [&bc](const CellStruct& e) { return bc.DistanceFrom(e) <= 30.0; })
						? 1 : 3;
				}
				else if (pBld->Type->Factory == AbstractType::None && !pBld->Type->IsBaseDefense)
				{
					tier = 4;
				}

				if (tier < bestTier)
				{
					bestTier = tier;
					pSell = pBld;
				}
			}

			if (pSell)
			{
				macro.LastBrokeSaleFrame = now;
				Debug::Log("[NeuralAI] House %d broke (no income, %d credits): selling %s at (%d,%d) for a harvester.\n",
					pHouse->ArrayIndex, pHouse->Available_Money(), pSell->Type->ID, pSell->GetMapCoords().X, pSell->GetMapCoords().Y);
				pSell->Sell(1);
			}
		}

		// Selector prior of Expert 0: the economy trouble it can act on now. Nothing gathering (with a
		// harvester or a refinery buildable), harvesters with no refinery, a raid that cost a third of
		// the economy, low power that lasted 300 frames (worse the bigger the deficit), harvesters
		// under fire, a refinery no harvester serves while another is crowded, storage nearly full.
		// 0 = nothing to do: Expert 0 is vetoed.
		{
			int trouble = 0;
			// Only harvesters that unload at a refinery need one (a slave miner is its own refinery).
			const bool dockingHarvesterOwned = std::any_of(harvesters.begin(), harvesters.end(),
				[](UnitClass* pHarv) { return pHarv->Type->Harvester && !pHarv->Type->DeploysInto; });

			if (noIncome && cheapestHarvester != INT_MAX)
				trouble += 900;
			else if (refCount == 0 && dockingHarvesterOwned)
				trouble += 600;

			if (macro.EconomyRecoveryActive)
				trouble += 450;

			if (macro.LowPowerSinceFrame >= 0 && now - macro.LowPowerSinceFrame >= 300)
				trouble += 250 + std::clamp((pHouse->PowerDrain - pHouse->PowerOutput) * 2, 0, 250);

			trouble += 150 * std::min(2, underFire);

			if (macro.StarvedRefineries > 0)
				trouble += 200;

			if (macro.StorageNearlyFull)
				trouble += 350;

			macro.EconomyTrouble = std::min(950, trouble);
		}

		if (!expertInCharge)
			return;

		// Harvester balance.
		if (refCount >= 2 && now - macro.LastHarvesterBalanceFrame >= 450)
		{
			if (pCrowded && pIdle && pCrowded != pIdle)
			{
				CellClass* pOre = FindFreeOreFieldCell(pHouse, pIdle->GetMapCoords(),
					MapClass::Instance.GetMovementZoneType(pIdle->GetMapCoords(), MovementZone::Normal, false), 0, true);

				if (pOre && pOre->MapCoords.DistanceFrom(pIdle->GetMapCoords()) <= 16.0)
				{
					for (auto const pHarv : harvesters)
					{
						// Chrono miners loop-teleport on a destination, slave miners deploy where they stop.
						if (pHarv->DistanceFrom(pCrowded) <= 6 * 256 && !pHarv->InLimbo && pHarv->CurrentMission != Mission::Enter
							&& pHarv->CurrentMission != Mission::Unload && pHarv->Type->Harvester && !pHarv->Type->Teleporter
							&& !pHarv->Type->DeploysInto)
						{
							macro.LastHarvesterBalanceFrame = now;
							pHarv->SetDestination(pOre, true);
							pHarv->QueueMission(Mission::Harvest, false);
							CountDecision(NeuralDecision::HarvesterRebalance, pHouse->ArrayIndex);
							break;
						}
					}
				}
			}
		}
	}

	// FindSpatialTetrisBuildCell costs 20-55 ms in a late game. Questions that only need a rough
	// answer (is there room for it, where would it go: deploy stands, a fleeing MCV, the shipyard
	// check) share one answer per house and building type for 240 frames; real placement always
	// searches. Lookups only, keyed by house and type index, tied to NeuralMatchSerial.
	CellStruct CachedTetrisBuildCell(HouseClass* pHouse, BuildingTypeClass* pType)
	{
		if (!pHouse || !pType)
			return CellStruct::Empty;

		struct Entry
		{
			int MatchSerial;
			int Frame;
			CellStruct Cell;
		};

		static std::unordered_map<int, Entry> cache;
		const int key = pHouse->ArrayIndex * 8192 + pType->ArrayIndex;
		const int now = Unsorted::CurrentFrame;

		if (auto const it = cache.find(key); it != cache.end() && it->second.MatchSerial == NeuralMatchSerial
			&& now >= it->second.Frame && now - it->second.Frame < 240)
		{
			return it->second.Cell;
		}

		const CellStruct cell = NeuralAI::FindSpatialTetrisBuildCell(pHouse, pType);
		cache[key] = Entry { NeuralMatchSerial, now, cell };
		return cell;
	}

	// Expert 3 (FFA opportunism), every 450 frames, only with two or more living enemies and Expert 3
	// in charge: a priority bonus per enemy house for the main enemy choice (Micro), from what this
	// house has seen only (fog-fair):
	// - up to +600 (40 per unit) to an enemy caught fighting someone else (its visible units next to a
	//   third house's known structures, or a third house's visible units next to its known structures):
	//   hit it while busy.
	// - +500 to an enemy whose visible units are inside our base (12 cells of our buildings): never
	//   two fronts, answer the one at the door first.
	// - up to +400 to the weakest by known structures, +150 to the nearest known base.
	// - the visible threat of an enemy not at our door counts 40%: let the others wear the big one down.
	void UpdateFfaTargets(HouseClass* pHouse)
	{
		if (!pHouse)
			return;

		auto& macro = GetMacroState(pHouse);
		const int now = Unsorted::CurrentFrame;

		if (now - macro.LastFfaTargetFrame < 450 && now >= macro.LastFfaTargetFrame)
			return;

		macro.LastFfaTargetFrame = now;
		macro.FfaEnemyBonus.fill(0);
		macro.FfaThreatScale.fill(100);

		auto const pExt = HouseExt::Fetch(pHouse);

		if (!pExt || pExt->NeuralAI_ActiveExpert != 3)
			return;

		std::vector<HouseClass*> enemies;

		for (auto const pOther : HouseClass::Array)
		{
			if (pOther && pOther != pHouse && !pHouse->IsAlliedWith(pOther) && !pOther->IsNeutral() && !pOther->IsObserver()
				&& !pOther->Defeated && pOther->ArrayIndex >= 0 && pOther->ArrayIndex < 8)
			{
				enemies.push_back(pOther);
			}
		}

		if (enemies.size() < 2)
			return;

		// Known structures per house (enemies and third parties alike) and our own building cells.
		std::map<int, std::vector<CellStruct>> knownStructures;
		std::vector<CellStruct> ownCells;

		for (auto const pBld : BuildingClass::Array)
		{
			if (!pBld || !pBld->IsAlive || pBld->InLimbo || !pBld->Owner || !pBld->Type || pBld->Type->InvisibleInGame)
				continue;

			if (pBld->Owner == pHouse)
				ownCells.push_back(pBld->GetMapCoords());
			else if (!pBld->Owner->IsNeutral() && IsEnemyBuildingKnown(pHouse, pBld))
				knownStructures[pBld->Owner->ArrayIndex].push_back(pBld->GetMapCoords());
		}

		auto isNear = [](const std::vector<CellStruct>& cells, const CellStruct& c, int range)
		{
			return std::any_of(cells.begin(), cells.end(),
				[&](const CellStruct& o) { return std::max(std::abs(o.X - c.X), std::abs(o.Y - c.Y)) <= range; });
		};

		std::array<int, 8> busy {};
		std::array<bool, 8> atOurDoor {};

		for (auto const pTechno : TechnoClass::Array)
		{
			if (!pTechno || !pTechno->IsAlive || pTechno->InLimbo || !pTechno->Owner || pTechno->Owner == pHouse
				|| pTechno->Owner->IsNeutral() || pTechno->WhatAmI() == AbstractType::Building
				|| pTechno->Owner->ArrayIndex < 0 || pTechno->Owner->ArrayIndex >= 8
				|| !IsCellVisibleToAI(pHouse, pTechno->GetCell()))
			{
				continue;
			}

			const int owner = pTechno->Owner->ArrayIndex;
			const CellStruct c = pTechno->GetMapCoords();

			if (!pHouse->IsAlliedWith(pTechno->Owner) && isNear(ownCells, c, 12))
				atOurDoor[owner] = true;

			for (auto const& [third, cells] : knownStructures)
			{
				if (third == owner || third < 0 || third >= 8)
					continue;

				if (isNear(cells, c, 12))
				{
					busy[owner]++; // its units at a third house's base
					busy[third]++; // that third house is under attack
				}
			}
		}

		int maxKnown = 1;

		for (auto const pEnemy : enemies)
			maxKnown = std::max(maxKnown, static_cast<int>(knownStructures[pEnemy->ArrayIndex].size()));

		const CellStruct home = pHouse->ConYards.Count > 0 ? pHouse->ConYards.GetItem(0)->GetMapCoords() : pHouse->BaseSpawnCell;
		int nearestIdx = -1;
		int nearestDist = INT_MAX;

		for (auto const pEnemy : enemies)
		{
			for (auto const& c : knownStructures[pEnemy->ArrayIndex])
			{
				const int d = std::max(std::abs(c.X - home.X), std::abs(c.Y - home.Y));

				if (d < nearestDist)
				{
					nearestDist = d;
					nearestIdx = pEnemy->ArrayIndex;
				}
			}
		}

		for (auto const pEnemy : enemies)
		{
			const int idx = pEnemy->ArrayIndex;
			const int known = static_cast<int>(knownStructures[idx].size());
			int bonus = 0;

			// Graded: a few units trading at a border do not make a house busy, a siege does.
			bonus += std::min(600, 40 * busy[idx]);

			if (atOurDoor[idx])
				bonus += 500;

			if (known > 0)
				bonus += 400 * (maxKnown - known) / maxKnown;

			if (idx == nearestIdx)
				bonus += 150;

			macro.FfaEnemyBonus[idx] = bonus;
			macro.FfaThreatScale[idx] = atOurDoor[idx] ? 100 : 40;
		}
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
